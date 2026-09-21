/* Lorentz: A black hole for Internet advertisements
*  (c) 2020 Pi-hole, LLC (https://pi-hole.net)
*  Network-wide ad blocking via your own hardware.
*
*  Lorentz Engine
*  Lorentz syscall implementation for snprintf
*
*  This file is copyright under the latest version of the EUPL.
*  Please see LICENSE file for your rights under this license. */

#include "lorentz.h"
//#include "syscalls.h" is implicitly done in lorentz.h
#include "log.h"

int Lorentzsnprintf(const char *file, const char *func, const int line, char *__restrict__ buffer, const size_t maxlen, const char *format, ...)
{
	va_list args;
	va_start(args, format);
	const int length = Lorentzvsnprintf(file, func, line, buffer, maxlen, format, args);
	va_end(args);

	return length;
}
