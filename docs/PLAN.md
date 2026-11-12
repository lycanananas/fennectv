# FennecTV plan

FennecTV is fast-scan, low-bandwidth television over the
[Data2G](https://github.com/arodland/Data2G) HF modem, by SQ5FOX
(Lycanananas). It sends video only (no audio and no container on the air)
one way, from one station to many, the way DVB-S2 does in CCM: the
operator picks the mode, and receivers never answer.

Status: plan approved 2026-11-11, M0 and M1 done.

## Fixed assumptions

- **One way, point to multipoint.** No repair, no receiver reports, no
  feedback of any kind. What Data2G's FEC can't correct is lost. This is
  the line between live (FennecTV) and file transfer (Ragtime).
- **1 Data2G burst = 1 packet = a few closed GOPs** of `--gop` seconds
  (3 by default), each from a keyframe. No FEC of our own: each codeword
  has Data2G's QC-LDPC and CRC (decided 2026-11-12).
- **Gears:** Q3k (`w48-16qam-r1/2`) and Z8k (`w48-256l-r5/8`), from
  Data2G's `mode_thresholds.json`. Other modes may work, but they are
  not a goal.
- **Sources:** a file first, then a webcam, and any URL FFmpeg opens,
  with UDP multicast (MPEG-TS from an IPTV head end) named and tested.
- **Platforms:** Arch Linux and Windows (MSYS2 UCRT64). Both build from
  pacman packages.
- **Licence:** GPL-3.0-or-later. FFmpeg built with x264 and x265 is GPL,
  and the code vendored from Ragtime is MIT, which GPL takes in.
- **GUI:** Qt 6 (below).
- **Shares the host with Ragtime.** Each opens its own group on the same
  data2g-host (M0, below).
- **Polish rules.** No CW ID and no band plan segments are required, so
  FennecTV adds neither. ID stays possible through the burst's group+from
  TLV (`BCAST OPEN FENNECTV4 FROM call`), which costs nothing.

## Why Qt 6 rather than wxWidgets

- QtNetwork gives the TCP sockets to data2g-host on both platforms, for
  the CLI tools (QCoreApplication) and the GUI alike, so there is no
  POSIX/Winsock layer of our own to write.
- Video in a widget, worker threads and queued signals between capture,
  encoder, link and display are what Qt does every day.
- data2g-gui is Qt too, so an operator who builds Data2G on MSYS2
  already has the toolchain: `mingw-w64-ucrt-x86_64-qt6-base`.
- The core library uses neither Qt nor FFmpeg, so it is tested on its
  own.

## M0: what data2g-host does (checked 2026-11-11)

Read from Data2G's `docs/broadcast.md`, `data2g/kisslink.py`,
`data2g/arq/policy.py` and the native host's `app/tcp_servers.cpp`.

1. **Two clients at once: yes.** The native host's command port is a
   `Port` with `multi = true`, and the KISS server takes any number of
   clients, frames going to all of them (`station.cpp`, `tcp_servers.cpp`).
   Ports opened by a client close when that client goes. Statuses go to
   the command clients that sent a BCAST command.
2. **Group name.** Up to 10 characters from A-Z 0-9 / - and space, and
   never a valid callsign. `FENNECTV4` (9, no letter after the digit) fits;
   its last digit is the protocol version.
3. **How a burst is built** (`KissLink.next_burst`). It takes the first
   queued frame's port and mode, then every queued frame of that port
   that fits, in queue order, within the port's size class. A port in
   its own fixed mode uses the longest class, `BROADCAST_S = 12.0` s:
   - `w48-16qam-r1/2`: 20 slots, 11.99 s (6 s: 9 slots);
   - `w48-256l-r5/8`: 40 slots, 11.99 s (6 s: 19 slots).
   One slot is the control codeword (two with DUP), the rest data. A
   burst carries only the codewords its frames need, so shorter bursts
   come from queueing fewer frames.
4. **Frame framing (v3).** Each frame is COBS encoded and ends with a
   zero: `n + 2` bytes up to 253, plus a zero before any frame that
   starts a codeword. A frame filling k codewords is
   `k × bytes-per-codeword − 3` bytes (at most 253).
   - Q3k: 236 B codewords, so one 233 B frame per codeword.
   - Z8k: 296 B codewords, more than the 255 B limit, so two frames per
     codeword: 146 + 145 B (`3 + n1 + 2 + n2 = 296`). Frames are all
     145 B and the first carries one byte of spare room, so a lost
     codeword is exactly two lost frames.
5. **Knowing when frames went out.** KISS ACKMODE (`0x0C`, a 2-byte tag):
   the host acks when the burst carrying the frame *finishes*. `PTT ON`
   on the command port says a burst has *started*. Frames too big for
   the mode are dropped, with `BCAST n DROPPED k`.
6. **Channel access.** KISS P (default 63) and SLOTTIME (1 s) apply only
   after BUSY. Broadcast waits while an ARQ session is under way, so a
   Ragtime ragchew on the same host pauses the stream.
7. **Control lost.** Receivers look for data in slots 1-8 under the
   open ports' keys, so a burst whose control was lost still arrives.
   `BCAST MODE n DUP mode` doubles the control for about 3 dB, at one
   codeword a burst. It is a preset option, off by default.

Left for M4, with two hosts on the bench: the real gap between back to
back bursts (PTT delays are 100 + 50 ms by default).

## Repository

```
fennectv/
  CMakeLists.txt        C++17; FFmpeg via pkg-config; Qt 6 (Core, Network, Widgets)
  VERSION               the version, one line: CMake, the packages and releases read it
  CHANGELOG.md          a section a version: the GitHub release notes
  LICENSE               GPL-3.0
  vendor/
    README.md           where each folder came from, the commit, its licence, what changed
    ragtime/            Data2GLink (KISS, MODES, framing), its test and Check.h;
                        LICENSE (MIT); ragtime 6b898d9
  core/                 libfennectv: no Qt, no FFmpeg, all unit tested
    protocol/           frame header, GOPs in a burst, the receiver's parser
    budget/             burst capacity from MODES, the best mix of GOP codings
  codec/                VVC (VVenC, VVdeC) and AV1 (libaom): encoders fit to a burst, decoders
  media/                FFmpeg: sources, scaling, frame rate, pixel format
  link/                 Qt: data2g-host command and KISS clients (BCAST OPEN/MODE, ACKMODE)
  apps/tx, apps/rx      CLI: fennectv-tx, fennectv-rx
  gui/                  fennectv (M4)
  tests/
  packaging/
    windows/            the zip: Docker, quasi-msys2 (MSYS2 UCRT64 libraries), Clang
    appimage/           the AppImage: Docker, Ubuntu 24.04, linuxdeploy
    linux/              .desktop entry and icon (installed by CMake)
    release             tests, both packages, source, SHA256SUMS; --publish to GitHub
  docs/PLAN.md          this
  docs/BUILD.md         building, the tests, the manual checks before a release
  docs/RELEASING.md     how to make a release
```

Ragtime's `HostLink` is left out: most of it is sessions. FennecTV needs
only the broadcast commands, a small class of its own in `link/`.

## On the air

Frames go to the KISS port of group `FENNECTV4`, opened with
`BCAST OPEN FENNECTV4 FROM <call>` and `BCAST MODE n <mode>` (never AUTO).
The group's last digit is the protocol version: Data2G sends the name in
the control codeword anyway and masks every CRC with it, so a receiver
of another version never even gets the frames, and no byte of the burst
says it.

Every byte counts: at 7 kbps a byte a frame is 1% of the picture.
**Each KISS frame** has a 2-byte header, 3 where a GOP starts:

```
[codec 2 bits | start 1 | burst 5][last 1 | idx 7]([GOP start 1])[payload]
```

- `codec` 1 AV1, 3 VVC. `burst` numbers the bursts mod 32, `idx` the
  frame in it, `last` marks the burst's last frame so a receiver needn't
  wait for quiet.
- `start`: a GOP begins in this frame, and the `GOP start` byte says
  where in the payload (the first one). After a lost frame the receiver
  picks up there.
- Frames are all the mode's length: the payload is 231 B a frame on Q3k
  (19 frames), 143 B on Z8k (78 frames), a byte less where a GOP starts:
  4385 and 11150 B a burst with 4 GOPs.

**The burst** (the payloads end to end):

```
GOP:  [units][first picture][fps][burst pictures][OSD length][OSD]
      units x unit, decode order, one a picture, each ending itself:
        AV1: [payload length << 3 | kind << 1 | more, LEB128]([OBU
             header])[payload]... for the temporal unit's OBUs but the
             delimiter: kind frame, frame header or sequence header (the
             OBU header implied), or other (it follows); more clear on
             the last
        VVC: [length << 1 | more, LEB128][NAL unit less its first byte]...,
             more clear on the last; no start codes, SEI or AUD
...   more GOPs
[zeros to the end]
```

- Protocol version 4 (2026-11-12). The codec is in every frame header.
- `units` is never 0, so a zero after a GOP is the padding. The OSD goes
  in the burst's first GOP only; the receiver keeps the last one.
- The first byte of a VVC NAL unit (forbidden and reserved bits, layer
  id) is 0 in one layer, which is all VVenC codes; the receiver puts it
  back, and AV1's OBU headers and sizes, before decoding. Neither form
  is ever longer than the codec's own, and often a byte a unit shorter.
- The sequence's parameters (AV1 sequence header, VVC SPS and PPS; 12
  and ~150 B) go in the burst's first GOP only, and never twice in a
  GOP: they don't change while the transmission lasts. The receiver
  keeps the last ones; a GOP that came without them gets those. Only a
  receiver that has just started and lost the burst's first GOP waits a
  burst for a picture.
