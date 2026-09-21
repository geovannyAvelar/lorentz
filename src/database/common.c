/* Lorentz: A black hole for Internet advertisements
*  (c) 2017 Pi-hole, LLC (https://pi-hole.net)
*  Network-wide ad blocking via your own hardware.
*
*  Lorentz Engine
*  Common database routines for lorentz.db
*
*  This file is copyright under the latest version of the EUPL.
*  Please see LICENSE file for your rights under this license. */

#include "lorentz.h"
#include "database/common.h"
// SQLITE_WARNING, SQLITE_NOTICE, SQLITE_SCHEMA for SQLite3LogCallback()
#include "database/sqlite3.h"
#include "database/network-table.h"
#include "database/message-table.h"
#include "shmem.h"
// struct config
#include "config/config.h"
#include "timers.h"
// file_exists()
#include "files.h"
// import_aliasclients()
#include "database/aliasclients.h"
// CREATE_QUERIES_TABLE
// add_additional_info_column()
#include "database/query-table.h"
// set_event()
#include "events.h"
// generate_backtrace()
#include "signals.h"
// create_session_table()
#include "database/session-table.h"
// assert()
#include <assert.h>
// _Atomic
#include <stdatomic.h>

bool DBdeleteoldqueries = false;
static _Atomic bool DBerror = false;
static _Atomic int dbopen_cnt = 0; // Number of times the database has been opened

bool __attribute__ ((pure)) LorentzDBerror(void)
{
	return atomic_load_explicit(&DBerror, memory_order_relaxed);
}

// Flag the database as unusable and tell the user why
static bool check_db_error(const bool corrupt, const bool readonly)
{
	// Check if the database file is malformed
	if(corrupt)
	{
		log_warn("Database %s is damaged and cannot be used.", config.files.database.v.s);
		atomic_store_explicit(&DBerror, true, memory_order_relaxed);
	}
	// Check if the database file is read-only
	if(readonly)
	{
		log_warn("Database %s is read-only and cannot be used.", config.files.database.v.s);
		atomic_store_explicit(&DBerror, true, memory_order_relaxed);
	}

	return atomic_load_explicit(&DBerror, memory_order_relaxed);
}

bool check_db_rc(const db_rc rc)
{
	return check_db_error(rc == DB_CORRUPT, rc == DB_READONLY);
}

// Check the most recent error of a connection after a failed operation that
// did not hand a return code back (e.g. db_prepare())
static void check_conn_error(db_conn *db)
{
	check_db_rc(db->drv->classify_error(db_errcode(db)));
}

void _dbclose(db_conn **db, const char *func, const int line, const char *file)
{
	// The shared in-memory connection is owned by close_memory_database() and
	// its prepared statements live as long as Lorentz does. Closing it here would
	// finalize them behind the back of whoever cached them, so return before
	// both the NULL assignment and the counter below: the connection stays
	// valid for its owner, and it never went through dbopen() to be counted
	if(db != NULL && is_memdb(*db))
	{
		log_err("dbclose() called on the in-memory database in %s() (%s:%i)",
		        func, short_path(file), line);
		return;
	}

	if(config.debug.database.v.b)
		log_debug(DEBUG_DATABASE, "Closing Lorentz database in %s() (%s:%i)", func, short_path(file), line);

	// Only try to close an existing database connection. Statements that
	// were not finalized are finalized (and logged) by the driver. Inside the
	// guard: dbopen() only counted up when it handed out a connection, so
	// closing a handle that is already NULL must not decrement the counter
	if(db != NULL && *db != NULL)
	{
		db_close(*db);
		atomic_fetch_sub_explicit(&dbopen_cnt, 1, memory_order_relaxed);
	}

	// Always set database pointer to NULL
	if(db) *db = NULL;
}

/**
 * @brief Callback function for handling SQLite database busy events.
 *
 * This function is called by SQLite when the database is locked and cannot be accessed
 * immediately. It implements an exponential backoff strategy using predefined delay and
 * total wait time arrays. The function waits for a specified delay (in milliseconds)
 * before retrying, up to DATABASE_BUSY_TIMEOUT.
 *
 * @param ptr Pointer to an unsigned int specifying the maximum allowed wait time (in ms).
 * @param count The number of times the busy handler has been invoked for the same operation.
 * @return int Returns 1 to indicate that SQLite should retry the operation after the delay,
 *             or 0 to indicate that the operation should fail because the timeout has been exceeded.
 *
 * The function logs a warning if the total wait time exceeds the allowed timeout, and
 * logs debug information for each wait period.
 */
