/* Lorentz: A black hole for Internet advertisements
*  (c) 2026 Pi-hole, LLC (https://pi-hole.net)
*  Network-wide ad blocking via your own hardware.
*
*  Lorentz Engine
*  Gravity updater: downloads the adlists and rebuilds the blocked domains
*
*  This file is copyright under the latest version of the EUPL.
*  Please see LICENSE file for your rights under this license. */

// What "gravity" means here: the adlists are the URLs (or file:// paths) the
// user registered, gravity and antigravity the tables holding the domains those
// lists contain (the block lists and the allow lists). A run downloads every
// enabled list, parses it with the same code as `lorentz gravity parseList`
// (gravity-parseList.c) and replaces the rows of that list in one transaction.
// A list that cannot be downloaded, or that answers with nothing usable, is not
// touched: it keeps the domains of its last good download. When all lists have
// been seen, info.updated moves on, which is what makes Lorentz reload.

#include "lorentz.h"
#include "tools/gravity-update.h"
#include "tools/gravity-parseList.h"
#include "database/gravity-db.h"
#include "database/db-driver.h"
// db_query_int()
#include "database/common.h"
#include "config/config.h"
#include "log.h"
#include "signals.h"
// gravity_running
#include "daemon.h"
// git_version()
#include "version.h"
// thread_names
#include "enums.h"

#include <inttypes.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <sys/prctl.h>
#include <time.h>
#include <unistd.h>

#ifdef HAVE_LIBCURL
#include <curl/curl.h>
#endif

// Rows written by one INSERT. 500 rows are 1000 parameters, far below what
// either database accepts in a statement
#define BATCH_ROWS 500

// Seconds Lorentz waits after its start before the first scheduled run, for
// the DNS server and the network to be up
#define STARTUP_DELAY 60
// Seconds between two checks of the schedule
#define SCHEDULE_TICK 30
// Seconds to wait before a scheduled run that failed is tried again
#define RETRY_DELAY 3600
// Seconds to leave a run alone after one: Lorentz only learns of the new
// "updated" time on its next look at the database
#define AFTER_RUN_DELAY 300

// adlist.status, as gravity has always kept it
#define STATUS_UPDATED 1
#define STATUS_UNAVAILABLE 3
#define STATUS_UNAVAILABLE_CACHED 4

struct run {
	gravity_log_fn log;
	void *arg;
};

// Write a line to the log of Lorentz and to whoever is following the run
static void emit(const struct run *run, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void emit(const struct run *run, const char *fmt, ...)
{
	char line[1024];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(line, sizeof(line), fmt, ap);
	va_end(ap);

	log_info("gravity: %s", line);
	if(run->log != NULL)
		run->log(line, run->arg);
}

#ifndef HAVE_LIBCURL

bool gravity_update_available(void)
{
	return false;
}

bool gravity_update_running(void)
{
	return false;
}

enum gravity_update_rc gravity_update_run(gravity_log_fn log, void *arg, struct gravity_update_result *result)
{
	if(result != NULL)
		memset(result, 0, sizeof(*result));
	const struct run run = { .log = log, .arg = arg };
	emit(&run, "This build of Lorentz cannot download lists (built without libcurl)");
	return GRAVITY_UPDATE_UNAVAILABLE;
}

void *gravity_update_thread(void *arg)
{
	(void)arg;
	// Nothing to schedule
	return NULL;
}

#else // HAVE_LIBCURL

bool gravity_update_available(void)
{
	return true;
}

/* ---- Download ---- */

static int abort_on_shutdown(void *clientp, curl_off_t dltotal, curl_off_t dlnow, curl_off_t ultotal, curl_off_t ulnow)
{
	(void)clientp; (void)dltotal; (void)dlnow; (void)ultotal; (void)ulnow;
	return killed ? 1 : 0;
}

// Download url into out. Returns false with a description in err
static bool download(const char *url, FILE *out, char *err, const size_t err_size)
{
	CURL *curl = curl_easy_init();
	if(curl == NULL)
	{
		snprintf(err, err_size, "cannot start libcurl");
		return false;
	}

	char user_agent[128];
	snprintf(user_agent, sizeof(user_agent), "Lorentz/%s", git_version());
	char curl_error[CURL_ERROR_SIZE] = { 0 };

	curl_easy_setopt(curl, CURLOPT_URL, url);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, out);
	curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, curl_error);
	curl_easy_setopt(curl, CURLOPT_USERAGENT, user_agent);
	// A thread of a process that also runs a DNS server: no signals, please
	curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
	// An HTTP error is an error, not a list
	curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
	curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
	curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 10L);
	// A list is a web address or a file. A redirect may not lead to a file
