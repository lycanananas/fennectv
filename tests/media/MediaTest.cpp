// The test film through the whole chain, no host: FFmpeg reads and scales
// it once, each codec (AV1, VVC; with B frames and without) codes 3-second
// GOPs that together fill a burst, a frame is lost in the middle of one,
// and the decoder gives back every picture that arrived, in its place: the
// cut GOP up to the loss, the others whole. Every colour mode is coded and
// decoded too.
//
//   fennectv_media_test VIDEO [START_SECONDS]
// Exits 77 (skipped) without the video.
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <future>
#include <set>

#include "Check.h"
#include "fennectv/budget.h"
#include "fennectv/codec.h"
#include "fennectv/media.h"
#include "fennectv/protocol.h"

using namespace fennectv;

namespace
{

std::vector<Picture> read(const std::string& video, double start, const VideoFormat& fmt, int count)
{
    Source src;
    Source::Options so;
    so.url = video;
    so.start = start;
    std::string err;
    CHECK(src.open(so, fmt, &err));
    std::vector<Picture> pics;
    while ((int)pics.size() < count)
    {
        auto p = src.nextPicture();
        if (!p) break;
        pics.push_back(std::move(*p));
    }
    return pics;
}

// The unit is a random access point: AV1, a sequence header and a key
// frame (frame header: show_existing_frame 0, frame_type KEY); VVC, an SPS
// and an IDR slice.
bool startsClean(Codec codec, const Bytes& unit)
{
    Bytes air = toAir(codec, unit);
    bool seq = false, key = false;
    for (size_t pos = 0; pos < air.size();)
    {
        size_t len = 0, at = pos;
        int type = 0;
        if (codec == Codec::Av1)
        {
            uint8_t h = air[pos];
            type = (h >> 3) & 0x0F;
            at = pos + 1 + ((h >> 2) & 1);
        }
        for (int i = 0;; i++)
        {
            uint8_t b = air[at++];
            len |= (size_t)(b & 0x7F) << (7 * i);
            if (!(b & 0x80)) break;
        }
        if (codec == Codec::Av1)
        {
            if (type == 1) seq = true;
            // reduced_still_picture_header is 0 here: the first bits are
            // show_existing_frame (1) and frame_type (2), KEY_FRAME = 0
            if ((type == 6 || type == 3) && len > 0) key = key || (air[at] & 0xE0) == 0;
        }
        else
        {
            int nal = air[at + 1] >> 3;
            if (nal == 15) seq = true;
            if (nal == 7 || nal == 8) key = true;  // IDR_W_RADL, IDR_N_LP
        }
        pos = at + len;
    }
    return seq && key;
}

// Decodes a GOP's units; the display orders that came out.
std::vector<int> decodeGop(Decoder& dec, const std::vector<Bytes>& units, const VideoFormat& fmt, std::string& err)
{
    std::vector<int> orders;
    dec.reset();
    auto take = [&](std::vector<Decoder::Image> imgs) {
        for (auto& im : imgs)
        {
            CHECK(im.width == fmt.width && im.height == fmt.height);
            orders.push_back(im.order);
        }
    };
    for (const Bytes& u : units)
    {
        take(dec.decode(u, &err));
        if (!err.empty()) return orders;
    }
    take(dec.flush(&err));
    return orders;
}

void gear(const std::string& video, double start, const ModeInfo& mode, VideoFormat fmt, int bursts, Codec codec,
          bool bFrames)
{
    auto plan = planBurst(mode);
    CHECK(plan.has_value());
    const double burstSeconds = plan->airSeconds + 0.3;
    const int perBurst = (int)(burstSeconds * fmt.fps + 0.5);
    const int nGops = std::max(1, (int)(perBurst / (3.0 * fmt.fps) + 0.5));
    std::vector<int> split((size_t)nGops, perBurst / nGops);
    for (int i = 0; i < perBurst % nGops; i++) split[(size_t)(nGops - 1 - i)]++;
    auto all = read(video, start, fmt, perBurst * bursts);
    CHECK_EQ((int)all.size(), perBurst * bursts);
    const size_t P = plan->framing.payloadBytes;
    EncoderOptions opts;
    opts.bFrames = bFrames;

    for (int b = 0; b < bursts; b++)
    {
        auto t0 = std::chrono::steady_clock::now();
        std::vector<std::future<std::pair<Gop, Encoder::Fit>>> coding;
        int first = 0;
        for (int g = 0; g < nGops; g++)
        {
            GopHeader h;
            h.fps = (uint8_t)fmt.fps;
            h.first = (uint8_t)first;
            h.burstPictures = (uint8_t)perBurst;
            h.osd = "N0CALL";
            std::vector<Picture> pics(all.begin() + b * perBurst + first,
                                      all.begin() + b * perBurst + first + split[(size_t)g]);
            const size_t share = plan->capacityBytes() * split[(size_t)g] / perBurst;
            first += split[(size_t)g];
            coding.push_back(std::async(std::launch::async, [=] {
                auto enc = makeEncoder(codec, fmt, opts);
                Gop gop;
                gop.header = h;
                Encoder::Fit fit;
                std::string err;
                CHECK(enc->encodeToFit(
                    pics, share, 0.95, 0.05, 6, [&](const std::vector<Bytes>& p) { return gopBytes(codec, h, p); },
                    gop.units, fit, &err));
                if (!err.empty()) std::printf("  %s\n", err.c_str());
                gop.header.count = (uint8_t)gop.units.size();
                return std::make_pair(std::move(gop), fit);
            }));
        }
        std::vector<Gop> gops;
        size_t bytes = 0;
        int kept = 0, tries = 0;
        for (auto& c : coding)
        {
            auto [gop, fit] = c.get();
            CHECK(fit.fits);
            bytes += gopBytes(codec, gop.header, gop.units);
            kept += (int)gop.units.size();
            tries += fit.tries;
            gops.push_back(std::move(gop));
        }
        double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        std::printf("%s %s%s %dx%d@%d: burst %d -> %zu GOPs, %zu B of %zu (%.1f%%), %d tries, %d/%d pictures, "
                    "coded in %.1f s\n",
                    mode.name.c_str(), codecName(codec), bFrames ? "+B" : "", fmt.width, fmt.height, fmt.fps, b,
                    gops.size(), bytes, plan->capacityBytes(), 100.0 * bytes / plan->capacityBytes(), tries, kept,
                    perBurst, secs);
        CHECK(bytes <= plan->capacityBytes());
        CHECK(bytes * 100 >= plan->capacityBytes() * 85);
        CHECK(kept * 10 >= perBurst * 9);  // a hard GOP loses a few at most

        // Every picture of every GOP comes back, each in its place once,
        // and every GOP starts where a receiver can start: a VVC IDR, an
        // AV1 key frame with the sequence header.
        auto dec = makeDecoder(codec);
        for (const Gop& g : gops)
        {
            CHECK(startsClean(codec, g.units[0]));
            std::string err;
            std::vector<Bytes> units;
            for (const Bytes& u : g.units) units.push_back(fromAir(codec, toAir(codec, u)));
            auto orders = decodeGop(*dec, units, fmt, err);
            CHECK(err.empty());
            CHECK_EQ(orders.size(), g.units.size());
            CHECK_EQ(std::set<int>(orders.begin(), orders.end()).size(), orders.size());
        }

        auto frames = packBurst((uint8_t)b, codec, gops, P, plan->frames);
        CHECK(frames.has_value());
        if (!frames) continue;
        // Lose the frame in the middle of GOP 1.
        size_t g0 = gopBytes(codec, gops[0].header, gops[0].units);
        size_t g1 = gopBytes(codec, gops[1].header, gops[1].units);
        const int lost = (int)((g0 + g1 * 2 / 3) / P);
        std::vector<std::optional<DecodedFrame>> got(frames->size());
        for (size_t i = 0; i < frames->size(); i++)
            if ((int)i != lost) got[i] = decodeFrame((*frames)[i]);
        auto received = parseBurst(got);
        CHECK(received.size() >= gops.size() - 1);
        size_t shown = 0, sent = 0;
        for (auto& g : gops) sent += g.units.size();
        for (const ReceivedGop& r : received)
        {
            std::string err;
            auto orders = decodeGop(*dec, r.units, fmt, err);
            CHECK(err.empty());
            CHECK_EQ(orders.size(), r.units.size());
            if (r.header.first != gops[1].header.first) CHECK(r.complete);
            shown += orders.size();
        }
        std::printf("   frame %d of %zu lost: %zu/%zu pictures decoded\n", lost, frames->size(), shown, sent);
        CHECK(shown < sent);
        CHECK(shown + split[1] >= sent);  // only GOP 1 lost anything
    }
}

void colourModes(const std::string& video, double start, Codec codec)
{
    for (PixelFormat px : {PixelFormat::Gray, PixelFormat::Yuv420, PixelFormat::Yuv422, PixelFormat::Yuv444, PixelFormat::Rgb})
    {
        VideoFormat fmt;
        fmt.pixels = px;
        std::string err;
        auto enc = makeEncoder(codec, fmt, EncoderOptions{}, &err);
        if (codec == Codec::Vvc && px != PixelFormat::Gray && px != PixelFormat::Yuv420)
        {
            CHECK(!enc);  // VVenC codes 4:0:0 and 4:2:0 only
            continue;
        }
        CHECK(enc != nullptr);
        if (!enc) continue;
        auto pics = read(video, start, fmt, 10);
        CHECK_EQ(pics.size(), (size_t)10);
        int cw = 0, ch = 0;
        Picture::chromaSize(px, fmt.width, fmt.height, cw, ch);
        CHECK_EQ(pics[0].plane[1].size(), (size_t)cw * ch);
        std::vector<Bytes> units;
        CHECK(enc->prepare(pics, &err));
        CHECK(enc->encode(pics, 20000.0, units, &err));
        CHECK_EQ(units.size(), (size_t)10);
        auto dec = makeDecoder(codec);
        dec->reset();
        size_t shown = 0;
        uint32_t mid = 0;
        auto take = [&](std::vector<Decoder::Image> imgs) {
            for (auto& im : imgs)
            {
                shown++;
                mid = im.rgb[im.rgb.size() / 2 + im.width / 2];
            }
        };
        for (auto& u : units) take(dec->decode(u, &err));
        take(dec->flush(&err));
        size_t bytes = 0;
        for (auto& u : units) bytes += u.size();
        std::printf("%s %s: 10 pictures, %zu B, decoded %zu, a middle pixel #%06x%s\n", codecName(codec),
                    pixelFormatName(px), bytes, shown, mid & 0xffffff, err.empty() ? "" : (" " + err).c_str());
        CHECK_EQ(shown, (size_t)10);
        if (px == PixelFormat::Gray)
        {
            uint8_t r = mid >> 16, g = mid >> 8, b = mid;
            CHECK(r == g && g == b);  // gray stays gray
        }
    }
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 2 || !std::ifstream(argv[1]).good())
    {
        std::printf("no test video: skipped\n");
        return 77;
    }
    std::string video = argv[1];
    double start = argc > 2 ? std::atof(argv[2]) : 120.0;

    VideoFormat z;  // Z8k defaults: 160x120 gray, 5 fps
    VideoFormat q;  // Q3k defaults: 96x72 gray, 3 fps
    q.width = 96;
    q.height = 72;
    q.fps = 3;
    for (Codec codec : {Codec::Av1, Codec::Vvc})
    {
        colourModes(video, start, codec);
        for (bool bFrames : {true, false})
        {
            gear(video, start, z8k(), z, 2, codec, bFrames);
            gear(video, start, q3k(), q, 2, codec, bFrames);
        }
    }
    return ragtime_test::finish();
}
