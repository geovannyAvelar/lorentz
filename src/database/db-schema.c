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
		"x_forwarded_for TEXT, "
		"user_id BIGINT)",

	// Accounts of the API (version 23). role is admin or viewer
	"CREATE TABLE users ("
		"id @PK@, "
		"username TEXT UNIQUE NOT NULL, "
		"pwhash TEXT NOT NULL, "
		"role TEXT NOT NULL DEFAULT 'admin', "
		"enabled SMALLINT NOT NULL DEFAULT 1, "
		"comment TEXT, "
		"created_at BIGINT NOT NULL, "
		"updated_at BIGINT NOT NULL, "
		"last_login BIGINT)",
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

// The gravity database: what the API edits (lists, groups, clients, allow and
// deny domains) and what a gravity run derives from the lists (the domains
// they contain), the tables of the gravity.db file of a SQLite setup with the
// same names and columns. Its triggers are written for a server (PL/pgSQL):
// SQLite creates its file from a schema of its own.
//   * flags are SMALLINT, compared with 0 and 1 as in SQLite,
//   * the modification time is set by a BEFORE trigger, where SQLite updates the
//     row afterwards - a server would run that trigger on its own update again,
//   * the junction tables cascade on delete, which replaces SQLite's triggers
//     for it.
// The views are the ones of the SQLite file. They use LEFT JOINs deliberately:
// groups can be removed from entries, leaving no row in a junction table, and
// such an entry has to match every client rather than vanish.
static const char *const gravity_statements[] = {
	"CREATE TABLE \"group\" ("
		"id @PK@, "
		"enabled SMALLINT NOT NULL DEFAULT 1, "
		"name TEXT UNIQUE NOT NULL, "
		"date_added BIGINT NOT NULL DEFAULT (@NOW@), "
		"date_modified BIGINT NOT NULL DEFAULT (@NOW@), "
		"description TEXT)",
	"CREATE TABLE domainlist ("
		"id @PK@, "
		"type INTEGER NOT NULL DEFAULT 0, "
		"domain TEXT NOT NULL, "
		"enabled SMALLINT NOT NULL DEFAULT 1, "
		"date_added BIGINT NOT NULL DEFAULT (@NOW@), "
		"date_modified BIGINT NOT NULL DEFAULT (@NOW@), "
		"comment TEXT, "
		"UNIQUE (domain, type))",
	"CREATE TABLE adlist ("
		"id @PK@, "
		"address TEXT NOT NULL, "
		"enabled SMALLINT NOT NULL DEFAULT 1, "
		"date_added BIGINT NOT NULL DEFAULT (@NOW@), "
		"date_modified BIGINT NOT NULL DEFAULT (@NOW@), "
		"comment TEXT, "
		"date_updated BIGINT, "
		"number BIGINT NOT NULL DEFAULT 0, "
		"invalid_domains BIGINT NOT NULL DEFAULT 0, "
		"status INTEGER NOT NULL DEFAULT 0, "
		"abp_entries BIGINT NOT NULL DEFAULT 0, "
		"type INTEGER NOT NULL DEFAULT 0, "
		"UNIQUE (address, type))",
	"CREATE TABLE adlist_by_group ("
		"adlist_id BIGINT NOT NULL REFERENCES adlist (id) ON DELETE CASCADE, "
		"group_id BIGINT NOT NULL REFERENCES \"group\" (id) ON DELETE CASCADE, "
		"PRIMARY KEY (adlist_id, group_id))",
	"CREATE TABLE gravity ("
		"domain TEXT NOT NULL, "
		"adlist_id BIGINT NOT NULL REFERENCES adlist (id))",
	"CREATE TABLE antigravity ("
		"domain TEXT NOT NULL, "
		"adlist_id BIGINT NOT NULL REFERENCES adlist (id))",
	"CREATE TABLE info (property TEXT PRIMARY KEY, value TEXT NOT NULL)",
	"CREATE TABLE domainlist_by_group ("
		"domainlist_id BIGINT NOT NULL REFERENCES domainlist (id) ON DELETE CASCADE, "
		"group_id BIGINT NOT NULL REFERENCES \"group\" (id) ON DELETE CASCADE, "
		"PRIMARY KEY (domainlist_id, group_id))",
	"CREATE TABLE client ("
		"id @PK@, "
		"ip TEXT NOT NULL UNIQUE, "
		"date_added BIGINT NOT NULL DEFAULT (@NOW@), "
		"date_modified BIGINT NOT NULL DEFAULT (@NOW@), "
		"comment TEXT)",
	"CREATE TABLE client_by_group ("
		"client_id BIGINT NOT NULL REFERENCES client (id) ON DELETE CASCADE, "
		"group_id BIGINT NOT NULL REFERENCES \"group\" (id) ON DELETE CASCADE, "
		"PRIMARY KEY (client_id, group_id))",

	"CREATE INDEX idx_adlist_by_group_gid ON adlist_by_group (group_id, adlist_id)",
	"CREATE INDEX idx_domainlist_by_group_gid ON domainlist_by_group (group_id, domainlist_id)",
	"CREATE INDEX idx_gravity ON gravity (domain, adlist_id)",
	"CREATE INDEX idx_antigravity ON antigravity (domain, adlist_id)",
	"CREATE INDEX idx_gravity_adlist ON gravity (adlist_id)",
	"CREATE INDEX idx_antigravity_adlist ON antigravity (adlist_id)",

	// date_modified follows a change of the entry (of an adlist, only of what
	// the user edits: a gravity run updates number, status and the like)
	"CREATE FUNCTION tr_touch_date_modified() RETURNS trigger AS $$ "
		"BEGIN NEW.date_modified := @NOW@; RETURN NEW; END $$ LANGUAGE plpgsql",
	"CREATE TRIGGER tr_adlist_update BEFORE UPDATE OF address, enabled, comment ON adlist "
		"FOR EACH ROW EXECUTE FUNCTION tr_touch_date_modified()",
	"CREATE TRIGGER tr_client_update BEFORE UPDATE ON client "
		"FOR EACH ROW EXECUTE FUNCTION tr_touch_date_modified()",
	"CREATE TRIGGER tr_domainlist_update BEFORE UPDATE ON domainlist "
		"FOR EACH ROW EXECUTE FUNCTION tr_touch_date_modified()",
	"CREATE TRIGGER tr_group_update BEFORE UPDATE ON \"group\" "
		"FOR EACH ROW EXECUTE FUNCTION tr_touch_date_modified()",

	// A new entry belongs to the default group, group 0
	"CREATE FUNCTION tr_domainlist_to_default_group() RETURNS trigger AS $$ "
		"BEGIN INSERT INTO domainlist_by_group (domainlist_id, group_id) VALUES (NEW.id, 0); "
		"RETURN NULL; END $$ LANGUAGE plpgsql",
	"CREATE TRIGGER tr_domainlist_add AFTER INSERT ON domainlist "
		"FOR EACH ROW EXECUTE FUNCTION tr_domainlist_to_default_group()",
	"CREATE FUNCTION tr_client_to_default_group() RETURNS trigger AS $$ "
		"BEGIN INSERT INTO client_by_group (client_id, group_id) VALUES (NEW.id, 0); "
		"RETURN NULL; END $$ LANGUAGE plpgsql",
	"CREATE TRIGGER tr_client_add AFTER INSERT ON client "
		"FOR EACH ROW EXECUTE FUNCTION tr_client_to_default_group()",
	"CREATE FUNCTION tr_adlist_to_default_group() RETURNS trigger AS $$ "
		"BEGIN INSERT INTO adlist_by_group (adlist_id, group_id) VALUES (NEW.id, 0); "
		"RETURN NULL; END $$ LANGUAGE plpgsql",
	"CREATE TRIGGER tr_adlist_add AFTER INSERT ON adlist "
		"FOR EACH ROW EXECUTE FUNCTION tr_adlist_to_default_group()",

	// Group 0 cannot be deleted for good
	"CREATE FUNCTION tr_group_zero() RETURNS trigger AS $$ "
		"BEGIN INSERT INTO \"group\" (id, enabled, name) VALUES (0, 1, 'Default') "
		"ON CONFLICT DO NOTHING; RETURN NULL; END $$ LANGUAGE plpgsql",
	"CREATE TRIGGER tr_group_zero AFTER DELETE ON \"group\" "
		"FOR EACH ROW EXECUTE FUNCTION tr_group_zero()",

#define GRAVITY_LIST_VIEW(name, type) \
	"CREATE VIEW " name " AS SELECT domain, domainlist.id AS id, domainlist_by_group.group_id AS group_id " \
		"FROM domainlist " \
		"LEFT JOIN domainlist_by_group ON domainlist_by_group.domainlist_id = domainlist.id " \
		"LEFT JOIN \"group\" ON \"group\".id = domainlist_by_group.group_id " \
		"WHERE domainlist.enabled = 1 AND (domainlist_by_group.group_id IS NULL OR \"group\".enabled = 1) " \
		"AND domainlist.type = " type
	GRAVITY_LIST_VIEW("vw_allowlist", "0"),
	GRAVITY_LIST_VIEW("vw_denylist", "1"),
	GRAVITY_LIST_VIEW("vw_regex_allowlist", "2"),
	GRAVITY_LIST_VIEW("vw_regex_denylist", "3"),
#undef GRAVITY_LIST_VIEW

	"CREATE VIEW vw_gravity AS SELECT domain, adlist.id AS adlist_id, adlist_by_group.group_id AS group_id "
		"FROM gravity "
		"LEFT JOIN adlist_by_group ON adlist_by_group.adlist_id = gravity.adlist_id "
		"LEFT JOIN adlist ON adlist.id = gravity.adlist_id "
		"LEFT JOIN \"group\" ON \"group\".id = adlist_by_group.group_id "
		"WHERE adlist.enabled = 1 AND (adlist_by_group.group_id IS NULL OR \"group\".enabled = 1)",
	"CREATE VIEW vw_antigravity AS SELECT domain, adlist.id AS adlist_id, adlist_by_group.group_id AS group_id "
		"FROM antigravity "
		"LEFT JOIN adlist_by_group ON adlist_by_group.adlist_id = antigravity.adlist_id "
		"LEFT JOIN adlist ON adlist.id = antigravity.adlist_id "
		"LEFT JOIN \"group\" ON \"group\".id = adlist_by_group.group_id "
		"WHERE adlist.enabled = 1 AND (adlist_by_group.group_id IS NULL OR \"group\".enabled = 1) "
		"AND adlist.type = 1",
	"CREATE VIEW vw_adlist AS SELECT DISTINCT address, id, type FROM adlist WHERE enabled = 1 ORDER BY id",

	// Rows the tables start with: the default group, and the properties a gravity
	// run maintains
	"INSERT INTO \"group\" (id, enabled, name, description) VALUES (0, 1, 'Default', 'The default group')",
	"INSERT INTO info VALUES ('version', '" GRAVITY_SCHEMA_VERSION_STR "')",
	"INSERT INTO info VALUES ('gravity_count', '0')",
	"INSERT INTO info VALUES ('antigravity_count', '0')",
	"INSERT INTO info VALUES ('abp_domains', '0')",
	"INSERT INTO info VALUES ('updated', '0')",
};

bool db_schema_gravity_baseline(db_conn *db, const char **error)
{
	const db_dialect *dialect = db->drv->dialect;

	if(db_begin(db, DB_TX_DEFERRED) != DB_OK)
		return fail(db, "cannot start the transaction", error);

	for(size_t i = 0; i < sizeof(gravity_statements) / sizeof(gravity_statements[0]); i++)
	{
		char *sql = expand(gravity_statements[i], dialect->autoincrement_pk(), dialect->now_expr());
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

	if(db_commit(db) != DB_OK)
	{
		const bool ok = fail(db, "cannot commit the gravity schema", error);
		db_rollback(db);
		return ok;
	}

	return true;
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
