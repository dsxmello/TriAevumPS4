#!/usr/bin/env bash
# SDL2 2.0.18 for the PS4 product: the PacBrew fork (the source of
# ps4-openorbis-sdl2-2.0.18-16, same commit and archive hash as its PKGBUILD)
# built with THIS toolchain and only the subsystems the product uses there:
# events, joystick (scePad), game controller, audio (sceAudioOut), timers.
# Video and render are off - the PacBrew package links its video/render
# drivers against EGL/GLES (Piglet), which the plain-identity Vulkan eboot
# must not import - and so is libsamplerate (SDL's own resampler is used).
#   downloads/pacbrew-sdl-<commit>.zip  sha256 97b6ed13fb3aa49dfa35737981458c6cecf8baae5fa87ae7216bcb2a09df70a9
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
source "$HERE/env.sh"
COMMIT=bf797a5315c7dbc0757d8ae7228e2dff1bf370cb
SHA=97b6ed13fb3aa49dfa35737981458c6cecf8baae5fa87ae7216bcb2a09df70a9
zip="$PS4_BUILD_ROOT/downloads/pacbrew-sdl-$COMMIT.zip"
src="$PS4_BUILD_ROOT/src/SDL-$COMMIT"
build="$PS4_BUILD_ROOT/build/sdl2"
prefix="$PS4_SYSROOT/sdl2"

if [ ! -d "$src" ]; then
    [ -f "$zip" ] || curl -sSL -o "$zip" "https://github.com/PacBrew/SDL/archive/$COMMIT.zip"
    echo "$SHA  $zip" | sha256sum -c -
    (cd "$PS4_BUILD_ROOT/src" && python3 -I -m zipfile -e "$zip" .)
fi
# The fork forces libsamplerate on for the PS4; make it follow the option.
sed -i 's/^  set(SDL_LIBSAMPLERATE ON)$/  # set(SDL_LIBSAMPLERATE ON)  (triaevum: follows the option)/;
        s/^  CheckLibSampleRate()$/  if(SDL_LIBSAMPLERATE)\n    CheckLibSampleRate()\n  endif()/' \
    "$src/CMakeLists.txt"

rm -rf "$build"
env -u CFLAGS -u CXXFLAGS -u CPPFLAGS -u LDFLAGS -u LIBS \
    cmake -S "$src" -B "$build" -G "Unix Makefiles" \
    -DCMAKE_TOOLCHAIN_FILE="$DOLPHINPS4/toolchain/ps4-love-modern.cmake" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
    -DCMAKE_C_FLAGS_RELEASE="-O2 -DNDEBUG -march=btver2 -Wno-error=declaration-after-statement" \
    -DCMAKE_INSTALL_PREFIX="$prefix" \
    -DSDL_SHARED=OFF -DSDL_STATIC=ON -DSDL_TEST=OFF \
    -DSDL_VIDEO=OFF -DSDL_RENDER=OFF -DSDL_OPENGL=OFF -DSDL_OPENGLES=OFF \
    -DSDL_VULKAN=OFF -DSDL_LIBSAMPLERATE=OFF -DSDL_HIDAPI=OFF >/dev/null
make -C "$build" -j"${JOBS:-$(nproc)}" >/dev/null
rm -rf "$prefix"
make -C "$build" install >/dev/null
llvm-nm -u "$prefix/lib/libSDL2.a" 2>/dev/null | grep -ciE ' U (egl|gl[A-Z]|scePiglet|src_)' \
    && { echo "SDL2 still references EGL/GL/Piglet/libsamplerate" >&2; exit 1; } || true
ls "$prefix/lib"
