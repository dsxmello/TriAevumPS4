#!/usr/bin/env bash
# Build the TriAevum PS4 product (ports/ps4-product: oot3d_native_game with
# TopScreen and the F1 GUI on the Vulkan stack). Incremental by default;
# `clean` wipes the build tree first.
#   build-product.sh [clean]
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
source "$HERE/env.sh"
PRODUCT="$PS4_PORT_ROOT/ports/ps4-product"
build="$PS4_BUILD_ROOT/build/product"
# PGO=generate builds the instrumented product in its own tree; PGO=use
# (with PGO_PROFILE=<merged .profdata>) the optimized one.
PGO="${PGO:-}"
[ "$PGO" = generate ] && build="$PS4_BUILD_ROOT/build/product-pgo-gen"
[ "$PGO" = use ] && build="$PS4_BUILD_ROOT/build/product-pgo-use"

SPDLOG_SHA=1586508029a7d0670dfcb2d97575dcdc242d3868a259742b69f100801ab4e16b

if [ ! -d "$PS4_BUILD_ROOT/src/spdlog-1.14.1" ]; then
    tarball="$PS4_BUILD_ROOT/downloads/spdlog-1.14.1.tar.gz"
    [ -f "$tarball" ] || curl -sSL -o "$tarball" \
        https://github.com/gabime/spdlog/archive/refs/tags/v1.14.1.tar.gz
    echo "$SPDLOG_SHA  $tarball" | sha256sum -c -
    tar -xzf "$tarball" -C "$PS4_BUILD_ROOT/src"
fi
# SDL2 from the PacBrew fork without video/render (no Piglet imports).
[ -f "$PS4_SYSROOT/sdl2/lib/libSDL2.a" ] || "$HERE/build-sdl2.sh"
[ -f "$PS4_SYSROOT/libzip/lib/libzip.a" ] || "$HERE/build-libzip.sh"
if [ ! -d "$PS4_BUILD_ROOT/src/tinyxml2-10.0.0" ]; then
    tarball="$PS4_BUILD_ROOT/downloads/tinyxml2-10.0.0.tar.gz"
    [ -f "$tarball" ] || curl -sSL -o "$tarball" \
        https://github.com/leethomason/tinyxml2/archive/refs/tags/10.0.0.tar.gz
    echo "3bdf15128ba16686e69bce256cc468e76c7b94ff2c7f391cc5ec09e40bff3839  $tarball" | sha256sum -c -
    tar -xzf "$tarball" -C "$PS4_BUILD_ROOT/src"
fi
[ -d "$PS4_BUILD_ROOT/src/deps/imgui" ] || {
    echo "vendored deps missing (src/deps: see src/deps/MANIFEST.txt)" >&2; exit 1; }

[ "${1:-}" = clean ] && rm -rf "$build"
if [ ! -f "$build/CMakeCache.txt" ]; then
    env -u CFLAGS -u CXXFLAGS -u CPPFLAGS -u LDFLAGS -u LIBS \
        cmake -S "$PRODUCT" -B "$build" -G Ninja \
        -DCMAKE_TOOLCHAIN_FILE="$DOLPHINPS4/toolchain/ps4-love-modern.cmake" \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
        -DPS4_MALLOC_REPLACE=ON -DPS4_PAID=0x3800000000000035 \
        -DPS4_MESA_BUILD="$PS4_BUILD_ROOT/build/mesa" \
        -DPS4_PGO="$PGO" -DPS4_PGO_PROFILE="${PGO_PROFILE:-}" >/dev/null
fi
ninja -C "$build" -k 0 "${@:2}"

# Package: TRIA00005 (plain identity like the VK probe/game), the product
# beside the module host (TRIA00004). /app0/resources is TriAevum's runtime
# resource root (gamecontrollerdb.txt, scene environments: no title data).
eboot="$build/triaevum_product_eboot/eboot.bin"
[ -f "$eboot" ] || { echo "product: eboot missing" >&2; exit 1; }
stage="$build/stage"
rm -rf "$stage"
mkdir -p "$stage/sce_sys" "$stage/resources"
cp "$eboot" "$stage/"
cp "$PS4_PORT_ROOT/sce_sys/icon0.png" "$stage/sce_sys/"
cp -r "$PS4_PORT_ROOT/../TriAevum/runtime/triaevum_resources/." "$stage/resources/"
pkg="$(SFO_STYLE=plain NO_PIGLET_MODULES=1 "$HERE/make-pkg.sh" "$stage" "${PRODUCT_TITLE_ID:-TRIA00005}" \
    "TriAevum PS4" "${PRODUCT_VERSION:-01.23}" TRIAEVUMPRODUCT0 "$PS4_BUILD_ROOT/out" | tail -1)"
echo "product: $(basename "$pkg")"
