#!/usr/bin/env bash
#
# Builds the engine library and daemon for x86_64 Linux (desktops, and running
# Songo from the editor):
#   bin/libsongo_engine.linux.x86_64.so
#   bin/songo_bgplayer.linux.x86_64
#
#   scripts/build_x86_64.sh [--rebuild-deps] [scons arguments...]
#
# mbedTLS and FFmpeg are built first if they aren't yet (--rebuild-deps always
# rebuilds them, eg: after updating a submodule). Needs the ALSA headers
# (libasound2-dev), cmake and a C/C++ toolchain. Pass target=debug for an
# unoptimized build with symbols, use_llvm=yes for clang.

set -e
source "$(dirname "${BASH_SOURCE[0]}")/deps/toolchain.sh"
check_submodules

REBUILD_DEPS=0
if [ "${1:-}" = "--rebuild-deps" ]; then
    REBUILD_DEPS=1
    shift
fi

ensure_deps "$REBUILD_DEPS" x86_64

echo "==> Building the engine and daemon..."
cd "$PROJECT_ROOT"
scons platform=linux arch=x86_64 -j"$(nproc)" "$@"

echo
echo "==> Done:"
ls -lh bin/*linux.x86_64*
