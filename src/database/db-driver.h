/* Lorentz: A black hole for Internet advertisements
*  (c) 2026 Pi-hole, LLC (https://pi-hole.net)
*  Network-wide ad blocking via your own hardware.
*
*  Lorentz Engine
*  Database driver abstraction layer (vtable)
*  /src/database/db-driver.h
*
*  This file is copyright under the latest version of the EUPL.
*  Please see LICENSE file for your rights under this license. */
#ifndef DB_DRIVER_H
#define DB_DRIVER_H

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct db_driver db_driver;

// Handles. Every driver embeds these as the FIRST member of its own handle
// struct so the dispatch helpers below can find the driver from a handle
typedef struct db_conn { const db_driver *drv; } db_conn;
typedef struct db_stmt { const db_driver *drv; } db_stmt;

// Normalized result codes. Raw driver codes come from errcode()
typedef enum {
	DB_OK = 0,
	DB_ROW,
	DB_DONE,
	DB_BUSY,
	DB_CONSTRAINT,
	DB_CORRUPT,
	DB_READONLY,
	DB_ERROR
} db_rc;

// Open flags (bitmask)
enum {
	DB_OPEN_READONLY = 1 << 0,
	DB_OPEN_READWRITE = 1 << 1,
	DB_OPEN_CREATE = 1 << 2,
	DB_OPEN_MEMORY = 1 << 3,
	DB_OPEN_URI = 1 << 4,
	DB_OPEN_NOMUTEX = 1 << 5 // Caller guarantees single-threaded use
};

// Column / array element types
typedef enum {
	DB_TYPE_NULL,
	DB_TYPE_INT,
	DB_TYPE_INT64,
	DB_TYPE_DOUBLE,
	DB_TYPE_TEXT,
	DB_TYPE_BLOB
} db_type;

typedef enum {
	DB_TX_DEFERRED,
	DB_TX_IMMEDIATE,
	DB_TX_EXCLUSIVE
} db_txmode;

// SQL dialect hooks
typedef struct db_dialect {
	const char *name;
	// Placeholder for the n-th (1-based) bind parameter
	const char *(*placeholder)(unsigned int n);
	// SQL expression for the current unix time as integer
	const char *(*now_expr)(void);
	// Column definition for an auto-incrementing integer primary key
	const char *(*autoincrement_pk)(void);
	// Clause appended to INSERT to update on conflict. The returned string
	// is valid until the next call on the same thread
	const char *(*upsert_suffix)(const char *conflict_col, const char *update_set);
	const char *(*glob_op)(void);
	const char *(*regexp_op)(void);
	// Write "<column> IN <array-parameter>" into buf, returns length or -1
	int (*in_list)(char *buf, size_t size, const char *column, const char *bind_name);
} db_dialect;

struct db_driver {
	const char *name;

	// Global driver lifecycle
	int (*init)(void);
	void (*shutdown)(void);
	const char *(*version)(void);
	// Must be called before init() and before the first open()
	void (*set_log_callback)(void (*cb)(void *arg, int code, const char *msg), void *arg);

	// Connection
	// rc and msg are optional out-parameters describing a failed open.
	// msg points to static storage
	db_conn *(*open)(const char *uri, unsigned int flags, db_rc *rc, const char **msg);
	void (*close)(db_conn *conn);
	// Release the connection but leave statements that are still alive
	// alone. The database stays open until the last of them is finalized
	void (*close_deferred)(db_conn *conn);
	void (*interrupt)(db_conn *conn);
	bool (*is_readonly)(db_conn *conn);
	db_rc (*set_busy_handler)(db_conn *conn, int (*cb)(void *arg, int count), void *arg);

	// Statement. prepare() returns NULL on failure, use errcode()/errmsg()
	db_stmt *(*prepare)(db_conn *conn, const char *sql, bool persistent);
	db_rc (*step)(db_stmt *stmt);
	db_rc (*reset)(db_stmt *stmt);
	void (*finalize)(db_stmt *stmt);
	db_rc (*exec)(db_conn *conn, const char *sql);
	const char *(*sql_text)(db_stmt *stmt);

