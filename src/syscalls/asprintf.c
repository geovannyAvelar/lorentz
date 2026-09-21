/* Lorentz: A black hole for Internet advertisements
*  (c) 2020 Pi-hole, LLC (https://pi-hole.net)
*  Network-wide ad blocking via your own hardware.
*
*  Lorentz Engine
*  Lorentz syscall implementation for asprintf
*
*  This file is copyright under the latest version of the EUPL.
*  Please see LICENSE file for your rights under this license. */

#include "lorentz.h"
//#include "syscalls.h" is implicitly done in lorentz.h
#include "log.h"

int Lorentzasprintf(const char *file, const char *func, const int line, char **buffer, const char *format, ...)
{
	va_list args;
	va_start(args, format);
	const int length = Lorentzvasprintf(file, func, line, buffer, format, args);
	va_end(args);

	return length;
}
