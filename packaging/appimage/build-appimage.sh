#!/usr/bin/env bash
# Gera dist/clivcut-<versão>-x86_64.AppImage (Linux, qualquer distro recente).
#
# Requer as mesmas dependências de compilação do README e, na primeira
# execução, baixa o linuxdeploy e o linuxdeploy-plugin-qt.
#
# Variáveis: BUILD_DIR, OUT_DIR, TOOLS_DIR, QMAKE (padrão: qmake6).
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD="${BUILD_DIR:-$ROOT/build-appimage}"
OUT="${OUT_DIR:-$ROOT/dist}"
TOOLS="${TOOLS_DIR:-$BUILD/tools}"
APPDIR="$BUILD/AppDir"
ARCH="$(uname -m)"

VERSION="$(sed -nE 's/^ *VERSION ([0-9.]+)$/\1/p' "$ROOT/CMakeLists.txt" | head -1)"
VERSION="${VERSION:-dev}"

echo "==> Compilando clivcut $VERSION (Release)"
# GGML_NATIVE=OFF: o whisper.cpp não usa instruções exclusivas desta CPU.
cmake -S "$ROOT" -B "$BUILD" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX=/usr \
    -DCLIVCUT_BUILD_TESTS=OFF \
    -DGGML_NATIVE=OFF
cmake --build "$BUILD"

echo "==> Instalando em $APPDIR"
rm -rf "$APPDIR"
DESTDIR="$APPDIR" cmake --install "$BUILD"

echo "==> Ferramentas de empacotamento"
mkdir -p "$TOOLS"
for tool in linuxdeploy linuxdeploy-plugin-qt; do
    file="$TOOLS/$tool-$ARCH.AppImage"
    if [ ! -x "$file" ]; then
        curl -fsSL --retry 3 -o "$file" \
            "https://github.com/linuxdeploy/$tool/releases/download/continuous/$tool-$ARCH.AppImage"
        chmod +x "$file"
    fi
done

# Sem FUSE (contêineres/CI), as ferramentas se extraem e rodam.
if [ ! -e /dev/fuse ] || ! command -v fusermount >/dev/null 2>&1; then
    export APPIMAGE_EXTRACT_AND_RUN=1
fi

export PATH="$TOOLS:$PATH"
# A interface QML é compilada no binário; o scanner precisa das fontes para achar os imports.
export QML_SOURCES_PATHS="$ROOT/src/app/qml"
# O módulo "Clivcut" é embutido; o qmldir gerado no build resolve o import para o scanner.
export QML_MODULES_PATHS="$BUILD/src/app"
# O strip do linuxdeploy é antigo para binários com .relr.dyn (distros recentes).
export NO_STRIP=1
export QMAKE="${QMAKE:-$(command -v qmake6 || command -v qmake)}"
# Wayland além de X11 (o nome do plugin mudou no Qt 6.10+) e offscreen, que o clivcut-render
# usa sem tela (servidor, SSH).
QT_PLUGINS="$("$QMAKE" -query QT_INSTALL_PLUGINS)"
if [ -e "$QT_PLUGINS/platforms/libqwayland.so" ]; then
    export EXTRA_PLATFORM_PLUGINS="libqwayland.so;libqoffscreen.so"
else
    export EXTRA_PLATFORM_PLUGINS="libqwayland-generic.so;libqwayland-egl.so;libqoffscreen.so"
fi
export EXTRA_QT_MODULES="waylandclient"
mkdir -p "$OUT"
export LDAI_OUTPUT="$OUT/clivcut-$VERSION-$ARCH.AppImage"
export OUTPUT="$LDAI_OUTPUT"

echo "==> Implantando dependências e Qt"
"$TOOLS/linuxdeploy-$ARCH.AppImage" \
    --appdir "$APPDIR" \
    --executable "$APPDIR/usr/bin/clivcut" \
    --executable "$APPDIR/usr/bin/clivcut-render" \
    --desktop-file "$APPDIR/usr/share/applications/io.github.clivcut.desktop" \
    --icon-file "$APPDIR/usr/share/icons/hicolor/scalable/apps/io.github.clivcut.svg" \
    --plugin qt

# Integrações do Wayland (EGL, shells, decorações): o plugin Qt não copia
# todas no Qt 6.10+, e sem elas não há contexto OpenGL numa sessão Wayland.
for dir in wayland-graphics-integration-client wayland-shell-integration wayland-decoration-client; do
    if [ -d "$QT_PLUGINS/$dir" ]; then
        mkdir -p "$APPDIR/usr/plugins/$dir"
        cp -n "$QT_PLUGINS/$dir"/*.so "$APPDIR/usr/plugins/$dir/"
        "$TOOLS/linuxdeploy-$ARCH.AppImage" --appdir "$APPDIR" --deploy-deps-only "$APPDIR/usr/plugins/$dir" >/dev/null
    fi
done

echo "==> Gerando o AppImage"
"$TOOLS/linuxdeploy-$ARCH.AppImage" --appdir "$APPDIR" --output appimage

echo "==> Pronto: $LDAI_OUTPUT"
