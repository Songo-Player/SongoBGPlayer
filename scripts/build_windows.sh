#!/usr/bin/env bash
#
# Builds the engine library for 64-bit Windows, which Songodot's Windows
# builds load for in-process playback (there's no daemon on Windows):
#   bin/libsongo_engine.windows.x86_64.dll
#
#   scripts/build_windows.sh [--rebuild-deps] [scons arguments...]
#
# Cross compiles with MinGW-w64 ($CROSS_PREFIX, x86_64-w64-mingw32- by
# default), which needs the posix thread model since the engine uses
# std::thread. Plays through WASAPI. The DLL links its C/C++ runtimes
# statically, so it ships on its own next to Songo's .exe.
#
# mbedTLS and FFmpeg are built first if they aren't yet (--rebuild-deps always
# rebuilds them).

set -e
source "$(dirname "${BASH_SOURCE[0]}")/deps/toolchain.sh"
check_submodules

export CROSS_PREFIX="${CROSS_PREFIX:-x86_64-w64-mingw32-}"
command -v "${CROSS_PREFIX}g++" > /dev/null \
    || { echo "!! ${CROSS_PREFIX}g++ not found: install MinGW-w64 (posix thread model), or set CROSS_PREFIX" >&2; exit 1; }

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