int sqliteBusyCallback(void *ptr, int count)
{
	static const uint8_t delays[] = { 1, 2, 5, 10, 15, 20, 25, 25,  25,  50,  50, 100 };
	static const uint8_t totals[] = { 0, 1, 3,  8, 18, 33, 53, 78, 103, 128, 178, 228 };
	# define NDELAY ArraySize(delays)

	int delay, prior;

	assert(count >= 0);
	if(count < (int)NDELAY)
	{
		// Use the predefined delays and totals
		delay = delays[count];
		prior = totals[count];
	}
	else
	{
		// Use the last predefined delay and total
		delay = delays[NDELAY - 1];
		prior = totals[NDELAY - 1] + delay*(count - (NDELAY - 1));
	}

	if(prior + delay > DATABASE_BUSY_TIMEOUT)
	{
		delay = DATABASE_BUSY_TIMEOUT - prior;
		if(delay <= 0)
		{
			// If the total time waited so far plus the next delay exceeds
			// the maximum allowed time, return 0 to indicate that the
			// database is busy beyond the allowed time and that we will not
			// wait any longer.
			log_debug(DEBUG_DATABASE, "Database busy for %d ms > %d ms, not waiting any longer",
			         prior + delay, DATABASE_BUSY_TIMEOUT);
			return 0;
		}
	}
	log_debug(DEBUG_DATABASE, "Database busy - waiting %d ms (dbopen: %d)", delay,
	          atomic_load_explicit(&dbopen_cnt, memory_order_relaxed));
	usleep(delay * 1000); // Convert ms to us

	// Return 1 to indicate that we will wait for the database to become
	// available again
	return 1;
}

db_conn *_dbopen(const bool readonly, const bool create, const char *func, const int line, const char *file)
{
	// Silently return NULL if the database is known to be broken
	if(LorentzDBerror())
		return NULL;

	// Try to open database
	log_debug(DEBUG_DATABASE, "Opening Lorentz database in %s() (node %s%s) (%s:%i)",
	          func, readonly ? "RO" : "RW", create ? ",C" : "", short_path(file), line);

	// DB_OPEN_NOMUTEX: dbopen() connections are strictly single-
	// threaded — every caller (civetweb worker, database thread) opens,
	// uses, and closes the connection within one function scope, so the
	// serialized-mode per-API-call mutex is pure overhead. The long-lived
	// shared connections (_memdb in query-table.c, gravity_db) keep the
	// default serialized mode because they are touched from multiple
	// threads.
	unsigned int flags = readonly ? DB_OPEN_READONLY : DB_OPEN_READWRITE;
	if(create && !readonly)
		flags |= DB_OPEN_CREATE;
	flags |= DB_OPEN_NOMUTEX;

	db_rc rc = DB_OK;
	const char *msg = NULL;
	db_conn *db = db_open_ex(config.files.database.v.s, flags, &rc, &msg);
	if(db == NULL)
	{
		log_err("Error while trying to open database: %s", msg);
		check_db_rc(rc);
		return NULL;
	}

	// Count the connection as soon as it exists. The failure paths below
	// release it through dbclose(), which is the matching decrement
	atomic_fetch_add_explicit(&dbopen_cnt, 1, memory_order_relaxed);

	// If the database is opened in read-write mode, actually check if it is
	// writable. If it is not, close the database and return an error
	if(!readonly && db_is_readonly(db))
	{
		log_err("Cannot open database in read-write mode");
		dbclose(&db);
		return NULL;
	}

	// Explicitly set busy handler to value defined in lorentz.h
	rc = db_set_busy_handler(db, sqliteBusyCallback, NULL);
	if(rc != DB_OK)
	{
		log_err("Error while trying to set busy timeout on database: %s",
		        db_errstr(db, db_errcode(db)));
		dbclose(&db);
		check_db_rc(rc);
		return NULL;
	}

	return db;
}

