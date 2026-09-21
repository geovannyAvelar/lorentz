/* Pi-hole: A black hole for Internet advertisements
*  (c) 2026 Pi-hole, LLC (https://pi-hole.net)
*  Network-wide ad blocking via your own hardware.
*
*  FTL Engine
*  SQLite3 database driver
*  /src/database/db-sqlite.c
*
*  This file is copyright under the latest version of the EUPL.
*  Please see LICENSE file for your rights under this license. */

#include "FTL.h"
#include "sqlite3.h"
#include "db-driver.h"
#include "log.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
	db_conn base; // must be first
	sqlite3 *raw;
} sqlite_conn;

typedef struct {
	db_stmt base; // must be first
	sqlite3_stmt *raw;
} sqlite_stmt;

static inline sqlite3 *dbh(db_conn *conn)
{
	return ((sqlite_conn*)conn)->raw;
}

static inline sqlite3_stmt *sth(db_stmt *stmt)
{
	return ((sqlite_stmt*)stmt)->raw;
}

// Map a raw SQLite result code (primary or extended) to db_rc
static db_rc map_rc(const int rc)
{
	switch(rc & 0xFF)
	{
		case SQLITE_OK:
			return DB_OK;
		case SQLITE_ROW:
			return DB_ROW;
		case SQLITE_DONE:
			return DB_DONE;
		case SQLITE_BUSY:
		case SQLITE_LOCKED:
			return DB_BUSY;
		case SQLITE_CONSTRAINT:
			return DB_CONSTRAINT;
		case SQLITE_CORRUPT:
			return DB_CORRUPT;
		case SQLITE_READONLY:
			return DB_READONLY;
		default:
			return DB_ERROR;
	}
}

/* ---- driver lifecycle ---- */

static int sqlite_init(void)
{
	return sqlite3_initialize() == SQLITE_OK ? 0 : -1;
}

static void sqlite_shutdown(void)
{
	sqlite3_shutdown();
}

static const char *sqlite_version(void)
{
	return sqlite3_libversion();
}

static void sqlite_set_log_callback(void (*cb)(void *arg, int code, const char *msg), void *arg)
{
	sqlite3_config(SQLITE_CONFIG_LOG, cb, arg);
}

/* ---- connection ---- */

static db_conn *sqlite_open(const char *uri, unsigned int flags, db_rc *rcp, const char **msg)
{
	int sflags = 0;
	if(flags & DB_OPEN_READONLY)
		sflags |= SQLITE_OPEN_READONLY;
	else
		sflags |= SQLITE_OPEN_READWRITE;
	if((flags & DB_OPEN_CREATE) && !(flags & DB_OPEN_READONLY))
		sflags |= SQLITE_OPEN_CREATE;
	if(flags & DB_OPEN_MEMORY)
		sflags |= SQLITE_OPEN_MEMORY;
	if(flags & DB_OPEN_URI)
		sflags |= SQLITE_OPEN_URI;
	if(flags & DB_OPEN_NOMUTEX)
		sflags |= SQLITE_OPEN_NOMUTEX;

	if(uri == NULL)
	{
		if(!(flags & DB_OPEN_MEMORY))
		{
			if(rcp != NULL)
				*rcp = DB_ERROR;
			if(msg != NULL)
				*msg = "no database path given";
			return NULL;
		}
		uri = ":memory:";
	}

	sqlite3 *raw = NULL;
	const int orc = sqlite3_open_v2(uri, &raw, sflags, NULL);
	if(orc != SQLITE_OK)
	{
		if(rcp != NULL)
			*rcp = map_rc(orc);
		if(msg != NULL)
			*msg = sqlite3_errstr(orc);
		// sqlite3_open_v2() allocates a handle even on failure
		sqlite3_close(raw);
		return NULL;
	}

	sqlite_conn *conn = calloc(1, sizeof(*conn));
	if(conn == NULL)
	{
		sqlite3_close(raw);
		return NULL;
	}
	conn->base.drv = &db_driver_sqlite;
	conn->raw = raw;
	return &conn->base;
}

// sqlite3_close() refuses while a statement of the connection is still alive,
// so finalize what was left behind - naming it, as it is a bug
static void sqlite_close(db_conn *conn)
{
	if(conn == NULL)
		return;

	sqlite3_stmt *stmt = NULL;
	while((stmt = sqlite3_next_stmt(dbh(conn), NULL)) != NULL)
	{
		log_err("Statement not finalized when closing database: %s", sqlite3_sql(stmt));
		sqlite3_finalize(stmt);
	}

	const int rc = sqlite3_close_v2(dbh(conn));
	if(rc != SQLITE_OK)
		log_err("Error while trying to close database: %s", sqlite3_errstr(rc));

	free(conn);
}

