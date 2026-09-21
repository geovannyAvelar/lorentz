/* Lorentz: A black hole for Internet advertisements
*  (c) 2020 Pi-hole, LLC (https://pi-hole.net)
*  Network-wide ad blocking via your own hardware.
*
*  Lorentz Engine
*  Lorentz syscall implementation for pthread_mutex_lock
*
*  This file is copyright under the latest version of the EUPL.
*  Please see LICENSE file for your rights under this license. */

#include "lorentz.h"
//#include "syscalls.h" is implicitly done in lorentz.h
#include "log.h"

#include <pthread.h>

#undef pthread_mutex_lock
int Lorentzpthread_mutex_lock(pthread_mutex_t *__mutex, const char *file, const char *func, const int line)
{
	ssize_t ret = 0;
	do
	{
		ret = pthread_mutex_lock(__mutex);
	}
	// Try again if the last accept() call failed due to an interruption by an
	// incoming signal
	while(ret == EINTR);

	// Backup errno value
	const int _errno = errno;

	// Final error checking (may have failed for some other reason then an
	// EINTR = interrupted system call)
	if(ret < 0)
		log_warn("Could not pthread_mutex_lock() in %s() (%s:%i): %s",
		         func, file, line, strerror(errno));

	// Restore errno value
	errno = _errno;

	return ret;
}
