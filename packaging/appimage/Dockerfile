# Linux builds of FennecTV as an AppImage, in a container: Ubuntu 24.04
# (glibc 2.39: the AppImage runs on that and anything newer), its Qt 6 and
# libaom; FFmpeg, VVenC and VVdeC built here; linuxdeploy to bundle it all.
# Nothing is installed on the host.
#
#   packaging/appimage/build        (builds this image, then FennecTV)
#
# Layers from the least to the most often changed, so Docker's cache keeps
# them: packages, VVenC/VVdeC, linuxdeploy.
FROM ubuntu:24.04

ARG DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential cmake ninja-build pkg-config git ca-certificates wget file ccache \
        qt6-base-dev qt6-base-dev-tools qmake6 libgl-dev \
        libaom-dev nasm zlib1g-dev libdav1d-dev \
    && rm -rf /var/lib/apt/lists/*

# VVenC and VVdeC: not in Ubuntu, so built here; static, SIMD picked at
# run time, so any x86-64 CPU runs them.
ARG VVENC_VERSION=1.14.0
ARG VVDEC_VERSION=3.2.1
RUN set -e; for p in "vvenc $VVENC_VERSION -DVVENC_INSTALL_FULLFEATURE_APP=OFF -DVVENC_ENABLE_LINK_TIME_OPT=OFF" \
                     "vvdec $VVDEC_VERSION -DVVDEC_ENABLE_LINK_TIME_OPT=OFF"; do \
        set -- $p; name=$1; version=$2; shift 2; \
        git clone --quiet --depth 1 --branch "v$version" "https://github.com/fraunhoferhhi/$name.git" "/tmp/$name"; \
        cmake -S "/tmp/$name" -B "/tmp/$name/build" -G Ninja -DCMAKE_BUILD_TYPE=Release \
            -DBUILD_SHARED_LIBS=OFF -DCMAKE_INSTALL_PREFIX=/usr/local "$@"; \
        cmake --build "/tmp/$name/build"; cmake --install "/tmp/$name/build"; rm -rf "/tmp/$name"; \
    done; pkg-config --modversion libvvenc libvvdec

# FFmpeg of our own: Ubuntu's pulls in X11, GL, ALSA, JACK, libusb... that
# an AppImage doesn't carry (every desktop has them, a server doesn't),
# for nothing FennecTV uses. This one has FFmpeg's own demuxers, decoders,
# filters and network protocols, the v4l2 camera, dav1d for AV1 sources,
# and needs nothing from the system but glibc and zlib.
ARG FFMPEG_VERSION=7.1.1
RUN set -e; git clone --quiet --depth 1 --branch "n$FFMPEG_VERSION" https://git.ffmpeg.org/ffmpeg.git /tmp/ffmpeg; \
    cd /tmp/ffmpeg; ./configure --prefix=/usr/local --enable-shared --disable-static \
        --disable-programs --disable-doc --disable-autodetect \
        --enable-zlib --enable-libdav1d --enable-pthreads \
        --disable-outdevs --disable-indevs --enable-indev=v4l2 --enable-indev=lavfi; \
    make -j"$(nproc)"; make install; cd /; rm -rf /tmp/ffmpeg; ldconfig; \
    pkg-config --modversion libavformat

# Qt's Wayland platform, for fennectv-rx --play on a Wayland desktop.
RUN apt-get update && apt-get install -y --no-install-recommends qt6-wayland \
    && rm -rf /var/lib/apt/lists/*

# linuxdeploy and its Qt plugin (it brings the AppImage output with it),
# unpacked (and readable: the build runs as you): a container has no FUSE
# to mount an AppImage with.
ARG LINUXDEPLOY_URL=https://github.com/linuxdeploy/linuxdeploy/releases/download/continuous/linuxdeploy-x86_64.AppImage
ARG LINUXDEPLOY_QT_URL=https://github.com/linuxdeploy/linuxdeploy-plugin-qt/releases/download/continuous/linuxdeploy-plugin-qt-x86_64.AppImage
RUN set -e; cd /opt; \
    for tool in "linuxdeploy $LINUXDEPLOY_URL" "linuxdeploy-plugin-qt $LINUXDEPLOY_QT_URL"; do \
        set -- $tool; wget -q -O "$1.AppImage" "$2"; chmod +x "$1.AppImage"; \
        "./$1.AppImage" --appimage-extract >/dev/null; mv squashfs-root "$1"; rm "$1.AppImage"; \
        chmod -R a+rX "$1"; ln -s "/opt/$1/AppRun" "/usr/local/bin/$1"; \
    done; linuxdeploy --version
# Any AppImage run inside (appimagetool) unpacks itself instead of mounting.
ENV APPIMAGE_EXTRACT_AND_RUN=1

WORKDIR /src
