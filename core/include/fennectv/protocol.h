// What FennecTV puts on the air (docs/PLAN.md, "On the air").
//
// One Data2G burst carries a few closed GOPs back to back, the rest of it
// zeros, cut into KISS frames with a 4-byte header. Data2G's LDPC and CRC
// look after each codeword and nothing more is added: a lost frame loses
// what it carried, and the receiver picks up again at the next GOP that
// starts in a frame it has (the header says where). No sockets, no clock,
// no FFmpeg.
//
// Frame:  [version 4 | codec 4][burst][idx 7 | last 1][GOP start][payload]
//   burst      the burst's number, wraps
//   idx        the frame's place in the burst; last set on the burst's last
//   GOP start  where in the payload the first GOP header begins, 0xFF none
//
// The burst's bytes, frame payloads end to end:
//   GOP        [0x80][fps][first][count][burst pictures][OSD length][OSD]
//              then `count` units, each [size, LEB128][unit], in decode
//              order: an AV1 temporal unit without its temporal delimiter,
//              or a VVC access unit as [size, LEB128][NAL unit]s, no start
//              codes, no SEI, no access unit delimiter
//   ...        more GOPs
//   zeros      to the end of the burst
#ifndef FENNECTV_PROTOCOL_H
#define FENNECTV_PROTOCOL_H

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace fennectv
{

using Bytes = std::vector<uint8_t>;

// The broadcast group FennecTV opens on data2g-host: up to 10 characters,
// never a valid callsign (Data2G docs/broadcast.md, 1).
inline constexpr const char* GROUP = "FENNECTV";

inline constexpr uint8_t PROTOCOL_VERSION = 3;
inline constexpr size_t FRAME_HEADER_BYTES = 4;
// The longest frame every Data2G data mode carries (KISS PACLEN, less one).
inline constexpr size_t MAX_FRAME_BYTES = 255;
inline constexpr int MAX_BURST_FRAMES = 128;
inline constexpr uint8_t NO_GOP_START = 0xFF;
inline constexpr uint8_t GOP_MARK = 0x80;

enum class Codec : uint8_t
{
    Unknown = 0,
    Av1 = 1,
    H264 = 2,  // later
    Vvc = 3,
};

// 8 bits a sample. AV1 profiles: main for Gray and Yuv420, high for
// Yuv444 and Rgb, professional for Yuv422. Not on the air: the AV1
// sequence header says it.
enum class PixelFormat : uint8_t
{
    Gray = 0,
    Yuv420 = 1,
    Yuv422 = 2,
    Yuv444 = 3,
    Rgb = 4,  // GBR planes, 4:4:4, identity matrix
};

//-------------------------------------------------------------------------
// Frames
//-------------------------------------------------------------------------

struct FrameHeader
{
    uint8_t version = PROTOCOL_VERSION;
    Codec codec = Codec::Av1;
    uint8_t burst = 0;
    uint8_t idx = 0;  // below MAX_BURST_FRAMES
    bool last = false;
    uint8_t gopStart = NO_GOP_START;
};

Bytes encodeFrame(const FrameHeader& header, const uint8_t* payload, size_t size);

struct DecodedFrame
{
    FrameHeader header;
    Bytes payload;
};

// Nothing for a frame too short, of another version, or whose GOP start
// lies outside its payload.
std::optional<DecodedFrame> decodeFrame(const Bytes& frame);

//-------------------------------------------------------------------------
// GOPs and bursts
//-------------------------------------------------------------------------

struct GopHeader
{
    uint8_t fps = 0;
    uint8_t first = 0;          // the GOP's first picture, counted in the burst
    uint8_t count = 0;          // its units (one a picture)
    uint8_t burstPictures = 0;  // the burst's pictures, all its GOPs
    std::string osd;            // text for the picture, e.g. "N0CALL"; up to 255 bytes
};

struct Gop
{
    GopHeader header;
    std::vector<Bytes> units;  // as the encoder gave them, one a picture, decode order
};

// A unit as it goes on the air, and back as the decoder takes it.
Bytes toAir(Codec codec, const Bytes& unit);
Bytes fromAir(Codec codec, const Bytes& air);
// What a GOP takes of a burst.
size_t gopBytes(Codec codec, const GopHeader& header, const std::vector<Bytes>& units);

// The burst's frames: the GOPs end to end in `frames` payloads of
// payloadBytes each, zero padded. Nothing when they don't fit.
std::optional<std::vector<Bytes>> packBurst(uint8_t burst, Codec codec, const std::vector<Gop>& gops,
                                            size_t payloadBytes, int frames);

struct ReceivedGop
{
    Codec codec = Codec::Unknown;
    GopHeader header;
    std::vector<Bytes> units;  // for the decoder, decode order
    bool complete = false;     // every unit of header.count
};

// What a receiver makes of a burst: frames by idx, missing ones empty.
// Every GOP whose header arrived comes back with its units up to the
// first lost byte.
std::vector<ReceivedGop> parseBurst(const std::vector<std::optional<DecodedFrame>>& frames);

} // namespace fennectv

#endif
