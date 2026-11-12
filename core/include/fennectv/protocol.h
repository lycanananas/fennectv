// What FennecTV puts on the air (docs/PLAN.md, "On the air").
//
// One Data2G burst carries a few closed GOPs back to back, the rest of it
// zeros, cut into KISS frames with a 2- or 3-byte header. Data2G's LDPC and
// CRC look after each codeword and nothing more is added: a lost frame loses
// what it carried, and the receiver picks up again at the next GOP that
// starts in a frame it has (the header says where). Every byte counts: no
// field the receiver can do without, and the version is in the group's name
// (Data2G sends that in the control codeword anyway, and masks every CRC
// with it: another version's bursts never reach us).
//
// Frame:  [codec 2 | start 1 | burst 5][last 1 | idx 7]([GOP start])[payload]
//   codec      1 AV1, 3 VVC
//   burst      the burst's number, mod 32
//   idx        the frame's place in the burst; last set on the burst's last
//   start      a GOP begins in this frame: the GOP start byte follows,
//              where in the payload the first one does
// Frames are all as long as the mode's; one with a GOP start carries a
// byte less of payload.
//
// The burst's bytes, frame payloads end to end:
//   GOP        [count][first][fps][burst pictures][OSD length][OSD]
//              then `count` units in decode order, each its own length:
//              AV1   its OBUs less temporal delimiters and padding, each
//                    [(payload length << 3 | kind << 1 | more), LEB128]
//                    ([OBU header])[payload]: kind 0 frame, 1 frame
//                    header, 2 sequence header (header implied), 3 other
//                    (header and extension follow); more clear on the last
//              VVC   its NAL units, each [(length << 1 | more), LEB128]
//                    [the NAL unit less its first byte (0: one layer)],
//                    more clear on the last; no SEI, no access unit
//                    delimiter, no start codes
//              count is never 0; the OSD only in the burst's first GOP,
//              and the sequence's parameters (AV1 sequence header, VVC
//              SPS and PPS) only in its first unit
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
// The version is its last digit: "FENNECTV4" is protocol 4.
inline constexpr const char* GROUP = "FENNECTV4";

// A frame's header without the GOP start byte, and with it.
inline constexpr size_t FRAME_HEADER_BYTES = 2;
inline constexpr size_t FRAME_HEADER_MAX = 3;
inline constexpr int BURST_NUMBERS = 32;
// The longest frame every Data2G data mode carries (KISS PACLEN, less one).
inline constexpr size_t MAX_FRAME_BYTES = 255;
inline constexpr int MAX_BURST_FRAMES = 128;
inline constexpr uint8_t NO_GOP_START = 0xFF;

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
    Codec codec = Codec::Av1;
    uint8_t burst = 0;  // mod BURST_NUMBERS on the air
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

// Nothing for a frame too short, of no codec, or whose GOP start lies
// outside its payload.
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
    // The sequence's parameters go with this GOP (the burst's first);
    // else they're left out and the receiver uses the last it had.
    bool params = true;
};

// The sequence's parameters in a unit (AV1 sequence header OBUs; VVC OPI,
// DCI, VPS, SPS and PPS), as the decoder takes them; empty if none. They
// don't change while a transmission lasts, so they go once a burst, and
// never twice in a GOP.
Bytes paramsOf(Codec codec, const Bytes& unit);
Bytes withoutParams(Codec codec, const Bytes& unit);
// The unit (as fromAir gives it) with the parameters in front of the rest.
Bytes withParams(Codec codec, const Bytes& unit, const Bytes& params);

// What a receiver keeps between bursts: the last parameters a codec sent.
struct ParamCache
{
    Bytes av1, vvc;
    Bytes& of(Codec codec) { return codec == Codec::Vvc ? vvc : av1; }
};

// A unit as it goes on the air, and back as the decoder takes it.
Bytes toAir(Codec codec, const Bytes& unit);
Bytes fromAir(Codec codec, const Bytes& air);
// A GOP's units as they go on the air: the parameters left out of all
// but the first unit, and out of that too without `params`.
std::vector<Bytes> gopToAir(Codec codec, const std::vector<Bytes>& units, bool params);
// What a GOP takes of a burst.
size_t gopBytes(Codec codec, const GopHeader& header, const std::vector<Bytes>& units, bool params = true);

// What a burst of `frames` frames carries with `gops` GOPs in it, at the
// least (frames of payloadBytes, the frame less FRAME_HEADER_BYTES, a byte
// less where a GOP starts).
size_t burstCapacity(size_t payloadBytes, int frames, int gops);

// The burst's frames: the GOPs end to end in `frames` frames of
// FRAME_HEADER_BYTES + payloadBytes each, zero padded. Nothing when they
// don't fit.
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
// first lost byte. A GOP that came without the parameters gets the last
// ones the cache (if given; else this burst) has; one that brings them
// updates it.
std::vector<ReceivedGop> parseBurst(const std::vector<std::optional<DecodedFrame>>& frames,
                                    ParamCache* cache = nullptr);

} // namespace fennectv

#endif
