/* Lorentz: A black hole for Internet advertisements
*  (c) 2019 Pi-hole, LLC (https://pi-hole.net)
*  Network-wide ad blocking via your own hardware.
*
*  Lorentz Engine
*  gravity database prototypes
*
*  This file is copyright under the latest version of the EUPL.
*  Please see LICENSE file for your rights under this license. */
#ifndef GRAVITY_H
#define GRAVITY_H

// clients data structure
#include "datastructure.h"
// Definition of struct regexData
#include "regex_r.h"
// SQLite3
#include "database/db-driver.h"

// Table row record, not all fields are used by all tables
typedef struct {
	bool enabled;
	int type_int;
	int number;
	int invalid_domains;
	int abp_entries;
	int status;
	const char *name;
	const char *domain;
	const char *address;
	const char *type;
	const char *kind;
	const char *comment;
	const char *group_ids;
	const char *client;
	const char *item;
	cJSON *items;
	long id;
	time_t date_added;
	time_t date_modified;
	time_t date_updated;
} tablerow;

bool gravityDB_reopen(void);
void gravityDB_forked(void);
void gravityDB_reload_groups(clientsData *client);
bool gravityDB_prepare_client_statements(clientsData *client);
void gravityDB_close(void);
bool gravityDB_getTable(unsigned char list);
const char* gravityDB_getDomain(int *rowid);
char* get_client_names_from_ids(const char *group_ids) __attribute__ ((malloc));
void gravityDB_finalizeTable(void);
int gravityDB_count(const enum gravity_tables list, const bool total);
void check_inaccessible_adlists(void);
void check_restored_gravity(void);
bool gravity_updated(void);

cJSON *gen_abp_patterns(const char *domain);

// Stack-based ABP pattern structure used on the hot query path to avoid
// heap-allocating a cJSON array per cache-miss query. Only the suffix start
// offsets into the original domain are stored; the actual ABP strings are built
// on the fly right before the SQLite bind call.
#define ABP_MAX_SUFFIXES 128
struct abp_patterns {
	unsigned int offsets[ABP_MAX_SUFFIXES];
	unsigned int lengths[ABP_MAX_SUFFIXES];
	unsigned int count;
	bool generated;
};

enum db_result in_gravity(const char *domain, struct abp_patterns *abp, clientsData *client, const bool antigravity, int *domain_id);
enum db_result in_denylist(const char *domain, DNSCacheData *dns_cache, clientsData *client);
enum db_result in_allowlist(const char *domain, DNSCacheData *dns_cache, clientsData *client);

bool gravityDB_get_regex_client_groups(clientsData *client, const unsigned int numregex, const regexData *regex,
                                       const unsigned char type, const char* table);

// The addresses in the client table of the gravity database (IPs, ranges, MACs,
// interfaces), lower case, as an array of allocated strings, *count long. NULL
// on error. Free with gravityDB_free_client_addresses()
// A read-write connection of its own to the gravity database, for the updater
// (a server database gets its tables here if it has none yet). NULL on error,
// with a description in *message
db_conn *gravityDB_open_write(const char **message);
// The "updated" time of the gravity database as Lorentz last saw it, -1 before
// its first look
int64_t gravityDB_last_updated(void);
char **gravityDB_client_addresses(size_t *count);
void gravityDB_free_client_addresses(char **addresses, size_t count);
db_conn *gravityDB_open_RO(void);
void gravityDB_close_RO(db_conn *db);
bool gravityDB_readTable(db_conn *db, const enum gravity_list_type listtype, const char *filter,
                         const char **message, const bool exact, const char *ids,
                         db_stmt **stmt);
bool gravityDB_readTableGetRow(const enum gravity_list_type listtype, tablerow *row, const char **message,
                               db_stmt *stmt);
void gravityDB_readTableFinalize(db_stmt *stmt);
bool gravityDB_addToTable(const enum gravity_list_type listtype, tablerow *row,
                          const char **message, const enum http_method method);
bool gravityDB_delFromTable(const enum gravity_list_type listtype, const cJSON* array, unsigned int *deleted, const char **message);
bool gravityDB_edit_groups(const enum gravity_list_type listtype, cJSON *groups,
                           const tablerow *row, const char **message);

time_t gravity_last_updated(void);

void gravityDB_dump_perf_stats(void);

#endif //GRAVITY_H
