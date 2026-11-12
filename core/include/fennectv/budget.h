// How much one burst carries in a Data2G mode (docs/PLAN.md, M0): slots
// within the burst length and the frames that fill each codeword. No
// parity of our own: Data2G's LDPC and CRC guard every codeword.
#ifndef FENNECTV_BUDGET_H
#define FENNECTV_BUDGET_H

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

#include "fennectv/protocol.h"
#include "ragtime/link/Data2GLink.h"

namespace fennectv
{

using ragtime::link::ModeInfo;

// data2g-host's longest size class: a port in its own mode never builds a
// longer burst (kisslink.BROADCAST_S).
inline constexpr double BROADCAST_SECONDS = 12.0;

// The two gears FennecTV is made for, as data2g-host 2400 Hz lists them
// (MODES, checked 2026-11-11). The host's own list wins when connected.
ModeInfo q3k();  // w48-16qam-r1/2
ModeInfo z8k();  // w48-256l-r5/8
std::optional<ModeInfo> knownGear(const std::string& name);

// Slots (control included) a burst gets within `seconds`, as Data2G's
// policy.slots_for: the most n whose air time is within it, at least 1.
int slotsFor(const ModeInfo& mode, double seconds);

// Air time of a burst of n slots, linear between MODES' two points.
double slotSeconds(const ModeInfo& mode, int slots);

struct Framing
{
    int framesPerCodeword = 1;
    size_t frameBytes = 0;    // every frame this long
    size_t payloadBytes = 0;  // the frame less its header (a byte less where a GOP starts)
};

// The fewest frames to a codeword that keep each within MAX_FRAME_BYTES:
// f frames take 1 + sum(n + 2) bytes of a codeword (broadcast v3 COBS
// framing and the zero before a frame that starts a codeword).
Framing framingFor(const ModeInfo& mode);

struct BurstPlan
{
    ModeInfo mode;
    Framing framing;
    int slots = 0;          // the burst, control included
    int controlSlots = 1;   // 2 with DUP
    int dataCodewords = 0;
    int frames = 0;
    double airSeconds = 0;

    // What the burst carries with `gops` GOPs in it.
    size_t capacityBytes(int gops) const { return burstCapacity(framing.payloadBytes, frames, gops); }
    // The codeword a frame travels in, counted from the first data codeword.
    int codewordOf(int idx) const { return idx / framing.framesPerCodeword; }
};

// A burst made of its GOPs' codings. Each GOP has a few to choose from
// (bytes, pictures left out); one each is picked: the fewest pictures left
// out, then the most bytes, within capacity. Which, by GOP; empty if
// nothing fits.
struct Coding
{
    size_t bytes = 0;
    int dropped = 0;
};
std::vector<size_t> bestFill(const std::vector<std::vector<Coding>>& gops, size_t capacity);

// Nothing for a mode with no room.
std::optional<BurstPlan> planBurst(const ModeInfo& mode, double seconds = BROADCAST_SECONDS, bool dupControl = false);

} // namespace fennectv

#endif
