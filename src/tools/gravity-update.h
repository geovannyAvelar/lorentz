/* Lorentz: A black hole for Internet advertisements
*  (c) 2026 Pi-hole, LLC (https://pi-hole.net)
*  Network-wide ad blocking via your own hardware.
*
*  Lorentz Engine
*  Gravity updater: downloads the adlists and rebuilds the blocked domains
*
*  This file is copyright under the latest version of the EUPL.
*  Please see LICENSE file for your rights under this license. */
#ifndef GRAVITY_UPDATE_H
#define GRAVITY_UPDATE_H

#include <stdbool.h>
#include <stdint.h>

// Receives the progress of a run, one line (without the newline) at a time
typedef void (*gravity_log_fn)(const char *line, void *arg);

enum gravity_update_rc {
	// Every list was updated
	GRAVITY_UPDATE_OK,
	// Some lists could not be updated and kept what they had
	GRAVITY_UPDATE_PARTIAL,
	// Nothing could be updated
	GRAVITY_UPDATE_FAILED,
	// Another run is going on
	GRAVITY_UPDATE_BUSY,
	// This build cannot download (no libcurl) or there is no gravity database
	GRAVITY_UPDATE_UNAVAILABLE
};

struct gravity_update_result {
	unsigned int lists;    // enabled lists
	unsigned int updated;  // of them downloaded and stored
	unsigned int failed;   // of them that kept their old domains
	uint64_t domains;      // domains stored by the lists that were updated
};

// Download every enabled adlist and replace the domains of the ones that
// succeeded (a list that fails keeps what it had), then let Lorentz reload.
// Runs in the calling thread and takes as long as the downloads do; only one
// run goes on at a time. log may be NULL
enum gravity_update_rc gravity_update_run(gravity_log_fn log, void *arg, struct gravity_update_result *result);

// Whether a run goes on right now
bool gravity_update_running(void);

// Whether this build can download lists
bool gravity_update_available(void) __attribute__((const));

// The thread that starts a run when gravity.updateInterval hours have passed
void *gravity_update_thread(void *arg);

#endif // GRAVITY_UPDATE_H
