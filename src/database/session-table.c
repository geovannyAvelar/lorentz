/* Lorentz: A black hole for Internet advertisements
*  (c) 2023 Pi-hole, LLC (https://pi-hole.net)
*  Network-wide ad blocking via your own hardware.
*
*  Lorentz Engine
*  Sessions table database routines
*
*  This file is copyright under the latest version of the EUPL.
*  Please see LICENSE file for your rights under this license. */

#include "lorentz.h"
#include "database/session-table.h"
#include "database/common.h"
#include "config/config.h"
// get_memdb()
#include "database/query-table.h"

bool create_session_table(db_conn *db)
{
	// Start transaction of database update
	SQL_bool(db, "BEGIN TRANSACTION;");

	// Create session table
	SQL_bool(db, "CREATE TABLE session (id INTEGER PRIMARY KEY, "\
	                                   "login_at TIMESTAMP NOT NULL, "\
	                                   "valid_until TIMESTAMP NOT NULL, "\
	                                   "remote_addr TEXT NOT NULL, "\
	                                   "user_agent TEXT, "\
	                                   "sid TEXT NOT NULL, "\
	                                   "csrf TEXT NOT NULL, "\
	                                   "tls_login BOOL, "\
	                                   "tls_mixed BOOL);");

	// Update database version to 15
	if(!db_set_Lorentz_property(db, DB_VERSION, 15))
	{
		log_err("create_session_table(): Failed to update database version!");
		dbquery(db, "ROLLBACK");
		return false;
	}

	// Finish transaction
	SQL_bool(db, "END");

	return true;
}

bool add_session_app_column(db_conn *db)
{
	// Start transaction of database update
	SQL_bool(db, "BEGIN TRANSACTION;");

	// Create session table
	SQL_bool(db, "ALTER TABLE session ADD COLUMN app BOOL;");

	// Update database version to 16
	if(!db_set_Lorentz_property(db, DB_VERSION, 16))
	{
		log_err("add_session_app_column(): Failed to update database version!");
		dbquery(db, "ROLLBACK");
		return false;
	}

	// Finish transaction
	SQL_bool(db, "END");

	return true;
}

bool add_session_cli_column(db_conn *db)
{
	// Start transaction of database update
	SQL_bool(db, "BEGIN TRANSACTION;");

	// Create session table
	SQL_bool(db, "ALTER TABLE session ADD COLUMN cli BOOL;");

	// Update database version to 18
	if(!db_set_Lorentz_property(db, DB_VERSION, 18))
	{
		log_err("add_session_cli_column(): Failed to update database version!");
		dbquery(db, "ROLLBACK");
		return false;
	}

	// Finish transaction
	SQL_bool(db, "END");

	return true;
}

bool add_session_x_forwarded_for_column(db_conn *db)
{
	// Start transaction of database update
	SQL_bool(db, "BEGIN TRANSACTION;");

	// Create session table
	SQL_bool(db, "ALTER TABLE session ADD COLUMN x_forwarded_for TEXT;");

	// Update database version to 18
	if(!db_set_Lorentz_property(db, DB_VERSION, 19))
	{
		log_err("add_session_x_forwarded_for_column(): Failed to update database version!");
		dbquery(db, "ROLLBACK");
		return false;
	}

	// Finish transaction
	SQL_bool(db, "END");

	return true;
}

