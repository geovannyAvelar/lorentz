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
// config.webserver.headers (also pulls in cJSON)
#include "config/config.h"
#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <stdio.h>

static const struct webui_file * __attribute__((pure)) find_file(const char *path)
{
	for(size_t i = 0; i < webui_files_count; i++)
		if(strcmp(webui_files[i].path, path) == 0)
			return &webui_files[i];
	return NULL;
}

// Inserts hashes right after "script-src 'self'" in an existing header line,
// e.g. Content-Security-Policy's. Returns NULL (unspliced) if the line does
// not contain that exact directive - a custom webserver.headers value keeps
// whatever script-src it already has rather than being silently rewritten.
static char * __attribute__((malloc)) splice_script_src(const char *line, const char *hashes)
{
	static const char needle[] = "script-src 'self'";
	const char *pos = strstr(line, needle);
	if(pos == NULL)
		return NULL;

	const int prefix_len = (int)(pos - line) + (int)sizeof(needle) - 1;
	char *spliced = NULL;
	if(asprintf(&spliced, "%.*s %s%s", prefix_len, line, hashes, pos + sizeof(needle) - 1) < 0)
		return NULL;
	return spliced;
}

// Builds the same "Name: value\r\n..." block webserver_start() sends with
// every other response (config.webserver.headers), except the
// Content-Security-Policy line - if there is one - gets this file's inline
// <script> hashes spliced into its script-src. Used instead of civetweb's
// own additional_header mechanism (send_additional_header() in civetweb.c),
// which cannot be overridden per file, only sent as configured.
static char * __attribute__((malloc)) build_response_headers(const char *csp_hashes)
{
	char *headers = strdup("");
	if(headers == NULL)
		return NULL;

	const size_t csp_prefix_len = strlen("Content-Security-Policy:");
	cJSON *header = NULL;
	cJSON_ArrayForEach(header, config.webserver.headers.v.json)
	{
		if(!cJSON_IsString(header))
			continue;
		const char *h = cJSON_GetStringValue(header);

		char *spliced = NULL;
		if(csp_hashes != NULL && csp_hashes[0] != '\0' &&
		   strncasecmp(h, "Content-Security-Policy:", csp_prefix_len) == 0)
			spliced = splice_script_src(h, csp_hashes);
		const char *line = spliced != NULL ? spliced : h;

		char *new_headers = NULL;
		const int ret = asprintf(&new_headers, "%s%s\r\n", headers, line);
		free(spliced);
		free(headers);
		if(ret < 0)
			return NULL;
		headers = new_headers;
	}

	return headers;
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

	// Built manually (mg_response_header_* - see response.inl) instead of
	// mg_send_http_ok(): that helper calls civetweb's send_additional_header(),
	// which always sends config.webserver.headers verbatim and cannot be
	// overridden per response, so a stricter, hash-augmented
	// Content-Security-Policy for this file could never replace it there.
	char lenbuf[32];
	snprintf(lenbuf, sizeof(lenbuf), "%zu", file->content_size);
	char *headers = build_response_headers(file->csp_script_hashes);

	mg_response_header_start(conn, 200);
	mg_response_header_add(conn, "Content-Type", file->mime_type, -1);
	mg_response_header_add(conn, "Content-Length", lenbuf, -1);
	if(headers != NULL)
		mg_response_header_add_lines(conn, headers);
	free(headers);
	mg_response_header_send(conn);

	return mg_write(conn, file->content, file->content_size);
}
