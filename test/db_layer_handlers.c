/* Pi-hole: A black hole for Internet advertisements
*  (c) 2026 Pi-hole, LLC (https://pi-hole.net)
*  Network-wide ad blocking via your own hardware.
*
*  FTL Engine
*  Regression harness for the API handlers that read the query databases
*
*  This file is copyright under the latest version of the EUPL.
*  Please see LICENSE file for your rights under this license. */

// The handlers are called directly with a hand-made request. They answer
// through send_http() and friends, which are replaced here by functions that
// keep the response body for inspection (this translation unit is linked with
// --allow-multiple-definition, the definitions below win over the webserver's).
// The handlers are included so that this file and the FTL object set do not
// need a running webserver.
#include "api/stats_database.c"
#include "api/queries.c"
#include "../test/db_layer_test.h"

static char response[256 * 1024];
static int response_code;
static char response_error[256];

int send_http(struct ftl_conn *api, const char *mime, const char *msg)
{
	(void)api; (void)mime;
	snprintf(response, sizeof(response), "%s", msg != NULL ? msg : "");
	response_code = 200;
	response_error[0] = '\0';
	return 200;
}

int send_http_code(struct ftl_conn *api, const char *mime, int code, const char *msg)
{
	(void)api; (void)mime;
	snprintf(response, sizeof(response), "%s", msg != NULL ? msg : "");
	response_code = code;
	return code;
}

int send_http_internal_error(struct ftl_conn *api)
{
	(void)api;
	response[0] = '\0';
	response_code = 500;
	return 500;
}

int send_json_error(struct ftl_conn *api, const int code, const char *key, const char *message, const char *hint)
{
	(void)api; (void)hint;
	response[0] = '\0';
	response_code = code;
	snprintf(response_error, sizeof(response_error), "%s: %s", key, message);
	return code;
}

int send_json_success(struct ftl_conn *api)
{
	(void)api;
	response_code = 200;
	return 200;
}

// Run a handler and parse its answer. The caller frees the returned object
static cJSON *call(int (*handler)(struct ftl_conn*), const char *query, enum api_flags flags)
{
	struct mg_request_info info;
	memset(&info, 0, sizeof(info));
	info.query_string = query;
	struct ftl_conn api = { .request = &info, .method = HTTP_GET, .now = double_time() };
	api.opts.flags = flags;

	response[0] = '\0';
	response_error[0] = '\0';
	response_code = 0;
	handler(&api);
	return response[0] != '\0' ? cJSON_Parse(response) : NULL;
}

static int array_size(cJSON *json, const char *key)
{
	cJSON *array = cJSON_GetObjectItemCaseSensitive(json, key);
	return cJSON_IsArray(array) ? cJSON_GetArraySize(array) : -1;
}

static double number(cJSON *json, const char *key)
{
	cJSON *item = cJSON_GetObjectItemCaseSensitive(json, key);
	return cJSON_IsNumber(item) ? item->valuedouble : -12345.0;
}

static const char *string(cJSON *json, const char *key)
{
	cJSON *item = cJSON_GetObjectItemCaseSensitive(json, key);
	return cJSON_IsString(item) ? item->valuestring : "";
}

static cJSON *item(cJSON *json, const char *array, int index)
{
	return cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(json, array), index);
}

// Six queries between 1700000010 and 1700001500:
//   id time    type status domain client
//   0  ...010  A    2      a.com  10.0.0.1 (pc)
//   1  ...020  A    1      b.com  10.0.0.1 (pc)   blocked by list 7
//   2  ...030  28   2      a.com  10.0.0.2
//   3  ...700  A    9      b.com  10.0.0.1        CNAME block, EDE 15
//   4  ...710  A    2      cname.net 10.0.0.2
//   5  ..1500  SOA  3      a.com  10.0.0.1
static void fill_queries(db_conn *db)
{
	CHECK(dbquery(db, "INSERT INTO domain_by_id VALUES (1,'a.com'),(2,'b.com'),(3,'cname.net')") == DB_OK);
	CHECK(dbquery(db, "INSERT INTO client_by_id VALUES (1,'10.0.0.1','pc'),(2,'10.0.0.2',NULL)") == DB_OK);
	CHECK(dbquery(db, "INSERT INTO forward_by_id VALUES (1,'1.1.1.1#53')") == DB_OK);
	CHECK(dbquery(db, "INSERT INTO addinfo_by_id VALUES (1,0,'cname.net')") == DB_OK);
	const char *rows[] = {
		"(0,1700000010.5,1,2,1,1,1,NULL,1,0.01,0,NULL,-1)",
		"(1,1700000020.5,1,1,2,1,NULL,NULL,4,0.02,1,7,-1)",
		"(2,1700000030.5,28,2,1,2,1,NULL,1,0.03,0,NULL,-1)",
		"(3,1700000700.5,1,9,2,1,NULL,1,4,0.04,0,NULL,15)",
		"(4,1700000710.5,1,2,3,2,1,NULL,1,NULL,0,NULL,-1)",
		"(5,1700001500.5,5,3,1,1,NULL,NULL,2,0.06,2,NULL,-1)"
	};
	for(unsigned int i = 0; i < ArraySize(rows); i++)
		CHECK(dbquery(db, "INSERT INTO query_storage(id,timestamp,type,status,domain,client,forward,"
		                  "additional_info,reply_type,reply_time,dnssec,list_id,ede) VALUES %s", rows[i]) == DB_OK);
}

