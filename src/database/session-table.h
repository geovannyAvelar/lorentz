/* Pi-hole: A black hole for Internet advertisements
*  (c) 2023 Pi-hole, LLC (https://pi-hole.net)
*  Network-wide ad blocking via your own hardware.
*
*  FTL Engine
*  Sessions table database prototypes
*
*  This file is copyright under the latest version of the EUPL.
*  Please see LICENSE file for your rights under this license. */
#ifndef SESSION_TABLE_PRIVATE_H
#define SESSION_TABLE_PRIVATE_H

#include "db-driver.h"
// struct session
#include "api/auth.h"

bool create_session_table(db_conn *db);
bool add_session_app_column(db_conn *db);
bool add_session_cli_column(db_conn *db);
bool add_session_x_forwarded_for_column(db_conn *db);
bool backup_db_sessions(struct session *sessions, const uint16_t max_sessions);
bool restore_db_sessions(struct session *sessions, const uint16_t max_sessions);

#endif // SESSION_TABLE_PRIVATE_H
