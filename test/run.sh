#!/bin/bash

# Skip tests on targets not supporting them
if [[ ${TEST} == "false" ]]; then
  echo "Skipping tests (CI_ARCH: ${CI_ARCH})!"
  exit 0
fi

# Create lorentz user if it does not exist
if ! id -u lorentz &> /dev/null; then
  useradd -m -s /usr/sbin/nologin lorentz
fi

# Kill possibly running lorentz process
while pidof -s lorentz > /dev/null; do
  pid="$(pidof -s lorentz)"
  echo "Terminating running lorentz process with PID ${pid}"
  kill "$pid"
  sleep 1
done

# Clean up possible old files from earlier test runs
rm -rf /etc/lorentz /etc/dnsmasq.d /var/log/lorentz /dev/shm/Lorentz-*
rm -f /tmp/dnsmasq_warnings /tmp/lorentz_test_*.json

# Create necessary directories and files
mkdir -p /home/lorentz /etc/lorentz /run/lorentz /var/log/lorentz /etc/lorentz/config_backups /var/www/html
echo "" > /var/log/lorentz/lorentz.log
echo "" > /var/log/lorentz/lorentz.log
echo "" > /var/log/lorentz/webserver.log
touch /run/lorentz.pid ptr.log
touch /etc/lorentz/dhcp.leases
chown -R lorentz:lorentz /etc/lorentz /run/lorentz /var/log/lorentz
chown lorentz:lorentz /run/lorentz.pid

# Copy binary into a location the new user lorentz can access
cp ./lorentz /home/lorentz/lorentz
chmod +x /home/lorentz/lorentz
# Note: We cannot add CAP_NET_RAW and CAP_NET_ADMIN at this point
setcap CAP_NET_BIND_SERVICE+eip /home/lorentz/lorentz

# Prepare gravity database
./lorentz sqlite3 /etc/lorentz/gravity.db < test/gravity.db.sql
chown lorentz:lorentz /etc/lorentz/gravity.db

# Prepare lorentz database
rm -rf /etc/lorentz/lorentz.db
./lorentz sqlite3 /etc/lorentz/lorentz.db < test/lorentz.db.sql
chown lorentz:lorentz /etc/lorentz/lorentz.db

# Prepare TLS key and certificate
cp test/test.pem /etc/lorentz/test.pem
cp test/test.crt /etc/lorentz/test.crt

# Prepare lorentz.toml
cp test/lorentz.toml /etc/lorentz/lorentz.toml
chown lorentz:lorentz /etc/lorentz/lorentz.toml

# Prepare 01-lorentz-tests.conf
mkdir -p /etc/dnsmasq.d
cp test/01-lorentz-tests.conf /etc/dnsmasq.d/01-lorentz-tests.conf

# Prepare versions file (read by /api/version)
cp test/versions /etc/lorentz/versions

# Prepare Lua test script
cp test/broken_lua.lp /var/www/html/broken_lua.lp
cp test/broken_lua_2.lp /var/www/html/broken_lua_2.lp

# Prepare local powerDNS resolver
if ! bash test/pdns/setup.sh; then
  echo "Local PowerDNS setup failed, the DNS tests below cannot pass"
  exit 1
fi

# Set restrictive umask
OLDUMASK=$(umask)
umask 0022

# Set exemplary config value by environment variable
export LORENTZCONF_misc_nice="-11"
export LORENTZCONF_dns_upstrrr="-11"
export LORENTZCONF_debug_api="not_a_bool"
export LORENTZCONF_MISC_CHECK_SHMEM=91
export LORENTZCONF_files_pcap='*123#./test/pcap'

# Start Lorentz
if ! su lorentz -s /bin/sh -c /home/lorentz/lorentz; then
  echo "lorentz failed to start"
  exit 1
fi

# Give Lorentz some time for startup preparations
sleep 2

# Optionally attach gdb for crash backtraces (opt-in via GDB=1)
if [[ "${GDB}" == "1" ]]; then
  echo "handle SIGHUP nostop SIGPIPE nostop SIGTERM nostop SIG32 nostop SIG33 nostop SIG34 nostop SIG35 nostop SIG41 nostop" > /root/.gdbinit
  gdb -p $(cat /run/lorentz.pid) --ex continue --ex "bt full" &