- Every GOP starts with a random access point: an AV1 key frame, a VVC
  IDR; with the sequence's parameters (below) a decoder starts there.
  Width, height and colour come from those. No frame table, no CRC, no unit sizes: the units end
  themselves and Data2G checks every codeword.
- With B frames units are in decode order; the decoder gives pictures
  back with their display order, and the receiver puts each in its place.
- **A lost frame** ends the GOP it falls in: its pictures up to the hole
  play, and the receiver resumes at the next GOP start a frame points
  to. A GOP whose header and keyframe arrived always shows.

## Sending

1. **Source** (libavformat/libavdevice): a file (paced to the air: one
   GOP's worth of frames is read per burst, `--loop` to repeat), a webcam
   (v4l2 on Linux, dshow on Windows), or any FFmpeg URL: UDP multicast
   (`udp://@239.1.1.1:1234?overrun_nonfatal=1&fifo_size=...`), RTP, SRT,
   HTTP. `--program` / `--stream` pick one from an MPTS. Live sources run
   at their own pace and the GOP buffer drops what it can't take.
2. **Filters** (libavfilter): `fps`, `scale`, `format=gray`, optionally
   `hqdn3d`. Denoising pays at these bit rates.
3. **GOP buffer:** the frames of one burst period (air time plus the gap,
   measured from `PTT ON/OFF`), so the stream keeps up with real time.
4. **Encoder** (`--codec`), straight through its library, one encoder a
   codec on purpose (decoders agree, encoders don't), each GOP on its own:
   - VVC, VVenC (the default): preset slow, no perceptual QP adaptation,
     two-pass rate control, 10 bits inside, gray or 4:2:0 only (VVenC
     codes nothing else); B frames as random access with a closed GOP
     (IDR first, hierarchical groups of 16). No HRD and no VUI in the
     SPS, a fixed level: the SPS is the same whatever the bit rate, so it
     can go once a burst.
   - AV1, libaom: good quality, cpu-used 2, a constant quantizer (cq-level)
     found per GOP instead of libaom's rate control, monochrome for gray;
     B frames as alt-refs (lag over the GOP).
   - `--preset` and `--param KEY=VALUE` reach either library directly.
   - `--b-frames off`: forward references only (AV1 no alt-ref, VVC low
     delay).
   The burst's GOPs are coded side by side on `--jobs` threads.
5. **Rate fit, no coding thrown away:** each GOP gets the burst's room
   by its share of pictures and is coded again (up to `--tries`) until
   it lands within `--fill` (100%) less `--tolerance` (4%). The first
   try is aimed by what that GOP's place in the bursts before learned
   (bytes made for bytes asked), the next ones by a secant through the
   tries so far. Every try is kept: the burst is then made of the mix
   of the GOPs' codings that fills it best (`bestFill`), so one GOP's
   too big a try takes the room another left. A GOP that still can't
   fit gets what the others left, then loses its last pictures.
   Measured: bursts 95.5% full before, 99.6% after, and faster (fewer
   tries).
6. **Pack, queue:** burst N+1 is queued when `PTT ON` says burst N has
   started, so bursts run back to back. ACKMODE tags confirm each burst
   went out.

## Receiving

KISS client and `BCAST OPEN FENNECTV4`. Frames are collected by burst
number; a burst is done at its last frame, a new burst number, or a
second of quiet. libaom or VVdeC decodes every GOP that arrived, each
from its keyframe, as far as its units go. Playback keeps the stream's pace a burst behind (3 s preroll):
each picture has its place in the burst, and a lost one shows the
picture before it. Nothing for a while: NO SIGNAL.

**Quality, worked out locally** (as MER in DVB): frames and pictures
received per burst, GOPs whole or cut, and the S/N from `BCAST n HEARD`. The received stream
can be recorded locally (`.ivf` or `.mkv`).

## Presets

| Mode | Picture (defaults) | Burst |
|---|---|---|
| Z8k `w48-256l-r5/8` | 160×120 gray, 5 fps, GOP 3 s | 11150 B / 12.3 s, 7.3 kbps |
| Q3k `w48-16qam-r1/2` | 96×72 gray, 3 fps, GOP 3 s | 4385 B / 12.3 s, 2.9 kbps |

Measured 2026-11-12 on the test film, protocol 4, the transmitter's own
path (bursts of 4 GOPs, decoded back, 6 bursts from 3 places; PSNR and
SSIM of the gray picture, full range):

| | Z8k | Q3k | time a burst (Z8k) |
|---|---|---|---|
| VVC slow, no QPA, B (default) | 28.86 dB, 0.904 | 26.4-27.3, 0.903 | 6 s |
| VVC slower | 28.94, 0.904 | 27.56, 0.908 | 23 s |
| VVC medium | 28.66, 0.900 | 27.32, 0.903 | 2.5 s |
| VVC low delay (no B) | 28.59, 0.897 | 26.57, 0.885 | 6 s |
| AV1 cpu-used 2, cq, B | 27.81, 0.882 | 26.10, 0.875 | 3 s |
| AV1 no B | 26.91, 0.856 | 25.46, 0.855 | 3 s |
| AV1 two-pass VBR (before) | 25.57, 0.823 | 24.31, 0.820 | 8 s |

Bursts 98-100% full, 61/61 and 37/37 pictures. VVenC's perceptual QP
adaptation cost 0.9 dB (and SSIM) at these sizes; libaom's cpu-used 0
gains nothing over 2. Protocol 4 against 3: 152 B more a Z8k burst from
the frame headers, ~400 B from VVC's SPS going once a burst (and 22 B
less of it), ~30 B from AV1's OBU headers, 18 B of OSD. Q3k at 128×96
needs `--gop 6`. GOPs of 1 s don't work at these rates: the keyframes
alone take 85% of a Z8k burst.

Measured 2026-11-11 on 12 s of 160×120 gray at 10 fps (FFmpeg 9.0):
libaom-av1 5.1 kbps SSIM 0.891; libsvtav1 5.1 kbps 0.877; libx264 5.75
kbps 0.886; libaom realtime 4.7 kbps 0.803; libx265 no lower than ~9
kbps. An AV1 keyframe is ~850 B.

## Builds

The version is `VERSION`; `fennectv-tx --version` prints it with the
broadcast group (the protocol).

- **Development (Arch):** `pacman -S cmake ninja qt6-base ffmpeg aom`,
  VVenC and VVdeC from the AUR. `ctest -L unit` for the unit tests,
  `ctest -L media` for the media test (the test film beside the checkout,
  minutes).
- **Windows zip** (`packaging/windows/build`): cross-compiled from Linux
  in Docker, nothing on the host. quasi-msys2 unpacks MSYS2's UCRT64
  packages (Qt 6, FFmpeg, libaom) and the native Clang builds against
  them; VVenC and VVdeC are built in the image, static. The zip holds the
  programs, Qt's Windows platform and style plugins, and every DLL they
  load from the MSYS2 tree (found with `llvm-objdump`). A UTF-8 manifest
  for file names. Size doesn't matter: ~100 MB, most of it FFmpeg's
  dependencies. Starts under Wine (checked 2026-11-12).
- **Linux AppImage** (`packaging/appimage/build`): in Docker on Ubuntu
  24.04, so it runs on glibc 2.39 and newer; Ubuntu's Qt 6 and libaom,
  FFmpeg 7.1, VVenC and VVdeC built in the image, linuxdeploy and its Qt
  plugin bundle the rest (xcb and Wayland platforms). FFmpeg is our own
  because Ubuntu's pulls in X11, GL, ALSA, JACK, libusb, which an
  AppImage leaves to the system: ours has FFmpeg's native demuxers,
  decoders, filters and protocols, v4l2 and dav1d, nothing autodetected.
  So fennectv-tx needs nothing but glibc and zlib (a server will do);
  fennectv-rx --play needs what every desktop has (EGL/GL, X11 or
  Wayland, fontconfig, HarfBuzz). 51 MB. One AppImage, two programs:
  `FennecTV.AppImage tx|rx ...`, or linked as `fennectv-tx` and
  `fennectv-rx`. Checked 2026-11-12 on bare Debian 13, Fedora 41 and
  Ubuntu 24.04 (fennectv-tx, both encoders) and on Debian 13 with Xvfb
  (fennectv-rx --play).
- **Release** (`packaging/release`): the unit tests on the host, both
  packages, `git archive` of the commit and `SHA256SUMS` into
  `dist/release/<version>/`; `--publish` tags `v<version>`, pushes the
  tag and makes the GitHub release with `gh`, the notes from
  `CHANGELOG.md`. It refuses uncommitted changes, an existing tag, a
  commit not on GitHub, a version without a changelog section.
- Later: a `fennectv-git` PKGBUILD beside `arch/ragtime-git`.

## Milestones

- **M0, what the host does.** Done (above).
- **M1, skeleton.** Done 2026-11-11: CMake, `vendor/` with its README, LICENSE. Done when
  the vendored Data2GLink tests pass.
- **M2, libfennectv core.** Protocol, GOPs packed in a burst, budget from
  MODES, the codeword channel model (below). Done 2026-11-12: every GOP
  a lost frame doesn't touch comes back whole, the cut one up to the hole.
- **M3, media.** FFmpeg sources (file, webcam, UDP multicast), filters,
  encoders with the rate fit, decoder. Done when picture and budget match
  the measurements above. UDP multicast checked with an MPEG-TS sent by
  `ffmpeg -re -i x.mkv -f mpegts udp://239.1.1.1:1234`.
- **M4, GUI.** Sender and receiver windows over data2g-host.
- **M5, with the host.** The link to data2g-host and the two-host bench.
  Done when 10 minutes stream with no gaps between bursts, about two
  burst periods glass to glass. Burst gap measured.
- **M6, on the air.** Q3k and Z8k on a real band.
- **Packaging.** Windows zip, AppImage and the release script done
  2026-11-12; a PKGBUILD once M5 passes.

## Testing without a radio

No loss model of our own: Data2G is the link layer, its FEC and its
losses are its business. Tests run on real data2g-host instances, on
Linux and Windows.

**Two real hosts, a clean channel.** Two `data2g-host`s joined
by fifos instead of sound cards; FennecTV TX on one, RX on the other.
Real bursts, framing, timing, PTT and ACKMODE. Checked 2026-11-11 with
data2g-host from the Arch package:

```
mkfifo ab ba
data2g-host --mycall N0CALL --audio-io pipe:ba,ab &                                # TX: 8300 / 8100
data2g-host --mycall N1CALL --command-port 8310 --kiss-port 8110 --audio-io pipe:ab,ba &  # RX
```

The audio pipes have no Windows fifo; there the bench uses local TCP
sockets or files, decided in M5.
