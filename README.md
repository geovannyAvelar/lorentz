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

The URI may leave out what the environment supplies: libpq reads `PGHOST`, `PGPORT`, `PGUSER`,
`PGPASSWORD`, `PGDATABASE`, `PGOPTIONS`, `PGSSLMODE`, `PGPASSFILE`, `PGSERVICE` and its other variables for
whatever the URI does not say. With `LORENTZCONF_files_database=postgresql://` and the variables set, no
password is written to `lorentz.toml`:

```bash
LORENTZCONF_files_database=postgresql://
PGHOST=db.example PGUSER=lorentz PGPASSWORD=secret PGDATABASE=lorentz
PGOPTIONS="-c search_path=lorentz"
```

Lorentz creates its tables on the first start (`db_schema_baseline()` in `src/database/db-schema.c`). Use a
database or a schema of its own: `?options=-c%20search_path%3Dlorentz` in the URI selects a schema. A
URI that contains the password is stored in `lorentz.toml`, where whoever can read that file can read it, and the
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
SQLite, and a change of the schema after version 23 has to be written for both databases. Lorentz opens a
connection for each request, and keeps up to `database.pool.size` (8) of the closed ones open for the next request, for at most `database.pool.idleTimeout` (300) seconds. Each connection is reset (`DISCARD ALL`) before it is used again, and one the server has closed is replaced. Set the size to 0 to turn the pool off. The pool caps the idle connections and not the busy ones, so an external pooler such as PgBouncer still helps when many API clients are expected; use session pooling mode, and set the timeout below the one of the pooler.

The driver is described at the top of `src/database/db-postgres.c`, the tests in `test/integration/README.md`.

## User accounts

Besides the one password of `webserver.api.password`, the API has accounts, managed under `/api/users`
(the reference is served at `/api/docs`, tag "User management"):

| Request | Who | |
|---|---|---|
| `GET /api/users` | admin | list the accounts |
| `POST /api/users` | admin | create one: `username`, `password`, optional `role`, `enabled`, `comment` |
| `GET /api/users/{username}` | admin, or the account itself | read one |
| `PUT /api/users/{username}` | admin, or the account itself | change the fields that are sent |
| `DELETE /api/users/{username}` | admin | delete one and end its sessions |

Log in with `POST /api/auth` and `{"username": "alice", "password": "..."}`; without `username` the password is
the one of the configuration, as before, and has the rights of an admin. The session tells who is logged in
(`session.user`). Usernames are 1 to 64 of letters, digits and `. _ - @` and are not case sensitive; passwords are
8 to 256 characters and are stored as Balloon hashes, like the configured password.

Two roles: an **admin** may do everything, a **viewer** reads statistics, queries, lists and the like and
changes nothing, and cannot read the configuration, the logs, the sessions or the Teleporter export. Every
account may read and change the password and the comment of its own account (a new password needs
`current_password`); an admin may reset the password of others, rename, enable, disable and promote them.
A password change ends the other sessions of the account, and disabling or deleting an account ends all of them.
There always is at least one enabled admin: the last one cannot be deleted, disabled or demoted, and nobody
deletes or disables their own account.

As soon as an enabled account exists, the API needs a login even when `webserver.api.password` is empty. While
it is still open (no password, no account) the first account has to be an enabled admin, and creating it
closes the API. Accounts are kept in the `users` table of the long-term database (SQLite or PostgreSQL) and
are held in memory for the checks of each request, so Lorentz instances that share one PostgreSQL database
notice a change of another only after a restart. They are not part of the Teleporter export, the
two-factor authentication (`webserver.api.totp_secret`) applies to the configured password only.

## Web UI

`web/` has a Next.js admin console (dashboard, query log, domains, groups, lists, clients, network
devices, user accounts) built on the REST API above.

A build with `-DEMBED_WEBUI=ON` (needs Node.js/npm - see `src/api/webui/CMakeLists.txt`) folds
`web/`'s static export straight into the `lorentz` binary: Lorentz serves it itself, from memory,
at its own admin path (`webserver.paths.webhome`, `/admin/` by default), and the browser talks to
`/api/*` directly, same origin, no separate process and no CORS configuration. The embedded build
assumes the default `/admin/` webhome (baked in as `web/next.config.ts`'s `basePath` at build
time); a Lorentz configured with a different webhome needs the standalone deployment below instead.
The published `lorentz` Docker image (see "Docker images") is always built this way.

```bash
# Build Lorentz with the UI embedded
./build.sh "-DUSE_POSTGRESQL=ON -DEMBED_WEBUI=ON"
```

Without `EMBED_WEBUI`, or for local frontend development, run `web/` as its own Next.js server; it
proxies `/api/*` to a real Lorentz instance (`LORENTZ_API_URL`) via `next.config.ts`'s `rewrites()`,
so the browser still only ever talks to one origin. See `web/README.md` for the architecture,
environment variables and a Docker setup.

```bash
cd web && npm install
LORENTZ_API_URL=http://127.0.0.1 npm run dev
```

## Documentation

Lorentz has no documentation of its own yet. The documentation of the upstream project, Pi-hole FTLDNS, can be found [here](https://docs.pi-hole.net/ftldns/). It applies only where this fork did not change the behavior (see the notice at the top).

## Docker images

Both publish `ghcr.io/geovannyavelar/lorentz` (root `Dockerfile`) to GitHub Packages: a from-source,
statically linked build with the PostgreSQL driver enabled and the web UI embedded (see "Web UI"
above) - everything in one binary, on one port, nothing else to run.

- **`.github/workflows/docker-unstable.yml`** - every push to `main` rebuilds and republishes
  `ghcr.io/geovannyavelar/lorentz:unstable`, replacing whatever the tag last pointed to (the image
  it replaces is deleted from the registry right after).
- **`.github/workflows/release.yml`** - pushing a version tag (`vX.Y.Z`) publishes
  `ghcr.io/geovannyavelar/lorentz:X.Y.Z`, `:X.Y` and `:latest`, then creates the matching GitHub
  Release with auto-generated notes.

```bash
docker run --network host ghcr.io/geovannyavelar/lorentz            # latest release
docker run --network host ghcr.io/geovannyavelar/lorentz:unstable   # latest main
# UI at http://<host>/admin/

# Cut a release:
git tag v1.0.0 && git push origin v1.0.0
```

`web/Dockerfile` (the UI as its own standalone Next.js server, for a Lorentz with a non-default
`webserver.paths.webhome`) is not built by CI - build it manually if needed, see `web/README.md`.

Both are also buildable locally with a plain `docker build`; see the `Dockerfile`s themselves and
`web/README.md` for the details.

## Installation

Lorentz (`lorentz`) is not installed by Pi-hole. Build it from source with `./build.sh`.

### IMPORTANT

>Lorentz will *disable* any existing installations of `dnsmasq`. This is because Lorentz *is* `dnsmasq` + its own code, so both cannot run simultaneously.
