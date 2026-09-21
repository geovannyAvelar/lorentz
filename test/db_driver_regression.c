/* Lorentz: A black hole for Internet advertisements
*  (c) 2026 Pi-hole, LLC (https://pi-hole.net)
*  Network-wide ad blocking via your own hardware.
*
*  Lorentz Engine
*  Regression harness for the database driver layer (src/database/db-driver.*,
*  src/database/db-sqlite.c)
*
*  This file is copyright under the latest version of the EUPL.
*  Please see LICENSE file for your rights under this license. */

// Standalone harness: it links the driver and the SQLite object library only,
// no Lorentz configuration, logging or shared memory. db-sqlite.c pulls in lorentz.h,
// which redefines a few libc calls to Lorentz* wrappers, and log.h, so the wrappers
// and the logging entry point it needs are provided as thin stubs at the end of
// this file. The harness itself only includes the driver header and therefore
// sees the plain libc names.

#define _GNU_SOURCE
#include "database/db-driver.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

static int failures = 0;
static int checks = 0;

#define CHECK(cond) do { \
	checks++; \
	if(!(cond)) { \
		failures++; \
		fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
	} \
} while(0)

static char tmpdir[256];

static const char *path_of(const char *name)
{
	static char buf[8][512];
	static unsigned int n = 0;
	char *p = buf[n++ % 8];
	snprintf(p, 512, "%s/%s", tmpdir, name);
	return p;
}

static db_conn *open_mem(void)
{
	return db_open(":memory:", DB_OPEN_READWRITE | DB_OPEN_CREATE);
}

static db_conn *open_file(const char *name)
{
	return db_open(path_of(name), DB_OPEN_READWRITE | DB_OPEN_CREATE);
}

static int64_t scalar(db_conn *db, const char *sql)
{
	db_stmt *s = db_prepare(db, sql, false);
	if(s == NULL)
		return -1000000;
	int64_t v = -1000001;
	if(db_step(s) == DB_ROW)
		v = db_column_int64(s, 0);
	db_finalize(s);
	return v;
}

#include "../test/db_schema_checks.h"

/* ---- registry and driver lifecycle ---- */

static void test_registry(void)
{
	CHECK(db_driver_get("sqlite") == &db_driver_sqlite);
	CHECK(db_driver_get("nosuchdriver") == NULL);
	CHECK(db_driver_get(NULL) == NULL);
	CHECK(db_driver_select("sqlite"));
	CHECK(!db_driver_select("nosuchdriver"));
	CHECK(db_driver_active() == &db_driver_sqlite);
	CHECK(strcmp(db_driver_sqlite.name, "sqlite") == 0);
	CHECK(db_driver_sqlite.dialect != NULL);

	const char *v = db_driver_sqlite.version();
	CHECK(v != NULL && v[0] >= '3');
	CHECK(db_driver_sqlite.init() == 0);
}

/* ---- connection ---- */

static void test_open(void)
{
	db_rc rc = DB_OK;
	const char *msg = NULL;

	// No path and no memory flag is refused with an explanation
	CHECK(db_open_ex(NULL, DB_OPEN_READWRITE, &rc, &msg) == NULL);
	CHECK(rc == DB_ERROR && msg != NULL);

	// A NULL path with the memory flag opens a private in-memory database
	db_conn *m = db_open_ex(NULL, DB_OPEN_READWRITE | DB_OPEN_MEMORY, NULL, NULL);
	CHECK(m != NULL);
	CHECK(db_exec(m, "CREATE TABLE t(a)") == DB_OK);
	db_close(m);

	// Missing file: read-only and read-write without CREATE both fail
	rc = DB_OK;
	msg = NULL;
	CHECK(db_open_ex(path_of("missing.db"), DB_OPEN_READONLY, &rc, &msg) == NULL);
	CHECK(rc != DB_OK && msg != NULL);
	CHECK(db_open_ex(path_of("missing.db"), DB_OPEN_READWRITE, &rc, &msg) == NULL);

	// CREATE makes the file
	db_conn *f = db_open(path_of("created.db"), DB_OPEN_READWRITE | DB_OPEN_CREATE);
	CHECK(f != NULL);
	CHECK(!db_is_readonly(f));
	CHECK(db_exec(f, "CREATE TABLE t(a)") == DB_OK);
	db_close(f);
	CHECK(access(path_of("created.db"), F_OK) == 0);

	// Read-only connections read but never write
	db_conn *ro = db_open(path_of("created.db"), DB_OPEN_READONLY);
	CHECK(ro != NULL);
	CHECK(db_is_readonly(ro));
	CHECK(db_exec(ro, "INSERT INTO t VALUES (1)") == DB_READONLY);
	CHECK(scalar(ro, "SELECT count(*) FROM t") == 0);
	db_close(ro);

	// URI names are honoured with the URI flag
	char uri[600];
	snprintf(uri, sizeof(uri), "file:%s?mode=ro", path_of("created.db"));
	db_conn *u = db_open(uri, DB_OPEN_READWRITE | DB_OPEN_URI);
	CHECK(u != NULL && db_is_readonly(u));
	db_close(u);

	// NOMUTEX connections work like any other
	db_conn *nm = db_open(path_of("created.db"), DB_OPEN_READWRITE | DB_OPEN_NOMUTEX);
	CHECK(nm != NULL);
	db_close(nm);

	// Closing NULL is a no-op
	db_close(NULL);
	db_close_deferred(NULL);
}

