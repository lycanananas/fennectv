# Changelog

Each release's section is its notes on GitHub (`packaging/release`
reads the one for `VERSION`). Newest first.

## 0.1.0 - First public beta

FennecTV sends television one way over the Data2G HF modem: a picture
from a file, a camera or a network stream goes out in Data2G bursts to
any number of receivers, protected by Data2G's own FEC, with no return
channel.

- `fennectv-tx` sends, `fennectv-rx` receives (`--play` to watch). Both
  are command line programs and work with a running `data2g-host`.
- Two Data2G modes:
  - **Z8k**: 160×120 gray at 5 fps in 7.3 kbps, from about 18.5 dB S/N
    in a quiet band;
  - **Q3k**: 96×72 gray at 3 fps in 2.9 kbps, from about 7.6 dB.

  Picture size, frame rate and colour can be changed.
- VVC (the default) or AV1, a few 3-second GOPs to a 12-second burst.
  A receiver can start at any GOP. A lost codeword costs only the rest of
  its GOP; the next one plays.
- Every byte of a burst goes to the picture, and each burst is filled to
  98-100%. A Z8k burst is coded in 4-6 s.
- Linux AppImage (x86-64 with glibc 2.39 or newer; the sender runs on a
  server too): one file, `./FennecTV-0.1.0-x86_64.AppImage tx ...` or
  `rx ...`, or `--install` for `fennectv-tx` and `fennectv-rx` in
  `~/.local/bin`. A Windows zip (Windows 10 or newer).

**Beta:** tested between two `data2g-host`s joined by pipes, not yet on a
real band. No GUI yet. The Windows build was checked under Wine.
