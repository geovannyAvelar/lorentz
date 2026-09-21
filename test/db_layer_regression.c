/* Lorentz: A black hole for Internet advertisements
*  (c) 2026 Pi-hole, LLC (https://pi-hole.net)
*  Network-wide ad blocking via your own hardware.
*
*  Lorentz Engine
*  Regression harness for the modules built on the database driver layer
*
*  This file is copyright under the latest version of the EUPL.
*  Please see LICENSE file for your rights under this license. */

// Unlike db_driver_regression, this harness is linked with the complete Lorentz
// object set (see src/CMakeLists.txt) and drives the real modules: the common
// helpers, the in-memory query database, the gravity database, the message,
// session and network tables, gravity_parseList() and, in the other
// translation units, the Teleporter and the database API handlers.
//
// Everything runs against throw-away databases in a temporary directory.
// Shared memory is not initialized, so code that needs the DNS engine's
// shared-memory structures is out of scope here.

#include "lorentz.h"
#include "config/config.h"
#include "database/common.h"
#include "database/query-table.h"
#include "database/gravity-db.h"
#include "database/message-table.h"
#include "database/session-table.h"
#include "database/network-table.h"
#include "database/aliasclients.h"
#include "tools/gravity-parseList.h"
#include "api/auth.h"
#include "shmem.h"
#include "webserver/http-common.h"
#include "webserver/cJSON/cJSON.h"
#include "../test/db_layer_test.h"
#include "database/db-schema.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int db_test_checks = 0;
int db_test_failures = 0;

static char tmpdir[256];

const char *db_test_path(const char *name)
{
	static char buf[16][512];
	static unsigned int n = 0;
	char *p = buf[n++ % 16];
	snprintf(p, 512, "%s/%s", tmpdir, name);
	return p;
}

// Configuration the modules read. Everything else stays zero-initialized
static void configure(void)
{
	config.database.maxDBdays.v.ui = 365;
	config.database.DBimport.v.b = false; // importing needs the shared memory
	config.database.useWAL.v.b = true;
	config.webserver.session.restore.v.b = true;
	config.resolver.resolveIPv4.v.b = true;
	config.resolver.resolveIPv6.v.b = true;
	config.resolver.macNames.v.b = true;
}

static char db_path[512];

void db_test_fresh_lorentz_db(const char *name)
{
	snprintf(db_path, sizeof(db_path), "%s", db_test_path(name));
	remove(db_path);
	config.files.database.v.s = db_path;
	db_init();
}

// Create a gravity database from the schema and sample data the bats suite uses
static char gravity_path[512];
static bool make_gravity_db(const char *name)
{
	const char *candidates[] = { "test/gravity.db.sql", "../test/gravity.db.sql", "../../test/gravity.db.sql" };
	FILE *fp = NULL;
	for(unsigned int i = 0; i < ArraySize(candidates) && fp == NULL; i++)
		fp = fopen(candidates[i], "r");
	if(fp == NULL)
	{
		fprintf(stderr, "cannot find test/gravity.db.sql, run the harness from the repository root\n");
		return false;
	}
	fseek(fp, 0, SEEK_END);
	const long size = ftell(fp);
	rewind(fp);
	char *sql = calloc((size_t)size + 1, 1);
	if(sql == NULL || fread(sql, 1, (size_t)size, fp) != (size_t)size)
	{
		fclose(fp);
		free(sql);
		return false;
	}
	fclose(fp);

	snprintf(gravity_path, sizeof(gravity_path), "%s", db_test_path(name));
	remove(gravity_path);
	db_conn *db = db_open(gravity_path, DB_OPEN_READWRITE | DB_OPEN_CREATE);
	const bool okay = db != NULL && db_exec(db, sql) == DB_OK;
	db_close(db);
	free(sql);
	return okay;
}

const char *db_test_make_gravity_db(const char *name)
{
	return make_gravity_db(name) ? gravity_path : NULL;
}

/* ---- common helpers (database/common.c) ---- */