static int busy_calls = 0;
static int busy_cb(void *arg, int count)
{
	(void)arg;
	busy_calls++;
	return count < 2 ? 1 : 0;
}

static void test_busy(void)
{
	db_conn *a = open_file("busy.db");
	db_conn *b = open_file("busy.db");
	CHECK(a != NULL && b != NULL);
	CHECK(db_exec(a, "CREATE TABLE t(a)") == DB_OK);

	// a holds the write lock, b cannot write and reports DB_BUSY
	CHECK(db_begin(a, DB_TX_EXCLUSIVE) == DB_OK);
	CHECK(db_set_busy_handler(b, NULL, NULL) == DB_OK);
	CHECK(db_exec(b, "INSERT INTO t VALUES (1)") == DB_BUSY);
	CHECK(db_last_rc(b) == DB_BUSY);

	// A busy handler is called and can give up
	CHECK(db_set_busy_handler(b, busy_cb, NULL) == DB_OK);
	busy_calls = 0;
	CHECK(db_exec(b, "INSERT INTO t VALUES (1)") == DB_BUSY);
	CHECK(busy_calls == 3);

	CHECK(db_rollback(a) == DB_OK);
	CHECK(db_exec(b, "INSERT INTO t VALUES (1)") == DB_OK);

	db_interrupt(a);
	db_close(a);
	db_close(b);
}

/* ---- statements, binding, columns ---- */