// sqlite3_close_v2() turns the connection into a zombie while statements are
// still alive. It goes away when the last of them is finalized
static void sqlite_close_deferred(db_conn *conn)
{
	if(conn == NULL)
		return;

	const int rc = sqlite3_close_v2(dbh(conn));
	if(rc != SQLITE_OK)
		log_err("Error while trying to close database: %s", sqlite3_errstr(rc));

	free(conn);
}

static void sqlite_interrupt(db_conn *conn)
{
	sqlite3_interrupt(dbh(conn));
}

static bool sqlite_is_readonly(db_conn *conn)
{
	return sqlite3_db_readonly(dbh(conn), "main") == 1;
}

static db_rc sqlite_set_busy_handler(db_conn *conn, int (*cb)(void *arg, int count), void *arg)
{
	return map_rc(sqlite3_busy_handler(dbh(conn), cb, arg));
}

/* ---- statement ---- */

static db_stmt *sqlite_prepare(db_conn *conn, const char *sql, bool persistent)
{
	sqlite3_stmt *raw = NULL;
	const unsigned int pflags = persistent ? SQLITE_PREPARE_PERSISTENT : 0;
	if(sqlite3_prepare_v3(dbh(conn), sql, -1, pflags, &raw, NULL) != SQLITE_OK)
	{
		sqlite3_finalize(raw);
		return NULL;
	}

	sqlite_stmt *stmt = calloc(1, sizeof(*stmt));
	if(stmt == NULL)
	{
		sqlite3_finalize(raw);
		return NULL;
	}
	stmt->base.drv = conn->drv;
	stmt->raw = raw;
	return &stmt->base;
}

static db_rc sqlite_step(db_stmt *stmt)
{
	return map_rc(sqlite3_step(sth(stmt)));
}

static db_rc sqlite_reset(db_stmt *stmt)
{
	return map_rc(sqlite3_reset(sth(stmt)));
}

static void sqlite_finalize(db_stmt *stmt)
{
	if(stmt == NULL)
		return;
	sqlite3_finalize(sth(stmt));
	free(stmt);
}

static db_rc sqlite_exec(db_conn *conn, const char *sql)
{
	return map_rc(sqlite3_exec(dbh(conn), sql, NULL, NULL, NULL));
}

static const char *sqlite_sql_text(db_stmt *stmt)
{
	return sqlite3_sql(sth(stmt));
}

/* ---- binding (idx 1-based) ---- */

static int sqlite_param_index(db_stmt *stmt, const char *name)
{
	return sqlite3_bind_parameter_index(sth(stmt), name);
}

static db_rc sqlite_bind_int(db_stmt *stmt, int idx, int value)
{
	return map_rc(sqlite3_bind_int(sth(stmt), idx, value));
}

static db_rc sqlite_bind_int64(db_stmt *stmt, int idx, int64_t value)
{
	return map_rc(sqlite3_bind_int64(sth(stmt), idx, value));
}

static db_rc sqlite_bind_double(db_stmt *stmt, int idx, double value)
{
	return map_rc(sqlite3_bind_double(sth(stmt), idx, value));
}

static db_rc sqlite_bind_text(db_stmt *stmt, int idx, const char *value)
{
	return map_rc(sqlite3_bind_text(sth(stmt), idx, value, -1, SQLITE_TRANSIENT));
}

static db_rc sqlite_bind_text_ref(db_stmt *stmt, int idx, const char *value)
{
	return map_rc(sqlite3_bind_text(sth(stmt), idx, value, -1, SQLITE_STATIC));
}

static db_rc sqlite_bind_blob(db_stmt *stmt, int idx, const void *data, size_t len)
{
	return map_rc(sqlite3_bind_blob64(sth(stmt), idx, data, (sqlite3_uint64)len, SQLITE_TRANSIENT));
}

static db_rc sqlite_bind_null(db_stmt *stmt, int idx)
{
	return map_rc(sqlite3_bind_null(sth(stmt), idx));
}

