#!/usr/bin/env bash
# VVenC and VVdeC for Windows, cross-compiled inside quasi-msys2's
# environment (source env/all.src first) and installed into its MSYS2
# prefix, where pkg-config finds them like any MSYS2 package. Static
# libraries; SIMD picked at run time, so any x86-64 CPU runs them.
set -euo pipefail

VVENC_VERSION=${VVENC_VERSION:-1.14.0}
VVDEC_VERSION=${VVDEC_VERSION:-3.2.1}
prefix=${MSYSTEM_PREFIX:?source quasi-msys2 env/all.src first}
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

build() {
    local name=$1 version=$2
    shift 2
    git clone --quiet --depth 1 --branch "v$version" "https://github.com/fraunhoferhhi/$name.git" "$work/$name"
    cmake -S "$work/$name" -B "$work/$name/build" -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DBUILD_SHARED_LIBS=OFF \
        -DCMAKE_INSTALL_PREFIX="$prefix" \
        "$@"
    cmake --build "$work/$name/build"
    cmake --install "$work/$name/build"
}

build vvenc "$VVENC_VERSION" -DVVENC_INSTALL_FULLFEATURE_APP=OFF -DVVENC_ENABLE_LINK_TIME_OPT=OFF
build vvdec "$VVDEC_VERSION" -DVVDEC_ENABLE_LINK_TIME_OPT=OFF

# pkg-config runs with quasi-msys2's sysroot, as MSYS2's own .pc files
# expect: paths in them start at /ucrt64, not at the Linux path.
msys_prefix="/${prefix##*/}"
sed -i "s|$prefix|$msys_prefix|g" "$prefix"/lib/pkgconfig/libvvenc.pc "$prefix"/lib/pkgconfig/libvvdec.pc
pkg-config --modversion libvvenc libvvdec