#if LIBCURL_VERSION_NUM >= 0x075500
	curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "http,https,file");
	curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR, "http,https");
#else
	curl_easy_setopt(curl, CURLOPT_PROTOCOLS, (long)(CURLPROTO_HTTP | CURLPROTO_HTTPS | CURLPROTO_FILE));
	curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS, (long)(CURLPROTO_HTTP | CURLPROTO_HTTPS));
#endif
	// Lists are large, so no limit on the whole transfer, but a connection
	// that goes nowhere is given up: 20 s to connect, then at least 1 kB/s
	curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 20L);
	curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1024L);
	curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 30L);
	// Compressed transfer, whatever curl was built with
	curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
	// Stop when Lorentz shuts down
	curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
	curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, abort_on_shutdown);

	const CURLcode code = curl_easy_perform(curl);
	curl_easy_cleanup(curl);
	if(code != CURLE_OK)
	{
		snprintf(err, err_size, "%s", curl_error[0] != '\0' ? curl_error : curl_easy_strerror(code));
		return false;
	}
	return true;
}

/* ---- Writing the domains ---- */

struct batch {
	db_conn *db;
	const char *table;
	int adlist_id;
	char *domains[BATCH_ROWS];
	unsigned int n;
	db_stmt *full; // the statement for BATCH_ROWS rows, prepared on the first
};

static db_stmt *prepare_insert(struct batch *b, const unsigned int rows)
{
	char sql[64 + BATCH_ROWS * 8];
	int len = snprintf(sql, sizeof(sql), "INSERT INTO %s (domain, adlist_id) VALUES ", b->table);
	for(unsigned int i = 0; i < rows && len > 0 && (size_t)len < sizeof(sql) - 8; i++)
		len += snprintf(sql + len, sizeof(sql) - (size_t)len, i == 0 ? "(?,?)" : ",(?,?)");
	return db_prepare(b->db, sql, false);
}

static void batch_clear(struct batch *b)
{
	for(unsigned int i = 0; i < b->n; i++)
		free(b->domains[i]);
	b->n = 0;
}

static bool batch_flush(struct batch *b)
{
	if(b->n == 0)
		return true;

	const bool is_full = b->n == BATCH_ROWS;
	db_stmt *stmt = NULL;
	if(is_full)
	{
		if(b->full == NULL)
			b->full = prepare_insert(b, BATCH_ROWS);
		stmt = b->full;
	}
	else
		stmt = prepare_insert(b, b->n);
	if(stmt == NULL)
		return false;

	bool okay = true;
	for(unsigned int i = 0; i < b->n && okay; i++)
		okay = db_bind_text_ref(stmt, (int)(2 * i + 1), b->domains[i]) == DB_OK &&
		       db_bind_int(stmt, (int)(2 * i + 2), b->adlist_id) == DB_OK;
	okay = okay && db_step(stmt) == DB_DONE;

	if(is_full)
		db_reset(stmt);
	else
		db_finalize(stmt);
	batch_clear(b);
	return okay;
}