void test_common_helpers(void)
{
	db_test_fresh_lorentz_db("common.db");
	CHECK(!LorentzDBerror());

	db_conn *c = dbopen(false, false);
	CHECK(c != NULL);
	if(c == NULL)
		return;

	// db_init() created and migrated the database
	CHECK(db_get_int(c, DB_VERSION) >= 20);
	CHECK(db_table_exists(c, "query_storage"));
	CHECK(db_table_exists(c, "queries"));
	CHECK(db_table_exists(c, "session"));
	CHECK(db_table_exists(c, "message"));
	CHECK(db_table_exists(c, "network"));
	CHECK(db_table_exists(c, "network_addresses"));
	CHECK(db_table_exists(c, "aliasclient"));
	CHECK(strlen(get_sqlite3_version()) > 0);

	// Lorentz properties and counters
	CHECK(db_set_Lorentz_property(c, DB_LASTTIMESTAMP, 1234));
	CHECK(db_get_int(c, DB_LASTTIMESTAMP) == 1234);
	CHECK(db_set_Lorentz_property(c, DB_LASTTIMESTAMP, 5678)); // upsert
	CHECK(db_get_int(c, DB_LASTTIMESTAMP) == 5678);
	CHECK(db_set_counter(c, DB_TOTALQUERIES, 41));
	CHECK(db_update_disk_counter(c, DB_TOTALQUERIES, 1));
	CHECK(db_query_int(c, "SELECT value FROM counters WHERE id = 0") == 42);
	CHECK(db_set_counter(c, DB_BLOCKEDQUERIES, 7));
	CHECK(db_query_int(c, "SELECT value FROM counters WHERE id = 1") == 7);

	// The db_query_* family
	CHECK(db_query_int_int(c, "SELECT value FROM counters WHERE id = ?", 0) == 42);
	CHECK(db_query_int_str(c, "SELECT value FROM counters WHERE id = ?", "1") == 7);
	CHECK(db_query_int(c, "SELECT value FROM counters WHERE id = 99") == DB_NODATA);
	CHECK(db_query_int(c, "SELECT nope FROM nothing") == DB_FAILED);
	CHECK(db_query_double(c, "SELECT 1.5") == 1.5);
	CHECK(db_query_double(c, "SELECT nope FROM nothing") == DB_FAILED);
	CHECK(db_query_int_from_until(c, "SELECT CAST(? + ? AS INTEGER)", 1.0, 2.0) == 3);
	CHECK(db_query_int_from_until_type(c, "SELECT CAST(? + ? + ? AS INTEGER)", 1.0, 2.0, 3) == 6);
	CHECK(db_query_int(NULL, "SELECT 1") == DB_FAILED);

	// dbquery(): formatting, success and failure return codes
	CHECK(dbquery(c, "CREATE TABLE zz(a)") == DB_OK);
	CHECK(dbquery(c, "CREATE TABLE zz(a)") == DB_ERROR);
	CHECK(dbquery(c, "INSERT INTO zz VALUES('%s')", "it''s") == DB_OK);
	CHECK(dbquery(c, "INSERT INTO zz VALUES(%d)", 3) == DB_OK);
	CHECK(dbquery(NULL, "SELECT 1") == DB_ERROR);
	CHECK(get_row_count("zz", false) == 2);
	CHECK(get_row_count("nosuchtable", false) == -3);

	// Read-only connections
	db_conn *ro = dbopen(true, false);
	CHECK(ro != NULL && db_is_readonly(ro));
	CHECK(db_query_int(ro, "SELECT count(*) FROM zz") == 2);
	dbclose(&ro);
	CHECK(ro == NULL);

	dbclose(&c);
	CHECK(c == NULL);
	dbclose(&c); // closing twice is harmless
}

/* ---- in-memory query database (database/query-table.c) ---- */

void test_memory_database(void)
{
	db_test_fresh_lorentz_db("memdb.db");
	CHECK(init_memory_database());

	db_conn *m = get_memdb();
	CHECK(m != NULL && is_memdb(m));

	// The long-term database is attached as "disk" with the same schema
	CHECK(db_query_int(m, "SELECT count(*) FROM disk.query_storage") == 0);
	CHECK(db_query_int(m, "SELECT count(*) FROM query_storage") == 0);

	size_t memsize = 0;
	int queries = -1;
	CHECK(get_memdb_size(&memsize, &queries));
	CHECK(memsize > 0 && queries == 0);

	// Rows added to the in-memory table are exported to disk
	CHECK(dbquery(m, "INSERT INTO query_storage(id,timestamp,type,status,domain,client) VALUES(0,1700000000.5,1,2,1,1)") == DB_OK);
	CHECK(dbquery(m, "INSERT INTO query_storage(id,timestamp,type,status,domain,client) VALUES(1,1700000001.5,1,2,1,1)") == DB_OK);
	CHECK(get_row_count("query_storage", true) == 2);
	CHECK(export_queries_to_disk(true));
	CHECK(db_query_int(m, "SELECT count(*) FROM disk.query_storage") == 2);
	CHECK(get_row_count("query_storage", false) == 2);

	// Deleting old queries works on both databases
	CHECK(delete_old_queries_from_db(true, 1700000001.0));
	CHECK(db_query_int(m, "SELECT count(*) FROM query_storage") == 1);
	CHECK(delete_old_queries_from_db(false, 1700000001.0));
	CHECK(get_row_count("query_storage", false) == 1);

	// The shared connection is owned by this module: dbclose() refuses it
	db_conn *alias = m;
	dbclose(&alias);
	CHECK(alias == m);
	CHECK(get_memdb() == m);

	// Attach and detach on an ordinary connection
	db_conn *c = dbopen(false, false);
	CHECK(c != NULL);
	const char *msg = NULL;
	CHECK(attach_database(c, &msg, db_path, "again"));
	CHECK(db_query_int(c, "SELECT count(*) FROM again.query_storage") == 1);
	CHECK(detach_database(c, &msg, "again"));
	msg = NULL;
	CHECK(!attach_database(c, &msg, "/nonexistent/dir/x.db", "a3") && msg != NULL);
	dbclose(&c);

	interrupt_memdb();
	close_memory_database();
	CHECK(get_memdb() == NULL);
}

