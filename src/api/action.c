/* Lorentz: A black hole for Internet advertisements
*  (c) 2023 Pi-hole, LLC (https://pi-hole.net)
*  Network-wide ad blocking via your own hardware.
*
*  Lorentz Engine
*  API Implementation /api/action
*
*  This file is copyright under the latest version of the EUPL.
*  Please see LICENSE file for your rights under this license. */

#include "lorentz.h"
#include "webserver/http-common.h"
#include "webserver/json_macros.h"
#include "api/api.h"
// wait()
#include <sys/wait.h>
// reboot()
#include <sys/reboot.h>
#include <unistd.h>
// exit_code
#include "signals.h"
// flush_network_table()
#include "database/network-table.h"
#include "config/config.h"
// gravity_running
#include "daemon.h"
// gravity_update_run()
#include "tools/gravity-update.h"

// Sends what a gravity update reports as it goes, one chunk per line
struct gravity_stream {
	struct lorentz_conn *api;
};

static void stream_line(const char *line, void *arg)
{
	struct gravity_stream *stream = arg;
	char chunk[1100];
	const int len = snprintf(chunk, sizeof(chunk), "%s\n", line);
	if(len > 0)
		mg_printf(stream->api->conn, "%zX\r\n%s\r\n", (size_t)len, chunk);
}

int api_action_gravity(struct lorentz_conn *api)
{
	if(!gravity_update_available())
		return send_json_error(api, 501,
		                       "not_implemented",
		                       "This build of Lorentz cannot download lists",
		                       "Lorentz was built without libcurl");

	// Once the stream has started the status cannot change, so a run that is
	// already going on is turned away first
	if(gravity_update_running())
		return send_json_error(api, 409,
		                       "conflict",
		                       "An update is already running",
		                       NULL);

	// Send 200 OK with chunked size (-1) and report as the run goes on
	mg_send_http_ok(api->conn, "text/plain", -1);
	struct gravity_stream stream = { .api = api };
	gravity_update_run(stream_line, &stream, NULL);

	// Send final chunk of size 0 showing end of data
	mg_printf(api->conn, "0\r\n\r\n");

	// If a termination/restart was requested while gravity was running,
	// act on it now rather than waiting up to ~1s for the GC thread to pick it up
	check_if_want_terminate();

	// The stream is the whole answer: how the run ended is its last line ("Done:
	// N updated, M failed"), the status was sent with the first byte. Another
	// response now would only be appended to it
	return 1;
}

int api_action_restartDNS(struct lorentz_conn *api)
{
	if(!config.webserver.api.allow_destructive.v.b)
		return send_json_error(api, 403,
		                       "forbidden",
		                       "Restarting DNS is not allowed",
		                       "Check setting webserver.api.allow_destructive");

	restart_lorentz("API action request");

	return send_json_success(api);
}

int api_action_flush_logs(struct lorentz_conn *api)
{
	if(!config.webserver.api.allow_destructive.v.b)
		return send_json_error(api, 403,
		                       "forbidden",
		                       "Flushing the logs is not allowed",
		                       "Check setting webserver.api.allow_destructive");

	log_info("Received API request to flush the logs");

	// Flush the logs
	if(flush_dnsmasq_log())
		return send_json_success(api);
	else
		return send_json_error(api, 500,
		                       "server_error",
		                       "Cannot flush the logs",
		                       NULL);
}

int api_action_flush_network(struct lorentz_conn *api)
{
	if(!config.webserver.api.allow_destructive.v.b)
		return send_json_error(api, 403,
		                       "forbidden",
		                       "Flushing the network tables is not allowed",
		                       "Check setting webserver.api.allow_destructive");

	log_info("Received API request to flush the network tables");

	// Flush the network tables
	if(flush_network_table())
		return send_json_success(api);
	else
		return send_json_error(api, 500,
		                       "server_error",
		                       "Cannot flush the network tables",
		                       NULL);
}
