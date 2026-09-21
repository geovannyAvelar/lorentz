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