/* ---- gravity database (database/gravity-db.c) ---- */

extern bool gravityDB_opened;

void test_gravity_database(void)
{
	static countersStruct dummy_counters;
	counters = &dummy_counters;

	CHECK(make_gravity_db("gravity.db"));
	config.files.gravity.v.s = gravity_path;

	CHECK(gravityDB_reopen());
	CHECK(gravityDB_opened);

	const int deny0 = gravityDB_count(EXACT_DENY_TABLE, true);
	const int allow0 = gravityDB_count(EXACT_ALLOW_TABLE, true);
	CHECK(deny0 >= 1 && allow0 >= 1);
	CHECK(gravityDB_count(GROUPS_TABLE, true) >= 1);

	// Table cursor on the shared connection
	CHECK(gravityDB_getTable(EXACT_DENY_TABLE));
	int rowid = 0;
	const char *domain = gravityDB_getDomain(&rowid);
	int seen = 0;
	while(domain != NULL)
	{
		CHECK(rowid > 0);
		seen++;
		domain = gravityDB_getDomain(&rowid);
	}
	CHECK(rowid == -1 && seen == deny0);
	gravityDB_finalizeTable();

	// Read-only connection with its own cursor
	db_conn *ro = gravityDB_open_RO();
	CHECK(ro != NULL);
	db_stmt *stmt = NULL;
	const char *msg = NULL;
	tablerow row;
	memset(&row, 0, sizeof(row));
	CHECK(gravityDB_readTable(ro, GRAVITY_DOMAINLIST_ALL_EXACT, NULL, &msg, true, NULL, &stmt));
	int rows = 0;
	while(gravityDB_readTableGetRow(GRAVITY_DOMAINLIST_ALL_EXACT, &row, &msg, stmt))
	{
		CHECK(row.domain != NULL);
		rows++;
	}
	CHECK(rows == deny0 + allow0);
	gravityDB_readTableFinalize(stmt);

	// A cursor keeps working after the connection was released
	CHECK(gravityDB_readTable(ro, GRAVITY_DOMAINLIST_ALL_EXACT, NULL, &msg, true, NULL, &stmt));
	gravityDB_close_RO(ro);
	CHECK(gravityDB_readTableGetRow(GRAVITY_DOMAINLIST_ALL_EXACT, &row, &msg, stmt));
	gravityDB_readTableFinalize(stmt);

	// Searching by item and by id list
	CHECK(gravityDB_readTable(NULL, GRAVITY_DOMAINLIST_ALL_EXACT, "denied.lorentz", &msg, true, NULL, &stmt));
	rows = 0;
	while(gravityDB_readTableGetRow(GRAVITY_DOMAINLIST_ALL_EXACT, &row, &msg, stmt))
	{
		CHECK(strcmp(row.domain, "denied.lorentz") == 0);
		rows++;
	}
	CHECK(rows == 1);
	gravityDB_readTableFinalize(stmt);

	// Writes: add, refuse a duplicate, delete
	memset(&row, 0, sizeof(row));
	row.item = "new.example";
	row.enabled = true;
	row.comment = "added by the regression harness";
	CHECK(gravityDB_addToTable(GRAVITY_DOMAINLIST_ALLOW_EXACT, &row, &msg, HTTP_POST));
	CHECK(gravityDB_count(EXACT_ALLOW_TABLE, true) == allow0 + 1);
	msg = NULL;
	CHECK(!gravityDB_addToTable(GRAVITY_DOMAINLIST_ALLOW_EXACT, &row, &msg, HTTP_POST));
	CHECK(msg != NULL && strlen(msg) > 0);

	cJSON *array = cJSON_CreateArray();
	cJSON *entry = cJSON_CreateObject();
	cJSON_AddStringToObject(entry, "item", "new.example");
	cJSON_AddNumberToObject(entry, "type", 0);
	cJSON_AddItemToArray(array, entry);
	unsigned int deleted = 0;
	CHECK(gravityDB_delFromTable(GRAVITY_DOMAINLIST_ALLOW_EXACT, array, &deleted, &msg));
	CHECK(deleted == 1);
	cJSON_Delete(array);
	CHECK(gravityDB_count(EXACT_ALLOW_TABLE, true) == allow0);

	// Watching for a newer gravity: the first call only records the timestamp
	CHECK(!gravity_updated());
	CHECK(!gravity_updated());
	CHECK(gravity_last_updated() >= 0);
	check_restored_gravity();

	gravityDB_close();
	CHECK(!gravityDB_opened);
	gravityDB_close(); // closing twice is harmless
}