static void test_statements(void)
{
	db_conn *db = open_mem();
	CHECK(db != NULL);
	CHECK(db_exec(db, "CREATE TABLE t(i INTEGER, r REAL, s TEXT, b BLOB, n)") == DB_OK);

	db_stmt *ins = db_prepare(db, "INSERT INTO t VALUES (:i, :r, :s, :b, :n)", true);
	CHECK(ins != NULL);
	CHECK(strcmp(db_sql_text(ins), "INSERT INTO t VALUES (:i, :r, :s, :b, :n)") == 0);
	CHECK(db_param_index(ins, ":i") == 1 && db_param_index(ins, ":n") == 5);
	CHECK(db_param_index(ins, ":nosuch") == 0);

	char text[16];
	strcpy(text, "hello");
	const unsigned char blob[4] = { 0x00, 0xde, 0xad, 0xbe };
	CHECK(db_bind_int64(ins, 1, 5000000000LL) == DB_OK);
	CHECK(db_bind_double(ins, 2, 2.5) == DB_OK);
	CHECK(db_bind_text(ins, 3, text) == DB_OK);
	CHECK(db_bind_blob(ins, 4, blob, sizeof(blob)) == DB_OK);
	CHECK(db_bind_null(ins, 5) == DB_OK);
	strcpy(text, "XXXXX"); // bind_text copies, this must not show up in the row
	CHECK(db_step(ins) == DB_DONE);
	CHECK(db_changes(db) == 1);
	CHECK(db_last_insert_id(db) == 1);

	// int and zero-copy text binds
	CHECK(db_reset(ins) == DB_OK);
	CHECK(db_bind_int(ins, 1, 7) == DB_OK);
	CHECK(db_bind_double(ins, 2, -1.0) == DB_OK);
	CHECK(db_bind_text_ref(ins, 3, "static text") == DB_OK);
	CHECK(db_bind_null(ins, 4) == DB_OK);
	CHECK(db_bind_null(ins, 5) == DB_OK);
	CHECK(db_step(ins) == DB_DONE);
	CHECK(db_last_insert_id(db) == 2);

	// Out-of-range index is an error, not a crash
	CHECK(db_bind_int(ins, 99, 1) != DB_OK);
	db_finalize(ins);

	db_stmt *sel = db_prepare(db, "SELECT i, r, s, b, n FROM t ORDER BY rowid", false);
	CHECK(sel != NULL);
	CHECK(db_column_count(sel) == 5);
	CHECK(strcmp(db_column_name(sel, 0), "i") == 0 && strcmp(db_column_name(sel, 4), "n") == 0);

	CHECK(db_step(sel) == DB_ROW);
	CHECK(db_column_type(sel, 0) == DB_TYPE_INT64);
	CHECK(db_column_type(sel, 1) == DB_TYPE_DOUBLE);
	CHECK(db_column_type(sel, 2) == DB_TYPE_TEXT);
	CHECK(db_column_type(sel, 3) == DB_TYPE_BLOB);
	CHECK(db_column_type(sel, 4) == DB_TYPE_NULL);
	CHECK(db_column_int64(sel, 0) == 5000000000LL);
	CHECK(db_column_double(sel, 1) == 2.5);
	CHECK(strcmp(db_column_text(sel, 2), "hello") == 0);
	CHECK(db_column_bytes(sel, 2) == 5);
	CHECK(db_column_bytes(sel, 3) == 4 && memcmp(db_column_blob(sel, 3), blob, 4) == 0);
	CHECK(db_column_text(sel, 4) == NULL);

	CHECK(db_step(sel) == DB_ROW);
	CHECK(db_column_int(sel, 0) == 7);
	CHECK(strcmp(db_column_text(sel, 2), "static text") == 0);
	CHECK(db_column_type(sel, 3) == DB_TYPE_NULL);
	CHECK(db_column_bytes(sel, 3) == 0);

	CHECK(db_step(sel) == DB_DONE);
	CHECK(db_reset(sel) == DB_OK);
	CHECK(db_step(sel) == DB_ROW); // starts over
	db_finalize(sel);
	db_finalize(NULL);

	db_close(db);
}

static void test_errors(void)
{
	db_conn *db = open_mem();
	CHECK(db_exec(db, "CREATE TABLE t(a INTEGER PRIMARY KEY, b TEXT NOT NULL)") == DB_OK);
	CHECK(db_exec(db, "INSERT INTO t VALUES (1, 'x')") == DB_OK);

	// Constraint violation
	CHECK(db_exec(db, "INSERT INTO t VALUES (1, 'y')") == DB_CONSTRAINT);
	CHECK(db_last_rc(db) == DB_CONSTRAINT);
	CHECK(db_errcode(db) != 0);
	CHECK(db_extended_errcode(db) != 0);
	CHECK(strlen(db_errmsg(db)) > 0);
	CHECK(strlen(DB_LAST_ERR(db)) > 0);
	CHECK(strncmp(db_errname(db, db_extended_errcode(db)), "SQLITE_CONSTRAINT", 17) == 0);

	// NOT NULL violation through a prepared statement
	db_stmt *s = db_prepare(db, "INSERT INTO t VALUES (?, ?)", false);
	CHECK(s != NULL);
	CHECK(db_bind_int(s, 1, 2) == DB_OK);
	CHECK(db_bind_null(s, 2) == DB_OK);
	CHECK(db_step(s) == DB_CONSTRAINT);
	CHECK(strlen(db_stmt_errmsg(s)) > 0);
	CHECK(strlen(db_stmt_errstr(s)) > 0);
	db_finalize(s);

	// Syntax error: prepare hands back NULL and the connection has the reason
	CHECK(db_prepare(db, "SELEKT nonsense", false) == NULL);
	CHECK(db_last_rc(db) == DB_ERROR);
	CHECK(strstr(db_errmsg(db), "syntax") != NULL);
	CHECK(db_exec(db, "NOT SQL") == DB_ERROR);

	// classify_error maps primary and extended codes
	const db_driver *drv = &db_driver_sqlite;
	CHECK(drv->classify_error(0) == DB_OK);
	CHECK(drv->classify_error(5) == DB_BUSY);       // SQLITE_BUSY
	CHECK(drv->classify_error(6) == DB_BUSY);       // SQLITE_LOCKED
	CHECK(drv->classify_error(8) == DB_READONLY);   // SQLITE_READONLY
	CHECK(drv->classify_error(11) == DB_CORRUPT);   // SQLITE_CORRUPT
	CHECK(drv->classify_error(19) == DB_CONSTRAINT);
	CHECK(drv->classify_error(100) == DB_ROW);
	CHECK(drv->classify_error(101) == DB_DONE);
	CHECK(drv->classify_error(1) == DB_ERROR);
	CHECK(drv->classify_error(2067) == DB_CONSTRAINT); // SQLITE_CONSTRAINT_UNIQUE
	CHECK(drv->classify_error(8 | (5 << 8)) == DB_READONLY);
	CHECK(strlen(db_errstr(db, 5)) > 0);

	db_close(db);
}

