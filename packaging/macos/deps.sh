#!/bin/sh
# Dependências nativas do clivcut para macOS, compiladas para UMA arquitetura
# (x86_64 | arm64) em /opt/deps/<arch>:
#
#   codecs estáticos (x264, dav1d, opus, lame, libvpx) dentro do FFmpeg
#   compartilhado, com VideoToolbox; SDL2 compartilhado. As .dylib têm nome
#   @rpath/..., prontas para ir ao Contents/Frameworks do .app.
#
# Roda no build da imagem (packaging/macos/Dockerfile), depois do setup.sh.
set -eu

ARCH=$1
FFMPEG_VERSION=8.0.3       # o mesmo do Flatpak
# Ao mudar uma versão, atualize também o sha256 na chamada de fetch correspondente.
X264_COMMIT=b35605ace3ddf7c1a5d67a2eb553f034aef41d55   # branch stable
DAV1D_VERSION=1.5.1
OPUS_VERSION=1.5.2
LAME_VERSION=3.100
VPX_VERSION=1.15.2
SDL2_VERSION=2.32.10

case $ARCH in
    x86_64) HOST=x86_64-apple-darwin;  FFARCH=x86_64;  VPX_TARGET=x86_64-darwin22-gcc ;;
    arm64)  HOST=aarch64-apple-darwin; FFARCH=aarch64; VPX_TARGET=arm64-darwin22-gcc ;;
    *) echo "arquitetura desconhecida: $ARCH" >&2; exit 1 ;;
esac

P=/opt/deps/$ARCH
SRC=/tmp/deps-$ARCH
JOBS=$(nproc)
T=/opt/darwin/bin/$ARCH-apple-darwin
export CC=$T-clang CXX=$T-clang++ AR=$T-ar RANLIB=$T-ranlib NM=$T-nm STRIP=$T-strip
export PKG_CONFIG_LIBDIR=$P/lib/pkgconfig PKG_CONFIG_PATH=
CMAKE_ARGS="-G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_TOOLCHAIN_FILE=/opt/darwin/$ARCH.cmake
    -DCMAKE_INSTALL_PREFIX=$P -DCMAKE_PREFIX_PATH=$P -DCMAKE_INSTALL_NAME_DIR=@rpath"

mkdir -p "$P" "$SRC"
cd "$SRC"

fetch() { # url pasta sha256 — o tar detecta a compressão pelo conteúdo do arquivo
    mkdir -p "$2"
    curl -fsSL --retry 3 -o "$2.archive" "$1"
    echo "$3  $2.archive" | sha256sum -c -
    tar -xf "$2.archive" -C "$2" --strip-components=1
    rm "$2.archive"
}

echo "==> [$ARCH] x264"
fetch "https://code.videolan.org/videolan/x264/-/archive/$X264_COMMIT/x264-$X264_COMMIT.tar.bz2" x264 \
    6eeb82934e69fd51e043bd8c5b0d152839638d1ce7aa4eea65a3fedcf83ff224
(cd x264 && ./configure --host=$HOST --prefix=$P --enable-static --enable-pic --disable-cli \
    --disable-opencl && make -j"$JOBS" && make install-lib-static)

echo "==> [$ARCH] dav1d"
fetch "https://code.videolan.org/videolan/dav1d/-/archive/$DAV1D_VERSION/dav1d-$DAV1D_VERSION.tar.bz2" dav1d \
    4eddffd108f098e307b93c9da57b6125224dc5877b1b3d157b31be6ae8f1f093
meson setup dav1d/build dav1d --cross-file /opt/darwin/$ARCH.meson --prefix=$P --libdir=lib \
    --buildtype=release --default-library=static -Denable_tools=false -Denable_tests=false
ninja -C dav1d/build install

echo "==> [$ARCH] opus"
fetch "https://downloads.xiph.org/releases/opus/opus-$OPUS_VERSION.tar.gz" opus \
    65c1d2f78b9f2fb20082c38cbe47c951ad5839345876e46941612ee87f9a7ce1
cmake -S opus -B opus/build $CMAKE_ARGS -DBUILD_SHARED_LIBS=OFF -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
    -DOPUS_BUILD_PROGRAMS=OFF -DOPUS_BUILD_TESTING=OFF
cmake --build opus/build --target install

echo "==> [$ARCH] lame"
fetch "https://downloads.sourceforge.net/project/lame/lame/$LAME_VERSION/lame-$LAME_VERSION.tar.gz" lame \
    ddfe36cab873794038ae2c1210557ad34857a4b6bdc515785d1da9e175b1da1e
(cd lame && ./configure --host=$HOST --prefix=$P --disable-shared --enable-static --with-pic \
    --disable-frontend --disable-gtktest && make -j"$JOBS" && make install)

echo "==> [$ARCH] libvpx"
fetch "https://github.com/webmproject/libvpx/archive/refs/tags/v$VPX_VERSION.tar.gz" libvpx \
    26fcd3db88045dee380e581862a6ef106f49b74b6396ee95c2993a260b4636aa
(mkdir -p libvpx/build && cd libvpx/build && LD=$CC ../configure --target=$VPX_TARGET --prefix=$P \
    --enable-pic --enable-vp9-highbitdepth --as=nasm --disable-examples --disable-tools \
    --disable-docs --disable-unit-tests && make -j"$JOBS" && make install)

echo "==> [$ARCH] FFmpeg $FFMPEG_VERSION"
fetch "https://ffmpeg.org/releases/ffmpeg-$FFMPEG_VERSION.tar.xz" ffmpeg \
    6136812ea6d4e68bdba27e33c2a94382711cdf4f8602ffef056ff792bd6f9818
(cd ffmpeg && ./configure --prefix=$P --enable-cross-compile --target-os=darwin --arch=$FFARCH \
    --cc="$CC" --cxx="$CXX" --host-cc=clang --ar="$AR" --ranlib="$RANLIB" --nm="$NM" --strip="$STRIP" \
    --pkg-config=pkg-config --pkg-config-flags=--static \
    --enable-shared --disable-static --install-name-dir=@rpath \
    --enable-gpl --disable-programs --disable-doc --disable-debug --disable-avdevice \
    --enable-videotoolbox --enable-audiotoolbox \
    --disable-avfoundation --disable-coreimage --disable-appkit --disable-securetransport --disable-metal \
    --enable-libx264 --enable-libdav1d --enable-libopus --enable-libmp3lame --enable-libvpx \
    --extra-cflags="-I$P/include" --extra-ldflags="-L$P/lib" \
    && make -j"$JOBS" && make install)

echo "==> [$ARCH] SDL2"
fetch "https://github.com/libsdl-org/SDL/releases/download/release-$SDL2_VERSION/SDL2-$SDL2_VERSION.tar.gz" sdl2 \
    5f5993c530f084535c65a6879e9b26ad441169b3e25d789d83287040a9ca5165
cmake -S sdl2 -B sdl2/build $CMAKE_ARGS -DSDL_SHARED=ON -DSDL_STATIC=OFF -DSDL_TEST=OFF
cmake --build sdl2/build --target install

# Só o que o app usa fica na imagem: headers, .pc e as .dylib.
rm -rf "$SRC" "$P/share" "$P/bin"
echo "==> [$ARCH] ok:"; ls "$P/lib"/*.dylib
