# Vendored code

Code copied from other projects, unchanged unless this file says so.
Each folder keeps its own licence file. To update one, copy the files
again from the commit you want and change the commit below.

## ragtime/

- **From:** [Ragtime](https://github.com/baitisj/ragtime), Jeff Baitis
  (AG7EW), commit `6b898d9` (2026-11-08).
- **Licence:** MIT (`ragtime/LICENSE`), which GPL-3.0 takes in.
- **Files:**
  - `include/ragtime/link/Data2GLink.h`, `src/link/Data2GLink.cpp`: KISS
    framing with ACKMODE, the command port's lines, MODES parsing,
    broadcast v3 frame sizes (`framedBytes`, `burstDataBytes`).
  - `tests/Check.h`, `tests/Data2GLinkTest.cpp`: its tests.
- **Changes:** none. Built by FennecTV's own CMake as the static library
  `ragtime_vendored`, namespace `ragtime` kept.
