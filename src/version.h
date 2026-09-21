/* Lorentz: A black hole for Internet advertisements
*  (c) 2025 Pi-hole, LLC (https://pi-hole.net)
*  Network-wide ad blocking via your own hardware.
*
*  Lorentz Engine
*  Version-related prototype declarations
*
*  This file is copyright under the latest version of the EUPL.
*  Please see LICENSE file for your rights under this license. */

#ifndef VERSION_H
#define VERSION_H

const char *git_version(void) __attribute__ ((const));
const char *git_date(void) __attribute__ ((const));
const char *git_branch(void) __attribute__ ((const));
const char *git_tag(void) __attribute__ ((const));
const char *git_hash(void) __attribute__ ((const));
const char *lorentz_arch(void) __attribute__ ((const));
const char *lorentz_cc(void) __attribute__ ((const));

#endif // VERSION_H
