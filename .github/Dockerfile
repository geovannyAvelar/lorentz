FROM ghcr.io/pi-hole/ftl-build:v2.19

WORKDIR /app

COPY . /app

ARG CI_ARCH="linux/amd64"
ENV CI_ARCH=${CI_ARCH}
ARG GIT_BRANCH="test"
ENV GIT_BRANCH=${GIT_BRANCH}
ARG GIT_TAG="test"
ENV GIT_TAG=${GIT_TAG}
ARG BUILD_OPTS=""
ENV BUILD_OPTS=${BUILD_OPTS}

# Setting TERM is needed for pretty output in BATS tests
ENV TERM=xterm

# Monkeypatch BATS to remove duplicate output of starting and finished test
# BATS uses ANSI escape codes to overwrite the line after the test has finished
# This is not supported by Github Actions as it does not provide a TTY to the docker build container
RUN sed -i '/buffer_with_truncation /d' /bats/bats-core/libexec/bats-core/bats-format-pretty

# Build Lorentz
# Remove possible old build files
RUN rm -rf cmake && \
# Build and test Lorentz
    bash build.sh "-DSTATIC=${STATIC} -DBUILD_TAR_REGRESSION=ON -DBUILD_GZIP_REGRESSION=ON -DBUILD_PTR_RESPONSE_REGRESSION=ON" ${BUILD_OPTS} && \
# Copy Lorentz binary to root directory
    cd / &&\
    cp /app/lorentz . && \
# Create tarball of API docs
    tar -C /app/src/api/docs/content/ -czvf /api-docs.tar.gz . && \
# Generate default config file
    ./lorentz create-default-config lorentz.toml
