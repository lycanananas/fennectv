// Burst budgets against what data2g-host does (docs/PLAN.md, M0): slot
// counts as Data2G's policy.slots_for gives them, frames that fill a
// codeword exactly under broadcast v3 framing.
#include "Check.h"
#include "fennectv/budget.h"
#include "fennectv/protocol.h"
#include "ragtime/link/Data2GLink.h"

using namespace fennectv;

namespace
{

// The frames of one burst, as the sender makes them.
std::vector<int> burstFrames(const BurstPlan& p)
{
    return std::vector<int>((size_t)p.frames, (int)p.framing.frameBytes);
}

void q3kPlan()
{
    auto p = planBurst(q3k());
    CHECK(p.has_value());
    CHECK_EQ(p->slots, 20);  // policy.slots_for(w48-16qam-r1/2, 12.0) = 20
    CHECK_EQ(p->dataCodewords, 19);
    CHECK_EQ(p->framing.framesPerCodeword, 1);
    CHECK_EQ(p->framing.frameBytes, (size_t)233);
    CHECK_EQ(p->framing.payloadBytes, (size_t)231);
    CHECK_EQ(p->frames, 19);
    CHECK_EQ(p->capacityBytes(4), (size_t)19 * 231 - 4);  // a byte for each GOP start
    CHECK(p->airSeconds <= BROADCAST_SECONDS);
    CHECK(p->airSeconds > 11.5);
}

void z8kPlan()
{
    auto p = planBurst(z8k());
    CHECK(p.has_value());
    CHECK_EQ(p->slots, 40);  // policy.slots_for(w48-256l-r5/8, 12.0) = 40
    CHECK_EQ(p->dataCodewords, 39);
    CHECK_EQ(p->framing.framesPerCodeword, 2);  // a 293 B frame is over 255
    CHECK_EQ(p->framing.frameBytes, (size_t)145);
    CHECK_EQ(p->framing.payloadBytes, (size_t)143);
    CHECK_EQ(p->frames, 78);
    CHECK_EQ(p->capacityBytes(4), (size_t)11150);
    CHECK_EQ(p->codewordOf(0), 0);
    CHECK_EQ(p->codewordOf(1), 0);
    CHECK_EQ(p->codewordOf(77), 38);
}

// The frames fit the burst exactly as Ragtime's (and so the host's) count
// of v3 framing has it, and never more codewords than planned.
void framesFitTheBurst()
{
    for (const ModeInfo& mode : {q3k(), z8k()})
    {
        for (double seconds : {4.0, 8.0, BROADCAST_SECONDS})
        {
            auto p = planBurst(mode, seconds);
            CHECK(p.has_value());
            CHECK(p->framing.frameBytes <= MAX_FRAME_BYTES);
            int bytes = ragtime::link::burstDataBytes(burstFrames(*p), mode.bytesPerCodeword);
            CHECK(bytes <= p->dataCodewords * mode.bytesPerCodeword);
            // Frames don't straddle codewords: one frame more would need another.
            std::vector<int> more = burstFrames(*p);
            for (int i = 0; i < p->framing.framesPerCodeword; i++) more.push_back((int)p->framing.frameBytes);
            int moreBytes = ragtime::link::burstDataBytes(more, mode.bytesPerCodeword);
            CHECK(moreBytes > p->dataCodewords * mode.bytesPerCodeword);
            CHECK(mode.burstSeconds(burstFrames(*p)) <= seconds + 0.01);
        }
    }
}

void shorterBursts()
{
    // policy.slots_for at 6 s: 9 and 19
    CHECK_EQ(slotsFor(q3k(), 6.0), 9);
    CHECK_EQ(slotsFor(z8k(), 6.0), 19);
    CHECK_EQ(slotsFor(q3k(), 0.5), 1);
    CHECK_EQ(slotsFor(z8k(), 100.0), 54);  // never past what the header says
}

void dupControl()
{
    auto p = planBurst(z8k(), BROADCAST_SECONDS, true);
    CHECK(p && p->dataCodewords == 38 && p->frames == 76);
}

void edges()
{
    CHECK(!planBurst(q3k(), 0.1));  // one slot: control only, no room
    CHECK(knownGear("w48-256l-r5/8").has_value());
    CHECK(!knownGear("qpsk-r1/5").has_value());
}

} // namespace

// One GOP's too big a coding goes where another left room; fewer pictures
// left out beat more bytes.
void bestFillPicks()
{
    // shares of 100 each: GOP 0 undershot (80), GOP 1 overshot (115) and
    // fell short (90) on its tries
    std::vector<std::vector<Coding>> gops = {{{80, 0}, {70, 0}}, {{90, 0}, {115, 0}}};
    auto pick = bestFill(gops, 200);
    CHECK(pick == std::vector<size_t>({0, 1}));  // 195
    pick = bestFill(gops, 190);
    CHECK(pick == std::vector<size_t>({1, 1}));  // 185
    // a coding with pictures dropped only when nothing whole fits
    gops = {{{120, 2}, {150, 0}}, {{60, 0}}};
    CHECK(bestFill(gops, 200) == std::vector<size_t>({0, 0}));
    CHECK(bestFill(gops, 210) == std::vector<size_t>({1, 0}));
    CHECK(bestFill(gops, 100).empty());
    CHECK(bestFill({{{10, 0}}, {}}, 100).empty());
}

int main()
{
    bestFillPicks();
    q3kPlan();
    z8kPlan();
    framesFitTheBurst();
    shorterBursts();
    dupControl();
    edges();
    return ragtime_test::finish();
}
