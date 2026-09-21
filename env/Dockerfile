# vim: syntax=dockerfile

# Ubuntu 24.04 is the supported container base for the DPDK 25.11 build.
ARG BASE_IMAGE=ubuntu:noble
FROM ${BASE_IMAGE}

RUN echo "APT::Install-Recommends false;" >> /etc/apt/apt.conf.d/00recommends && \
	echo "APT::Install-Suggests false;" >> /etc/apt/apt.conf.d/00recommends && \
	echo "APT::AutoRemove::RecommendsImportant false;" >> /etc/apt/apt.conf.d/00recommends && \
	echo "APT::AutoRemove::SuggestsImportant false;" >> /etc/apt/apt.conf.d/00recommends

COPY build-dep.yml /tmp/
COPY kmod.yml /tmp/
COPY ci.yml /tmp/

# Install dependency packages with Ansible
RUN apt-get -q update && \
	apt-get install -y ansible && \
        ansible-playbook /tmp/ci.yml -i "localhost," -c local && \
	apt-get purge -y ansible && \
	apt-get autoremove -y && \
	rm -rf /var/lib/apt/lists

# The image provides the dependency and runtime environment.  The mounted
# checkout owns DPDK bootstrap and the Meson build.
WORKDIR /build/bess

ENV CCACHE_DIR=/tmp/ccache
ENV CCACHE_COMPRESS=true

