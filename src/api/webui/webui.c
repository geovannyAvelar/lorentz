/* Lorentz: A black hole for Internet advertisements
*  (c) 2026 Pi-hole, LLC (https://pi-hole.net)
*  Network-wide ad blocking via your own hardware.
*
*  Lorentz Engine
*  Embedded web UI
*
*  Serves the web UI's static export (web/out, embedded at build time - see
*  generate.sh) directly from memory, at Lorentz's own admin path. No file
*  system access and no separate Node.js process are involved at runtime.
*
*  This file is copyright under the latest version of the EUPL.
*  Please see LICENSE file for your rights under this license. */

#include "webui.h"
#include "webserver/webserver.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

static const struct webui_file * __attribute__((pure)) find_file(const char *path)
{
	for(size_t i = 0; i < webui_files_count; i++)
		if(strcmp(webui_files[i].path, path) == 0)
			return &webui_files[i];
	return NULL;
}

int webui_handler(struct mg_connection *conn, void *cbdata)
{
	(void)cbdata;
	const struct mg_request_info *request = mg_get_request_info(conn);
	const char *uri = request->local_uri_raw;
	const char *prefix_webhome = get_prefix_webhome();
	const size_t prefix_len = strlen(prefix_webhome);

	// Path relative to the webhome prefix, e.g. "/admin/queries/" -> "queries/"
	const char *rel = uri;
	if(strncmp(uri, prefix_webhome, prefix_len) == 0)
		rel += prefix_len;

	const struct webui_file *file = NULL;
	if(*rel == '\0' || rel[strlen(rel) - 1] == '/')
	{
		// Directory request: serve its index.html
		char *indexpath = NULL;
		if(asprintf(&indexpath, "%sindex.html", rel) >= 0)
		{
			file = find_file(indexpath);
			free(indexpath);
		}
	}
	else
	{
		file = find_file(rel);
	}

	if(file == NULL)
	{
		mg_send_http_error(conn, 404, "Not Found");
		return 1;
	}

	mg_send_http_ok(conn, file->mime_type, (long long)file->content_size);
	return mg_write(conn, file->content, file->content_size);
}
