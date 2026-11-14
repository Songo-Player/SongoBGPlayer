FFmpeg version: n8.1.2
mbedTLS version: v3.6.7 (built first with scripts/deps/build_mbedtls.sh)

Configure (x86_64; see scripts/deps/build_ffmpeg.sh for the per-platform variants):

PKG_CONFIG_LIBDIR="$MBEDTLS_INSTALL_DIR/lib/pkgconfig" \
./configure \
    --pkg-config-flags="--static" \
    --prefix="$INSTALL_DIR" \
    --disable-programs \
    --disable-doc \
    --disable-debug \
    --disable-shared \
    --enable-static \
    --enable-pic \
    --extra-cflags="-fPIC" \
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
    --disable-muxers \
    --enable-network \
    --enable-mbedtls \
    --disable-x86asm \
    --disable-zlib \
    --disable-bzlib \
    --disable-lzma \
    --disable-vaapi \
    --disable-vdpau \
    --disable-libdrm \
    --disable-xlib
