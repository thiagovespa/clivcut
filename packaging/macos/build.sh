#!/bin/sh
# Compila o clivcut para macOS e gera um .dmg por arquitetura pedida:
#
#   dist/clivcut-<versão>-macos-<arch>.dmg      <arch> = x86_64 | arm64
#
# Roda dentro do builder (packaging/macos/Dockerfile), com o repositório em
# /src e as pastas de build no volume /cache. Uso: `make dmg` / `make dmg-all`.
set -eu

[ $# -gt 0 ] || set -- x86_64 arm64
QT_MAC=/opt/qt/$QT_VERSION/macos
QT_HOST=/opt/qt/$QT_VERSION/gcc_64
# Um compilador de C++ com Qt passa de 1 GB: limita os jobs pela memória do Docker.
MEM_GB=$(awk '/MemTotal/ { print int($2 / 1048576) }' /proc/meminfo)
JOBS=$(nproc)
[ "$MEM_GB" -lt "$JOBS" ] && JOBS=$(( MEM_GB > 1 ? MEM_GB : 1 ))

cd /src
# O CMake só embute o ícone no .app se o .icns existir na configuração.
python3 packaging/macos/dmg.py --icns packaging/macos/clivcut.icns

for ARCH in "$@"; do
    case $ARCH in
        x86_64) EXTRA="-DGGML_AVX=ON -DGGML_AVX2=ON -DGGML_FMA=ON -DGGML_F16C=ON" ;;  # Haswell+
        arm64)  EXTRA="" ;;
        *) echo "arquitetura desconhecida: $ARCH (use x86_64 ou arm64)" >&2; exit 1 ;;
    esac
    B=/cache/build-$ARCH
    echo "==> [$ARCH] compilando (jobs: $JOBS)"
    # WrapOpenGL_AGL: o SDK 26 não tem mais o AGL e o Qt 6.8 cairia num
    # "-framework AGL" que não linka; o OpenGL.framework no lugar não muda nada.
    # shellcheck disable=SC2086
    cmake -S . -B "$B" -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_TOOLCHAIN_FILE=/opt/darwin/$ARCH.cmake \
        -DCMAKE_PREFIX_PATH="$QT_MAC;/opt/deps/$ARCH" \
        -DQT_HOST_PATH="$QT_HOST" -DQT_NO_APPLE_SDK_AND_XCODE_CHECK=ON \
        -DWrapOpenGL_AGL=/opt/sdk/MacOSX.sdk/System/Library/Frameworks/OpenGL.framework \
        -DCLIVCUT_BUILD_TESTS=OFF -DCLIVCUT_DEPLOY=OFF -DGGML_NATIVE=OFF $EXTRA
    cmake --build "$B" -j "$JOBS"

    VERSION=$(sed -n 's/^CMAKE_PROJECT_VERSION:[A-Z]*=//p' "$B/CMakeCache.txt")
    W=/cache/package-$ARCH
    rm -rf "$W"
    echo "==> [$ARCH] montando o clivcut.app"
    python3 packaging/macos/bundle.py --arch "$ARCH" --build "$B" --qt "$QT_MAC" --qt-host "$QT_HOST" \
        --deps "/opt/deps/$ARCH" --out "$W/clivcut.app"
    echo "==> [$ARCH] gerando o .dmg"
    python3 packaging/macos/dmg.py --app "$W/clivcut.app" --work "$W/dmg" \
        --out "dist/clivcut-$VERSION-macos-$ARCH.dmg"
    rm -rf "$W"
done
ls -lh dist/*.dmg
