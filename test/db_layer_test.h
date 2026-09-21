/* Lorentz: A black hole for Internet advertisements
*  (c) 2026 Pi-hole, LLC (https://pi-hole.net)
*  Network-wide ad blocking via your own hardware.
*
*  Lorentz Engine
*  Shared declarations of the database layer regression harness
*
*  This file is copyright under the latest version of the EUPL.
*  Please see LICENSE file for your rights under this license. */
#ifndef DB_LAYER_TEST_H
#define DB_LAYER_TEST_H

#include <stdio.h>

extern int db_test_checks;
extern int db_test_failures;

#define CHECK(cond) do { \
	db_test_checks++; \
	if(!(cond)) { \
		db_test_failures++; \
		fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
	} \
} while(0)

// Temporary directory of this run and a path inside it
const char *db_test_path(const char *name);

// Point Lorentz at a freshly created long-term database (runs db_init())
void db_test_fresh_lorentz_db(const char *name);

// Create a gravity database from test/gravity.db.sql, returns its path or NULL
const char *db_test_make_gravity_db(const char *name);

// Test groups, one per translation unit or topic
void test_common_helpers(void);
void test_memory_database(void);
void test_gravity_database(void);
void test_message_session_network(void);
void test_gravity_parselist(void);
void test_schema_baseline(void);
void test_postgres_database(void);
void test_users(void);
void test_teleporter(void);
void test_api_handlers(void);

#endif // DB_LAYER_TEST_H
