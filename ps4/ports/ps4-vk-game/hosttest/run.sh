#!/usr/bin/env bash
# Builds and runs the host harness for the PS4 Vulkan PICA backend
# (vk_backend_hosttest.cpp) on this Linux machine's Vulkan driver, with the
# Khronos validation layer when VVL_ROOT points at an extracted
# vulkan-validation-layers package (usr/lib + usr/share/vulkan).
#   VVL_ROOT=/path/to/root hosttest/run.sh [build-dir]
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
PORT="$(cd "$HERE/../../.." && pwd)"
TRIAEVUM="$PORT/../TriAevum"
OUT="${1:-${TMPDIR:-/tmp}/triaevum-vk-hosttest}"
mkdir -p "$OUT"
c++ -std=c++20 -O1 -g -Wall -Wno-missing-field-initializers \
    -I"$HERE/../src" -I"$HERE/../third_party" \
    -I"$TRIAEVUM/runtime/three_ds_recomp/include" \
    -I"$PORT/src/mesa/include" \
    "$HERE/vk_backend_hosttest.cpp" "$HERE/../src/ps4_vk_pica_backend.cpp" \
    "$TRIAEVUM/runtime/three_ds_recomp/src/fast/oot3d/pica_texture_decode.cpp" \
    -lglslang -lglslang-default-resource-limits -lSPIRV-Tools-opt -lSPIRV-Tools \
    -lvulkan -lpthread -o "$OUT/vk_backend_hosttest"
if [ -n "${VVL_ROOT:-}" ]; then
    export VK_ADD_LAYER_PATH="$VVL_ROOT/usr/share/vulkan/explicit_layer.d"
    export LD_LIBRARY_PATH="$VVL_ROOT/usr/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
    export VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation
fi
"$OUT/vk_backend_hosttest" \
    "$TRIAEVUM/tools/oot3d/native_pica_frontend/profiles/oot3d_pica_boot_title_kokiri_effective.json"