static db_rc sqlite_bind_array(db_stmt *stmt, int idx, db_type type, const void *values, size_t count)
{
	int ctype;
	switch(type)
	{
		case DB_TYPE_INT:
			ctype = SQLITE_CARRAY_INT32;
			break;
		case DB_TYPE_INT64:
			ctype = SQLITE_CARRAY_INT64;
			break;
		case DB_TYPE_DOUBLE:
			ctype = SQLITE_CARRAY_DOUBLE;
			break;
		case DB_TYPE_TEXT:
			ctype = SQLITE_CARRAY_TEXT;
			break;
		case DB_TYPE_NULL:
		case DB_TYPE_BLOB:
		default:
			return DB_ERROR;
	}

	return map_rc(sqlite3_carray_bind(sth(stmt), idx, (void*)values, (int)count, ctype, SQLITE_STATIC));
}

/* ---- columns (idx 0-based) ---- */

static int sqlite_column_count(db_stmt *stmt)
{
	return sqlite3_column_count(sth(stmt));
}

static const char *sqlite_column_name(db_stmt *stmt, int idx)
{
	return sqlite3_column_name(sth(stmt), idx);
}

static db_type sqlite_column_type(db_stmt *stmt, int idx)
{
	switch(sqlite3_column_type(sth(stmt), idx))
	{
		case SQLITE_INTEGER:
			return DB_TYPE_INT64;
		case SQLITE_FLOAT:
			return DB_TYPE_DOUBLE;
		case SQLITE_TEXT:
			return DB_TYPE_TEXT;
		case SQLITE_BLOB:
			return DB_TYPE_BLOB;
		default:
			return DB_TYPE_NULL;
	}
}

static int sqlite_column_int(db_stmt *stmt, int idx)
{
	return sqlite3_column_int(sth(stmt), idx);
}

static int64_t sqlite_column_int64(db_stmt *stmt, int idx)
{
	return sqlite3_column_int64(sth(stmt), idx);
}

static double sqlite_column_double(db_stmt *stmt, int idx)
{
	return sqlite3_column_double(sth(stmt), idx);
}

static const char *sqlite_column_text(db_stmt *stmt, int idx)
{
	return (const char*)sqlite3_column_text(sth(stmt), idx);
}

static const void *sqlite_column_blob(db_stmt *stmt, int idx)
{
	return sqlite3_column_blob(sth(stmt), idx);
}

static int sqlite_column_bytes(db_stmt *stmt, int idx)
{
	return sqlite3_column_bytes(sth(stmt), idx);
}

/* ---- result metadata / errors ---- */

static int64_t sqlite_changes(db_conn *conn)
{
	return sqlite3_changes64(dbh(conn));
}

static int64_t sqlite_last_insert_id(db_conn *conn)
{
	return sqlite3_last_insert_rowid(dbh(conn));
}

static int sqlite_errcode(db_conn *conn)
{
	return sqlite3_errcode(dbh(conn));
}

static int sqlite_extended_errcode(db_conn *conn)
{
	return sqlite3_extended_errcode(dbh(conn));
}

static const char *sqlite_errmsg(db_conn *conn)
{
	return sqlite3_errmsg(dbh(conn));
}

static const char *sqlite_stmt_errmsg(db_stmt *stmt)
{
	return sqlite3_errmsg(sqlite3_db_handle(sth(stmt)));
}

static const char *sqlite_stmt_errstr(db_stmt *stmt)
{
	return sqlite3_errstr(sqlite3_errcode(sqlite3_db_handle(sth(stmt))));
}

static const char *sqlite_errstr(int code)
{
	return sqlite3_errstr(code);
}

// Defined in sqlite3.c
extern const char *sqlite3ErrName(int rc);

static const char *sqlite_errname(int code)
{
	return sqlite3ErrName(code);
}

static db_rc sqlite_classify_error(int code)
{
	return map_rc(code);
}

/* ---- helpers ---- */

static char *sqlite_mprintf(const char *fmt, ...)
{
	va_list args;
	va_start(args, fmt);
	char *str = sqlite3_vmprintf(fmt, args);
	va_end(args);
	return str;
}

static char *sqlite_vmprintf(const char *fmt, va_list args)
{
	return sqlite3_vmprintf(fmt, args);
}

static void sqlite_free(void *ptr)
{
	sqlite3_free(ptr);
}

// Format SQL with sqlite3_mprintf() and execute it
static db_rc exec_fmt(db_conn *conn, const char *fmt, ...)
{
	va_list args;
	va_start(args, fmt);
	char *sql = sqlite3_vmprintf(fmt, args);
	va_end(args);
	if(sql == NULL)
		return DB_ERROR;

	const db_rc rc = sqlite_exec(conn, sql);
	sqlite3_free(sql);
	return rc;
}