fi

# Print versions of lorentz
echo -n "Lorentz version (DNS): "
dig TXT CHAOS version.Lorentz @127.0.0.1 +short
echo "Lorentz verbose version (CLI): "
/home/lorentz/lorentz -vv
echo -n "Contained dnsmasq version (DNS): "
dig TXT CHAOS version.bind @127.0.0.1 +short

RET=0

# Prepare BATS
if [ -z "$BATS" ]; then
  mkdir -p test/libs
  git clone --depth=1 --quiet https://github.com/bats-core/bats-core test/libs/bats-core > /dev/null
  git clone --depth=1 --quiet https://github.com/bats-core/bats-support test/libs/bats-support > /dev/null
  git clone --depth=1 --quiet https://github.com/bats-core/bats-assert  test/libs/bats-assert > /dev/null
  git clone --depth=1 --quiet https://github.com/bats-core/bats-file    test/libs/bats-file > /dev/null
  BATS=${PWD}/test/libs/bats-core/bin/bats
  # BATS_LIB_PATH needs to be an absolute path for bats to find the libraries
  BATS_LIB_PATH=${PWD}/test/libs/
  export BATS_LIB_PATH
fi

# Run BATS test suite (includes DNS, regex, CLI, config tests;
# Lorentz remains running for the pytest API tests afterwards)
echo "Running BATS test suite..."
$BATS -p "test/test_suite.bats"
RET=$?

# Trigger network table update (PARSE_NEIGHBOR_CACHE) so mock-hwaddr
# devices like ip-127.0.0.1 exist before pytest checks them.
# RT signal offset 5 maps to PARSE_NEIGHBOR_CACHE in signals.c.
kill -SIGRTMIN+5 "$(cat /run/lorentz.pid)" 2>/dev/null
sleep 2

# Run pytest API tests (Lorentz is still running — BATS no longer terminates it)
# Skip on riscv64 — the emulated runner is too slow for the full API suite
if [[ "${CI_ARCH}" != "linux/riscv64" ]]; then
  echo "Running pytest API tests..."
  python3 -m pytest test/api/ -v
  PYTEST_RET=$?
  if [[ $PYTEST_RET != 0 ]]; then
    RET=$PYTEST_RET
  fi
else
  echo "Skipping pytest API tests (too slow on ${CI_ARCH})"
fi

# Run final BATS suite — log validation and Lorentz termination
# This runs after both test_suite.bats and pytest to catch any
# unexpected log messages from the entire run, then terminates Lorentz.
echo ""
echo "Running final log validation..."
$BATS -p "test/test_final.bats"
FINAL_RET=$?
if [[ $FINAL_RET != 0 ]]; then
  RET=$FINAL_RET
fi

curl_to_tricorder() {
  curl --silent --upload-file "${1}" https://tricorder.pi-hole.net
}

if [[ $RET != 0 ]]; then
  echo -n "lorentz/lorentz.log: "
  curl_to_tricorder /var/log/lorentz/lorentz.log
  echo ""
  echo -n "lorentz/lorentz.log: "
  curl_to_tricorder /var/log/lorentz/lorentz.log
  echo ""
  echo -n "ptr.log: "
  curl_to_tricorder ./ptr.log
  echo ""
  echo -n "webserver.log: "
  curl_to_tricorder /var/log/lorentz/webserver.log
  echo ""
  echo -n "lorentz.toml: "
  curl_to_tricorder /etc/lorentz/lorentz.toml
  echo ""
fi

# Restore umask
umask "$OLDUMASK"

# Run performance tests (opt-in via RUN_PERF_TEST=1)
if [[ "${RUN_PERF_TEST}" == "1" ]]; then
  if ! su lorentz -s /bin/sh -c "/home/lorentz/lorentz --perf"; then
    echo "lorentz --perf failed to start"
  fi
fi

# Remove copied file
rm /home/lorentz/lorentz

# Stop local powerDNS resolver
killall pdns_server
killall pdns_recursor

# Exit with return code of bats tests
exit $RET