/* ---- arrays (carray) ---- */

static void test_arrays(void)
{
	db_conn *db = open_mem();
	CHECK(db_exec(db, "CREATE TABLE t(i INTEGER, d REAL, s TEXT)") == DB_OK);
	CHECK(db_exec(db, "INSERT INTO t VALUES (1, 1.5, 'a'), (2, 2.5, 'b'), (3, 3.5, 'c'), (4, 4.5, 'd')") == DB_OK);

	const int32_t ints[] = { 1, 3, 9 };
	db_stmt *s = db_prepare(db, "SELECT count(*) FROM t WHERE i IN carray(?1)", false);
	CHECK(s != NULL);
	CHECK(db_bind_array(s, 1, DB_TYPE_INT, ints, 3) == DB_OK);
	CHECK(db_step(s) == DB_ROW && db_column_int(s, 0) == 2);
	CHECK(db_reset(s) == DB_OK);

	// Rebind with a different array
	const int32_t ints2[] = { 4 };
	CHECK(db_bind_array(s, 1, DB_TYPE_INT, ints2, 1) == DB_OK);
	CHECK(db_step(s) == DB_ROW && db_column_int(s, 0) == 1);
	db_finalize(s);

	const int64_t big[] = { 2, 4 };
	s = db_prepare(db, "SELECT count(*) FROM t WHERE i IN carray(?1)", false);
	CHECK(db_bind_array(s, 1, DB_TYPE_INT64, big, 2) == DB_OK);
	CHECK(db_step(s) == DB_ROW && db_column_int(s, 0) == 2);
	db_finalize(s);

	const double dbl[] = { 1.5, 4.5 };
	s = db_prepare(db, "SELECT count(*) FROM t WHERE d IN carray(?1)", false);
	CHECK(db_bind_array(s, 1, DB_TYPE_DOUBLE, dbl, 2) == DB_OK);
	CHECK(db_step(s) == DB_ROW && db_column_int(s, 0) == 2);
	db_finalize(s);

	const char *txt[] = { "b", "c", "zz" };
	s = db_prepare(db, "SELECT count(*) FROM t WHERE s IN carray(?1)", false);
	CHECK(db_bind_array(s, 1, DB_TYPE_TEXT, txt, 3) == DB_OK);
	CHECK(db_step(s) == DB_ROW && db_column_int(s, 0) == 2);

	// Types without an array representation are refused
	CHECK(db_bind_array(s, 1, DB_TYPE_BLOB, txt, 1) == DB_ERROR);
	CHECK(db_bind_array(s, 1, DB_TYPE_NULL, txt, 1) == DB_ERROR);
	db_finalize(s);

	db_close(db);
}

/* ---- transactions ---- */

