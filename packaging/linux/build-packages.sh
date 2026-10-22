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

# --- .tar.gz ---
NAME="clivcut-$VERSION-linux-x86_64"
cp -a "$TREE" "$STAGE/$NAME"
cat > "$STAGE/$NAME/README.txt" <<EOF
clivcut $VERSION for Linux (x86_64)

Run it from this folder:
    ./bin/clivcut
    ./bin/clivcut-render --help

Everything it needs is inside this folder, except the system graphics
libraries (OpenGL/EGL, X11 or Wayland) and glibc 2.39 or newer.
EOF
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
      - libc6 (>= 2.39)
      - libstdc++6
      - libgl1
      - libegl1
      - libfontconfig1
      - libfreetype6
      - libx11-6
      - libx11-xcb1
      - libxcb1
      - libxkbcommon0
      - libxkbcommon-x11-0
      - libdbus-1-3
  rpm:
    # Pelo nome da biblioteca: vale para Fedora e openSUSE, que nomeiam os pacotes diferente.
    depends:
      - libc.so.6(GLIBC_2.39)(64bit)
      - libstdc++.so.6()(64bit)
      - libGL.so.1()(64bit)
      - libEGL.so.1()(64bit)
      - libfontconfig.so.1()(64bit)
      - libfreetype.so.6()(64bit)
      - libX11.so.6()(64bit)
      - libX11-xcb.so.1()(64bit)
      - libxcb.so.1()(64bit)
      - libxkbcommon.so.0()(64bit)
      - libxkbcommon-x11.so.0()(64bit)
      - libdbus-1.so.3()(64bit)
EOF

"$NFPM" package --config "$STAGE/nfpm.yaml" --packager deb --target "$OUT/"
"$NFPM" package --config "$STAGE/nfpm.yaml" --packager rpm --target "$OUT/"
ls -la "$OUT"/clivcut*"$VERSION"*
