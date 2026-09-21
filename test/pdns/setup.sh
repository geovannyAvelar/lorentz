#!/bin/bash
set -euo pipefail

echo "************ Installing PowerDNS configuration ************"

# Delete possibly existing zone database. Include the WAL/SHM siblings —
# leaving them behind lets SQLite replay stale transactions against the
# freshly created database on the next open.
mkdir -p /var/lib/powerdns/
rm -f /var/lib/powerdns/pdns.sqlite3*

# Install config files
if [ -d /etc/powerdns ]; then
  # Debian
  cp test/pdns/pdns.conf /etc/powerdns/pdns.conf
  RECURSOR_CONF=/etc/powerdns/recursor.conf
elif [ -d /etc/pdns ]; then
  cp test/pdns/pdns.conf /etc/pdns/pdns.conf
  if [ -d /etc/pdns-recursor ]; then
    # Fedora
    RECURSOR_CONF=/etc/pdns-recursor/recursor.conf
  else
    # Alpine
    RECURSOR_CONF=/etc/pdns/recursor.conf
  fi
else
  echo "Error: Unable to determine powerDNS config directory"
  exit 1
fi

cp test/pdns/luadns.lua /etc/pdns/luadns.lua
cp test/pdns/recursor.conf $RECURSOR_CONF

# Create zone database
if [ -f /usr/share/doc/pdns-backend-sqlite3/schema.sqlite3.sql ]; then
  # Debian
  ./lorentz sqlite3 /var/lib/powerdns/pdns.sqlite3 < /usr/share/doc/pdns-backend-sqlite3/schema.sqlite3.sql
elif [ -f /usr/share/doc/pdns/schema.sqlite3.sql ]; then
  # Alpine
  ./lorentz sqlite3 /var/lib/powerdns/pdns.sqlite3 < /usr/share/doc/pdns/schema.sqlite3.sql
else
  echo "Error: powerDNS SQL schema not found"
  exit 1
fi
# Create zone lorentz
pdnsutil zone create lorentz ns1.lorentz

# Create A records
pdnsutil rrset add lorentz. a.lorentz. A 192.168.1.1
pdnsutil rrset add lorentz. gravity.lorentz. A 192.168.1.2
pdnsutil rrset add lorentz. denied.lorentz. A 192.168.1.3
pdnsutil rrset add lorentz. allowed.lorentz. A 192.168.1.4
pdnsutil rrset add lorentz. gravity-allowed.lorentz. A 192.168.1.5
pdnsutil rrset add lorentz. antigravity.lorentz. A 192.168.1.6
pdnsutil rrset add lorentz. x.y.z.abp.antigravity.lorentz. A 192.168.1.7
pdnsutil rrset add lorentz. regex1.lorentz. A 192.168.2.1
pdnsutil rrset add lorentz. regex2.lorentz. A 192.168.2.2
pdnsutil rrset add lorentz. regex5.lorentz. A 192.168.2.3
pdnsutil rrset add lorentz. regexA.lorentz. A 192.168.2.4
pdnsutil rrset add lorentz. regex-REPLYv4.lorentz. A 192.168.2.5
pdnsutil rrset add lorentz. regex-REPLYv6.lorentz. A 192.168.2.6
pdnsutil rrset add lorentz. regex-REPLYv46.lorentz. A 192.168.2.7
pdnsutil rrset add lorentz. regex-A.lorentz. A 192.168.2.8
pdnsutil rrset add lorentz. regex-notA.lorentz. A 192.168.2.9
pdnsutil rrset add lorentz. any.lorentz. A 192.168.3.1

# Create AAAA records
pdnsutil rrset add lorentz. aaaa.lorentz. AAAA fe80::1c01
pdnsutil rrset add lorentz. regex-REPLYv4.lorentz. AAAA fe80::2c01
pdnsutil rrset add lorentz. regex-REPLYv6.lorentz. AAAA fe80::2c02
pdnsutil rrset add lorentz. regex-REPLYv46.lorentz. AAAA fe80::2c03
pdnsutil rrset add lorentz. any.lorentz. AAAA fe80::3c01
pdnsutil rrset add lorentz. gravity-aaaa.lorentz. AAAA fe80::4c01

# Create CNAME records
pdnsutil rrset add lorentz. cname-1.lorentz. CNAME gravity.lorentz.
pdnsutil rrset add lorentz. cname-2.lorentz. CNAME cname-1.lorentz.
pdnsutil rrset add lorentz. cname-3.lorentz. CNAME cname-2.lorentz.
pdnsutil rrset add lorentz. cname-4.lorentz. CNAME cname-3.lorentz.
pdnsutil rrset add lorentz. cname-5.lorentz. CNAME cname-4.lorentz.
pdnsutil rrset add lorentz. cname-6.lorentz. CNAME cname-5.lorentz.
pdnsutil rrset add lorentz. cname-7.lorentz. CNAME cname-6.lorentz.
pdnsutil rrset add lorentz. cname-ok.lorentz. CNAME a.lorentz.

# Create CNAME for SOA test domain
pdnsutil rrset add lorentz. soa.lorentz. CNAME lorentz.