/* ---- message, session and network tables ---- */

void test_message_session_network(void)
{
	db_test_fresh_lorentz_db("tables.db");
	CHECK(init_memory_database());

	// Messages
	CHECK(count_messages() == 0);
	logg_fatal_dnsmasq_message("boom");
	logg_regex_warning("deny", "bad regex", 1, "(");
	log_gravity_restored("ok");
	CHECK(count_messages() == 3);

	cJSON *array = cJSON_CreateArray();
	CHECK(format_messages(array));
	CHECK(cJSON_GetArraySize(array) == 3);
	cJSON *first = cJSON_GetArrayItem(array, 0);
	CHECK(cJSON_IsNumber(cJSON_GetObjectItem(first, "id")));
	CHECK(cJSON_IsString(cJSON_GetObjectItem(first, "plain")));
	CHECK(cJSON_IsString(cJSON_GetObjectItem(first, "html")));
	const int id = cJSON_GetObjectItem(first, "id")->valueint;

	cJSON *ids = cJSON_CreateArray();
	cJSON_AddItemToArray(ids, cJSON_CreateNumber(id));
	int deleted = 0;
	CHECK(delete_message(ids, &deleted));
	CHECK(deleted == 1);
	CHECK(count_messages() == 2);
	cJSON_Delete(ids);
	cJSON_Delete(array);
	CHECK(flush_message_table(get_memdb()));

	// Sessions survive a backup and restore through the database
	static struct session sessions[2], restored[2];
	memset(sessions, 0, sizeof(sessions));
	memset(restored, 0, sizeof(restored));
	sessions[0].used = true;
	sessions[0].app = true;
	sessions[0].login_at = time(NULL);
	sessions[0].valid_until = time(NULL) + 300;
	strcpy(sessions[0].remote_addr, "10.0.0.1");
	strcpy(sessions[0].user_agent, "regression-agent");
	strcpy(sessions[0].sid, "0123456789abcdefghijkl");
	strcpy(sessions[0].csrf, "ABCDEFGHIJKLMNOPQRSTUV");
	CHECK(backup_db_sessions(sessions, 2));
	CHECK(restore_db_sessions(restored, 2));
	CHECK(restored[0].used && restored[0].app);
	CHECK(strcmp(restored[0].remote_addr, "10.0.0.1") == 0);
	CHECK(strcmp(restored[0].user_agent, "regression-agent") == 0);
	CHECK(strcmp(restored[0].sid, sessions[0].sid) == 0);
	CHECK(!restored[1].used);

	// Network table: lookups by IP and MAC, device and address readers
	db_conn *c = dbopen(false, false);
	CHECK(c != NULL);
	CHECK(dbquery(c, "INSERT INTO network(id,hwaddr,interface,firstSeen,lastQuery,numQueries,macVendor) "
	                 "VALUES(1,'aa:bb:cc:dd:ee:ff','eth0',100,200,5,'ACME')") == DB_OK);
	CHECK(dbquery(c, "INSERT INTO network_addresses(network_id,ip,lastSeen,name,nameUpdated) "
	                 "VALUES(1,'192.168.1.5',200,'host.lan',150)") == DB_OK);
	CHECK(dbquery(c, "INSERT INTO network_addresses(network_id,ip,lastSeen,name,nameUpdated) "
	                 "VALUES(1,'192.168.1.6',201,NULL,0)") == DB_OK);

	char mac[MAXMACLEN] = { 0 }, name[MAXDOMAINLEN] = { 0 }, iface[MAXIFACESTRLEN] = { 0 };
	CHECK(getMACfromIP(NULL, mac, "192.168.1.5"));
	CHECK(strcmp(mac, "aa:bb:cc:dd:ee:ff") == 0);
	CHECK(getMACfromIP(c, mac, "192.168.1.6")); // explicit connection
	CHECK(!getMACfromIP(NULL, mac, "1.2.3.4"));
	CHECK(getNameFromIP(NULL, name, "192.168.1.5"));
	CHECK(strcmp(name, "host.lan") == 0);
	memset(name, 0, sizeof(name));
	CHECK(getNameFromMAC("aa:bb:cc:dd:ee:ff", name));
	CHECK(strcmp(name, "host.lan") == 0);
	CHECK(getIfaceFromIP(NULL, iface, "192.168.1.5"));
	CHECK(strcmp(iface, "eth0") == 0);
	CHECK(getAliasclientIDfromIP(c, "192.168.1.5") == -1);
	CHECK(unify_hwaddr(c));

	// MAC vendor lookup from a separate database
	db_conn *mv = db_open(db_test_path("macvendor.db"), DB_OPEN_READWRITE | DB_OPEN_CREATE);
	CHECK(mv != NULL);
	CHECK(db_exec(mv, "CREATE TABLE macvendor(mac TEXT PRIMARY KEY, vendor TEXT);"
	                  "INSERT INTO macvendor VALUES('AA:BB:CC','Vendor X');") == DB_OK);
	db_close(mv);
	config.files.macvendor.v.s = strdup(db_test_path("macvendor.db"));
	CHECK(db_exec(c, "UPDATE network SET macVendor = NULL") == DB_OK);
	CHECK(updateMACVendorRecords(c));
	CHECK(db_query_int_str(c, "SELECT count(*) FROM network WHERE macVendor = ?", "Vendor X") == 1);
	dbclose(&c);

	db_conn *ro = dbopen(true, false);
	CHECK(ro != NULL);
	db_stmt *devices = NULL, *ips = NULL;
	const char *msg = NULL;
	network_record device;
	network_addresses_record address;
	int ndevices = 0, naddresses = 0;
	CHECK(networkTable_readDevices(ro, &devices, &msg));
	while(networkTable_readDevicesGetRecord(devices, &device, &msg))
	{
		ndevices++;
		CHECK(device.id == 1);
		CHECK(strcmp(device.hwaddr, "aa:bb:cc:dd:ee:ff") == 0);
		CHECK(device.numQueries == 5);
		CHECK(strcmp(device.macVendor, "Vendor X") == 0);
	}
	networkTable_readDevicesFinalize(devices);
	CHECK(ndevices == 1);
	CHECK(networkTable_readIPs(ro, &ips, 1, &msg));
	while(networkTable_readIPsGetRecord(ips, &address, &msg))
		naddresses++;
	networkTable_readIPsFinalize(ips);
	CHECK(naddresses == 2);
	dbclose(&ro);

	c = dbopen(false, false);
	int removed = 0;
	CHECK(networkTable_deleteDevice(c, 1, &removed, &msg));
	CHECK(removed >= 1);
	dbclose(&c);
	CHECK(!getMACfromIP(NULL, mac, "192.168.1.5"));

	close_memory_database();
}

