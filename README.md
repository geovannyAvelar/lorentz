> [!IMPORTANT]
> **Lorentz is a fork of [Pi-hole FTL](https://github.com/pi-hole/FTL).**
> It is based on FTL v6.7.1 and keeps the full upstream git history. It is an independent project: it is
> **not affiliated with, endorsed by, or supported by Pi-hole, LLC or the Pi-hole project**, and it is not a
> drop-in replacement for `pihole-FTL`. Names, paths, environment variables and the config file were renamed
> (for example the binary is `lorentz`, the configuration lives in `/etc/lorentz/lorentz.toml` and options are
> set with `LORENTZCONF_` variables), so existing Pi-hole installations, the Pi-hole web interface and the
> Pi-hole documentation do not work with it unchanged.
>
> The main change is a database abstraction layer: all database access goes through a driver interface
> (`src/database/db-driver.*`) with a SQLite driver, covered by regression harnesses and container based
> integration tests (`test/`).
>
> Pi-hole and FTL are the work and, where applicable, trademarks of Pi-hole, LLC. This project keeps the
> original licence (EUPL-1.2) and all upstream copyright notices. Please report problems with this fork here,
> not to the Pi-hole project.

<h1 align="center">Lorentz</h1>

<p align="center">
  <strong>Network-wide ad blocking via your own Linux hardware</strong>
</p>

Lorentz (`lorentz`) provides an interactive API and also generates statistics for the [Pi-hole®](https://pi-hole.net/trademark-rules-and-brand-guidelines/) web interface.

- **Fast**: stats are read directly from memory by coupling our codebase closely with `dnsmasq`
- **Versatile**: upstream changes to `dnsmasq` can quickly be merged in without much conflict
- **Lightweight**: runs smoothly with [minimal hardware and software requirements](https://discourse.pi-hole.net/t/hardware-software-requirements/273) such as Raspberry Pi Zero
- **Interactive**: our API can be used to interface with your projects
- **Insightful**: stats normally reserved inside of `dnsmasq` are made available so you can see what's really happening on your network

## Database drivers

All database access goes through a driver interface (`src/database/db-driver.h`). The default driver
is SQLite. A PostgreSQL driver (`src/database/db-postgres.c`, on libpq) is built with
`-DUSE_POSTGRESQL=ON` and needs `libpq-dev`.

### Running Lorentz on PostgreSQL

Point `files.database` (`LORENTZCONF_files_database`) at a connection URI instead of a file:

```toml
[files]
  database = "postgresql://lorentz:password@db.example/lorentz"
```

Lorentz creates its tables on the first start (`db_schema_baseline()` in `src/database/db-schema.c`). Use a
database or a schema of its own: `?options=-c%20search_path%3Dlorentz` in the URI selects a schema. The
URI is stored in `lorentz.toml`, where the password is readable to whoever can read that file, and the
logs mask it.

What is on PostgreSQL is the **long-term database**: the query history, the counters, the network table,
the messages, the sessions and the alias-clients. Three things stay in SQLite files whatever `files.database`
is, because they are private to one host or are produced by tools that write SQLite:

- the in-memory query database (a cache of the last `webserver.api.maxHistory` seconds, filled from the
  server on start and written back every `database.DBinterval` seconds and on shutdown),
- `gravity.db` (written by `pihole -g` and the gravity tools),
- the MAC vendor database.

Differences to be aware of: SQLite migrations are not replayed (the schema of the current version is created
in one step, and a database of an older version is refused), `LIKE` ignores the case of letters as it does in
SQLite, and a change of the schema after version 22 has to be written for both databases. Lorentz opens a
connection for each request, so use a pooler such as PgBouncer when many API clients are expected.

The driver is described at the top of `src/database/db-postgres.c`, the tests in `test/integration/README.md`.

## Documentation

Lorentz has no documentation of its own yet. The documentation of the upstream project, Pi-hole FTLDNS, can be found [here](https://docs.pi-hole.net/ftldns/). It applies only where this fork did not change the behavior (see the notice at the top).

## Installation

Lorentz (`lorentz`) is not installed by Pi-hole. Build it from source with `./build.sh`.

### IMPORTANT

>Lorentz will *disable* any existing installations of `dnsmasq`. This is because Lorentz *is* `dnsmasq` + its own code, so both cannot run simultaneously.
