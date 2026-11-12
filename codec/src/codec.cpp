#include "fennectv/codec.h"

#include <algorithm>
#include <cmath>
#include <map>

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
                          std::vector<Bytes>& units, Fit& fit, std::string* error, double* learned)
{
    fit = Fit{};
    fill = std::clamp(fill, 0.3, 1.0);
    if (!prepare(pictures, error)) return false;

    const double aim = capacityBytes * fill;
    const double low = capacityBytes * std::max(0.0, fill - tolerance);
    const bool ok = this->aim() == Aim::Quantizer
                        ? aimByQuantizer(pictures, capacityBytes, aim, low, maxTries, sizeOf, fit, error, learned)
                        : aimByBits(pictures, capacityBytes, aim, low, maxTries, sizeOf, fit, error, learned);
    if (!ok) return false;
    std::vector<Try>& tries = fit.codings;
    if (tries.empty()) return false;
    std::sort(tries.begin(), tries.end(), [](const Try& x, const Try& y) { return x.bytes < y.bytes; });
    const Try* best = nullptr;  // the fullest that fits
    for (const Try& c : tries)
        if (c.bytes <= capacityBytes) best = &c;
    if (best)
    {
        units = best->units;
        fit.bytes = best->bytes;
        fit.target = best->target;
        fit.fits = true;
    }
    else
    {
        // Nothing fitted: the last units go. What came before them never
        // refers to them, so the rest plays and the gaps hold a picture.
        units = tries.front().units;
        fit.target = tries.front().target;
        while (sizeOf(units) > capacityBytes && units.size() > 1)
        {
            units.pop_back();
            fit.dropped++;
        }
        fit.bytes = sizeOf(units);
        fit.fits = fit.bytes <= capacityBytes;
    }
    if (learned && this->aim() == Aim::Quantizer)
        *learned = fit.target;  // the quantizer that went
    else if (learned && fit.target > 0)
    {
        // from the try nearest the aim, whole GOP
        const Try* near = &tries.front();
        for (const Try& c : tries)
            if (std::fabs(std::log(c.bytes / aim)) < std::fabs(std::log(near->bytes / aim))) near = &c;
        *learned = near->bytes * 8.0 / near->target;
    }
    return true;
}

bool Encoder::aimByBits(const std::vector<Picture>& pictures, size_t capacityBytes, double aim, double low,
                        int maxTries, const std::function<size_t(const std::vector<Bytes>&)>& sizeOf, Fit& fit,
                        std::string* error, double* rate)
{
    // Bytes made for bytes asked: what the GOPs before taught, else 1.
    double target = aim * 8.0 / (rate && *rate > 0.05 && *rate < 20 ? *rate : 1.0);
    std::vector<Try>& tries = fit.codings;
    int smallest = -1, bestFit = -1;
    for (int t = 1; t <= std::max(1, maxTries); t++)
    {
        Try got;
        got.target = target;
        if (!encode(pictures, target, got.units, error) || got.units.empty()) return false;
        got.bytes = sizeOf(got.units);
        fit.tries = t;
        const bool shrank = smallest < 0 || got.bytes < tries[(size_t)smallest].bytes * 0.98;
        tries.push_back(std::move(got));
        const int i = (int)tries.size() - 1;
        const Try& now = tries.back();
        if (now.bytes <= capacityBytes && (bestFit < 0 || now.bytes > tries[(size_t)bestFit].bytes)) bestFit = i;
        if (smallest < 0 || now.bytes < tries[(size_t)smallest].bytes) smallest = i;
        if (bestFit >= 0 && tries[(size_t)bestFit].bytes >= low) break;  // in the window: done
        if (now.bytes > capacityBytes && !shrank) break;                  // the encoder at its coarsest
        // Aim again. Bytes go roughly as a power of the target: its slope
        // from the two nearest tries (each side of the aim if there are),
        // else 1; kept sane, rate control being what it is.
        double slope = 1.0;
        int below = -1, above = -1;
        for (int k = 0; k < (int)tries.size(); k++)
        {
            const Try& c = tries[(size_t)k];
            if (c.bytes <= aim && (below < 0 || c.bytes > tries[(size_t)below].bytes)) below = k;
            if (c.bytes > aim && (above < 0 || c.bytes < tries[(size_t)above].bytes)) above = k;
        }
        int p = below, q = above;
        if (p < 0 || q < 0)
        {
            // all on one side: the last two
            p = i;
            q = i > 0 ? i - 1 : -1;
        }
        if (q >= 0 && p != q && tries[(size_t)p].target != tries[(size_t)q].target &&
            tries[(size_t)p].bytes != tries[(size_t)q].bytes)
        {
            const Try &x = tries[(size_t)p], &y = tries[(size_t)q];
            slope = std::log((double)x.bytes / y.bytes) / std::log(x.target / y.target);
            slope = std::clamp(slope, 0.25, 2.0);
        }
        // from the nearer of the tries the slope came from
        const Try& from = std::fabs(std::log(tries[(size_t)p].bytes / aim)) <=
                                  (q >= 0 ? std::fabs(std::log(tries[(size_t)q].bytes / aim)) : 1e9)
                              ? tries[(size_t)p]
                              : tries[(size_t)q];
        double next = from.target * std::pow(aim / (double)from.bytes, 1.0 / slope);
        // never ask what a try already answered
        if (below >= 0 && above >= 0)
            next = std::clamp(next, tries[(size_t)below].target, tries[(size_t)above].target);
        target = next;
    }
    return true;
}

