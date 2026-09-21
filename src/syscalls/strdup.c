/* Lorentz: A black hole for Internet advertisements
*  (c) 2020 Pi-hole, LLC (https://pi-hole.net)
*  Network-wide ad blocking via your own hardware.
*
*  Lorentz Engine
*  Lorentz syscall implementation for strdup
*
*  This file is copyright under the latest version of the EUPL.
*  Please see LICENSE file for your rights under this license. */

#include "lorentz.h"
//#include "syscalls.h" is implicitly done in lorentz.h
#include "log.h"

char* __attribute__((malloc)) Lorentzstrdup(const char *src, const char *file, const char *func, const int line)
{
	// The Lorentzstrdup() function returns a pointer to a new string which is a
	// duplicate of the string s. Memory for the new string is obtained with
	// calloc(3), and can be freed with free(3).
	if(src == NULL)
	{
		log_warn("Trying to copy a NULL string in %s() (%s:%i)", func, file, line);
		return NULL;
	}
	const size_t len = strlen(src);
	char *dest = Lorentzcalloc(len+1, sizeof(char), file, func, line);

	// Return early in case of an unrecoverable error, error reporting has
	// already been done in Lorentzcalloc()
	if(dest == NULL)
		return NULL;

	// Use memcpy as memory areas cannot overlap
	memcpy(dest, src, len);
	dest[len] = '\0';

	return dest;
}
