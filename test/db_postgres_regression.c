/* Lorentz: A black hole for Internet advertisements
*  (c) 2026 Pi-hole, LLC (https://pi-hole.net)
*  Network-wide ad blocking via your own hardware.
*
*  Lorentz Engine
*  Regression harness for the PostgreSQL driver (src/database/db-postgres.c)
*
*  This file is copyright under the latest version of the EUPL.
*  Please see LICENSE file for your rights under this license. */

// Standalone harness like db_driver_regression, but it talks to a real
// PostgreSQL server. The connection string comes from POSTGRES_URL, for
// example "postgresql://postgres:secret@127.0.0.1:5432/lorentz_test". The
// database must be a throw-away one: the harness creates and drops schemas in
// it and, when the database name contains "test", also resets the public
// schema. Without POSTGRES_URL it reports SKIP and succeeds. The Node.js
// integration test (test/integration/postgres.driver.test.mjs) starts a
// PostgreSQL container and runs this binary against it.

#define _GNU_SOURCE
#include "database/db-driver.h"

#include <pthread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int failures = 0;
static int checks = 0;

#define CHECK(cond) do { \
	checks++; \
	if(!(cond)) { \
		failures++; \
		fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
	} \
} while(0)

static const char *url;
static const db_driver *pg;

// Every connection works in the schema lz_test
static db_conn *open_pg(void)
{
	db_rc rc = DB_OK;
	const char *msg = NULL;
	db_conn *db = db_open_ex(url, DB_OPEN_READWRITE, &rc, &msg);
	if(db == NULL)
	{
		fprintf(stderr, "cannot connect: %s\n", msg);
		return NULL;
	}
	if(db_exec(db, "SET search_path TO lz_test") != DB_OK)
		fprintf(stderr, "cannot set the search path: %s\n", db_errmsg(db));
	return db;
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

static char *text_scalar(db_conn *db, const char *sql)
{
	db_stmt *s = db_prepare(db, sql, false);
	if(s == NULL)
		return NULL;
	char *v = NULL;
	if(db_step(s) == DB_ROW && db_column_text(s, 0) != NULL)
		v = strdup(db_column_text(s, 0));
	db_finalize(s);
	return v;
}


#include "../test/db_schema_checks.h"

/* ---- registry and connection ---- */

static void test_registry_and_open(void)
{
	CHECK(db_driver_get("postgres") == &db_driver_postgres);
	CHECK(db_driver_select("postgres"));
	CHECK(db_driver_active() == &db_driver_postgres);
	CHECK(strcmp(pg->name, "postgres") == 0);
	CHECK(strncmp(pg->version(), "libpq ", 6) == 0);
	CHECK(pg->init() == 0);

	// In-memory databases do not exist
	db_rc rc = DB_OK;
	const char *msg = NULL;
	CHECK(db_open_ex(url, DB_OPEN_READWRITE | DB_OPEN_MEMORY, &rc, &msg) == NULL);
	CHECK(rc == DB_ERROR && msg != NULL);

	// A server that is not there
	CHECK(db_open_ex("postgresql://nobody@127.0.0.1:1/none?connect_timeout=2", DB_OPEN_READWRITE, &rc, &msg) == NULL);
	CHECK(rc == DB_ERROR && msg != NULL && strlen(msg) > 0);

	db_conn *db = open_pg();
	CHECK(db != NULL);
	CHECK(!db_is_readonly(db));
	CHECK(scalar(db, "SELECT 1") == 1);
	db_close(db);

	// A read-only connection reads but never writes (SQLSTATE 25006)
	db_conn *ro = db_open(url, DB_OPEN_READONLY);
	CHECK(ro != NULL && db_is_readonly(ro));
	CHECK(scalar(ro, "SELECT 2") == 2);
	CHECK(db_exec(ro, "CREATE TABLE lz_test.nope (a int)") == DB_READONLY);
	CHECK(strcmp(db_errname(ro, db_errcode(ro)), "SQLSTATE_25006") == 0);
	db_close(ro);

	db_close(NULL);
	db_close_deferred(NULL);
}

/* ---- placeholders, binding, columns ---- */

static void test_statements(void)
{
	db_conn *db = open_pg();
	CHECK(db_exec(db, "CREATE TABLE t (id BIGSERIAL PRIMARY KEY, i BIGINT, r DOUBLE PRECISION, s TEXT, b BYTEA, n TEXT, f BOOLEAN, d NUMERIC(10,2))") == DB_OK);

	// SQLite style placeholders: ?, ?N, :name and @name become $N
	db_stmt *ins = db_prepare(db, "INSERT INTO t (i, r, s, b, n) VALUES (:i, :r, :s, @b, ?5)", true);
	CHECK(ins != NULL);
	CHECK(strcmp(db_sql_text(ins), "INSERT INTO t (i, r, s, b, n) VALUES (:i, :r, :s, @b, ?5)") == 0);
	CHECK(db_param_index(ins, ":i") == 1 && db_param_index(ins, ":r") == 2 && db_param_index(ins, ":s") == 3);
	CHECK(db_param_index(ins, "@b") == 4);
	CHECK(db_param_index(ins, ":nosuch") == 0);

	char text[16];
	strcpy(text, "hello");
	const unsigned char blob[6] = { 0x00, 0xde, 0xad, 0x00, 0xbe, 0xef };
	CHECK(db_bind_int64(ins, 1, 5000000000LL) == DB_OK);
	CHECK(db_bind_double(ins, 2, 2.5) == DB_OK);
	CHECK(db_bind_text(ins, 3, text) == DB_OK);
	CHECK(db_bind_blob(ins, 4, blob, sizeof(blob)) == DB_OK);
	CHECK(db_bind_null(ins, 5) == DB_OK);
	strcpy(text, "XXXXX"); // bind_text copies
	CHECK(db_step(ins) == DB_DONE);
	CHECK(db_changes(db) == 1);
	CHECK(db_last_insert_id(db) == 1);

	// Bindings survive a reset; the zero-copy text and small ints work too
	CHECK(db_reset(ins) == DB_OK);
	CHECK(db_bind_int(ins, 1, -7) == DB_OK);
	CHECK(db_bind_double(ins, 2, -1.25) == DB_OK);
	CHECK(db_bind_text_ref(ins, 3, "it's \"quoted\" \\ text") == DB_OK);
	CHECK(db_bind_null(ins, 4) == DB_OK);
	CHECK(db_step(ins) == DB_DONE);
	CHECK(db_last_insert_id(db) == 2);

	// Stepping a finished statement runs it again, like SQLite
	CHECK(db_step(ins) == DB_DONE);
	CHECK(scalar(db, "SELECT count(*) FROM t") == 3);

	// Special floating point values
	CHECK(db_reset(ins) == DB_OK);
	CHECK(db_bind_int(ins, 1, 0) == DB_OK);
	CHECK(db_bind_double(ins, 2, 1.0 / 0.0) == DB_OK);
	CHECK(db_bind_text_ref(ins, 3, "inf") == DB_OK);
	CHECK(db_step(ins) == DB_DONE);
	CHECK(db_bind_double(ins, 2, 0.0 / 0.0) == DB_OK);
	CHECK(db_step(ins) == DB_DONE);

	// Bad indexes are errors, not crashes
	CHECK(db_bind_int(ins, 0, 1) != DB_OK);
	CHECK(db_bind_int(ins, 99, 1) != DB_OK);
	db_finalize(ins);

	db_stmt *sel = db_prepare(db, "SELECT i, r, s, b, n, (i > 0) AS positive, 12.5::numeric AS d FROM t ORDER BY id", false);
	CHECK(sel != NULL);
	// Names and count are known before the first step
	CHECK(db_column_count(sel) == 7);
	CHECK(strcmp(db_column_name(sel, 0), "i") == 0 && strcmp(db_column_name(sel, 5), "positive") == 0);

	CHECK(db_step(sel) == DB_ROW);
	CHECK(db_column_type(sel, 0) == DB_TYPE_INT64);
	CHECK(db_column_type(sel, 1) == DB_TYPE_DOUBLE);
	CHECK(db_column_type(sel, 2) == DB_TYPE_TEXT);
	CHECK(db_column_type(sel, 3) == DB_TYPE_BLOB);
	CHECK(db_column_type(sel, 4) == DB_TYPE_NULL);
	CHECK(db_column_type(sel, 5) == DB_TYPE_INT64);
	CHECK(db_column_type(sel, 6) == DB_TYPE_DOUBLE);
	CHECK(db_column_int64(sel, 0) == 5000000000LL);
	CHECK(db_column_int(sel, 5) == 1);
	CHECK(db_column_double(sel, 1) == 2.5);
	CHECK(db_column_double(sel, 6) == 12.5);
	CHECK(strcmp(db_column_text(sel, 2), "hello") == 0);
	CHECK(db_column_bytes(sel, 2) == 5);
	CHECK(db_column_bytes(sel, 3) == 6 && memcmp(db_column_blob(sel, 3), blob, 6) == 0);
	CHECK(db_column_text(sel, 4) == NULL && db_column_bytes(sel, 4) == 0);

	CHECK(db_step(sel) == DB_ROW);
	CHECK(db_column_int(sel, 0) == -7);
	CHECK(strcmp(db_column_text(sel, 2), "it's \"quoted\" \\ text") == 0);
	CHECK(db_column_int(sel, 5) == 0);
	CHECK(db_column_type(sel, 3) == DB_TYPE_NULL);
	CHECK(db_column_blob(sel, 3) == NULL);

	CHECK(db_step(sel) == DB_ROW); // the row that was stepped again
	CHECK(db_step(sel) == DB_ROW); // the infinite one
	CHECK(db_step(sel) == DB_ROW); // the NaN one
	CHECK(db_step(sel) == DB_DONE);
	CHECK(db_reset(sel) == DB_OK);
	CHECK(db_step(sel) == DB_ROW); // starts over
	db_finalize(sel);
	db_finalize(NULL);

	// Rows affected by an UPDATE and a DELETE
	CHECK(db_exec(db, "UPDATE t SET n = 'x' WHERE i IS NOT NULL") == DB_OK);
	CHECK(db_changes(db) == 5);
	CHECK(db_exec(db, "DELETE FROM t WHERE i = 0") == DB_OK);
	CHECK(db_changes(db) == 2);

	db_close(db);
}

static void test_placeholder_translation(void)
{
	db_conn *db = open_pg();
	db_stmt *s;

	// Question marks and colons inside literals, comments and casts stay
	s = db_prepare(db, "SELECT '?' || ':name' || \"col\" FROM (SELECT 1 AS \"col\") q -- a ? in a comment\n WHERE $1::int = ?1", false);
	CHECK(s != NULL);
	if(s != NULL)
	{
		CHECK(db_bind_int(s, 1, 1) == DB_OK);
		CHECK(db_step(s) == DB_ROW);
		CHECK(strcmp(db_column_text(s, 0), "?:name1") == 0);
		db_finalize(s);
	}

	// Dollar quoting, casts, block comments and array slices
	s = db_prepare(db, "SELECT $tag$?:x$tag$, 5::int + ?, /* ? */ (ARRAY[1,2,3])[1:2]::text", false);
	CHECK(s != NULL);
	if(s != NULL)
	{
		CHECK(db_bind_int(s, 1, 2) == DB_OK);
		CHECK(db_step(s) == DB_ROW);
		CHECK(strcmp(db_column_text(s, 0), "?:x") == 0);
		CHECK(db_column_int(s, 1) == 7);
		CHECK(strcmp(db_column_text(s, 2), "{1,2}") == 0);
		db_finalize(s);
	}

	// The same name is one parameter, positional and named parameters mix
	s = db_prepare(db, "SELECT :a::int + :a::int + ?::int", false);
	CHECK(s != NULL);
	if(s != NULL)
	{
		CHECK(db_param_index(s, ":a") == 1);
		CHECK(db_bind_int(s, 1, 10) == DB_OK && db_bind_int(s, 2, 5) == DB_OK);
		CHECK(db_step(s) == DB_ROW && db_column_int(s, 0) == 25);
		db_finalize(s);
	}

	// Native $N placeholders pass through
	s = db_prepare(db, "SELECT $2::int - $1::int", false);
	CHECK(s != NULL);
	if(s != NULL)
	{
		CHECK(db_bind_int(s, 1, 3) == DB_OK && db_bind_int(s, 2, 10) == DB_OK);
		CHECK(db_step(s) == DB_ROW && db_column_int(s, 0) == 7);
		db_finalize(s);
	}

	db_close(db);
}

/* ---- errors ---- */

// The packed form of a SQLSTATE, as errcode() returns it
static int code_of(const char *state)
{
	int v = 0;
	for(int i = 0; i < 5; i++)
	{
		const char c = state[i];
		v = v * 36 + (c >= 'A' ? 10 + (c - 'A') : c - '0');
	}
	return v;
}

static void test_errors(void)
{
	db_conn *db = open_pg();
	CHECK(db_exec(db, "CREATE TABLE e (a BIGINT PRIMARY KEY, b TEXT NOT NULL)") == DB_OK);
	CHECK(db_exec(db, "INSERT INTO e VALUES (1, 'x')") == DB_OK);

	CHECK(db_exec(db, "INSERT INTO e VALUES (1, 'y')") == DB_CONSTRAINT);
	CHECK(db_last_rc(db) == DB_CONSTRAINT);
	CHECK(strcmp(db_errname(db, db_errcode(db)), "SQLSTATE_23505") == 0);
	CHECK(strstr(db_errmsg(db), "duplicate key") != NULL);
	CHECK(strcmp(DB_LAST_ERR(db), "integrity constraint violation") == 0);
	CHECK(db_extended_errcode(db) == db_errcode(db));

	db_stmt *s = db_prepare(db, "INSERT INTO e VALUES (?, ?)", false);
	CHECK(s != NULL);
	CHECK(db_bind_int(s, 1, 2) == DB_OK && db_bind_null(s, 2) == DB_OK);
	CHECK(db_step(s) == DB_CONSTRAINT);
	CHECK(strcmp(db_errname(db, db_errcode(db)), "SQLSTATE_23502") == 0);
	CHECK(strlen(db_stmt_errmsg(s)) > 0 && strlen(db_stmt_errstr(s)) > 0);
	db_finalize(s);

	// Syntax errors and missing objects fail at prepare or execution
	CHECK(db_prepare(db, "SELEKT nonsense", false) == NULL);
	CHECK(db_last_rc(db) == DB_ERROR && strcmp(db_errname(db, db_errcode(db)), "SQLSTATE_42601") == 0);
	CHECK(db_prepare(db, "SELECT * FROM no_such_table", false) == NULL);
	CHECK(strcmp(db_errname(db, db_errcode(db)), "SQLSTATE_42P01") == 0);
	CHECK(db_exec(db, "NOT SQL") == DB_ERROR);
	CHECK(scalar(db, "SELECT 1") == 1);
	CHECK(db_errcode(db) == 0); // a success clears the error

	// A row lock that is taken is reported as busy, not as an error
	db_conn *other = open_pg();
	CHECK(db_begin(db, DB_TX_IMMEDIATE) == DB_OK);
	CHECK(scalar(db, "SELECT a FROM e WHERE a = 1 FOR UPDATE") == 1);
	CHECK(db_set_busy_handler(other, NULL, NULL) == DB_OK);
	CHECK(db_exec(other, "SELECT a FROM e WHERE a = 1 FOR UPDATE NOWAIT") == DB_BUSY);
	CHECK(strcmp(db_errname(other, db_errcode(other)), "SQLSTATE_55P03") == 0);
	CHECK(db_exec(other, "UPDATE e SET b = 'z' WHERE a = 1") == DB_BUSY); // lock_timeout of 1 ms
	CHECK(db_rollback(db) == DB_OK);
	CHECK(db_exec(other, "UPDATE e SET b = 'z' WHERE a = 1") == DB_OK);
	// The waiting handler is a longer lock_timeout
	CHECK(db_set_busy_handler(other, (int (*)(void*, int))0x1, NULL) == DB_OK);
	char *timeout = text_scalar(other, "SHOW lock_timeout");
	CHECK(timeout != NULL && strcmp(timeout, "5s") == 0);
	free(timeout);
	db_close(other);

	// After a failure a transaction is aborted until it is rolled back
	CHECK(db_begin(db, DB_TX_DEFERRED) == DB_OK);
	CHECK(db_exec(db, "INSERT INTO e VALUES (1, 'dup')") == DB_CONSTRAINT);
	CHECK(db_exec(db, "SELECT 1") == DB_ERROR);
	CHECK(strcmp(db_errname(db, db_errcode(db)), "SQLSTATE_25P02") == 0);
	CHECK(db_rollback(db) == DB_OK);
	CHECK(scalar(db, "SELECT 1") == 1);

	// classify_error maps SQLSTATEs
	CHECK(pg->classify_error(0) == DB_OK);
	CHECK(pg->classify_error(code_of("23503")) == DB_CONSTRAINT);
	CHECK(pg->classify_error(code_of("40001")) == DB_BUSY);
	CHECK(pg->classify_error(code_of("40P01")) == DB_BUSY);
	CHECK(pg->classify_error(code_of("55P03")) == DB_BUSY);
	CHECK(pg->classify_error(code_of("25006")) == DB_READONLY);
	CHECK(pg->classify_error(code_of("XX001")) == DB_CORRUPT);
	CHECK(pg->classify_error(code_of("42601")) == DB_ERROR);
	CHECK(strcmp(pg->errstr(code_of("42P01")), "syntax error or access rule violation") == 0);
	CHECK(strcmp(pg->errstr(code_of("99999")), "database error") == 0);

	db_close(db);
}

/* ---- arrays ---- */

static void test_arrays(void)
{
	db_conn *db = open_pg();
	CHECK(db_exec(db, "CREATE TABLE a (i INTEGER, d DOUBLE PRECISION, s TEXT)") == DB_OK);
	CHECK(db_exec(db, "INSERT INTO a VALUES (1, 1.5, 'a'), (2, 2.5, 'b c'), (3, 3.5, 'q\"uote'), (4, 4.5, 'back\\slash')") == DB_OK);

	const int32_t ints[] = { 1, 3, 9 };
	db_stmt *s = db_prepare(db, "SELECT count(*) FROM a WHERE i = ANY(?1)", false);
	CHECK(s != NULL);
	CHECK(db_bind_array(s, 1, DB_TYPE_INT, ints, 3) == DB_OK);
	CHECK(db_step(s) == DB_ROW && db_column_int(s, 0) == 2);
	CHECK(db_reset(s) == DB_OK);
	const int32_t none[] = { 0 };
	CHECK(db_bind_array(s, 1, DB_TYPE_INT, none, 0) == DB_OK); // an empty array
	CHECK(db_step(s) == DB_ROW && db_column_int(s, 0) == 0);
	db_finalize(s);

	const int64_t big[] = { 2, 4 };
	s = db_prepare(db, "SELECT count(*) FROM a WHERE i = ANY(?)", false);
	CHECK(db_bind_array(s, 1, DB_TYPE_INT64, big, 2) == DB_OK);
	CHECK(db_step(s) == DB_ROW && db_column_int(s, 0) == 2);
	db_finalize(s);

	const double dbl[] = { 1.5, 4.5 };
	s = db_prepare(db, "SELECT count(*) FROM a WHERE d = ANY(?)", false);
	CHECK(db_bind_array(s, 1, DB_TYPE_DOUBLE, dbl, 2) == DB_OK);
	CHECK(db_step(s) == DB_ROW && db_column_int(s, 0) == 2);
	db_finalize(s);

	const char *txt[] = { "a", "b c", "q\"uote", "back\\slash", "zz" };
	s = db_prepare(db, "SELECT count(*) FROM a WHERE s = ANY(?)", false);
	CHECK(db_bind_array(s, 1, DB_TYPE_TEXT, txt, 5) == DB_OK);
	CHECK(db_step(s) == DB_ROW && db_column_int(s, 0) == 4);
	CHECK(db_bind_array(s, 1, DB_TYPE_BLOB, txt, 1) == DB_ERROR);
	CHECK(db_bind_array(s, 1, DB_TYPE_NULL, txt, 1) == DB_ERROR);
	db_finalize(s);

	db_close(db);
}

/* ---- transactions ---- */

static void test_transactions(void)
{
	db_conn *db = open_pg();
	CHECK(db_exec(db, "CREATE TABLE x (a INTEGER)") == DB_OK);

	const db_txmode modes[] = { DB_TX_DEFERRED, DB_TX_IMMEDIATE, DB_TX_EXCLUSIVE };
	for(unsigned int i = 0; i < 3; i++)
	{
		CHECK(db_begin(db, modes[i]) == DB_OK);
		CHECK(db_exec(db, "INSERT INTO x VALUES (1)") == DB_OK);
		CHECK(db_rollback(db) == DB_OK);
		CHECK(scalar(db, "SELECT count(*) FROM x") == 0);
	}

	CHECK(db_begin(db, DB_TX_DEFERRED) == DB_OK);
	CHECK(db_exec(db, "INSERT INTO x VALUES (1)") == DB_OK);
	CHECK(db_commit(db) == DB_OK);
	CHECK(scalar(db, "SELECT count(*) FROM x") == 1);

	// A savepoint can be rolled back on its own; names are quoted
	CHECK(db_begin(db, DB_TX_DEFERRED) == DB_OK);
	CHECK(db_savepoint(db, "sp1") == DB_OK);
	CHECK(db_exec(db, "INSERT INTO x VALUES (2)") == DB_OK);
	CHECK(db_savepoint(db, "we\"ird; DROP TABLE x") == DB_OK);
	CHECK(db_exec(db, "INSERT INTO x VALUES (3)") == DB_OK);
	CHECK(db_release_savepoint(db, "we\"ird; DROP TABLE x") == DB_OK);
	CHECK(db_exec(db, "ROLLBACK TO SAVEPOINT sp1") == DB_OK);
	CHECK(db_release_savepoint(db, "sp1") == DB_OK);
	CHECK(db_commit(db) == DB_OK);
	CHECK(scalar(db, "SELECT count(*) FROM x") == 1);

	CHECK(db_release_savepoint(db, "nosuch") != DB_OK);
	db_close(db);
}

/* ---- schemas and maintenance ---- */

static void test_schema(void)
{
	db_conn *db = open_pg();
	CHECK(db_exec(db, "CREATE TABLE s (a BIGSERIAL PRIMARY KEY, b TEXT)") == DB_OK);
	CHECK(db_exec(db, "CREATE VIEW v AS SELECT * FROM s") == DB_OK);

	CHECK(db_table_exists(db, "s") && db_table_exists(db, "v") && db_table_exists(db, "lz_test.s"));
	CHECK(!db_table_exists(db, "nosuch"));
	CHECK(db_column_exists(db, "s", "b") && db_column_exists(db, "S", "B"));
	CHECK(!db_column_exists(db, "s", "zzz") && !db_column_exists(db, "nosuch", "b"));

	CHECK(db_get_schema_version(db) == 0);
	CHECK(db_set_schema_version(db, 22) == DB_OK);
	CHECK(db_get_schema_version(db) == 22);
	CHECK(db_set_schema_version(db, 23) == DB_OK);
	CHECK(db_get_schema_version(db) == 23);

	CHECK(db_row_count(db, "s") == 0);
	CHECK(db_exec(db, "INSERT INTO s(b) VALUES ('a'), ('b')") == DB_OK);
	CHECK(db_row_count(db, "s") == 2 && db_row_count(db, "lz_test.s") == 2);
	CHECK(db_row_count(db, "nosuch") == -1);

	CHECK(db_optimize(db) == DB_OK);
	CHECK(db_vacuum(db) == DB_OK);
	const char *msg = "unset";
	CHECK(db_integrity_check(db, &msg) == DB_OK && msg == NULL);

	// Copy between schemas, the counterpart of attached databases
	CHECK(db_exec(db, "CREATE SCHEMA lz_src") == DB_OK);
	CHECK(db_exec(db, "CREATE TABLE lz_src.s (a BIGINT, b TEXT); INSERT INTO lz_src.s VALUES (10,'x'),(11,'y'),(12,'z')") == DB_OK);
	CHECK(db_exec(db, "DELETE FROM s") == DB_OK);
	CHECK(db_copy_table(db, "lz_src", "lz_test", "s", NULL) == DB_OK);
	CHECK(scalar(db, "SELECT count(*) FROM s") == 3);
	CHECK(db_exec(db, "DELETE FROM s") == DB_OK);
	CHECK(db_copy_table(db, "lz_src", "lz_test", "s", "a >= 11") == DB_OK);
	CHECK(scalar(db, "SELECT count(*) FROM s") == 2);
	CHECK(db_copy_table(db, "lz_src", "lz_test", "nosuch", NULL) != DB_OK);
	CHECK(db_copy_table(db, "lz_src", "lz_test", "s\"; DROP TABLE s; --", NULL) != DB_OK);
	CHECK(scalar(db, "SELECT count(*) FROM s") == 2);

	// Not supported: attach, detach, serialization
	CHECK(!db_can_attach(db));
	CHECK(db_attach(db, "x", "y") == DB_ERROR && db_detach(db, "y") == DB_ERROR);
	int64_t size = 0;
	CHECK(db_serialize(db, NULL, &size) == NULL);
	CHECK(db_deserialize(db, NULL, "x", 1, false) == DB_ERROR);

	// reset_database erases the public schema, only in a database made for tests
	char *name = text_scalar(db, "SELECT current_database()");
	if(name != NULL && strstr(name, "test") != NULL)
	{
		CHECK(db_exec(db, "CREATE TABLE public.reset_me (a int)") == DB_OK);
		CHECK(db_reset_database(db) == DB_OK);
		CHECK(!db_table_exists(db, "public.reset_me"));
		CHECK(db_exec(db, "CREATE TABLE public.after_reset (a int)") == DB_OK);
	}
	free(name);

	db_close(db);
}

/* ---- formatting and dialect ---- */

static char *vmprintf_wrapper(db_conn *db, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	char *s = db_vmprintf(db, fmt, ap);
	va_end(ap);
	return s;
}

static void test_format_and_dialect(void)
{
	db_conn *db = open_pg();
	char *q;

	q = pg->mprintf("SELECT %Q, %d, '%q', \"%w\"", "it's", 42, "it's", "co\"l");
	CHECK(q != NULL && strcmp(q, "SELECT 'it''s', 42, 'it''s', \"co\"\"l\"") == 0);
	db_free(db, q);
	q = pg->mprintf("%Q|%s|%5d|%-4d|%05.1f|%lld|%zu|%x|%c|%%|%.3s", (char*)NULL, "text", 42, 7, 3.14159, 1234567890123LL, (size_t)9, 255, 'z', "abcdef");
	CHECK(q != NULL && strcmp(q, "NULL|text|   42|7   |003.1|1234567890123|9|ff|z|%|abc") == 0);
	db_free(db, q);
	q = vmprintf_wrapper(db, "%*d-%.*f", 4, 5, 1, 2.55);
	CHECK(q != NULL && (strcmp(q, "   5-2.5") == 0 || strcmp(q, "   5-2.6") == 0));
	db_free(db, q);

	// %Q makes a string safe to put in SQL
	q = pg->mprintf("SELECT %Q", "x'; DROP TABLE t; --");
	CHECK(q != NULL && db_exec(db, q) == DB_OK);
	db_free(db, q);

	const db_dialect *d = pg->dialect;
	CHECK(strcmp(d->name, "postgres") == 0);
	CHECK(strcmp(d->placeholder(3), "$3") == 0);

	char sql[512];
	snprintf(sql, sizeof(sql), "SELECT %s", d->now_expr());
	CHECK(scalar(db, sql) > 1700000000);

	snprintf(sql, sizeof(sql), "CREATE TABLE dial (id %s, name TEXT UNIQUE, n INTEGER)", d->autoincrement_pk());
	CHECK(db_exec(db, sql) == DB_OK);
	CHECK(db_exec(db, "INSERT INTO dial(name, n) VALUES ('a', 1)") == DB_OK);
	CHECK(db_last_insert_id(db) == 1);

	snprintf(sql, sizeof(sql), "INSERT INTO dial(name, n) VALUES ('a', 5) %s", d->upsert_suffix("name", "n = excluded.n"));
	CHECK(db_exec(db, sql) == DB_OK);
	CHECK(scalar(db, "SELECT n FROM dial WHERE name = 'a'") == 5);
	snprintf(sql, sizeof(sql), "INSERT INTO dial(name, n) VALUES ('a', 9) %s", d->upsert_suffix("name", NULL));
	CHECK(db_exec(db, sql) == DB_OK);
	CHECK(scalar(db, "SELECT n FROM dial WHERE name = 'a'") == 5);

	CHECK(db_exec(db, "INSERT INTO dial(name, n) VALUES ('b', 2), ('c', 3)") == DB_OK);
	char in[128];
	CHECK(d->in_list(in, sizeof(in), "n", "?1") > 0 && strcmp(in, "n = ANY(?1)") == 0);
	snprintf(sql, sizeof(sql), "SELECT count(*) FROM dial WHERE %s", in);
	db_stmt *s = db_prepare(db, sql, false);
	const int32_t wanted[] = { 2, 3 };
	CHECK(s != NULL && db_bind_array(s, 1, DB_TYPE_INT, wanted, 2) == DB_OK);
	CHECK(s != NULL && db_step(s) == DB_ROW && db_column_int(s, 0) == 2);
	db_finalize(s);
	char tiny[8];
	CHECK(d->in_list(tiny, sizeof(tiny), "some_long_column", "?1") == -1);

	CHECK(d->glob_op() == NULL);
	CHECK(strcmp(d->regexp_op(), "~") == 0);
	snprintf(sql, sizeof(sql), "SELECT 'a.example' %s '^a\\.'", d->regexp_op());
	char *matched = text_scalar(db, sql);
	CHECK(matched != NULL);
	free(matched);

	db_close(db);
}

/* ---- closing, interrupting, threads ---- */

struct sleeper {
	db_conn *db;
	db_rc rc;
	double elapsed;
};

static void *sleeper_thread(void *arg)
{
	struct sleeper *sl = arg;
	struct timespec a, b;
	clock_gettime(CLOCK_MONOTONIC, &a);
	sl->rc = db_exec(sl->db, "SELECT pg_sleep(30)");
	clock_gettime(CLOCK_MONOTONIC, &b);
	sl->elapsed = (double)(b.tv_sec - a.tv_sec) + (double)(b.tv_nsec - a.tv_nsec) / 1e9;
	return NULL;
}

static void test_close_and_interrupt(void)
{
	// A statement still alive on close is released by the driver
	db_conn *db = open_pg();
	CHECK(db_exec(db, "CREATE TABLE c (a INTEGER)") == DB_OK);
	CHECK(db_prepare(db, "SELECT * FROM c", false) != NULL);
	db_close(db); // logs and frees, must not crash or leak

	// Deferred close: the cursor keeps working until it is finalized
	db = open_pg();
	CHECK(db_exec(db, "INSERT INTO c VALUES (1),(2)") == DB_OK);
	db_stmt *cursor = db_prepare(db, "SELECT a FROM c ORDER BY a", false);
	CHECK(cursor != NULL && db_step(cursor) == DB_ROW && db_column_int(cursor, 0) == 1);
	db_close_deferred(db);
	CHECK(db_step(cursor) == DB_ROW && db_column_int(cursor, 0) == 2);
	CHECK(db_step(cursor) == DB_DONE);
	CHECK(strlen(db_stmt_errstr(cursor)) > 0);
	db_finalize(cursor); // the connection goes with it

	// interrupt() cancels a running query from another thread
	db = open_pg();
	struct sleeper sl = { db, DB_OK, 0.0 };
	pthread_t thread;
	pthread_create(&thread, NULL, sleeper_thread, &sl);
	usleep(500000);
	db_interrupt(db);
	pthread_join(thread, NULL);
	CHECK(sl.rc != DB_OK && sl.elapsed < 10.0);
	CHECK(strcmp(db_errname(db, db_errcode(db)), "SQLSTATE_57014") == 0);
	CHECK(scalar(db, "SELECT 1") == 1); // the connection is still good
	db_close(db);
}

struct worker {
	db_conn *db;
	int id;
	int inserted;
};

static void *worker_thread(void *arg)
{
	struct worker *w = arg;
	db_stmt *ins = db_prepare(w->db, "INSERT INTO shared (worker, n) VALUES (?, ?)", false);
	for(int i = 0; ins != NULL && i < 150; i++)
	{
		db_bind_int(ins, 1, w->id);
		db_bind_int(ins, 2, i);
		if(db_step(ins) == DB_DONE)
			w->inserted++;
		db_reset(ins);
	}
	db_finalize(ins);
	return NULL;
}

static void test_threads_and_big_results(void)
{
	// Two threads share one connection (the driver serializes access)
	db_conn *db = open_pg();
	CHECK(db_exec(db, "CREATE TABLE shared (worker INTEGER, n INTEGER)") == DB_OK);
	struct worker workers[2] = { { db, 1, 0 }, { db, 2, 0 } };
	pthread_t threads[2];
	for(int i = 0; i < 2; i++)
		pthread_create(&threads[i], NULL, worker_thread, &workers[i]);
	for(int i = 0; i < 2; i++)
		pthread_join(threads[i], NULL);
	CHECK(workers[0].inserted == 150 && workers[1].inserted == 150);
	CHECK(scalar(db, "SELECT count(*) FROM shared") == 300);

	// A large result is read row by row
	db_stmt *s = db_prepare(db, "SELECT g, md5(g::text) FROM generate_series(1, 20000) g", false);
	CHECK(s != NULL);
	int64_t sum = 0;
	int rows = 0;
	while(s != NULL && db_step(s) == DB_ROW)
	{
		sum += db_column_int64(s, 0);
		CHECK(db_column_bytes(s, 1) == 32 || rows > 0);
		rows++;
	}
	CHECK(rows == 20000 && sum == 200010000LL);
	db_finalize(s);

	// Two statements are open on the connection at once
	db_stmt *outer = db_prepare(db, "SELECT g FROM generate_series(1, 3) g", false);
	db_stmt *inner = db_prepare(db, "SELECT count(*) FROM shared WHERE worker = ?", false);
	int seen = 0;
	while(outer != NULL && inner != NULL && db_step(outer) == DB_ROW)
	{
		db_bind_int(inner, 1, (db_column_int(outer, 0) % 2) + 1);
		CHECK(db_step(inner) == DB_ROW && db_column_int(inner, 0) == 150);
		db_reset(inner);
		seen++;
	}
	CHECK(seen == 3);
	db_finalize(outer);
	db_finalize(inner);
	db_close(db);
}


/* ---- baseline schema ---- */

// Names in one column of a query, sorted
static int collect(db_conn *db, const char *sql, const char *arg, char out[64][64])
{
	db_stmt *s = db_prepare(db, sql, false);
	if(s == NULL)
		return -1;
	if(arg != NULL)
		db_bind_text_ref(s, 1, arg);
	int n = 0;
	while(n < 64 && db_step(s) == DB_ROW)
		snprintf(out[n++], 64, "%s", db_column_text(s, 0) != NULL ? db_column_text(s, 0) : "");
	db_finalize(s);
	return n;
}

static int by_name(const void *a, const void *b)
{
	return strcmp((const char*)a, (const char*)b);
}

static void test_baseline_schema(void)
{
	db_conn *db = open_pg();
	CHECK(db_exec(db, "DROP SCHEMA lz_test CASCADE; CREATE SCHEMA lz_test") == DB_OK); // an empty database
	const char *error = NULL;
	CHECK(db_schema_baseline(db, &error));
	if(error != NULL)
		fprintf(stderr, "baseline: %s\n", error);
	check_baseline_content(db);
	check_baseline_behaviour(db);
	CHECK(db_schema_migrate(db, DB_SCHEMA_VERSION, &error));
	CHECK(!db_schema_migrate(db, DB_SCHEMA_VERSION - 1, &error) && strstr(error, "no migration") != NULL);
	CHECK(!db_schema_migrate(db, DB_SCHEMA_VERSION + 1, &error) && strstr(error, "newer") != NULL);

	// A failed baseline rolls back completely: a second schema fails in the first
	// statement and the data of the first is untouched
	CHECK(scalar(db, "SELECT count(*) FROM domain_by_id") == 2);

	// The same schema as the one SQLite gets: tables, columns in order,
	// whether they can be NULL, and the indexes that were created explicitly
	db_conn *lite = db_driver_sqlite.open(":memory:", DB_OPEN_READWRITE | DB_OPEN_MEMORY, NULL, NULL);
	CHECK(lite != NULL && db_schema_baseline(lite, &error));
	for(unsigned int i = 0; lite != NULL && i < sizeof(baseline_tables) / sizeof(baseline_tables[0]); i++)
	{
		const char *table = baseline_tables[i];
		static char sqlite_cols[64][64], pg_cols[64][64];
		char sql[256];

		// name:notnull, a primary key counts as not null on both
		snprintf(sql, sizeof(sql), "SELECT lower(name) || ':' || CASE WHEN \"notnull\" = 1 OR pk > 0 THEN 'n' ELSE 'y' END "
		                           "FROM pragma_table_info('%s') ORDER BY cid", table);
		const int a = collect(lite, sql, NULL, sqlite_cols);
		const int b = collect(db,
			"SELECT column_name || ':' || CASE WHEN is_nullable = 'NO' THEN 'n' ELSE 'y' END "
			"FROM information_schema.columns WHERE table_schema = 'lz_test' AND table_name = ? ORDER BY ordinal_position",
			table, pg_cols);
		CHECK(a > 0 && a == b);
		for(int c = 0; c < a && c < b; c++)
			if(strcmp(sqlite_cols[c], pg_cols[c]) != 0)
			{
				CHECK(false);
				fprintf(stderr, "  %s column %d: SQLite %s, PostgreSQL %s\n", table, c, sqlite_cols[c], pg_cols[c]);
			}

		static char sqlite_idx[64][64], pg_idx[64][64];
		const int ai = collect(lite, "SELECT name FROM sqlite_master WHERE type = 'index' AND tbl_name = ? AND name NOT LIKE 'sqlite_%'", table, sqlite_idx);
		const int bi = collect(db, "SELECT indexname FROM pg_indexes WHERE schemaname = 'lz_test' AND tablename = ? "
		                           "AND indexname NOT LIKE '%_pkey' AND indexname NOT LIKE '%_key'", table, pg_idx);
		CHECK(ai == bi);
		qsort(sqlite_idx, (size_t)(ai > 0 ? ai : 0), 64, by_name);
		qsort(pg_idx, (size_t)(bi > 0 ? bi : 0), 64, by_name);
		for(int c = 0; c < ai && c < bi; c++)
			if(strcmp(sqlite_idx[c], pg_idx[c]) != 0)
			{
				CHECK(false);
				fprintf(stderr, "  %s index: SQLite %s, PostgreSQL %s\n", table, sqlite_idx[c], pg_idx[c]);
			}
	}
	db_close(lite);
	db_close(db);
}

/* ---- main and stubs ---- */

int main(void)
{
	url = getenv("POSTGRES_URL");
	if(url == NULL || *url == '\0')
	{
		printf("POSTGRES_URL is not set\n");
		printf("DB_POSTGRES_REGRESSION=SKIP\n");
		return 0;
	}
	pg = &db_driver_postgres;
	if(!db_driver_select("postgres"))
	{
		fprintf(stderr, "the PostgreSQL driver is not built in\n");
		return 1;
	}

	db_conn *setup = db_open(url, DB_OPEN_READWRITE);
	if(setup == NULL)
	{
		fprintf(stderr, "cannot connect to POSTGRES_URL\n");
		printf("DB_POSTGRES_REGRESSION=FAIL\n");
		return 1;
	}
	db_exec(setup, "DROP SCHEMA IF EXISTS lz_test CASCADE; DROP SCHEMA IF EXISTS lz_src CASCADE; CREATE SCHEMA lz_test");
	db_close(setup);

	test_registry_and_open();
	test_statements();
	test_placeholder_translation();
	test_errors();
	test_arrays();
	test_transactions();
	test_schema();
	test_format_and_dialect();
	test_close_and_interrupt();
	test_threads_and_big_results();
	test_baseline_schema();

	setup = db_open(url, DB_OPEN_READWRITE);
	if(setup != NULL)
	{
		db_exec(setup, "DROP SCHEMA IF EXISTS lz_test CASCADE; DROP SCHEMA IF EXISTS lz_src CASCADE");
		db_close(setup);
	}

	printf("%d checks, %d failures\n", checks, failures);
	printf("DB_POSTGRES_REGRESSION=%s\n", failures == 0 ? "PASS" : "FAIL");
	return failures == 0 ? 0 : 1;
}

// The drivers include lorentz.h (libc wrappers) and log.h (logging entry point).
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
