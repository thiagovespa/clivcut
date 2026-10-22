#!/usr/bin/env bash
# Gera, a partir do AppDir do AppImage (rode packaging/appimage/build-appimage.sh antes):
#   dist/clivcut-<versão>-linux-x86_64.tar.gz   pasta autocontida, roda de qualquer lugar
#   dist/clivcut_<versão>_amd64.deb              Debian/Ubuntu, em /opt/clivcut
#   dist/clivcut-<versão>-1.x86_64.rpm           Fedora/openSUSE, em /opt/clivcut
#
# O Qt e as demais bibliotecas vão embutidos (as distros estáveis têm Qt antigo demais);
# do sistema ficam só glibc, OpenGL/EGL, X11/xcb, fontconfig e afins.
#
# Variáveis: BUILD_DIR (o mesmo do AppImage), OUT_DIR, TOOLS_DIR.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD="${BUILD_DIR:-$ROOT/build-appimage}"
OUT="${OUT_DIR:-$ROOT/dist}"
TOOLS="${TOOLS_DIR:-$BUILD/tools}"
APPDIR="$BUILD/AppDir"
ARCH="$(uname -m)"

VERSION="$(sed -nE 's/^ *VERSION ([0-9.]+)$/\1/p' "$ROOT/CMakeLists.txt" | head -1)"
VERSION="${VERSION:-0.0.0}"

NFPM_VERSION=2.47.0
NFPM_SHA256=0660ca602b2d2d2ae4781a06c692b3eeb9d437ffea05b831d76e41f4a3188783

if [ ! -x "$APPDIR/usr/bin/clivcut" ]; then
    echo "AppDir não encontrado em $APPDIR: rode packaging/appimage/build-appimage.sh antes" >&2
    exit 1
fi
[ "$ARCH" = x86_64 ] || { echo "só x86_64 por enquanto" >&2; exit 1; }

STAGE="$BUILD/linux-packages"
rm -rf "$STAGE"
mkdir -p "$STAGE" "$OUT"

# --- árvore do programa: o usr/ do AppDir, sem o que é da integração do AppImage ---
TREE="$STAGE/clivcut"
mkdir -p "$TREE"
cp -a "$APPDIR/usr/." "$TREE/"
# O install do CMake também põe o ONNX Runtime no LIBDIR da distro (lib64, lib/x86_64-linux-gnu);
# o executável só usa a cópia de lib/, deixada pelo linuxdeploy.
if [ -e "$TREE/lib/libonnxruntime.so.1" ]; then
    find "$TREE" -name 'libonnxruntime.so*' ! -path "$TREE/lib/libonnxruntime*" -delete
fi
find "$TREE" -type d -empty -delete