static bool add_domain(const char *domain, void *arg)
{
	struct batch *b = arg;
	if((b->domains[b->n] = strdup(domain)) == NULL)
		return false;
	b->n++;
	return b->n < BATCH_ROWS || batch_flush(b);
}

static bool table_has_rows(db_conn *db, const char *table, const int adlist_id)
{
	char sql[96];
	snprintf(sql, sizeof(sql), "SELECT 1 FROM %s WHERE adlist_id = ? LIMIT 1", table);
	db_stmt *stmt = db_prepare(db, sql, false);
	if(stmt == NULL)
		return false;
	bool has = db_bind_int(stmt, 1, adlist_id) == DB_OK && db_step(stmt) == DB_ROW;
	db_finalize(stmt);
	return has;
}

static bool set_status(db_conn *db, const int adlist_id, const int status)
{
	db_stmt *stmt = db_prepare(db, "UPDATE adlist SET status = ? WHERE id = ?", false);
	if(stmt == NULL)
		return false;
	const bool okay = db_bind_int(stmt, 1, status) == DB_OK && db_bind_int(stmt, 2, adlist_id) == DB_OK &&
	                  db_step(stmt) == DB_DONE;
	db_finalize(stmt);
	return okay;
}

// The rows of a list, once downloaded: replace what the list had by what it has
// now, in one transaction, so a reader sees the old rows or the new ones and a
// failure leaves the old ones. Returns false with a description in err
static bool store_list(db_conn *db, const int adlist_id, const bool allow, FILE *fp, const size_t fsize,
                       struct gravity_parse_stats *stats, char *err, const size_t err_size)
{
	struct batch batch = { .db = db, .table = allow ? "antigravity" : "gravity", .adlist_id = adlist_id };
	if(db_begin(db, DB_TX_DEFERRED) != DB_OK)
	{
		snprintf(err, err_size, "cannot start a transaction: %s", db_errmsg(db));
		return false;
	}

	char sql[80];
	snprintf(sql, sizeof(sql), "DELETE FROM %s WHERE adlist_id = ?", batch.table);
	db_stmt *del = db_prepare(db, sql, false);
	bool okay = del != NULL && db_bind_int(del, 1, adlist_id) == DB_OK && db_step(del) == DB_DONE;
	if(del != NULL)
		db_finalize(del);
	if(!okay)
	{
		snprintf(err, err_size, "cannot clear the old domains: %s", db_errmsg(db));
		db_rollback(db);
		return false;
	}

	struct gravity_parser parser = { .antigravity = allow, .add = add_domain, .arg = &batch };
	const int parsed = gravity_parse_stream(fp, fsize, &parser, stats);
	okay = parsed == 0 && batch_flush(&batch);
	batch_clear(&batch);
	if(batch.full != NULL)
		db_finalize(batch.full);
	if(!okay)
	{
		snprintf(err, err_size, "cannot store the domains: %s", db_errmsg(db));
		db_rollback(db);
		return false;
	}

	// An answer without a single domain is not a list (an error page a server
	// sent with a success status, an emptied file): it must not wipe out what
	// the list had, nor count as a list that was updated
	if(stats->exact + stats->abp == 0)
	{
		snprintf(err, err_size, "the download contains no domains");
		db_rollback(db);
		return false;
	}

	db_stmt *upd = db_prepare(db, "UPDATE adlist SET number = ?, invalid_domains = ?, abp_entries = ?, "
	                              "status = ?, date_updated = ? WHERE id = ?", false);
	okay = upd != NULL &&
	       db_bind_int(upd, 1, (int)(stats->exact + stats->abp)) == DB_OK &&
	       db_bind_int(upd, 2, (int)stats->invalid) == DB_OK &&
	       db_bind_int(upd, 3, (int)stats->abp) == DB_OK &&
	       db_bind_int(upd, 4, STATUS_UPDATED) == DB_OK &&
	       db_bind_int64(upd, 5, (int64_t)time(NULL)) == DB_OK &&
	       db_bind_int(upd, 6, adlist_id) == DB_OK &&
	       db_step(upd) == DB_DONE;
	if(upd != NULL)
		db_finalize(upd);
	if(!okay || db_commit(db) != DB_OK)
	{
		snprintf(err, err_size, "cannot finish the update: %s", db_errmsg(db));
		db_rollback(db);
		return false;
	}
	return true;
}

