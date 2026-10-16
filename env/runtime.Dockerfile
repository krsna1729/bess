# SPDX-License-Identifier: BSD-3-Clause
# The BESS runtime image (M26, D-087, D-135): the release's bess package on the
# distribution it was built for. Built by the release job from the .deb it
# just made:
#
#   docker build -f env/runtime.Dockerfile --build-arg DEB=bess_<v>_amd64.deb -t bess:<v> <dir with the .deb>
#
# Run with the privileges a DPDK dataplane needs (hugepages, devices), e.g.
#   docker run --rm --privileged --network host -v /dev/hugepages:/dev/hugepages bess:<v>
FROM ubuntu:24.04
ARG DEB
COPY ${DEB} /tmp/
# apt resolves the package's Depends (the libraries bessd loads). No Python:
# bessctl is bessd (D-120), and the Python client is gone (D-140).
RUN apt-get update \
 && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends /tmp/${DEB} \
 && rm -rf /var/lib/apt/lists/* /tmp/${DEB}
ENTRYPOINT ["/usr/local/bin/bessd", "-f"]
