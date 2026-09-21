/* Lorentz: A black hole for Internet advertisements
*  (c) 2017 Pi-hole, LLC (https://pi-hole.net)
*  Network-wide ad blocking via your own hardware.
*
*  Lorentz Engine
*  API route prototypes
*
*  This file is copyright under the latest version of the EUPL.
*  Please see LICENSE file for your rights under this license. */
#ifndef ROUTES_H
#define ROUTES_H

// type cJSON
#include "webserver/cJSON/cJSON.h"
#include "webserver/http-common.h"
// regex_t
#include "regex_r.h"
// enum conf_type
#include "config/config.h"

// Common definitions
#define LOCALHOSTv4 "127.0.0.1"
#define LOCALHOSTv6 "::1"

// API router
int api_handler(struct mg_connection *conn, void *ignored);

// Statistic methods
int __attribute__((pure)) cmpdesc(const void *a, const void *b);
unsigned int get_active_clients(void);
int api_stats_summary(struct lorentz_conn *api);
int api_stats_query_types(struct lorentz_conn *api);
int api_stats_upstreams(struct lorentz_conn *api);
int api_stats_top_domains(struct lorentz_conn *api);
int api_stats_top_clients(struct lorentz_conn *api);
int api_stats_recentblocked(struct lorentz_conn *api);
cJSON *get_top_domains(struct lorentz_conn *api, const int count,
                       const bool blocked, const bool domains_only);
cJSON *get_top_clients(struct lorentz_conn *api, const int count,
                       const bool blocked, const bool clients_only,
                       const bool names_only, const bool ip_if_no_name);
cJSON *get_top_upstreams(struct lorentz_conn *api, const bool upstreams_only);

// History methods
int api_history(struct lorentz_conn *api);
int api_history_clients(struct lorentz_conn *api);

// History methods (database)
int api_history_database(struct lorentz_conn *api);
int api_history_database_clients(struct lorentz_conn *api);

// Query methods
int api_queries(struct lorentz_conn *api);
int api_queries_suggestions(struct lorentz_conn *api);
bool compile_filter_regex(struct lorentz_conn *api, const char *path, cJSON *json,
                          regex_t **regex, unsigned int *N_regex, int *ret);
void free_filter_regex(regex_t *regex, const unsigned int N_regex);

// Statistics methods (database)
int api_stats_database_top_items(struct lorentz_conn *api);
int api_stats_database_summary(struct lorentz_conn *api);
int api_stats_database_query_types(struct lorentz_conn *api);
int api_stats_database_upstreams(struct lorentz_conn *api);

// Info methods
int api_info_client(struct lorentz_conn *api);
int api_info_database(struct lorentz_conn *api);
int api_info_system(struct lorentz_conn *api);
int api_info_lorentz(struct lorentz_conn *api);
int api_info_host(struct lorentz_conn *api);
int api_info_sensors(struct lorentz_conn *api);
int api_info_version(struct lorentz_conn *api);
int api_info_messages_count(struct lorentz_conn *api);
int api_info_messages(struct lorentz_conn *api);
int api_info_metrics(struct lorentz_conn *api);
int api_info_login(struct lorentz_conn *api);
cJSON *read_sys_property(const char *path);
int get_system_obj(struct lorentz_conn *api, cJSON *system);
int get_sensors_obj(struct lorentz_conn *api, cJSON *sensors, const bool add_list);
int get_version_obj(struct lorentz_conn *api, cJSON *version);

// Config methods
int api_config(struct lorentz_conn *api);
int api_config_properties(struct lorentz_conn *api);
int get_json_config(struct lorentz_conn *api, cJSON *json, const bool detailed);
cJSON *addJSONConfValue(const enum conf_type conf_type, union conf_value *val);

// Log methods
int api_logs(struct lorentz_conn *api);

// Network methods
int api_network_gateway(struct lorentz_conn *api);
int api_network_routes(struct lorentz_conn *api);
int api_network_interfaces(struct lorentz_conn *api);
int api_network_devices(struct lorentz_conn *api);
int api_client_suggestions(struct lorentz_conn *api);
int get_gateway(struct lorentz_conn *api, cJSON * json, const bool detailed);

// DNS methods
int api_dns_blocking(struct lorentz_conn *api);

// List methods
int api_list(struct lorentz_conn *api);
int api_group(struct lorentz_conn *api);

// Auth method
void init_api_sessions(void);
void free_api(void);
int check_client_auth(struct lorentz_conn *api, const bool is_api);
int api_auth(struct lorentz_conn *api);
void delete_all_sessions(void);
int api_auth_sessions(struct lorentz_conn *api);
int api_auth_session_delete(struct lorentz_conn *api);

// 2FA methods
enum totp_status {
	TOTP_INVALID,
	TOTP_CORRECT,
	TOTP_REUSED,
	TOTP_RATE_LIMIT
} __attribute__ ((packed));
enum totp_status verifyTOTP(const uint32_t code);
int generateTOTP(struct lorentz_conn *api);
int printTOTP(void);
int generateAppPw(struct lorentz_conn *api);

// Documentation methods
int api_docs(struct lorentz_conn *api);

// Teleporter methods
int api_teleporter(struct lorentz_conn *api);

// Action methods
int api_action_gravity(struct lorentz_conn *api);
int api_action_restartDNS(struct lorentz_conn *api);
int api_action_flush_logs(struct lorentz_conn *api);
int api_action_flush_network(struct lorentz_conn *api);

// Search methods
int api_search(struct lorentz_conn *api);

// DHCP methods
int api_dhcp_leases_GET(struct lorentz_conn *api);
int api_dhcp_leases_DELETE(struct lorentz_conn *api);

// PADD methods
int api_padd(struct lorentz_conn *api);

#endif // ROUTES_H