static void test_transactions(void)
{
	db_conn *db = open_mem();
	CHECK(db_exec(db, "CREATE TABLE t(a)") == DB_OK);

	const db_txmode modes[] = { DB_TX_DEFERRED, DB_TX_IMMEDIATE, DB_TX_EXCLUSIVE };
	for(unsigned int i = 0; i < 3; i++)
	{
		CHECK(db_begin(db, modes[i]) == DB_OK);
		CHECK(db_exec(db, "INSERT INTO t VALUES (1)") == DB_OK);
		CHECK(db_rollback(db) == DB_OK);
		CHECK(scalar(db, "SELECT count(*) FROM t") == 0);
	}

	CHECK(db_begin(db, DB_TX_DEFERRED) == DB_OK);
	CHECK(db_exec(db, "INSERT INTO t VALUES (1)") == DB_OK);
	CHECK(db_commit(db) == DB_OK);
	CHECK(scalar(db, "SELECT count(*) FROM t") == 1);

	// Nested work with savepoints
	CHECK(db_begin(db, DB_TX_DEFERRED) == DB_OK);
	CHECK(db_savepoint(db, "sp1") == DB_OK);
	CHECK(db_exec(db, "INSERT INTO t VALUES (2)") == DB_OK);
	CHECK(db_savepoint(db, "we\"ird") == DB_OK); // quoted, not injected
	CHECK(db_exec(db, "INSERT INTO t VALUES (3)") == DB_OK);
	CHECK(db_release_savepoint(db, "we\"ird") == DB_OK);
	CHECK(db_release_savepoint(db, "sp1") == DB_OK);
	CHECK(db_commit(db) == DB_OK);
	CHECK(scalar(db, "SELECT count(*) FROM t") == 3);

	// Commit and rollback without a transaction are errors
	CHECK(db_commit(db) != DB_OK);
	CHECK(db_rollback(db) != DB_OK);

	db_close(db);
}

/* ---- multiple databases ---- */

static void test_attach(void)
{
	db_conn *other = open_file("other.db");
	CHECK(db_exec(other, "CREATE TABLE src(id INTEGER PRIMARY KEY, v TEXT)") == DB_OK);
	CHECK(db_exec(other, "INSERT INTO src VALUES (1,'a'),(2,'b'),(3,'c')") == DB_OK);
	db_close(other);

	db_conn *db = open_mem();
	CHECK(db_can_attach(db));
	CHECK(db_attach(db, path_of("other.db"), "o") == DB_OK);
	CHECK(scalar(db, "SELECT count(*) FROM o.src") == 3);

	// copy_table copies everything or the rows of a WHERE clause
	CHECK(db_exec(db, "CREATE TABLE src(id INTEGER PRIMARY KEY, v TEXT)") == DB_OK);
	CHECK(db_copy_table(db, "o", "main", "src", NULL) == DB_OK);
	CHECK(scalar(db, "SELECT count(*) FROM main.src") == 3);
	CHECK(db_exec(db, "DELETE FROM main.src") == DB_OK);
	CHECK(db_copy_table(db, "o", "main", "src", "id >= 2") == DB_OK);
	CHECK(scalar(db, "SELECT count(*) FROM main.src") == 2);
	CHECK(db_copy_table(db, "o", "main", "nosuchtable", NULL) != DB_OK);

	// Table and column names are quoted
	CHECK(db_copy_table(db, "o", "main", "src\"; DROP TABLE src; --", NULL) != DB_OK);
	CHECK(scalar(db, "SELECT count(*) FROM main.src") == 2);

	CHECK(db_detach(db, "o") == DB_OK);
	CHECK(db_exec(db, "SELECT * FROM o.src") != DB_OK);
	CHECK(db_detach(db, "o") != DB_OK);
	CHECK(db_attach(db, path_of("missing-dir/x.db"), "bad") != DB_OK);

	// Attach parameters are bound, a quote in the path does not break out
	CHECK(db_attach(db, "/nonexistent'; DROP TABLE src; --", "evil") != DB_OK);
	CHECK(scalar(db, "SELECT count(*) FROM main.src") == 2);

	db_close(db);
}

/* ---- schema helpers ---- */

static void test_schema(void)
{
	db_conn *db = open_mem();
	CHECK(db_exec(db, "CREATE TABLE t(a INTEGER PRIMARY KEY AUTOINCREMENT, b TEXT)") == DB_OK);
	CHECK(db_exec(db, "CREATE VIEW v AS SELECT * FROM t") == DB_OK);

	CHECK(db_table_exists(db, "t"));
	CHECK(db_table_exists(db, "v"));
	CHECK(!db_table_exists(db, "nosuch"));
	CHECK(db_column_exists(db, "t", "b"));
	CHECK(!db_column_exists(db, "t", "zzz"));
	CHECK(!db_column_exists(db, "nosuch", "b"));

	CHECK(db_get_schema_version(db) == 0);
	CHECK(db_set_schema_version(db, 42) == DB_OK);
	CHECK(db_get_schema_version(db) == 42);

	CHECK(db_row_count(db, "t") == 0);
	CHECK(db_exec(db, "INSERT INTO t(b) VALUES ('a'),('b')") == DB_OK);
	CHECK(db_row_count(db, "t") == 2);
	CHECK(db_row_count(db, "nosuch") == -1);

	CHECK(db_optimize(db) == DB_OK);
	CHECK(db_vacuum(db) == DB_OK);

	const char *msg = "unset";
	CHECK(db_integrity_check(db, &msg) == DB_OK);
	CHECK(msg == NULL);

	// reset_database erases everything but keeps the connection usable
	CHECK(db_reset_database(db) == DB_OK);
	CHECK(!db_table_exists(db, "t"));
	CHECK(db_exec(db, "CREATE TABLE again(a)") == DB_OK);

	db_close(db);
}

