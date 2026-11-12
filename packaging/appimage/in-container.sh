#!/usr/bin/env bash
# Inside the fennectv-appimage container (packaging/appimage/build runs
# it): builds /src into /src/build-appimage, installs it into an AppDir and
# bundles it with every library it needs (glibc and the like stay the
# system's) into /src/dist/FennecTV-<version>-x86_64.AppImage.
set -euo pipefail

src=/src
build=/src/build-appimage
dist=/src/dist
appdir=$build/AppDir
export CCACHE_DIR=$build/.ccache
version=$(tr -d " \t\r\n" <"$src/VERSION")

# Configured afresh each time: the image may have changed under the cache
# (ccache keeps the build itself quick).
cmake --fresh -S "$src" -B "$build/cmake" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX=/usr \
    -DCMAKE_C_COMPILER_LAUNCHER=ccache \
    -DCMAKE_CXX_COMPILER_LAUNCHER=ccache \
    -DFENNECTV_BUILD_TESTS=OFF
cmake --build "$build/cmake"
rm -rf "$appdir"
DESTDIR="$appdir" cmake --install "$build/cmake"
# On linuxdeploy's list of what every system has, but needed by libraries
# it does bundle, so they go with them (small): libcom_err (Kerberos, under
# Qt Network; a bare Debian hasn't it), libgpg-error (libgcrypt's), libgmp
# (GnuTLS's).
mkdir -p "$appdir/usr/lib"
for l in libcom_err.so.2 libgpg-error.so.0 libgmp.so.10; do
    cp -L "/usr/lib/x86_64-linux-gnu/$l" "$appdir/usr/lib/"
done

# The Qt plugin finds Qt through qmake and brings the xcb platform (X11)
# for fennectv-rx --play; Wayland's too.
export QMAKE=/usr/lib/qt6/bin/qmake
export EXTRA_PLATFORM_PLUGINS="libqwayland-generic.so;libqwayland-egl.so"
# linuxdeploy leaves out what every desktop has (EGL, GL, X11, fontconfig:
# the graphics driver's and the desktop's). Only fennectv-rx's window needs
# them; fennectv-tx needs nothing but glibc and zlib, a server will do.
export LINUXDEPLOY_OUTPUT_VERSION="$version"
out="$dist/FennecTV-$version-x86_64.AppImage"
export LDAI_OUTPUT="$out"
rm -f "$out"
cd "$build"
linuxdeploy --appdir "$appdir" \
    --executable "$appdir/usr/bin/fennectv-tx" \
    --executable "$appdir/usr/bin/fennectv-rx" \
    --desktop-file "$appdir/usr/share/applications/fennectv.desktop" \
    --icon-file "$appdir/usr/share/icons/hicolor/scalable/apps/fennectv.svg" \
    --custom-apprun "$src/packaging/appimage/AppRun" \
    --plugin qt \
    --output appimage

echo
echo "$(du -h "$out" | cut -f1) dist/$(basename "$out")"