/* ---- transactions ---- */

static db_rc sqlite_begin(db_conn *conn, db_txmode mode)
{
	switch(mode)
	{
		case DB_TX_IMMEDIATE:
			return sqlite_exec(conn, "BEGIN IMMEDIATE");
		case DB_TX_EXCLUSIVE:
			return sqlite_exec(conn, "BEGIN EXCLUSIVE");
		case DB_TX_DEFERRED:
		default:
			return sqlite_exec(conn, "BEGIN DEFERRED");
	}
}

static db_rc sqlite_commit(db_conn *conn)
{
	return sqlite_exec(conn, "COMMIT");
}

static db_rc sqlite_rollback(db_conn *conn)
{
	return sqlite_exec(conn, "ROLLBACK");
}

static db_rc sqlite_savepoint(db_conn *conn, const char *name)
{
	return exec_fmt(conn, "SAVEPOINT \"%w\"", name);
}

static db_rc sqlite_release_savepoint(db_conn *conn, const char *name)
{
	return exec_fmt(conn, "RELEASE SAVEPOINT \"%w\"", name);
}

/* ---- multi-database ---- */

// Erase the whole database. The connection stays open and usable. Works even
// for a badly corrupted database file
static db_rc sqlite_reset_database(db_conn *conn)
{
	sqlite3_db_config(dbh(conn), SQLITE_DBCONFIG_RESET_DATABASE, 1, 0);
	const db_rc rc = sqlite_exec(conn, "VACUUM");
	sqlite3_db_config(dbh(conn), SQLITE_DBCONFIG_RESET_DATABASE, 0, 0);
	return rc;
}

// Prepare, bind up to two text parameters, step once and finalize
static db_rc run_text_stmt(db_conn *conn, const char *sql, const char *a, const char *b)
{
	sqlite3_stmt *stmt = NULL;
	int rc = sqlite3_prepare_v2(dbh(conn), sql, -1, &stmt, NULL);
	if(rc != SQLITE_OK)
		return map_rc(rc);

	if((rc = sqlite3_bind_text(stmt, 1, a, -1, SQLITE_STATIC)) == SQLITE_OK &&
	   b != NULL)
		rc = sqlite3_bind_text(stmt, 2, b, -1, SQLITE_STATIC);
	if(rc == SQLITE_OK)
		rc = sqlite3_step(stmt);

	sqlite3_finalize(stmt);
	return rc == SQLITE_DONE ? DB_OK : map_rc(rc);
}

static db_rc sqlite_attach(db_conn *conn, const char *path, const char *alias)
{
	return run_text_stmt(conn, "ATTACH ? AS ?", path, alias);
}

static db_rc sqlite_detach(db_conn *conn, const char *alias)
{
	return run_text_stmt(conn, "DETACH ?", alias, NULL);
}

static db_rc sqlite_copy_table(db_conn *conn, const char *src_alias, const char *dst_alias,
                               const char *table, const char *where)
{
	if(where != NULL && *where != '\0')
		return exec_fmt(conn, "INSERT INTO \"%w\".\"%w\" SELECT * FROM \"%w\".\"%w\" WHERE %s",
		                dst_alias, table, src_alias, table, where);

	return exec_fmt(conn, "INSERT INTO \"%w\".\"%w\" SELECT * FROM \"%w\".\"%w\"",
	                dst_alias, table, src_alias, table);
}

static void *sqlite_serialize(db_conn *conn, const char *schema, int64_t *size)
{
	sqlite3_int64 sz = 0;
	unsigned char *buf = sqlite3_serialize(dbh(conn), schema != NULL ? schema : "main", &sz, 0);
	if(size != NULL)
		*size = buf != NULL ? sz : 0;
	return buf;
}

static db_rc sqlite_deserialize(db_conn *conn, const char *schema, const void *buf, int64_t size, bool readonly)
{
	if(readonly)
	{
		// Used in place, so SQLite must not free or grow the buffer
		return map_rc(sqlite3_deserialize(dbh(conn), schema != NULL ? schema : "main",
		                                  (unsigned char*)buf, size, size,
		                                  SQLITE_DESERIALIZE_READONLY));
	}

	// SQLite takes ownership of the buffer and needs sqlite3_malloc()ed
	// memory, so hand it a private copy
	unsigned char *copy = sqlite3_malloc64((sqlite3_uint64)size);
	if(copy == NULL)
		return DB_ERROR;
	memcpy(copy, buf, (size_t)size);

	const int rc = sqlite3_deserialize(dbh(conn), schema != NULL ? schema : "main", copy, size, size,
	                                   SQLITE_DESERIALIZE_FREEONCLOSE | SQLITE_DESERIALIZE_RESIZEABLE);
	return map_rc(rc);
}