// Run a formatted query
static db_rc vdbquery(db_conn *db, const char *format, va_list args)
{
	const db_driver *drv = db != NULL ? db->drv : db_driver_active();
	char *query = drv->vmprintf(format, args);

	if(query == NULL)
	{
		log_err("Memory allocation failed in dbquery()");
		return DB_ERROR;
	}

	// Log generated SQL string when dbquery() is called
	// although the database connection is not available
	if(db == NULL)
	{
		log_err("dbquery(\"%s\") called but database is not available!", query);
		drv->mem_free(query);
		return DB_ERROR;
	}

	log_debug(DEBUG_DATABASE, "dbquery: \"%s\"", query);

	const db_rc rc = db_exec(db, query);
	if(rc != DB_OK)
	{
		log_err("ERROR: SQL query \"%s\" failed: %s (%s)",
		        query, drv->errstr(db_errcode(db)), drv->errname(db_extended_errcode(db)));
		drv->mem_free(query);
		check_db_rc(rc);
		return rc;
	}

	// Free allocated memory for query string
	drv->mem_free(query);

	log_debug(DEBUG_DATABASE,"         ---> OK");

	// Return success
	return DB_OK;
}

int dbquery(db_conn *db, const char *format, ...)
{
	va_list args;
	va_start(args, format);
	const db_rc rc = vdbquery(db, format, args);
	va_end(args);

	return rc;
}

static bool create_counter_table(db_conn *db)
{
	// Start transaction
	SQL_bool(db, "BEGIN");

	// Create Lorentz table in the database (holds properties like database version, etc.)
	SQL_bool(db, "CREATE TABLE counters ( id INTEGER PRIMARY KEY NOT NULL, value INTEGER NOT NULL );");

	// ID 0 = total queries
	if(!db_set_counter(db, DB_TOTALQUERIES, 0))
	{
		log_err("create_counter_table(): Failed to set total queries counter to zero!");
		dbquery(db, "ROLLBACK");
		return false;
	}

	// ID 1 = total blocked queries
	if(!db_set_counter(db, DB_BLOCKEDQUERIES, 0))
	{
		log_err("create_counter_table(): Failed to set blocked queries counter to zero!");
		dbquery(db, "ROLLBACK");
		return false;
	}

	// Time stamp of creation of the counters database
	if(!db_set_Lorentz_property(db, DB_FIRSTCOUNTERTIMESTAMP, (unsigned long)time(0)))
	{
		log_err("create_counter_table(): Failed to update first counter timestamp!");
		dbquery(db, "ROLLBACK");
		return false;
	}

	// Update database version to 2
	if(!db_set_Lorentz_property(db, DB_VERSION, 2))
	{
		log_err("create_counter_table(): Failed to update database version!");
		dbquery(db, "ROLLBACK");
		return false;
	}
	// End transaction
	SQL_bool(db, "END");

	return true;
}

// Split from db_create() so that every failure below returns through it and
// the connection is closed exactly once. SQL_bool() returns on failure, so the
// close cannot live in here
static bool db_create_tables(db_conn *db)
{
	// Create Queries table in the database
	SQL_bool(db, CREATE_QUERIES_TABLE_V1);

	// Add an index on the timestamps (not a unique index!)
	SQL_bool(db, CREATE_QUERIES_TIMESTAMP_INDEX);

	// Create Lorentz table in the database (holds properties like database version, etc.)
	SQL_bool(db, CREATE_LORENTZ_TABLE);

	// Set Lorentz_db version 1
	if(!db_set_Lorentz_property(db, DB_VERSION, 1))
		return false;

	// Most recent timestamp initialized to 00:00 1 Jan 1970
	if(!db_set_Lorentz_property(db, DB_LASTTIMESTAMP, 0))
		return false;

	return true;
}

static bool db_create(void)
{
	db_conn *db = dbopen(false, true);
	if(db == NULL)
		return false;

	const bool okay = db_create_tables(db);

	// Close database handle whether or not the tables were created: a
	// half-created database that stays open holds the file for the life of
	// the process while db_init() carries on without it
	dbclose(&db);

	return okay;
}

void SQLite3LogCallback(void *pArg, int iErrCode, const char *zMsg)
{
	// Note: pArg is NULL and not used
	// See https://sqlite.org/rescode.html#extrc for details
	// concerning the return codes returned here
	if(zMsg != NULL && strncmp(zMsg, "file renamed while open: ", sizeof("file renamed while open: ")-1) == 0)
	{
		// This happens when gravity.db is replaced while Lorentz is running
		// We can safely ignore this warning
		return;
	}

	if(iErrCode == SQLITE_WARNING)
		log_warn("SQLite3: %s (%d)", zMsg, iErrCode);
	else if(iErrCode == SQLITE_NOTICE || iErrCode == SQLITE_SCHEMA)
	{
		// SQLITE_SCHEMA is returned when the database schema has changed
		// This is not necessarily an error, as sqlite3_step() will re-prepare
		// the statement and try again. If it cannot, it will return an error
		// and this will be handled over there.
		log_debug(DEBUG_ANY, "SQLite3: %s (%d)", zMsg, iErrCode);
	}
	else
		log_err("SQLite3: %s (%d)", zMsg, iErrCode);
}

