/* Lorentz: A black hole for Internet advertisements
*  (c) 2026 Pi-hole, LLC (https://pi-hole.net)
*  Network-wide ad blocking via your own hardware.
*
*  Lorentz Engine
*  User accounts of the API: database routines
*
*  This file is copyright under the latest version of the EUPL.
*  Please see LICENSE file for your rights under this license. */

#include "lorentz.h"
#include "database/user-table.h"
#include "database/common.h"
#include "config/config.h"
#include "log.h"

#include <ctype.h>
#include <pthread.h>

#define USER_COLUMNS "id,username,role,enabled,comment,created_at,updated_at,last_login"

// Changes are made one at a time, so that the check for the last admin and the
// change itself cannot be interleaved by two requests. It guards the cache as well
static pthread_mutex_t users_lock = PTHREAD_MUTEX_INITIALIZER;

struct cached_user {
	int64_t id;
	enum user_role role;
	char username[USERNAME_MAX + 1];
};
static struct cached_user *cache = NULL;
static size_t cache_count = 0;
static unsigned int enabled_count = 0;

/* ---- migration ---- */

bool add_users_table(db_conn *db)
{
	SQL_bool(db, "BEGIN TRANSACTION;");

	// The same columns as db_schema_baseline() creates
	SQL_bool(db, "CREATE TABLE users ("
	                 "id INTEGER PRIMARY KEY AUTOINCREMENT, "
	                 "username TEXT UNIQUE NOT NULL, "
	                 "pwhash TEXT NOT NULL, "
	                 "role TEXT NOT NULL DEFAULT 'admin', "
	                 "enabled SMALLINT NOT NULL DEFAULT 1, "
	                 "comment TEXT, "
	                 "created_at BIGINT NOT NULL, "
	                 "updated_at BIGINT NOT NULL, "
	                 "last_login BIGINT);");
	// A session of an account remembers which one
	SQL_bool(db, "ALTER TABLE session ADD COLUMN user_id BIGINT;");

	if(!db_set_Lorentz_property(db, DB_VERSION, 23))
	{
		log_err("add_users_table(): Failed to update database version!");
		dbquery(db, "ROLLBACK");
		return false;
	}

	SQL_bool(db, "END");
	return true;
}

/* ---- names ---- */

const char *__attribute__((const)) user_role_str(enum user_role role)
{
	return role == USER_ROLE_VIEWER ? "viewer" : "admin";
}

bool user_role_parse(const char *str, enum user_role *role)
{
	if(str == NULL)
		return false;
	if(strcmp(str, "admin") == 0)
		*role = USER_ROLE_ADMIN;
	else if(strcmp(str, "viewer") == 0)
		*role = USER_ROLE_VIEWER;
	else
		return false;
	return true;
}

const char *__attribute__((const)) user_result_str(enum user_result result)
{
	switch(result)
	{
		case USER_OK: return "ok";
		case USER_NOT_FOUND: return "user not found";
		case USER_EXISTS: return "a user with this name exists already";
		case USER_INVALID_NAME: return "invalid username (1-64 letters, digits and . _ - @)";
		case USER_INVALID_PASSWORD: return "invalid password (8-256 characters)";
		case USER_INVALID_COMMENT: return "invalid comment (at most 256 characters)";
		case USER_LAST_ADMIN: return "there has to be at least one enabled admin";
		case USER_ERROR:
		default: return "internal error";
	}
}

bool user_normalize_username(char name[USERNAME_MAX + 1], const char *input)
{
	if(input == NULL)
		return false;
	const size_t len = strlen(input);
	if(len < 1 || len > USERNAME_MAX)
		return false;

	for(size_t i = 0; i < len; i++)
	{
		const unsigned char c = (unsigned char)input[i];
		if(!(isalnum(c) || c == '.' || c == '_' || c == '-' || c == '@'))
			return false;
		name[i] = (char)tolower(c);
	}
	name[len] = '\0';
	return true;
}

static bool valid_password(const char *password)
{
	if(password == NULL)
		return false;
	const size_t len = strlen(password);
	return len >= USER_PASSWORD_MIN && len <= USER_PASSWORD_MAX;
}

static bool valid_comment(const char *comment)
{
	return comment == NULL || strlen(comment) <= USER_COMMENT_MAX;
}

