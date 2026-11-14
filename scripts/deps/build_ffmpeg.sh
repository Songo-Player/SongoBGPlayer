#!/usr/bin/env bash
#
# Builds FFmpeg trimmed to audio (see ffmpeg_audio_components.sh) as static,
# PIC libraries with mbedTLS for https://, into build/ffmpeg/<target>.
# Build mbedTLS for the same target first. Run by the scripts/build_*.sh
# scripts.
#
#   scripts/deps/build_ffmpeg.sh <x86_64|arm64|android-arm64|windows-x86_64>
#
# FFmpeg builds inside third_party/ffmpeg and runs "make distclean" first, so
# don't run two of these at once. Toolchains are as for build_mbedtls.sh.

set -e
source "$(dirname "${BASH_SOURCE[0]}")/toolchain.sh"

TARGET="${1:?usage: build_ffmpeg.sh <x86_64|arm64|android-arm64|windows-x86_64>}"
FFMPEG_DIR="$PROJECT_ROOT/third_party/ffmpeg"
INSTALL_DIR="$(deps_dir ffmpeg "$TARGET")"
MBEDTLS_INSTALL_DIR="$(deps_dir mbedtls "$TARGET")"

TARGET_ARGS=()
case "$TARGET" in
    x86_64)
        TARGET_ARGS=(--disable-x86asm)
        ;;
    arm64)
        if [ "$(uname -m)" != "aarch64" ]; then
            CROSS_PREFIX="${CROSS_PREFIX:-aarch64-linux-gnu-}"
            echo "==> Cross compiling with ${CROSS_PREFIX}gcc"
            # The host's pkg-config is fine, PKG_CONFIG_LIBDIR below only
            # shows it our mbedTLS
            TARGET_ARGS=(
                --enable-cross-compile
                --cross-prefix="$CROSS_PREFIX"
                --arch=aarch64
                --target-os=linux
                --pkg-config=pkg-config
            )
        fi
        ;;
    android-arm64)
        find_android_ndk
        TOOLCHAIN="$ANDROID_NDK_ROOT/toolchains/llvm/prebuilt/linux-x86_64"
        export PATH="$TOOLCHAIN/bin:$PATH"
        TARGET_ARGS=(
            --enable-cross-compile
            --target-os=android
            --arch=aarch64
            --cpu=armv8-a
            --sysroot="$TOOLCHAIN/sysroot"
            --cc="$TOOLCHAIN/bin/clang --target=aarch64-linux-android21"
            --cxx="$TOOLCHAIN/bin/clang++ --target=aarch64-linux-android21"
            --ar="$TOOLCHAIN/bin/llvm-ar"
            --ranlib="$TOOLCHAIN/bin/llvm-ranlib"
            --strip="$TOOLCHAIN/bin/llvm-strip"
            --nm="$TOOLCHAIN/bin/llvm-nm"
        )
        ;;
    windows-x86_64)
        CROSS_PREFIX="${CROSS_PREFIX:-x86_64-w64-mingw32-}"
        echo "==> Cross compiling with ${CROSS_PREFIX}gcc"
        # mbedTLS gets its randomness from BCryptGenRandom, which its
        # pkg-config file doesn't list
        TARGET_ARGS=(
            --enable-cross-compile
            --cross-prefix="$CROSS_PREFIX"
            --arch=x86_64
            --target-os=mingw32
            --pkg-config=pkg-config
            --disable-x86asm
            --extra-libs="-lbcrypt -lws2_32"
        )
        ;;
    *)
        echo "!! Unknown target '$TARGET', expected x86_64, arm64, android-arm64 or windows-x86_64" >&2
        exit 1
        ;;
esac

if [ ! -f "$MBEDTLS_INSTALL_DIR/lib/libmbedtls.a" ]; then
    echo "!! mbedTLS for $TARGET not found, run scripts/deps/build_mbedtls.sh $TARGET first" >&2
    exit 1
fi

source "$PROJECT_ROOT/scripts/deps/ffmpeg_audio_components.sh"

# Only let configure see our static mbedTLS, never a system copy
export PKG_CONFIG_LIBDIR="$MBEDTLS_INSTALL_DIR/lib/pkgconfig"

echo "==> FFmpeg $TARGET: $FFMPEG_DIR -> $INSTALL_DIR (mbedTLS from $MBEDTLS_INSTALL_DIR)"
cd "$FFMPEG_DIR"
make distclean || true
rm -rf "$INSTALL_DIR"

./configure "${TARGET_ARGS[@]}" \
    --pkg-config-flags="--static" \
    --prefix="$INSTALL_DIR" \
    --disable-programs \
    --disable-doc \
    --disable-debug \
    --enable-small \
    --disable-shared \
    --enable-static \
    --enable-pic \
    --extra-cflags="-fPIC $FFMPEG_SIZE_CFLAGS" \
    --disable-gpl \
    --disable-nonfree \
    --enable-version3 \
    --disable-avdevice \
    --disable-avfilter \
    --disable-swscale \
    --enable-avcodec \
    --enable-avformat \
    --enable-avutil \
    --enable-swresample \
    --disable-encoders \
    --disable-decoders \
    --enable-decoder="$FFMPEG_AUDIO_DECODERS" \
    --disable-parsers \
    --enable-parser="$FFMPEG_AUDIO_PARSERS" \
    --disable-demuxer="$FFMPEG_IMAGE_DEMUXERS" \
    --disable-demuxer="$FFMPEG_NO_AUDIO_DEMUXERS" \
    --disable-bsfs \
    --enable-bsf="$FFMPEG_AUDIO_BSFS" \
    --disable-muxers \
    --enable-network \
    --enable-mbedtls \
    --disable-zlib \
    --disable-bzlib \
    --disable-lzma \
    --disable-vaapi \
    --disable-vdpau \
    --disable-libdrm \
    --disable-xlib

make -j"$(nproc)"
make install

echo "==> FFmpeg $TARGET done:"
ls -lh "$INSTALL_DIR/lib/"*.a
