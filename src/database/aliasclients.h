/* Pi-hole: A black hole for Internet advertisements
*  (c) 2020 Pi-hole, LLC (https://pi-hole.net)
*  Network-wide ad blocking via your own hardware.
*
*  FTL Engine
*  pihole-FTL.db -> alias-clients tables prototypes
*
*  This file is copyright under the latest version of the EUPL.
*  Please see LICENSE file for your rights under this license. */
#ifndef ALIASCLIENTS_TABLE_H
#define ALIASCLIENTS_TABLE_H

// type clientsData
#include "datastructure.h"
#include "db-driver.h"


bool create_aliasclients_table(db_conn *db);
bool import_aliasclients(db_conn *db);
void reimport_aliasclients(db_conn *db);

void reset_aliasclient(db_conn *db, clientsData *client);

#endif //ALIASCLIENTS_TABLE_H
