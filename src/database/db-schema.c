/* Lorentz: A black hole for Internet advertisements
*  (c) 2026 Pi-hole, LLC (https://pi-hole.net)
*  Network-wide ad blocking via your own hardware.
*
*  Lorentz Engine
*  Baseline schema of the long-term database
*  /src/database/db-schema.c
*
*  This file is copyright under the latest version of the EUPL.
*  Please see LICENSE file for your rights under this license. */

// This file only depends on the driver interface, so it can be used and tested
// without the rest of Lorentz.
//
// The tables are the ones a SQLite database has after all migrations
// (verified by the regression harness, which compares them). The column types
// were chosen so that they mean the same thing to SQLite and to servers:
//   * ids, counters and Unix timestamps are BIGINT,
//   * query timestamps and reply times have fractions and are DOUBLE PRECISION,
//   * flags are SMALLINT (SQLite stores them as 0 and 1),
//   * columns that SQLite left without a type and stored numbers, text and
//     floating point in are TEXT: message.blob1..5 and addinfo_by_id.content
//     (readers convert with the column_int/column_double accessors).
// Names are lower case in the server, unquoted identifiers are folded there.

#include "db-schema.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

// @PK@ is the dialect's auto-incrementing primary key, @NOW@ the Unix time
static const char *const baseline_statements[] = {
	"CREATE TABLE lorentz ("
		"id BIGINT PRIMARY KEY NOT NULL, "
		"value BIGINT NOT NULL, "
		"description TEXT)",

	"CREATE TABLE counters ("
		"id BIGINT PRIMARY KEY NOT NULL, "
		"value BIGINT NOT NULL)",

	"CREATE TABLE query_storage ("
		"id @PK@, "
		"timestamp DOUBLE PRECISION NOT NULL, "
		"type INTEGER NOT NULL, "
		"status INTEGER NOT NULL, "
		"domain BIGINT NOT NULL, "
		"client BIGINT NOT NULL, "
		"forward BIGINT, "
		"additional_info BIGINT, "
		"reply_type INTEGER, "
		"reply_time DOUBLE PRECISION, "
		"dnssec INTEGER, "
		"list_id INTEGER, "
		"ede INTEGER)",
	"CREATE INDEX idx_queries_timestamp ON query_storage (timestamp)",

	"CREATE TABLE domain_by_id (id @PK@, domain TEXT NOT NULL)",
	"CREATE UNIQUE INDEX domain_by_id_domain_idx ON domain_by_id (domain)",
	"CREATE TABLE client_by_id (id @PK@, ip TEXT NOT NULL, name TEXT)",
	"CREATE UNIQUE INDEX client_by_id_client_idx ON client_by_id (ip, name)",
	"CREATE TABLE forward_by_id (id @PK@, forward TEXT NOT NULL)",
	"CREATE UNIQUE INDEX forward_by_id_forward_idx ON forward_by_id (forward)",
	"CREATE TABLE addinfo_by_id (id @PK@, type INTEGER NOT NULL, content TEXT NOT NULL)",
	"CREATE UNIQUE INDEX addinfo_by_id_idx ON addinfo_by_id (type, content)",

	// The ids in the storage table point into the tables above. Rows the
	// tables do not know show the id itself
	"CREATE VIEW queries AS SELECT q.id, q.timestamp, q.type, q.status, "
		"COALESCE(d.domain, CAST(q.domain AS TEXT)) AS domain, "
		"COALESCE(c.ip, CAST(q.client AS TEXT)) AS client, "
		"COALESCE(f.forward, CAST(q.forward AS TEXT)) AS forward, "
		"COALESCE(a.content, CAST(q.additional_info AS TEXT)) AS additional_info, "
		"q.reply_type, q.reply_time, q.dnssec, q.list_id, q.ede "
		"FROM query_storage q "
		"LEFT JOIN domain_by_id d ON q.domain = d.id "
		"LEFT JOIN client_by_id c ON q.client = c.id "
		"LEFT JOIN forward_by_id f ON q.forward = f.id "
		"LEFT JOIN addinfo_by_id a ON q.additional_info = a.id",

	"CREATE TABLE message ("
		"id @PK@, "
		"timestamp BIGINT NOT NULL, "
		"type TEXT NOT NULL, "
		"message TEXT NOT NULL, "
		"blob1 TEXT, blob2 TEXT, blob3 TEXT, blob4 TEXT, blob5 TEXT)",

	"CREATE TABLE network ("
		"id @PK@, "
		"hwaddr TEXT UNIQUE NOT NULL, "
		"interface TEXT NOT NULL, "
		"firstSeen BIGINT NOT NULL, "
		"lastQuery BIGINT NOT NULL, "
		"numQueries BIGINT NOT NULL, "
		"macVendor TEXT, "
		"aliasclient_id BIGINT)",
	"CREATE TABLE network_addresses ("
		"network_id BIGINT NOT NULL REFERENCES network (id), "
		"ip TEXT UNIQUE NOT NULL, "
		"lastSeen BIGINT NOT NULL DEFAULT (@NOW@), "
		"name TEXT, "
		"nameUpdated BIGINT)",
	"CREATE INDEX network_addresses_network_id_index ON network_addresses (network_id)",

	"CREATE TABLE aliasclient (id @PK@, name TEXT NOT NULL, comment TEXT)",

	"CREATE TABLE session ("
		"id @PK@, "
		"login_at BIGINT NOT NULL, "
		"valid_until BIGINT NOT NULL, "
		"remote_addr TEXT NOT NULL, "
		"user_agent TEXT, "
		"sid TEXT NOT NULL, "
		"csrf TEXT NOT NULL, "
		"tls_login SMALLINT, "
		"tls_mixed SMALLINT, "
		"app SMALLINT, "
		"cli SMALLINT, "
		"x_forwarded_for TEXT)",
};

