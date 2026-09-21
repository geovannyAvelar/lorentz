/* Lorentz: A black hole for Internet advertisements
*  (c) 2026 Pi-hole, LLC (https://pi-hole.net)
*  Network-wide ad blocking via your own hardware.
*
*  Lorentz Engine
*  Checks of the baseline schema, shared by the driver harnesses
*
*  This file is copyright under the latest version of the EUPL.
*  Please see LICENSE file for your rights under this license. */
#ifndef DB_SCHEMA_CHECKS_H
#define DB_SCHEMA_CHECKS_H

// Included by a harness after it defined CHECK() and scalar(). The functions
// only use the driver interface and SQL that means the same to SQLite and to
// PostgreSQL, so every driver has to pass them

#include "database/db-schema.h"

static const char *const baseline_tables[] = {
	"lorentz", "counters", "query_storage", "domain_by_id", "client_by_id", "forward_by_id",
	"addinfo_by_id", "message", "network", "network_addresses", "aliasclient", "session"
};

// The schema exists and has its rows
static void check_baseline_content(db_conn *db)
{
	for(unsigned int i = 0; i < sizeof(baseline_tables) / sizeof(baseline_tables[0]); i++)
		CHECK(db_table_exists(db, baseline_tables[i]));
	CHECK(db_table_exists(db, "queries")); // the view

	CHECK(scalar(db, "SELECT value FROM lorentz WHERE id = 0") == DB_SCHEMA_VERSION);
	CHECK(scalar(db, "SELECT value FROM lorentz WHERE id = 1") == 0);
	CHECK(scalar(db, "SELECT value FROM lorentz WHERE id = 2") > 1700000000);
	CHECK(scalar(db, "SELECT count(*) FROM lorentz WHERE description IS NOT NULL") == 3);
	CHECK(scalar(db, "SELECT count(*) FROM counters") == 2);
	CHECK(scalar(db, "SELECT sum(value) FROM counters") == 0);
	CHECK(db_column_exists(db, "query_storage", "reply_time") && db_column_exists(db, "query_storage", "ede"));
	CHECK(db_column_exists(db, "network", "aliasclient_id"));
	CHECK(db_column_exists(db, "session", "x_forwarded_for"));
	// The schema is created once
	const char *error = NULL;
	CHECK(!db_schema_baseline(db, &error) && error != NULL && strlen(error) > 0);
	CHECK(scalar(db, "SELECT value FROM lorentz WHERE id = 0") == DB_SCHEMA_VERSION);
}

