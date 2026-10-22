# syntax=docker/dockerfile:1
# Builder dos pacotes macOS do clivcut (x86_64 e arm64), num container Linux.
#
# Cross-compile com clang + ld64.lld contra o SDK da Apple, que entra pelo
# contexto nomeado "sdk" (`make macos-sdk` o exporta do Mac do desenvolvedor;
# na CI vem do runner macOS). O Qt é o pacote oficial (universal) mais o de
# Linux para as ferramentas de host (moc, qsb, qmlcachegen...). FFmpeg, codecs
# e SDL2 são compilados aqui, uma vez por arquitetura, em camadas cacheadas.
#
#   .app  packaging/macos/bundle.py (Qt, plugins e QML no pacote; rcodesign ad hoc)
#   .dmg  packaging/macos/dmg.py   (mkfs.hfsplus + libdmg-hfsplus, sem hdiutil)
#
# Uso: pelo Makefile da raiz — `make dmg`, `make dmg-all`.

ARG BASE=ubuntu:noble-20260905

# libdmg-hfsplus (hfsplus + dmg): revisão fixa, o build sempre roda a mesma.
FROM ${BASE} AS libdmg
ARG LIBDMG_REVISION=ec239599c1f234a4e01ae3fe51214d0c77e5baa3
ARG LIBDMG_SHA256=190e29e4e0bd53e31af1f617b62dcd46b300ffbb0b7cd9e1bf49adb218872763
RUN apt-get update && apt-get install -y --no-install-recommends \
        ca-certificates curl build-essential cmake zlib1g-dev libbz2-dev libssl-dev \
 && curl -fsSL -o /tmp/libdmg.tar.gz "https://github.com/mozilla/libdmg-hfsplus/archive/${LIBDMG_REVISION}.tar.gz" \
 && echo "${LIBDMG_SHA256}  /tmp/libdmg.tar.gz" | sha256sum -c - \
 && tar -xzf /tmp/libdmg.tar.gz -C /tmp \
 && cmake -S /tmp/libdmg-hfsplus-${LIBDMG_REVISION} -B /tmp/libdmg-build -DCMAKE_BUILD_TYPE=Release \
 && cmake --build /tmp/libdmg-build --target dmg-bin hfsplus -j"$(nproc)" \
 && install -m 755 /tmp/libdmg-build/dmg/dmg /tmp/libdmg-build/hfs/hfsplus /usr/local/bin/

FROM ${BASE} AS base
ARG LLVM_VERSION=21
ARG QT_VERSION=6.8.3
ARG RCODESIGN_VERSION=0.29.0
ARG RCODESIGN_SHA256=dbe85cedd8ee4217b64e9a0e4c2aef92ab8bcaaa41f20bde99781ff02e600002
# Fonte do fundo do DMG, travada num commit do google/fonts.
ARG FONT_COMMIT=7dc16b7de42db624b902f2292b68ed9e489e5053
ARG FONT_SHA256=d3bf1bdaf0550e83da9ac0b1d1d9fe6db086835a83aa28578e609a394b9a0286

# LLVM do apt.llvm.org: clang, ld64.lld e as ferramentas Mach-O (otool,
# install-name-tool, lipo). Sem pacotes -dev do Linux: o CMake do cross-compile
# não pode achar bibliotecas do host por engano.
RUN apt-get update && apt-get install -y --no-install-recommends ca-certificates curl gnupg \
 && curl -fsSL https://apt.llvm.org/llvm-snapshot.gpg.key | gpg --dearmor -o /usr/share/keyrings/llvm.gpg \
 && echo "deb [signed-by=/usr/share/keyrings/llvm.gpg] http://apt.llvm.org/noble/ llvm-toolchain-noble-${LLVM_VERSION} main" \
      > /etc/apt/sources.list.d/llvm.list \
 && apt-get update && apt-get install -y --no-install-recommends \
        clang-${LLVM_VERSION} lld-${LLVM_VERSION} llvm-${LLVM_VERSION} \
        python3.12 python3.12-venv git make nasm ninja-build pkgconf perl \
        xz-utils bzip2 file hfsprogs libbz2-1.0 zlib1g libssl3t64 \
 && rm -rf /var/lib/apt/lists/*
COPY --from=libdmg /usr/local/bin/dmg /usr/local/bin/hfsplus /usr/local/bin/

# Python do builder num venv: CMake recente, Meson (dav1d), aqtinstall (Qt) e
# as libs da arte e do .DS_Store do DMG.
RUN python3.12 -m venv /opt/builder \
 && /opt/builder/bin/pip install --no-cache-dir --upgrade pip \
 && /opt/builder/bin/pip install --no-cache-dir "cmake==3.31.6" "meson==1.6.1" "aqtinstall==3.3.0" \
        "Pillow>=10" "ds_store==1.3.3" "mac_alias==2.2.3"
ENV PATH=/opt/darwin/bin:/usr/lib/llvm-${LLVM_VERSION}/bin:/opt/builder/bin:$PATH \
    LLVM_VERSION=${LLVM_VERSION} \
    QT_VERSION=${QT_VERSION}

# rcodesign: assinatura ad hoc no Linux (o arm64 não roda código sem assinatura).
RUN curl -fsSL -o /tmp/rcodesign.tar.gz "https://github.com/indygreg/apple-platform-rs/releases/download/apple-codesign%2F${RCODESIGN_VERSION}/apple-codesign-${RCODESIGN_VERSION}-x86_64-unknown-linux-musl.tar.gz" \
 && echo "${RCODESIGN_SHA256}  /tmp/rcodesign.tar.gz" | sha256sum -c - \
 && tar -xzf /tmp/rcodesign.tar.gz -C /tmp \
 && install -m 755 /tmp/apple-codesign-*/rcodesign /usr/local/bin/ && rm -rf /tmp/apple-codesign-* /tmp/rcodesign.tar.gz