	// Binding (idx is 1-based). bind_text() copies the string.
	// bind_array() does NOT copy: values must outlive the statement run
	int (*param_index)(db_stmt *stmt, const char *name);
	db_rc (*bind_int)(db_stmt *stmt, int idx, int value);
	db_rc (*bind_int64)(db_stmt *stmt, int idx, int64_t value);
	db_rc (*bind_double)(db_stmt *stmt, int idx, double value);
	db_rc (*bind_text)(db_stmt *stmt, int idx, const char *value);
	// Like bind_text() but does not copy: value must stay valid until the
	// statement is reset, finalized or bound again
	db_rc (*bind_text_ref)(db_stmt *stmt, int idx, const char *value);
	db_rc (*bind_blob)(db_stmt *stmt, int idx, const void *data, size_t len);
	db_rc (*bind_null)(db_stmt *stmt, int idx);
	db_rc (*bind_array)(db_stmt *stmt, int idx, db_type type, const void *values, size_t count);

	// Columns (idx is 0-based). Integer columns are reported as DB_TYPE_INT64
	int (*column_count)(db_stmt *stmt);
	const char *(*column_name)(db_stmt *stmt, int idx);
	db_type (*column_type)(db_stmt *stmt, int idx);
	int (*column_int)(db_stmt *stmt, int idx);
	int64_t (*column_int64)(db_stmt *stmt, int idx);
	double (*column_double)(db_stmt *stmt, int idx);
	const char *(*column_text)(db_stmt *stmt, int idx);
	const void *(*column_blob)(db_stmt *stmt, int idx);
	int (*column_bytes)(db_stmt *stmt, int idx);

	// Result metadata and errors
	int64_t (*changes)(db_conn *conn);
	int64_t (*last_insert_id)(db_conn *conn);
	int (*errcode)(db_conn *conn);
	int (*extended_errcode)(db_conn *conn);
	const char *(*errmsg)(db_conn *conn);
	// Error message of the connection a statement belongs to. Stays valid
	// after the connection was released with close_deferred()
	const char *(*stmt_errmsg)(db_stmt *stmt);
	// Description of the last error of the connection of a statement. Static
	// storage, valid even after the connection was closed
	const char *(*stmt_errstr)(db_stmt *stmt);
	const char *(*errstr)(int code);
	// Symbolic name of a (possibly extended) code, e.g. "SQLITE_BUSY_RECOVERY"
	const char *(*errname)(int code);
	db_rc (*classify_error)(int code);

	// Transactions
	db_rc (*begin)(db_conn *conn, db_txmode mode);
	db_rc (*commit)(db_conn *conn);
	db_rc (*rollback)(db_conn *conn);
	db_rc (*savepoint)(db_conn *conn, const char *name);
	db_rc (*release_savepoint)(db_conn *conn, const char *name);

	// Erase everything stored in the database behind conn, keeping the
	// connection usable (NULL = unsupported)
	db_rc (*reset_database)(db_conn *conn);

	// Optional multi-database features (NULL = unsupported)
	db_rc (*attach)(db_conn *conn, const char *path, const char *alias);
	db_rc (*detach)(db_conn *conn, const char *alias);
	// "where" is raw SQL and must come from trusted code
	db_rc (*copy_table)(db_conn *conn, const char *src_alias, const char *dst_alias,
	                    const char *table, const char *where);
	// Buffer returned by serialize() must be released with mem_free() (db_free())
	void *(*serialize)(db_conn *conn, const char *schema, int64_t *size);
	// By default deserialize() copies buf and the caller keeps ownership.
	// With readonly set the database is read straight out of buf without a
	// copy and cannot be written to: buf must stay valid and unchanged until
	// the connection is closed
	db_rc (*deserialize)(db_conn *conn, const char *schema, const void *buf, int64_t size, bool readonly);

	// Schema and maintenance
	bool (*table_exists)(db_conn *conn, const char *table);
	bool (*column_exists)(db_conn *conn, const char *table, const char *column);
	int (*get_schema_version)(db_conn *conn);
	db_rc (*set_schema_version)(db_conn *conn, int version);
	int64_t (*row_count)(db_conn *conn, const char *table);
	db_rc (*vacuum)(db_conn *conn);
	db_rc (*optimize)(db_conn *conn);
	// message is set to NULL on success, otherwise to a thread-local string
	db_rc (*integrity_check)(db_conn *conn, const char **message);

	// Memory helpers for driver-owned buffers
	// Format strings use the driver's own syntax (e.g. %q and %Q for SQLite)
	char *(*mprintf)(const char *fmt, ...);
	char *(*vmprintf)(const char *fmt, va_list args);
	void (*mem_free)(void *ptr);

	const db_dialect *dialect;
};

