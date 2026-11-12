// The video codecs, each straight through its own library: AV1 with
// libaom, VVC (H.266) with VVenC and VVdeC. No FFmpeg in between.
//
// One encoder a codec, at the speed past which it gains next to nothing
// (measured): a decoder gives the same pictures whoever made the stream,
// but encoders differ, so FennecTV is tuned and measured against these. Each GOP is coded on its own, from a keyframe.
// With B frames (the default) pictures may be coded out of display order
// within the GOP; the decoder puts them back.
#ifndef FENNECTV_CODEC_H
#define FENNECTV_CODEC_H

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "fennectv/protocol.h"

namespace fennectv
{

struct VideoFormat
{
    int width = 160;
    int height = 120;
    int fps = 5;
    PixelFormat pixels = PixelFormat::Gray;
};

// "gray", "yuv420", "yuv422", "yuv444", "rgb"; nothing for anything else.
std::optional<PixelFormat> pixelFormatNamed(const std::string& name);
const char* pixelFormatName(PixelFormat p);

// "av1", "vvc"; nothing for anything else.
std::optional<Codec> codecNamed(const std::string& name);
const char* codecName(Codec c);

// A picture already at the stream's size and pixel format, 8 bits a
// sample, planes tightly packed: Y (or G), then U and V (or B and R) at
// the format's chroma size; gray has only Y. Scaled once, coded many times.
struct Picture
{
    int width = 0;
    int height = 0;
    PixelFormat pixels = PixelFormat::Gray;
    std::vector<uint8_t> plane[3];

    // The chroma planes' size for a format (0 x 0 for gray).
    static void chromaSize(PixelFormat p, int width, int height, int& cw, int& ch);
};

struct EncoderOptions
{
    bool bFrames = true;  // pictures referring forward too (AV1 alt-ref, VVC random access)
    // Speed: libaom's cpu-used ("0" slowest to "9"), VVenC's preset
    // ("faster" to "slower"); empty for FennecTV's default.
    std::string preset;
    // Straight to the library, after FennecTV's own settings: libaom's
    // aom_codec_set_option names ("tune" = "ssim"), VVenC's expert options
    // ("ALF" = "0").
    std::vector<std::pair<std::string, std::string>> params;
};

class Encoder
{
public:
    virtual ~Encoder() = default;

    // Once a GOP, before encode(): what doesn't depend on the target (a
    // first pass), so the tries that follow share it.
    virtual bool prepare(const std::vector<Picture>& pictures, std::string* error) = 0;

    // What encode() is aimed by: the GOP's bits (the encoder's own rate
    // control: VVenC), or a quantizer in whole steps, the lower the bigger
    // (libaom, whose rate control loses 2 dB at these rates).
    enum class Aim
    {
        Bits,
        Quantizer
    };
    virtual Aim aim() const { return Aim::Bits; }
    // The quantizers there are, for Aim::Quantizer.
    virtual void quantizers(int& lowest, int& highest) const { lowest = highest = 0; }

    // One closed GOP, keyframe first, aimed at `at` (bits for the GOP, or
    // the quantizer): one unit a picture (an AV1 temporal unit, a VVC
    // access unit), in decode order.
    virtual bool encode(const std::vector<Picture>& pictures, double at, std::vector<Bytes>& units,
                        std::string* error) = 0;

    // One coding of the whole GOP.
    struct Try
    {
        std::vector<Bytes> units;
        size_t bytes = 0;   // as sizeOf counts it
        double target = 0;  // what encode() was aimed at
    };

    struct Fit
    {
        int tries = 0;      // encode() calls
        double target = 0;  // what encode() was aimed at for the result
        size_t bytes = 0;   // the block, as sizeOf counts it
        int dropped = 0;    // pictures left out at the end (in decode order) to fit
        bool fits = false;
        // Every coding of the whole GOP, the result's too, smallest first:
        // with the burst's other GOPs, one too big for this GOP's share
        // may still go where another left room.
        std::vector<Try> codings;
    };

    // Codes the GOP so that sizeOf(units) lands between (fill - tolerance)
    // and 100% of capacityBytes: again and again (up to maxTries), each
    // aimed from the tries before (by bits: a secant through bytes against
    // target, in logs; by quantizer: steps by how bytes went with it, never
    // one tried). The fullest that fits wins; if none fits, the smallest
    // loses its last units until it does. learned, if given, is what GOPs
    // before taught (bytes made for bytes asked, or the quantizer chosen;
    // 0 for nothing): the first try is aimed by it, and it comes back
    // updated, so the next GOP starts closer.
    bool encodeToFit(const std::vector<Picture>& pictures, size_t capacityBytes, double fill, double tolerance,
                     int maxTries, const std::function<size_t(const std::vector<Bytes>&)>& sizeOf,
                     std::vector<Bytes>& units, Fit& fit, std::string* error, double* learned = nullptr);

private:
    bool aimByBits(const std::vector<Picture>& pictures, size_t capacityBytes, double aimBytes, double low,
                   int maxTries, const std::function<size_t(const std::vector<Bytes>&)>& sizeOf, Fit& fit,
                   std::string* error, double* rate);
    bool aimByQuantizer(const std::vector<Picture>& pictures, size_t capacityBytes, double aimBytes, double low,
                        int maxTries, const std::function<size_t(const std::vector<Bytes>&)>& sizeOf, Fit& fit,
                        std::string* error, double* learned);
};

class Decoder
{
public:
    virtual ~Decoder() = default;

    struct Image
    {
        int width = 0;
        int height = 0;
        int order = 0;              // display order in the GOP, from 0
        std::vector<uint32_t> rgb;  // 0xffRRGGBB (QImage::Format_RGB32)
    };

    // A new GOP (each begins with a keyframe).
    virtual void reset() = 0;
    // One unit in, in decode order; the pictures ready to show.
    virtual std::vector<Image> decode(const Bytes& unit, std::string* error = nullptr) = 0;
    // The GOP's end (or as far as it came): the pictures still held back.
    virtual std::vector<Image> flush(std::string* error = nullptr) = 0;
};

// Nothing (and why) for a codec or a format the codec can't do.
std::unique_ptr<Encoder> makeEncoder(Codec codec, const VideoFormat& format, const EncoderOptions& options,
                                     std::string* error = nullptr);
std::unique_ptr<Decoder> makeDecoder(Codec codec, std::string* error = nullptr);

//-------------------------------------------------------------------------
// Inside the codec module
//-------------------------------------------------------------------------

namespace detail
{

std::unique_ptr<Encoder> makeAv1Encoder(const VideoFormat& format, const EncoderOptions& options);
std::unique_ptr<Decoder> makeAv1Decoder();
std::unique_ptr<Encoder> makeVvcEncoder(const VideoFormat& format, const EncoderOptions& options);
std::unique_ptr<Decoder> makeVvcDecoder();

// A decoded picture to 0xffRRGGBB. sample(plane, x, y) gives 8-bit samples
// at luma coordinates (the caller subsamples chroma).
std::vector<uint32_t> toRgb(int width, int height, bool monochrome, bool identity, bool fullRange,
                            const std::function<int(int, int, int)>& sample);

void setError(std::string* error, const std::string& what);

} // namespace detail

} // namespace fennectv

#endif