/* ---- One list ---- */

struct adlist {
	int id;
	bool allow;
	char *address;
};

// A temporary file for the download, in the directory gravity uses for that
static FILE *open_temp_file(void)
{
	const char *dirs[] = { config.files.gravity_tmp.v.s, "/tmp" };
	for(unsigned int i = 0; i < ArraySize(dirs); i++)
	{
		if(dirs[i] == NULL || dirs[i][0] == '\0')
			continue;
		char path[512];
		snprintf(path, sizeof(path), "%s/lorentz-adlist-XXXXXX", dirs[i]);
		const int fd = mkstemp(path);
		if(fd < 0)
			continue;
		// Only the open file is needed, it goes away with it
		unlink(path);
		FILE *fp = fdopen(fd, "w+");
		if(fp != NULL)
			return fp;
		close(fd);
	}
	return NULL;
}

// Returns true if the list was updated
static bool update_list(const struct run *run, db_conn *db, const struct adlist *list, uint64_t *domains)
{
	char err[512] = "";
	FILE *fp = open_temp_file();
	if(fp == NULL)
	{
		emit(run, "  Cannot create a temporary file for %s", list->address);
		return false;
	}

	bool okay = download(list->address, fp, err, sizeof(err)) && fflush(fp) == 0;
	if(!okay && err[0] == '\0')
		snprintf(err, sizeof(err), "cannot write the download");

	struct gravity_parse_stats stats = { 0 };
	if(okay)
	{
		fseek(fp, 0L, SEEK_END);
		const long size = ftell(fp);
		rewind(fp);
		okay = store_list(db, list->id, list->allow, fp, size > 0 ? (size_t)size : 0u, &stats, err, sizeof(err));
	}
	fclose(fp);

	if(okay)
	{
		emit(run, "  %s: %u %s domains (%u ABP-style), ignored %u non-domain entries",
		     list->address, stats.exact + stats.abp, list->allow ? "allowed" : "blocked", stats.abp, stats.invalid);
		*domains += stats.exact + stats.abp;
	}
	else
	{
		// Say how the list stands: it may still have what it had
		const bool cached = table_has_rows(db, list->allow ? "antigravity" : "gravity", list->id);
		set_status(db, list->id, cached ? STATUS_UNAVAILABLE_CACHED : STATUS_UNAVAILABLE);
		emit(run, "  %s: %s (%s)", list->address, err, cached ? "keeping the domains it had" : "no domains yet");
	}
	gravity_parse_stats_free(&stats);
	return okay;
}

/* ---- A run ---- */

static bool set_info(db_conn *db, const char *property, const char *value)
{
	db_stmt *stmt = db_prepare(db, "INSERT INTO info (property, value) VALUES (?, ?) "
	                               "ON CONFLICT(property) DO UPDATE SET value = excluded.value", false);
	if(stmt == NULL)
		return false;
	const bool okay = db_bind_text(stmt, 1, property) == DB_OK && db_bind_text(stmt, 2, value) == DB_OK &&
	                  db_step(stmt) == DB_DONE;
	db_finalize(stmt);
	return okay;
}