// Driver registry
extern const db_driver db_driver_sqlite;
// Only available when built with USE_POSTGRESQL
extern const db_driver db_driver_postgres;
const db_driver *db_driver_get(const char *name);
// Select the driver used by db_open(). Defaults to "sqlite"
bool db_driver_select(const char *name);
const db_driver *db_driver_active(void);
// Is the database location a connection URI (postgresql://...) rather than the path of a file?
bool db_uri_is_remote(const char *uri);
// The driver that serves a location: PostgreSQL for a connection URI, SQLite otherwise
const db_driver *db_driver_for_uri(const char *uri);
// A location that can be written to a log: the password of a connection URI is masked
const char *db_uri_display(const char *uri);

// Dispatch helpers: the API the rest of Lorentz uses. Callers never touch a driver
// directly. A NULL slot means the driver does not support the feature; helpers
// for optional features return DB_ERROR (or a neutral value) in that case
// SQLite is what the in-memory database, gravity.db and the temporary databases use, whatever the
// long-term database is
static inline db_conn *db_open_sqlite_ex(const char *uri, unsigned int flags, db_rc *rc, const char **msg) { return db_driver_sqlite.open(uri, flags, rc, msg); }
// The driver of the long-term database (files.database)
static inline db_conn *db_open_uri_ex(const char *uri, unsigned int flags, db_rc *rc, const char **msg) { return db_driver_for_uri(uri)->open(uri, flags, rc, msg); }
static inline db_conn *db_open(const char *uri, unsigned int flags) { return db_driver_active()->open(uri, flags, NULL, NULL); }
static inline db_conn *db_open_ex(const char *uri, unsigned int flags, db_rc *rc, const char **msg) { return db_driver_active()->open(uri, flags, rc, msg); }
static inline void db_close(db_conn *c) { if(c) c->drv->close(c); }
static inline void db_close_deferred(db_conn *c) { if(c) c->drv->close_deferred(c); }
static inline void db_interrupt(db_conn *c) { c->drv->interrupt(c); }
static inline bool db_is_readonly(db_conn *c) { return c->drv->is_readonly(c); }
static inline db_rc db_set_busy_handler(db_conn *c, int (*cb)(void *, int), void *arg) { return c->drv->set_busy_handler(c, cb, arg); }

static inline db_stmt *db_prepare(db_conn *c, const char *sql, bool persistent) { return c->drv->prepare(c, sql, persistent); }
static inline db_rc db_step(db_stmt *s) { return s->drv->step(s); }
static inline db_rc db_reset(db_stmt *s) { return s->drv->reset(s); }
static inline void db_finalize(db_stmt *s) { if(s) s->drv->finalize(s); }
static inline db_rc db_exec(db_conn *c, const char *sql) { return c->drv->exec(c, sql); }
static inline const char *db_sql_text(db_stmt *s) { return s->drv->sql_text(s); }

static inline int db_param_index(db_stmt *s, const char *n) { return s->drv->param_index(s, n); }
static inline db_rc db_bind_int(db_stmt *s, int i, int v) { return s->drv->bind_int(s, i, v); }
static inline db_rc db_bind_int64(db_stmt *s, int i, int64_t v) { return s->drv->bind_int64(s, i, v); }
static inline db_rc db_bind_double(db_stmt *s, int i, double v) { return s->drv->bind_double(s, i, v); }
static inline db_rc db_bind_text(db_stmt *s, int i, const char *v) { return s->drv->bind_text(s, i, v); }
static inline db_rc db_bind_text_ref(db_stmt *s, int i, const char *v) { return s->drv->bind_text_ref(s, i, v); }
static inline db_rc db_bind_blob(db_stmt *s, int i, const void *d, size_t n) { return s->drv->bind_blob(s, i, d, n); }
static inline db_rc db_bind_null(db_stmt *s, int i) { return s->drv->bind_null(s, i); }
static inline db_rc db_bind_array(db_stmt *s, int i, db_type t, const void *v, size_t n) { return s->drv->bind_array(s, i, t, v, n); }

static inline int db_column_count(db_stmt *s) { return s->drv->column_count(s); }
static inline const char *db_column_name(db_stmt *s, int i) { return s->drv->column_name(s, i); }
static inline db_type db_column_type(db_stmt *s, int i) { return s->drv->column_type(s, i); }
static inline int db_column_int(db_stmt *s, int i) { return s->drv->column_int(s, i); }
static inline int64_t db_column_int64(db_stmt *s, int i) { return s->drv->column_int64(s, i); }
static inline double db_column_double(db_stmt *s, int i) { return s->drv->column_double(s, i); }
static inline const char *db_column_text(db_stmt *s, int i) { return s->drv->column_text(s, i); }
static inline const void *db_column_blob(db_stmt *s, int i) { return s->drv->column_blob(s, i); }
static inline int db_column_bytes(db_stmt *s, int i) { return s->drv->column_bytes(s, i); }