static void test_corrupt_file(void)
{
	// Damage a database file below the header: integrity_check must notice
	db_conn *db = open_file("corrupt.db");
	CHECK(db_exec(db, "CREATE TABLE t(a TEXT)") == DB_OK);
	CHECK(db_begin(db, DB_TX_DEFERRED) == DB_OK);
	for(int i = 0; i < 200; i++)
		CHECK(db_exec(db, "INSERT INTO t VALUES (hex(randomblob(64)))") == DB_OK);
	CHECK(db_commit(db) == DB_OK);
	CHECK(db_exec(db, "CREATE INDEX ti ON t(a)") == DB_OK);
	db_close(db);

	// Keep the first page (header and schema) and wipe every b-tree page after it
	struct stat st;
	CHECK(stat(path_of("corrupt.db"), &st) == 0 && st.st_size > 3 * 4096);
	FILE *fp = fopen(path_of("corrupt.db"), "r+b");
	CHECK(fp != NULL);
	if(fp != NULL)
	{
		fseek(fp, 4096, SEEK_SET);
		for(off_t i = 4096; i < st.st_size; i++)
			fputc(0xFF, fp);
		fclose(fp);
	}

	db = open_file("corrupt.db");
	if(db != NULL)
	{
		const char *msg = NULL;
		const db_rc rc = db_integrity_check(db, &msg);
		CHECK(rc != DB_OK || msg != NULL);
		if(msg != NULL)
			CHECK(strlen(msg) > 0);
		db_close(db);
	}
	else
		CHECK(true); // damaged so badly that it does not even open
}

/* ---- serialize and deserialize ---- */

static void test_serialize(void)
{
	db_conn *db = open_mem();
	CHECK(db_exec(db, "CREATE TABLE t(a)") == DB_OK);
	CHECK(db_exec(db, "INSERT INTO t VALUES (1),(2),(3)") == DB_OK);

	int64_t size = 0;
	void *buf = db_serialize(db, NULL, &size);
	CHECK(buf != NULL && size > 100);
	CHECK(memcmp(buf, "SQLite format 3", 15) == 0);
	db_close(db);

	// Copying deserialize: the buffer is not needed afterwards and the
	// database is writable
	db_conn *copy = open_mem();
	CHECK(db_deserialize(copy, NULL, buf, size, false) == DB_OK);
	CHECK(scalar(copy, "SELECT count(*) FROM t") == 3);
	CHECK(db_exec(copy, "INSERT INTO t VALUES (4)") == DB_OK);
	db_close(copy);

	// In-place read-only deserialize: reads work, writes are refused, the
	// buffer stays untouched
	unsigned char *snapshot = malloc((size_t)size);
	memcpy(snapshot, buf, (size_t)size);
	db_conn *ro = open_mem();
	CHECK(db_deserialize(ro, "main", buf, size, true) == DB_OK);
	CHECK(scalar(ro, "SELECT count(*) FROM t") == 3);
	CHECK(db_exec(ro, "INSERT INTO t VALUES (9)") != DB_OK);
	db_close(ro);
	CHECK(memcmp(snapshot, buf, (size_t)size) == 0);
	free(snapshot);

	// Garbage is rejected when it is used, not when it is loaded
	char junk[200];
	memset(junk, 0x5A, sizeof(junk));
	db_conn *bad = open_mem();
	db_rc rc = db_deserialize(bad, NULL, junk, sizeof(junk), true);
	if(rc == DB_OK)
		CHECK(db_prepare(bad, "SELECT * FROM sqlite_master", false) == NULL);
	db_close(bad);

	db_free_buffer(buf);
}

/* ---- closing ---- */

