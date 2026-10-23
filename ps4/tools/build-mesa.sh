#!/usr/bin/env bash
# Build Mesa's RADV (Vulkan for the PS4 GPU) with DolphinPS4's scripts/build-mesa.sh,
# which uses the patched tree in refs/DolphinPS4 (GNM winsys) and src/mesa (WSI videoout).
#   build-mesa.sh [setup]        regular build in build/mesa
#   FLAVOR=final build-mesa.sh   release winsys (PS4_WINSYS_DIAG=0) in build/mesa-final
set -euo pipefail
source "$(dirname "$0")/env.sh"

# Host tools meson/mako/pyyaml for the cross build (build-mesa.sh puts mesa-venv first
# in PATH; system glslangValidator is used for the SPIR-V of host-side tools).
if [ ! -d "$PS4_PORT_ROOT/mesa-venv" ]; then
    python3 -m venv "$PS4_PORT_ROOT/mesa-venv"
    # packaging: Python 3.12+ has no distutils, which Mesa's meson.build otherwise requires.
    "$PS4_PORT_ROOT/mesa-venv/bin/pip" install -q meson mako pyyaml packaging
fi

FLAVOR="${FLAVOR:-dev}"
if [ "$FLAVOR" = final ]; then
    export MESA_BUILD="$PS4_PORT_ROOT/build/mesa-final"
    export MESA_EXTRA_FLAGS="${MESA_EXTRA_FLAGS:-}, '-DPS4_WINSYS_DIAG=0'"
fi

# Our Mesa delta on top of dusklight's patch: disable dep_elf on PS4 (see the patch).
# Idempotent: applies only when the tree does not have it yet.
if git -C "$PS4_PORT_ROOT/src/mesa" apply --check \
        "$PS4_PORT_ROOT/patches/mesa-26.0.8-triaevum.patch" 2>/dev/null; then
    git -C "$PS4_PORT_ROOT/src/mesa" apply "$PS4_PORT_ROOT/patches/mesa-26.0.8-triaevum.patch"
fi

# Keep the linker away from the HOST's libraries. The reference builds ran on WSL Ubuntu
# without libelf-dev, so meson's cc.find_library('elf') failed and Mesa built RADV without
# ac_rtld (the upstream #if !defined(USE_LIBELF) path - the configuration DolphinPS4 and
# dusklight r25 run on real consoles). This distro ships libelf.so in /usr/lib, and ld.lld 12
# searches the host defaults, which silently pulled host libelf in and then broke the eboot
# link with undefined elf_* symbols. -Wl,--sysroot restores cross hygiene: only the SDK's
# own libs resolve (they are on the -L path already). One token with '=' - a separate
# path argument would be swallowed by the clang driver as an input file.
export MESA_EXTRA_LINK="${MESA_EXTRA_LINK:-}, '-Wl,--sysroot=$OPENORBIS'"

exec bash "$DOLPHINPS4/scripts/build-mesa.sh" "$@"
