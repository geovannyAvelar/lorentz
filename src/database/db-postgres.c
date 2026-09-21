/* Lorentz: A black hole for Internet advertisements
*  (c) 2026 Pi-hole, LLC (https://pi-hole.net)
*  Network-wide ad blocking via your own hardware.
*
*  Lorentz Engine
*  PostgreSQL database driver
*  /src/database/db-postgres.c
*
*  This file is copyright under the latest version of the EUPL.
*  Please see LICENSE file for your rights under this license. */

// A driver for the database layer on top of libpq.
//
// What it does, and what it deliberately does not:
//
// * The connection string is the libpq one, either a URI
//   ("postgresql://user:password@host:5432/dbname") or "key=value" pairs. A
//   NULL string uses the PG* environment variables.
// * Statements are prepared on the server. The placeholders SQLite style SQL
//   uses (?, ?NNN, :name, @name) are translated to $N, so callers may write
//   either form. Values are sent in text format, so the type of a parameter
//   is inferred by the server from its context: write "$1::int" where
//   there is none (for example "SELECT $1 + $2").
// * A statement is executed by its first step and its whole result is kept on
//   the client until it is reset or finalized. This matches SQLite's freedom
//   to interleave statements on one connection, at the cost of memory for
//   very large results.
// * Differences a caller has to know about are listed at the end of this
//   comment; the SQL text itself is not translated (only the placeholders).
//     - There is no ATTACH: attach() and detach() are NULL, schemas take
//       their place and copy_table() copies between schemas.
//     - There is no serialization of a database file and no in-memory
//       database (DB_OPEN_MEMORY fails).
//     - After a failed statement a transaction is aborted until it is rolled
//       back, SQLite would carry on.
//     - BEGIN IMMEDIATE and BEGIN EXCLUSIVE are a plain BEGIN.
//     - last_insert_id() needs a sequence (BIGSERIAL) and returns 0 otherwise.
//     - There is no busy handler, waiting for a lock is the job of
//       lock_timeout, which set_busy_handler() sets.
//     - glob_op() has no equivalent and returns NULL.
//     - The schema version is kept in the table lorentz_schema_version.
//
// Errors are reported through SQLSTATE codes, packed into the integer that
// errcode() returns (five base 36 digits).

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "lorentz.h"
#include "db-driver.h"
#include "log.h"

#include <libpq-fe.h>

#include <ctype.h>
#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// lorentz.h redirects a number of libc functions to tracking wrappers. This
// driver only uses plain libc, which is what libpq allocates with as well
#undef free
#undef strdup
#undef calloc
#undef realloc
#undef snprintf
#undef vsnprintf
#undef strlen
#undef strncmp
#undef strcmp
#undef memset
#undef memcpy
#undef strstr
#undef strncpy
#undef strcpy
#undef strcat
#undef strncat
#undef sprintf
#undef vsprintf
#undef memmove
#undef memcmp
#undef pthread_mutex_lock

struct pg_stmt;

typedef struct pg_conn {
	db_conn base; // must be first
	PGconn *pg;
	pthread_mutex_t lock;
	struct pg_stmt *stmts; // statements that are alive
	unsigned int stmt_seq;
	int refs; // one for the owner and one for each live statement
	bool readonly;
	int64_t changes;
	int sqlstate; // packed SQLSTATE of the last error, 0 after a success
	char errmsg[512];
} pg_conn;

typedef struct pg_stmt {
	db_stmt base; // must be first
	pg_conn *conn;
	struct pg_stmt *next, *prev;

	char *sql; // as the caller wrote it
	char name[32]; // name of the server side prepared statement
	int nparams;
	char **names; // ":name" of each parameter, entries may be NULL
	char **values;
	int *lengths;
	int *formats;
	bool *owned; // whether values[i] must be freed

	int ncols;
	char **colnames;

	PGresult *res;
	int state; // 0 not executed, 1 row available, 2 finished
	int row, ntuples;

	unsigned char *blob; // decoded bytea of the last column_blob() call
	size_t bloblen;
} pg_stmt;

static void (*log_callback)(void *arg, int code, const char *msg) = NULL;
static void *log_callback_arg = NULL;

/* ---- SQLSTATE handling ---- */

static int sqlstate_value(const char c)
{
	if(c >= '0' && c <= '9')
		return c - '0';
	if(c >= 'A' && c <= 'Z')
		return 10 + (c - 'A');
	if(c >= 'a' && c <= 'z')
		return 10 + (c - 'a');
	return 0;
}

static int pack_sqlstate(const char *state)
{
	if(state == NULL || strlen(state) < 5)
		return 0;
	int v = 0;
	for(int i = 0; i < 5; i++)
		v = v * 36 + sqlstate_value(state[i]);
	return v;
}