/* ---- rows ---- */

static void read_user(db_stmt *stmt, struct user *u)
{
	memset(u, 0, sizeof(*u));
	u->id = db_column_int64(stmt, 0);
	snprintf(u->username, sizeof(u->username), "%s", db_column_text(stmt, 1) != NULL ? db_column_text(stmt, 1) : "");
	if(!user_role_parse(db_column_text(stmt, 2), &u->role))
		u->role = USER_ROLE_VIEWER; // an unknown role gets the least
	u->enabled = db_column_int(stmt, 3) != 0;
	if(db_column_type(stmt, 4) == DB_TYPE_TEXT)
		snprintf(u->comment, sizeof(u->comment), "%s", db_column_text(stmt, 4));
	u->created_at = (time_t)db_column_int64(stmt, 5);
	u->updated_at = (time_t)db_column_int64(stmt, 6);
	u->last_login = db_column_type(stmt, 7) == DB_TYPE_NULL ? 0 : (time_t)db_column_int64(stmt, 7);
}

// Read the account with this (normalized) name. pwhash may be NULL, otherwise it
// gets the hash (malloc, the caller frees it)
static enum user_result read_by_name(db_conn *db, const char *name, struct user *out, char **pwhash)
{
	db_stmt *stmt = db_prepare(db, "SELECT " USER_COLUMNS ",pwhash FROM users WHERE username = ?", false);
	if(stmt == NULL)
	{
		log_err("Cannot prepare the user lookup: %s", DB_LAST_ERR(db));
		return USER_ERROR;
	}
	enum user_result result = USER_NOT_FOUND;
	if(db_bind_text(stmt, 1, name) != DB_OK)
		result = USER_ERROR;
	else
	{
		const db_rc rc = db_step(stmt);
		if(rc == DB_ROW)
		{
			read_user(stmt, out);
			if(pwhash != NULL)
			{
				const char *hash = db_column_text(stmt, 8);
				*pwhash = hash != NULL ? strdup(hash) : NULL;
			}
			result = USER_OK;
		}
		else if(rc != DB_DONE)
			result = USER_ERROR;
	}
	db_finalize(stmt);
	return result;
}

static unsigned int count_enabled_admins(db_conn *db, const int64_t except_id)
{
	db_stmt *stmt = db_prepare(db, "SELECT COUNT(*) FROM users WHERE role = 'admin' AND enabled = 1 AND id != ?", false);
	if(stmt == NULL)
		return 0;
	unsigned int count = 0;
	if(db_bind_int64(stmt, 1, except_id) == DB_OK && db_step(stmt) == DB_ROW)
		count = (unsigned int)db_column_int(stmt, 0);
	db_finalize(stmt);
	return count;
}

/* ---- cache ---- */

// users_lock is held
static bool reload_cache(db_conn *db)
{
	db_stmt *stmt = db_prepare(db, "SELECT id,role,username FROM users WHERE enabled = 1", false);
	if(stmt == NULL)
	{
		log_err("Cannot read the users: %s", DB_LAST_ERR(db));
		return false;
	}

	struct cached_user *fresh = NULL;
	size_t count = 0, capacity = 0;
	db_rc rc;
	while((rc = db_step(stmt)) == DB_ROW)
	{
		if(count == capacity)
		{
			capacity = capacity == 0 ? 8 : capacity * 2;
			struct cached_user *grown = realloc(fresh, capacity * sizeof(*fresh));
			if(grown == NULL)
			{
				free(fresh);
				db_finalize(stmt);
				return false;
			}
			fresh = grown;
		}
		fresh[count].id = db_column_int64(stmt, 0);
		if(!user_role_parse(db_column_text(stmt, 1), &fresh[count].role))
			fresh[count].role = USER_ROLE_VIEWER;
		snprintf(fresh[count].username, sizeof(fresh[count].username), "%s",
		         db_column_text(stmt, 2) != NULL ? db_column_text(stmt, 2) : "");
		count++;
	}
	db_finalize(stmt);
	if(rc != DB_DONE)
	{
		free(fresh);
		return false;
	}

	free(cache);
	cache = fresh;
	cache_count = count;
	enabled_count = (unsigned int)count;
	return true;
}

