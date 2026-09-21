/* Pi-hole: A black hole for Internet advertisements
*  (c) 2019 Pi-hole, LLC (https://pi-hole.net)
*  Network-wide ad blocking via your own hardware.
*
*  FTL Engine
*  pihole-FTL.db -> network tables prototypes
*
*  This file is copyright under the latest version of the EUPL.
*  Please see LICENSE file for your rights under this license. */
#ifndef NETWORKTABLE_H
#define NETWORKTABLE_H

#include "FTL.h"
#include "db-driver.h"

bool create_network_table(db_conn *db);
bool create_network_addresses_table(db_conn *db);
bool create_network_addresses_with_names_table(db_conn *db);
bool create_network_addresses_network_id_index(db_conn *db);
void parse_neighbor_cache(db_conn *db);
bool updateMACVendorRecords(db_conn *db);
bool unify_hwaddr(db_conn *db);
bool getMACfromIP(db_conn *db, char mac[MAXMACLEN], const char* ipaddr);
int getAliasclientIDfromIP(db_conn *db, const char *ipaddr);
bool getNameFromIP(db_conn *db, char hostn[MAXDOMAINLEN], const char* ipaddr);
bool getNameFromMAC(const char *client, char hostn[MAXDOMAINLEN]);
bool getIfaceFromIP(db_conn *db, char iface[MAXIFACESTRLEN], const char* ipaddr);
void resolveNetworkTableNames(void);
bool flush_network_table(void);
bool isMAC(const char *input) __attribute__ ((pure));

typedef struct {
	unsigned int id;
	const char *hwaddr;
	const char *iface;
	const char *macVendor;
	unsigned long numQueries;
	time_t firstSeen;
	time_t lastQuery;
} network_record;

bool networkTable_readDevices(db_conn *db, db_stmt **read_stmt, const char **message);
bool networkTable_readDevicesGetRecord(db_stmt *read_stmt, network_record *network, const char **message);
void networkTable_readDevicesFinalize(db_stmt *read_stmt);

typedef struct {
	const char *ip;
	const char *name;
	time_t lastSeen;
	time_t nameUpdated;
} network_addresses_record;

bool networkTable_readIPs(db_conn *db, db_stmt **read_stmt, const int id, const char **message);
bool networkTable_readIPsGetRecord(db_stmt *read_stmt, network_addresses_record *network_addresses, const char **message);
void networkTable_readIPsFinalize(db_stmt *read_stmt);

bool networkTable_deleteDevice(db_conn *db, const int id, int *deleted, const char **message);

#endif //NETWORKTABLE_H