/* ---- gravity_parseList() (tools/gravity-parseList.c) ---- */

static bool write_list(const char *name)
{
	FILE *fp = fopen(db_test_path(name), "w");
	if(fp == NULL)
		return false;
	fputs("# comment line\n"
	      "example.com\n"
	      "UPPER.Example.ORG\n"
	      "trailing.dot.com.\n"
	      "0.0.0.0 hosts.format.net\n"
	      "127.0.0.1 localhost\n"
	      "||abp.example^\n"
	      "@@||allow.abp.example^\n"
	      "not_a_domain!!\n"
	      "ex ample.com\n"
	      "another-valid.io   # inline comment\n"
	      "xn--bcher-kva.example\n"
	      "-badstart.com\n", fp);
	for(unsigned int i = 0; i < 5000; i++)
		fprintf(fp, "bulk%u.example.com\n", i);
	fclose(fp);
	return true;
}

void test_gravity_parselist(void)
{
	CHECK(write_list("list.txt"));
	CHECK(make_gravity_db("parse.db"));
	const char *list = db_test_path("list.txt");

	db_conn *db = db_open(gravity_path, DB_OPEN_READONLY);
	CHECK(db != NULL);
	const int before = db_query_int(db, "SELECT count(*) FROM gravity");
	const int anti_before = db_query_int(db, "SELECT count(*) FROM antigravity");
	db_close(db);

	// Check-only mode reports but never touches the database
	CHECK(gravity_parseList(list, "", "-1", true, false) == EXIT_SUCCESS);
	db = db_open(gravity_path, DB_OPEN_READONLY);
	CHECK(db_query_int(db, "SELECT count(*) FROM gravity") == before);
	db_close(db);

	// Import into gravity
	CHECK(gravity_parseList(list, gravity_path, "1", false, false) == EXIT_SUCCESS);
	db = db_open(gravity_path, DB_OPEN_READONLY);
	CHECK(db != NULL);
	CHECK(db_query_int(db, "SELECT count(*) FROM gravity WHERE domain LIKE 'bulk%'") == 5000);
	CHECK(db_query_int_str(db, "SELECT count(*) FROM gravity WHERE domain = ?", "example.com") == 1);
	CHECK(db_query_int_str(db, "SELECT count(*) FROM gravity WHERE domain = ?", "upper.example.org") == 1);
	CHECK(db_query_int_str(db, "SELECT count(*) FROM gravity WHERE domain = ?", "trailing.dot.com") == 1);
	CHECK(db_query_int_str(db, "SELECT count(*) FROM gravity WHERE domain = ?", "hosts.format.net") == 1);
	CHECK(db_query_int_str(db, "SELECT count(*) FROM gravity WHERE domain = ?", "another-valid.io") == 1);
	CHECK(db_query_int_str(db, "SELECT count(*) FROM gravity WHERE domain = ?", "||abp.example^") == 1);
	CHECK(db_query_int_str(db, "SELECT count(*) FROM gravity WHERE domain = ?", "not_a_domain!!") == 0);
	CHECK(db_query_int_str(db, "SELECT count(*) FROM gravity WHERE domain = ?", "ex ample.com") == 0);
	CHECK(db_query_int(db, "SELECT count(*) FROM antigravity") == anti_before); // untouched in gravity mode
	CHECK(db_query_int(db, "SELECT value FROM info WHERE property = 'abp_domains'") == 1);
	CHECK(db_query_int(db, "SELECT number FROM adlist WHERE id = 1") == 5009);
	CHECK(db_query_int(db, "SELECT invalid_domains FROM adlist WHERE id = 1") == 3);
	CHECK(db_query_int(db, "SELECT abp_entries FROM adlist WHERE id = 1") == 1);
	db_close(db);

	// Import as antigravity: the "@@" allow patterns are the ABP entries
	CHECK(gravity_parseList(list, gravity_path, "1", false, true) == EXIT_SUCCESS);
	db = db_open(gravity_path, DB_OPEN_READONLY);
	CHECK(db_query_int(db, "SELECT count(*) FROM antigravity WHERE domain LIKE 'bulk%'") == 5000);
	CHECK(db_query_int_str(db, "SELECT count(*) FROM antigravity WHERE domain = ?", "@@||allow.abp.example^") == 1);
	db_close(db);

	// Errors: missing input, unwritable output
	CHECK(gravity_parseList(db_test_path("nosuchlist.txt"), gravity_path, "1", false, false) == EXIT_FAILURE);
	CHECK(gravity_parseList(list, "/nonexistent/dir/x.db", "1", false, false) == EXIT_FAILURE);
}


