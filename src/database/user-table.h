/* Lorentz: A black hole for Internet advertisements
*  (c) 2026 Pi-hole, LLC (https://pi-hole.net)
*  Network-wide ad blocking via your own hardware.
*
*  Lorentz Engine
*  User accounts of the API: database routines
*
*  This file is copyright under the latest version of the EUPL.
*  Please see LICENSE file for your rights under this license. */
#ifndef USER_TABLE_H
#define USER_TABLE_H

#include "db-driver.h"
#include <stdbool.h>
#include <stdint.h>
#include <time.h>

// admin: everything. viewer: read the statistics and the lists, change nothing
enum user_role {
	USER_ROLE_ADMIN = 0,
	USER_ROLE_VIEWER
};

#define USERNAME_MAX 64
#define USER_COMMENT_MAX 256
#define USER_PASSWORD_MIN 8
#define USER_PASSWORD_MAX 256

struct user {
	int64_t id;
	char username[USERNAME_MAX + 1];
	enum user_role role;
	bool enabled;
	char comment[USER_COMMENT_MAX + 1];
	time_t created_at, updated_at;
	time_t last_login; // 0: never
};

enum user_result {
	USER_OK = 0,
	USER_NOT_FOUND,
	USER_EXISTS,
	USER_INVALID_NAME,
	USER_INVALID_PASSWORD,
	USER_INVALID_COMMENT,
	USER_LAST_ADMIN, // the change would leave no enabled admin
	USER_ERROR // database or memory failure
};

// What to change in user_update(). A field is left alone unless its has_ flag is set
struct user_changes {
	bool has_username, has_password, has_role, has_enabled, has_comment;
	const char *username;
	const char *password;
	enum user_role role;
	bool enabled;
	const char *comment;
};

// Migration to database version 23: the users table and session.user_id
bool add_users_table(db_conn *db);

const char *user_role_str(enum user_role role) __attribute__((const));
bool user_role_parse(const char *str, enum user_role *role);
const char *user_result_str(enum user_result result) __attribute__((const));

// Usernames are case insensitive and stored in lower case. This lowers name in
// place if it is valid and reports whether it is
bool user_normalize_username(char name[USERNAME_MAX + 1], const char *input);

enum user_result user_create(const char *username, const char *password, enum user_role role,
                             const char *comment, bool enabled, struct user *out);
enum user_result user_get(const char *username, struct user *out);
// The caller frees the array
enum user_result user_list(struct user **users, size_t *count);
enum user_result user_update(const char *username, const struct user_changes *changes, struct user *out);
enum user_result user_delete(const char *username, int64_t *id);

// Check a password of an account. PASSWORD_CORRECT fills out and records the login.
// An unknown or disabled account costs as much time as a wrong password
#include "config/config.h"
#include "config/password.h"
enum password_result user_authenticate(const char *username, const char *password, struct user *out);
// The same for a user who is already known, e.g. before a password change
enum password_result user_check_password(const char *username, const char *password);

// The accounts as of the last change, kept in memory so that every API request can be checked
// without the database. Returns false if the account does not exist or is disabled
bool user_cache_lookup(int64_t id, enum user_role *role, char username[USERNAME_MAX + 1]);
// How many enabled accounts there are: without any, and without a password in the
// configuration, the API is open
unsigned int users_enabled_count(void);
// Read the accounts from the database, done at start
bool users_load_cache(db_conn *db);

#endif // USER_TABLE_H
