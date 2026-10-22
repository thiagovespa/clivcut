#!/usr/bin/env bash
# Instala os pacotes de dist/ em contêineres limpos e confere que o clivcut abre e o
# clivcut-render renderiza (num X virtual) e lê mídia sem tela nenhuma. Pega dependência
# faltando e plugin ausente.
#
# Uso: packaging/linux/test-packages.sh [pasta dos pacotes]   (requer Docker e ffmpeg)
# DISTROS escolhe os testes (padrão: todos): ubuntu-24.04 debian-13 fedora-42 opensuse-tumbleweed arch-tar.gz
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
DIST="$(cd "${1:-$ROOT/dist}" && pwd)"
DISTROS="${DISTROS:-ubuntu-24.04 debian-13 fedora-42 opensuse-tumbleweed arch-tar.gz}"

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
cp "$DIST"/clivcut_*_amd64.deb "$DIST"/clivcut-*.x86_64.rpm "$DIST"/clivcut-*-linux-x86_64.tar.gz "$WORK/"

ffmpeg -loglevel error -y -f lavfi -i testsrc2=size=1280x720:rate=30 -f lavfi -i sine=f=440 -t 2 \
    -c:v libx264 -pix_fmt yuv420p -c:a aac -shortest "$WORK/clip.mp4"
cat > "$WORK/project.clivcut" <<'EOF'
{"format": "clivcut", "version": 1, "id": "t", "name": "t", "width": 1280, "height": 720, "fps": 30,
 "background": "#000000", "mainTrackMagnet": true,
 "assets": [{"id": "a1", "kind": "video", "path": "/t/clip.mp4", "name": "clip.mp4", "duration": 2000000,
             "width": 1280, "height": 720, "fps": 30, "hasAudio": true, "hasVideo": true}],
 "tracks": [
  {"id": "t0", "kind": "video", "name": "Main", "clips": [
    {"id": "c1", "type": "media", "assetId": "a1", "start": 0, "duration": 2000000, "sourceIn": 0, "speed": 1, "volume": 1,
     "transform": {"x": 0, "y": 0, "scale": 1, "rotation": 0, "opacity": 1, "flipH": false, "flipV": false}}]},
  {"id": "t1", "kind": "text", "name": "Text", "clips": [
    {"id": "x1", "type": "text", "text": "clivcut", "start": 0, "duration": 2000000,
     "style": {"fontFamily": "Sans Serif", "fontSize": 96, "bold": true, "color": "#ffffff", "align": 1},
     "transform": {"x": 0, "y": 0, "scale": 1, "rotation": 0, "opacity": 1, "flipH": false, "flipV": false}}]}]}
EOF

cat > "$WORK/check.sh" <<'EOF'
#!/bin/sh
# Dentro do contêiner, com o pacote instalado.
set -e
APP=${1:-clivcut}; RENDER=${2:-clivcut-render}
export HOME=/tmp LC_ALL=C.UTF-8
echo "-- $(. /etc/os-release; echo "$PRETTY_NAME")"
# Sem tela nenhuma (plugin offscreen): os comandos que não usam a GPU.
env -u DISPLAY -u WAYLAND_DISPLAY "$RENDER" --probe /t/clip.mp4 | grep -q 'size: 1280x720' || { echo "FAIL: headless probe"; exit 1; }
# Render: Vulkan/OpenGL por software num X virtual.
xvfb-run -a "$RENDER" /t/project.clivcut /tmp/out.mp4 --height 360 >/dev/null 2>/tmp/render.log \
    || { tail -8 /tmp/render.log; echo "FAIL: render"; exit 1; }
"$RENDER" --probe /tmp/out.mp4 | grep -q 'size: 640x360' || { echo "FAIL: output"; exit 1; }
# Interface: plugin xcb num X virtual.
xvfb-run -a "$APP" --version || { echo "FAIL: GUI"; exit 1; }
echo "PASS"
EOF
chmod +x "$WORK/check.sh"

run() {
    local name=$1 image=$2 script=$3
    case " $DISTROS " in *" $name "*) ;; *) return 0 ;; esac
    echo "===== $name"
    if docker run --rm -v "$WORK":/t:ro "$image" sh -c "$script" > "$WORK/$name.log" 2>&1; then
        grep -E '^(-- |PASS)' "$WORK/$name.log"
    else
        tail -15 "$WORK/$name.log"
        FAILED+=" $name"
    fi
}

FAILED=""
APT='export DEBIAN_FRONTEND=noninteractive; apt-get update -qq && apt-get install -y -qq /t/clivcut_*_amd64.deb xvfb xauth mesa-vulkan-drivers >/dev/null && /t/check.sh'
DNF='dnf install -y -q /t/clivcut-*.x86_64.rpm xorg-x11-server-Xvfb xauth mesa-vulkan-drivers >/dev/null && /t/check.sh'
ZYP='zypper -q --non-interactive --no-gpg-checks install --allow-unsigned-rpm /t/clivcut-*.x86_64.rpm xvfb-run libvulkan_lvp >/dev/null && /t/check.sh'
# .tar.gz: as bibliotecas de DEPENDENCIES.txt, pelos nomes do Arch.
PAC='pacman -Sy --noconfirm --needed -q mesa vulkan-swrast libglvnd harfbuzz fribidi fontconfig freetype2 alsa-lib wayland libx11 libxcb gmp libgpg-error e2fsprogs xorg-server-xvfb xorg-xauth >/dev/null && tar -C /opt -xzf /t/clivcut-*-linux-x86_64.tar.gz && d=$(ls -d /opt/clivcut-*) && /t/check.sh $d/bin/clivcut $d/bin/clivcut-render'
run ubuntu-24.04 ubuntu:24.04 "$APT"
run debian-13 debian:13 "$APT"
run fedora-42 fedora:42 "$DNF"
run opensuse-tumbleweed opensuse/tumbleweed "$ZYP"
run arch-tar.gz archlinux:latest "$PAC"

if [ -n "$FAILED" ]; then
    echo "Falhou:$FAILED" >&2
    exit 1
fi
