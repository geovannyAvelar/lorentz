/* Lorentz: A black hole for Internet advertisements
*  (c) 2026 Pi-hole, LLC (https://pi-hole.net)
*  Network-wide ad blocking via your own hardware.
*
*  Lorentz Engine
*  Embedded web UI (helper)
*
*  This file is copyright under the latest version of the EUPL.
*  Please see LICENSE file for your rights under this license. */
#ifndef API_WEBUI_H
#define API_WEBUI_H

#include "webserver/civetweb/civetweb.h"
#include <stddef.h>

// One file of the web UI's static export, embedded as a byte array. Either
// generated at build time from web/out (see generate.sh, CMakeLists.txt) or,
// when EMBED_WEBUI is off, an empty placeholder (webui_stub.c).
struct webui_file {
	const char *path;
	const char *mime_type;
	const unsigned char *content;
	size_t content_size;
};

extern const struct webui_file webui_files[];
extern const size_t webui_files_count;

int webui_handler(struct mg_connection *conn, void *cbdata);

#endif // API_WEBUI_H
