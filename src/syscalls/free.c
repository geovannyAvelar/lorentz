/* Lorentz: A black hole for Internet advertisements
*  (c) 2020 Pi-hole, LLC (https://pi-hole.net)
*  Network-wide ad blocking via your own hardware.
*
*  Lorentz Engine
*  Lorentz syscall implementation for free
*
*  This file is copyright under the latest version of the EUPL.
*  Please see LICENSE file for your rights under this license. */

#include "lorentz.h"
//#include "syscalls.h" is implicitly done in lorentz.h
#include "log.h"

#undef free
bool Lorentzfree(void *ptr, const char *file, const char *func, const int line)
{
	// The free() function frees the memory space pointed  to  by  ptr,  which
	// must  have  been  returned by a previous call to malloc(), calloc(), or
	// realloc().  Otherwise, or if free(ptr) has already been called  before,
	// undefined behavior occurs.  If ptr is NULL, no operation is performed.
	if(ptr == NULL)
	{
		log_warn("Trying to free NULL pointer in %s() (%s:%i)", func, file, line);
		return false;
	}

	// Actually free the memory
	free(ptr);

	return true;
}
