#!/usr/bin/env bash
# Regenerates ports/ps4-product/generated/{closure.json,product_sources.cmake}
# from a host configure of ../TriAevum (Switch-like closure: whole-AOT plugin
# mode, no Vulkan/NRI). Run after every upstream sync, then build-product.sh.
# The host packages the product does not need are stubbed (host-stubs/).
#   tools/product/regen-closure.sh
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
port="$(cd "$HERE/../.." && pwd)"
tri="$(cd "$port/../TriAevum" && pwd)"
host="$port/build/tri-host"
gen="$port/ports/ps4-product/generated"
mkdir -p "$host/.cmake/api/v1/query"
touch "$host/.cmake/api/v1/query/codemodel-v2"
cmake -S "$tri" -B "$host" -G Ninja -Wno-dev -Wno-deprecated \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
    -DCMAKE_PREFIX_PATH="$HERE/host-stubs" \
    -DOOT3D_ENABLE_VULKAN_RENDERER=OFF -DOOT3D_DIRECT_AOT_PLUGIN=ON \
    -DOOT3D_WHOLE_AOT_PRODUCT_MODE=ON >/dev/null
python3 "$HERE/closure.py" "$host" oot3d_native_game --json "$gen/closure.json" | tail -1
TRIAEVUM_ROOT="$tri" python3 "$HERE/gen_cmake.py" "$gen/closure.json" "$host" \
    "$gen/product_sources.cmake"
commit="$(git -C "$tri" rev-parse HEAD 2>/dev/null || true)"
if [ -n "$commit" ]; then
    sed -i -E "s/(kTriAevumSourceCommit = \")[^\"]*/\1$commit/" "$gen/triaevum_product_build.h"
fi