/* ---- schema / maintenance ---- */

// Run a query with up to two text parameters and report whether it has a row
static bool has_row(db_conn *conn, const char *sql, const char *a, const char *b)
{
	sqlite3_stmt *stmt = NULL;
	if(sqlite3_prepare_v2(dbh(conn), sql, -1, &stmt, NULL) != SQLITE_OK)
		return false;

	int rc = sqlite3_bind_text(stmt, 1, a, -1, SQLITE_STATIC);
	if(rc == SQLITE_OK && b != NULL)
		rc = sqlite3_bind_text(stmt, 2, b, -1, SQLITE_STATIC);

	const bool found = rc == SQLITE_OK && sqlite3_step(stmt) == SQLITE_ROW;
	sqlite3_finalize(stmt);
	return found;
}

static bool sqlite_table_exists(db_conn *conn, const char *table)
{
	return has_row(conn, "SELECT 1 FROM sqlite_master WHERE type IN ('table','view') AND name = ?1",
	               table, NULL);
}

static bool sqlite_column_exists(db_conn *conn, const char *table, const char *column)
{
	return has_row(conn, "SELECT 1 FROM pragma_table_info(?1) WHERE name = ?2", table, column);
}

// Uses PRAGMA user_version. This is independent of the FTL properties table
static int sqlite_get_schema_version(db_conn *conn)
{
	sqlite3_stmt *stmt = NULL;
	if(sqlite3_prepare_v2(dbh(conn), "PRAGMA user_version", -1, &stmt, NULL) != SQLITE_OK)
		return -1;

	int version = -1;
	if(sqlite3_step(stmt) == SQLITE_ROW)
		version = sqlite3_column_int(stmt, 0);

	sqlite3_finalize(stmt);
	return version;
}

static db_rc sqlite_set_schema_version(db_conn *conn, int version)
{
	return exec_fmt(conn, "PRAGMA user_version = %d", version);
}

// Returns -1 on error
static int64_t sqlite_row_count(db_conn *conn, const char *table)
{
	char *sql = sqlite3_mprintf("SELECT COUNT(*) FROM \"%w\"", table);
	if(sql == NULL)
		return -1;

	sqlite3_stmt *stmt = NULL;
	const int rc = sqlite3_prepare_v2(dbh(conn), sql, -1, &stmt, NULL);
	sqlite3_free(sql);
	if(rc != SQLITE_OK)
		return -1;

	int64_t count = -1;
	if(sqlite3_step(stmt) == SQLITE_ROW)
		count = sqlite3_column_int64(stmt, 0);

	sqlite3_finalize(stmt);
	return count;
}

static db_rc sqlite_vacuum(db_conn *conn)
{
	return sqlite_exec(conn, "VACUUM");
}

static db_rc sqlite_optimize(db_conn *conn)
{
	return sqlite_exec(conn, "PRAGMA optimize");
}

static db_rc sqlite_integrity_check(db_conn *conn, const char **message)
{
	static _Thread_local char msg[256];

	if(message != NULL)
		*message = NULL;

	sqlite3_stmt *stmt = NULL;
	int rc = sqlite3_prepare_v2(dbh(conn), "PRAGMA integrity_check", -1, &stmt, NULL);
	if(rc != SQLITE_OK)
		return map_rc(rc);

	db_rc result = DB_ERROR;
	if((rc = sqlite3_step(stmt)) == SQLITE_ROW)
	{
		const char *text = (const char*)sqlite3_column_text(stmt, 0);
		if(text != NULL && strcmp(text, "ok") == 0)
			result = DB_OK;
		else if(message != NULL)
		{
			snprintf(msg, sizeof(msg), "%s", text != NULL ? text : "unknown error");
			*message = msg;
		}
	}
	else
		result = map_rc(rc);

	sqlite3_finalize(stmt);
	return result;
}

/* ---- dialect ---- */

static const char *sqlite_dialect_placeholder(unsigned int n)
{
	(void)n;
	return "?";
}

static const char *sqlite_dialect_now_expr(void)
{
	return "CAST(strftime('%s','now') AS INTEGER)";
}

static const char *sqlite_dialect_autoincrement_pk(void)
{
	return "INTEGER PRIMARY KEY AUTOINCREMENT";
}