// Store all session in database
bool backup_db_sessions(struct session *sessions, const uint16_t max_sessions)
{
	if(!config.webserver.session.restore.v.b)
	{
		log_debug(DEBUG_API, "Session restore is disabled, not adding sessions to database");
		return true;
	}

	db_conn *db = dbopen(false, false);
	if(db == NULL)
	{
		log_warn("Failed to open database in backup_db_sessions()");
		return false;
	}

	// Insert session into database
	bool success = false;
	unsigned int api_sessions = 0;
	db_stmt *stmt = NULL;
	if((stmt = db_prepare(db, "INSERT INTO session (login_at, valid_until, remote_addr, user_agent, sid, csrf, tls_login, tls_mixed, app, cli, x_forwarded_for) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?);", false)) == NULL)
	{
		log_err("SQL error in backup_db_sessions(): %s (%d)",
		        db_errmsg(db), db_errcode(db));
		goto backup_db_sessions_end;
	}

	for(unsigned int i = 0; i < max_sessions; i++)
	{
		// Get session
		struct session *sess = &sessions[i];

		// Skip unused sessions
		if(!sess->used)
			continue;

		// Bind values to statement
		// 1: login_at
		if(db_bind_int64(stmt, 1, sess->login_at) != DB_OK)
		{
			log_err("Cannot bind login_at = %ld in backup_db_sessions(): %s (%d)",
			        (long int)sess->login_at, db_errmsg(db), db_errcode(db));
			goto backup_db_sessions_end;
		}
		// 2: valid_until
		if(db_bind_int64(stmt, 2, sess->valid_until) != DB_OK)
		{
			log_err("Cannot bind valid_until = %ld in backup_db_sessions(): %s (%d)",
			        (long int)sess->valid_until, db_errmsg(db), db_errcode(db));
			goto backup_db_sessions_end;
		}
		// 3: remote_addr
		if(db_bind_text_ref(stmt, 3, sess->remote_addr) != DB_OK)
		{
			log_err("Cannot bind remote_addr = %s in backup_db_sessions(): %s (%d)",
			        sess->remote_addr, db_errmsg(db), db_errcode(db));
			goto backup_db_sessions_end;
		}
		// 4: user_agent
		if(db_bind_text_ref(stmt, 4, sess->user_agent) != DB_OK)
		{
			log_err("Cannot bind user_agent = %s in backup_db_sessions(): %s (%d)",
			        sess->user_agent, db_errmsg(db), db_errcode(db));
			goto backup_db_sessions_end;
		}
		// 5: sid
		if(db_bind_text_ref(stmt, 5, sess->sid) != DB_OK)
		{
			log_err("Cannot bind sid = %s in backup_db_sessions(): %s (%d)",
			        sess->sid, db_errmsg(db), db_errcode(db));
			goto backup_db_sessions_end;
		}
		// 6: csrf
		if(db_bind_text_ref(stmt, 6, sess->csrf) != DB_OK)
		{
			log_err("Cannot bind csrf = %s in backup_db_sessions(): %s (%d)",
			        sess->csrf, db_errmsg(db), db_errcode(db));
			goto backup_db_sessions_end;
		}
		// 7: tls_login
		if(db_bind_int(stmt, 7, sess->tls.login ? 1 : 0) != DB_OK)
		{
			log_err("Cannot bind tls_login = %d in backup_db_sessions(): %s (%d)",
			        sess->tls.login ? 1 : 0, db_errmsg(db), db_errcode(db));
			goto backup_db_sessions_end;
		}
		// 8: tls_mixed
		if(db_bind_int(stmt, 8, sess->tls.mixed ? 1 : 0) != DB_OK)
		{
			log_err("Cannot bind tls_mixed = %d in backup_db_sessions(): %s (%d)",
			        sess->tls.mixed ? 1 : 0, db_errmsg(db), db_errcode(db));
			goto backup_db_sessions_end;
		}
		// 9: app
		if(db_bind_int(stmt, 9, sess->app ? 1 : 0) != DB_OK)
		{
			log_err("Cannot bind app = %d in backup_db_sessions(): %s (%d)",
			        sess->app ? 1 : 0, db_errmsg(db), db_errcode(db));
			goto backup_db_sessions_end;
		}
		// 10: cli
		if(db_bind_int(stmt, 10, sess->cli ? 1 : 0) != DB_OK)
		{
			log_err("Cannot bind cli = %d in backup_db_sessions(): %s (%d)",
			        sess->cli ? 1 : 0, db_errmsg(db), db_errcode(db));
			goto backup_db_sessions_end;
		}
		// 11: x_forwarded_for
		if(db_bind_text_ref(stmt, 11, sess->x_forwarded_for) != DB_OK)
		{
			log_err("Cannot bind x_forwarded_for = %s in backup_db_sessions(): %s (%d)",
			        sess->x_forwarded_for, db_errmsg(db), db_errcode(db));
			goto backup_db_sessions_end;
		}

		// Execute statement
		if(db_step(stmt) != DB_DONE)
		{
			log_err("SQL error in backup_db_sessions(): %s (%d)",
			        db_errmsg(db), db_errcode(db));
			goto backup_db_sessions_end;
		}

		// Reset statement
		if(db_reset(stmt) != DB_OK)
		{
			log_err("SQL error in backup_db_sessions(): %s (%d)",
			        db_errmsg(db), db_errcode(db));
			goto backup_db_sessions_end;
		}

		api_sessions++;
	}

	success = true;
	log_info("Stored %u API session%s in the database",
	         api_sessions, api_sessions == 1 ? "" : "s");

backup_db_sessions_end:
	// Finalize statement and close database connection
	db_finalize(stmt);
	dbclose(&db);

	return success;
}