# Fonte do texto do fundo do DMG (OFL).
RUN mkdir -p /opt/fonts \
 && curl -fsSL -o /opt/fonts/Poppins-SemiBold.ttf https://github.com/google/fonts/raw/${FONT_COMMIT}/ofl/poppins/Poppins-SemiBold.ttf \
 && echo "${FONT_SHA256}  /opt/fonts/Poppins-SemiBold.ttf" | sha256sum -c -

# Qt: macOS (universal, as bibliotecas do app) + Linux (ferramentas de host).
RUN aqt install-qt mac desktop ${QT_VERSION} clang_64 -m qtshadertools -O /opt/qt \
 && aqt install-qt linux desktop ${QT_VERSION} linux_gcc_64 -m qtshadertools -O /opt/qt \
 && rm -rf /opt/qt/${QT_VERSION}/*/doc /opt/qt/${QT_VERSION}/*/examples aqtinstall.log

# SDK da Apple (contexto "sdk") e o osxcross em cima dele: ld64 + cctools da
# Apple (flavor "latest"). Revisão fixa, o build sempre roda a mesma.
FROM base AS toolchain
ARG MACOS_MIN=13.0
ARG OSXCROSS_COMMIT=27d21e4977c9751d01199c7a226a6faf494c3dd9
ENV MACOS_MIN=${MACOS_MIN}
RUN apt-get update && apt-get install -y --no-install-recommends \
        cpio patch libxml2-dev libssl-dev zlib1g-dev liblzma-dev libbz2-dev uuid-dev libc6-dev \
 && rm -rf /var/lib/apt/lists/* \
 && ln -s /usr/lib/llvm-${LLVM_VERSION}/bin/clang /usr/local/bin/cc \
 && ln -s /usr/lib/llvm-${LLVM_VERSION}/bin/clang++ /usr/local/bin/c++
RUN --mount=type=bind,from=sdk,target=/mnt/sdk \
    git clone -q https://github.com/tpoechtrager/osxcross.git /tmp/osxcross \
 && git -C /tmp/osxcross checkout -q ${OSXCROSS_COMMIT} \
 && mkdir -p /tmp/sdk && tar -xzf /mnt/sdk/MacOSX.sdk.tar.gz -C /tmp/sdk \
 && v=$(python3 -c "import json; print(json.load(open('/tmp/sdk/MacOSX.sdk/SDKSettings.json'))['Version'])") \
 && mv /tmp/sdk/MacOSX.sdk /tmp/sdk/MacOSX$v.sdk \
 && tar -C /tmp/sdk -cJf /tmp/osxcross/tarballs/MacOSX$v.sdk.tar.xz MacOSX$v.sdk \
 && cd /tmp/osxcross \
 && UNATTENDED=1 BUILD_FLAVOR=latest TARGET_DIR=/opt/osxcross OSX_VERSION_MIN=${MACOS_MIN} \
      ENABLE_ARCHS="arm64 x86_64" JOBS=$(nproc) ./build.sh \
 && mkdir -p /opt/sdk && ln -s /opt/osxcross/SDK/MacOSX$v.sdk /opt/sdk/MacOSX.sdk \
 && rm -rf /tmp/osxcross /tmp/sdk
# O wrapper do osxcross acha o ld64 (<alvo>-ld) pelo PATH.
ENV PATH=/opt/darwin/bin:/opt/osxcross/bin:$PATH
# compiler-rt de Darwin (libclang_rt.osx.a): o @available() do Objective-C
# (SDL2, ggml/Metal) precisa do ___isPlatformVersionAtLeast que mora nele.
RUN git clone -q https://github.com/tpoechtrager/osxcross.git /tmp/osxcross \
 && git -C /tmp/osxcross checkout -q ${OSXCROSS_COMMIT} \
 && cd /tmp/osxcross && ENABLE_COMPILER_RT_INSTALL=1 JOBS=$(nproc) ./build_compiler_rt.sh \
 && rm -rf /tmp/osxcross \
 && ls /usr/lib/llvm-${LLVM_VERSION}/lib/clang/*/lib/darwin/libclang_rt.osx.a
COPY toolchain/setup.sh /opt/darwin/setup.sh
RUN sh /opt/darwin/setup.sh

# Dependências nativas, uma camada por arquitetura.
FROM toolchain AS builder
COPY deps.sh /opt/darwin/deps.sh
RUN sh /opt/darwin/deps.sh x86_64
RUN sh /opt/darwin/deps.sh arm64

# Bibliotecas que as ferramentas de host do Qt de Linux carregam (moc, qsb,
# qmlimportscanner...). Só runtime: nada de -dev para o cross-compile achar.
RUN apt-get update && apt-get install -y --no-install-recommends \
        libglib2.0-0t64 libdbus-1-3 libegl1 libgl1 libx11-6 libxkbcommon0 libfontconfig1 libfreetype6 \
 && rm -rf /var/lib/apt/lists/*

# Os scripts de build/empacotamento vêm do repositório montado em /src: mudar
# um deles não refaz a imagem.
ENV HOME=/tmp PYTHONDONTWRITEBYTECODE=1 PYTHONUNBUFFERED=1
WORKDIR /src
ENTRYPOINT ["sh", "/src/packaging/macos/build.sh"]