// Rows the tables start with: the version, the time of the latest stored query
// and the creation time (ids 0, 1 and 2 of the lorentz table), and the two
// counters
static const struct { int id; const char *description; } lorentz_rows[] = {
	{ 0, "Database version" },
	{ 1, "Unix timestamp of the latest stored query" },
	{ 2, "Unix timestamp of the database creation" },
};

static _Thread_local char error_buffer[512];

static bool fail(db_conn *db, const char *what, const char **error)
{
	snprintf(error_buffer, sizeof(error_buffer), "%s: %s", what, db_errmsg(db));
	if(error != NULL)
		*error = error_buffer;
	return false;
}

// Replace the tokens of a statement by the dialect's text. Free the result
static char *expand(const char *sql, const char *pk, const char *now)
{
	const size_t pk_len = strlen(pk), now_len = strlen(now);
	char *out = malloc(strlen(sql) + 16 * (pk_len + now_len) + 1);
	if(out == NULL)
		return NULL;

	char *o = out;
	while(*sql != '\0')
	{
		if(strncmp(sql, "@PK@", 4) == 0)
		{
			memcpy(o, pk, pk_len);
			o += pk_len;
			sql += 4;
		}
		else if(strncmp(sql, "@NOW@", 5) == 0)
		{
			memcpy(o, now, now_len);
			o += now_len;
			sql += 5;
		}
		else
			*o++ = *sql++;
	}
	*o = '\0';
	return out;
}

static bool insert_pair(db_conn *db, const char *sql, int64_t a, int64_t b, const char *text)
{
	db_stmt *stmt = db_prepare(db, sql, false);
	if(stmt == NULL)
		return false;

	bool okay = db_bind_int64(stmt, 1, a) == DB_OK && db_bind_int64(stmt, 2, b) == DB_OK;
	if(okay && text != NULL)
		okay = db_bind_text(stmt, 3, text) == DB_OK;
	okay = okay && db_step(stmt) == DB_DONE;
	db_finalize(stmt);
	return okay;
}

bool db_schema_baseline(db_conn *db, const char **error)
{
	const db_dialect *dialect = db->drv->dialect;

	if(db_begin(db, DB_TX_DEFERRED) != DB_OK)
		return fail(db, "cannot start the transaction", error);

	for(size_t i = 0; i < sizeof(baseline_statements) / sizeof(baseline_statements[0]); i++)
	{
		char *sql = expand(baseline_statements[i], dialect->autoincrement_pk(), dialect->now_expr());
		const db_rc rc = sql != NULL ? db_exec(db, sql) : DB_ERROR;
		if(rc != DB_OK)
		{
			const bool ok = fail(db, sql != NULL ? sql : "out of memory", error);
			free(sql);
			db_rollback(db);
			return ok;
		}
		free(sql);
	}

	// The version, the time of the latest query (none yet) and the creation
	// time, each with its description
	const int64_t now = (int64_t)time(NULL);
	const int64_t values[] = { DB_SCHEMA_VERSION, 0, now };
	for(size_t i = 0; i < sizeof(lorentz_rows) / sizeof(lorentz_rows[0]); i++)
		if(!insert_pair(db, "INSERT INTO lorentz (id, value, description) VALUES (?, ?, ?)",
		                lorentz_rows[i].id, values[i], lorentz_rows[i].description))
		{
			const bool ok = fail(db, "cannot store the database properties", error);
			db_rollback(db);
			return ok;
		}

	// Counters: total and blocked queries
	for(int id = 0; id < 2; id++)
		if(!insert_pair(db, "INSERT INTO counters (id, value) VALUES (?, ?)", id, 0, NULL))
		{
			const bool ok = fail(db, "cannot create the counters", error);
			db_rollback(db);
			return ok;
		}

	if(db_commit(db) != DB_OK)
	{
		const bool ok = fail(db, "cannot commit the schema", error);
		db_rollback(db);
		return ok;
	}

	return true;
}

bool db_schema_migrate(db_conn *db, int from_version, const char **error)
{
	if(from_version == DB_SCHEMA_VERSION)
		return true;

	snprintf(error_buffer, sizeof(error_buffer),
	         from_version > DB_SCHEMA_VERSION
	             ? "the database has version %d, newer than the version %d this build knows"
	             : "there is no migration from version %d to version %d for this database driver",
	         from_version, DB_SCHEMA_VERSION);
	(void)db;
	if(error != NULL)
		*error = error_buffer;
	return false;
}