static void test_stats_database(void)
{
	db_test_fresh_ftl_db("handlers-disk.db");
	db_conn *db = dbopen(false, false);
	CHECK(db != NULL);
	fill_queries(db);
	dbclose(&db);

	const char *window = "from=1700000000&until=1700002000";
	cJSON *json;

	// History: three 10-minute buckets
	json = call(api_history_database, window, 0);
	CHECK(json != NULL && response_code == 200);
	CHECK(array_size(json, "history") == 3);
	CHECK(number(item(json, "history", 0), "timestamp") == 1699999800);
	CHECK(number(item(json, "history", 0), "total") == 3);
	CHECK(number(item(json, "history", 0), "blocked") == 1);
	CHECK(number(item(json, "history", 0), "forwarded") == 2);
	CHECK(number(item(json, "history", 2), "cached") == 1);
	cJSON_Delete(json);

	// Missing parameters are a client error
	json = call(api_history_database, NULL, 0);
	CHECK(json == NULL && response_code == 400);
	CHECK(strstr(response_error, "bad_request") != NULL);

	// Top domains and clients, blocked or not
	json = call(api_stats_database_top_items, "from=1700000000&until=1700002000&count=5", API_DOMAINS);
	CHECK(json != NULL);
	CHECK(array_size(json, "domains") == 2);
	CHECK(strcmp(string(item(json, "domains", 0), "domain"), "a.com") == 0);
	CHECK(number(item(json, "domains", 0), "count") == 3);
	CHECK(number(json, "total_queries") == 6 && number(json, "blocked_queries") == 2);
	cJSON_Delete(json);

	json = call(api_stats_database_top_items, "from=1700000000&until=1700002000&blocked=true", API_DOMAINS);
	CHECK(json != NULL && array_size(json, "domains") == 1);
	CHECK(strcmp(string(item(json, "domains", 0), "domain"), "b.com") == 0);
	CHECK(number(item(json, "domains", 0), "count") == 2);
	cJSON_Delete(json);

	json = call(api_stats_database_top_items, window, 0);
	CHECK(json != NULL && array_size(json, "clients") == 2);
	CHECK(strcmp(string(item(json, "clients", 0), "ip"), "10.0.0.2") == 0);
	CHECK(number(item(json, "clients", 0), "count") == 2);
	CHECK(cJSON_IsNull(cJSON_GetObjectItemCaseSensitive(item(json, "clients", 0), "name")));
	cJSON_Delete(json);

	json = call(api_stats_database_top_items, "from=1700000000&until=1700002000&blocked=true", 0);
	CHECK(json != NULL && array_size(json, "clients") == 1);
	CHECK(strcmp(string(item(json, "clients", 0), "name"), "pc") == 0);
	cJSON_Delete(json);

	// Summary, per-client history, query types and upstreams
	json = call(api_stats_database_summary, window, 0);
	CHECK(json != NULL);
	CHECK(number(json, "sum_queries") == 6 && number(json, "sum_blocked") == 2);
	CHECK(number(json, "total_clients") == 2);
	cJSON_Delete(json);

	json = call(api_history_database_clients, window, 0);
	CHECK(json != NULL && array_size(json, "history") == 3);
	CHECK(cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(json, "clients"), "10.0.0.1") != NULL);
	cJSON_Delete(json);

	json = call(api_stats_database_query_types, window, 0);
	CHECK(json != NULL && cJSON_IsObject(cJSON_GetObjectItemCaseSensitive(json, "types")));
	cJSON_Delete(json);

	json = call(api_stats_database_upstreams, window, 0);
	CHECK(json != NULL && array_size(json, "upstreams") == 3);
	CHECK(number(json, "forwarded_queries") == 3);
	CHECK(number(json, "total_queries") == 6);
	cJSON_Delete(json);
}

