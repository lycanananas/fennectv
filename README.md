# FennecTV

Fast-scan, low-bandwidth television over the
[Data2G](https://github.com/arodland/Data2G) HF modem, by SQ5FOX.

One way, from one station to many, like DVB-S2: video only, AV1 (libaom)
or VVC (VVenC/VVdeC) coded straight through the codec libraries (FFmpeg
only reads and scales the source), B frames on, a few 3-second GOPs a
Data2G burst, Data2G's own FEC and no repair. Runs on
Linux and Windows (MSYS2).

Status: early work. See [docs/PLAN.md](docs/PLAN.md).

Licence: GPL-3.0-or-later. Code under `vendor/` keeps its own licence.
