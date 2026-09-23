# Runtime image for the Lorentz daemon, built and published to GitHub
# Packages (ghcr.io) by .github/workflows/docker.yml.
#
#   docker run --network host ghcr.io/<owner>/lorentz
#
# Build stage: the same toolchain image the test suite and the old CI
# pipeline (build.sh via .github/Dockerfile) used, so the compiler and its
# flags match what this codebase is actually developed and tested against.
FROM ghcr.io/pi-hole/ftl-build:v2.19 AS build
WORKDIR /app
COPY . .

# The PostgreSQL driver is optional (see README.md, "Database drivers") and
# off by default; build it in so the published image can point
# files.database at either a SQLite file or a PostgreSQL server without a
# rebuild. This toolchain image's default build is fully static
# (STATIC=true in its own environment already) - openssl-libs-static is
# needed alongside libpq-dev only because of that: libpq's own static
# helper libraries need OpenSSL's for password hashing and its own TLS
# support, entirely unrelated to the mbedTLS this project's webserver uses
# (see the comments around USE_POSTGRESQL in src/CMakeLists.txt for the
# exact libraries and why the ordinary, non-"_shlib" ones are not enough).
#
# EMBED_WEBUI (see src/api/webui/CMakeLists.txt) folds web/'s static export
# into the binary, so it needs Node.js/npm here at build time only - nothing
# of it remains in the runtime stage or the final binary's dependencies.
RUN apk add --no-cache libpq-dev openssl-libs-static nodejs npm
RUN rm -rf cmake && bash build.sh "-DUSE_POSTGRESQL=ON -DEMBED_WEBUI=ON"

# Runtime stage. The binary above is fully static (musl, no dynamic
# libraries at all - `file` on it says "static-pie linked"), so this stage
# needs no matching shared libraries, just a directory layout and
# ca-certificates for the gravity lists and DoH/DoT upstreams that use
# https:// - no compiler, no sources, no package manager left behind
# either.
FROM ubuntu:24.04 AS run
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends ca-certificates \
    && rm -rf /var/lib/apt/lists/*

RUN mkdir -p /etc/lorentz /var/log/lorentz /run/lorentz /etc/dnsmasq.d /var/www/html /home/lorentz

# Lorentz still runs as root (needed to bind DNS port 53 and the raw sockets
# dnsmasq uses), but chown_lorentz() (src/files.c) normalizes the ownership
# of its config, logs and database to a "lorentz" account regardless -
# without one, that lookup fails and every such chown warns instead. Matches
# the account test/run.sh creates for the same reason.
RUN useradd --system --no-create-home --home-dir /home/lorentz --shell /usr/sbin/nologin lorentz \
    && chown -R lorentz:lorentz /etc/lorentz /var/log/lorentz /run/lorentz /var/www/html /home/lorentz

# /api/info/version reads this for the update-check fields Pi-hole's own
# installer normally maintains; nothing populates it in a plain container,
# but an empty file at least reports empty fields instead of failing outright.
RUN touch /etc/lorentz/versions

# readPID() (src/procps.c) opens LORENTZ_PID_FILE to check for an already
# running instance and warns if that fails for any reason, ENOENT (a fresh
# container) included - pre-create it empty so a first start doesn't warn.
RUN touch /run/lorentz.pid && chown lorentz:lorentz /run/lorentz.pid

COPY --from=build /app/lorentz /usr/bin/lorentz

# A default configuration, so a first start does not log a missing file.
WORKDIR /etc/lorentz
RUN lorentz create-default-config lorentz.toml && chown lorentz:lorentz lorentz.toml

# 53: DNS. 80/443: the API and the web UI, both served by Lorentz itself at
# its default admin path (/admin/, see README.md, "Web UI"). Lorentz replaces
# dnsmasq, so DNS needs the host's network (--network host), or the
# container's own network namespace with these ports published.
EXPOSE 53/udp 53/tcp 80 443
CMD ["lorentz", "no-daemon"]
