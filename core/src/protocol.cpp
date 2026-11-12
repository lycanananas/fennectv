#include "fennectv/protocol.h"

#include <algorithm>

namespace fennectv
{

namespace
{

constexpr int OBU_TEMPORAL_DELIMITER = 2;
constexpr int OBU_PADDING = 15;

void putLeb(Bytes& out, size_t v)
{
    while (v >= 0x80)
    {
        out.push_back((uint8_t)(v | 0x80));
        v >>= 7;
    }
    out.push_back((uint8_t)v);
}

// A LEB128 at data[pos..end): false if it isn't whole (or is absurd).
bool getLeb(const uint8_t* data, size_t& pos, size_t end, size_t& v)
{
    v = 0;
    for (int i = 0; i < 4; i++)
    {
        if (pos >= end) return false;
        const uint8_t b = data[pos++];
        v |= (size_t)(b & 0x7F) << (7 * i);
        if (!(b & 0x80)) return true;
    }
    return false;
}

// One OBU at data[pos..end): its type and length, header and size field
// included. False if it isn't whole or has no size field.
bool readObu(const uint8_t* data, size_t pos, size_t end, int& type, size_t& length)
{
    if (pos >= end) return false;
    const uint8_t h = data[pos];
    if (h & 0x80) return false;            // forbidden bit
    if (!(h & 0x02)) return false;         // no size field
    type = (h >> 3) & 0x0F;
    size_t p = pos + 1 + ((h >> 2) & 1);   // the extension byte
    uint64_t size = 0;
    for (int i = 0;; i++)
    {
        if (i == 8 || p >= end) return false;
        const uint8_t b = data[p++];
        size |= (uint64_t)(b & 0x7F) << (7 * i);
        if (!(b & 0x80)) break;
    }
    if (size > end - p) return false;
    length = (p - pos) + (size_t)size;
    return true;
}

} // namespace

Bytes encodeFrame(const FrameHeader& h, const uint8_t* payload, size_t size)
{
    const bool start = h.gopStart != NO_GOP_START;
    Bytes out;
    out.reserve(FRAME_HEADER_MAX + size);
    out.push_back((uint8_t)(((uint8_t)h.codec & 3) << 6 | (start ? 0x20 : 0) | (h.burst % BURST_NUMBERS)));
    out.push_back((uint8_t)((h.idx & 0x7F) | (h.last ? 0x80 : 0)));
    if (start) out.push_back(h.gopStart);
    out.insert(out.end(), payload, payload + size);
    return out;
}

std::optional<DecodedFrame> decodeFrame(const Bytes& f)
{
    if (f.size() <= FRAME_HEADER_BYTES) return std::nullopt;
    DecodedFrame d;
    d.header.codec = (Codec)(f[0] >> 6);
    const bool start = (f[0] & 0x20) != 0;
    d.header.burst = f[0] & 0x1F;
    d.header.idx = f[1] & 0x7F;
    d.header.last = (f[1] & 0x80) != 0;
    if (d.header.codec == Codec::Unknown) return std::nullopt;
    const size_t head = start ? FRAME_HEADER_MAX : FRAME_HEADER_BYTES;
    if (f.size() <= head) return std::nullopt;
    const size_t payload = f.size() - head;
    d.header.gopStart = start ? f[2] : NO_GOP_START;
    if (start && d.header.gopStart >= payload) return std::nullopt;
    d.payload.assign(f.begin() + (long)head, f.end());
    return d;
}

namespace
{

// AV1 on the air: each OBU but temporal delimiters and padding as
// [(payload length << 3 | kind << 1 | more), LEB128]([OBU header])[payload]:
// kind 0 a frame, 1 a frame header, 2 a sequence header, their OBU header
// implied (no extension); 3 anything else, its OBU header (and extension)
// following. more is clear on the unit's last OBU. Never longer than the
// OBU itself, often a byte shorter.
constexpr int OBU_FRAME_HEADER = 3;
constexpr int OBU_FRAME = 6;
constexpr int AV1_KIND_TYPE[3] = {OBU_FRAME, OBU_FRAME_HEADER, 1 /* sequence header */};

Bytes av1ToAir(const Bytes& tu)
{
    struct Obu
    {
        size_t head, headLen, payload, payloadLen;
        int type;
    };
    std::vector<Obu> keep;
    for (size_t pos = 0; pos < tu.size();)
    {
        int type = 0;
        size_t length = 0;
        if (!readObu(tu.data(), pos, tu.size(), type, length)) return {};  // not a unit we can send
        const size_t headLen = 1 + ((tu[pos] >> 2) & 1);
        size_t p = pos + headLen, size = 0;
        getLeb(tu.data(), p, tu.size(), size);  // readObu checked it
        if (type != OBU_TEMPORAL_DELIMITER && type != OBU_PADDING) keep.push_back({pos, headLen, p, size, type});
        pos += length;
    }
    Bytes out;
    out.reserve(tu.size());
    for (size_t k = 0; k < keep.size(); k++)
    {
        const Obu& o = keep[k];
        int kind = 3;
        for (int c = 0; c < 3; c++)
            if (o.headLen == 1 && o.type == AV1_KIND_TYPE[c]) kind = c;
        putLeb(out, o.payloadLen << 3 | (size_t)kind << 1 | (k + 1 < keep.size() ? 1 : 0));
        if (kind == 3) out.insert(out.end(), tu.begin() + (long)o.head, tu.begin() + (long)(o.head + o.headLen));
        out.insert(out.end(), tu.begin() + (long)o.payload, tu.begin() + (long)(o.payload + o.payloadLen));
    }
    return out;
}

// Walks the AV1 unit at data[pos..end); calls obu(header bytes, payload
// offset, payload length) for each. Its length on the air, 0 if it isn't
// whole.
template <typename F>
size_t av1Walk(const uint8_t* data, size_t pos, size_t end, F obu)
{
    for (size_t p = pos; p < end;)
    {
        size_t v = 0;
        if (!getLeb(data, p, end, v)) return 0;
        const int kind = (int)(v >> 1 & 3);
        const size_t len = v >> 3;
        uint8_t head[2] = {0, 0};
        size_t headLen = 1;
        if (kind < 3)
            head[0] = (uint8_t)(AV1_KIND_TYPE[kind] << 3);
        else
        {
            if (p >= end) return 0;
            head[0] = data[p++];
            if (head[0] & 0x04)
            {
                if (p >= end) return 0;
                head[1] = data[p++];
                headLen = 2;
            }
        }
        if (len > end - p) return 0;
        obu(head, headLen, p, len);
        p += len;
        if (!(v & 1)) return p - pos;
    }
    return 0;
}

size_t av1AirLength(const uint8_t* data, size_t pos, size_t end)
{
    return av1Walk(data, pos, end, [](const uint8_t*, size_t, size_t, size_t) {});
}

Bytes av1FromAir(const Bytes& air)
{
    Bytes tu{0x12, 0x00};  // the temporal delimiter, back for the decoder
    av1Walk(air.data(), 0, air.size(), [&](const uint8_t* head, size_t headLen, size_t at, size_t len) {
        tu.push_back((uint8_t)((head[0] | 0x02) & ~0x01));  // with its size field
        if (headLen == 2) tu.push_back(head[1]);
        putLeb(tu, len);
        tu.insert(tu.end(), air.begin() + (long)at, air.begin() + (long)(at + len));
    });
    return tu;
}

// VVC: the NAL units of an Annex B access unit, without the start codes.
std::vector<std::pair<size_t, size_t>> annexBNals(const Bytes& au)
{
    std::vector<std::pair<size_t, size_t>> nals;  // offset, length
    size_t i = 0, start = std::string::npos;
    const size_t n = au.size();
    while (i + 2 < n)
    {
        if (au[i] == 0 && au[i + 1] == 0 && au[i + 2] == 1)
        {
            if (start != std::string::npos)
            {
                size_t end = i;
                while (end > start && au[end - 1] == 0) end--;  // a 4-byte start code's zero, trailing zeros
                nals.push_back({start, end - start});
            }
            i += 3;
            start = i;
        }
        else
            i++;
    }
    if (start != std::string::npos && start < n)
    {
        size_t end = n;
        while (end > start && au[end - 1] == 0) end--;
        nals.push_back({start, end - start});
    }
    return nals;
}

bool vvcKeep(int type)
{
    // AUD 20, EOS 21, EOB 22, prefix and suffix SEI 23 and 24, filler 25
    return type < 20 || type > 25;
}

// Each NAL unit [(length << 1 | more)][all but its first byte]: that byte
// is 0 in one layer (forbidden and reserved bits, layer 0).
Bytes vvcToAir(const Bytes& au)
{
    std::vector<std::pair<size_t, size_t>> keep;
    for (auto [at, len] : annexBNals(au))
    {
        if (len < 2 || !vvcKeep(au[at + 1] >> 3)) continue;
        if (au[at] != 0) return {};  // more than one layer: not ours to send
        keep.push_back({at + 1, len - 1});
    }
    Bytes out;
    for (size_t k = 0; k < keep.size(); k++)
    {
        auto [at, len] = keep[k];
        putLeb(out, len << 1 | (k + 1 < keep.size() ? 1 : 0));
        out.insert(out.end(), au.begin() + (long)at, au.begin() + (long)(at + len));
    }
    return out;
}

size_t vvcAirLength(const uint8_t* data, size_t pos, size_t end)
{
    for (size_t p = pos; p < end;)
    {
        size_t v = 0;
        if (!getLeb(data, p, end, v) || (v >> 1) == 0 || (v >> 1) > end - p) return 0;
        p += v >> 1;
        if (!(v & 1)) return p - pos;
    }
    return 0;
}

Bytes vvcFromAir(const Bytes& air)
{
    Bytes out;
    size_t pos = 0, v = 0;
    while (pos < air.size() && getLeb(air.data(), pos, air.size(), v) && (v >> 1) <= air.size() - pos)
    {
        out.insert(out.end(), {0, 0, 0, 1, 0});
        out.insert(out.end(), air.begin() + (long)pos, air.begin() + (long)(pos + (v >> 1)));
        pos += v >> 1;
        if (!(v & 1)) break;
    }
    return out;
}

// The length of the unit at data[pos..end) as it went on the air; 0 if it
// isn't whole.
size_t airLength(Codec codec, const uint8_t* data, size_t pos, size_t end)
{
    switch (codec)
    {
    case Codec::Av1: return av1AirLength(data, pos, end);
    case Codec::Vvc: return vvcAirLength(data, pos, end);
    default: return 0;
    }
}

} // namespace

namespace
{

constexpr int OBU_SEQUENCE_HEADER = 1;

bool vvcParam(int type)
{
    // OPI 12, DCI 13, VPS 14, SPS 15, PPS 16
    return type >= 12 && type <= 16;
}

// An AV1 temporal unit's OBUs (offset, length); empty if they don't parse.
std::vector<std::pair<size_t, size_t>> obus(const Bytes& tu)
{
    std::vector<std::pair<size_t, size_t>> out;
    for (size_t pos = 0; pos < tu.size();)
    {
        int type = 0;
        size_t length = 0;
        if (!readObu(tu.data(), pos, tu.size(), type, length)) return {};
        out.push_back({pos, length});
        pos += length;
    }
    return out;
}

// The unit split into the parameters and the rest, as the decoder takes
// them (Annex B with 4-byte start codes for VVC).
void splitParams(Codec codec, const Bytes& unit, Bytes* params, Bytes* rest)
{
    if (codec == Codec::Av1)
    {
        auto list = obus(unit);
        if (list.empty())
        {
            if (rest) *rest = unit;
            return;
        }
        for (auto [at, len] : list)
        {
            const bool seq = ((unit[at] >> 3) & 0x0F) == OBU_SEQUENCE_HEADER;
            Bytes* to = seq ? params : rest;
            if (to) to->insert(to->end(), unit.begin() + (long)at, unit.begin() + (long)(at + len));
        }
        return;
    }
    if (codec == Codec::Vvc)
    {
        for (auto [at, len] : annexBNals(unit))
        {
            if (len < 2) continue;
            Bytes* to = vvcParam(unit[at + 1] >> 3) ? params : rest;
            if (!to) continue;
            to->insert(to->end(), {0, 0, 0, 1});
            to->insert(to->end(), unit.begin() + (long)at, unit.begin() + (long)(at + len));
        }
        return;
    }
    if (rest) *rest = unit;
}

} // namespace

Bytes paramsOf(Codec codec, const Bytes& unit)
{
    Bytes p;
    splitParams(codec, unit, &p, nullptr);
    return p;
}

Bytes withoutParams(Codec codec, const Bytes& unit)
{
    Bytes r;
    splitParams(codec, unit, nullptr, &r);
    return r;
}

Bytes withParams(Codec codec, const Bytes& unit, const Bytes& params)
{
    if (params.empty()) return unit;
    if (codec == Codec::Av1)
    {
        // after the temporal delimiter, if it leads
        auto list = obus(unit);
        size_t at = 0;
        if (!list.empty() && ((unit[0] >> 3) & 0x0F) == OBU_TEMPORAL_DELIMITER) at = list[0].second;
        Bytes out(unit.begin(), unit.begin() + (long)at);
        out.insert(out.end(), params.begin(), params.end());
        out.insert(out.end(), unit.begin() + (long)at, unit.end());
        return out;
    }
    Bytes out = params;
    out.insert(out.end(), unit.begin(), unit.end());
    return out;
}

std::vector<Bytes> gopToAir(Codec codec, const std::vector<Bytes>& units, bool params)
{
    std::vector<Bytes> out;
    out.reserve(units.size());
    const Bytes first = units.empty() ? Bytes{} : paramsOf(codec, units[0]);
    for (size_t i = 0; i < units.size(); i++)
    {
        bool cut = i == 0 ? !params : !first.empty() && paramsOf(codec, units[i]) == first;
        out.push_back(toAir(codec, cut ? withoutParams(codec, units[i]) : units[i]));
    }
    return out;
}

Bytes toAir(Codec codec, const Bytes& unit)
{
    switch (codec)
    {
    case Codec::Av1: return av1ToAir(unit);
    case Codec::Vvc: return vvcToAir(unit);
    default: return unit;
    }
}

Bytes fromAir(Codec codec, const Bytes& air)
{
    switch (codec)
    {
    case Codec::Av1: return av1FromAir(air);
    case Codec::Vvc: return vvcFromAir(air);
    default: return air;
    }
}

namespace
{

void appendGopHeader(Bytes& out, const GopHeader& h)
{
    const size_t osd = std::min<size_t>(h.osd.size(), 255);
    out.push_back(h.count);
    out.push_back(h.first);
    out.push_back(h.fps);
    out.push_back(h.burstPictures);
    out.push_back((uint8_t)osd);
    out.insert(out.end(), h.osd.begin(), h.osd.begin() + (long)osd);
}

constexpr size_t GOP_HEADER_BYTES = 5;

} // namespace

size_t gopBytes(Codec codec, const GopHeader& header, const std::vector<Bytes>& units, bool params)
{
    size_t n = GOP_HEADER_BYTES + std::min<size_t>(header.osd.size(), 255);
    for (const Bytes& a : gopToAir(codec, units, params)) n += a.size();
    return n;
}

size_t burstCapacity(size_t payloadBytes, int frames, int gops)
{
    const size_t all = payloadBytes * (size_t)std::max(0, frames);
    const size_t starts = (size_t)std::max(0, gops);
    return all > starts ? all - starts : 0;
}

std::optional<std::vector<Bytes>> packBurst(uint8_t burst, Codec codec, const std::vector<Gop>& gops,
                                            size_t payloadBytes, int frames)
{
    if (payloadBytes < 2 || payloadBytes > NO_GOP_START || payloadBytes + FRAME_HEADER_BYTES > MAX_FRAME_BYTES ||
        frames < 1 || frames > MAX_BURST_FRAMES)
        return std::nullopt;
    Bytes block;
    std::vector<size_t> starts;
    for (const Gop& g : gops)
    {
        if (g.units.empty()) continue;
        starts.push_back(block.size());
        GopHeader h = g.header;
        h.count = (uint8_t)std::min<size_t>(g.units.size(), 255);
        appendGopHeader(block, h);
        const std::vector<Bytes> air =
            gopToAir(codec, std::vector<Bytes>(g.units.begin(), g.units.begin() + h.count), g.params);
        for (const Bytes& a : air)
        {
            if (a.empty()) return std::nullopt;
            block.insert(block.end(), a.begin(), a.end());
        }
    }

    // Frames of payloadBytes, a byte less where a GOP starts.
    std::vector<Bytes> out;
    size_t from = 0, next = 0;
    for (int i = 0; i < frames; i++)
    {
        FrameHeader h;
        h.codec = codec;
        h.burst = burst;
        h.idx = (uint8_t)i;
        h.last = i == frames - 1;
        size_t take = payloadBytes;
        while (next < starts.size() && starts[next] < from) next++;
        if (next < starts.size() && starts[next] < from + payloadBytes - 1)
        {
            take = payloadBytes - 1;
            h.gopStart = (uint8_t)(starts[next] - from);
        }
        Bytes payload(take, 0);
        if (from < block.size())
            std::copy(block.begin() + (long)from, block.begin() + (long)std::min(block.size(), from + take),
                      payload.begin());
        out.push_back(encodeFrame(h, payload.data(), payload.size()));
        from += take;
    }
    if (block.size() > from) return std::nullopt;
    return out;
}

std::vector<ReceivedGop> parseBurst(const std::vector<std::optional<DecodedFrame>>& frames, ParamCache* cache)
{
    std::vector<ReceivedGop> out;
    ParamCache local;
    ParamCache& params = cache ? *cache : local;
    // Frames are all as long; a payload is the frame less its header. A
    // lost frame is taken to have had no GOP start: if it had, what
    // follows it sits a byte off, but nothing is read across a lost frame,
    // only from the next GOP start a frame points out, and that's where the
    // frame says, wherever the frame is taken to begin.
    size_t P = 0;
    Codec codec = Codec::Unknown;
    for (const auto& f : frames)
        if (f)
        {
            P = f->payload.size() + (f->header.gopStart != NO_GOP_START ? 1 : 0);
            codec = f->header.codec;
            break;
        }
    if (P == 0) return out;
    const size_t n = frames.size();
    std::vector<size_t> at0(n + 1, 0);  // where each frame begins in the block
    std::vector<bool> ok(n, false);
    for (size_t i = 0; i < n; i++)
    {
        size_t len = P;
        if (frames[i])
        {
            const size_t full = frames[i]->payload.size() + (frames[i]->header.gopStart != NO_GOP_START ? 1 : 0);
            if (full == P)
            {
                ok[i] = true;
                len = frames[i]->payload.size();
            }
        }
        at0[i + 1] = at0[i] + len;
    }
    const size_t end = at0[n];
    Bytes block(end, 0);
    for (size_t i = 0; i < n; i++)
        if (ok[i]) std::copy(frames[i]->payload.begin(), frames[i]->payload.end(), block.begin() + (long)at0[i]);

    auto frameOf = [&](size_t pos) {
        return (size_t)(std::upper_bound(at0.begin(), at0.end(), pos) - at0.begin()) - 1;
    };
    // Every byte of [pos, pos + len) arrived.
    auto have = [&](size_t pos, size_t len) {
        if (len == 0) return true;
        if (pos + len > end) return false;
        for (size_t f = frameOf(pos); f <= frameOf(pos + len - 1); f++)
            if (!ok[f]) return false;
        return true;
    };
    // The first GOP start, as the frame headers point them out, at or after pos.
    auto resync = [&](size_t pos) -> std::optional<size_t> {
        for (size_t f = pos < end ? frameOf(pos) : n; f < n; f++)
            if (ok[f] && frames[f]->header.gopStart != NO_GOP_START)
            {
                size_t s = at0[f] + frames[f]->header.gopStart;
                if (s >= pos) return s;
            }
        return std::nullopt;
    };

    ReceivedGop cur;
    bool inGop = false;
    auto close = [&] {
        if (inGop)
        {
            cur.complete = cur.units.size() == cur.header.count;
            if (!cur.units.empty())
            {
                Bytes p = paramsOf(codec, cur.units[0]);
                if (!p.empty())
                    params.of(codec) = std::move(p);
                else
                    cur.units[0] = withParams(codec, cur.units[0], params.of(codec));
            }
            out.push_back(std::move(cur));
            cur = ReceivedGop{};
            inGop = false;
        }
    };

    std::optional<size_t> at = resync(0);
    if (ok[0]) at = 0;
    while (at && *at < end)
    {
        const size_t pos = *at;
        // Lost bytes, or bytes that make no sense: the GOP ends here, and
        // the next one the frames point out takes over.
        auto broken = [&] {
            close();
            at = resync(pos + 1);
        };
        if (!have(pos, 1))
        {
            broken();
            continue;
        }
        if (inGop && cur.units.size() < cur.header.count)
        {
            // A unit ends itself; it must end within what arrived.
            size_t stop = pos;
            while (stop < end && ok[frameOf(stop)]) stop = at0[frameOf(stop) + 1];
            const size_t len = airLength(codec, block.data(), pos, stop);
            if (len == 0)
            {
                broken();
                continue;
            }
            cur.units.push_back(fromAir(codec, Bytes(block.begin() + (long)pos, block.begin() + (long)(pos + len))));
            at = pos + len;
            continue;
        }
        close();
        if (block[pos] == 0) break;  // the padding: nothing more in this burst
        if (!have(pos, GOP_HEADER_BYTES) || !have(pos, GOP_HEADER_BYTES + block[pos + 4]))
        {
            broken();
            continue;
        }
        ReceivedGop g;
        g.codec = codec;
        g.header.count = block[pos];
        g.header.first = block[pos + 1];
        g.header.fps = block[pos + 2];
        g.header.burstPictures = block[pos + 3];
        const size_t osd = block[pos + 4];
        g.header.osd.assign(block.begin() + (long)(pos + GOP_HEADER_BYTES),
                            block.begin() + (long)(pos + GOP_HEADER_BYTES + osd));
        cur = std::move(g);
        inGop = true;
        at = pos + GOP_HEADER_BYTES + osd;
    }
    close();
    return out;
}

} // namespace fennectv
