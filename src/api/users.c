/* Lorentz: A black hole for Internet advertisements
*  (c) 2026 Pi-hole, LLC (https://pi-hole.net)
*  Network-wide ad blocking via your own hardware.
*
*  Lorentz Engine
*  API Implementation /api/users
*
*  This file is copyright under the latest version of the EUPL.
*  Please see LICENSE file for your rights under this license. */

#include "lorentz.h"
#include "api/api.h"
#include "webserver/http-common.h"
#include "webserver/json_macros.h"
#include "log.h"
#include "config/config.h"
#include "database/user-table.h"
#include "database/common.h"

// Who is asking. An account has the role it has now, a login with the password
// of the configuration and an API that is open are admins
static bool current_account(const struct lorentz_conn *api, struct user *user)
{
	if(api->user_id < 0 || api->session.account_id == SESSION_NO_ACCOUNT)
		return false;

	enum user_role role;
	char name[USERNAME_MAX + 1];
	if(!user_cache_lookup(api->session.account_id, &role, name))
		return false;

	memset(user, 0, sizeof(*user));
	user->id = api->session.account_id;
	user->role = role;
	user->enabled = true;
	snprintf(user->username, sizeof(user->username), "%s", name);
	return true;
}

static bool is_admin(const struct lorentz_conn *api)
{
	struct user me;
	return !current_account(api, &me) || me.role == USER_ROLE_ADMIN;
}

// May the client request this endpoint with this method? Called for every
// request that needs authentication. A viewer reads statistics, lists and
// the like, and nothing else: no change, and nothing that holds secrets. The
// account endpoints decide for themselves, a viewer may change their own password
bool api_role_allows(const struct lorentz_conn *api, const char *endpoint)
{
	if(is_admin(api))
		return true;

	if(strcmp(endpoint, "/api/users") == 0)
		return api->method == HTTP_GET || api->method == HTTP_PUT;

	if(api->method != HTTP_GET)
		return false;

	static const char *const admin_only[] = {
		"/api/config", "/api/teleporter", "/api/logs", "/api/auth/sessions",
		"/api/auth/app", "/api/auth/totp", "/api/action", "/api/dhcp"
	};
	for(size_t i = 0; i < ArraySize(admin_only); i++)
		if(strncmp(endpoint, admin_only[i], strlen(admin_only[i])) == 0)
			return false;

	return true;
}

static cJSON *user_object(const struct user *user)
{
	cJSON *json = cJSON_CreateObject();
	if(json == NULL)
		return NULL;
	cJSON_AddNumberToObject(json, "id", (double)user->id);
	cJSON_AddStringToObject(json, "username", user->username);
	cJSON_AddStringToObject(json, "role", user_role_str(user->role));
	cJSON_AddBoolToObject(json, "enabled", user->enabled);
	if(user->comment[0] != '\0')
		cJSON_AddStringToObject(json, "comment", user->comment);
	else
		cJSON_AddNullToObject(json, "comment");
	cJSON_AddNumberToObject(json, "created_at", (double)user->created_at);
	cJSON_AddNumberToObject(json, "updated_at", (double)user->updated_at);
	if(user->last_login > 0)
		cJSON_AddNumberToObject(json, "last_login", (double)user->last_login);
	else
		cJSON_AddNullToObject(json, "last_login");
	return json;
}

static int send_users(struct lorentz_conn *api, const struct user *users, const size_t count, const int code)
{
	cJSON *json = JSON_NEW_OBJECT();
	cJSON *array = JSON_NEW_ARRAY();
	for(size_t i = 0; i < count; i++)
	{
		cJSON *item = user_object(&users[i]);
		if(item == NULL)
		{
			cJSON_Delete(json);
			cJSON_Delete(array);
			return send_json_error(api, 500, "internal_error", "Internal server error", NULL);
		}
		JSON_ADD_ITEM_TO_ARRAY(array, item);
	}
	JSON_ADD_ITEM_TO_OBJECT(json, "users", array);
	JSON_SEND_OBJECT_CODE(json, code);
}

// The error for a failed operation
static int send_user_error(struct lorentz_conn *api, const enum user_result result)
{
	switch(result)
	{
		case USER_NOT_FOUND:
			return send_json_error(api, 404, "not_found", user_result_str(result), NULL);
		case USER_EXISTS:
		case USER_LAST_ADMIN:
			return send_json_error(api, 409, "conflict", user_result_str(result), NULL);
		case USER_INVALID_NAME:
		case USER_INVALID_PASSWORD:
		case USER_INVALID_COMMENT:
			return send_json_error(api, 400, "bad_request", user_result_str(result), NULL);
		case USER_OK:
		case USER_ERROR:
		default:
			return send_json_error(api, 500, "database_error", "Could not access the users", NULL);
	}
}

