# Runtime image for the integration tests. The lorentz binary under test is
# copied in from the build context, so this image only provides the shared
# libraries it links against and a few tools the tests use (dig, curl)
FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
        libhogweed6t64 libgmp10 libnettle8t64 libidn2-0 libunistring5 \
        libdbus-1-3 libreadline8t64 libcap2 libtinfo6 \
        dnsutils curl ca-certificates \
    && rm -rf /var/lib/apt/lists/*

RUN mkdir -p /etc/lorentz /var/log/lorentz /run/lorentz /etc/dnsmasq.d /var/www/html /home/lorentz

COPY lorentz /usr/bin/lorentz
COPY versions /etc/lorentz/versions
COPY gravity.db.sql /tmp/gravity.db.sql

# Gravity database from the same schema and sample data the bats suite uses,
# created with the SQLite shell of the binary under test
RUN lorentz sqlite3 /etc/lorentz/gravity.db < /tmp/gravity.db.sql

# Default configuration, so that a normal start does not log a missing file
WORKDIR /etc/lorentz
RUN lorentz create-default-config lorentz.toml

EXPOSE 80
CMD ["lorentz", "no-daemon"]