static void test_queries(void)
{
	db_test_fresh_ftl_db("handlers-mem.db");
	CHECK(init_memory_database());
	fill_queries(get_memdb());

	cJSON *json;

	json = call(api_queries, NULL, 0);
	CHECK(json != NULL && response_code == 200);
	CHECK(array_size(json, "queries") == 6);
	CHECK(number(item(json, "queries", 0), "id") == 5); // newest first
	CHECK(strcmp(string(item(json, "queries", 0), "type"), "SOA") == 0);
	CHECK(strcmp(string(item(json, "queries", 0), "status"), "CACHE") == 0);
	CHECK(strcmp(string(item(json, "queries", 0), "domain"), "a.com") == 0);
	CHECK(cJSON_HasObjectItem(json, "recordsTotal") && cJSON_HasObjectItem(json, "cursor"));
	cJSON_Delete(json);

	json = call(api_queries, "length=2", 0);
	CHECK(json != NULL && array_size(json, "queries") == 2);
	cJSON_Delete(json);

	json = call(api_queries, "length=3&start=1", 0);
	CHECK(json != NULL && array_size(json, "queries") == 3);
	CHECK(number(item(json, "queries", 0), "id") == 4);
	CHECK(strcmp(string(item(json, "queries", 0), "upstream"), "1.1.1.1#53") == 0);
	cJSON_Delete(json);

	json = call(api_queries, "domain=a.com", 0);
	CHECK(json != NULL && array_size(json, "queries") == 3);
	cJSON_Delete(json);

	json = call(api_queries, "domain=*.com", 0);
	CHECK(json != NULL && array_size(json, "queries") == 5);
	cJSON_Delete(json);

	json = call(api_queries, "domain=zzz", 0);
	CHECK(json != NULL && array_size(json, "queries") == 0);
	CHECK(number(json, "recordsFiltered") == 0);
	cJSON_Delete(json);

	json = call(api_queries, "client_ip=10.0.0.1", 0);
	CHECK(json != NULL && array_size(json, "queries") == 4);
	cJSON_Delete(json);

	json = call(api_queries, "client_name=pc", 0);
	CHECK(json != NULL && array_size(json, "queries") == 4);
	cJSON_Delete(json);

	json = call(api_queries, "from=1700000000&until=1700000100", 0);
	CHECK(json != NULL && array_size(json, "queries") == 3);
	cJSON_Delete(json);

	json = call(api_queries, "status=GRAVITY", 0);
	CHECK(json != NULL && array_size(json, "queries") == 1);
	CHECK(number(item(json, "queries", 0), "id") == 1);
	CHECK(number(item(json, "queries", 0), "list_id") == 7);
	cJSON_Delete(json);

	json = call(api_queries, "dnssec=SECURE", 0);
	CHECK(json != NULL && array_size(json, "queries") == 1);
	CHECK(number(item(json, "queries", 0), "id") == 1);
	cJSON_Delete(json);

	json = call(api_queries, "reply=NXDOMAIN", 0);
	CHECK(json != NULL && array_size(json, "queries") == 1);
	CHECK(number(item(json, "queries", 0), "id") == 5);
	cJSON_Delete(json);

	// A cursor continues below the given database ID; a CNAME block carries
	// the domain that caused it and the extended DNS error
	json = call(api_queries, "cursor=3&length=2", 0);
	CHECK(json != NULL && array_size(json, "queries") == 2);
	CHECK(number(item(json, "queries", 0), "id") == 3);
	CHECK(strcmp(string(item(json, "queries", 0), "status"), "GRAVITY_CNAME") == 0);
	CHECK(strcmp(string(item(json, "queries", 0), "cname"), "cname.net") == 0);
	CHECK(number(cJSON_GetObjectItemCaseSensitive(item(json, "queries", 0), "ede"), "code") == 15);
	cJSON_Delete(json);

	close_memory_database();
}

void test_api_handlers(void)
{
	test_stats_database();
	test_queries();
}