void db_init(void)
{
	// Check if database exists, if not create empty database
	if(!file_exists(config.files.database.v.s))
	{
		log_warn("No database file found, creating new (empty) database at %s",
		         config.files.database.v.s);
		if (!db_create())
		{
			log_err("Creation of database failed, database is not available");
			return;
		}
	}

	// Explicitly set permissions to 0640
	// 640 =            u+w       u+r       g+r
	const mode_t mode = S_IWUSR | S_IRUSR | S_IRGRP;
	if(file_exists(config.files.database.v.s))
		chmod_file(config.files.database.v.s, mode);

	// Open database
	db_conn *db = dbopen(false, true);

	// Explicitly set permissions if file just created
	if(file_exists(config.files.database.v.s))
		chmod_file(config.files.database.v.s, mode);

	// Return if database access failed
	if(!db)
	{
		log_err("Database not available!");
		DBerror = true;
		return;
	}

	// Test Lorentz_db version and see if we need to upgrade the database file
	int dbversion = db_get_int(db, DB_VERSION);
	// Warn if there is an error, however, do not warn on database file
	// corruption. This has already been logged before
	if(dbversion < 1 && !LorentzDBerror())
	{
		log_warn("Database not available, please ensure the database is unlocked when starting lorentz !");
		dbclose(&db);
		DBerror = true;
		return;
	}
	else
	{
		log_info("Database version is %i", dbversion);
	}


	// Update to version 2 if lower
	if(dbversion < 2)
	{
		// Update to version 2: Create counters table
		log_info("Updating long-term database to version 2");
		if (!create_counter_table(db))
		{
			log_err("Counter table not initialized, database not available");
			dbclose(&db);
			DBerror = true;
			return;
		}
		// Get updated version
		dbversion = db_get_int(db, DB_VERSION);
	}

	// Update to version 3 if lower
	if(dbversion < 3)
	{
		// Update to version 3: Create network table
		log_info("Updating long-term database to version 3");
		if (!create_network_table(db))
		{
			log_err("Network table not initialized, database not available");
			dbclose(&db);
			DBerror = true;
			return;
		}
		// Get updated version
		dbversion = db_get_int(db, DB_VERSION);
	}

	// Update to version 4 if lower
	if(dbversion < 4)
	{
		// Update to version 4: Unify clients in network table
		log_info("Updating long-term database to version 4");
		if(!unify_hwaddr(db))
		{
			log_err("Unable to unify clients in network table, database not available");
			dbclose(&db);
			DBerror = true;
			return;
		}
		// Get updated version
		dbversion = db_get_int(db, DB_VERSION);
	}

	// Update to version 5 if lower
	if(dbversion < 5)
	{
		// Update to version 5: Create network-addresses table
		log_info("Updating long-term database to version 5");
		if(!create_network_addresses_table(db))
		{
			log_err("Network-addresses table not initialized, database not available");
			dbclose(&db);
			DBerror = true;
			return;
		}
		// Get updated version
		dbversion = db_get_int(db, DB_VERSION);
	}

	// Update to version 6 if lower
	if(dbversion < 6)
	{
		// Update to version 6: Create message table
		log_info("Updating long-term database to version 6");
		if(!create_message_table(db))
		{
			log_err("Message table not initialized, database not available");
			dbclose(&db);
			DBerror = true;
			return;
		}
		// Get updated version
		dbversion = db_get_int(db, DB_VERSION);
	}

	// Update to version 7 if lower
	if(dbversion < 7)
	{
		// Update to version 7: Add additional_info column to queries table
		log_info("Updating long-term database to version 7");
		if(!add_additional_info_column(db))
		{
			log_err("Column additional_info not initialized, database not available");
			dbclose(&db);
			DBerror = true;
			return;
		}
		// Get updated version
		dbversion = db_get_int(db, DB_VERSION);
	}

	// Update to version 8 if lower
	if(dbversion < 8)
	{
		// Update to version 8: Add name field to network_addresses table
		log_info("Updating long-term database to version 8");
		if(!create_network_addresses_with_names_table(db))
		{
			log_err("Network addresses table not initialized, database not available");
			dbclose(&db);
			DBerror = true;
			return;
		}
		// Get updated version
		dbversion = db_get_int(db, DB_VERSION);
	}

	// Update to version 9 if lower
	if(dbversion < 9)
	{
		// Update to version 9: Add aliasclients table
		log_info("Updating long-term database to version 9");
		if(!create_aliasclients_table(db))
		{
			log_err("Aliasclients table not initialized, database not available");
			dbclose(&db);
			DBerror = true;
			return;
		}
		// Get updated version
		dbversion = db_get_int(db, DB_VERSION);
	}

	// Update to version 10 if lower
	if(dbversion < 10)
	{
		// Update to version 10: Use linking tables for queries table
		log_info("Updating long-term database to version 10");
		if(!optimize_queries_table(db))
		{
			log_info("Queries table not optimized, database not available");
			dbclose(&db);
			DBerror = true;
			return;
		}

		// Reopen database after low-level schema editing to reload the schema
		dbclose(&db);
		if(!(db = dbopen(false, false)))
			return;

		// Get updated version
		dbversion = db_get_int(db, DB_VERSION);
	}

	// Update to version 11 if lower
	if(dbversion < 11)
	{
		// Update to version 11: Use link table also for additional_info column
		log_info("Updating long-term database to version 11");
		if(!create_addinfo_table(db))
		{
			log_info("Link table for additional_info not generated, database not available");
			dbclose(&db);
			DBerror = true;
			return;
		}

		// Reopen database after low-level schema editing to reload the schema
		dbclose(&db);
		if(!(db = dbopen(false, false)))
			return;

		// Get updated version
		dbversion = db_get_int(db, DB_VERSION);
	}

	// Update to version 12 if lower
	if(dbversion < 12)
	{
		// Update to version 12: Add additional columns for reply type and time, and dnssec status
		log_info("Updating long-term database to version 12");
		if(!add_query_storage_columns(db))
		{
			log_info("Additional records not generated, database not available");
			dbclose(&db);
			DBerror = true;
			return;
		}
		// Get updated version
		dbversion = db_get_int(db, DB_VERSION);
	}

	// Update to version 13 if lower
	if(dbversion < 13)
	{
		// Update to version 13: Add additional column for regex ID
		log_info("Updating long-term database to version 13");
		if(!add_query_storage_column_regex_id(db))
		{
			log_info("Additional records not generated, database not available");
			dbclose(&db);
			DBerror = true;
			return;
		}
		// Get updated version
		dbversion = db_get_int(db, DB_VERSION);
	}

	// Update to version 14 if lower
	if(dbversion < 14)
	{
		// Update to version 14: Add additional column for the lorentz table
		log_info("Updating long-term database to version 14");
		if(!add_lorentz_table_description(db))
		{
			log_info("Lorentz table description cannot be added, database not available");
			dbclose(&db);
			DBerror = true;
			return;
		}
		// Get updated version
		dbversion = db_get_int(db, DB_VERSION);
	}

	// Update to version 15 if lower
	if(dbversion < 15)
	{
		// Update to version 15: Add session table
		log_info("Updating long-term database to version 15");
		if(!create_session_table(db))
		{
			log_info("Session table cannot be created, database not available");
			dbclose(&db);
			DBerror = true;
			return;
		}
		// Get updated version
		dbversion = db_get_int(db, DB_VERSION);
	}

	// Update to version 16 if lower
	if(dbversion < 16)
	{
		// Update to version 16: Add app column to session table
		log_info("Updating long-term database to version 16");
		if(!add_session_app_column(db))
		{
			log_info("Session table cannot be updated, database not available");
			dbclose(&db);
			DBerror = true;
			return;
		}
		// Get updated version
		dbversion = db_get_int(db, DB_VERSION);
	}

	// Update to version 17 if lower
	if(dbversion < 17)
	{
		// Update to version 17: Rename regex_id column to regex_id_old
		log_info("Updating long-term database to version 17");
		if(!rename_query_storage_column_regex_id(db))
		{
			log_info("regex_id cannot be renamed to list_id, database not available");
			dbclose(&db);
			DBerror = true;
			return;
		}
		// Get updated version
		dbversion = db_get_int(db, DB_VERSION);
	}

	// Update to version 18 if lower
	if(dbversion < 18)
	{
		// Update to version 18: Add cli column to session table
		log_info("Updating long-term database to version 18");
		if(!add_session_cli_column(db))
		{
			log_info("Session table cannot be updated, database not available");
			dbclose(&db);
			DBerror = true;
			return;
		}
		// Get updated version
		dbversion = db_get_int(db, DB_VERSION);
	}

	// Update to version 19 if lower
	if(dbversion < 19)
	{
		// Update to version 19: Add x_forwarded_for column to session table
		log_info("Updating long-term database to version 19");
		if(!add_session_x_forwarded_for_column(db))
		{
			log_info("Session table cannot be updated, database not available");
			dbclose(&db);
			DBerror = true;
			return;
		}
		// Get updated version
		dbversion = db_get_int(db, DB_VERSION);
	}

	// Update to version 20 if lower
	if(dbversion < 20)
	{
		// Update to version 20: Add additional column for the network table
		log_info("Updating long-term database to version 20");
		if(!create_network_addresses_network_id_index(db))
		{
			log_info("Network addresses network_id index cannot be added, database not available");
			dbclose(&db);
			DBerror = true;
			return;
		}
		// Get updated version
		dbversion = db_get_int(db, DB_VERSION);
	}

	// Update to version 21 if lower
	if(dbversion < 21)
	{
		// Update to version 21: Add additional column "ede" in the query_storage table
		log_info("Updating long-term database to version 21");
		if(!add_query_storage_column_ede(db))
		{
			log_info("Additional column 'ede' in the query_storage table cannot be added, database not available");
			dbclose(&db);
			DBerror = true;
			return;
		}
		// Get updated version
		dbversion = db_get_int(db, DB_VERSION);
	}

	// Update to version 22 if lower
	if(dbversion < 22)
	{
		// Update to version 22: Replace queries VIEW with JOIN-based definition
		log_info("Updating long-term database to version 22");
		if(!replace_queries_view_with_joins(db))
		{
			log_info("Queries VIEW cannot be replaced, database not available");
			dbclose(&db);
			DBerror = true;
			return;
		}
		// Get updated version
		dbversion = db_get_int(db, DB_VERSION);
	}

	/* * * * * * * * * * * * * IMPORTANT * * * * * * * * * * * * *
	 * If you add a new database version, check if the in-memory
	 * schema needs to be update as well (always recreated from
	 * scratch on every Lorentz (re)start). Also, ensure to update the
	 * MEMDB_VERSION in src/database/query-table.h as well as the
	 * expected database schema in the CI tests.
	 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

	// Last check after all migrations, if this happens, it will cause the
	// CI to fail the tests
	if(dbversion != MEMDB_VERSION)
	{
		log_err("Expected query database version %d but found %d, database not available", MEMDB_VERSION, dbversion);
		dbclose(&db);
		DBerror = true;
		return;
	}

	lock_shm();
	import_aliasclients(db);
	unlock_shm();

	// Close database to prevent having it opened all time
	// We already closed the database when we returned earlier
	dbclose(&db);

	// Log if users asked us to not use the long-term database for queries
	// We will still use it to store warnings (Lorentz diagnosis system)
	if(config.database.maxDBdays.v.ui == 0)
		log_info("Not using the database for storing queries");

	log_info("Database successfully initialized");
}

int db_get_int(db_conn *db, const enum lorentz_table_props ID)
{
	// Prepare SQL statement
	char* querystr = NULL;
	int ret = asprintf(&querystr, "SELECT VALUE FROM lorentz WHERE id = %u;", ID);

	if(querystr == NULL || ret < 0)
	{
		log_err("Memory allocation failed in db_get_int db, with ID = %u (%i)", ID, ret);
		return DB_FAILED;
	}

	int value = db_query_int(db, querystr);
	free(querystr);

	return value;
}

bool db_set_Lorentz_property(db_conn *db, const enum lorentz_table_props ID, const int value)
{
	// Use UPSERT (https://sqlite.org/lang_upsert.html)
	// UPSERT is a clause added to INSERT that causes the INSERT to behave
	// as an UPDATE or a no-op if the INSERT would violate a uniqueness
	// constraint. UPSERT is not standard SQL. UPSERT in SQLite follows the
	// syntax established by PostgreSQL, with generalizations. 
	SQL_bool(db, "INSERT INTO lorentz (id, value) VALUES ( %u, %d ) ON CONFLICT (id) DO UPDATE SET value=%d;", ID, value, value);
	return true;
}

// Run a prepared two-integer statement (bind 1 and 2, expect no rows)
static bool exec_int_int(db_conn *db, const char *sql, const int first, const int second)
{
	if(db == NULL)
		return false;

	db_stmt *stmt = db_prepare(db, sql, false);
	if(stmt == NULL)
	{
		check_conn_error(db);
		return false;
	}

	db_rc rc;
	if((rc = db_bind_int(stmt, 1, first)) != DB_OK ||
	   (rc = db_bind_int(stmt, 2, second)) != DB_OK ||
	   (rc = db_step(stmt)) != DB_DONE)
	{
		check_db_rc(rc);
		db_finalize(stmt);
		return false;
	}

	db_finalize(stmt);
	return true;
}

bool db_set_counter(db_conn *db, const enum counters_table_props ID, const int value)
{
	// The counter row exists after the first write, INSERT OR REPLACE keeps
	// this a single statement for both cases
	return exec_int_int(db, "INSERT OR REPLACE INTO counters (id, value) VALUES (?,?)", ID, value);
}

bool db_update_disk_counter(db_conn *db, const enum counters_table_props ID, const int change)
{
	return exec_int_int(db, "UPDATE counters SET value = value + ? WHERE id = ?", change, ID);
}

// Prepare querystr and bind its parameters through bind(). Steps once and
// hands the statement back positioned on the first row. Returns NULL when the
// query failed (the failure is logged with the name of the calling helper) or
// produced no row, in which case *nodata tells the two apart
typedef bool (*bind_fn)(db_stmt *stmt, const void *args);

static db_stmt *query_first_row(db_conn *db, const char *caller, const char *querystr,
                                bind_fn bind, const void *args, bool *nodata)
{
	*nodata = false;
	if(db == NULL)
		return NULL;

	db_stmt *stmt = db_prepare(db, querystr, false);
	if(stmt == NULL)
	{
		const int code = db_errcode(db);
		if(db->drv->classify_error(code) != DB_BUSY)
			log_err("Encountered prepare error in %s(\"%s\"): %s",
			        caller, querystr, db_errstr(db, code));
		check_conn_error(db);
		return NULL;
	}

	if(bind != NULL && !bind(stmt, args))
	{
		log_err("Encountered bind error in %s(\"%s\")", caller, querystr);
		db_finalize(stmt);
		return NULL;
	}

	const db_rc rc = db_step(stmt);
	if(rc == DB_ROW)
		return stmt;

	if(rc == DB_DONE)
		*nodata = true;
	else
	{
		log_err("Encountered step error in %s(\"%s\"): %s",
		        caller, querystr, db_errstr(db, db_errcode(db)));
		check_db_rc(rc);
	}

	db_finalize(stmt);
	return NULL;
}

static bool bind_int_arg(db_stmt *stmt, const void *args)
{
	return db_bind_int(stmt, 1, *(const int*)args) == DB_OK;
}

static bool bind_str_arg(db_stmt *stmt, const void *args)
{
	return db_bind_text_ref(stmt, 1, (const char*)args) == DB_OK;
}

struct from_until {
	double from;
	double until;
	int type;
	bool has_type;
};

static bool bind_from_until(db_stmt *stmt, const void *args)
{
	const struct from_until *fu = args;
	return db_bind_double(stmt, 1, fu->from) == DB_OK &&
	       db_bind_double(stmt, 2, fu->until) == DB_OK &&
	       (!fu->has_type || db_bind_int(stmt, 3, fu->type) == DB_OK);
}

int db_query_int(db_conn *db, const char* querystr)
{
	log_debug(DEBUG_DATABASE, "dbquery_int: \"%s\"", querystr);

	bool nodata;
	db_stmt *stmt = query_first_row(db, "db_query_int", querystr, NULL, NULL, &nodata);
	if(stmt == NULL)
	{
		if(nodata)
			log_debug(DEBUG_DATABASE, "         ---> No data");
		return nodata ? DB_NODATA : DB_FAILED;
	}

	const int result = db_column_int(stmt, 0);
	log_debug(DEBUG_DATABASE, "         ---> Result %i (int)", result);
	db_finalize(stmt);
	return result;
}

int db_query_int_int(db_conn *db, const char* querystr, const int arg)
{
	log_debug(DEBUG_DATABASE, "db_query_int_arg: \"%s\"", querystr);

	bool nodata;
	db_stmt *stmt = query_first_row(db, "db_query_int_int", querystr, bind_int_arg, &arg, &nodata);
	if(stmt == NULL)
	{
		if(nodata)
			log_debug(DEBUG_DATABASE, "         ---> No data");
		return nodata ? DB_NODATA : DB_FAILED;
	}

	const int result = db_column_int(stmt, 0);
	log_debug(DEBUG_DATABASE, "         ---> Result %i (int)", result);
	db_finalize(stmt);
	return result;
}

int db_query_int_str(db_conn *db, const char* querystr, const char *arg)
{
	log_debug(DEBUG_DATABASE, "db_query_int_str: \"%s\" with \"%s\"", querystr, arg);

	bool nodata;
	db_stmt *stmt = query_first_row(db, "db_query_int_str", querystr, bind_str_arg, arg, &nodata);
	if(stmt == NULL)
	{
		if(nodata)
			log_debug(DEBUG_DATABASE, "         ---> No data");
		return nodata ? DB_NODATA : DB_FAILED;
	}

	const int result = db_column_int(stmt, 0);
	log_debug(DEBUG_DATABASE, "         ---> Result %i (int)", result);
	db_finalize(stmt);
	return result;
}

double db_query_double(db_conn *db, const char* querystr)
{
	log_debug(DEBUG_DATABASE, "dbquery_double: \"%s\"", querystr);

	bool nodata;
	db_stmt *stmt = query_first_row(db, "db_query_double", querystr, NULL, NULL, &nodata);
	if(stmt == NULL)
	{
		if(nodata)
			log_debug(DEBUG_DATABASE, "         ---> No data");
		return nodata ? DB_NODATA : DB_FAILED;
	}

	const double result = db_column_double(stmt, 0);
	log_debug(DEBUG_DATABASE, "         ---> Result %f (double)", result);
	db_finalize(stmt);
	return result;
}

int db_query_int_from_until(db_conn *db, const char* querystr, const double from, const double until)
{
	log_debug(DEBUG_DATABASE, "db_query_int_from_until: \"%s\" (from: %f, until: %f)", querystr, from, until);

	const struct from_until fu = { .from = from, .until = until, .has_type = false };
	bool nodata;
	db_stmt *stmt = query_first_row(db, "db_query_int_from_until", querystr, bind_from_until, &fu, &nodata);
	if(stmt == NULL)
		return nodata ? DB_NODATA : DB_FAILED;

	const int result = db_column_int(stmt, 0);
	db_finalize(stmt);
	return result;
}

int db_query_int_from_until_type(db_conn *db, const char* querystr, const double from, const double until, const int type)
{
	log_debug(DEBUG_DATABASE, "db_query_int_from_until_type: \"%s\" (from: %f, until: %f, type: %d)", querystr, from, until, type);

	const struct from_until fu = { .from = from, .until = until, .type = type, .has_type = true };
	bool nodata;
	db_stmt *stmt = query_first_row(db, "db_query_int_from_until_type", querystr, bind_from_until, &fu, &nodata);
	if(stmt == NULL)
		return nodata ? DB_NODATA : DB_FAILED;

	const int result = db_column_int(stmt, 0);
	db_finalize(stmt);
	return result;
}

// Return the version string of the database engine in use
const char *get_sqlite3_version(void)
{
	return db_driver_active()->version();
}

/**
 * get_row_count - Get the row count of a table.
 *
 * Uses the shared in-memory connection or opens a transient read-only one
 * (dbopen(true, false)), prepares and executes a "SELECT COUNT(*) FROM
 * <table>;" query for the given table name, and returns the number of rows in
 * that table. Only a connection opened here is closed again.
 * 
 * @param table_name The name of the table to get the size of.
 * @param memory If true, use the in-memory database; if false, use the on-disk database.
 * @return The number of rows in the table, or -2 if the database could not be opened,
 *         or -3 if the SQL statement could not be prepared (e.g. invalid table name).
 */
int64_t get_row_count(const char *table_name, const bool memory)
{
	// The in-memory connection is owned by query-table.c
	db_conn *db = memory ? get_memdb() : dbopen(true, false);
	if(!db)
		return -2;

	char * const query = db->drv->mprintf("SELECT COUNT(*) FROM %s;", table_name);
	db_stmt *stmt = query != NULL ? db_prepare(db, query, false) : NULL;
	if(query != NULL)
		db_free(db, query);
	if(stmt == NULL)
	{
		log_err("Failed to prepare statement to get size of in-memory table %s: %s",
		        table_name, db_errmsg(db));
		if(!memory)
			dbclose(&db);
		return -3;
	}

	int64_t size = -1;
	if(db_step(stmt) == DB_ROW)
	{
		size = db_column_int64(stmt, 0);
	}
	else
	{
		log_err("Failed to step statement to get size of in-memory table %s: %s",
		        table_name, db_errmsg(db));
	}

	db_finalize(stmt);
	if(!memory)
		dbclose(&db);
	return size;
}
