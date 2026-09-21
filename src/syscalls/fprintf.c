/* Lorentz: A black hole for Internet advertisements
*  (c) 2020 Pi-hole, LLC (https://pi-hole.net)
*  Network-wide ad blocking via your own hardware.
*
*  Lorentz Engine
*  Lorentz syscall implementation for fprintf
*
*  This file is copyright under the latest version of the EUPL.
*  Please see LICENSE file for your rights under this license. */

#include "lorentz.h"
//#include "syscalls.h" is implicitly done in lorentz.h
#include "log.h"

int Lorentzfprintf(FILE *stream, const char *file, const char *func, const int line, const char *format, ...)
{
	va_list args;
	va_start(args, format);
	const int length = Lorentzvfprintf(stream, file, func, line, format, args);
	va_end(args);

	return length;
}