# Dependências do sistema: as bibliotecas que os binários e plugins pedem e o pacote não traz.
# Calculadas a cada build, para nunca faltar uma (instalações mínimas não têm OpenGL, HarfBuzz...).
EXTERNAL="$({ find "$TREE" -type f \( -name '*.so*' -o -path "$TREE/bin/*" \) -exec readelf -d {} + 2>/dev/null || true; } \
    | sed -n 's/.*(NEEDED).*\[\(.*\)\]/\1/p' | sort -u \
    | comm -23 - <(find "$TREE" -name '*.so*' -printf '%f\n' | sort -u))"

# .deb: o pacote de cada biblioteca (nomes do Ubuntu 24.04 / Debian 13).
deb_package() {
    case "$1" in
        ld-linux-x86-64.so.2 | libc.so.6 | libdl.so.2 | libm.so.6 | libpthread.so.0 | libresolv.so.2 | librt.so.1)
            echo "libc6 (>= 2.39)" ;;
        libstdc++.so.6) echo libstdc++6 ;;
        libgcc_s.so.1) echo libgcc-s1 ;;
        libasound.so.2) echo "libasound2t64 | libasound2" ;;
        libcom_err.so.2) echo libcom-err2 ;;
        libdrm.so.2) echo libdrm2 ;;
        libEGL.so.1) echo libegl1 ;;
        libGL.so.1) echo libgl1 ;;
        libGLX.so.0) echo libglx0 ;;
        libOpenGL.so.0) echo libopengl0 ;;
        libgbm.so.1) echo libgbm1 ;;
        libfontconfig.so.1) echo libfontconfig1 ;;
        libfreetype.so.6) echo libfreetype6 ;;
        libfribidi.so.0) echo libfribidi0 ;;
        libharfbuzz.so.0) echo libharfbuzz0b ;;
        libgmp.so.10) echo libgmp10 ;;
        libgpg-error.so.0) echo libgpg-error0 ;;
        libwayland-client.so.0) echo libwayland-client0 ;;
        libwayland-cursor.so.0) echo libwayland-cursor0 ;;
        libwayland-egl.so.1) echo libwayland-egl1 ;;
        libX11.so.6) echo libx11-6 ;;
        libX11-xcb.so.1) echo libx11-xcb1 ;;
        libxcb.so.1) echo libxcb1 ;;
        libxcb-dri3.so.0) echo libxcb-dri3-0 ;;
        libz.so.1) echo zlib1g ;;
        libdbus-1.so.3) echo libdbus-1-3 ;;
        libxkbcommon.so.0) echo libxkbcommon0 ;;
        libxkbcommon-x11.so.0) echo libxkbcommon-x11-0 ;;
        libglib-2.0.so.0 | libgobject-2.0.so.0 | libgthread-2.0.so.0) echo "libglib2.0-0t64 | libglib2.0-0" ;;
        *) return 1 ;;
    esac
}

DEB_DEPENDS="" RPM_DEPENDS="      - libc.so.6(GLIBC_2.39)(64bit)"
for lib in $EXTERNAL; do
    pkg="$(deb_package "$lib")" || { echo "build-packages: sem pacote .deb para $lib (acrescente em deb_package)" >&2; exit 1; }
    DEB_DEPENDS+="      - $pkg"$'\n'
    # .rpm: pela própria biblioteca, o que vale para Fedora e openSUSE (nomes de pacote diferentes).
    RPM_DEPENDS+=$'\n'"      - $lib()(64bit)"
done
DEB_DEPENDS="$(printf '%s' "$DEB_DEPENDS" | sort -u)"
echo "==> Dependências do sistema:" $EXTERNAL

# --- .tar.gz ---
NAME="clivcut-$VERSION-linux-x86_64"
cp -a "$TREE" "$STAGE/$NAME"
cat > "$STAGE/$NAME/README.txt" <<EOF
clivcut $VERSION for Linux (x86_64)

Run it from this folder:
    ./bin/clivcut
    ./bin/clivcut-render --help

Qt, FFmpeg and the other libraries are inside this folder. The system must
provide glibc 2.39 or newer and the libraries listed in DEPENDENCIES.txt
(OpenGL/EGL, X11/Wayland, fonts, ALSA), present on any desktop install.
EOF
printf '%s\n' $EXTERNAL > "$STAGE/$NAME/DEPENDENCIES.txt"
tar -C "$STAGE" -czf "$OUT/$NAME.tar.gz" "$NAME"
echo "==> $OUT/$NAME.tar.gz"

# --- .deb e .rpm com o nfpm ---
mkdir -p "$TOOLS"
NFPM="$TOOLS/nfpm-$NFPM_VERSION"
if [ ! -x "$NFPM" ]; then
    tgz="$TOOLS/nfpm-$NFPM_VERSION.tar.gz"
    curl -fsSL --retry 3 -o "$tgz" \
        "https://github.com/goreleaser/nfpm/releases/download/v$NFPM_VERSION/nfpm_${NFPM_VERSION}_Linux_x86_64.tar.gz"
    echo "$NFPM_SHA256  $tgz" | sha256sum -c -
    tar -C "$TOOLS" -xzf "$tgz" nfpm
    mv "$TOOLS/nfpm" "$NFPM"
fi

# Lançadores em /usr/bin (o executável real fica em /opt/clivcut/bin, ao lado das bibliotecas).
for exe in clivcut clivcut-render; do
    printf '#!/bin/sh\nexec /opt/clivcut/bin/%s "$@"\n' "$exe" > "$STAGE/$exe.sh"
    chmod 755 "$STAGE/$exe.sh"
done


cat > "$STAGE/nfpm.yaml" <<EOF
name: clivcut
arch: amd64
platform: linux
version: $VERSION
release: 1
section: video
priority: optional
maintainer: Thiago Vespa <thiago@thiagovespa.com.br>
description: |
  Open-source, cross-platform video editor.
  Timeline editing with GPU compositing, text and stickers, color grading,
  effects and transitions, automatic captions and local AI tools.
vendor: clivcut
homepage: https://github.com/thiagovespa/clivcut
license: GPL-3.0-or-later
contents:
  - src: $TREE
    dst: /opt/clivcut
    type: tree
  - src: $STAGE/clivcut.sh
    dst: /usr/bin/clivcut
    file_info: { mode: 0755 }
  - src: $STAGE/clivcut-render.sh
    dst: /usr/bin/clivcut-render
    file_info: { mode: 0755 }
  - src: $TREE/share/applications/io.github.clivcut.desktop
    dst: /usr/share/applications/io.github.clivcut.desktop
  - src: $TREE/share/icons/hicolor/scalable/apps/io.github.clivcut.svg
    dst: /usr/share/icons/hicolor/scalable/apps/io.github.clivcut.svg
  - src: $TREE/share/metainfo/io.github.clivcut.metainfo.xml
    dst: /usr/share/metainfo/io.github.clivcut.metainfo.xml
overrides:
  deb:
    depends:
$DEB_DEPENDS
    # Driver de vídeo (OpenGL/Vulkan por software ou da GPU) e decodificação por hardware.
    recommends:
      - libgl1-mesa-dri
      - mesa-va-drivers
  rpm:
    depends:
$RPM_DEPENDS
    recommends:
      - mesa-dri-drivers
EOF

"$NFPM" package --config "$STAGE/nfpm.yaml" --packager deb --target "$OUT/"
"$NFPM" package --config "$STAGE/nfpm.yaml" --packager rpm --target "$OUT/"
ls -la "$OUT"/clivcut*"$VERSION"*
