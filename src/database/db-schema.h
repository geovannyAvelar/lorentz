/* Lorentz: A black hole for Internet advertisements
*  (c) 2026 Pi-hole, LLC (https://pi-hole.net)
*  Network-wide ad blocking via your own hardware.
*
*  Lorentz Engine
*  Baseline schema of the long-term database
*  /src/database/db-schema.h
*
*  This file is copyright under the latest version of the EUPL.
*  Please see LICENSE file for your rights under this license. */
#ifndef DB_SCHEMA_H
#define DB_SCHEMA_H

#include "db-driver.h"

// Version of the long-term database schema created by db_schema_baseline().
// It has to be the version the migrations of a SQLite database end at
// (MEMDB_VERSION in query-table.h, checked when the database is initialized)
#define DB_SCHEMA_VERSION 23

// Create the current schema in an empty database in one step, instead of
// replaying the history of migrations. The DDL is written once and adapted to
// the driver through its dialect (auto-incrementing keys, the current time
// as a default), so it works for every driver. SQLite databases still take
// the migration path; drivers that have no history, such as PostgreSQL, start
// here.
//
// Everything happens in one transaction. Returns false on failure, with a
// description in *error (valid until the next call on this thread) when it is
// given. The database must not contain the tables yet.
bool db_schema_baseline(db_conn *db, const char **error);

// Bring a database of an older version up to DB_SCHEMA_VERSION with the
// migrations written for drivers that start from the baseline. There are none
// yet, this returns true only when the database is current. Future migrations
// have to run on every such driver, so they are written with the dialect hooks.
bool db_schema_migrate(db_conn *db, int from_version, const char **error);

#endif // DB_SCHEMA_H