static void test_close(void)
{
	// A statement that is still alive on close is finalized by the driver
	db_conn *db = open_mem();
	CHECK(db_exec(db, "CREATE TABLE t(a)") == DB_OK);
	db_stmt *leaked = db_prepare(db, "SELECT * FROM t", false);
	CHECK(leaked != NULL);
	db_close(db); // logs, must not crash or leak the SQLite handle
	free(leaked); // only the wrapper is ours to release

	// Deferred close: the cursor keeps working until it is finalized
	db_conn *file = open_file("deferred.db");
	CHECK(db_exec(file, "CREATE TABLE t(a)") == DB_OK);
	CHECK(db_exec(file, "INSERT INTO t VALUES (1),(2)") == DB_OK);
	db_stmt *cursor = db_prepare(file, "SELECT a FROM t ORDER BY a", false);
	CHECK(cursor != NULL);
	CHECK(db_step(cursor) == DB_ROW && db_column_int(cursor, 0) == 1);
	db_close_deferred(file);
	CHECK(db_step(cursor) == DB_ROW && db_column_int(cursor, 0) == 2);
	CHECK(db_step(cursor) == DB_DONE);
	CHECK(strlen(db_stmt_errstr(cursor)) > 0); // needs no connection wrapper
	db_finalize(cursor);
}

/* ---- memory helpers and dialect ---- */

static char *vmprintf_wrapper(db_conn *db, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	char *s = db_vmprintf(db, fmt, ap);
	va_end(ap);
	return s;
}

static void test_dialect(void)
{
	db_conn *db = open_mem();
	const db_dialect *d = db_driver_sqlite.dialect;

	// Formatting uses the driver's own escapes
	char *q = db_driver_sqlite.mprintf("SELECT %Q, %d, '%q'", "it's", 42, "it's");
	CHECK(q != NULL && strcmp(q, "SELECT 'it''s', 42, 'it''s'") == 0);
	db_free(db, q);
	q = vmprintf_wrapper(db, "%s-%d", "x", 7);
	CHECK(q != NULL && strcmp(q, "x-7") == 0);
	db_free(db, q);

	// Dialect fragments are executable
	CHECK(strcmp(d->placeholder(1), "?") == 0);
	char sql[512];
	snprintf(sql, sizeof(sql), "SELECT %s", d->now_expr());
	CHECK(scalar(db, sql) > 1700000000);

	snprintf(sql, sizeof(sql), "CREATE TABLE t(id %s, name TEXT UNIQUE, n INTEGER)", d->autoincrement_pk());
	CHECK(db_exec(db, sql) == DB_OK);
	CHECK(db_exec(db, "INSERT INTO t(name, n) VALUES ('a', 1)") == DB_OK);

	// Upsert: update on conflict, or ignore
	snprintf(sql, sizeof(sql), "INSERT INTO t(name, n) VALUES ('a', 5) %s",
	         d->upsert_suffix("name", "n = excluded.n"));
	CHECK(db_exec(db, sql) == DB_OK);
	CHECK(scalar(db, "SELECT n FROM t WHERE name = 'a'") == 5);
	snprintf(sql, sizeof(sql), "INSERT INTO t(name, n) VALUES ('a', 9) %s", d->upsert_suffix("name", NULL));
	CHECK(db_exec(db, sql) == DB_OK);
	CHECK(scalar(db, "SELECT n FROM t WHERE name = 'a'") == 5);
	CHECK(scalar(db, "SELECT count(*) FROM t") == 1);

	// Array membership
	CHECK(db_exec(db, "INSERT INTO t(name, n) VALUES ('b', 2), ('c', 3)") == DB_OK);
	char in[128];
	CHECK(d->in_list(in, sizeof(in), "n", "?1") > 0);
	CHECK(strcmp(in, "n IN carray(?1)") == 0);
	snprintf(sql, sizeof(sql), "SELECT count(*) FROM t WHERE %s", in);
	db_stmt *s = db_prepare(db, sql, false);
	CHECK(s != NULL);
	const int32_t wanted[] = { 2, 3 };
	CHECK(db_bind_array(s, 1, DB_TYPE_INT, wanted, 2) == DB_OK);
	CHECK(db_step(s) == DB_ROW && db_column_int(s, 0) == 2);
	db_finalize(s);
	char tiny[8];
	CHECK(d->in_list(tiny, sizeof(tiny), "some_long_column", "?1") == -1); // does not fit

	CHECK(strcmp(d->glob_op(), "GLOB") == 0);
	CHECK(strcmp(d->regexp_op(), "REGEXP") == 0);
	snprintf(sql, sizeof(sql), "SELECT 'a.example' %s '*.example'", d->glob_op());
	CHECK(scalar(db, sql) == 1);
	CHECK(strcmp(d->name, "sqlite") == 0);

	db_close(db);
}


