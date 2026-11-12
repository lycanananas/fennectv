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

size_t lebBytes(size_t v)
{
    size_t n = 1;
    while (v >= 0x80)
    {
        v >>= 7;
        n++;
    }
    return n;
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
    Bytes out;
    out.reserve(FRAME_HEADER_BYTES + size);
    out.push_back((uint8_t)((h.version << 4) | ((uint8_t)h.codec & 0x0F)));
    out.push_back(h.burst);
    out.push_back((uint8_t)((h.idx & 0x7F) | (h.last ? 0x80 : 0)));
    out.push_back(h.gopStart);
    out.insert(out.end(), payload, payload + size);
    return out;
}

std::optional<DecodedFrame> decodeFrame(const Bytes& f)
{
    if (f.size() <= FRAME_HEADER_BYTES) return std::nullopt;
    DecodedFrame d;
    d.header.version = f[0] >> 4;
    d.header.codec = (Codec)(f[0] & 0x0F);
    d.header.burst = f[1];
    d.header.idx = f[2] & 0x7F;
    d.header.last = (f[2] & 0x80) != 0;
    d.header.gopStart = f[3];
    if (d.header.version != PROTOCOL_VERSION) return std::nullopt;
    const size_t payload = f.size() - FRAME_HEADER_BYTES;
    if (d.header.gopStart != NO_GOP_START && d.header.gopStart >= payload) return std::nullopt;
    d.payload.assign(f.begin() + FRAME_HEADER_BYTES, f.end());
    return d;
}

namespace
{

// AV1: the OBUs less temporal delimiters and padding.
Bytes av1ToAir(const Bytes& tu)
{
    Bytes out;
    out.reserve(tu.size());
    for (size_t pos = 0; pos < tu.size();)
    {
        int type = 0;
        size_t length = 0;
        if (!readObu(tu.data(), pos, tu.size(), type, length)) return tu;  // not ours to cut: as it is
        if (type != OBU_TEMPORAL_DELIMITER && type != OBU_PADDING)
            out.insert(out.end(), tu.begin() + (long)pos, tu.begin() + (long)(pos + length));
        pos += length;
    }
    return out;
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

Bytes vvcToAir(const Bytes& au)
{
    Bytes out;
    for (auto [at, len] : annexBNals(au))
    {
        if (len < 2) continue;
        if (!vvcKeep(au[at + 1] >> 3)) continue;
        putLeb(out, len);
        out.insert(out.end(), au.begin() + (long)at, au.begin() + (long)(at + len));
    }
    return out;
}

Bytes vvcFromAir(const Bytes& air)
{
    Bytes out;
    size_t pos = 0, len = 0;
    while (pos < air.size() && getLeb(air.data(), pos, air.size(), len) && len <= air.size() - pos)
    {
        out.insert(out.end(), {0, 0, 0, 1});
        out.insert(out.end(), air.begin() + (long)pos, air.begin() + (long)(pos + len));
        pos += len;
    }
    return out;
}

} // namespace

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
    case Codec::Av1:
    {
        Bytes tu{0x12, 0x00};  // the temporal delimiter, back for the decoder
        tu.insert(tu.end(), air.begin(), air.end());
        return tu;
    }
    case Codec::Vvc: return vvcFromAir(air);
    default: return air;
    }
}

namespace
{

void appendGopHeader(Bytes& out, const GopHeader& h)
{
    const size_t osd = std::min<size_t>(h.osd.size(), 255);
    out.push_back(GOP_MARK);
    out.push_back(h.fps);
    out.push_back(h.first);
    out.push_back(h.count);
    out.push_back(h.burstPictures);
    out.push_back((uint8_t)osd);
    out.insert(out.end(), h.osd.begin(), h.osd.begin() + (long)osd);
}

constexpr size_t GOP_HEADER_BYTES = 6;

} // namespace

size_t gopBytes(Codec codec, const GopHeader& header, const std::vector<Bytes>& units)
{
    size_t n = GOP_HEADER_BYTES + std::min<size_t>(header.osd.size(), 255);
    for (const Bytes& u : units)
    {
        size_t a = toAir(codec, u).size();
        n += lebBytes(a) + a;
    }
    return n;
}

