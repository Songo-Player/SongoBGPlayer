#!/usr/bin/env bash
#
# Builds the engine library and daemon for arm64 Linux handhelds:
#   bin/libsongo_engine.linux.arm64.so
#   bin/songo_bgplayer.linux.arm64
#
#   scripts/build_arm64.sh [--rebuild-deps] [scons arguments...]
#
# Builds natively on an aarch64 machine (eg: an arm64 chroot), otherwise cross
# compiles with $CROSS_PREFIX (aarch64-linux-gnu- by default), which needs the
# arm64 ALSA headers in that toolchain's sysroot. The toolchain's glibc sets
# the oldest CFW the binaries run on: SongoDevKit builds these in Ubuntu 20.04
# for glibc 2.29.
#
# mbedTLS and FFmpeg are built first if they aren't yet (--rebuild-deps always
# rebuilds them).

set -e
source "$(dirname "${BASH_SOURCE[0]}")/deps/toolchain.sh"
check_submodules

# Cross compiling from another architecture: the aarch64 GNU toolchain, unless
# CROSS_PREFIX names another (scripts/deps and the SConstruct both read it)
if [ "$(uname -m)" != "aarch64" ]; then
    export CROSS_PREFIX="${CROSS_PREFIX:-aarch64-linux-gnu-}"
    command -v "${CROSS_PREFIX}gcc" > /dev/null \
        || { echo "!! ${CROSS_PREFIX}gcc not found: install an aarch64 cross toolchain, or set CROSS_PREFIX" >&2; exit 1; }
fi

REBUILD_DEPS=0
if [ "${1:-}" = "--rebuild-deps" ]; then
    REBUILD_DEPS=1
    shift
fi

ensure_deps "$REBUILD_DEPS" arm64

echo "==> Building the engine and daemon..."
cd "$PROJECT_ROOT"
scons platform=linux arch=arm64 -j"$(nproc)" "$@"

echo
echo "==> Done:"
ls -lh bin/*linux.arm64*