# Create CNAME for NODATA tests
pdnsutil rrset add lorentz. aaaa-cname.lorentz. CNAME gravity-aaaa.lorentz.
pdnsutil rrset add lorentz. a-cname.lorentz. CNAME gravity.lorentz.

# Create PTR records
pdnsutil rrset add lorentz. ptr.lorentz. PTR ptr.lorentz.

# Other testing records
pdnsutil rrset add lorentz. srv.lorentz. SRV "0 1 80 a.lorentz"
pdnsutil rrset add lorentz. txt.lorentz. TXT "\"Some example text\""
# We want this to output $1 without expansion
# shellcheck disable=SC2016
pdnsutil rrset add lorentz. naptr.lorentz. NAPTR '10 10 "u" "smtp+E2U" "!.*([^\.]+[^\.]+)$!mailto:postmaster@$1!i" .'
pdnsutil rrset add lorentz. naptr.lorentz. NAPTR '20 10 "s" "http+N2L+N2C+N2R" "" lorentz.'
pdnsutil rrset add lorentz. mx.lorentz. MX "50 ns1.lorentz."

# SVCB + HTTPS
pdnsutil rrset add lorentz. svcb.lorentz. SVCB '1 port="80"'
pdnsutil rrset add lorentz. regex-multiple.lorentz. SVCB '1 port="80"'
pdnsutil rrset add lorentz. regex-notMultiple.lorentz. SVCB '1 port="80"'

# HTTPS
pdnsutil rrset add lorentz. https.lorentz. HTTPS '1 . alpn="h3,h2"'
pdnsutil rrset add lorentz. regex-multiple.lorentz. HTTPS '1 . alpn="h3,h2"'
pdnsutil rrset add lorentz. regex-notMultiple.lorentz. HTTPS '1 . alpn="h3,h2"'

# ANY
pdnsutil rrset add lorentz. regex-multiple.lorentz. A 192.168.3.12
pdnsutil rrset add lorentz. regex-multiple.lorentz. AAAA fe80::3f41
pdnsutil rrset add lorentz. regex-notMultiple.lorentz. A 192.168.3.12
pdnsutil rrset add lorentz. regex-notMultiple.lorentz. AAAA fe80::3f41

# TXT
pdnsutil rrset add lorentz. any.lorentz. TXT "\"Some example text\""

# NOERROR: Create a record that returns NOERROR but no data
pdnsutil rrset add lorentz. noerror.lorentz. NS ns1.lorentz.

# Blocked Cisco Umbrella IP (https://support.opendns.com/hc/en-us/articles/227986927-What-are-the-Cisco-Umbrella-Block-Page-IP-Addresses)
pdnsutil rrset add lorentz. umbrella.lorentz. A 146.112.61.104
pdnsutil rrset add lorentz. umbrella.lorentz. AAAA ::ffff:9270:3d68 #::ffff:146.112.61.104

# Special record which consists of both blocked and non-blocked IP
pdnsutil rrset add lorentz. umbrella-multi.lorentz. A 1.2.3.4
pdnsutil rrset add lorentz. umbrella-multi.lorentz. A 146.112.61.104
pdnsutil rrset add lorentz. umbrella-multi.lorentz. A 8.8.8.8

# Null address
pdnsutil rrset add lorentz. null.lorentz. A 0.0.0.0
pdnsutil rrset add lorentz. null.lorentz. AAAA ::

# Serve Apple's iCloud Private Relay domains locally (unsigned) instead of
# recursing to the public internet. The bats suite resolves mask.icloud.com
# through an allowlisted client; hosting the mask.icloud.com -> mask.apple-dns.net
# CNAME chain here keeps the query counts deterministic regardless of whether
# Apple currently DNSSEC-signs these zones (see test/api/test_api.py).
pdnsutil zone create icloud.com ns1.lorentz
pdnsutil rrset add icloud.com. mask.icloud.com. CNAME mask.apple-dns.net.
pdnsutil rrset add icloud.com. mask-h2.icloud.com. CNAME mask.apple-dns.net.
pdnsutil zone create apple-dns.net ns1.lorentz
pdnsutil rrset add apple-dns.net. mask.apple-dns.net. A 172.224.181.14

# Create valid internal DNSSEC zone
pdnsutil zone create dnssec ns1.lorentz
pdnsutil rrset add dnssec. a.dnssec. A 192.168.4.1
pdnsutil rrset add dnssec. aaaa.dnssec. AAAA fe80::4c01
pdnsutil zone secure dnssec
# Export zone DS records and convert to dnsmasq trust-anchor format
# Example:
#   dnssec. IN DS 42206 8 2 6d2007e292483fa061db37011676d9592649d1600e5b2ece1326f792ebedd412 ; ( SHA256 digest )
# --->
#   trust-anchor=dnssec.,42206,8,2,6d2007e292483fa061db37011676d9592649d1600e5b2ece1326f792ebedd412
pdnsutil zone export-ds dnssec. | awk 'NR==1{FS=" "; OFS=""; print "trust-anchor=",$1,",",$4,",",$5,",",$6,",",$7}' > /etc/dnsmasq.d/02-trust-anchor.conf