/* ---- schema: migrations against the baseline ---- */

// The affinity SQLite gives a declared type, which decides what it stores
static const char *affinity(const char *declared)
{
	char up[64];
	size_t i = 0;
	for(; declared != NULL && declared[i] != '\0' && i < sizeof(up) - 1; i++)
		up[i] = (char)toupper((unsigned char)declared[i]);
	up[i] = '\0';
	if(strstr(up, "INT") != NULL)
		return "integer";
	if(strstr(up, "CHAR") != NULL || strstr(up, "CLOB") != NULL || strstr(up, "TEXT") != NULL)
		return "text";
	if(up[0] == '\0' || strstr(up, "BLOB") != NULL)
		return "blob";
	if(strstr(up, "REAL") != NULL || strstr(up, "FLOA") != NULL || strstr(up, "DOUB") != NULL)
		return "real";
	return "numeric";
}

// name:affinity:required, sorted, one line per column of a table
static int describe_table(db_conn *db, const char *table, char out[64][128])
{
	char sql[160];
	snprintf(sql, sizeof(sql), "SELECT lower(name), type, \"notnull\" = 1 OR pk > 0 FROM pragma_table_info('%s')", table);
	db_stmt *s = db_prepare(db, sql, false);
	int n = 0;
	while(s != NULL && n < 64 && db_step(s) == DB_ROW)
		snprintf(out[n++], 128, "%s:%s:%s", db_column_text(s, 0), affinity(db_column_text(s, 1)), db_column_int(s, 2) ? "required" : "optional");
	db_finalize(s);
	for(int i = 1; i < n; i++)
		for(int j = i; j > 0 && strcmp(out[j - 1], out[j]) > 0; j--)
		{
			char tmp[128];
			strcpy(tmp, out[j]);
			strcpy(out[j], out[j - 1]);
			strcpy(out[j - 1], out[j]);
			strcpy(out[j - 1], tmp);
		}
	return n;
}