// What gravity says about itself: how many domains, whether any is ABP-style,
// and when it changed - the latter is what Lorentz watches for a reload
static bool finish(const struct run *run, db_conn *db)
{
	// Of the enabled lists only: what a disabled list still has stored does not block
	const int gravity = db_query_int(db, "SELECT COUNT(DISTINCT g.domain) FROM gravity g "
	                                     "JOIN adlist a ON a.id = g.adlist_id WHERE a.enabled = 1");
	const int antigravity = db_query_int(db, "SELECT COUNT(DISTINCT g.domain) FROM antigravity g "
	                                         "JOIN adlist a ON a.id = g.adlist_id WHERE a.enabled = 1");
	const int abp = db_query_int(db, "SELECT COUNT(*) FROM adlist WHERE enabled = 1 AND abp_entries > 0");

	// It has to move on, whatever the clock says
	int64_t now = (int64_t)time(NULL);
	const int64_t previous = gravityDB_last_updated();
	if(previous >= now)
		now = previous + 1;

	char value[32];
	bool okay = true;
	snprintf(value, sizeof(value), "%d", gravity > 0 ? gravity : 0);
	okay = set_info(db, "gravity_count", value) && okay;
	snprintf(value, sizeof(value), "%d", antigravity > 0 ? antigravity : 0);
	okay = set_info(db, "antigravity_count", value) && okay;
	okay = set_info(db, "abp_domains", abp > 0 ? "1" : "0") && okay;
	snprintf(value, sizeof(value), "%"PRId64, now);
	okay = set_info(db, "updated", value) && okay;

	if(!okay)
		emit(run, "Cannot store the result: %s", db_errmsg(db));
	else
		emit(run, "Blocking %d domains, allowing %d", gravity, antigravity);
	return okay;
}

// Lists are read into memory first: nothing writes to the database while its
// rows are being read
static struct adlist *read_lists(db_conn *db, unsigned int *count)
{
	*count = 0;
	db_stmt *stmt = db_prepare(db, "SELECT id, address, type FROM adlist WHERE enabled = 1 ORDER BY id", false);
	if(stmt == NULL)
		return NULL;

	unsigned int cap = 16;
	struct adlist *lists = calloc(cap, sizeof(*lists));
	while(lists != NULL && db_step(stmt) == DB_ROW)
	{
		if(*count == cap)
		{
			struct adlist *grown = realloc(lists, 2 * cap * sizeof(*lists));
			if(grown == NULL)
				break;
			lists = grown;
			cap *= 2;
		}
		const char *address = (const char*)db_column_text(stmt, 1);
		if(address == NULL || (lists[*count].address = strdup(address)) == NULL)
			continue;
		lists[*count].id = db_column_int(stmt, 0);
		lists[*count].allow = db_column_int(stmt, 2) == 1;
		(*count)++;
	}
	db_finalize(stmt);
	return lists;
}

static atomic_bool running = false;

bool gravity_update_running(void)
{
	return atomic_load(&running);
}

enum gravity_update_rc gravity_update_run(gravity_log_fn log, void *arg, struct gravity_update_result *result)
{
	struct gravity_update_result local = { 0 };
	if(result == NULL)
		result = &local;
	memset(result, 0, sizeof(*result));
	const struct run run = { .log = log, .arg = arg };

	if(atomic_exchange(&running, true))
	{
		emit(&run, "An update is already running");
		return GRAVITY_UPDATE_BUSY;
	}
	// Do not let a restart or a shutdown request cut a run in half
	gravity_running = 1;

	const char *message = NULL;
	db_conn *db = gravityDB_open_write(&message);
	if(db == NULL)
	{
		emit(&run, "Cannot open the gravity database: %s", message != NULL ? message : "unknown error");
		gravity_running = 0;
		atomic_store(&running, false);
		return GRAVITY_UPDATE_UNAVAILABLE;
	}

	// A file keeps its readers out of the way of the writer with a
	// write-ahead log, and needs an index to find the rows of one list
	if(!db_uri_is_remote(config.files.gravity.v.s))
	{
		db_exec(db, "PRAGMA journal_mode = WAL");
		db_exec(db, "CREATE INDEX IF NOT EXISTS idx_gravity_adlist ON gravity (adlist_id)");
		db_exec(db, "CREATE INDEX IF NOT EXISTS idx_antigravity_adlist ON antigravity (adlist_id)");
	}