// The tables behave the way the code that uses them expects
static void check_baseline_behaviour(db_conn *db)
{
	// Ids are assigned by the tables
	CHECK(db_exec(db, "INSERT INTO domain_by_id (domain) VALUES ('a.example'), ('b.example')") == DB_OK);
	CHECK(db_exec(db, "INSERT INTO client_by_id (ip, name) VALUES ('10.0.0.1', 'pc'), ('10.0.0.2', NULL)") == DB_OK);
	CHECK(db_exec(db, "INSERT INTO forward_by_id (forward) VALUES ('1.1.1.1#53')") == DB_OK);
	CHECK(db_exec(db, "INSERT INTO addinfo_by_id (type, content) VALUES (0, 'cname.example'), (1, 7)") == DB_OK);
	CHECK(scalar(db, "SELECT max(id) FROM domain_by_id") == 2);
	CHECK(scalar(db, "SELECT id FROM domain_by_id WHERE domain = 'b.example'") == 2);

	// The lookup tables refuse duplicates
	CHECK(db_exec(db, "INSERT INTO domain_by_id (domain) VALUES ('a.example')") == DB_CONSTRAINT);
	CHECK(db_exec(db, "INSERT INTO client_by_id (ip, name) VALUES ('10.0.0.1', 'pc')") == DB_CONSTRAINT);
	CHECK(db_exec(db, "INSERT INTO addinfo_by_id (type, content) VALUES (0, 'cname.example')") == DB_CONSTRAINT);
	CHECK(db_exec(db, "INSERT INTO domain_by_id (domain) VALUES (NULL)") == DB_CONSTRAINT);

	// Queries keep their fractional timestamps and read back through the view
	db_stmt *ins = db_prepare(db,
		"INSERT INTO query_storage (id, timestamp, type, status, domain, client, forward, additional_info, "
		"reply_type, reply_time, dnssec, list_id, ede) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)", true);
	CHECK(ins != NULL);
	if(ins != NULL)
	{
		db_bind_int64(ins, 1, 0);
		db_bind_double(ins, 2, 1700000010.25);
		db_bind_int(ins, 3, 1);
		db_bind_int(ins, 4, 9);
		db_bind_int(ins, 5, 1);
		db_bind_int(ins, 6, 1);
		db_bind_null(ins, 7);
		db_bind_int(ins, 8, 1); // addinfo row 1: a domain
		db_bind_int(ins, 9, 4);
		db_bind_double(ins, 10, 0.0125);
		db_bind_int(ins, 11, 1);
		db_bind_int(ins, 12, 7);
		db_bind_int(ins, 13, 15);
		CHECK(db_step(ins) == DB_DONE);
		db_reset(ins);
		db_bind_int64(ins, 1, 1);
		db_bind_double(ins, 2, 1700000020.5);
		db_bind_int(ins, 5, 2);
		db_bind_int(ins, 6, 2);
		db_bind_int(ins, 7, 1);
		db_bind_null(ins, 8);
		CHECK(db_step(ins) == DB_DONE);
		// An explicit id that exists is refused
		db_reset(ins);
		CHECK(db_step(ins) == DB_CONSTRAINT);
		db_finalize(ins);
	}
	CHECK(db_exec(db, "INSERT INTO query_storage (id, timestamp, type, status, domain, client) VALUES (2, NULL, 1, 1, 1, 1)") == DB_CONSTRAINT);

	db_stmt *sel = db_prepare(db, "SELECT timestamp, domain, client, forward, additional_info, reply_time, list_id, ede "
	                              "FROM queries ORDER BY id", false);
	CHECK(sel != NULL);
	if(sel != NULL)
	{
		CHECK(db_step(sel) == DB_ROW);
		CHECK(db_column_double(sel, 0) == 1700000010.25);
		CHECK(strcmp(db_column_text(sel, 1), "a.example") == 0);
		CHECK(strcmp(db_column_text(sel, 2), "10.0.0.1") == 0);
		CHECK(db_column_type(sel, 3) == DB_TYPE_NULL);
		CHECK(strcmp(db_column_text(sel, 4), "cname.example") == 0);
		CHECK(db_column_double(sel, 5) == 0.0125);
		CHECK(db_column_int(sel, 6) == 7 && db_column_int(sel, 7) == 15);
		CHECK(db_step(sel) == DB_ROW);
		CHECK(db_column_double(sel, 0) == 1700000020.5);
		CHECK(strcmp(db_column_text(sel, 1), "b.example") == 0);
		CHECK(strcmp(db_column_text(sel, 3), "1.1.1.1#53") == 0);
		CHECK(db_step(sel) == DB_DONE);
		db_finalize(sel);
	}
	CHECK(scalar(db, "SELECT count(*) FROM queries WHERE timestamp BETWEEN 1700000000 AND 1700000015") == 1);

	// The network table and its addresses: keys, defaults and the reference
	CHECK(db_exec(db, "INSERT INTO network (hwaddr, interface, firstSeen, lastQuery, numQueries) "
	                  "VALUES ('aa:bb:cc:dd:ee:ff', 'eth0', 100, 200, 5)") == DB_OK);
	CHECK(db_exec(db, "INSERT INTO network (hwaddr, interface, firstSeen, lastQuery, numQueries) "
	                  "VALUES ('aa:bb:cc:dd:ee:ff', 'eth0', 100, 200, 5)") == DB_CONSTRAINT);
	const int64_t network_id = scalar(db, "SELECT id FROM network WHERE hwaddr = 'aa:bb:cc:dd:ee:ff'");
	CHECK(network_id > 0);
	db_stmt *addr = db_prepare(db, "INSERT INTO network_addresses (network_id, ip, name) VALUES (?, ?, ?)", false);
	CHECK(addr != NULL);
	if(addr != NULL)
	{
		db_bind_int64(addr, 1, network_id);
		db_bind_text_ref(addr, 2, "192.168.1.5");
		db_bind_text_ref(addr, 3, "host.lan");
		CHECK(db_step(addr) == DB_DONE);
		db_reset(addr);
		db_bind_text_ref(addr, 2, "192.168.1.5"); // the address is unique
		CHECK(db_step(addr) == DB_CONSTRAINT);
		db_reset(addr);
		db_bind_int64(addr, 1, network_id + 1000); // there is no such device
		db_bind_text_ref(addr, 2, "192.168.1.6");
		CHECK(db_step(addr) == DB_CONSTRAINT);
		db_finalize(addr);
	}
	CHECK(scalar(db, "SELECT lastSeen FROM network_addresses WHERE ip = '192.168.1.5'") > 1700000000); // the default
	CHECK(db_exec(db, "DELETE FROM network WHERE id = (SELECT network_id FROM network_addresses LIMIT 1)") == DB_CONSTRAINT);

	// Messages hold what the message types put in their columns
	db_stmt *msg = db_prepare(db, "INSERT INTO message (timestamp, type, message, blob1, blob2, blob3) VALUES (?, ?, ?, ?, ?, ?)", false);
	CHECK(msg != NULL);
	if(msg != NULL)
	{
		db_bind_int64(msg, 1, 1700000000);
		db_bind_text_ref(msg, 2, "REGEX_WARNING");
		db_bind_text_ref(msg, 3, "bad regex");
		db_bind_text_ref(msg, 4, "(");
		db_bind_int(msg, 5, 42);
		db_bind_double(msg, 6, 0.75);
		CHECK(db_step(msg) == DB_DONE);
		db_finalize(msg);
	}
	db_stmt *m = db_prepare(db, "SELECT blob1, blob2, blob3, blob4 FROM message", false);
	CHECK(m != NULL && db_step(m) == DB_ROW);
	if(m != NULL)
	{
		CHECK(strcmp(db_column_text(m, 0), "(") == 0);
		CHECK(db_column_int(m, 1) == 42 && db_column_double(m, 2) == 0.75);
		CHECK(db_column_type(m, 3) == DB_TYPE_NULL);
		db_finalize(m);
	}
	CHECK(db_exec(db, "INSERT INTO message (timestamp, type, message) VALUES (1, NULL, 'x')") == DB_CONSTRAINT);

	// Sessions, with flags as 0 and 1
	db_stmt *sess = db_prepare(db, "INSERT INTO session (login_at, valid_until, remote_addr, user_agent, sid, csrf, "
	                               "tls_login, tls_mixed, app, cli, x_forwarded_for) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)", false);
	CHECK(sess != NULL);
	if(sess != NULL)
	{
		db_bind_int64(sess, 1, 1700000000);
		db_bind_int64(sess, 2, 1700000300);
		db_bind_text_ref(sess, 3, "10.0.0.1");
		db_bind_null(sess, 4);
		db_bind_text_ref(sess, 5, "sid");
		db_bind_text_ref(sess, 6, "csrf");
		db_bind_int(sess, 7, 1);
		db_bind_int(sess, 8, 0);
		db_bind_int(sess, 9, 1);
		db_bind_int(sess, 10, 0);
		db_bind_null(sess, 11);
		CHECK(db_step(sess) == DB_DONE);
		db_finalize(sess);
	}
	CHECK(scalar(db, "SELECT tls_login + app FROM session") == 2);
	CHECK(scalar(db, "SELECT tls_mixed + cli FROM session") == 0);

	CHECK(db_exec(db, "INSERT INTO aliasclient (name, comment) VALUES ('a', NULL)") == DB_OK);
	CHECK(scalar(db, "SELECT id FROM aliasclient") > 0);
}

#endif // DB_SCHEMA_CHECKS_H
