#!/bin/sh
# Toolchain de cross-compile para macOS (roda uma vez, no build da imagem),
# por cima do osxcross (/opt/osxcross: clang do sistema + ld64 e cctools da
# Apple). O ld64.lld do LLVM não serve: cai (segfault) com objetos do nasm,
# que o x264, o dav1d, o libvpx e o FFmpeg usam no x86_64.
#
#   /opt/darwin/bin/<arch>-apple-darwin-<ferramenta>  nomes estáveis, sem a versão
#                                                     do Darwin do SDK (darwin25.5...)
#   /opt/darwin/bin/xcrun                             o mínimo que scripts de build perguntam
#   /opt/darwin/<arch>.cmake                          toolchain do CMake
#   /opt/darwin/<arch>.meson                          cross file do Meson
#
# <arch> = x86_64 | arm64.
set -eu

OSXCROSS=/opt/osxcross
SDK=/opt/sdk/MacOSX.sdk
MIN=${MACOS_MIN}
D=/opt/darwin
SDK_VERSION=$(python3 -c "import json; print(json.load(open('$SDK/SDKSettings.json'))['Version'])")
mkdir -p "$D/bin"

for arch in x86_64 arm64; do
    case $arch in
        x86_64) cpu_family=x86_64 ;;
        arm64)  cpu_family=aarch64 ;;
    esac
    # O wrapper do osxcross deduz o alvo do próprio nome: chamamos pelo nome real.
    real=$(ls "$OSXCROSS/bin/$arch-apple-darwin"*-clang | head -1)
    prefix=${real%-clang}
    for tool in clang clang++ ar ranlib nm strip install_name_tool otool lipo libtool; do
        cat > "$D/bin/$arch-apple-darwin-$tool" <<EOF
#!/bin/sh
exec $prefix-$tool "\$@"
EOF
        chmod 755 "$D/bin/$arch-apple-darwin-$tool"
    done
    T=$D/bin/$arch-apple-darwin

    cat > "$D/$arch.cmake" <<EOF
set(CMAKE_SYSTEM_NAME Darwin)
set(CMAKE_SYSTEM_PROCESSOR $arch)
set(CMAKE_OSX_ARCHITECTURES $arch CACHE STRING "")
set(CMAKE_OSX_DEPLOYMENT_TARGET $MIN CACHE STRING "")
set(CMAKE_OSX_SYSROOT $SDK CACHE PATH "")
set(CMAKE_C_COMPILER $T-clang)
set(CMAKE_CXX_COMPILER $T-clang++)
set(CMAKE_OBJC_COMPILER $T-clang)
set(CMAKE_OBJCXX_COMPILER $T-clang++)
set(CMAKE_ASM_COMPILER $T-clang)
set(CMAKE_AR $T-ar CACHE FILEPATH "")
set(CMAKE_RANLIB $T-ranlib CACHE FILEPATH "")
set(CMAKE_NM $T-nm CACHE FILEPATH "")
set(CMAKE_STRIP $T-strip CACHE FILEPATH "")
set(CMAKE_INSTALL_NAME_TOOL $T-install_name_tool CACHE FILEPATH "")
set(CMAKE_OTOOL $T-otool CACHE FILEPATH "")
set(CMAKE_LIPO $T-lipo CACHE FILEPATH "")
# Bibliotecas e headers só do SDK, das dependências e do Qt de macOS; pacotes
# CMake também do host (as ferramentas do Qt de Linux, via QT_HOST_PATH).
set(CMAKE_FIND_ROOT_PATH $SDK /opt/deps/$arch /opt/qt/$QT_VERSION/macos)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE BOTH)
EOF

    cat > "$D/$arch.meson" <<EOF
[binaries]
c = '$T-clang'
cpp = '$T-clang++'
objc = '$T-clang'
ar = '$T-ar'
strip = '$T-strip'
nasm = 'nasm'
pkg-config = 'pkg-config'

[host_machine]
system = 'darwin'
subsystem = 'macos'
cpu_family = '$cpu_family'
cpu = '$arch'
endian = 'little'
EOF
done

# xcrun mínimo: o configure do libvpx e o CMake do Qt perguntam pelo SDK.
# Não existe compilador Metal aqui: `--find metal` falha e quem pergunta
# (FFmpeg, ggml) segue sem shaders pré-compilados.
cat > "$D/bin/xcrun" <<EOF
#!/bin/sh
while [ \$# -gt 0 ]; do
    case "\$1" in
        -sdk|--sdk) shift ;;
        --show-sdk-path) echo $SDK; exit 0 ;;
        --show-sdk-version) echo $SDK_VERSION; exit 0 ;;
        --show-sdk-platform-path) echo /opt/sdk; exit 0 ;;
        -f|--find)
            shift
            case "\$1" in
                cc|clang|c++|clang++|ld|ar|ranlib|nm|strip|otool|lipo|install_name_tool|libtool)
                    ls $OSXCROSS/bin/x86_64-apple-darwin*-"\$1" 2>/dev/null | head -1 ;;
                *) exit 1 ;;
            esac
            exit 0 ;;
        *) exit 1 ;;
    esac
    shift
done
EOF
chmod 755 "$D/bin/xcrun"
echo "toolchain: SDK $SDK_VERSION, macOS >= $MIN, $(basename "$prefix")"