static const char *sqlite_dialect_upsert_suffix(const char *conflict_col, const char *update_set)
{
	static _Thread_local char buf[512];

	if(update_set == NULL || *update_set == '\0')
		snprintf(buf, sizeof(buf), "ON CONFLICT(%s) DO NOTHING", conflict_col);
	else
		snprintf(buf, sizeof(buf), "ON CONFLICT(%s) DO UPDATE SET %s", conflict_col, update_set);

	return buf;
}

static const char *sqlite_dialect_glob_op(void)
{
	return "GLOB";
}

static const char *sqlite_dialect_regexp_op(void)
{
	return "REGEXP";
}

static int sqlite_dialect_in_list(char *buf, size_t size, const char *column, const char *bind_name)
{
	const int len = snprintf(buf, size, "%s IN carray(%s)", column, bind_name);
	return (len < 0 || (size_t)len >= size) ? -1 : len;
}

static const db_dialect sqlite_dialect = {
	.name = "sqlite",
	.placeholder = sqlite_dialect_placeholder,
	.now_expr = sqlite_dialect_now_expr,
	.autoincrement_pk = sqlite_dialect_autoincrement_pk,
	.upsert_suffix = sqlite_dialect_upsert_suffix,
	.glob_op = sqlite_dialect_glob_op,
	.regexp_op = sqlite_dialect_regexp_op,
	.in_list = sqlite_dialect_in_list
};

/* ---- driver instance ---- */

const db_driver db_driver_sqlite = {
	.name = "sqlite",

	.init = sqlite_init,
	.shutdown = sqlite_shutdown,
	.version = sqlite_version,
	.set_log_callback = sqlite_set_log_callback,

	.open = sqlite_open,
	.close = sqlite_close,
	.close_deferred = sqlite_close_deferred,
	.interrupt = sqlite_interrupt,
	.is_readonly = sqlite_is_readonly,
	.set_busy_handler = sqlite_set_busy_handler,

	.prepare = sqlite_prepare,
	.step = sqlite_step,
	.reset = sqlite_reset,
	.finalize = sqlite_finalize,
	.exec = sqlite_exec,
	.sql_text = sqlite_sql_text,

	.param_index = sqlite_param_index,
	.bind_int = sqlite_bind_int,
	.bind_int64 = sqlite_bind_int64,
	.bind_double = sqlite_bind_double,
	.bind_text = sqlite_bind_text,
	.bind_text_ref = sqlite_bind_text_ref,
	.bind_blob = sqlite_bind_blob,
	.bind_null = sqlite_bind_null,
	.bind_array = sqlite_bind_array,

	.column_count = sqlite_column_count,
	.column_name = sqlite_column_name,
	.column_type = sqlite_column_type,
	.column_int = sqlite_column_int,
	.column_int64 = sqlite_column_int64,
	.column_double = sqlite_column_double,
	.column_text = sqlite_column_text,
	.column_blob = sqlite_column_blob,
	.column_bytes = sqlite_column_bytes,

	.changes = sqlite_changes,
	.last_insert_id = sqlite_last_insert_id,
	.errcode = sqlite_errcode,
	.extended_errcode = sqlite_extended_errcode,
	.errmsg = sqlite_errmsg,
	.stmt_errmsg = sqlite_stmt_errmsg,
	.stmt_errstr = sqlite_stmt_errstr,
	.errstr = sqlite_errstr,
	.errname = sqlite_errname,
	.classify_error = sqlite_classify_error,

	.begin = sqlite_begin,
	.commit = sqlite_commit,
	.rollback = sqlite_rollback,
	.savepoint = sqlite_savepoint,
	.release_savepoint = sqlite_release_savepoint,

	.reset_database = sqlite_reset_database,
	.attach = sqlite_attach,
	.detach = sqlite_detach,
	.copy_table = sqlite_copy_table,
	.serialize = sqlite_serialize,
	.deserialize = sqlite_deserialize,

	.table_exists = sqlite_table_exists,
	.column_exists = sqlite_column_exists,
	.get_schema_version = sqlite_get_schema_version,
	.set_schema_version = sqlite_set_schema_version,
	.row_count = sqlite_row_count,
	.vacuum = sqlite_vacuum,
	.optimize = sqlite_optimize,
	.integrity_check = sqlite_integrity_check,

	.mprintf = sqlite_mprintf,
	.vmprintf = sqlite_vmprintf,
	.mem_free = sqlite_free,

	.dialect = &sqlite_dialect
};
