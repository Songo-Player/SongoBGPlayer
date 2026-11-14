#!/usr/bin/env bash
#
# Builds mbedTLS (https:// for FFmpeg) as static, PIC libraries, into
# build/mbedtls/<target>. They get linked into FFmpeg's static libraries and
# from there into libsongo_engine. Run by the scripts/build_*.sh scripts.
#
#   scripts/deps/build_mbedtls.sh <x86_64|arm64|android-arm64|windows-x86_64>
#
# arm64 builds natively on an aarch64 machine (eg: in a chroot), otherwise
# cross compiles with $CROSS_PREFIX (aarch64-linux-gnu- by default).
# android-arm64 needs the NDK, see find_android_ndk in toolchain.sh.
# windows-x86_64 cross compiles with MinGW-w64 ($CROSS_PREFIX,
# x86_64-w64-mingw32- by default); scripts/build_windows.sh has it in Docker.

set -e
source "$(dirname "${BASH_SOURCE[0]}")/toolchain.sh"

TARGET="${1:?usage: build_mbedtls.sh <x86_64|arm64|android-arm64|windows-x86_64>}"
MBEDTLS_DIR="$PROJECT_ROOT/third_party/mbedtls"
INSTALL_DIR="$(deps_dir mbedtls "$TARGET")"
BUILD_DIR="$INSTALL_DIR-cmake"

TARGET_ARGS=()
case "$TARGET" in
    x86_64) ;;
    arm64)
        if [ "$(uname -m)" != "aarch64" ]; then
            CROSS_PREFIX="${CROSS_PREFIX:-aarch64-linux-gnu-}"
            echo "==> Cross compiling with ${CROSS_PREFIX}gcc"
            TARGET_ARGS=(
                -DCMAKE_SYSTEM_NAME=Linux
                -DCMAKE_SYSTEM_PROCESSOR=aarch64
                -DCMAKE_C_COMPILER="${CROSS_PREFIX}gcc"
            )
        fi
        ;;
    android-arm64)
        find_android_ndk
        TARGET_ARGS=(
            -DCMAKE_TOOLCHAIN_FILE="$ANDROID_NDK_ROOT/build/cmake/android.toolchain.cmake"
            -DANDROID_ABI=arm64-v8a
            -DANDROID_PLATFORM=android-21
        )
        ;;
    windows-x86_64)
        CROSS_PREFIX="${CROSS_PREFIX:-x86_64-w64-mingw32-}"
        echo "==> Cross compiling with ${CROSS_PREFIX}gcc"
        TARGET_ARGS=(
            -DCMAKE_SYSTEM_NAME=Windows
            -DCMAKE_SYSTEM_PROCESSOR=x86_64
            -DCMAKE_C_COMPILER="${CROSS_PREFIX}gcc"
            -DCMAKE_RC_COMPILER="${CROSS_PREFIX}windres"
        )
        ;;
    *)
        echo "!! Unknown target '$TARGET', expected x86_64, arm64, android-arm64 or windows-x86_64" >&2
        exit 1
        ;;
esac

echo "==> mbedTLS $TARGET: $MBEDTLS_DIR -> $INSTALL_DIR"
rm -rf "$BUILD_DIR" "$INSTALL_DIR"

cmake -S "$MBEDTLS_DIR" -B "$BUILD_DIR" "${TARGET_ARGS[@]}" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="$INSTALL_DIR" \
    -DCMAKE_INSTALL_LIBDIR=lib \
    -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
    -DUSE_STATIC_MBEDTLS_LIBRARY=ON \
    -DUSE_SHARED_MBEDTLS_LIBRARY=OFF \
    -DENABLE_PROGRAMS=OFF \
    -DENABLE_TESTING=OFF \
    -DGEN_FILES=OFF \
    -DMBEDTLS_FATAL_WARNINGS=OFF

cmake --build "$BUILD_DIR" -j"$(nproc)"
cmake --install "$BUILD_DIR"

echo "==> mbedTLS $TARGET done:"
ls -lh "$INSTALL_DIR/lib/"*.a
