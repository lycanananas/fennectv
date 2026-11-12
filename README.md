# FennecTV

Fast-scan, low-bandwidth television over the
[Data2G](https://github.com/arodland/Data2G) HF modem, by SQ5FOX.

One way, from one station to many, like DVB-S2: video only, VVC
(VVenC/VVdeC, the default) or AV1 (libaom) coded straight through the
codec libraries (FFmpeg only reads and scales the source), B frames on, a
few 3-second GOPs a Data2G burst, Data2G's own FEC and no repair. Every
byte of a burst goes to the picture: 160×120 at 5 fps in Z8k (7.3 kbps),
96×72 at 3 fps in Q3k (2.9 kbps). Runs on Linux and Windows.

Status: early work. Version in [VERSION](VERSION), changes in
[CHANGELOG.md](CHANGELOG.md), design and measurements in
[docs/PLAN.md](docs/PLAN.md).

## Use

Both programs talk to a running `data2g-host` (its command and KISS
ports, 8300 and 8100 by default):

```
fennectv-tx --source film.mp4 --call N0CALL          # send, Z8k, VVC
fennectv-tx --source film.mp4 --mode q3k --codec av1 # Q3k, AV1
fennectv-rx --play                                   # receive and show
```

`fennectv-tx --help` lists the options and the Data2G modes (FEC, speed,
the S/N each needs). Senders and receivers must share the protocol: the
broadcast group, `fennectv-tx --version`.

## Downloads

[Releases](https://github.com/lycanananas/fennectv/releases):

- **Linux:** `FennecTV-<version>-x86_64.AppImage` (below).
- **Windows:** `fennectv-<version>-win64.zip`, Windows 10 or newer,
  x86-64: unzip, then `fennectv-tx.exe --help` and `fennectv-rx.exe
  --play` in that folder.

## The Linux AppImage

One file, both programs, for any x86-64 distribution with glibc 2.39 or
newer (Ubuntu 24.04, Debian 13, Fedora 40, Arch...). Nothing to install.

Make it executable once:

```
chmod +x FennecTV-0.1.0-x86_64.AppImage
```

Then say which program to run, `tx` (send) or `rx` (receive); everything
after it goes to that program:

```
./FennecTV-0.1.0-x86_64.AppImage tx --source film.mp4 --call YOURCALL
./FennecTV-0.1.0-x86_64.AppImage rx --play
./FennecTV-0.1.0-x86_64.AppImage tx --help
./FennecTV-0.1.0-x86_64.AppImage                 # how to use it
```

Or let it put `fennectv-tx` and `fennectv-rx` in `~/.local/bin`, and use
them like any program:

```
./FennecTV-0.1.0-x86_64.AppImage --install
fennectv-tx --source film.mp4 --call YOURCALL
fennectv-rx --play
```

They are links to the AppImage file: after moving it (or downloading a
newer one), run `--install` again from there. `--uninstall` takes them
away. If `~/.local/bin` isn't on your `PATH`, `--install` says how to add
it.

The sender runs anywhere, servers without a desktop too. The receiver
needs a desktop's graphics libraries (EGL, X11 or Wayland), with or
without `--play`. Without FUSE (some containers):
`APPIMAGE_EXTRACT_AND_RUN=1 ./FennecTV-...`.

## Building

On Linux with the libraries installed (Arch: `pacman -S cmake ninja
qt6-base ffmpeg aom`, VVenC and VVdeC from the AUR, `vvenc` and `vvdec`):

```
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build -L unit
```

More, with the tests to run by hand before a release:
[docs/BUILD.md](docs/BUILD.md). The packages build in Docker, nothing
installed on the host:

```
packaging/windows/build     # dist/fennectv-<version>-win64.zip (quasi-msys2, MSYS2's libraries)
packaging/appimage/build    # dist/FennecTV-<version>-x86_64.AppImage (Ubuntu 24.04)
```

## Releasing

`packaging/release --check`, then `packaging/release`, then
`packaging/release --publish`: step by step in
[docs/RELEASING.md](docs/RELEASING.md).

## Licence

GPL-3.0-or-later. Code under `vendor/` keeps its own licence; the
packages carry the libraries they bundle, each under its own.
