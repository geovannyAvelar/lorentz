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
RUN apk add --no-cache libpq-dev openssl-libs-static
RUN rm -rf cmake && bash build.sh "-DUSE_POSTGRESQL=ON"

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
# /api/info/version reads this for the update-check fields Pi-hole's own
# installer normally maintains; nothing populates it in a plain container,
# but an empty file at least reports empty fields instead of failing outright.
RUN touch /etc/lorentz/versions

COPY --from=build /app/lorentz /usr/bin/lorentz

# A default configuration, so a first start does not log a missing file.
WORKDIR /etc/lorentz
RUN lorentz create-default-config lorentz.toml

# 53: DNS. 80/443: the web API and, once configured, the web UI's own
# reverse proxy in front of it (see web/README.md). Lorentz replaces
# dnsmasq, so DNS needs the host's network (--network host), or the
# container's own network namespace with these ports published.
EXPOSE 53/udp 53/tcp 80 443
CMD ["lorentz", "no-daemon"]
