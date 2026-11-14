# Cross compiles the engine and daemon for arm64 Linux handhelds on an x86_64
# host. scripts/build_arm64.sh builds and runs this, running itself inside.
#
# Ubuntu 20.04 on purpose: its glibc (2.31) and GCC 9.4 are what the arm64
# chroot builds used, so the binaries run on the same CFWs. Moving to a newer
# base raises the glibc the binaries need.

FROM ubuntu:20.04

ENV DEBIAN_FRONTEND=noninteractive

# arm64 packages (ALSA) come from ports.ubuntu.com, so the main archive is
# limited to amd64. The native gcc is for the helper tools FFmpeg's build
# runs on the host.
RUN dpkg --add-architecture arm64 \
    && sed -i 's/^deb /deb [arch=amd64] /' /etc/apt/sources.list \
    && printf '%s\n' \
        "deb [arch=arm64] http://ports.ubuntu.com/ubuntu-ports focal main universe" \
        "deb [arch=arm64] http://ports.ubuntu.com/ubuntu-ports focal-updates main universe" \
        "deb [arch=arm64] http://ports.ubuntu.com/ubuntu-ports focal-security main universe" \
        > /etc/apt/sources.list.d/arm64-ports.list \
    && apt-get update \
    && apt-get install -y --no-install-recommends \
        gcc-aarch64-linux-gnu \
        g++-aarch64-linux-gnu \
        libasound2-dev:arm64 \
        gcc \
        libc6-dev \
        cmake \
        make \
        pkg-config \
        python3 \
        python3-pip \
        ca-certificates \
    && rm -rf /var/lib/apt/lists/*

# SCons 4, newer than focal's
RUN python3 -m pip install --no-cache-dir scons==4.7.0

# Read by scripts/deps/ and the SConstruct to pick the aarch64 tools, and by
# scripts/build_arm64.sh to build here instead of starting Docker again
ENV CROSS_PREFIX=aarch64-linux-gnu-
ENV SONGO_IN_DOCKER=1
WORKDIR /src