/* ---- baseline schema ---- */

static void test_baseline_schema(void)
{
	db_conn *db = open_mem();
	const char *error = NULL;
	CHECK(db_schema_baseline(db, &error));
	if(error != NULL)
		fprintf(stderr, "baseline: %s\n", error);
	check_baseline_content(db);
	check_baseline_behaviour(db);

	// A failed attempt leaves nothing behind: the second call did not change the data
	CHECK(scalar(db, "SELECT count(*) FROM domain_by_id") == 2);

	// Versions: the current one needs nothing, older ones have no migration yet
	CHECK(db_schema_migrate(db, DB_SCHEMA_VERSION, &error));
	CHECK(!db_schema_migrate(db, DB_SCHEMA_VERSION - 1, &error) && strstr(error, "no migration") != NULL);
	CHECK(!db_schema_migrate(db, DB_SCHEMA_VERSION + 1, &error) && strstr(error, "newer") != NULL);
	db_close(db);

	// The same schema in a file
	db = open_file("baseline.db");
	CHECK(db_schema_baseline(db, &error));
	CHECK(scalar(db, "SELECT value FROM lorentz WHERE id = 0") == DB_SCHEMA_VERSION);
	db_close(db);
}

/* ---- main and stubs ---- */

int main(void)
{
	snprintf(tmpdir, sizeof(tmpdir), "/tmp/db_driver_regression.XXXXXX");
	if(mkdtemp(tmpdir) == NULL)
	{
		perror("mkdtemp");
		return 2;
	}

	test_registry();
	test_open();
	test_busy();
	test_statements();
	test_errors();
	test_arrays();
	test_transactions();
	test_attach();
	test_schema();
	test_corrupt_file();
	test_serialize();
	test_close();
	test_dialect();
	test_baseline_schema();

	char cmd[300];
	snprintf(cmd, sizeof(cmd), "rm -rf '%s'", tmpdir);
	if(system(cmd) != 0)
		fprintf(stderr, "could not remove %s\n", tmpdir);

	printf("%d checks, %d failures\n", checks, failures);
	printf("DB_DRIVER_REGRESSION=%s\n", failures == 0 ? "PASS" : "FAIL");
	return failures == 0 ? 0 : 1;
}

// db-sqlite.c includes lorentz.h (libc wrappers) and log.h (logging entry point).
// These stubs stand in for src/syscalls and src/log.c so that the harness does
// not need the rest of Lorentz. The last two are hooks of the SQLite shell, which
// is part of the SQLite object library but never started here.
void _Lorentz_log(const int priority, const int flag, const char *format, ...)
{
	(void)priority;
	(void)flag;
	va_list ap;
	va_start(ap, format);
	fputs("      [driver log] ", stderr);
	vfprintf(stderr, format, ap);
	fputc('\n', stderr);
	va_end(ap);
}

void *Lorentzcalloc(size_t n, size_t size, const char *file, const char *func, const int line)
{
	(void)file; (void)func; (void)line;
	return calloc(n, size);
}

bool Lorentzfree(void *ptr, const char *file, const char *func, const int line)
{
	(void)file; (void)func; (void)line;
	free(ptr);
	return true;
}

void *Lorentzmemcpy(void *dest, const void *src, const size_t n, const char *file, const char *func, const int line)
{
	(void)file; (void)func; (void)line;
	return memcpy(dest, src, n);
}

int Lorentzstrcmp(const char *s1, const char *s2, const char *file, const char *func, const int line)
{
	(void)file; (void)func; (void)line;
	return strcmp(s1, s2);
}

int Lorentzsnprintf(const char *file, const char *func, const int line, char *__restrict__ buffer, const size_t maxlen, const char *format, ...)
{
	(void)file; (void)func; (void)line;
	va_list ap;
	va_start(ap, format);
	const int ret = vsnprintf(buffer, maxlen, format, ap);
	va_end(ap);
	return ret;
}

void lorentz_sqlite3_initalize(void)
{
}

void print_Lorentz_version(void)
{
}