// Columns whose declared type differs on purpose between the history of a
// SQLite database and the baseline. SQLite stores whatever the code puts in
// them either way; the baseline needs a type a server accepts
static bool known_type_difference(const char *table, const char *migrated, const char *baseline)
{
	static const struct { const char *table, *migrated, *baseline; } known[] = {
		{ "lorentz", "value:blob:required", "value:integer:required" },
		{ "query_storage", "timestamp:integer:required", "timestamp:real:required" },
		{ "addinfo_by_id", "content:blob:required", "content:text:required" },
		{ "session", "login_at:numeric:required", "login_at:integer:required" },
		{ "session", "valid_until:numeric:required", "valid_until:integer:required" },
		{ "session", "tls_login:numeric:optional", "tls_login:integer:optional" },
		{ "session", "tls_mixed:numeric:optional", "tls_mixed:integer:optional" },
		{ "session", "app:numeric:optional", "app:integer:optional" },
		{ "session", "cli:numeric:optional", "cli:integer:optional" },
	};
	for(unsigned int i = 0; i < ArraySize(known); i++)
		if(strcmp(known[i].table, table) == 0 && strcmp(known[i].migrated, migrated) == 0 && strcmp(known[i].baseline, baseline) == 0)
			return true;
	// message.blob1..blob5 hold numbers, text and floating point in SQLite
	return strcmp(table, "message") == 0 && strstr(migrated, ":blob:optional") != NULL && strstr(baseline, ":text:optional") != NULL;
}

void test_schema_baseline(void)
{
	// The database the migrations of db_init() produce...
	db_test_fresh_lorentz_db("schema.db");
	db_conn *migrated = dbopen(true, false);
	// ...and the baseline in an empty one
	db_conn *baseline = db_open(":memory:", DB_OPEN_READWRITE | DB_OPEN_MEMORY);
	CHECK(migrated != NULL && baseline != NULL);
	const char *error = NULL;
	CHECK(baseline != NULL && db_schema_baseline(baseline, &error));
	if(migrated == NULL || baseline == NULL)
		return;

	static const char *tables[] = { "lorentz", "counters", "query_storage", "domain_by_id", "client_by_id", "forward_by_id",
	                                "addinfo_by_id", "message", "network", "network_addresses", "aliasclient", "session" };
	for(unsigned int t = 0; t < ArraySize(tables); t++)
	{
		static char a[64][128], b[64][128];
		const int na = describe_table(migrated, tables[t], a);
		const int nb = describe_table(baseline, tables[t], b);
		CHECK(na > 0 && na == nb);
		for(int i = 0; i < na && i < nb; i++)
		{
			// same column, same requirement, same kind of value (or a known difference)
			const bool same = strcmp(a[i], b[i]) == 0 || known_type_difference(tables[t], a[i], b[i]);
			CHECK(same);
			if(!same)
				fprintf(stderr, "  %s: migrated %s, baseline %s\n", tables[t], a[i], b[i]);
		}
	}

	// The same indexes and the view of the same shape
	CHECK(db_query_int(migrated, "SELECT count(*) FROM sqlite_master WHERE type = 'index' AND name NOT LIKE 'sqlite_%'") ==
	      db_query_int(baseline, "SELECT count(*) FROM sqlite_master WHERE type = 'index' AND name NOT LIKE 'sqlite_%'"));
	static char va[64][128], vb[64][128];
	CHECK(describe_table(migrated, "queries", va) == describe_table(baseline, "queries", vb));

	// The rows the migrations leave: properties with their descriptions and the counters
	CHECK(db_query_int(migrated, "SELECT value FROM lorentz WHERE id = 0") == DB_SCHEMA_VERSION);
	CHECK(db_query_int(migrated, "SELECT count(*) FROM lorentz WHERE description IS NOT NULL") ==
	      db_query_int(baseline, "SELECT count(*) FROM lorentz WHERE description IS NOT NULL"));
	CHECK(db_query_int(migrated, "SELECT count(*) FROM counters") == db_query_int(baseline, "SELECT count(*) FROM counters"));

	dbclose(&migrated);
	db_close(baseline);
}


/* ---- long-term database on PostgreSQL (needs POSTGRES_URL and a build with USE_POSTGRESQL) ---- */

