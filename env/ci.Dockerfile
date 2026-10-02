# The gating CI toolchain (Ubuntu 24.04, gcc-14, clang-19), for running
# tools/ci_profile.py on a machine that does not have those versions.
# Use tools/ci_container.sh; do not build this by hand.
#
# It installs the same packages as the CI jobs, with the same script
# (env/install-deps.sh), so the two cannot differ in which libraries or
# compilers they see.
FROM ubuntu:24.04

COPY env/install-deps.sh /opt/bess/env/install-deps.sh
COPY requirements.txt /opt/bess/requirements.txt
RUN bash /opt/bess/env/install-deps.sh runtime /opt/bess/requirements.txt \
 && bash /opt/bess/env/install-deps.sh build \
 && apt-get install -y --no-install-recommends git ca-certificates \
 && rm -rf /var/lib/apt/lists/*