std::optional<std::vector<Bytes>> packBurst(uint8_t burst, Codec codec, const std::vector<Gop>& gops,
                                            size_t payloadBytes, int frames)
{
    if (payloadBytes == 0 || payloadBytes >= NO_GOP_START || payloadBytes + FRAME_HEADER_BYTES > MAX_FRAME_BYTES ||
        frames < 1 || frames > MAX_BURST_FRAMES)
        return std::nullopt;
    Bytes block;
    std::vector<size_t> starts;
    for (const Gop& g : gops)
    {
        starts.push_back(block.size());
        GopHeader h = g.header;
        h.count = (uint8_t)std::min<size_t>(g.units.size(), 255);
        appendGopHeader(block, h);
        for (size_t u = 0; u < h.count; u++)
        {
            Bytes a = toAir(codec, g.units[u]);
            putLeb(block, a.size());
            block.insert(block.end(), a.begin(), a.end());
        }
    }
    const size_t capacity = payloadBytes * (size_t)frames;
    if (block.size() > capacity) return std::nullopt;
    block.resize(capacity, 0);

    std::vector<Bytes> out;
    for (int i = 0; i < frames; i++)
    {
        const size_t from = (size_t)i * payloadBytes;
        FrameHeader h;
        h.codec = codec;
        h.burst = burst;
        h.idx = (uint8_t)i;
        h.last = i == frames - 1;
        for (size_t s : starts)
            if (s >= from && s < from + payloadBytes)
            {
                h.gopStart = (uint8_t)(s - from);
                break;
            }
        out.push_back(encodeFrame(h, block.data() + from, payloadBytes));
    }
    return out;
}

std::vector<ReceivedGop> parseBurst(const std::vector<std::optional<DecodedFrame>>& frames)
{
    std::vector<ReceivedGop> out;
    size_t P = 0;
    for (const auto& f : frames)
        if (f)
        {
            P = f->payload.size();
            break;
        }
    if (P == 0) return out;
    const size_t n = frames.size(), end = n * P;
    Bytes block(end, 0);
    std::vector<bool> ok(n, false);
    for (size_t i = 0; i < n; i++)
        if (frames[i] && frames[i]->payload.size() == P)
        {
            std::copy(frames[i]->payload.begin(), frames[i]->payload.end(), block.begin() + (long)(i * P));
            ok[i] = true;
        }

    // Every byte of [pos, pos + len) arrived.
    auto have = [&](size_t pos, size_t len) {
        if (len == 0) return true;
        if (pos + len > end) return false;
        for (size_t f = pos / P; f <= (pos + len - 1) / P; f++)
            if (!ok[f]) return false;
        return true;
    };
    // The first GOP start, as the frame headers point them out, at or after pos.
    auto resync = [&](size_t pos) -> std::optional<size_t> {
        for (size_t f = pos / P; f < n; f++)
            if (ok[f] && frames[f]->header.gopStart != NO_GOP_START)
            {
                size_t s = f * P + frames[f]->header.gopStart;
                if (s >= pos) return s;
            }
        return std::nullopt;
    };

    Codec codec = Codec::Unknown;
    for (const auto& f : frames)
        if (f)
        {
            codec = f->header.codec;
            break;
        }

    ReceivedGop cur;
    bool inGop = false;
    auto close = [&] {
        if (inGop)
        {
            cur.complete = cur.units.size() == cur.header.count;
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
            size_t p = pos, len = 0;
            if (!getLeb(block.data(), p, end, len) || len > end - p || !have(pos, p - pos + len))
            {
                broken();
                continue;
            }
            cur.units.push_back(fromAir(codec, Bytes(block.begin() + (long)p, block.begin() + (long)(p + len))));
            at = p + len;
            continue;
        }
        close();
        const uint8_t b = block[pos];
        if (b == 0) break;  // the padding: nothing more in this burst
        if (b != GOP_MARK || !have(pos, GOP_HEADER_BYTES) || !have(pos, GOP_HEADER_BYTES + block[pos + 5]))
        {
            broken();
            continue;
        }
        ReceivedGop g;
        g.codec = codec;
        g.header.fps = block[pos + 1];
        g.header.first = block[pos + 2];
        g.header.count = block[pos + 3];
        g.header.burstPictures = block[pos + 4];
        const size_t osd = block[pos + 5];
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
