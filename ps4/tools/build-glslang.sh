#!/usr/bin/env bash
# glslang 15.4.0 (tag commit 8a85691a0740d390761a1008b4696f57facd02c4) as static PS4 libraries,
# for the game's runtime GLSL -> SPIR-V compiles (the same model as DolphinPS4, whose Vulkan
# backend compiles with Dolphin's Externals/glslang on the console; ACO then does SPIR-V -> GCN).
# Front end + SPIR-V back end only: no SPIRV-Tools optimizer, no HLSL, no executables.
#   downloads/glslang-15.4.0.tar.gz  sha256 b16c78e7604b9be9f546ee35ad8b6db6f39bbbbfb19e8d038b6fe2ea5bba4ff4
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
source "$HERE/env.sh"
VER=15.4.0
SHA=b16c78e7604b9be9f546ee35ad8b6db6f39bbbbfb19e8d038b6fe2ea5bba4ff4
tarball="$PS4_BUILD_ROOT/downloads/glslang-$VER.tar.gz"
src="$PS4_BUILD_ROOT/src/glslang-$VER"
build="$PS4_BUILD_ROOT/build/glslang"
prefix="$PS4_SYSROOT/glslang"

if [ ! -d "$src" ]; then
    [ -f "$tarball" ] || curl -sSL -o "$tarball" \
        "https://github.com/KhronosGroup/glslang/archive/refs/tags/$VER.tar.gz"
    echo "$SHA  $tarball" | sha256sum -c -
    tar -xzf "$tarball" -C "$PS4_BUILD_ROOT/src"
fi

rm -rf "$build"
env -u CFLAGS -u CXXFLAGS -u CPPFLAGS -u LDFLAGS -u LIBS \
    cmake -S "$src" -B "$build" -G "Unix Makefiles" \
    -DCMAKE_TOOLCHAIN_FILE="$DOLPHINPS4/toolchain/ps4-love-modern.cmake" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
    -DCMAKE_C_FLAGS_RELEASE="-O2 -DNDEBUG -march=btver2" \
    -DCMAKE_CXX_FLAGS_RELEASE="-O2 -DNDEBUG -march=btver2" \
    -DCMAKE_INSTALL_PREFIX="$prefix" \
    -DBUILD_SHARED_LIBS=OFF \
    -DGLSLANG_TESTS=OFF -DGLSLANG_ENABLE_INSTALL=ON \
    -DENABLE_GLSLANG_BINARIES=OFF -DENABLE_HLSL=OFF -DENABLE_OPT=OFF \
    -DENABLE_SPVREMAPPER=OFF -DENABLE_PCH=OFF -DENABLE_CTEST=OFF \
    -DALLOW_EXTERNAL_SPIRV_TOOLS=OFF >/dev/null
make -C "$build" -j"${JOBS:-$(nproc)}"
rm -rf "$prefix"
make -C "$build" install >/dev/null
ls "$prefix/lib"
