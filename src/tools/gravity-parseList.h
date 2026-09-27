/* Lorentz: A black hole for Internet advertisements
*  (c) 2023 Pi-hole, LLC (https://pi-hole.net)
*  Network-wide ad blocking via your own hardware.
*
*  Lorentz Engine
*  Gravity parseList prototypes
*
*  This file is copyright under the latest version of the EUPL.
*  Please see LICENSE file for your rights under this license. */

#ifndef GRAVITY_PARSELIST_H
#define GRAVITY_PARSELIST_H

#include "lorentz.h"
#include <stdio.h>

// How many non-domain entries of a list are kept as a sample
#define MAX_INVALID_DOMAINS 5

// What a parse of a list found
struct gravity_parse_stats {
	unsigned int exact, abp, invalid;
	// Up to MAX_INVALID_DOMAINS of the invalid entries, to show
	char *sample[MAX_INVALID_DOMAINS];
	ssize_t sample_len[MAX_INVALID_DOMAINS];
	unsigned int samples;
	bool oom;
};

struct gravity_parser {
	bool antigravity;
	// Called for every domain or ABP-style pattern, false stops the parse.
	// NULL only counts
	bool (*add)(const char *domain, void *arg);
	void *arg;
	// Print every invalid entry with its line number (the check mode)
	bool print_invalid;
	// Print the progress of a large file
	bool progress;
};

int gravity_parse_stream(FILE *fp, size_t fsize, struct gravity_parser *p, struct gravity_parse_stats *st);
void gravity_parse_stats_free(struct gravity_parse_stats *st);

int gravity_parseList(const char *infile, const char *outfile, const char *adlistID, const bool checkOnly, const bool antigravity);
bool __attribute__((pure)) valid_domain(const char *domain, const size_t len,
                                       const bool fqdn_only, const bool allow_utf8);

#endif // GRAVITY_PARSELIST_H