bool users_load_cache(db_conn *db)
{
	pthread_mutex_lock(&users_lock);
	const bool okay = reload_cache(db);
	pthread_mutex_unlock(&users_lock);
	return okay;
}

bool user_cache_lookup(int64_t id, enum user_role *role, char username[USERNAME_MAX + 1])
{
	bool found = false;
	pthread_mutex_lock(&users_lock);
	for(size_t i = 0; i < cache_count; i++)
	{
		if(cache[i].id == id)
		{
			if(role != NULL)
				*role = cache[i].role;
			if(username != NULL)
				snprintf(username, USERNAME_MAX + 1, "%s", cache[i].username);
			found = true;
			break;
		}
	}
	pthread_mutex_unlock(&users_lock);
	return found;
}

unsigned int users_enabled_count(void)
{
	pthread_mutex_lock(&users_lock);
	const unsigned int count = enabled_count;
	pthread_mutex_unlock(&users_lock);
	return count;
}

/* ---- create, read, update, delete ---- */

enum user_result user_create(const char *username, const char *password, enum user_role role,
                             const char *comment, bool enabled, struct user *out)
{
	char name[USERNAME_MAX + 1];
	if(!user_normalize_username(name, username))
		return USER_INVALID_NAME;
	if(!valid_password(password))
		return USER_INVALID_PASSWORD;
	if(!valid_comment(comment))
		return USER_INVALID_COMMENT;

	char *pwhash = create_password(password);
	if(pwhash == NULL)
		return USER_ERROR;

	db_conn *db = dbopen(false, false);
	if(db == NULL)
	{
		free(pwhash);
		return USER_ERROR;
	}

	pthread_mutex_lock(&users_lock);
	enum user_result result = USER_ERROR;
	struct user existing;
	const enum user_result found = read_by_name(db, name, &existing, NULL);
	if(found == USER_OK)
		result = USER_EXISTS;
	else if(found == USER_NOT_FOUND)
	{
		const time_t now = time(NULL);
		db_stmt *stmt = db_prepare(db, "INSERT INTO users (username,pwhash,role,enabled,comment,created_at,updated_at) "
		                               "VALUES (?,?,?,?,?,?,?)", false);
		if(stmt != NULL &&
		   db_bind_text(stmt, 1, name) == DB_OK &&
		   db_bind_text(stmt, 2, pwhash) == DB_OK &&
		   db_bind_text(stmt, 3, user_role_str(role)) == DB_OK &&
		   db_bind_int(stmt, 4, enabled ? 1 : 0) == DB_OK &&
		   (comment != NULL ? db_bind_text(stmt, 5, comment) : db_bind_null(stmt, 5)) == DB_OK &&
		   db_bind_int64(stmt, 6, now) == DB_OK &&
		   db_bind_int64(stmt, 7, now) == DB_OK)
		{
			const db_rc rc = db_step(stmt);
			if(rc == DB_DONE)
				result = USER_OK;
			else if(rc == DB_CONSTRAINT)
				result = USER_EXISTS;
			else
				log_err("Cannot create user \"%s\": %s", name, DB_LAST_ERR(db));
		}
		else
			log_err("Cannot prepare the creation of user \"%s\": %s", name, DB_LAST_ERR(db));
		db_finalize(stmt);

		if(result == USER_OK)
		{
			if(out != NULL && read_by_name(db, name, out, NULL) != USER_OK)
				result = USER_ERROR;
			reload_cache(db);
		}
	}
	pthread_mutex_unlock(&users_lock);

	dbclose(&db);
	memset(pwhash, 0, strlen(pwhash));
	free(pwhash);
	return result;
}

enum user_result user_get(const char *username, struct user *out)
{
	char name[USERNAME_MAX + 1];
	if(!user_normalize_username(name, username))
		return USER_NOT_FOUND;

	db_conn *db = dbopen(true, false);
	if(db == NULL)
		return USER_ERROR;
	const enum user_result result = read_by_name(db, name, out, NULL);
	dbclose(&db);
	return result;
}

enum user_result user_list(struct user **users, size_t *count)
{
	*users = NULL;
	*count = 0;

	db_conn *db = dbopen(true, false);
	if(db == NULL)
		return USER_ERROR;
	db_stmt *stmt = db_prepare(db, "SELECT " USER_COLUMNS " FROM users ORDER BY username", false);
	if(stmt == NULL)
	{
		log_err("Cannot list the users: %s", DB_LAST_ERR(db));
		dbclose(&db);
		return USER_ERROR;
	}

