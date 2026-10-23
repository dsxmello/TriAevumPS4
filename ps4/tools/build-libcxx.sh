#!/usr/bin/env bash
# Build the libc++ 21 sysroot for the PS4 target (into $PS4_SYSROOT), using
# DolphinPS4's toolchain script as is. Requires the PacBrew SDK (fetch-openorbis.sh)
# and the LLVM runtimes source (downloads/llvm-project-21.1.6.src.tar.xz).
set -euo pipefail
source "$(dirname "$0")/env.sh"
[ -d "$OPENORBIS" ] || { echo "SDK missing: $OPENORBIS (run tools/fetch-openorbis.sh)" >&2; exit 1; }
[ -f "$PSB/downloads/llvm-project-$LLVM_VER.src.tar.xz" ] || {
    echo "LLVM source missing (see tools/env.sh layout)" >&2; exit 1; }
exec bash "$DOLPHINPS4/toolchain/build-libcxx.sh"
