# vim: syntax=dockerfile

# Ubuntu 24.04 is the supported container base for the DPDK 25.11 build.
ARG BASE_IMAGE=ubuntu:noble
FROM ${BASE_IMAGE} AS runtime

COPY env/install-deps.sh /usr/local/bin/install-bess-deps
COPY requirements.txt /tmp/requirements.txt

RUN bash /usr/local/bin/install-bess-deps runtime /tmp/requirements.txt && \
    rm -rf /var/lib/apt/lists/*

FROM runtime AS build

RUN bash /usr/local/bin/install-bess-deps build && \
    rm -rf /var/lib/apt/lists/*

# The mounted checkout owns DPDK bootstrap and the Meson build.
WORKDIR /build/bess

ENV CCACHE_DIR=/tmp/ccache
ENV CCACHE_COMPRESS=true
