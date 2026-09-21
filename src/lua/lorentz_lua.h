/* Lorentz: A black hole for Internet advertisements
*  (c) 2020 Pi-hole, LLC (https://pi-hole.net)
*  Network-wide ad blocking via your own hardware.
*
*  Lorentz Engine
*  LUA prototypes
*
*  This file is copyright under the latest version of the EUPL.
*  Please see LICENSE file for your rights under this license. */
#ifndef LORENTZ_LUA_H
#define LORENTZ_LUA_H

#include "lua.h"
#include <stdbool.h>

#define LUA_HISTORY_FILE "~/.lorentz_lua_history"

int run_lua_interpreter(const int argc, char **argv, bool debug);
int run_luac(const int argc, char **argv);

int lua_main (int argc, char **argv);
int luac_main (int argc, char **argv);

void print_embedded_scripts(void);
void lorentz_lua_init(lua_State *L);

#endif //LORENTZ_LUA_H
