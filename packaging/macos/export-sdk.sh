#!/bin/sh
# Exporta o SDK do macOS deste Mac (Command Line Tools ou Xcode) para o builder
# em Docker: <saída>/MacOSX.sdk.tar.gz, com a pasta renomeada para MacOSX.sdk.
# Roda no macOS (o `make macos-sdk` e a CI chamam). O SDK não é versionado nem
# publicado: fica em .cache/ (ignorado pelo git) e, na CI, no cache do Actions.
set -eu

OUT=${1:-.cache/macos-sdk}
SDK=$(xcrun --sdk macosx --show-sdk-path)
SDK=$(cd "$SDK" && pwd -P) # MacOSX.sdk é um link para MacOSX<versão>.sdk
NAME=$(basename "$SDK")

mkdir -p "$OUT"
# Módulos Swift não servem para C/C++/Objective-C e são metade do tamanho.
tar -C "$(dirname "$SDK")" -czf "$OUT/MacOSX.sdk.tar.gz" -s ",^$NAME,MacOSX.sdk," \
    --exclude '*.swiftmodule' --exclude '*.swiftinterface' --exclude '*.swiftdoc' "$NAME"
echo "SDK $NAME -> $OUT/MacOSX.sdk.tar.gz ($(du -h "$OUT/MacOSX.sdk.tar.gz" | cut -f1))"
