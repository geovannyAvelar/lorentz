/* Lorentz: A black hole for Internet advertisements
*  (c) 2026 Pi-hole, LLC (https://pi-hole.net)
*  Network-wide ad blocking via your own hardware.
*
*  Lorentz Engine
*  Regression harness for the Teleporter database code
*
*  This file is copyright under the latest version of the EUPL.
*  Please see LICENSE file for your rights under this license. */

// The Teleporter keeps its database code in file-internal functions. Including
// the implementation files here makes them reachable; the harness is linked
// with --allow-multiple-definition so the copies built into this translation
// unit take the place of the ones in the Lorentz object set.
#include "zip/teleporter.c"
#include "api/teleporter.c"
#include "../test/db_layer_test.h"

static bool copy_file(const char *from, const char *to)
{
	FILE *in = fopen(from, "rb"), *out = fopen(to, "wb");
	if(in == NULL || out == NULL)
	{
		if(in != NULL) fclose(in);
		if(out != NULL) fclose(out);
		return false;
	}
	char buf[4096];
	size_t n;
	while((n = fread(buf, 1, sizeof(buf), in)) > 0)
		fwrite(buf, 1, n, out);
	fclose(in);
	fclose(out);
	return true;
}

static int64_t count(const char *path, const char *table)
{
	db_conn *db = db_open(path, DB_OPEN_READONLY);
	if(db == NULL)
		return -1;
	char sql[128];
	snprintf(sql, sizeof(sql), "SELECT count(*) FROM \"%s\"", table);
	const int64_t n = db_query_int(db, sql);
	db_close(db);
	return n;
}

void test_teleporter(void)
{
	// ---- database export and import (zip/teleporter.c) ----
	const char *source = db_test_make_gravity_db("tele-source.db");
	CHECK(source != NULL);
	if(source == NULL)
		return;

	void *image = NULL;
	size_t size = 0;
	CHECK(create_teleporter_database(source, gravity_tables, ArraySize(gravity_tables), &image, &size));
	CHECK(size > 100 && memcmp(image, "SQLite format 3", 15) == 0);

	// Importing replaces the tables of the destination with the image
	const char *dest = db_test_path("tele-dest.db");
	CHECK(copy_file(source, dest));
	db_conn *db = db_open(dest, DB_OPEN_READWRITE);
	CHECK(db != NULL);
	CHECK(db_exec(db, "DELETE FROM domainlist_by_group; DELETE FROM domainlist; DELETE FROM client_by_group; DELETE FROM client;") == DB_OK);
	db_close(db);
	CHECK(count(dest, "domainlist") == 0);

	char hint[ERRBUF_SIZE] = { 0 };
	const char *error = test_and_import_database(image, size, dest, gravity_tables, ArraySize(gravity_tables), hint);
	CHECK(error == NULL);
	if(error != NULL)
		fprintf(stderr, "import error: %s (%s)\n", error, hint);
	for(unsigned int i = 0; i < ArraySize(gravity_tables); i++)
		CHECK(count(dest, gravity_tables[i]) == count(source, gravity_tables[i]));
	CHECK(count(dest, "domainlist") > 0);
	db_free_buffer(image);

	// Lorentz database tables
	const char *lorentz = db_test_path("tele-lorentz.db");
	db = db_open(lorentz, DB_OPEN_READWRITE | DB_OPEN_CREATE);
	CHECK(db_exec(db, "CREATE TABLE message(id INTEGER PRIMARY KEY, x TEXT);"
	                  "INSERT INTO message(x) VALUES('a'),('b');"
	                  "CREATE TABLE aliasclient(id INTEGER);"
	                  "CREATE TABLE network(id INTEGER, hwaddr TEXT);"
	                  "INSERT INTO network VALUES(1,'aa');"
	                  "CREATE TABLE network_addresses(network_id INTEGER, ip TEXT);") == DB_OK);
	db_close(db);
	CHECK(create_teleporter_database(lorentz, lorentz_tables, ArraySize(lorentz_tables), &image, &size));
	const char *lorentz_dest = db_test_path("tele-lorentz-dest.db");
	CHECK(copy_file(lorentz, lorentz_dest));
	CHECK(test_and_import_database(image, size, lorentz_dest, lorentz_tables, ArraySize(lorentz_tables), hint) == NULL);
	CHECK(count(lorentz_dest, "message") == 2 && count(lorentz_dest, "network") == 1);
	db_free_buffer(image);

	// Rejected input, with an explanation for the user
	char junk[200];
	memset(junk, 0, sizeof(junk));
	memcpy(junk, "SQLite format 3", 15);
	memset(hint, 0, sizeof(hint));
	CHECK(test_and_import_database(junk, sizeof(junk), lorentz_dest, lorentz_tables, ArraySize(lorentz_tables), hint) != NULL);
	CHECK(strlen(hint) > 0);
	CHECK(test_and_import_database(junk, 50, lorentz_dest, lorentz_tables, ArraySize(lorentz_tables), hint) != NULL);
	CHECK(test_and_import_database("not a database at all, just text........................................"
	                               "........................................................................",
	                               120, lorentz_dest, lorentz_tables, ArraySize(lorentz_tables), hint) != NULL);

	// A table the source does not have cannot be exported
	CHECK(!create_teleporter_database(lorentz, gravity_tables, ArraySize(gravity_tables), &image, &size));

	// ---- JSON import of a Lorentz v5 Teleporter file (api/teleporter.c) ----
	const char *gravity = db_test_make_gravity_db("tele-json.db");
	CHECK(gravity != NULL);
	config.files.gravity.v.s = (char*)gravity;

	cJSON *json = cJSON_Parse("["
		"{\"id\":100,\"address\":\"https://a.example/list\",\"enabled\":1,\"date_added\":1,\"date_modified\":2,"
		 "\"comment\":\"c\",\"date_updated\":null,\"number\":3,\"invalid_domains\":0,\"status\":1},"
		"{\"id\":101,\"address\":\"https://b.example/list\",\"enabled\":0,\"date_added\":1,\"date_modified\":2,"
		 "\"comment\":null,\"date_updated\":5,\"number\":0,\"invalid_domains\":0,\"status\":2}]");
	CHECK(json != NULL);
	struct teleporter_files *adlists = &teleporter_v5_files[0];
	CHECK(strcmp(adlists->table_name, "adlist") == 0);
	CHECK(import_json_table(json, adlists));
	cJSON_Delete(json);

	db = db_open(gravity, DB_OPEN_READONLY);
	CHECK(db_query_int_str(db, "SELECT count(*) FROM adlist WHERE address = ?", "https://a.example/list") == 1);
	CHECK(db_query_int_str(db, "SELECT enabled FROM adlist WHERE address = ?", "https://b.example/list") == 0);
	CHECK(db_query_int(db, "SELECT count(*) FROM adlist WHERE id IN (100,101)") == 2);
	db_close(db);

	// Missing column and non-array input are refused
	cJSON *bad = cJSON_Parse("[{\"id\":1}]");
	CHECK(!import_json_table(bad, adlists));
	cJSON_Delete(bad);
	cJSON *object = cJSON_Parse("{}");
	CHECK(!import_json_table(object, adlists));
	cJSON_Delete(object);
}