	size_t capacity = 0;
	struct user *list = NULL;
	db_rc rc;
	while((rc = db_step(stmt)) == DB_ROW)
	{
		if(*count == capacity)
		{
			capacity = capacity == 0 ? 8 : capacity * 2;
			struct user *grown = realloc(list, capacity * sizeof(*list));
			if(grown == NULL)
			{
				free(list);
				db_finalize(stmt);
				dbclose(&db);
				*count = 0;
				return USER_ERROR;
			}
			list = grown;
		}
		read_user(stmt, &list[(*count)++]);
	}
	db_finalize(stmt);
	dbclose(&db);
	if(rc != DB_DONE)
	{
		free(list);
		*count = 0;
		return USER_ERROR;
	}

	*users = list;
	return USER_OK;
}

enum user_result user_update(const char *username, const struct user_changes *changes, struct user *out)
{
	char name[USERNAME_MAX + 1], newname[USERNAME_MAX + 1] = "";
	if(!user_normalize_username(name, username))
		return USER_NOT_FOUND;
	if(changes->has_username && !user_normalize_username(newname, changes->username))
		return USER_INVALID_NAME;
	if(changes->has_password && !valid_password(changes->password))
		return USER_INVALID_PASSWORD;
	if(changes->has_comment && !valid_comment(changes->comment))
		return USER_INVALID_COMMENT;

	char *newhash = changes->has_password ? create_password(changes->password) : NULL;
	if(changes->has_password && newhash == NULL)
		return USER_ERROR;

	db_conn *db = dbopen(false, false);
	if(db == NULL)
	{
		free(newhash);
		return USER_ERROR;
	}

	pthread_mutex_lock(&users_lock);
	struct user user;
	char *pwhash = NULL;
	enum user_result result = read_by_name(db, name, &user, &pwhash);
	if(result == USER_OK)
	{
		const struct user before = user;
		if(changes->has_username)
			snprintf(user.username, sizeof(user.username), "%s", newname);
		if(changes->has_role)
			user.role = changes->role;
		if(changes->has_enabled)
			user.enabled = changes->enabled;
		if(changes->has_comment)
			snprintf(user.comment, sizeof(user.comment), "%s", changes->comment != NULL ? changes->comment : "");

		// A name that is taken (the constraint would refuse it as well, and be logged)
		struct user taken;
		if(changes->has_username && strcmp(before.username, user.username) != 0 &&
		   read_by_name(db, user.username, &taken, NULL) == USER_OK)
			result = USER_EXISTS;

		// Demoting or disabling the last enabled admin would lock everyone out
		if(result == USER_OK && before.role == USER_ROLE_ADMIN && before.enabled &&
		   (user.role != USER_ROLE_ADMIN || !user.enabled) &&
		   count_enabled_admins(db, before.id) == 0)
			result = USER_LAST_ADMIN;
	}
	if(result == USER_OK)
	{
		result = USER_ERROR;
		db_stmt *stmt = db_prepare(db, "UPDATE users SET username = ?, pwhash = ?, role = ?, enabled = ?, "
		                               "comment = ?, updated_at = ? WHERE id = ?", false);
		if(stmt != NULL &&
		   db_bind_text(stmt, 1, user.username) == DB_OK &&
		   db_bind_text(stmt, 2, newhash != NULL ? newhash : (pwhash != NULL ? pwhash : "")) == DB_OK &&
		   db_bind_text(stmt, 3, user_role_str(user.role)) == DB_OK &&
		   db_bind_int(stmt, 4, user.enabled ? 1 : 0) == DB_OK &&
		   (user.comment[0] != '\0' ? db_bind_text(stmt, 5, user.comment) : db_bind_null(stmt, 5)) == DB_OK &&
		   db_bind_int64(stmt, 6, time(NULL)) == DB_OK &&
		   db_bind_int64(stmt, 7, user.id) == DB_OK)
		{
			const db_rc rc = db_step(stmt);
			if(rc == DB_DONE)
				result = USER_OK;
			else if(rc == DB_CONSTRAINT)
				result = USER_EXISTS;
			else
				log_err("Cannot update user \"%s\": %s", name, DB_LAST_ERR(db));
		}
		else
			log_err("Cannot prepare the update of user \"%s\": %s", name, DB_LAST_ERR(db));
		db_finalize(stmt);

		if(result == USER_OK)
		{
			if(out != NULL && read_by_name(db, user.username, out, NULL) != USER_OK)
				result = USER_ERROR;
			reload_cache(db);
		}
	}
	pthread_mutex_unlock(&users_lock);