void test_postgres_database(void)
{
	const char *url = getenv("POSTGRES_URL");
	if(url == NULL || *url == '\0' || db_driver_get("postgres") == NULL)
	{
		fprintf(stderr, "skipping the PostgreSQL tests: POSTGRES_URL is not set or the driver is not built in\n");
		return;
	}

	// The database of this test lives in its own schema, chosen through the
	// connection string
	CHECK(db_driver_select("postgres"));
	db_conn *setup = db_open(url, DB_OPEN_READWRITE);
	CHECK(setup != NULL);
	if(setup == NULL)
	{
		db_driver_select("sqlite");
		return;
	}
	CHECK(db_exec(setup, "DROP SCHEMA IF EXISTS lz_init CASCADE; CREATE SCHEMA lz_init") == DB_OK);
	db_close(setup);

	static char uri[1024];
	snprintf(uri, sizeof(uri), "%s%coptions=-c%%20search_path%%3Dlz_init", url, strchr(url, '?') != NULL ? '&' : '?');
	config.files.database.v.s = uri;

	// db_init() creates the current schema in one step
	db_init();
	CHECK(!LorentzDBerror());
	db_conn *db = dbopen(false, false);
	CHECK(db != NULL);
	if(db == NULL)
	{
		db_driver_select("sqlite");
		return;
	}
	CHECK(db_table_exists(db, "lorentz") && db_table_exists(db, "query_storage") && db_table_exists(db, "queries"));
	CHECK(db_table_exists(db, "session") && db_table_exists(db, "network_addresses") && db_table_exists(db, "message"));
	CHECK(db_get_int(db, DB_VERSION) == DB_SCHEMA_VERSION);
	CHECK(db_query_int(db, "SELECT count(*) FROM information_schema.tables WHERE table_schema = 'lz_init' AND table_type = 'BASE TABLE'") == 12);

	// The helpers of common.c work on it
	CHECK(db_set_Lorentz_property(db, DB_LASTTIMESTAMP, 1234));
	CHECK(db_get_int(db, DB_LASTTIMESTAMP) == 1234);
	CHECK(db_set_Lorentz_property(db, DB_LASTTIMESTAMP, 5678)); // upsert
	CHECK(db_get_int(db, DB_LASTTIMESTAMP) == 5678);
	CHECK(db_set_counter(db, DB_TOTALQUERIES, 41));
	CHECK(db_update_disk_counter(db, DB_TOTALQUERIES, 1));
	CHECK(db_set_counter(db, DB_BLOCKEDQUERIES, 7));
	CHECK(db_query_int(db, "SELECT value FROM counters WHERE id = 0") == 42);
	CHECK(db_query_int_int(db, "SELECT value FROM counters WHERE id = ?", 1) == 7);
	CHECK(db_query_int_str(db, "SELECT count(*) FROM lorentz WHERE description = ?", "Database version") == 1);
	CHECK(db_query_int(db, "SELECT value FROM counters WHERE id = 99") == DB_NODATA);
	CHECK(db_query_int(db, "SELECT nope FROM nothing") == DB_FAILED);
	CHECK(dbquery(db, "INSERT INTO domain_by_id (domain) VALUES ('%s')", "postgres.example") == DB_OK);
	CHECK(dbquery(db, "INSERT INTO domain_by_id (domain) VALUES ('postgres.example')") == DB_CONSTRAINT);
	CHECK(get_row_count("domain_by_id", false) == 1);
	CHECK(dbquery(db, "INSERT INTO query_storage (id, timestamp, type, status, domain, client) VALUES (0, 1700000000.5, 1, 2, 1, 1)") == DB_OK);
	CHECK(db_query_int_from_until(db, "SELECT count(*) FROM query_storage WHERE timestamp BETWEEN ? AND ?", 1700000000.0, 1700000001.0) == 1);
	CHECK(db_query_int_from_until_type(db, "SELECT count(*) FROM query_storage WHERE timestamp BETWEEN ? AND ? AND type = ?", 1700000000.0, 1700000001.0, 1) == 1);
	dbclose(&db);

	// Starting again finds the schema and keeps everything in it
	db_init();
	CHECK(!LorentzDBerror());
	db = dbopen(false, false);
	CHECK(db != NULL);
	if(db != NULL)
	{
		CHECK(db_get_int(db, DB_VERSION) == DB_SCHEMA_VERSION);
		CHECK(db_query_int(db, "SELECT value FROM counters WHERE id = 0") == 42);
		CHECK(get_row_count("query_storage", false) == 1);
		dbclose(&db);
	}

	setup = db_open(url, DB_OPEN_READWRITE);
	if(setup != NULL)
	{
		db_exec(setup, "DROP SCHEMA IF EXISTS lz_init CASCADE");
		db_close(setup);
	}
	db_driver_select("sqlite");
}

/* ---- main ---- */

int main(void)
{
	setvbuf(stdout, NULL, _IONBF, 0);
	snprintf(tmpdir, sizeof(tmpdir), "/tmp/db_layer_regression.XXXXXX");
	if(mkdtemp(tmpdir) == NULL)
	{
		perror("mkdtemp");
		return 2;
	}
	configure();

	test_common_helpers();
	test_memory_database();
	test_gravity_database();
	test_message_session_network();
	test_gravity_parselist();
	test_schema_baseline();
	test_teleporter();
	test_api_handlers();
	test_postgres_database();

	char cmd[300];
	snprintf(cmd, sizeof(cmd), "rm -rf '%s'", tmpdir);
	if(system(cmd) != 0)
		fprintf(stderr, "could not remove %s\n", tmpdir);

	printf("%d checks, %d failures\n", db_test_checks, db_test_failures);
	printf("DB_LAYER_REGRESSION=%s\n", db_test_failures == 0 ? "PASS" : "FAIL");
	return db_test_failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
