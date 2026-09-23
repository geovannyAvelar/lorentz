/* Lorentz: A black hole for Internet advertisements
*  (c) 2026 Pi-hole, LLC (https://pi-hole.net)
*  Network-wide ad blocking via your own hardware.
*
*  Lorentz Engine
*  Embedded web UI (stub)
*
*  Used instead of the generated web UI source when EMBED_WEBUI is off, so
*  webui_handler() (webui.c) and the rest of the build need no #ifdef: the
*  admin path simply 404s until Lorentz is built with -DEMBED_WEBUI=ON (needs
*  Node.js/npm - see README.md, "Web UI").
*
*  This file is copyright under the latest version of the EUPL.
*  Please see LICENSE file for your rights under this license. */

#include "webui.h"

const struct webui_file webui_files[1] = { { NULL, NULL, NULL, 0, NULL } };
const size_t webui_files_count = 0;