static void unpack_sqlstate(int code, char out[6])
{
	static const char digits[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ";
	for(int i = 4; i >= 0; i--)
	{
		out[i] = digits[code % 36];
		code /= 36;
	}
	out[5] = '\0';
}

static db_rc classify_sqlstate(const char state[6])
{
	if(strcmp(state, "00000") == 0)
		return DB_OK;
	// Serialization failure, deadlock, lock not available: try again later
	if(strcmp(state, "40001") == 0 || strcmp(state, "40P01") == 0 || strcmp(state, "55P03") == 0)
		return DB_BUSY;
	if(strncmp(state, "23", 2) == 0)
		return DB_CONSTRAINT;
	if(strcmp(state, "25006") == 0)
		return DB_READONLY;
	if(strcmp(state, "XX001") == 0 || strcmp(state, "XX002") == 0)
		return DB_CORRUPT;
	return DB_ERROR;
}

static db_rc pg_classify_error(int code)
{
	char state[6];
	unpack_sqlstate(code, state);
	return classify_sqlstate(state);
}

static const char *pg_errstr(int code)
{
	if(code == 0)
		return "not an error";

	char state[6];
	unpack_sqlstate(code, state);
	static const struct { const char *cls; const char *text; } classes[] = {
		{ "08", "connection exception" },
		{ "0A", "feature not supported" },
		{ "22", "data exception" },
		{ "23", "integrity constraint violation" },
		{ "25", "invalid transaction state" },
		{ "28", "invalid authorization specification" },
		{ "2B", "dependent privilege descriptors still exist" },
		{ "3D", "invalid catalog name" },
		{ "3F", "invalid schema name" },
		{ "40", "transaction rollback" },
		{ "42", "syntax error or access rule violation" },
		{ "53", "insufficient resources" },
		{ "54", "program limit exceeded" },
		{ "55", "object not in prerequisite state" },
		{ "57", "operator intervention" },
		{ "58", "system error" },
		{ "XX", "internal error" },
	};
	for(size_t i = 0; i < sizeof(classes) / sizeof(classes[0]); i++)
		if(strncmp(state, classes[i].cls, 2) == 0)
			return classes[i].text;
	return "database error";
}

static const char *pg_errname(int code)
{
	static _Thread_local char name[16];
	char state[6];
	unpack_sqlstate(code, state);
	snprintf(name, sizeof(name), "SQLSTATE_%s", state);
	return name;
}

// Remember the error of a failed result (or of the connection)
static void set_error(pg_conn *c, const PGresult *res)
{
	const char *state = res != NULL ? PQresultErrorField(res, PG_DIAG_SQLSTATE) : NULL;
	const char *msg = res != NULL ? PQresultErrorMessage(res) : PQerrorMessage(c->pg);

	// A lost connection has no SQLSTATE in the result, use the class 08
	c->sqlstate = pack_sqlstate(state != NULL ? state : "08000");
	if(c->sqlstate == 0)
		c->sqlstate = pack_sqlstate("XX000");

	snprintf(c->errmsg, sizeof(c->errmsg), "%s", msg != NULL ? msg : "unknown error");
	// libpq ends messages with a newline
	size_t len = strlen(c->errmsg);
	while(len > 0 && (c->errmsg[len - 1] == '\n' || c->errmsg[len - 1] == '\r'))
		c->errmsg[--len] = '\0';
}

static void clear_error(pg_conn *c)
{
	c->sqlstate = 0;
	c->errmsg[0] = '\0';
}

static db_rc rc_of_error(const pg_conn *c)
{
	return pg_classify_error(c->sqlstate);
}

/* ---- driver lifecycle ---- */

static int pg_init(void)
{
	return 0;
}

static void pg_shutdown(void)
{
}

static const char *pg_version(void)
{
	static _Thread_local char version[64];
	const int v = PQlibVersion();
	// 160015 is 16.15, 90624 is 9.6.24
	if(v >= 100000)
		snprintf(version, sizeof(version), "libpq %d.%d", v / 10000, v % 10000);
	else
		snprintf(version, sizeof(version), "libpq %d.%d.%d", v / 10000, (v / 100) % 100, v % 100);
	return version;
}

static void pg_set_log_callback(void (*cb)(void *arg, int code, const char *msg), void *arg)
{
	log_callback = cb;
	log_callback_arg = arg;
}

static void notice_processor(void *arg, const char *message)
{
	(void)arg;
	if(log_callback != NULL)
		log_callback(log_callback_arg, 0, message);
}

/* ---- connection ---- */

static inline pg_conn *cn(db_conn *conn)
{
	return (pg_conn*)conn;
}

static inline pg_stmt *st(db_stmt *stmt)
{
	return (pg_stmt*)stmt;
}

static void conn_free(pg_conn *c)
{
	PQfinish(c->pg);
	pthread_mutex_destroy(&c->lock);
	free(c);
}

static void conn_unref(pg_conn *c)
{
	if(--c->refs == 0)
		conn_free(c);
}

static db_conn *pg_open(const char *uri, unsigned int flags, db_rc *rcp, const char **msg)
{
	static _Thread_local char message[512];

	if(flags & DB_OPEN_MEMORY)
	{
		if(rcp != NULL)
			*rcp = DB_ERROR;
		if(msg != NULL)
			*msg = "PostgreSQL has no in-memory databases";
		return NULL;
	}

	PGconn *pg = PQconnectdb(uri != NULL ? uri : "");
	if(pg == NULL || PQstatus(pg) != CONNECTION_OK)
	{
		snprintf(message, sizeof(message), "%s", pg != NULL ? PQerrorMessage(pg) : "out of memory");
		size_t len = strlen(message);
		while(len > 0 && (message[len - 1] == '\n' || message[len - 1] == '\r'))
			message[--len] = '\0';
		if(rcp != NULL)
			*rcp = DB_ERROR;
		if(msg != NULL)
			*msg = message;
		PQfinish(pg);
		return NULL;
	}

	pg_conn *c = calloc(1, sizeof(*c));
	if(c == NULL)
	{
		PQfinish(pg);
		if(rcp != NULL)
			*rcp = DB_ERROR;
		if(msg != NULL)
			*msg = "out of memory";
		return NULL;
	}
	c->base.drv = &db_driver_postgres;
	c->pg = pg;
	c->refs = 1;
	c->readonly = (flags & DB_OPEN_READONLY) != 0;
	pthread_mutex_init(&c->lock, NULL);
	PQsetNoticeProcessor(pg, notice_processor, NULL);

	// Notices are only interesting to the log callback
	PGresult *res = PQexec(pg, "SET client_min_messages = warning");
	PQclear(res);
	if(c->readonly)
	{
		res = PQexec(pg, "SET default_transaction_read_only = on");
		PQclear(res);
	}

	return &c->base;
}

// Statements that are still alive when the connection is closed are a bug of
// the caller, name them and release them
static void stmt_release(pg_stmt *s);

static void pg_close(db_conn *conn)
{
	if(conn == NULL)
		return;

	pg_conn *c = cn(conn);
	while(c->stmts != NULL)
	{
		log_err("Statement not finalized when closing database: %s", c->stmts->sql);
		stmt_release(c->stmts);
	}
	conn_unref(c);
}

// Release the caller's reference, the connection lives on for its statements
static void pg_close_deferred(db_conn *conn)
{
	if(conn != NULL)
		conn_unref(cn(conn));
}

static void pg_interrupt(db_conn *conn)
{
	PGcancel *cancel = PQgetCancel(cn(conn)->pg);
	if(cancel == NULL)
		return;

	char error[256];
	PQcancel(cancel, error, sizeof(error));
	PQfreeCancel(cancel);
}

static bool pg_is_readonly(db_conn *conn)
{
	pg_conn *c = cn(conn);
	if(c->readonly)
		return true;
	const char *setting = PQparameterStatus(c->pg, "default_transaction_read_only");
	return setting != NULL && strcmp(setting, "on") == 0;
}

// A callback cannot be honoured, the server waits for a lock as long as
// lock_timeout says. Without a handler a lock is not waited for at all
static db_rc pg_set_busy_handler(db_conn *conn, int (*cb)(void *arg, int count), void *arg)
{
	(void)arg;
	pg_conn *c = cn(conn);
	pthread_mutex_lock(&c->lock);
	PGresult *res = PQexec(c->pg, cb != NULL ? "SET lock_timeout = '5s'" : "SET lock_timeout = '1ms'");
	const bool okay = PQresultStatus(res) == PGRES_COMMAND_OK;
	if(!okay)
		set_error(c, res);
	PQclear(res);
	pthread_mutex_unlock(&c->lock);
	return okay ? DB_OK : rc_of_error(c);
}

/* ---- running SQL ---- */

// Run a statement without parameters. c->lock must be held
static db_rc exec_locked(pg_conn *c, const char *sql)
{
	clear_error(c);
	PGresult *res = PQexec(c->pg, sql);
	const ExecStatusType status = PQresultStatus(res);
	db_rc rc = DB_OK;
	if(status == PGRES_COMMAND_OK)
		c->changes = strtoll(PQcmdTuples(res), NULL, 10);
	else if(status != PGRES_TUPLES_OK && status != PGRES_EMPTY_QUERY)
	{
		set_error(c, res);
		rc = rc_of_error(c);
		if(rc == DB_OK)
			rc = DB_ERROR;
	}
	PQclear(res);
	return rc;
}

static db_rc pg_exec(db_conn *conn, const char *sql)
{
	pg_conn *c = cn(conn);
	pthread_mutex_lock(&c->lock);
	const db_rc rc = exec_locked(c, sql);
	pthread_mutex_unlock(&c->lock);
	return rc;
}

// printf style helper for the fixed statements below
__attribute__((format(printf, 2, 3)))
static db_rc exec_fmt(pg_conn *c, const char *fmt, ...)
{
	char sql[1024];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(sql, sizeof(sql), fmt, ap);
	va_end(ap);
	return pg_exec(&c->base, sql);
}

// Quote an identifier, free the result with free()
static char *quote_ident(pg_conn *c, const char *name)
{
	pthread_mutex_lock(&c->lock);
	char *escaped = PQescapeIdentifier(c->pg, name, strlen(name));
	pthread_mutex_unlock(&c->lock);
	if(escaped == NULL)
		return NULL;
	char *copy = strdup(escaped);
	PQfreemem(escaped);
	return copy;
}

// Quote "schema.table" or "table"
static char *quote_qualified(pg_conn *c, const char *name)
{
	const char *dot = strchr(name, '.');
	if(dot == NULL)
		return quote_ident(c, name);

	char *schema = strndup(name, (size_t)(dot - name));
	char *first = schema != NULL ? quote_ident(c, schema) : NULL;
	char *second = quote_ident(c, dot + 1);
	free(schema);
	char *joined = NULL;
	if(first != NULL && second != NULL)
	{
		const size_t len = strlen(first) + strlen(second) + 2;
		joined = malloc(len);
		if(joined != NULL)
			snprintf(joined, len, "%s.%s", first, second);
	}
	free(first);
	free(second);
	return joined;
}

/* ---- statements ---- */

// Translate the placeholders of SQLite style SQL to $N. Returns the new text
// (malloc) and fills the highest parameter index and the ":name" of each
static char *translate_sql(const char *sql, int *nparams, char ***names_out)
{
	const size_t len = strlen(sql);
	// $N may be longer than "?", allow generous room
	char *out = malloc(len * 4 + 16);
	if(out == NULL)
		return NULL;

	int max_idx = 0;
	int cap = 8;
	char **names = calloc((size_t)cap, sizeof(char*));
	size_t o = 0;

	for(size_t i = 0; i < len;)
	{
		const char c = sql[i];

		// Quoted strings and identifiers, copied as they are
		if(c == '\'' || c == '"')
		{
			out[o++] = c;
			i++;
			while(i < len)
			{
				out[o++] = sql[i];
				if(sql[i] == c)
				{
					if(i + 1 < len && sql[i + 1] == c)
					{
						out[o++] = sql[++i];
					}
					else
					{
						i++;
						break;
					}
				}
				i++;
			}
			continue;
		}

		// Comments
		if(c == '-' && i + 1 < len && sql[i + 1] == '-')
		{
			while(i < len && sql[i] != '\n')
				out[o++] = sql[i++];
			continue;
		}
		if(c == '/' && i + 1 < len && sql[i + 1] == '*')
		{
			out[o++] = sql[i++];
			out[o++] = sql[i++];
			while(i < len && !(sql[i] == '*' && i + 1 < len && sql[i + 1] == '/'))
				out[o++] = sql[i++];
			if(i < len)
			{
				out[o++] = sql[i++];
				out[o++] = sql[i++];
			}
			continue;
		}

		// Dollar quoting $tag$ ... $tag$, and $N parameters
		if(c == '$')
		{
			size_t j = i + 1;
			if(j < len && isdigit((unsigned char)sql[j]))
			{
				int n = 0;
				while(j < len && isdigit((unsigned char)sql[j]))
					n = n * 10 + (sql[j++] - '0');
				if(n > max_idx)
					max_idx = n;
				memcpy(out + o, sql + i, j - i);
				o += j - i;
				i = j;
				continue;
			}
			while(j < len && (isalnum((unsigned char)sql[j]) || sql[j] == '_'))
				j++;
			if(j < len && sql[j] == '$')
			{
				// $tag$ ... $tag$, the text in between is not SQL
				const size_t taglen = j - i + 1;
				const char *tag = sql + i;
				memcpy(out + o, tag, taglen);
				o += taglen;
				i += taglen;
				while(i < len)
				{
					if(sql[i] == '$' && strncmp(sql + i, tag, taglen) == 0)
					{
						memcpy(out + o, tag, taglen);
						o += taglen;
						i += taglen;
						break;
					}
					out[o++] = sql[i++];
				}
				continue;
			}
			out[o++] = sql[i++];
			continue;
		}

		// ?, ?NNN
		if(c == '?')
		{
			size_t j = i + 1;
			int idx;
			if(j < len && isdigit((unsigned char)sql[j]))
			{
				idx = 0;
				while(j < len && isdigit((unsigned char)sql[j]))
					idx = idx * 10 + (sql[j++] - '0');
			}
			else
				idx = max_idx + 1;
			if(idx > max_idx)
				max_idx = idx;
			o += (size_t)sprintf(out + o, "$%d", idx);
			i = j;
			continue;
		}

		// :name and @name (but not the :: cast, an array slice or a time)
		if((c == ':' && !(i + 1 < len && sql[i + 1] == ':') && !(i > 0 && sql[i - 1] == ':')) || c == '@')
		{
			size_t j = i + 1;
			if(j < len && (isalpha((unsigned char)sql[j]) || sql[j] == '_'))
			{
				while(j < len && (isalnum((unsigned char)sql[j]) || sql[j] == '_'))
					j++;
				const size_t namelen = j - i;
				int idx = 0;
				for(int k = 0; k < max_idx && k < cap; k++)
					if(names[k] != NULL && strlen(names[k]) == namelen && strncmp(names[k], sql + i, namelen) == 0)
					{
						idx = k + 1;
						break;
					}
				if(idx == 0)
				{
					idx = ++max_idx;
					if(idx > cap)
					{
						const int ncap = idx * 2;
						char **grown = realloc(names, (size_t)ncap * sizeof(char*));
						if(grown == NULL)
						{
							free(out);
							free(names);
							return NULL;
						}
						memset(grown + cap, 0, (size_t)(ncap - cap) * sizeof(char*));
						names = grown;
						cap = ncap;
					}
					names[idx - 1] = strndup(sql + i, namelen);
				}
				o += (size_t)sprintf(out + o, "$%d", idx);
				i = j;
				continue;
			}
		}

		out[o++] = sql[i++];
	}

	out[o] = '\0';
	// names may be shorter than the highest index when ?NNN skips ahead
	if(max_idx > cap)
	{
		char **grown = realloc(names, (size_t)max_idx * sizeof(char*));
		if(grown != NULL)
		{
			memset(grown + cap, 0, (size_t)(max_idx - cap) * sizeof(char*));
			names = grown;
		}
	}
	*nparams = max_idx;
	*names_out = names;
	return out;
}

static void stmt_clear_result(pg_stmt *s)
{
	if(s->res != NULL)
	{
		PQclear(s->res);
		s->res = NULL;
	}
	free(s->blob);
	s->blob = NULL;
	s->bloblen = 0;
	s->state = 0;
	s->row = -1;
	s->ntuples = 0;
}

static void clear_param(pg_stmt *s, int i)
{
	if(s->owned[i])
		free(s->values[i]);
	s->values[i] = NULL;
	s->owned[i] = false;
	s->lengths[i] = 0;
	s->formats[i] = 0;
}

// Unlink a statement, deallocate it on the server and free it
static void stmt_release(pg_stmt *s)
{
	pg_conn *c = s->conn;

	if(s->prev != NULL)
		s->prev->next = s->next;
	else
		c->stmts = s->next;
	if(s->next != NULL)
		s->next->prev = s->prev;

	stmt_clear_result(s);
	pthread_mutex_lock(&c->lock);
	char sql[64];
	snprintf(sql, sizeof(sql), "DEALLOCATE %s", s->name);
	PGresult *res = PQexec(c->pg, sql);
	PQclear(res);
	pthread_mutex_unlock(&c->lock);

	for(int i = 0; i < s->nparams; i++)
	{
		clear_param(s, i);
		free(s->names[i]);
	}
	for(int i = 0; i < s->ncols; i++)
		free(s->colnames[i]);
	free(s->colnames);
	free(s->names);
	free(s->values);
	free(s->lengths);
	free(s->formats);
	free(s->owned);
	free(s->sql);
	free(s);

	conn_unref(c);
}

static db_stmt *pg_prepare(db_conn *conn, const char *sql, bool persistent)
{
	(void)persistent;
	pg_conn *c = cn(conn);
	int nparams = 0;
	char **names = NULL;
	char *translated = translate_sql(sql, &nparams, &names);
	if(translated == NULL)
		return NULL;

	pg_stmt *s = calloc(1, sizeof(*s));
	if(s == NULL)
	{
		free(translated);
		free(names);
		return NULL;
	}

	pthread_mutex_lock(&c->lock);
	clear_error(c);
	snprintf(s->name, sizeof(s->name), "lz_%u", ++c->stmt_seq);
	PGresult *res = PQprepare(c->pg, s->name, translated, 0, NULL);
	free(translated);
	if(PQresultStatus(res) != PGRES_COMMAND_OK)
	{
		set_error(c, res);
		PQclear(res);
		pthread_mutex_unlock(&c->lock);
		for(int i = 0; i < nparams; i++)
			free(names[i]);
		free(names);
		free(s);
		return NULL;
	}
	PQclear(res);

	// Column names and count are known before the first step
	PGresult *desc = PQdescribePrepared(c->pg, s->name);
	if(PQresultStatus(desc) == PGRES_COMMAND_OK)
	{
		s->ncols = PQnfields(desc);
		s->colnames = calloc((size_t)(s->ncols > 0 ? s->ncols : 1), sizeof(char*));
		for(int i = 0; s->colnames != NULL && i < s->ncols; i++)
			s->colnames[i] = strdup(PQfname(desc, i));
	}
	PQclear(desc);
	pthread_mutex_unlock(&c->lock);

	s->base.drv = conn->drv;
	s->conn = c;
	s->sql = strdup(sql);
	s->nparams = nparams;
	s->names = names;
	s->values = calloc((size_t)(nparams > 0 ? nparams : 1), sizeof(char*));
	s->lengths = calloc((size_t)(nparams > 0 ? nparams : 1), sizeof(int));
	s->formats = calloc((size_t)(nparams > 0 ? nparams : 1), sizeof(int));
	s->owned = calloc((size_t)(nparams > 0 ? nparams : 1), sizeof(bool));
	s->row = -1;

	s->next = c->stmts;
	if(c->stmts != NULL)
		c->stmts->prev = s;
	c->stmts = s;
	c->refs++;
	return &s->base;
}

// Execute the statement and keep its result
static db_rc stmt_execute(pg_stmt *s)
{
	pg_conn *c = s->conn;
	stmt_clear_result(s);

	pthread_mutex_lock(&c->lock);
	clear_error(c);
	s->res = PQexecPrepared(c->pg, s->name, s->nparams, (const char *const*)s->values,
	                        s->lengths, s->formats, 0);
	const ExecStatusType status = PQresultStatus(s->res);
	db_rc rc = DB_OK;
	if(status == PGRES_TUPLES_OK)
	{
		s->ntuples = PQntuples(s->res);
		s->row = 0;
		if(s->ntuples > 0)
			s->state = 1;
		else
			s->state = 2;
		rc = s->state == 1 ? DB_ROW : DB_DONE;
	}
	else if(status == PGRES_COMMAND_OK || status == PGRES_EMPTY_QUERY)
	{
		c->changes = strtoll(PQcmdTuples(s->res), NULL, 10);
		s->state = 2;
		rc = DB_DONE;
	}
	else
	{
		set_error(c, s->res);
		rc = rc_of_error(c);
		if(rc == DB_OK)
			rc = DB_ERROR;
		PQclear(s->res);
		s->res = NULL;
		s->state = 0;
	}
	pthread_mutex_unlock(&c->lock);
	return rc;
}

static db_rc pg_step(db_stmt *stmt)
{
	pg_stmt *s = st(stmt);
	if(s->state == 0)
		return stmt_execute(s);

	if(s->state == 1)
	{
		if(++s->row < s->ntuples)
			return DB_ROW;
		s->state = 2;
		return DB_DONE;
	}

	// Stepping a finished statement starts it over, as SQLite does
	return stmt_execute(s);
}

static db_rc pg_reset(db_stmt *stmt)
{
	stmt_clear_result(st(stmt));
	return DB_OK;
}

static void pg_finalize(db_stmt *stmt)
{
	if(stmt != NULL)
		stmt_release(st(stmt));
}

static const char *pg_sql_text(db_stmt *stmt)
{
	return st(stmt)->sql;
}

/* ---- binding ---- */

static int pg_param_index(db_stmt *stmt, const char *name)
{
	pg_stmt *s = st(stmt);
	for(int i = 0; i < s->nparams; i++)
		if(s->names[i] != NULL && strcmp(s->names[i], name) == 0)
			return i + 1;
	return 0;
}

// Store an owned text value for parameter idx (1-based)
static db_rc set_owned(pg_stmt *s, int idx, char *value)
{
	if(idx < 1 || idx > s->nparams || value == NULL)
	{
		free(value);
		return DB_ERROR;
	}
	clear_param(s, idx - 1);
	s->values[idx - 1] = value;
	s->owned[idx - 1] = true;
	return DB_OK;
}

static db_rc pg_bind_int64(db_stmt *stmt, int idx, int64_t value)
{
	char buf[32];
	snprintf(buf, sizeof(buf), "%lld", (long long)value);
	return set_owned(st(stmt), idx, strdup(buf));
}

static db_rc pg_bind_int(db_stmt *stmt, int idx, int value)
{
	return pg_bind_int64(stmt, idx, value);
}

static db_rc pg_bind_double(db_stmt *stmt, int idx, double value)
{
	char buf[64];
	if(value != value)
		snprintf(buf, sizeof(buf), "NaN");
	else if(value > 1.7976931348623157e308)
		snprintf(buf, sizeof(buf), "Infinity");
	else if(value < -1.7976931348623157e308)
		snprintf(buf, sizeof(buf), "-Infinity");
	else
		snprintf(buf, sizeof(buf), "%.17g", value);
	return set_owned(st(stmt), idx, strdup(buf));
}

static db_rc pg_bind_text(db_stmt *stmt, int idx, const char *value)
{
	if(value == NULL)
		return set_owned(st(stmt), idx, NULL) == DB_OK ? DB_OK : DB_ERROR;
	return set_owned(st(stmt), idx, strdup(value));
}

static db_rc pg_bind_text_ref(db_stmt *stmt, int idx, const char *value)
{
	pg_stmt *s = st(stmt);
	if(idx < 1 || idx > s->nparams)
		return DB_ERROR;
	clear_param(s, idx - 1);
	s->values[idx - 1] = (char*)value;
	return DB_OK;
}

static db_rc pg_bind_blob(db_stmt *stmt, int idx, const void *data, size_t len)
{
	pg_stmt *s = st(stmt);
	if(idx < 1 || idx > s->nparams)
		return DB_ERROR;
	char *copy = malloc(len > 0 ? len : 1);
	if(copy == NULL)
		return DB_ERROR;
	memcpy(copy, data, len);
	clear_param(s, idx - 1);
	s->values[idx - 1] = copy;
	s->owned[idx - 1] = true;
	s->lengths[idx - 1] = (int)len;
	s->formats[idx - 1] = 1; // binary
	return DB_OK;
}

static db_rc pg_bind_null(db_stmt *stmt, int idx)
{
	pg_stmt *s = st(stmt);
	if(idx < 1 || idx > s->nparams)
		return DB_ERROR;
	clear_param(s, idx - 1);
	return DB_OK;
}

// An array parameter is sent as an array literal, "{1,2,3}", and used with
// "= ANY($n)". The element type comes from the comparison
static db_rc pg_bind_array(db_stmt *stmt, int idx, db_type type, const void *values, size_t count)
{
	if(type != DB_TYPE_INT && type != DB_TYPE_INT64 && type != DB_TYPE_DOUBLE && type != DB_TYPE_TEXT)
		return DB_ERROR;

	size_t cap = 32 + count * 32;
	if(type == DB_TYPE_TEXT)
		for(size_t i = 0; i < count; i++)
			cap += 2 * strlen(((const char *const*)values)[i]) + 4;

	char *lit = malloc(cap);
	if(lit == NULL)
		return DB_ERROR;
	size_t o = 0;
	lit[o++] = '{';
	for(size_t i = 0; i < count; i++)
	{
		if(i > 0)
			lit[o++] = ',';
		switch(type)
		{
			case DB_TYPE_INT:
				o += (size_t)snprintf(lit + o, cap - o, "%d", ((const int32_t*)values)[i]);
				break;
			case DB_TYPE_INT64:
				o += (size_t)snprintf(lit + o, cap - o, "%lld", (long long)((const int64_t*)values)[i]);
				break;
			case DB_TYPE_DOUBLE:
				o += (size_t)snprintf(lit + o, cap - o, "%.17g", ((const double*)values)[i]);
				break;
			default:
			{
				const char *text = ((const char *const*)values)[i];
				lit[o++] = '"';
				for(; *text != '\0'; text++)
				{
					if(*text == '"' || *text == '\\')
						lit[o++] = '\\';
					lit[o++] = *text;
				}
				lit[o++] = '"';
				break;
			}
		}
	}
	lit[o++] = '}';
	lit[o] = '\0';
	return set_owned(st(stmt), idx, lit);
}

/* ---- columns ---- */

static int pg_column_count(db_stmt *stmt)
{
	return st(stmt)->ncols;
}

static const char *pg_column_name(db_stmt *stmt, int idx)
{
	pg_stmt *s = st(stmt);
	return idx >= 0 && idx < s->ncols ? s->colnames[idx] : NULL;
}

// The value of the current row, NULL if there is none
static const char *cell(pg_stmt *s, int col)
{
	if(s->res == NULL || s->state != 1 || col < 0 || col >= PQnfields(s->res) || PQgetisnull(s->res, s->row, col))
		return NULL;
	return PQgetvalue(s->res, s->row, col);
}

static db_type pg_column_type(db_stmt *stmt, int idx)
{
	pg_stmt *s = st(stmt);
	if(cell(s, idx) == NULL)
		return DB_TYPE_NULL;

	switch(PQftype(s->res, idx))
	{
		case 16: // bool
		case 20: // int8
		case 21: // int2
		case 23: // int4
		case 26: // oid
			return DB_TYPE_INT64;
		case 700: // float4
		case 701: // float8
		case 1700: // numeric
			return DB_TYPE_DOUBLE;
		case 17: // bytea
			return DB_TYPE_BLOB;
		default:
			return DB_TYPE_TEXT;
	}
}

static int64_t pg_column_int64(db_stmt *stmt, int idx)
{
	pg_stmt *s = st(stmt);
	const char *v = cell(s, idx);
	if(v == NULL)
		return 0;
	if(PQftype(s->res, idx) == 16) // bool
		return v[0] == 't' ? 1 : 0;
	return strtoll(v, NULL, 10);
}

static int pg_column_int(db_stmt *stmt, int idx)
{
	return (int)pg_column_int64(stmt, idx);
}

static double pg_column_double(db_stmt *stmt, int idx)
{
	const char *v = cell(st(stmt), idx);
	return v != NULL ? strtod(v, NULL) : 0.0;
}

static const char *pg_column_text(db_stmt *stmt, int idx)
{
	return cell(st(stmt), idx);
}

static const void *pg_column_blob(db_stmt *stmt, int idx)
{
	pg_stmt *s = st(stmt);
	const char *v = cell(s, idx);
	if(v == NULL)
		return NULL;

	if(PQftype(s->res, idx) != 17)
		return v;

	free(s->blob);
	size_t len = 0;
	unsigned char *decoded = PQunescapeBytea((const unsigned char*)v, &len);
	if(decoded == NULL)
		return NULL;
	// PQunescapeBytea() memory is released with PQfreemem(), keep our own copy
	s->blob = malloc(len > 0 ? len : 1);
	if(s->blob != NULL)
		memcpy(s->blob, decoded, len);
	s->bloblen = len;
	PQfreemem(decoded);
	return s->blob;
}

static int pg_column_bytes(db_stmt *stmt, int idx)
{
	pg_stmt *s = st(stmt);
	if(cell(s, idx) == NULL)
		return 0;
	if(PQftype(s->res, idx) == 17)
	{
		pg_column_blob(stmt, idx);
		return (int)s->bloblen;
	}
	return PQgetlength(s->res, s->row, idx);
}

/* ---- result metadata and errors ---- */

static int64_t pg_changes(db_conn *conn)
{
	return cn(conn)->changes;
}

// There is no last_insert_rowid(): the value of the sequence that was used
// last on this connection stands in, and it is 0 when none was
static int64_t pg_last_insert_id(db_conn *conn)
{
	pg_conn *c = cn(conn);
	pthread_mutex_lock(&c->lock);
	PGresult *res = PQexec(c->pg, "SELECT lastval()");
	int64_t id = 0;
	if(PQresultStatus(res) == PGRES_TUPLES_OK && PQntuples(res) == 1)
		id = strtoll(PQgetvalue(res, 0, 0), NULL, 10);
	PQclear(res);
	pthread_mutex_unlock(&c->lock);
	return id;
}

static int pg_errcode(db_conn *conn)
{
	return cn(conn)->sqlstate;
}

static const char *pg_errmsg(db_conn *conn)
{
	return cn(conn)->errmsg;
}

static const char *pg_stmt_errmsg(db_stmt *stmt)
{
	return st(stmt)->conn->errmsg;
}

static const char *pg_stmt_errstr(db_stmt *stmt)
{
	return pg_errstr(st(stmt)->conn->sqlstate);
}

/* ---- transactions ---- */

// PostgreSQL has no BEGIN IMMEDIATE or EXCLUSIVE, all three are a BEGIN
static db_rc pg_begin(db_conn *conn, db_txmode mode)
{
	(void)mode;
	return pg_exec(conn, "BEGIN");
}

static db_rc pg_commit(db_conn *conn)
{
	return pg_exec(conn, "COMMIT");
}

static db_rc pg_rollback(db_conn *conn)
{
	return pg_exec(conn, "ROLLBACK");
}

static db_rc savepoint_command(db_conn *conn, const char *verb, const char *name)
{
	char *quoted = quote_ident(cn(conn), name);
	if(quoted == NULL)
		return DB_ERROR;
	const size_t len = strlen(verb) + strlen(quoted) + 2;
	char *sql = malloc(len);
	if(sql == NULL)
	{
		free(quoted);
		return DB_ERROR;
	}
	snprintf(sql, len, "%s %s", verb, quoted);
	const db_rc rc = pg_exec(conn, sql);
	free(sql);
	free(quoted);
	return rc;
}

static db_rc pg_savepoint(db_conn *conn, const char *name)
{
	return savepoint_command(conn, "SAVEPOINT", name);
}

static db_rc pg_release_savepoint(db_conn *conn, const char *name)
{
	return savepoint_command(conn, "RELEASE SAVEPOINT", name);
}

/* ---- multiple databases ---- */

static db_rc pg_reset_database(db_conn *conn)
{
	return pg_exec(conn, "DROP SCHEMA IF EXISTS public CASCADE; CREATE SCHEMA public");
}

// Schemas take the place of attached databases
static db_rc pg_copy_table(db_conn *conn, const char *src_alias, const char *dst_alias,
                           const char *table, const char *where)
{
	pg_conn *c = cn(conn);
	char *src = quote_ident(c, src_alias);
	char *dst = quote_ident(c, dst_alias);
	char *tbl = quote_ident(c, table);
	if(src == NULL || dst == NULL || tbl == NULL)
	{
		free(src);
		free(dst);
		free(tbl);
		return DB_ERROR;
	}

	const size_t len = strlen(src) + strlen(dst) + 2 * strlen(tbl) + (where != NULL ? strlen(where) : 0) + 96;
	char *sql = malloc(len);
	if(sql == NULL)
	{
		free(src);
		free(dst);
		free(tbl);
		return DB_ERROR;
	}
	if(where != NULL && *where != '\0')
		snprintf(sql, len, "INSERT INTO %s.%s SELECT * FROM %s.%s WHERE %s", dst, tbl, src, tbl, where);
	else
		snprintf(sql, len, "INSERT INTO %s.%s SELECT * FROM %s.%s", dst, tbl, src, tbl);
	const db_rc rc = pg_exec(conn, sql);
	free(sql);
	free(src);
	free(dst);
	free(tbl);
	return rc;
}

/* ---- schema and maintenance ---- */

// Run a query with text parameters and return the first cell as a string
// (malloc), NULL if there is no row or the query failed
static char *query_text(pg_conn *c, const char *sql, int nparams, const char *const *params)
{
	pthread_mutex_lock(&c->lock);
	PGresult *res = PQexecParams(c->pg, sql, nparams, NULL, params, NULL, NULL, 0);
	char *value = NULL;
	if(PQresultStatus(res) == PGRES_TUPLES_OK && PQntuples(res) > 0 && !PQgetisnull(res, 0, 0))
		value = strdup(PQgetvalue(res, 0, 0));
	PQclear(res);
	pthread_mutex_unlock(&c->lock);
	return value;
}

static bool query_true(pg_conn *c, const char *sql, int nparams, const char *const *params)
{
	char *value = query_text(c, sql, nparams, params);
	const bool yes = value != NULL && (value[0] == 't' || value[0] == '1');
	free(value);
	return yes;
}

static bool pg_table_exists(db_conn *conn, const char *table)
{
	const char *params[] = { table };
	return query_true(cn(conn), "SELECT to_regclass($1) IS NOT NULL", 1, params);
}

static bool pg_column_exists(db_conn *conn, const char *table, const char *column)
{
	const char *params[] = { table, column };
	return query_true(cn(conn),
	                  "SELECT EXISTS (SELECT 1 FROM information_schema.columns "
	                  "WHERE table_schema = ANY (current_schemas(false)) "
	                  "AND table_name = lower($1) AND column_name = lower($2))", 2, params);
}

#define VERSION_TABLE "lorentz_schema_version"

static int pg_get_schema_version(db_conn *conn)
{
	pg_conn *c = cn(conn);
	if(!pg_table_exists(conn, VERSION_TABLE))
		return 0;
	char *value = query_text(c, "SELECT version FROM " VERSION_TABLE " WHERE id = 1", 0, NULL);
	const int version = value != NULL ? atoi(value) : 0;
	free(value);
	return version;
}

static db_rc pg_set_schema_version(db_conn *conn, int version)
{
	pg_conn *c = cn(conn);
	db_rc rc = pg_exec(conn, "CREATE TABLE IF NOT EXISTS " VERSION_TABLE
	                         " (id INTEGER PRIMARY KEY CHECK (id = 1), version INTEGER NOT NULL)");
	if(rc != DB_OK)
		return rc;
	return exec_fmt(c, "INSERT INTO " VERSION_TABLE " (id, version) VALUES (1, %d) "
	                   "ON CONFLICT (id) DO UPDATE SET version = %d", version, version);
}

static int64_t pg_row_count(db_conn *conn, const char *table)
{
	pg_conn *c = cn(conn);
	char *quoted = quote_qualified(c, table);
	if(quoted == NULL)
		return -1;
	const size_t len = strlen(quoted) + 32;
	char *sql = malloc(len);
	if(sql == NULL)
	{
		free(quoted);
		return -1;
	}
	snprintf(sql, len, "SELECT count(*) FROM %s", quoted);
	char *value = query_text(c, sql, 0, NULL);
	const int64_t count = value != NULL ? strtoll(value, NULL, 10) : -1;
	free(value);
	free(sql);
	free(quoted);
	return count;
}

static db_rc pg_vacuum(db_conn *conn)
{
	return pg_exec(conn, "VACUUM");
}

static db_rc pg_optimize(db_conn *conn)
{
	return pg_exec(conn, "ANALYZE");
}

// PostgreSQL has no equivalent of PRAGMA integrity_check without an extension.
// This proves that the server answers
static db_rc pg_integrity_check(db_conn *conn, const char **message)
{
	if(message != NULL)
		*message = NULL;
	return pg_exec(conn, "SELECT 1");
}

/* ---- formatting ---- */

typedef struct {
	char *data;
	size_t len, cap;
} strbuf;

static bool sb_put(strbuf *b, const char *s, size_t n)
{
	if(b->len + n + 1 > b->cap)
	{
		size_t cap = b->cap == 0 ? 128 : b->cap;
		while(cap < b->len + n + 1)
			cap *= 2;
		char *grown = realloc(b->data, cap);
		if(grown == NULL)
			return false;
		b->data = grown;
		b->cap = cap;
	}
	memcpy(b->data + b->len, s, n);
	b->len += n;
	b->data[b->len] = '\0';
	return true;
}

static bool sb_escaped(strbuf *b, const char *s, char quote)
{
	for(; *s != '\0'; s++)
		if((*s == quote && !sb_put(b, &quote, 1)) || !sb_put(b, s, 1))
			return false;
	return true;
}

// printf with the escapes SQL builders of SQLite use: %q doubles single
// quotes, %Q also adds the quotes (NULL for a NULL pointer), %w doubles
// double quotes
static char *pg_vmprintf(const char *fmt, va_list ap)
{
	strbuf b = { NULL, 0, 0 };
	if(!sb_put(&b, "", 0))
		return NULL;

	for(const char *p = fmt; *p != '\0'; p++)
	{
		if(*p != '%')
		{
			if(!sb_put(&b, p, 1))
				goto fail;
			continue;
		}

		// %[flags][width][.precision][length]conversion
		char spec[32];
		size_t sl = 0;
		spec[sl++] = *p++;
		while(*p != '\0' && strchr("-+ #0", *p) != NULL && sl < sizeof(spec) - 8)
			spec[sl++] = *p++;
		if(*p == '*')
		{
			sl += (size_t)snprintf(spec + sl, sizeof(spec) - sl, "%d", va_arg(ap, int));
			p++;
		}
		while(*p != '\0' && isdigit((unsigned char)*p) && sl < sizeof(spec) - 8)
			spec[sl++] = *p++;
		if(*p == '.')
		{
			spec[sl++] = *p++;
			if(*p == '*')
			{
				sl += (size_t)snprintf(spec + sl, sizeof(spec) - sl, "%d", va_arg(ap, int));
				p++;
			}
			while(*p != '\0' && isdigit((unsigned char)*p) && sl < sizeof(spec) - 8)
				spec[sl++] = *p++;
		}
		char length[3] = { 0 };
		size_t ll = 0;
		while(*p != '\0' && strchr("hlLzjt", *p) != NULL && ll < 2)
			length[ll++] = *p++;
		const char conv = *p;
		if(conv == '\0')
			break;

		char tmp[512];
		int n = 0;
		switch(conv)
		{
			case '%':
				if(!sb_put(&b, "%", 1))
					goto fail;
				continue;
			case 'q':
			case 'Q':
			case 'w':
			{
				const char *arg = va_arg(ap, const char*);
				if(conv == 'Q')
				{
					if(arg == NULL)
					{
						if(!sb_put(&b, "NULL", 4))
							goto fail;
						continue;
					}
					if(!sb_put(&b, "'", 1) || !sb_escaped(&b, arg, '\'') || !sb_put(&b, "'", 1))
						goto fail;
				}
				else if(!sb_escaped(&b, arg != NULL ? arg : "(NULL)", conv == 'q' ? '\'' : '"'))
					goto fail;
				continue;
			}
			case 's':
			{
				const char *arg = va_arg(ap, const char*);
				strcpy(spec + sl, "s");
				if(arg == NULL)
					arg = "(null)";
				const int need = snprintf(NULL, 0, spec, arg);
				char *big = malloc((size_t)need + 1);
				if(big == NULL)
					goto fail;
				snprintf(big, (size_t)need + 1, spec, arg);
				const bool ok = sb_put(&b, big, (size_t)need);
				free(big);
				if(!ok)
					goto fail;
				continue;
			}
			case 'd': case 'i':
			case 'u': case 'x': case 'X': case 'o': case 'c':
				spec[sl] = '\0';
				strcat(spec, length);
				sl = strlen(spec);
				spec[sl++] = conv;
				spec[sl] = '\0';
				if(strcmp(length, "ll") == 0 || strcmp(length, "j") == 0)
					n = snprintf(tmp, sizeof(tmp), spec, va_arg(ap, long long));
				else if(strcmp(length, "l") == 0)
					n = snprintf(tmp, sizeof(tmp), spec, va_arg(ap, long));
				else if(strcmp(length, "z") == 0)
					n = snprintf(tmp, sizeof(tmp), spec, va_arg(ap, size_t));
				else if(strcmp(length, "t") == 0)
					n = snprintf(tmp, sizeof(tmp), spec, va_arg(ap, ptrdiff_t));
				else
					n = snprintf(tmp, sizeof(tmp), spec, va_arg(ap, int));
				break;
			case 'f': case 'F': case 'e': case 'E': case 'g': case 'G':
				spec[sl++] = conv;
				spec[sl] = '\0';
				n = snprintf(tmp, sizeof(tmp), spec, va_arg(ap, double));
				break;
			case 'p':
				spec[sl++] = 'p';
				spec[sl] = '\0';
				n = snprintf(tmp, sizeof(tmp), spec, va_arg(ap, void*));
				break;
			default:
				// Not a conversion we know, keep the text
				if(!sb_put(&b, spec, sl) || !sb_put(&b, &conv, 1))
					goto fail;
				continue;
		}
		if(n < 0 || (size_t)n >= sizeof(tmp) || !sb_put(&b, tmp, (size_t)n))
			goto fail;
	}
	return b.data;

fail:
	free(b.data);
	return NULL;
}

static char *pg_mprintf(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	char *s = pg_vmprintf(fmt, ap);
	va_end(ap);
	return s;
}

static void pg_mem_free(void *ptr)
{
	free(ptr);
}

/* ---- dialect ---- */

static const char *pg_dialect_placeholder(unsigned int n)
{
	static _Thread_local char buf[16];
	snprintf(buf, sizeof(buf), "$%u", n);
	return buf;
}

static const char *pg_dialect_now_expr(void)
{
	return "CAST(EXTRACT(EPOCH FROM now()) AS BIGINT)";
}

static const char *pg_dialect_autoincrement_pk(void)
{
	return "BIGSERIAL PRIMARY KEY";
}

static const char *pg_dialect_upsert_suffix(const char *conflict_col, const char *update_set)
{
	static _Thread_local char buf[512];
	if(update_set == NULL || *update_set == '\0')
		snprintf(buf, sizeof(buf), "ON CONFLICT (%s) DO NOTHING", conflict_col);
	else
		snprintf(buf, sizeof(buf), "ON CONFLICT (%s) DO UPDATE SET %s", conflict_col, update_set);
	return buf;
}

// GLOB has no equivalent: the patterns differ from LIKE, SIMILAR TO and
// regular expressions, so there is nothing to hand out
static const char *pg_dialect_glob_op(void)
{
	return NULL;
}

static const char *pg_dialect_regexp_op(void)
{
	return "~";
}

static int pg_dialect_in_list(char *buf, size_t size, const char *column, const char *bind_name)
{
	const int len = snprintf(buf, size, "%s = ANY(%s)", column, bind_name);
	return (len < 0 || (size_t)len >= size) ? -1 : len;
}

static const db_dialect pg_dialect = {
	.name = "postgres",
	.placeholder = pg_dialect_placeholder,
	.now_expr = pg_dialect_now_expr,
	.autoincrement_pk = pg_dialect_autoincrement_pk,
	.upsert_suffix = pg_dialect_upsert_suffix,
	.glob_op = pg_dialect_glob_op,
	.regexp_op = pg_dialect_regexp_op,
	.in_list = pg_dialect_in_list
};

/* ---- driver instance ---- */

const db_driver db_driver_postgres = {
	.name = "postgres",

	.init = pg_init,
	.shutdown = pg_shutdown,
	.version = pg_version,
	.set_log_callback = pg_set_log_callback,

	.open = pg_open,
	.close = pg_close,
	.close_deferred = pg_close_deferred,
	.interrupt = pg_interrupt,
	.is_readonly = pg_is_readonly,
	.set_busy_handler = pg_set_busy_handler,

	.prepare = pg_prepare,
	.step = pg_step,
	.reset = pg_reset,
	.finalize = pg_finalize,
	.exec = pg_exec,
	.sql_text = pg_sql_text,

	.param_index = pg_param_index,
	.bind_int = pg_bind_int,
	.bind_int64 = pg_bind_int64,
	.bind_double = pg_bind_double,
	.bind_text = pg_bind_text,
	.bind_text_ref = pg_bind_text_ref,
	.bind_blob = pg_bind_blob,
	.bind_null = pg_bind_null,
	.bind_array = pg_bind_array,

	.column_count = pg_column_count,
	.column_name = pg_column_name,
	.column_type = pg_column_type,
	.column_int = pg_column_int,
	.column_int64 = pg_column_int64,
	.column_double = pg_column_double,
	.column_text = pg_column_text,
	.column_blob = pg_column_blob,
	.column_bytes = pg_column_bytes,

	.changes = pg_changes,
	.last_insert_id = pg_last_insert_id,
	.errcode = pg_errcode,
	.extended_errcode = pg_errcode,
	.errmsg = pg_errmsg,
	.stmt_errmsg = pg_stmt_errmsg,
	.stmt_errstr = pg_stmt_errstr,
	.errstr = pg_errstr,
	.errname = pg_errname,
	.classify_error = pg_classify_error,

	.begin = pg_begin,
	.commit = pg_commit,
	.rollback = pg_rollback,
	.savepoint = pg_savepoint,
	.release_savepoint = pg_release_savepoint,

	.reset_database = pg_reset_database,
	.attach = NULL,
	.detach = NULL,
	.copy_table = pg_copy_table,
	.serialize = NULL,
	.deserialize = NULL,

	.table_exists = pg_table_exists,
	.column_exists = pg_column_exists,
	.get_schema_version = pg_get_schema_version,
	.set_schema_version = pg_set_schema_version,
	.row_count = pg_row_count,
	.vacuum = pg_vacuum,
	.optimize = pg_optimize,
	.integrity_check = pg_integrity_check,

	.mprintf = pg_mprintf,
	.vmprintf = pg_vmprintf,
	.mem_free = pg_mem_free,

	.dialect = &pg_dialect
};