static int send_forbidden(struct lorentz_conn *api)
{
	return send_json_error(api, 403, "forbidden", "Insufficient permissions", NULL);
}

// A string, or NULL if it is absent. Sets bad to a message if it is there and not a string
static const char *json_string_field(const cJSON *json, const char *key, const char **bad)
{
	const cJSON *item = cJSON_GetObjectItemCaseSensitive(json, key);
	if(item == NULL || cJSON_IsNull(item))
		return NULL;
	if(!cJSON_IsString(item))
	{
		*bad = key;
		return NULL;
	}
	return item->valuestring;
}

static int bad_field(struct lorentz_conn *api, const char *key, const char *type)
{
	char message[96];
	snprintf(message, sizeof(message), "Field %s has to be of type '%s'", key, type);
	return send_json_error(api, 400, "bad_request", message, NULL);
}

// Forget what was sent: the JSON holds passwords until the request is freed
static void wipe(cJSON *json, const char *key)
{
	cJSON *item = cJSON_GetObjectItemCaseSensitive(json, key);
	if(cJSON_IsString(item) && item->valuestring != NULL)
		memset(item->valuestring, 0, strlen(item->valuestring));
}

// The role and enabled fields of a payload. Returns 0 or the error that was sent
static int parse_role_enabled(struct lorentz_conn *api, struct user_changes *changes)
{
	const cJSON *role = cJSON_GetObjectItemCaseSensitive(api->payload.json, "role");
	if(role != NULL)
	{
		if(!cJSON_IsString(role))
			return bad_field(api, "role", "string");
		if(!user_role_parse(role->valuestring, &changes->role))
			return send_json_error(api, 400, "bad_request", "Field role has to be \"admin\" or \"viewer\"", NULL);
		changes->has_role = true;
	}
	const cJSON *enabled = cJSON_GetObjectItemCaseSensitive(api->payload.json, "enabled");
	if(enabled != NULL)
	{
		if(!cJSON_IsBool(enabled))
			return bad_field(api, "enabled", "boolean");
		changes->enabled = cJSON_IsTrue(enabled);
		changes->has_enabled = true;
	}
	return 0;
}

// GET /api/users
static int list_users(struct lorentz_conn *api)
{
	if(!is_admin(api))
		return send_forbidden(api);

	struct user *users = NULL;
	size_t count = 0;
	const enum user_result result = user_list(&users, &count);
	if(result != USER_OK)
		return send_user_error(api, result);

	const int ret = send_users(api, users, count, 200);
	free(users);
	return ret;
}

// POST /api/users
static int create_user(struct lorentz_conn *api)
{
	if(!is_admin(api))
		return send_forbidden(api);

	const int ret = check_json_payload(api);
	if(ret != 0)
		return ret;

	const char *bad = NULL;
	const char *username = json_string_field(api->payload.json, "username", &bad);
	const char *password = json_string_field(api->payload.json, "password", &bad);
	const char *comment = json_string_field(api->payload.json, "comment", &bad);
	if(bad != NULL)
		return bad_field(api, bad, "string");
	if(username == NULL)
		return send_json_error(api, 400, "bad_request", "No username found in JSON payload", NULL);
	if(password == NULL)
		return send_json_error(api, 400, "bad_request", "No password found in JSON payload", NULL);

	struct user_changes fields = { .role = USER_ROLE_VIEWER, .enabled = true };
	const int fret = parse_role_enabled(api, &fields);
	if(fret != 0)
		return fret;

	// While the API is open there is nobody to give the rights to a first account
	// but itself: one that could not manage the others would lock everybody out
	if(config.webserver.api.pwhash.v.s[0] == '\0' && users_enabled_count() == 0 &&
	   ((fields.has_role && fields.role != USER_ROLE_ADMIN) || !fields.has_role || (fields.has_enabled && !fields.enabled)))
		return send_json_error(api, 409, "conflict",
		                       "The first account has to be an enabled admin",
		                       "set \"role\": \"admin\"");

	struct user created;
	const enum user_result result = user_create(username, password,
	                                            fields.has_role ? fields.role : USER_ROLE_VIEWER,
	                                            comment, fields.has_enabled ? fields.enabled : true, &created);
	wipe(api->payload.json, "password");
	if(result != USER_OK)
		return send_user_error(api, result);

	log_info("Created user \"%s\" (%s)", created.username, user_role_str(created.role));
	return send_users(api, &created, 1, 201);
}

// GET /api/users/{username}
static int get_user(struct lorentz_conn *api)
{
	struct user me;
	const bool self = current_account(api, &me) && strcasecmp(me.username, api->item) == 0;
	if(!self && !is_admin(api))
		return send_forbidden(api);

	struct user user;
	const enum user_result result = user_get(api->item, &user);
	if(result != USER_OK)
		return send_user_error(api, result);
	return send_users(api, &user, 1, 200);
}

