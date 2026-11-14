#!/usr/bin/env bash
#
# Builds the engine library for 64-bit Windows, which Songodot's Windows
# builds load for in-process playback (there's no daemon on Windows):
#   bin/libsongo_engine.windows.x86_64.dll
#
#   scripts/build_windows.sh [--rebuild-deps] [scons arguments...]
#
# Cross compiles with MinGW-w64 in Docker (scripts/docker/Dockerfile.windows).
# Plays through WASAPI. The DLL links its C/C++ runtimes statically, so it
# ships on its own next to Songo's .exe.
#
# mbedTLS and FFmpeg are built first if they aren't yet (--rebuild-deps always
# rebuilds them). Set SONGO_WINDOWS_IMAGE to use another Docker image name.

set -e
source "$(dirname "${BASH_SOURCE[0]}")/deps/toolchain.sh"
check_submodules

if [ -z "${SONGO_IN_WINDOWS_DOCKER:-}" ]; then
    IMAGE="${SONGO_WINDOWS_IMAGE:-songo-bgplayer-windows}"
    echo "==> Building image $IMAGE..."
    docker build -t "$IMAGE" -f "$PROJECT_ROOT/scripts/docker/Dockerfile.windows" "$PROJECT_ROOT/scripts/docker"
    TTY_ARGS=()
    if [ -t 1 ]; then
        TTY_ARGS=(-t)
    fi
    exec docker run --rm "${TTY_ARGS[@]}" \
        --user "$(id -u):$(id -g)" \
        -e HOME=/tmp \
        -v "$PROJECT_ROOT:/src" \
        "$IMAGE" scripts/build_windows.sh "$@"
fi

REBUILD_DEPS=0
if [ "${1:-}" = "--rebuild-deps" ]; then
    REBUILD_DEPS=1
    shift
fi

ensure_deps "$REBUILD_DEPS" windows-x86_64

echo "==> Building the engine..."
cd "$PROJECT_ROOT"
scons platform=windows arch=x86_64 -j"$(nproc)" "$@"

echo
echo "==> Done:"
ls -lh bin/*windows*
