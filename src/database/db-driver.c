/* Lorentz: A black hole for Internet advertisements
*  (c) 2026 Pi-hole, LLC (https://pi-hole.net)
*  Network-wide ad blocking via your own hardware.
*
*  Lorentz Engine
*  Database driver registry
*  /src/database/db-driver.c
*
*  This file is copyright under the latest version of the EUPL.
*  Please see LICENSE file for your rights under this license. */

#include "lorentz.h"
#include "db-driver.h"

#include <string.h>
#include <stdio.h>

// The driver harnesses link this file without the tracking wrappers of lorentz.h
#undef strstr
#undef strlen
#undef memmove
#undef strchr
#undef strncmp
#undef memcpy

static const db_driver *const drivers[] = {
	&db_driver_sqlite,
#ifdef HAVE_POSTGRES
	&db_driver_postgres,
#endif
};

// Look up a driver by name, NULL if unknown
const db_driver *db_driver_get(const char *name)
{
	if(name == NULL)
		return NULL;

	for(size_t i = 0; i < sizeof(drivers) / sizeof(drivers[0]); i++)
		if(strcmp(drivers[i]->name, name) == 0)
			return drivers[i];

	return NULL;
}

static const db_driver *active_driver = &db_driver_sqlite;

// Select the driver used by db_open(). Returns false for an unknown name
bool db_driver_select(const char *name)
{
	const db_driver *drv = db_driver_get(name);
	if(drv == NULL)
		return false;

	active_driver = drv;
	return true;
}

const db_driver *db_driver_active(void)
{
	return active_driver;
}

bool db_uri_is_remote(const char *uri)
{
	return uri != NULL && (strncmp(uri, "postgresql://", 13) == 0 ||
	                       strncmp(uri, "postgres://", 11) == 0);
}

const db_driver *db_driver_for_uri(const char *uri)
{
	if(db_uri_is_remote(uri))
	{
		const db_driver *drv = db_driver_get("postgres");
		// Not built in: fall back to SQLite, which fails to open the URI
		// and reports it like any other unusable database
		if(drv != NULL)
			return drv;
	}

	return &db_driver_sqlite;
}

// Mask the password of postgresql://user:password@host/db. Everything else is
// returned as it is. The result is valid until the next call of the thread
const char *db_uri_display(const char *uri)
{
	static __thread char buf[512];
	if(uri == NULL)
		return "";
	if(!db_uri_is_remote(uri))
		return uri;

	const char *scheme_end = strstr(uri, "://") + 3;
	const char *at = strchr(scheme_end, '@');
	const char *slash = strchr(scheme_end, '/');
	const char *colon = strchr(scheme_end, ':');
	// A password exists only if a colon comes before the "@" of the userinfo
	if(at != NULL && colon != NULL && colon < at && (slash == NULL || at < slash))
		snprintf(buf, sizeof(buf), "%.*s:***%s", (int)(colon - uri), uri, at);
	else
		snprintf(buf, sizeof(buf), "%s", uri);

	// A password can also be given as a parameter
	char *pw = strstr(buf, "password=");
	if(pw != NULL)
	{
		pw += 9;
		char *end = strchr(pw, '&');
		memmove(pw + 3, end != NULL ? end : pw + strlen(pw), end != NULL ? strlen(end) + 1 : 1);
		memcpy(pw, "***", 3);
	}
	return buf;
}