// PUT /api/users/{username}: change what is in the payload
static int update_user(struct lorentz_conn *api)
{
	struct user me;
	const bool has_account = current_account(api, &me);
	const bool admin = !has_account || me.role == USER_ROLE_ADMIN;
	const bool self = has_account && strcasecmp(me.username, api->item) == 0;
	if(!self && !admin)
		return send_forbidden(api);

	const int ret = check_json_payload(api);
	if(ret != 0)
		return ret;

	const char *bad = NULL;
	const char *username = json_string_field(api->payload.json, "username", &bad);
	const char *password = json_string_field(api->payload.json, "password", &bad);
	const char *current = json_string_field(api->payload.json, "current_password", &bad);
	if(bad != NULL)
		return bad_field(api, bad, "string");

	struct user_changes changes = { 0 };
	const int fret = parse_role_enabled(api, &changes);
	if(fret != 0)
		return fret;
	if(username != NULL)
	{
		changes.has_username = true;
		changes.username = username;
	}
	if(password != NULL)
	{
		changes.has_password = true;
		changes.password = password;
	}
	const cJSON *comment = cJSON_GetObjectItemCaseSensitive(api->payload.json, "comment");
	if(comment != NULL)
	{
		if(!cJSON_IsString(comment) && !cJSON_IsNull(comment))
			return bad_field(api, "comment", "string");
		changes.has_comment = true;
		changes.comment = cJSON_IsString(comment) ? comment->valuestring : NULL;
	}

	// What somebody may do to their own account: not what gives them rights
	if(!admin && (changes.has_role || changes.has_enabled || changes.has_username))
		return send_forbidden(api);

	// A password is only changed by somebody who knows the old one, unless an
	// admin resets the password of another account
	if(changes.has_password && self)
	{
		if(current == NULL)
			return send_json_error(api, 400, "bad_request", "No current_password found in JSON payload", NULL);
		const enum password_result check = user_check_password(api->item, current);
		wipe(api->payload.json, "current_password");
		if(check == PASSWORD_RATE_LIMITED)
			return send_json_error(api, 429, "rate_limiting", "Rate-limiting login attempts", NULL);
		if(check != PASSWORD_CORRECT)
		{
			wipe(api->payload.json, "password");
			return send_json_error(api, 401, "unauthorized", "The current password is wrong", NULL);
		}
	}

	// Somebody cannot switch off or take the rights of their own account
	if(self && ((changes.has_enabled && !changes.enabled) || (changes.has_role && changes.role != USER_ROLE_ADMIN && me.role == USER_ROLE_ADMIN)))
	{
		wipe(api->payload.json, "password");
		return send_json_error(api, 409, "conflict", "You cannot disable or demote your own account", NULL);
	}

	struct user updated;
	const enum user_result result = user_update(api->item, &changes, &updated);
	wipe(api->payload.json, "password");
	if(result != USER_OK)
		return send_user_error(api, result);

	// A disabled account is out, and after a new password nobody logged in with
	// the old one stays logged in (the session that asked keeps going)
	if(!updated.enabled)
		delete_user_sessions(updated.id, -1);
	else if(changes.has_password)
		delete_user_sessions(updated.id, self ? api->user_id : -1);

	log_info("Updated user \"%s\"", updated.username);
	return send_users(api, &updated, 1, 200);
}

// DELETE /api/users/{username}
static int delete_user(struct lorentz_conn *api)
{
	if(!is_admin(api))
		return send_forbidden(api);

	struct user me;
	if(current_account(api, &me) && strcasecmp(me.username, api->item) == 0)
		return send_json_error(api, 409, "conflict", "You cannot delete your own account", NULL);

	int64_t id = 0;
	const enum user_result result = user_delete(api->item, &id);
	if(result != USER_OK)
		return send_user_error(api, result);

	delete_user_sessions(id, -1);
	log_info("Deleted user \"%s\"", api->item);
	send_http_code(api, NULL, 204, "");
	return 204;
}

// /api/users and /api/users/{username}
int api_users(struct lorentz_conn *api)
{
	const bool named = api->item != NULL && api->item[0] != '\0';

	if(!named)
	{
		if(api->method == HTTP_GET)
			return list_users(api);
		if(api->method == HTTP_POST)
			return create_user(api);
		return 0;
	}

	switch(api->method)
	{
		case HTTP_GET: return get_user(api);
		case HTTP_PUT: return update_user(api);
		case HTTP_DELETE: return delete_user(api);
		// Not allowed on a named account (api_request[] in api.c does not
		// route these methods here in the first place)
		case HTTP_UNKNOWN:
		case HTTP_POST:
		case HTTP_PATCH:
		case HTTP_OPTIONS:
		default: return 0;
	}
}
