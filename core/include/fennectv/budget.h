// How much one burst carries in a Data2G mode (docs/PLAN.md, M0): slots
// within the burst length and the frames that fill each codeword. No
// parity of our own: Data2G's LDPC and CRC guard every codeword.
#ifndef FENNECTV_BUDGET_H
#define FENNECTV_BUDGET_H

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

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
    size_t payloadBytes = 0;  // the frame less its header
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

    size_t capacityBytes() const { return (size_t)frames * framing.payloadBytes; }
    // The codeword a frame travels in, counted from the first data codeword.
    int codewordOf(int idx) const { return idx / framing.framesPerCodeword; }
};

// Nothing for a mode with no room.
std::optional<BurstPlan> planBurst(const ModeInfo& mode, double seconds = BROADCAST_SECONDS, bool dupControl = false);

} // namespace fennectv

#endif