# Create a locally-signed root zone so DNSSEC validation never leaves the test
# environment. Lorentz ships the real ICANN root trust anchors, so without a local
# root dnsmasq would fetch the live root DNSKEY (whose key set drifts with ICANN
# rollovers). Serving a signed root here and trusting its key keeps it hermetic.
pdnsutil zone create . ns1.
pdnsutil zone secure .
pdnsutil zone export-ds . | awk 'NR==1{FS=" "; OFS=""; print "trust-anchor=",$1,",",$4,",",$5,",",$6,",",$7}' >> /etc/dnsmasq.d/02-trust-anchor.conf

# Create intentionally broken DNSSEC (BOGUS) zone
# The only difference to above is that this zone is signed with a key that is
# not in the trust chain
# It will cause the DNSSEC validation to fail with error message:
#   unsupported DS digest
pdnsutil zone create bogus ns1.lorentz
pdnsutil rrset add bogus. a.bogus. A 192.168.5.1
pdnsutil rrset add bogus. aaaa.bogus. AAAA fe80::5c01
pdnsutil zone secure bogus
# Install a *deliberately mismatched* trust anchor for the bogus zone (same
# keytag/algorithm/digest-type as the real key, but a corrupted digest). This
# makes bogus validation fail as BOGUS *locally* instead of dnsmasq walking up
# to the real ICANN root to learn the zone has no secure delegation.
pdnsutil zone export-ds bogus. | \
  awk 'NR==1{OFS=""; d=$7; d=substr(d, 1, length(d) - 8) "deadbeef"; print "trust-anchor=", $1, ",", $4, ",", $5, ",", $6, ",", d}' \
  >> /etc/dnsmasq.d/02-trust-anchor.conf

# Create reverse lookup zone
pdnsutil zone create arpa ns1.lorentz
pdnsutil rrset add arpa. 1.1.168.192.in-addr.arpa. PTR lorentz.
pdnsutil rrset add arpa. 2.1.168.192.in-addr.arpa. PTR a.lorentz.
pdnsutil rrset add arpa. 1.0.c.1.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.8.e.f.ip6.arpa. PTR lorentz.
pdnsutil rrset add arpa. 2.0.c.1.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.8.e.f.ip6.arpa. PTR aaaa.lorentz.

# Delegate the unsigned child zones from the locally-signed root. Both lorentz and
# arpa are children of the '.' zone we serve, so the root has to carry an NS
# delegation for them; without it the signed root answers NXDOMAIN for their DS
# query (rather than a proper "insecure delegation" NODATA proof) and
# `pdnsutil zone check` reports "No delegation ... in parent '.'". They are
# unsigned, so an NS record with no DS is the correct, secure-parent way to mark
# them as an insecure delegation.
pdnsutil rrset add . lorentz.  NS ns1.lorentz.
pdnsutil rrset add . arpa. NS ns1.lorentz.

# Calculates the ‘ordername’ and ‘auth’ fields for all zones so they comply with
# DNSSEC settings. Can be used to fix up migrated data. Can always safely be
# run, it does no harm.
pdnsutil zone rectify-all

# Do final checking
pdnsutil zone check lorentz
pdnsutil zone check arpa

pdnsutil zone list-all

echo "********* Done installing PowerDNS configuration **********"

# Stop any previously running powerDNS daemons. killall is asynchronous,
# so wait until each process is actually gone before starting the new
# instance — otherwise the new daemon can fail to bind its port silently.
for proc in pdns_server pdns_recursor; do
  killall "$proc" 2> /dev/null || true
  for _ in 1 2 3 4 5 6 7 8 9 10; do
    pidof "$proc" > /dev/null 2>&1 || break
    sleep 0.2
  done
  killall -9 "$proc" 2> /dev/null || true
done

# Have to create the socketdir or the recursor will fail to start. Also
# clean stale pid/control-socket files left behind by a previous run.
mkdir -p /var/run/pdns-recursor
rm -f /var/run/pdns-recursor/*

# Wait until a pdns process answers on its port, for up to thirty seconds: a
# slow or emulated runner takes noticeably longer than a local one, and the
# tests below are meaningless against a resolver that is not up. Failing here
# says which process did not start, rather than leaving it to be inferred
wait_for_pdns() {
  local port="${1}" name="${2}" i
  for i in $(seq 1 60); do
    if dig @127.0.0.1 -p "${port}" lorentz. SOA +tries=1 +time=1 +short > /dev/null 2>&1; then
      return 0
    fi
    sleep 0.5
  done
  echo "ERROR: ${name} did not answer on port ${port} within 30 s" >&2
  return 1
}

# Start authoritative pdns_server and wait for it to accept queries on
# its configured port (5554) before continuing.
pdns_server --daemon
if ! wait_for_pdns 5554 "pdns_server"; then
  exit 1
fi

# Start pdns_recursor and wait for it to accept queries on port 5555.
pdns_recursor --daemon
if ! wait_for_pdns 5555 "pdns_recursor"; then
  exit 1
fi
