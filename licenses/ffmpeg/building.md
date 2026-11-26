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
    --disable-x86asm \
    --disable-zlib \
    --disable-bzlib \
    --disable-lzma \
    --disable-vaapi \
    --disable-vdpau \
    --disable-libdrm \
    --disable-xlib

The FFMPEG_* variables are set by scripts/deps/ffmpeg_audio_components.sh.
