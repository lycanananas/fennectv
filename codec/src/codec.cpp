#include "fennectv/codec.h"

#include <algorithm>
#include <cmath>

namespace fennectv
{

std::optional<PixelFormat> pixelFormatNamed(const std::string& name)
{
    for (PixelFormat p : {PixelFormat::Gray, PixelFormat::Yuv420, PixelFormat::Yuv422, PixelFormat::Yuv444, PixelFormat::Rgb})
        if (name == pixelFormatName(p)) return p;
    return std::nullopt;
}

const char* pixelFormatName(PixelFormat p)
{
    switch (p)
    {
    case PixelFormat::Gray: return "gray";
    case PixelFormat::Yuv420: return "yuv420";
    case PixelFormat::Yuv422: return "yuv422";
    case PixelFormat::Yuv444: return "yuv444";
    case PixelFormat::Rgb: return "rgb";
    }
    return "?";
}

std::optional<Codec> codecNamed(const std::string& name)
{
    if (name == "av1") return Codec::Av1;
    if (name == "vvc") return Codec::Vvc;
    return std::nullopt;
}

const char* codecName(Codec c)
{
    switch (c)
    {
    case Codec::Av1: return "av1";
    case Codec::Vvc: return "vvc";
    case Codec::H264: return "h264";
    case Codec::Unknown: break;
    }
    return "?";
}

void Picture::chromaSize(PixelFormat p, int width, int height, int& cw, int& ch)
{
    switch (p)
    {
    case PixelFormat::Gray: cw = ch = 0; return;
    case PixelFormat::Yuv420: cw = (width + 1) / 2; ch = (height + 1) / 2; return;
    case PixelFormat::Yuv422: cw = (width + 1) / 2; ch = height; return;
    case PixelFormat::Yuv444:
    case PixelFormat::Rgb: cw = width; ch = height; return;
    }
    cw = ch = 0;
}

bool Encoder::encodeToFit(const std::vector<Picture>& pictures, size_t capacityBytes, double fill, double tolerance,
                          int maxTries, const std::function<size_t(const std::vector<Bytes>&)>& sizeOf,
                          std::vector<Bytes>& units, Fit& fit, std::string* error)
{
    fit = Fit{};
    fill = std::clamp(fill, 0.3, 1.0);
    if (!prepare(pictures, error)) return false;

    const double aim = capacityBytes * fill;
    const double low = capacityBytes * std::max(0.0, fill - tolerance);
    double target = aim * 8.0;
    std::vector<Bytes> bestFit, smallest;
    size_t bestFitBytes = 0, smallestBytes = 0;
    double bestFitTarget = 0, smallestTarget = 0;
    for (int t = 1; t <= std::max(1, maxTries); t++)
    {
        std::vector<Bytes> got;
        if (!encode(pictures, target, got, error) || got.empty()) return false;
        size_t bytes = sizeOf(got);
        fit.tries = t;
        if (bytes <= capacityBytes && bytes > bestFitBytes)
        {
            bestFit = got;
            bestFitBytes = bytes;
            bestFitTarget = target;
        }
        bool shrank = smallest.empty() || bytes < smallestBytes * 0.98;
        if (smallest.empty() || bytes < smallestBytes)
        {
            smallest = std::move(got);
            smallestBytes = bytes;
            smallestTarget = target;
        }
        if (bestFitBytes >= low) break;               // in the window: done
        if (bytes > capacityBytes && !shrank) break;  // the encoder at its coarsest
        // Aim again by how far this one missed.
        target *= aim / (double)bytes;
    }
    if (!bestFit.empty())
    {
        units = std::move(bestFit);
        fit.bytes = bestFitBytes;
        fit.target = bestFitTarget;
        fit.fits = true;
        return true;
    }
    // Nothing fitted: the last units go. What came before them never
    // refers to them, so the rest plays and the gaps hold a picture.
    while (sizeOf(smallest) > capacityBytes && smallest.size() > 1)
    {
        smallest.pop_back();
        fit.dropped++;
    }
    units = std::move(smallest);
    fit.bytes = sizeOf(units);
    fit.target = smallestTarget;
    fit.fits = fit.bytes <= capacityBytes;
    return true;
}

std::unique_ptr<Encoder> makeEncoder(Codec codec, const VideoFormat& format, const EncoderOptions& options,
                                     std::string* error)
{
    switch (codec)
    {
    case Codec::Av1: return detail::makeAv1Encoder(format, options);
    case Codec::Vvc:
        if (format.pixels != PixelFormat::Gray && format.pixels != PixelFormat::Yuv420)
        {
            detail::setError(error, "vvc: gray or yuv420 only (what VVenC codes)");
            return nullptr;
        }
        return detail::makeVvcEncoder(format, options);
    default: break;
    }
    detail::setError(error, std::string("no encoder for ") + codecName(codec));
    return nullptr;
}

std::unique_ptr<Decoder> makeDecoder(Codec codec, std::string* error)
{
    switch (codec)
    {
    case Codec::Av1: return detail::makeAv1Decoder();
    case Codec::Vvc: return detail::makeVvcDecoder();
    default: break;
    }
    detail::setError(error, std::string("no decoder for ") + codecName(codec));
    return nullptr;
}

namespace detail
{

void setError(std::string* error, const std::string& what)
{
    if (error) *error = what;
}

std::vector<uint32_t> toRgb(int width, int height, bool monochrome, bool identity, bool full,
                            const std::function<int(int, int, int)>& sample)
{
    auto clamp8 = [](double v) { return (uint32_t)std::clamp((int)std::lround(v), 0, 255); };
    std::vector<uint32_t> out((size_t)width * height);
    for (int y = 0; y < height; y++)
    {
        for (int x = 0; x < width; x++)
        {
            double r, g, b;
            if (monochrome)
            {
                double Y = sample(0, x, y);
                r = g = b = full ? Y : (Y - 16) * 255.0 / 219.0;
            }
            else if (identity)
            {
                g = sample(0, x, y);
                b = sample(1, x, y);
                r = sample(2, x, y);
            }
            else
            {
                // BT.601
                double Y = sample(0, x, y), U = sample(1, x, y) - 128.0, V = sample(2, x, y) - 128.0;
                if (!full)
                {
                    Y = (Y - 16) * 255.0 / 219.0;
                    U *= 255.0 / 224.0;
                    V *= 255.0 / 224.0;
                }
                r = Y + 1.402 * V;
                g = Y - 0.344136 * U - 0.714136 * V;
                b = Y + 1.772 * U;
            }
            out[(size_t)y * width + x] = 0xff000000u | clamp8(r) << 16 | clamp8(g) << 8 | clamp8(b);
        }
    }
    return out;
}

} // namespace detail

} // namespace fennectv
