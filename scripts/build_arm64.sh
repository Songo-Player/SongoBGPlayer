#!/usr/bin/env bash
#
# Builds the engine library and daemon for arm64 Linux handhelds:
#   bin/libsongo_engine.linux.arm64.so
#   bin/songo_bgplayer.linux.arm64
#
#   scripts/build_arm64.sh [--rebuild-deps] [scons arguments...]
#
# On an x86_64 machine this cross compiles in Docker (scripts/docker/), whose
# Ubuntu 20.04 base keeps the binaries running on old CFWs (glibc 2.29 or
# newer). On an aarch64 machine, eg: an arm64 chroot, it builds natively;
# mind its glibc, which sets the oldest CFW the binaries run on.
#
# mbedTLS and FFmpeg are built first if they aren't yet (--rebuild-deps always
# rebuilds them). Set SONGO_ARM64_IMAGE to use another Docker image name.

set -e
source "$(dirname "${BASH_SOURCE[0]}")/deps/toolchain.sh"
check_submodules

if [ "$(uname -m)" != "aarch64" ] && [ -z "${SONGO_IN_DOCKER:-}" ]; then
    IMAGE="${SONGO_ARM64_IMAGE:-songo-bgplayer-arm64}"
    echo "==> Building image $IMAGE..."
    docker build -t "$IMAGE" "$PROJECT_ROOT/scripts/docker"
    # As the calling user, so what lands in build/ and bin/ isn't owned by root
    TTY_ARGS=()
    if [ -t 1 ]; then
        TTY_ARGS=(-t)
    fi
    exec docker run --rm "${TTY_ARGS[@]}" \
        --user "$(id -u):$(id -g)" \
        -e HOME=/tmp \
        -v "$PROJECT_ROOT:/src" \
        "$IMAGE" scripts/build_arm64.sh "$@"
fi

REBUILD_DEPS=0
if [ "${1:-}" = "--rebuild-deps" ]; then
    REBUILD_DEPS=1
    shift
fi

ensure_deps "$REBUILD_DEPS" arm64

echo "==> Building the engine and daemon..."
cd "$PROJECT_ROOT"
# In Docker, CROSS_PREFIX (set in the Dockerfile) picks the aarch64 toolchain
scons platform=linux arch=arm64 -j"$(nproc)" "$@"

echo
echo "==> Done:"
ls -lh bin/*linux.arm64*
