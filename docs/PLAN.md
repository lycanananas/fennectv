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
  TLV (`BCAST OPEN FENNECTV FROM call`), which costs nothing.

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
   never a valid callsign. `FENNECTV` (8, no digit) fits.
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
  LICENSE               GPL-3.0
  vendor/
    README.md           where each folder came from, the commit, its licence, what changed
    ragtime/            Data2GLink (KISS, MODES, framing), its test and Check.h;
                        LICENSE (MIT); ragtime 6b898d9
  core/                 libfennectv: no Qt, no FFmpeg, all unit tested
    protocol/           frame header, GOPs in a burst, the receiver's parser
    budget/             burst capacity from MODES: frames, their size
    sim/                loss models (iid, Gilbert-Elliott fading)
  codec/                AV1 (libaom) and VVC (VVenC, VVdeC): encoders fit to a burst, decoders
  media/                FFmpeg: sources, scaling, frame rate, pixel format
  link/                 Qt: data2g-host command and KISS clients (BCAST OPEN/MODE, ACKMODE)
  apps/tx, apps/rx      CLI: fennectv-tx, fennectv-rx
  gui/                  fennectv (M5)
  tests/
  packaging/            PKGBUILD (beside arch/ragtime-git), Windows zip script
  docs/PLAN.md, docs/PROTOCOL.md
```

Ragtime's `HostLink` is left out: most of it is sessions. FennecTV needs
only the broadcast commands, a small class of its own in `link/`.

## On the air

Frames go to the KISS port of group `FENNECTV`, opened with
`BCAST OPEN FENNECTV FROM <call>` and `BCAST MODE n <mode>` (never AUTO).

**Each KISS frame** has a 4-byte header:

```
[version 4 bits | codec 4][burst 1][idx 7 bits | last 1][GOP start 1][payload]
```

- `burst` numbers the bursts (wraps), `idx` the frame in it, `last`
  marks the burst's last frame so a receiver needn't wait for quiet.
- `GOP start`: where in this payload the first GOP header begins, 0xFF
  for none. After a lost frame the receiver picks up there.
- Payload: 229 B a frame on Q3k (19 frames, 4351 B a burst), 141 B on
  Z8k (78 frames, 10998 B).

**The burst** (the payloads end to end):

```
GOP:  [0x80][fps][first picture][units][burst pictures][OSD length][OSD]
      units x [size, LEB128][unit], decode order, one a picture:
        AV1: the temporal unit's OBUs, no temporal delimiter
        VVC: [size, LEB128][NAL unit]..., no start codes, SEI or AUD
...   more GOPs
[zeros to the end]
```

- Protocol version 3 (2026-11-12). The codec is in every frame header.
- Every GOP starts with a random access point: an AV1 key frame with the
  sequence header, a VVC IDR with SPS and PPS. Width, height and colour
  come from those. No frame table, no CRC: the sizes delimit the units
  and Data2G checks every codeword.
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
   - AV1, libaom: good quality, cpu-used 0, two-pass, monochrome for gray;
     B frames as alt-refs (lag over the GOP).
   - VVC, VVenC: preset slower, two-pass rate control, 10 bits inside,
     gray or 4:2:0 only (VVenC codes nothing else); B frames as random
     access with a closed GOP (IDR first, hierarchical groups of 16).
   - `--b-frames off`: forward references only (AV1 no alt-ref, VVC low
     delay).
   The burst's GOPs are coded side by side on `--jobs` threads.
5. **Rate fit:** each GOP gets the burst's room by its share of pictures
   and is coded again until it lands within `--fill` (95%) less
   `--tolerance`. One that can't fit gets what the others left, then
   loses its last pictures.
6. **Pack, queue:** burst N+1 is queued when `PTT ON` says burst N has
   started, so bursts run back to back. ACKMODE tags confirm each burst
   went out.

## Receiving

KISS client and `BCAST OPEN FENNECTV`. Frames are collected by burst
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
| Z8k `w48-256l-r5/8` | 160×120 gray, 5 fps, GOP 3 s | 10998 B / 12.3 s, 7.2 kbps |
| Q3k `w48-16qam-r1/2` | 96×72 gray, 3 fps, GOP 3 s | 4351 B / 12.3 s, 2.8 kbps |

Measured 2026-11-12 on the test film: Z8k bursts 94-98% full, 61/61
pictures, coded in 5-10 s; Q3k 97-100%, 37/37, 2-5 s. Q3k at 128×96
needs `--gop 6`. GOPs of 1 s don't work at these rates: the keyframes
alone take 85% of a Z8k burst.

Measured 2026-11-11 on 12 s of 160×120 gray at 10 fps (FFmpeg 9.0):
libaom-av1 5.1 kbps SSIM 0.891; libsvtav1 5.1 kbps 0.877; libx264 5.75
kbps 0.886; libaom realtime 4.7 kbps 0.803; libx265 no lower than ~9
kbps. An AV1 keyframe is ~850 B.

## Builds

- **Arch:** `pacman -S cmake ninja qt6-base ffmpeg`. A `fennectv-git`
  PKGBUILD beside `arch/ragtime-git` and `aur/`.
- **Windows (MSYS2 UCRT64):** `pacman -S mingw-w64-ucrt-x86_64-{toolchain,cmake,ninja,qt6-base,ffmpeg}`.
  The zip is made with `windeployqt` plus the FFmpeg DLLs found with
  `ldd`.

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
- **Packaging.** PKGBUILD and the Windows zip, once M5 passes.

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
