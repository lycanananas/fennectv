#include "fennectv/budget.h"

#include <algorithm>
#include <cmath>

#include "fennectv/protocol.h"

namespace fennectv
{

namespace
{

ModeInfo make(const char* name, int hz, int bpc, int maxCw, double t1, double tmax)
{
    ModeInfo m;
    m.name = name;
    m.bandwidthHz = hz;
    m.bytesPerCodeword = bpc;
    m.maxCodewords = maxCw;
    m.secondsAtOne = t1;
    m.secondsAtMax = tmax;
    return m;
}

} // namespace

ModeInfo q3k() { return make("w48-16qam-r1/2", 2400, 236, 27, 1.05, 16.02); }
ModeInfo z8k() { return make("w48-256l-r5/8", 2400, 296, 54, 0.76, 16.02); }

std::optional<ModeInfo> knownGear(const std::string& name)
{
    for (const ModeInfo& m : {q3k(), z8k()})
        if (m.name == name) return m;
    return std::nullopt;
}

double slotSeconds(const ModeInfo& mode, int slots)
{
    if (mode.maxCodewords <= 1) return mode.secondsAtOne;
    double per = (mode.secondsAtMax - mode.secondsAtOne) / (mode.maxCodewords - 1);
    return mode.secondsAtOne + (slots - 1) * per;
}

int slotsFor(const ModeInfo& mode, double seconds)
{
    int n = 1;
    // MODES prints two decimals: a hundredth of slack, as the host's own sum would land
    while (n < mode.maxCodewords && slotSeconds(mode, n + 1) <= seconds + 0.01) n++;
    return n;
}

Framing framingFor(const ModeInfo& mode)
{
    Framing f;
    const int bpc = mode.bytesPerCodeword;
    for (int frames = 1; frames <= 8; frames++)
    {
        int n = (bpc - 1 - 2 * frames) / frames;
        if (n <= (int)FRAME_HEADER_BYTES + 2) break;
        if (n <= (int)MAX_FRAME_BYTES)
        {
            f.framesPerCodeword = frames;
            f.frameBytes = (size_t)n;
            f.payloadBytes = (size_t)n - FRAME_HEADER_BYTES;
            return f;
        }
    }
    return Framing{};
}

std::optional<BurstPlan> planBurst(const ModeInfo& mode, double seconds, bool dupControl)
{
    BurstPlan p;
    p.mode = mode;
    p.framing = framingFor(mode);
    if (p.framing.payloadBytes == 0) return std::nullopt;
    p.controlSlots = dupControl ? 2 : 1;
    p.slots = slotsFor(mode, seconds);
    p.dataCodewords = p.slots - p.controlSlots;
    if (p.dataCodewords < 1) return std::nullopt;
    p.frames = std::min(MAX_BURST_FRAMES, p.dataCodewords * p.framing.framesPerCodeword);
    p.airSeconds = slotSeconds(mode, p.slots);
    return p;
}

} // namespace fennectv
