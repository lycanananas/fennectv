// Frames and bursts: round trips, what readers refuse, and what a receiver
// gets from a burst with frames missing.
#include <random>

#include "Check.h"
#include "fennectv/protocol.h"

using namespace fennectv;

namespace
{

void obu(Bytes& out, int type, const Bytes& payload)
{
    out.push_back((uint8_t)(type << 3 | 0x02));
    size_t n = payload.size();
    do
    {
        uint8_t b = n & 0x7F;
        n >>= 7;
        out.push_back(n ? (uint8_t)(b | 0x80) : b);
    } while (n);
    out.insert(out.end(), payload.begin(), payload.end());
}

// A temporal unit as libaom gives it: delimiter, sequence header on a
// keyframe, the frame. The payload bytes are anything, 0x00 and 0x80 too.
Bytes picture(std::mt19937& rng, size_t size, bool key)
{
    Bytes tu{0x12, 0x00};
    if (key) obu(tu, 1, Bytes{0x00, 0x00, 0x00, 0x0a, 0x0b, 0xf0, 0x80, 0x00});
    Bytes payload(size);
    for (auto& b : payload) b = (uint8_t)rng();
    obu(tu, 6, payload);
    return tu;
}

std::vector<Gop> someGops(std::mt19937& rng, int gops, int perGop)
{
    std::vector<Gop> out;
    for (int g = 0; g < gops; g++)
    {
        Gop gop;
        gop.header.fps = 5;
        gop.header.first = (uint8_t)(g * perGop);
        gop.header.count = (uint8_t)perGop;
        gop.header.burstPictures = (uint8_t)(gops * perGop);
        if (g == 0) gop.header.osd = "N0CALL";  // once a burst
        for (int i = 0; i < perGop; i++) gop.units.push_back(picture(rng, i == 0 ? 700 : 40 + rng() % 150, i == 0));
        out.push_back(std::move(gop));
    }
    return out;
}

std::vector<std::optional<DecodedFrame>> receive(const std::vector<Bytes>& frames, std::vector<int> lost = {})
{
    std::vector<std::optional<DecodedFrame>> got(frames.size());
    for (size_t i = 0; i < frames.size(); i++)
    {
        bool gone = false;
        for (int l : lost) gone = gone || l == (int)i;
        if (!gone) got[i] = decodeFrame(frames[i]);
    }
    return got;
}

// The frame holding byte `pos` of the burst (payloads differ: a byte
// less where a GOP starts).
int frameAt(const std::vector<Bytes>& frames, size_t pos)
{
    size_t at = 0;
    for (size_t i = 0; i < frames.size(); i++)
    {
        at += decodeFrame(frames[i])->payload.size();
        if (pos < at) return (int)i;
    }
    return (int)frames.size();
}

void frames()
{
    FrameHeader h;
    h.burst = 200;  // 8 on the air
    h.idx = 77;
    h.last = true;
    h.gopStart = 12;
    Bytes payload(142, 0x5a);
    Bytes f = encodeFrame(h, payload.data(), payload.size());
    CHECK_EQ(f.size(), (size_t)145);
    CHECK_EQ(FRAME_HEADER_BYTES, (size_t)2);
    auto d = decodeFrame(f);
    CHECK(d.has_value());
    CHECK(d->header.codec == Codec::Av1);
    CHECK(d->header.burst == 8 && d->header.idx == 77 && d->header.last && d->header.gopStart == 12);
    CHECK(d->payload == payload);

    // No GOP start: two bytes of header.
    h.gopStart = NO_GOP_START;
    h.codec = Codec::Vvc;
    h.last = false;
    Bytes plain = encodeFrame(h, payload.data(), payload.size());
    CHECK_EQ(plain.size(), (size_t)144);
    d = decodeFrame(plain);
    CHECK(d && d->header.codec == Codec::Vvc && d->header.gopStart == NO_GOP_START && !d->header.last);
    CHECK(d->payload == payload);

    Bytes none = f;
    none[0] &= 0x3F;  // no codec
    CHECK(!decodeFrame(none));
    Bytes far = f;
    far[2] = 142;  // GOP start past the payload
    CHECK(!decodeFrame(far));
    CHECK(!decodeFrame(Bytes{0x60, 0}));         // no payload
    CHECK(!decodeFrame(Bytes{0x60 | 0x20, 0, 0}));  // GOP start, no payload
}

void delimitersGo()
{
    std::mt19937 rng(1);
    Bytes key = picture(rng, 300, true);
    Bytes air = toAir(Codec::Av1, key);
    // [8 << 3 | sequence header << 1 | more][8 B][300 << 3 | frame << 1][300 B]
    CHECK_EQ(air.size(), (size_t)(1 + 8 + 2 + 300));
    CHECK_EQ(air[0], (uint8_t)(8 << 3 | 2 << 1 | 1));
    CHECK(fromAir(Codec::Av1, air) == key);
    Bytes p = picture(rng, 20, false);
    obu(p, 15, Bytes(9, 0));  // padding goes too
    CHECK_EQ(toAir(Codec::Av1, p).size(), (size_t)(2 + 20));
    // a small frame header, a byte less than the OBU
    Bytes show{0x12, 0x00};
    obu(show, 3, Bytes{0x9a});
    CHECK_EQ(toAir(Codec::Av1, show).size(), (size_t)2);
    CHECK(fromAir(Codec::Av1, toAir(Codec::Av1, show)) == show);
    // anything else keeps its OBU header (metadata, with an extension)
    Bytes meta{0x12, 0x00, (uint8_t)(5 << 3 | 0x04 | 0x02), 0x28, 0x02, 0x01, 0x02};
    CHECK_EQ(toAir(Codec::Av1, meta).size(), (size_t)(1 + 2 + 2));
    CHECK(fromAir(Codec::Av1, toAir(Codec::Av1, meta)) == meta);
    Bytes junk{0x80, 1, 2};
    CHECK(toAir(Codec::Av1, junk).empty());  // not OBUs: nothing to send
}

// VVC: start codes become sizes, SEI and delimiters go.
void vvcUnits()
{
    auto nal = [](int type, size_t len, uint8_t fill) {
        Bytes n(len, fill);
        n[0] = 0x00;
        n[1] = (uint8_t)(type << 3 | 1);
        return n;
    };
    Bytes sps = nal(15, 20, 0x11), pps = nal(16, 8, 0x22), sei = nal(23, 30, 0x33), aud = nal(20, 3, 0x44),
          slice = nal(8, 300, 0x55);  // IDR_N_LP
    slice.back() = 0x80;              // rbsp trailing bits
    Bytes au;
    for (const Bytes* n : {&aud, &sps, &pps, &sei, &slice})
    {
        au.insert(au.end(), {0, 0, 0, 1});
        au.insert(au.end(), n->begin(), n->end());
    }
    Bytes air = toAir(Codec::Vvc, au);
    // each [length << 1 | more][the NAL less its first byte]
    CHECK_EQ(air.size(), (size_t)(1 + 19 + 1 + 7 + 2 + 299));
    CHECK_EQ(air[0], (uint8_t)(19 << 1 | 1));
    Bytes back = fromAir(Codec::Vvc, air);
    Bytes want;
    for (const Bytes* n : {&sps, &pps, &slice})
    {
        want.insert(want.end(), {0, 0, 0, 1});
        want.insert(want.end(), n->begin(), n->end());
    }
    CHECK(back == want);

    // SPS and PPS are the parameters; without them only the slice goes.
    CHECK_EQ(paramsOf(Codec::Vvc, au).size(), (size_t)(4 + 20 + 4 + 8));
    CHECK_EQ(toAir(Codec::Vvc, withoutParams(Codec::Vvc, au)).size(), (size_t)(2 + 299));

    // Two layers can't go: the first byte isn't 0.
    Bytes layered = au;
    layered[4 + 3 + 4] = 0x01;  // the SPS's layer
    CHECK(toAir(Codec::Vvc, layered).empty());

    // A VVC burst goes and comes back the same way.
    Gop g;
    g.header.fps = 5;
    g.header.count = 2;
    g.header.burstPictures = 2;
    g.units = {au, au};
    auto frames = packBurst(4, Codec::Vvc, {g}, 141, 10);
    CHECK(frames.has_value());
    std::vector<std::optional<DecodedFrame>> got;
    for (auto& f : *frames) got.push_back(decodeFrame(f));
    auto r = parseBurst(got);
    CHECK_EQ(r.size(), (size_t)1);
    // the second unit's SPS and PPS repeat the first's: they don't go
    CHECK(r[0].codec == Codec::Vvc && r[0].complete && r[0].units.size() == 2);
    CHECK(r[0].units[0] == want && r[0].units[1] == withoutParams(Codec::Vvc, want));
}

void wholeBurst()
{
    std::mt19937 rng(2);
    auto gops = someGops(rng, 4, 15);
    size_t bytes = 0;
    for (auto& g : gops) bytes += gopBytes(Codec::Av1, g.header, g.units);
    auto frames = packBurst(9, Codec::Av1, gops, 141, 78);
    CHECK(frames.has_value());
    CHECK_EQ(frames->size(), (size_t)78);
    for (size_t i = 0; i < frames->size(); i++)
    {
        auto d = decodeFrame((*frames)[i]);
        CHECK(d && d->header.idx == i && d->header.burst == 9 && d->header.last == (i == 77));
    }
    CHECK_EQ(decodeFrame((*frames)[0])->header.gopStart, (uint8_t)0);

    auto got = parseBurst(receive(*frames));
    CHECK_EQ(got.size(), (size_t)4);
    for (size_t g = 0; g < got.size(); g++)
    {
        CHECK(got[g].complete);
        CHECK_EQ(got[g].header.first, (uint8_t)(g * 15));
        CHECK_EQ(got[g].header.burstPictures, (uint8_t)60);
        CHECK(got[g].header.osd == gops[g].header.osd);
        CHECK(got[g].units == gops[g].units);  // the delimiter is back
        CHECK(got[g].codec == Codec::Av1);
    }
    CHECK(bytes <= burstCapacity(141, 78, 4));
    CHECK(!packBurst(9, Codec::Av1, gops, 141, (int)(bytes / 141)));  // a frame short
    // the capacity is what it says: GOPs filling it to the byte still go
    CHECK(packBurst(9, Codec::Av1, gops, 141, (int)((bytes + 4 + 140) / 141)).has_value());
}

// Lost frames: the GOP they cut ends there, the next GOP the frames point
// to picks up.
void lostFrames()
{
    std::mt19937 rng(3);
    auto gops = someGops(rng, 4, 15);
    const size_t P = 141;
    auto frames = packBurst(1, Codec::Av1, gops, P, 78);
    CHECK(frames.has_value());
    std::vector<size_t> start;
    size_t at = 0;
    for (auto& g : gops)
    {
        start.push_back(at);
        at += gopBytes(Codec::Av1, g.header, g.units);
    }
    const size_t end = at;

    // Inside GOP 1, past its keyframe, no GOP starting in that frame.
    int inside = frameAt(*frames, start[2]) - 1;
    CHECK(inside > frameAt(*frames, start[1]) + 5);
    auto got = parseBurst(receive(*frames, {inside}));
    CHECK_EQ(got.size(), (size_t)4);
    CHECK(got[0].complete && got[2].complete && got[3].complete);
    CHECK(!got[1].complete);
    CHECK(got[1].units.size() > 0 && got[1].units.size() < 15);
    for (size_t i = 0; i < got[1].units.size(); i++) CHECK(got[1].units[i] == gops[1].units[i]);

    // The first frame: GOP 0 is gone, the rest are whole.
    got = parseBurst(receive(*frames, {0}));
    CHECK_EQ(got.size(), (size_t)3);
    CHECK(got[0].header.first == 15 && got[0].complete);

    // The frame where GOP 2 starts: GOP 1 loses its end, GOP 2 is gone.
    got = parseBurst(receive(*frames, {frameAt(*frames, start[2])}));
    CHECK_EQ(got.size(), (size_t)3);
    CHECK(got[0].complete && !got[1].complete && got[2].header.first == 45 && got[2].complete);

    // Only the padding lost: nothing missed.
    got = parseBurst(receive(*frames, {77, 76}));
    CHECK(frameAt(*frames, end) < 76);
    CHECK_EQ(got.size(), (size_t)4);
    for (auto& g : got) CHECK(g.complete);

    // A fade over most of the burst: whatever GOP starts after it.
    std::vector<int> fade;
    for (int i = 0; i <= frameAt(*frames, start[3]) - 1; i++) fade.push_back(i);
    got = parseBurst(receive(*frames, fade));
    CHECK_EQ(got.size(), (size_t)1);
    CHECK(got[0].header.first == 45 && got[0].complete);

    // Nothing at all.
    std::vector<int> all;
    for (int i = 0; i < 78; i++) all.push_back(i);
    CHECK(parseBurst(receive(*frames, all)).empty());
}

// GOPs shorter than a frame: two start in one, the second is still found.
void smallGops()
{
    std::mt19937 rng(4);
    std::vector<Gop> gops;
    for (int g = 0; g < 6; g++)
    {
        Gop gop;
        gop.header.fps = 1;
        gop.header.first = (uint8_t)g;
        gop.header.count = 1;
        gop.header.burstPictures = 6;
        gop.units.push_back(picture(rng, 30, true));
        gops.push_back(std::move(gop));
    }
    auto frames = packBurst(2, Codec::Av1, gops, 229, 5);
    CHECK(frames.has_value());
    auto got = parseBurst(receive(*frames));
    CHECK_EQ(got.size(), (size_t)6);
    for (auto& g : got) CHECK(g.complete);
}

// The sequence header only with the burst's first GOP: the others get it
// back from it, or from the burst before when the first GOP is lost, and
// nothing repeats it within a GOP.
void paramsOnce()
{
    std::mt19937 rng(5);
    auto gops = someGops(rng, 4, 15);
    // libaom sends the sequence header again later in a GOP: it goes
    obu(gops[2].units[5], 1, Bytes{0x00, 0x00, 0x00, 0x0a, 0x0b, 0xf0, 0x80, 0x00});
    const Bytes seq = paramsOf(Codec::Av1, gops[0].units[0]);
    CHECK_EQ(seq.size(), (size_t)10);
    CHECK(paramsOf(Codec::Av1, gops[0].units[1]).empty());
    CHECK(withParams(Codec::Av1, withoutParams(Codec::Av1, gops[0].units[0]), seq) == gops[0].units[0]);

    size_t all = 0, once = 0;
    for (size_t g = 0; g < gops.size(); g++)
    {
        all += gopBytes(Codec::Av1, gops[g].header, gops[g].units);
        once += gopBytes(Codec::Av1, gops[g].header, gops[g].units, g == 0);
        gops[g].params = g == 0;
    }
    CHECK_EQ(all - once, (size_t)(3 * 9));  // three GOPs' headers (9 B on the air)
    size_t raw = 5;  // GOP 2's header (no OSD) and units as they are, less delimiters
    for (const Bytes& u : gops[2].units) raw += toAir(Codec::Av1, u).size();
    CHECK_EQ(raw - gopBytes(Codec::Av1, gops[2].header, gops[2].units), (size_t)9);  // the repeat

    auto frames = packBurst(3, Codec::Av1, gops, 141, 78);
    CHECK(frames.has_value());
    auto want = [&](size_t g, size_t u) {
        Bytes x = gops[g].units[u];
        return g == 2 && u == 5 ? withoutParams(Codec::Av1, x) : x;
    };
    ParamCache cache;
    auto got = parseBurst(receive(*frames), &cache);
    CHECK_EQ(got.size(), (size_t)4);
    for (size_t g = 0; g < got.size(); g++)
        for (size_t u = 0; u < got[g].units.size(); u++) CHECK(got[g].units[u] == want(g, u));
    CHECK(cache.av1 == seq);

    // GOP 0 lost: the next burst's GOPs take the header from the cache...
    got = parseBurst(receive(*frames, {0}), &cache);
    CHECK_EQ(got.size(), (size_t)3);
    CHECK(got[0].header.first == 15 && got[0].units[0] == gops[1].units[0]);
    // ...and without one, they come as they are (no header: no picture)
    got = parseBurst(receive(*frames, {0}));
    CHECK(got[0].units[0] == withoutParams(Codec::Av1, gops[1].units[0]));
}

} // namespace

int main()
{
    frames();
    delimitersGo();
    vvcUnits();
    wholeBurst();
    lostFrames();
    smallGops();
    paramsOnce();
    return ragtime_test::finish();
}