	dbclose(&db);
	if(newhash != NULL)
	{
		memset(newhash, 0, strlen(newhash));
		free(newhash);
	}
	free(pwhash);
	return result;
}

enum user_result user_delete(const char *username, int64_t *id)
{
	char name[USERNAME_MAX + 1];
	if(!user_normalize_username(name, username))
		return USER_NOT_FOUND;

	db_conn *db = dbopen(false, false);
	if(db == NULL)
		return USER_ERROR;

	pthread_mutex_lock(&users_lock);
	struct user user;
	enum user_result result = read_by_name(db, name, &user, NULL);
	if(result == USER_OK && user.role == USER_ROLE_ADMIN && user.enabled &&
	   count_enabled_admins(db, user.id) == 0)
		result = USER_LAST_ADMIN;
	if(result == USER_OK)
	{
		result = USER_ERROR;
		db_stmt *stmt = db_prepare(db, "DELETE FROM users WHERE id = ?", false);
		if(stmt != NULL && db_bind_int64(stmt, 1, user.id) == DB_OK && db_step(stmt) == DB_DONE)
			result = USER_OK;
		else
			log_err("Cannot delete user \"%s\": %s", name, DB_LAST_ERR(db));
		db_finalize(stmt);

		if(result == USER_OK)
		{
			if(id != NULL)
				*id = user.id;
			reload_cache(db);
		}
	}
	pthread_mutex_unlock(&users_lock);

	dbclose(&db);
	return result;
}

/* ---- passwords ---- */

// What is checked against when the account does not exist, so that the time of
// the answer does not tell
static char *dummy_hash(void)
{
	static char *hash = NULL;
	static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
	pthread_mutex_lock(&lock);
	if(hash == NULL)
		hash = create_password("lorentz-no-such-user");
	char *copy = hash != NULL ? strdup(hash) : NULL;
	pthread_mutex_unlock(&lock);
	return copy;
}

static enum password_result authenticate(const char *username, const char *password, struct user *out, const bool record)
{
	char name[USERNAME_MAX + 1];
	struct user user;
	char *pwhash = NULL;
	bool known = false;

	if(user_normalize_username(name, username))
	{
		db_conn *db = dbopen(true, false);
		if(db != NULL)
		{
			known = read_by_name(db, name, &user, &pwhash) == USER_OK && user.enabled;
			dbclose(&db);
		}
	}

	if(!known)
	{
		// Spend the time of a check, and the attempt counts against the rate limit
		free(pwhash);
		pwhash = dummy_hash();
		const enum password_result rate = verify_password(password, pwhash, true);
		free(pwhash);
		return rate == PASSWORD_RATE_LIMITED ? PASSWORD_RATE_LIMITED : PASSWORD_INCORRECT;
	}

	const enum password_result result = verify_password(password, pwhash, true);
	free(pwhash);
	if(result != PASSWORD_CORRECT)
		return result;

	// Remember the login. A failure to do so does not stop it
	db_conn *db = record ? dbopen(false, false) : NULL;
	if(db != NULL)
	{
		db_stmt *stmt = db_prepare(db, "UPDATE users SET last_login = ? WHERE id = ?", false);
		if(stmt != NULL && db_bind_int64(stmt, 1, time(NULL)) == DB_OK &&
		   db_bind_int64(stmt, 2, user.id) == DB_OK)
			db_step(stmt);
		db_finalize(stmt);
		dbclose(&db);
	}
	if(record)
		user.last_login = time(NULL);
	if(out != NULL)
		*out = user;
	return PASSWORD_CORRECT;
}

enum password_result user_authenticate(const char *username, const char *password, struct user *out)
{
	return authenticate(username, password, out, true);
}

enum password_result user_check_password(const char *username, const char *password)
{
	struct user user;
	return authenticate(username, password, &user, false);
}