	unsigned int count = 0;
	struct adlist *lists = read_lists(db, &count);
	result->lists = count;
	if(count == 0)
		emit(&run, "There is no enabled list to download");
	else
		emit(&run, "Downloading %u list%s", count, count == 1 ? "" : "s");

	for(unsigned int i = 0; i < count && !killed; i++)
	{
		if(update_list(&run, db, &lists[i], &result->domains))
			result->updated++;
		else
			result->failed++;
	}

	enum gravity_update_rc rc;
	if(killed)
	{
		emit(&run, "Stopped, Lorentz is shutting down");
		rc = GRAVITY_UPDATE_PARTIAL;
	}
	else
	{
		// Something new: let Lorentz know. When every list failed nothing
		// changed, and "updated" stays as it was so that the schedule
		// (gravity_update_thread) tries again soon
		if(result->updated > 0 || result->lists == 0)
			finish(&run, db);
		if(result->failed == 0)
			rc = GRAVITY_UPDATE_OK;
		else
			rc = result->updated > 0 ? GRAVITY_UPDATE_PARTIAL : GRAVITY_UPDATE_FAILED;
		emit(&run, "Done: %u updated, %u failed", result->updated, result->failed);
	}

	for(unsigned int i = 0; i < count; i++)
		free(lists[i].address);
	free(lists);
	db_close(db);
	gravity_running = 0;
	atomic_store(&running, false);
	return rc;
}

/* ---- The schedule ---- */

void *gravity_update_thread(void *arg)
{
	(void)arg;
	prctl(PR_SET_NAME, thread_names[GRAVITY], 0, 0, 0);

	// Once, before any thread of this process makes a request of its own
	curl_global_init(CURL_GLOBAL_DEFAULT);

	time_t next_attempt = time(NULL) + STARTUP_DELAY;
	// A run that left a list without a download is tried again after a while,
	// whatever "updated" says
	bool retry = false;
	unsigned int ticks = 0;
	while(!killed)
	{
		thread_sleepms(GRAVITY, 1000);
		if(killed)
			break;
		if(++ticks % SCHEDULE_TICK != 0)
			continue;

		const unsigned int hours = config.gravity.updateInterval.v.ui;
		const time_t now = time(NULL);
		if(hours == 0 || now < next_attempt)
			continue;

		// Lorentz has not looked at the database yet
		const int64_t last = gravityDB_last_updated();
		if(last < 0)
			continue;
		if(!retry && now - last < (int64_t)hours * 3600)
			continue;

		if(retry)
			log_info("gravity: Trying the lists that could not be updated again");
		else if(last == 0)
			log_info("gravity: The lists were never updated, updating them");
		else
			log_info("gravity: The lists were last updated %"PRId64" hours ago, updating them", (now - last) / 3600);
		struct gravity_update_result result;
		const enum gravity_update_rc rc = gravity_update_run(NULL, NULL, &result);
		next_attempt = time(NULL);
		retry = rc != GRAVITY_UPDATE_OK;
		switch(rc)
		{
			case GRAVITY_UPDATE_OK:
				next_attempt += AFTER_RUN_DELAY;
				break;
			case GRAVITY_UPDATE_BUSY:
				next_attempt += SCHEDULE_TICK;
				break;
			case GRAVITY_UPDATE_PARTIAL:
			case GRAVITY_UPDATE_FAILED:
			case GRAVITY_UPDATE_UNAVAILABLE:
			default:
				// Try again in an hour, or sooner if the interval is shorter
				next_attempt += (int64_t)hours * 3600 < RETRY_DELAY ? (int64_t)hours * 3600 : RETRY_DELAY;
				break;
		}
	}

	curl_global_cleanup();
	return NULL;
}

#endif // HAVE_LIBCURL
