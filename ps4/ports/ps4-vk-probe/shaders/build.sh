#!/usr/bin/env bash
# Compiles the probe's GLSL to SPIR-V C headers (host glslangValidator from build-mesa.sh's tools).
set -euo pipefail
cd "$(dirname "$0")"
G="${GLSLANG:-glslangValidator}"
for s in triangle.vert triangle.frag; do
    "$G" -V --vn "${s/./_}" -o "${s/./_}.h" "$s" >/dev/null
done
