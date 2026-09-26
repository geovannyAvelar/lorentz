/* Lorentz: A black hole for Internet advertisements
*  (c) 2020 Pi-hole, LLC (https://pi-hole.net)
*  Network-wide ad blocking via your own hardware.
*
*  Lorentz Engine
*  SQLite3 database engine extension prototypes
*
*  This file is copyright under the latest version of the EUPL.
*  Please see LICENSE file for your rights under this license. */

#ifndef SQLITE3_EXT_H
#define SQLITE3_EXT_H

// int64_t
#include <stdint.h>
// size_t
#include <stddef.h>

struct sqlite3_memory_usage {
        int64_t total;
        int64_t highwater;
        int64_t largest_block;
        size_t current_allocations;
};

// Bits of the network of a client table entry that an address belongs to, 0 if
// none (see the definition)
int subnet_match_bits(const char *addrDBcidr, const char *addrLorentz);

// Initialization point for SQLite3 extensions
void lorentz_sqlite3_initalize(void);
struct sqlite3_memory_usage *sqlite3_mem_used(void) __attribute__((const));

#endif // SQLITE3_EXT_H
