# Windows builds of FennecTV from Linux, in a container: quasi-msys2
# (MSYS2's own UCRT64 packages, unpacked) and the native Clang cross-
# compiling against them. Nothing is installed on the host.
#
#   packaging/windows/build        (builds this image, then FennecTV)
#
# Layers from the least to the most often changed, so Docker's cache keeps
# the big downloads: tools, MSYS2 packages, VVenC/VVdeC.
FROM archlinux:base

RUN pacman -Syu --noconfirm --needed \
        make wget tar zstd gawk gnupg which git \
        llvm clang lld cmake ninja pkgconf ccache \
    && pacman -Scc --noconfirm

# quasi-msys2 at a known commit; UCRT64 (the default) is what MSYS2 users run.
ARG QUASI_MSYS2_COMMIT=00a2b1645c1e7a21e4c8d975976c8d9808bb135c
RUN git clone https://github.com/holyblackcat/quasi-msys2 /opt/quasi-msys2 \
    && git -C /opt/quasi-msys2 checkout --quiet "$QUASI_MSYS2_COMMIT"

# The libraries FennecTV links, from MSYS2's repository, prebuilt.
# SDL2 (pulled in by FFmpeg) and sdl2-compat ship the same SDL2.dll, and
# quasi-msys2 would take both: the SDL2 alias goes to sdl2-compat alone.
WORKDIR /opt/quasi-msys2
RUN echo 'mingw-w64-ucrt-x86_64-SDL2:mingw-w64-ucrt-x86_64-sdl2-compat' >alternatives.txt \
    && make install _gcc _qt6-base _ffmpeg _aom _ntldd \
    && rm -rf cache

# VVenC and VVdeC: no MSYS2 package, so built here, into the same prefix.
COPY build-vvc.sh /opt/build-vvc.sh
RUN bash -c 'source env/all.src && bash /opt/build-vvc.sh'

WORKDIR /src