// Restore all sessions found in the database
bool restore_db_sessions(struct session *sessions, const uint16_t max_sessions)
{
	if(!config.webserver.session.restore.v.b)
	{
		log_debug(DEBUG_API, "Session restore is disabled, not restoring sessions from database");
		return true;
	}

	const char *prefix = "";
	db_conn *memdb = get_longterm_db(&prefix);
	if(memdb == NULL)
		return false;

	// Remove expired sessions from database
	if(dbquery(memdb, "DELETE FROM %ssession WHERE valid_until < %s", prefix,
	           memdb->drv->dialect->now_expr()) != DB_OK)
	{
		log_err("restore_db_sessions(): Cannot remove expired sessions: %s", DB_LAST_ERR(memdb));
		release_longterm_db(&memdb);
		return false;
	}

	// Get all sessions from database
	char selectstr[256];
	snprintf(selectstr, sizeof(selectstr), "SELECT login_at, valid_until, remote_addr, user_agent, sid, csrf, "
	                                       "tls_login, tls_mixed, app, cli, x_forwarded_for FROM %ssession", prefix);
	db_stmt *stmt = NULL;
	if((stmt = db_prepare(memdb, selectstr, false)) == NULL)
	{
		log_err("SQL error in restore_db_sessions(): %s (%d)",
		        db_errmsg(memdb), db_errcode(memdb));
		release_longterm_db(&memdb);
		return false;
	}

	// Iterate over all still valid sessions
	unsigned int i = 0;
	while(db_step(stmt) == DB_ROW && i < max_sessions)
	{
		// Allocate memory for new session
		struct session *sess = &sessions[i];

		// Get values from database
		// 1: login_at
		sess->login_at = db_column_int64(stmt, 0);

		// 2: valid_until
		sess->valid_until = db_column_int64(stmt, 1);

		// 3: remote_addr
		const char *remote_addr = (const char *)db_column_text(stmt, 2);
		if(remote_addr != NULL)
		{
			strncpy(sess->remote_addr, remote_addr, sizeof(sess->remote_addr)-1);
			sess->remote_addr[sizeof(sess->remote_addr)-1] = '\0';
		}

		// 4: user_agent
		const char *user_agent = (const char *)db_column_text(stmt, 3);
		if(user_agent != NULL)
		{
			strncpy(sess->user_agent, user_agent, sizeof(sess->user_agent)-1);
			sess->user_agent[sizeof(sess->user_agent)-1] = '\0';
		}

		// 5: sid
		const char *sid = (const char *)db_column_text(stmt, 4);
		if(sid != NULL)
		{
			strncpy(sess->sid, sid, sizeof(sess->sid)-1);
			sess->sid[sizeof(sess->sid)-1] = '\0';
		}

		// 6: csrf
		const char *csrf = (const char *)db_column_text(stmt, 5);
		if(csrf != NULL)
		{
			strncpy(sess->csrf, csrf, sizeof(sess->csrf)-1);
			sess->csrf[sizeof(sess->csrf)-1] = '\0';
		}

		// 7: tls_login
		sess->tls.login = db_column_int(stmt, 6) == 1 ? true : false;

		// 8: tls_mixed
		sess->tls.mixed = db_column_int(stmt, 7) == 1 ? true : false;

		// 9: app
		sess->app = db_column_int(stmt, 8) == 1 ? true : false;

		// 10: cli
		sess->cli = db_column_int(stmt, 9) == 1 ? true : false;

		// 11: x_forwarded_for
		const char *x_forwarded_for = (const char *)db_column_text(stmt, 10);
		if(x_forwarded_for != NULL)
		{
			strncpy(sess->x_forwarded_for, x_forwarded_for, sizeof(sess->x_forwarded_for)-1);
			sess->x_forwarded_for[sizeof(sess->x_forwarded_for)-1] = '\0';
		}

		// Mark session as used
		sess->used = true;

		i++;
	}

	log_info("Restored %u API session%s from the database",
	         i, i == 1 ? "" : "s");

	// Finalize statement
	db_finalize(stmt);

	// Delete all sessions from database after restoring them
	// We use secure_delete to make sure the sessions are really gone
	// In this mode, SQLite overwrites the deleted content with zeros
	// (https://www.sqlite.org/pragma.html#pragma_secure_delete)
	// PostgreSQL has no such mode, its pages are vacuumed
	const bool secure = strcmp(memdb->drv->name, "sqlite") == 0;
	bool okay = true;
	if(secure && dbquery(memdb, "PRAGMA secure_delete = ON") != DB_OK)
		okay = false;
	if(okay && dbquery(memdb, "DELETE FROM %ssession", prefix) != DB_OK)
	{
		log_err("restore_db_sessions(): Cannot remove the restored sessions: %s", DB_LAST_ERR(memdb));
		okay = false;
	}
	if(secure && dbquery(memdb, "PRAGMA secure_delete = OFF") != DB_OK)
		okay = false;
	release_longterm_db(&memdb);

	return okay;
}
