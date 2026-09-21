# Container based integration tests

These tests run a real `pihole-FTL` binary in a container and check, from the
outside, that FTL behaves normally. They exist to catch regressions in
everything that sits on top of the database layer: the long-term database, the
in-memory query database, the gravity database, sessions, the network table,
messages and Teleporter.

The unit-level harnesses (`test/db_driver_regression.c` and
`test/db_layer_regression.c`) cover the database code in isolation. These tests
cover what only a running FTL can show: the migration of a database on startup,
DNS decisions that depend on gravity and on a client's groups, the shared
memory paths, the REST API on top of the databases, and a restart that must
keep the data.

## What is tested

| Scenario | Checks |
|---|---|
| Startup | Every migration from version 1 runs, no `ERROR` in `FTL.log`, integrity check, tables and view exist |
| DNS | Local record, gravity block, exact denylist block, exact allowlist pass, query log and its filters, summary |
| Lists through the API | Add, read and delete an entry, invalid regex refused, a client moved to another group (and back), a rebuilt gravity database picked up |
| Network and messages | The client shows up as a device, messages are listed and deleted |
| Teleporter | Export as ZIP and import it again |
| Restart | The history survives in the long-term database and is imported again, long-term statistics and history endpoints answer, shutdown is free of database errors |
| Sessions | Login, a session survives a restart, logout |
| Upgrade | The version 9 database the bats suite starts from is migrated to the current version and keeps its network table and alias-clients |

## Running

You need Node.js 20 or newer and a running Docker daemon.

```bash
./build.sh                                  # build pihole-FTL
npm --prefix test/integration install
npm --prefix test/integration test
```

The binary under test is `FTL_BINARY` if it is set, otherwise the most recent of
`./pihole-FTL` and `./cmake/pihole-FTL`. The test prints which one it uses. The
image is built on `ubuntu:24.04`, so the binary has to be built for the same
kind of system (a dynamically linked build from an Ubuntu or Debian host, or a
static build).

Comparing against another build is the way to prove that a change did not alter
behavior: build the old revision (for example in a `git worktree`) and run the
same suite against it.

```bash
FTL_BINARY=/path/to/old/pihole-FTL npm --prefix test/integration test
```

Both must pass. A test that fails for both is a wrong expectation of the
test, one that fails only for the new binary is a regression.