bool Encoder::aimByQuantizer(const std::vector<Picture>& pictures, size_t capacityBytes, double aimBytes, double low,
                             int maxTries, const std::function<size_t(const std::vector<Bytes>&)>& sizeOf, Fit& fit,
                             std::string* error, double* learned)
{
    int lowest = 0, highest = 0;
    quantizers(lowest, highest);
    std::map<int, size_t> tried;  // quantizer: bytes
    int q = learned && *learned >= lowest && *learned <= highest ? (int)std::lround(*learned) : (lowest + highest) / 2;
    for (int t = 1; t <= std::max(1, maxTries); t++)
    {
        Try got;
        got.target = q;
        if (!encode(pictures, q, got.units, error) || got.units.empty()) return false;
        got.bytes = sizeOf(got.units);
        fit.tries = t;
        tried[q] = got.bytes;
        const size_t bytes = got.bytes;
        fit.codings.push_back(std::move(got));
        if (bytes <= capacityBytes && bytes >= low) break;  // in the window: done
        // The finest that fits and the coarsest that doesn't: next to each
        // other, nothing in between to try.
        int fits = highest + 1, over = lowest - 1;
        for (auto [k, b] : tried)
        {
            if (b <= capacityBytes) fits = std::min(fits, k);
            if (b > capacityBytes) over = std::max(over, k);
        }
        if (fits - over <= 1) break;
        // Bytes go roughly as exp(-slope * q): the slope from the two
        // tries nearest the aim, else a usual one.
        double slope = 0.07;
        if (tried.size() >= 2)
        {
            std::vector<std::pair<double, int>> near;
            for (auto [k, b] : tried) near.push_back({std::fabs(std::log(b / aimBytes)), k});
            std::sort(near.begin(), near.end());
            const int a = near[0].second, b = near[1].second;
            if (tried[a] != tried[b])
                slope = std::clamp(std::log((double)tried[a] / tried[b]) / (b - a), 0.02, 0.4);
        }
        int next = q + (int)std::lround(std::log(bytes / aimBytes) / slope);
        if (bytes > capacityBytes) next = std::max(next, q + 1);
        if (bytes < low) next = std::min(next, q - 1);
        next = std::clamp(next, over + 1, fits - 1);
        next = std::clamp(next, lowest, highest);
        if (tried.count(next))
        {
            // the nearest untried within the bracket
            int pick = -1;
            for (int d = 1; d <= highest - lowest && pick < 0; d++)
                for (int c : {next - d, next + d})
                    if (pick < 0 && c > over && c < fits && c >= lowest && c <= highest && !tried.count(c)) pick = c;
            if (pick < 0) break;
            next = pick;
        }
        q = next;
    }
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
