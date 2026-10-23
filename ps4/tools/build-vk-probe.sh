#!/usr/bin/env bash
# Build the TriAevum Vulkan probe (ports/ps4-vk-probe: RADV on GNM) and package it as
# TRIA00003 (plain homebrew identity: 4.5 GiB of direct memory, no Piglet, no /data/self).
#   build-vk-probe.sh [upload]     upload: FTP the PKG to the console ($PS4_HOST)
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
source "$HERE/env.sh"
PROBE="$PS4_PORT_ROOT/ports/ps4-vk-probe"
build="$PS4_BUILD_ROOT/build/vk-probe"
OUT="$PS4_BUILD_ROOT/out"
TITLE_ID="${PROBE_TITLE_ID:-TRIA00003}"

[ -f "$PS4_BUILD_ROOT/build/mesa/src/amd/vulkan/libvulkan_radeon.a" ] || {
    echo "Mesa RADV not built yet (run tools/build-mesa.sh)" >&2; exit 1; }

rm -rf "$build"
env -u CFLAGS -u CXXFLAGS -u CPPFLAGS -u LDFLAGS -u LIBS \
    cmake -S "$PROBE" -B "$build" -G "Unix Makefiles" \
    -DCMAKE_TOOLCHAIN_FILE="$DOLPHINPS4/toolchain/ps4-love-modern.cmake" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
    -DPS4_MALLOC_REPLACE=OFF -DPS4_PAID=0x3800000000000035 \
    -DPS4_MESA_BUILD="$PS4_BUILD_ROOT/build/mesa" >/dev/null
make -C "$build" -j"${JOBS:-$(nproc)}"

eboot="$build/vk_probe_eboot/eboot.bin"
[ -f "$eboot" ] || { echo "vk probe: build failed" >&2; exit 1; }

stage="$build/stage"
rm -rf "$stage"
mkdir -p "$stage/sce_sys"
cp "$eboot" "$stage/"
cp "$PS4_PORT_ROOT/sce_sys/icon0.png" "$stage/sce_sys/"
pkg="$(SFO_STYLE=plain NO_PIGLET_MODULES=1 "$HERE/make-pkg.sh" "$stage" "$TITLE_ID" \
    "TriAevum VK Probe" "${PROBE_VERSION:-01.00}" TRIAEVUMVKPROBE "$OUT" | tail -1)"
echo "vk probe: $(basename "$pkg")"
if [ "${1:-}" = upload ]; then
    curl -sS -T "$pkg" "ftp://${PS4_HOST}:${PS4_FTP_PORT}/data/pkg/"
    echo "uploaded $(basename "$pkg")"
fi
