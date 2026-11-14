#!/usr/bin/env bash
#
# Builds the Android (arm64) engine library and the plugin AAR that carries
# it, with the playback service and media session:
#   bin/libsongo_engine.android.arm64.so
#   android/plugin/build/outputs/aar/songo_harness-release.aar
#
#   scripts/build_android.sh [--rebuild-deps] [--no-aar] [scons arguments...]
#
# Needs the NDK r23c (ANDROID_NDK_ROOT, or ANDROID_HOME with
# ndk/23.2.8568313), cmake, and for the AAR the Android SDK (ANDROID_HOME) and
# a JDK 17 (JAVA_HOME, or ~/Android/jdk17 if that exists). mbedTLS and FFmpeg
# are built first if they aren't yet (--rebuild-deps always rebuilds them).
# --no-aar stops after the engine library.

set -e
source "$(dirname "${BASH_SOURCE[0]}")/deps/toolchain.sh"
check_submodules

REBUILD_DEPS=0
BUILD_AAR=1
while [ $# -gt 0 ]; do
    case "$1" in
        --rebuild-deps) REBUILD_DEPS=1 ;;
        --no-aar) BUILD_AAR=0 ;;
        *) break ;;
    esac
    shift
done

find_android_ndk
ensure_deps "$REBUILD_DEPS" android-arm64

echo "==> Building the engine..."
cd "$PROJECT_ROOT"
scons platform=android arch=arm64 -j"$(nproc)" "$@"

if [ "$BUILD_AAR" = 1 ]; then
    if [ -z "${ANDROID_HOME:-}" ]; then
        echo "!! ANDROID_HOME isn't set, it's needed for the AAR (or pass --no-aar)" >&2
        exit 1
    fi
    if [ -z "${JAVA_HOME:-}" ] && [ -d "$HOME/Android/jdk17" ]; then
        export JAVA_HOME="$HOME/Android/jdk17"
    fi
    echo "==> Building the plugin AAR..."
    (cd android/plugin && ./gradlew assembleRelease)
fi

echo
echo "==> Done:"
ls -lh bin/libsongo_engine.android.arm64.so
if [ "$BUILD_AAR" = 1 ]; then
    ls -lh android/plugin/build/outputs/aar/songo_harness-release.aar
fi
