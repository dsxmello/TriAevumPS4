#!/usr/bin/env bash
# Build the TriAevum game host (ports/ps4-vk-game) and package it as TRIA00004
# (plain homebrew identity, like the VK probe). The eboot statically links the
# whole-AOT closure, the DolphinPS4 runtime (dlmalloc malloc-replace heap,
# crash log), RADV (tools/build-mesa.sh) and glslang (tools/build-glslang.sh);
# no Piglet, no SDL.
#   build-game.sh [upload]     upload: FTP the PKG to the console ($PS4_HOST)
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
source "$HERE/env.sh"
GAME="$PS4_PORT_ROOT/ports/ps4-vk-game"
build="$PS4_BUILD_ROOT/build/game"
OUT="$PS4_BUILD_ROOT/out"
TITLE_ID="${GAME_TITLE_ID:-TRIA00004}"

[ -f "$PS4_BUILD_ROOT/sysroot/lib/libc++.a" ] || {
    echo "libc++ sysroot missing (run tools/build-libcxx.sh)" >&2; exit 1; }
[ -f "$PS4_BUILD_ROOT/build/mesa/src/amd/vulkan/libvulkan_radeon.a" ] || {
    echo "Mesa RADV not built yet (run tools/build-mesa.sh)" >&2; exit 1; }
[ -f "$PS4_SYSROOT/glslang/lib/libglslang.a" ] || {
    echo "glslang not built yet (run tools/build-glslang.sh)" >&2; exit 1; }
[ -d "$(cd "$PS4_PORT_ROOT/.." && pwd)/title-content/aot-cpp" ] || [ -d "$TITLE_CONTENT_AOT" ] || {
    echo "generated whole-AOT shards missing (title-content/aot-cpp)" >&2; exit 1; }

rm -rf "$build"
env -u CFLAGS -u CXXFLAGS -u CPPFLAGS -u LDFLAGS -u LIBS \
    cmake -S "$GAME" -B "$build" -G "Unix Makefiles" \
    -DCMAKE_TOOLCHAIN_FILE="$DOLPHINPS4/toolchain/ps4-love-modern.cmake" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
    -DPS4_MALLOC_REPLACE=ON -DPS4_PAID=0x3800000000000035 \
    -DPS4_MESA_BUILD="$PS4_BUILD_ROOT/build/mesa" \
    -DTRIAEVUM_ROOT="$PS4_PORT_ROOT/../TriAevum" \
    -DTITLE_CONTENT_AOT="$PS4_PORT_ROOT/../title-content/aot-cpp" >/dev/null
make -C "$build" -j"${JOBS:-$(nproc)}"

eboot="$build/triaevum_game_eboot/eboot.bin"
[ -f "$eboot" ] || { echo "game: build failed" >&2; exit 1; }

stage="$build/stage"
rm -rf "$stage"
mkdir -p "$stage/sce_sys"
cp "$eboot" "$stage/"
cp "$PS4_PORT_ROOT/sce_sys/icon0.png" "$stage/sce_sys/"
pkg="$(SFO_STYLE=plain NO_PIGLET_MODULES=1 "$HERE/make-pkg.sh" "$stage" "$TITLE_ID" \
    "TriAevum PS4" "${GAME_VERSION:-00.94}" TRIAEVUMVKGAME00 "$OUT" | tail -1)"
echo "game: $(basename "$pkg")"
if [ "${1:-}" = upload ]; then
    curl -sS -T "$pkg" "ftp://${PS4_HOST}:${PS4_FTP_PORT}/data/pkg/"
    echo "uploaded $(basename "$pkg")"
fi