static inline int64_t db_changes(db_conn *c) { return c->drv->changes(c); }
static inline int64_t db_last_insert_id(db_conn *c) { return c->drv->last_insert_id(c); }
static inline int db_errcode(db_conn *c) { return c->drv->errcode(c); }
static inline int db_extended_errcode(db_conn *c) { return c->drv->extended_errcode(c); }
static inline const char *db_errmsg(db_conn *c) { return c->drv->errmsg(c); }
static inline const char *db_stmt_errmsg(db_stmt *s) { return s->drv->stmt_errmsg(s); }
static inline const char *db_stmt_errstr(db_stmt *s) { return s->drv->stmt_errstr(s); }
static inline const char *db_errstr(db_conn *c, int code) { return c->drv->errstr(code); }
static inline const char *db_errname(db_conn *c, int code) { return c->drv->errname(code); }
static inline char *db_vmprintf(db_conn *c, const char *fmt, va_list args) { return c->drv->vmprintf(fmt, args); }
static inline void db_free(db_conn *c, void *ptr) { c->drv->mem_free(ptr); }

// Classification of the most recent error of a connection
static inline db_rc db_last_rc(db_conn *c) { return c->drv->classify_error(c->drv->errcode(c)); }

// Description of the most recent error of a connection, for log messages
#define DB_LAST_ERR(db) db_errstr((db), db_errcode(db))

static inline db_rc db_begin(db_conn *c, db_txmode m) { return c->drv->begin(c, m); }
static inline db_rc db_commit(db_conn *c) { return c->drv->commit(c); }
static inline db_rc db_rollback(db_conn *c) { return c->drv->rollback(c); }
static inline db_rc db_savepoint(db_conn *c, const char *n) { return c->drv->savepoint(c, n); }
static inline db_rc db_release_savepoint(db_conn *c, const char *n) { return c->drv->release_savepoint(c, n); }

static inline void *db_serialize(db_conn *c, const char *schema, int64_t *size) { return c->drv->serialize ? c->drv->serialize(c, schema, size) : NULL; }
static inline db_rc db_deserialize(db_conn *c, const char *schema, const void *buf, int64_t size, bool readonly) { return c->drv->deserialize ? c->drv->deserialize(c, schema, buf, size, readonly) : DB_ERROR; }
// Release a buffer handed out by db_serialize() after its connection is gone
static inline void db_free_buffer(void *ptr) { db_driver_active()->mem_free(ptr); }
static inline db_rc db_reset_database(db_conn *c) { return c->drv->reset_database ? c->drv->reset_database(c) : DB_ERROR; }
static inline bool db_can_attach(db_conn *c) { return c->drv->attach != NULL && c->drv->detach != NULL; }
static inline db_rc db_attach(db_conn *c, const char *path, const char *alias) { return c->drv->attach ? c->drv->attach(c, path, alias) : DB_ERROR; }
static inline db_rc db_detach(db_conn *c, const char *alias) { return c->drv->detach ? c->drv->detach(c, alias) : DB_ERROR; }
static inline db_rc db_copy_table(db_conn *c, const char *src, const char *dst, const char *t, const char *where) { return c->drv->copy_table ? c->drv->copy_table(c, src, dst, t, where) : DB_ERROR; }

static inline bool db_table_exists(db_conn *c, const char *t) { return c->drv->table_exists(c, t); }
static inline bool db_column_exists(db_conn *c, const char *t, const char *col) { return c->drv->column_exists(c, t, col); }
static inline int db_get_schema_version(db_conn *c) { return c->drv->get_schema_version(c); }
static inline db_rc db_set_schema_version(db_conn *c, int v) { return c->drv->set_schema_version(c, v); }
static inline int64_t db_row_count(db_conn *c, const char *t) { return c->drv->row_count(c, t); }
static inline db_rc db_vacuum(db_conn *c) { return c->drv->vacuum(c); }
static inline db_rc db_optimize(db_conn *c) { return c->drv->optimize(c); }
static inline db_rc db_integrity_check(db_conn *c, const char **msg) { return c->drv->integrity_check(c, msg); }

#endif // DB_DRIVER_H
