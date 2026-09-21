#!/usr/bin/env bats
# Final log validation and Lorentz termination tests.
# This file runs AFTER both test_suite.bats and the pytest API tests
# to catch any unexpected log messages produced during the entire run.

# Load BATS libraries for enhanced testing capabilities
bats_load_library 'bats-support'
bats_load_library 'bats-assert'
load 'bats_helper.bash'

@test "No WARNING messages in lorentz.log (besides known warnings)" {
  run bash -c 'grep "WARNING:" /var/log/lorentz/lorentz.log | grep -v -E "CAP_NET_ADMIN|CAP_NET_RAW|CAP_SYS_NICE|CAP_IPC_LOCK|CAP_CHOWN|CAP_NET_BIND_SERVICE|CAP_SYS_TIME|LORENTZCONF_|(negative DS reply without NS record received for ([a-z0-9-]+\.)*(lorentz|icloud\.com|apple-dns\.net|in-addr\.arpa|ip6\.arpa),)|(nameserver 127.0.0.1 refused to do a recursive query)|API: Config item is invalid|API: Config item validation failed|API: Not found|API: Config items set via environment variables|API: Rate-limiting login attempts|API: You need to specify both|API: No request body data|API: Invalid request|API: Rate-limiting 2FA token requests|2FA code has already been used|API: Reused 2FA token|(Teleporter import skipped )"'
  refute_output
}

@test "No ERROR messages in lorentz.log (besides known/intended errors)" {
  run bash -c 'grep "ERROR: " /var/log/lorentz/lorentz.log | grep -v -E "(index\.html)|(Failed to create shared memory object)|(LORENTZCONF_debug_api is not a boolean)|(LORENTZCONF_files_pcap)|(Failed to set|adjust time during NTP sync: Insufficient permissions)|(nlrequest error)|(Failed to read ARP cache)|(Teleporter: )"'
  refute_output
}

@test "No CRIT messages in lorentz.log (besides error due to starting Lorentz more than once)" {
  run bash -c 'grep "CRIT:" /var/log/lorentz/lorentz.log | grep -v "CRIT: lorentz is already running"'
  refute_output
}

@test "No \"DB not available\" messages in lorentz.log" {
  run bash -c 'grep -c "database not available" /var/log/lorentz/lorentz.log'
  assert_line --index 0 "0"
}

@test "Expected number of config file rotations" {
  # BATS:   1x lorentz.toml write (dns.reply.host API PATCH)
  # BATS:   2x lorentz.toml writes (CLI password set/remove processes)
  # pytest: 3x lorentz.toml writes (password, app_pwhash, serve_all via API)
  # pytest: 2x lorentz.toml writes (dns/hosts config array PUT + DELETE)
  # pytest: 2x lorentz.toml writes (excludeDomains config array PUT + DELETE)
  # pytest: 2x lorentz.toml writes (dns/blocking disable + enable)
  # pytest: 4x lorentz.toml writes (config PATCH round-trips: bool + int, change + restore each)
  # pytest: 2x lorentz.toml writes (auth stress test password set + remove)
  # pytest: 2x lorentz.toml writes (TOTP stress test secret set + remove)
  # pytest: 2x lorentz.toml writes (auth security test password set + remove)
  # pytest: 2x lorentz.toml writes (auth security test TOTP secret set + remove)
  # pytest: 2x lorentz.toml writes (top_domains exclude filter set + reset)
  run bash -c 'grep -c "INFO: Config file written to /etc/lorentz/lorentz.toml" /var/log/lorentz/lorentz.log'
  printf "lorentz.toml write count: %s\n" "${lines[0]}"
  # On RISCV64, pytest is skipped (too slow), so only BATS writes occur
  if [[ "${CI_ARCH}" == "linux/riscv64" ]]; then
      assert_line --index 0 "1"
  else
    [[ ${lines[0]} == "26" ]]
  fi
  # CLI password set/remove trigger inotify reload but result in
  # "lorentz.toml unchanged" as the in-memory config already matches
  run bash -c 'grep -c "lorentz.toml unchanged" /var/log/lorentz/lorentz.log'
  printf "lorentz.toml unchanged count: %s\n" "${lines[0]}"
  [[ ${lines[0]} -ge 2 ]]
  assert_success
  run bash -c 'grep -c "DEBUG_CONFIG: Config file written to /etc/lorentz/dnsmasq.conf" /var/log/lorentz/lorentz.log'
  printf "dnsmasq.conf write count: %s\n" "${lines[0]}"
  assert_line --index 0 "1"
  run bash -c 'grep -c "DEBUG_CONFIG: HOSTS file written to /etc/lorentz/hosts/custom.list" /var/log/lorentz/lorentz.log'
  printf "custom.list write count: %s\n" "${lines[0]}"
  # On RISCV64, pytest is skipped, so only BATS writes occur (5x)
  # Otherwise, pytest dns/hosts config array PUT + DELETE add 2 more (7x)
  if [[ "${CI_ARCH}" == "linux/riscv64" ]]; then
    assert_line --index 0 "5"
  else
    assert_line --index 0 "7"
  fi
}

@test "Query with ID 0 has been saved to the database" {
  # Lorentz exports queries from in-memory DB to disk after a configurable
  # delay (default 30s). Poll up to 60s for the export to complete.
  for i in $(seq 1 30); do
    run bash -c './lorentz sqlite3 /etc/lorentz/lorentz.db "SELECT COUNT(*) FROM queries WHERE id=0;"'
    if [[ ${lines[0]} == "1" ]]; then
      break
    fi
    sleep 2
  done
  assert_line --index 0 "1"
}

@test "Lorentz terminates with message" {
  logsize_before=$(stat -c%s /var/log/lorentz/lorentz.log)
  # Kill lorentz after having completed all tests
  pid=$(cat /run/lorentz.pid)
  printf "Killing lorentz with PID %s\n" "$pid"

  run bash -c "kill $pid"
  assert_success

  # Wait until lorentz has terminated
  run bash -c "./lorentz wait-for '########## Lorentz terminated after' /var/log/lorentz/lorentz.log 30 $logsize_before"
  assert_success
}

@test "Shutdown reason logged at INFO level (#2818)" {
  # Verify the shutdown path now logs at INFO level instead of DEBUG-only
  run bash -c 'grep "INFO: Shutting down (exit code" /var/log/lorentz/lorentz.log'
  assert_success
}

@test "SIGTERM source re-logged near final termination message (#2818)" {
  # Verify the SIGTERM sender is re-logged during cleanup so it appears
  # near the "Lorentz terminated" message even in truncated logs
  run bash -c 'grep "INFO: Terminated by" /var/log/lorentz/lorentz.log'
  assert_success

  # Verify ordering: "Terminated by" must appear AFTER "Shutting down" and
  # BEFORE the final "Lorentz terminated" message
  run bash -c 'grep -n "Shutting down (exit code\|Terminated by\|Lorentz terminated after" /var/log/lorentz/lorentz.log | tail -3'
  assert_line --partial --index 0 "Shutting down (exit code"
  assert_line --partial --index 1 "Terminated by"
  assert_line --partial --index 2 "Lorentz terminated after"
}
