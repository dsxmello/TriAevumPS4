#!/usr/bin/env bash
# libzip 1.11.4 (TriAevum's pin; tag commit 6f8a0cdd24a0dc6cce9dac4a7679da784ab124ea)
# static for the PS4 product, configured like TriAevum's Switch build: zlib
# only (the SDK's), no bzip2/lzma/zstd, no crypto, no tools.
#   downloads/libzip-1.11.4.tar.gz  sha256 4844595615e2436e3cf1ed46a1b260fbdaf8b8fa8f2b594e8b5d8162c696b8b2
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
source "$HERE/env.sh"
SHA=4844595615e2436e3cf1ed46a1b260fbdaf8b8fa8f2b594e8b5d8162c696b8b2
tarball="$PS4_BUILD_ROOT/downloads/libzip-1.11.4.tar.gz"
src="$PS4_BUILD_ROOT/src/libzip-1.11.4"
build="$PS4_BUILD_ROOT/build/libzip"
prefix="$PS4_SYSROOT/libzip"

if [ ! -d "$src" ]; then
    [ -f "$tarball" ] || curl -sSL -o "$tarball" \
        https://github.com/nih-at/libzip/archive/refs/tags/v1.11.4.tar.gz
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
    -DCMAKE_INSTALL_PREFIX="$prefix" \
    -DBUILD_SHARED_LIBS=OFF -DBUILD_DOC=OFF -DBUILD_EXAMPLES=OFF \
    -DBUILD_REGRESS=OFF -DBUILD_TOOLS=OFF -DBUILD_OSSFUZZ=OFF \
    -DENABLE_BZIP2=OFF -DENABLE_LZMA=OFF -DENABLE_ZSTD=OFF \
    -DENABLE_OPENSSL=OFF -DENABLE_GNUTLS=OFF -DENABLE_MBEDTLS=OFF \
    -DENABLE_COMMONCRYPTO=OFF -DENABLE_WINDOWS_CRYPTO=OFF >/dev/null
make -C "$build" -j"${JOBS:-$(nproc)}" zip >/dev/null
mkdir -p "$prefix/lib" "$prefix/include"
cp "$build/lib/libzip.a" "$prefix/lib/"
cp "$src/lib/zip.h" "$build/zipconf.h" "$prefix/include/"
ls "$prefix/lib" "$prefix/include"
