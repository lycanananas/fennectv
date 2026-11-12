# Building and testing FennecTV

How to build FennecTV from source, the packages, and the manual tests
that come before a release (the checklist at the end).

## 1. Build on Linux (development)

Arch:

```
sudo pacman -S --needed base-devel cmake ninja pkgconf qt6-base ffmpeg aom
# VVenC and VVdeC: from the AUR (vvenc, vvdec), version 1.14 / 3.2 or newer
# data2g-host: from the Data2G package, for the tests with the host
```

Other distributions: the same libraries with their -dev packages (Qt 6
Core/Network/Widgets, FFmpeg's libavformat, libavcodec, libavdevice,
libavfilter, libswscale, libavutil, libaom, libvvenc, libvvdec), CMake
3.20+, a C++17 compiler.

```
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

Gives `build/fennectv-tx` and `build/fennectv-rx`.

```
build/fennectv-tx --version    # fennectv-tx 0.1.0, broadcast group FENNECTV4
```

## 2. Automatic tests

```
ctest --test-dir build -L unit             # seconds: protocol, burst budget, Data2G link
ctest --test-dir build -L media            # minutes: real encoders and decoders
```

The media test needs a film: the first `.mp4` in `../video/` beside the
checkout, or `-DFENNECTV_TEST_VIDEO=/path/film.mp4` when configuring.
Without one it is skipped. It codes two bursts in each mode with each
codec, B frames on and off, packs them, loses a frame and decodes what
arrives. It must end with `0 failed`.

## 3. The packages (Docker, nothing installed on the host)

```
packaging/appimage/build    # dist/FennecTV-<version>-x86_64.AppImage
packaging/windows/build     # dist/fennectv-<version>-win64.zip
```

The first build of each Docker image takes a while (it compiles VVenC,
VVdeC and, for the AppImage, FFmpeg). After that it takes minutes. To
throw it all away:

```
docker image rm fennectv-appimage fennectv-win
rm -rf build-appimage build-win dist
```

All of it at once, as a release would (tests, both packages, source,
checksums into `dist/release/<version>/`, nothing published):

```
packaging/release --allow-dirty    # from uncommitted changes
packaging/release                  # from a clean commit
```

## 4. Manual tests before a release

Each step says what should happen. Use your own callsigns in
`bench/local.env` (copy `bench/local.env.example`; the file is not in git).

### 4.1 Two hosts on one machine

In terminal 1, two `data2g-host`s joined by pipes instead of sound cards:

```
bench/start-data2g
```

The sender's host listens on 8300/8100, the receiver's on 8310/8110.
Ctrl-C stops both.

### 4.2 Z8k with VVC (the defaults)

Terminal 2, the receiver:

```
build/fennectv-rx --cmd 8310 --kiss 8110 --play
```

A window shows "waiting for FENNECTV4...".

Terminal 3, the sender (any film; `--loop` repeats it):

```
build/fennectv-tx --source ../video/film.mp4 --start 300 --call YOURCALL
```

Expected:

- **Sender:** two lines a burst, about every 12 s:
  - `burst N: 61/61 pictures in 4 GOPs`, 98-100% full, `coded in` well
    under 12 s; no `late`, no `dropped to fit` (once in a while on a very
    hard scene is fine);
  - `burst N on the air: 78 frames, ... B of picture; ... kbps` when the
    host has sent it, about 7.26 kbps on Z8k (2.85 on Q3k).

  At the end: `N bursts queued, N on the air`.

  Data2G's own window doesn't count broadcast under "Sent" (that's for
  ARQ connections); with its **Monitor** box ticked, the monitor window
  shows each burst: `TX w48-256l-r5/8 x40`, `BCAST "FENNECTV4" from
  YOURCALL: 78 frame(s)`.
- **Receiver:** `burst N: frames 78/78, 61/61 pictures`, the GOPs whole
  (`15/15 15/15 15/15 16/16`).
- **Window:** the film plays smoothly at 5 fps, about two bursts (~25 s)
  behind the sender, with the callsign in the corner. No freezes between
  bursts.

Stop the sender with Ctrl-C. After a while the window shows NO SIGNAL.

### 4.3 The other combinations

Repeat 4.2 with each of these on the sender (the receiver needs no
options: it follows the stream):

| Sender options | Expected |
|---|---|
| `--codec av1` | as above; the picture slightly softer |
| `--mode q3k` | 96×72 at 3 fps; `37/37 pictures`, `frames 19/19` |
| `--mode q3k --codec av1` | the same with AV1 |
| `--b-frames off` | as above; the picture a little worse |
| `--color yuv420` | in colour, at the same bit rate (so a little softer) |

### 4.4 A receiver that joins late

Start the sender first, wait for two bursts, then start the receiver. It
should show a picture from the first whole burst it hears, and the first
picture at most a burst later.

### 4.5 Headless receive

```
build/fennectv-rx --cmd 8310 --kiss 8110 --out received.obu
```

It runs without a display, prints the bursts, and writes the stream.

### 4.6 Wrong options are caught at once

```
build/fennectv-tx --source x --param nope=1        # vvenc: can't set nope=1
build/fennectv-tx --source x --codec vvc --preset ultra   # vvenc: no preset ultra
build/fennectv-tx --source x --cmd 9               # can't reach data2g-host
```

### 4.7 The AppImage

```
chmod +x dist/FennecTV-*-x86_64.AppImage
dist/FennecTV-*-x86_64.AppImage --version
dist/FennecTV-*-x86_64.AppImage tx --help
```

Then 4.2 again with the AppImage in place of `build/`:

```
dist/FennecTV-*-x86_64.AppImage rx --cmd 8310 --kiss 8110 --play
dist/FennecTV-*-x86_64.AppImage tx --source ../video/film.mp4 --start 300
```

Expected: the same as 4.2. The window opens on X11 and on Wayland.

The links:

```
dist/FennecTV-*-x86_64.AppImage                # usage
dist/FennecTV-*-x86_64.AppImage --install      # ~/.local/bin/fennectv-tx, fennectv-rx
fennectv-tx --version && fennectv-rx --version
dist/FennecTV-*-x86_64.AppImage --uninstall
```

If you can, run it on another distribution too (a laptop, a VM). To check
it in a throwaway container:

```
docker run --rm -v "$PWD/dist:/d:ro" -e APPIMAGE_EXTRACT_AND_RUN=1 ubuntu:24.04 \
    sh -c 'cp /d/FennecTV-*.AppImage /tmp/F && /tmp/F tx --source x --cmd 9'
```

That should print `can't reach data2g-host`: the program and both
encoders loaded.

### 4.8 The Windows zip

Best on a real Windows 10/11 machine with the Data2G host for Windows:
unzip it, then `fennectv-tx.exe --help`, then 4.2 with `fennectv-rx.exe
--play` and `fennectv-tx.exe`. File names with Polish letters should work.

Without Windows, a start under Wine in a container:

```
docker run --rm -v "$PWD/dist:/d:ro" archlinux:base bash -c '
    printf "[multilib]\nInclude = /etc/pacman.d/mirrorlist\n" >>/etc/pacman.conf
    pacman -Syu --noconfirm wine >/dev/null 2>&1; cd /tmp && bsdtar xf /d/fennectv-*-win64.zip
    WINEDEBUG=-all wine fennectv-*/fennectv-tx.exe --version'
```

### 4.9 Before publishing

- `dist/release/<version>/NOTES.md` reads right; it is the text on
  GitHub.
- `cd dist/release/<version> && sha256sum -c SHA256SUMS` passes.
- `packaging/release --check` is all ✓.

Then publish as in [RELEASING.md](RELEASING.md).
