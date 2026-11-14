#!/usr/bin/env bash
# Sourced by the scripts in scripts/. Sets PROJECT_ROOT and helpers shared by
# the dependency and engine builds.

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"

# The NDK for Android builds: $ANDROID_NDK_ROOT, else r23c (the NDK Godot 4.3
# builds with) under $ANDROID_HOME. Exports ANDROID_NDK_ROOT.
find_android_ndk() {
    if [ -z "${ANDROID_NDK_ROOT:-}" ] && [ -n "${ANDROID_HOME:-}" ]; then
        ANDROID_NDK_ROOT="$ANDROID_HOME/ndk/23.2.8568313"
    fi
    if [ -z "${ANDROID_NDK_ROOT:-}" ] || [ ! -d "$ANDROID_NDK_ROOT/toolchains/llvm" ]; then
        echo "!! Android NDK r23c not found: set ANDROID_NDK_ROOT, or ANDROID_HOME with ndk/23.2.8568313 under it" >&2
        exit 1
    fi
    export ANDROID_NDK_ROOT
}

# Where build_mbedtls.sh / build_ffmpeg.sh install for a target
deps_dir() { # <mbedtls|ffmpeg> <x86_64|arm64|android-arm64|windows-x86_64>
    case "$2" in
        android-arm64) echo "$PROJECT_ROOT/build/$1/android/arm64" ;;
        windows-x86_64) echo "$PROJECT_ROOT/build/$1/windows/x86_64" ;;
        *) echo "$PROJECT_ROOT/build/$1/$2" ;;
    esac
}

# Builds mbedTLS then FFmpeg for a target if they aren't built yet, or always
# with $1 = 1
ensure_deps() { # <rebuild 0|1> <target>
    if [ "$1" = 1 ] || [ ! -f "$(deps_dir mbedtls "$2")/lib/libmbedtls.a" ]; then
        "$PROJECT_ROOT/scripts/deps/build_mbedtls.sh" "$2"
    fi
    if [ "$1" = 1 ] || [ ! -f "$(deps_dir ffmpeg "$2")/lib/libavformat.a" ]; then
        "$PROJECT_ROOT/scripts/deps/build_ffmpeg.sh" "$2"
    fi
}

check_submodules() {
    if [ ! -f "$PROJECT_ROOT/third_party/ffmpeg/configure" ] || [ ! -f "$PROJECT_ROOT/third_party/mbedtls/CMakeLists.txt" ]; then
        echo "!! Submodules missing, run: git submodule update --init --recursive" >&2
        exit 1
    fi
}
