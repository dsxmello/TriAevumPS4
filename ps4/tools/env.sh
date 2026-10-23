# Settings shared by the TriAevum PS4 (Vulkan) build scripts. Source this file.
#
# Layout (dusklight-ps4 model): this directory's parent is the build root.
#   src/          Mesa 26.0.8 (patched), libdrm headers, LLVM runtimes source
#   refs/         DolphinPS4 (patched, "as built" ac_ps4_drm.c) - the console runtime
#   sdk/          PacBrew OpenOrbis SDK (fetched by fetch-openorbis.sh)
#   sysroot/      libc++ 21 (DolphinPS4 toolchain/build-libcxx.sh)
#   build/        build trees (mesa, probe)
#   out/          packages
#
# DolphinPS4's toolchain scripts are used as they are; they keep their sources in
# $PSB/src, the libc++ sysroot in $PSB/sysroot and build trees in $PSB/build.
PS4_PORT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
export PS4_PORT_ROOT
export PSB="$PS4_PORT_ROOT"
export PS4_BUILD_ROOT="$PSB"
export PS4_SYSROOT="$PSB/sysroot"
export OPENORBIS="$PS4_PORT_ROOT/sdk/opt/pacbrew/ps4/openorbis"
export OO_PS4_TOOLCHAIN="$OPENORBIS"
export DOLPHINPS4="$PS4_PORT_ROOT/refs/DolphinPS4"
export LLVM_VER=21.1.6

# DolphinPS4's scripts invoke the host compiler as clang-21/clang++-21 (LLVM 21 was
# what the reference builds used). This machine has LLVM 23, which also builds
# libc++ 21 and Mesa fine; provide the version-suffixed names as symlinks.
export PATH="$PS4_PORT_ROOT/tools/host-bin:$PATH"

# The SDK's ld.lld (an older lld) links against libxml2.so.2; this distro ships
# libxml2.so.16 (same C API). Give it the private compat link (love-ps4's recipe)
# so ps4-env.sh's ldd check finds it resolved.
export LD_LIBRARY_PATH="$PS4_PORT_ROOT/host-compat${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

# Console (GoldHEN FTP) for upload steps; the operator sets these when testing.
export PS4_HOST="${PS4_HOST:-192.168.0.15}"
export PS4_FTP_PORT="${PS4_FTP_PORT:-2121}"
