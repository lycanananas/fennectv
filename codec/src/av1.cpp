// AV1 through libaom: good quality, cpu-used 0, two-pass.
#include "fennectv/codec.h"

#include <aom/aom_decoder.h>
#include <aom/aom_encoder.h>
#include <aom/aomcx.h>
#include <aom/aomdx.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace fennectv
{

namespace
{

// libaom takes its target in whole kbps, far too coarse at 2-8 kbps. The
// encoder is told each picture lasts 1/TICKS s instead of 1/fps, so the
// same bits per GOP read as a large, finely set kbps figure.
constexpr int TICKS = 1000;

using detail::setError;

std::string aomError(aom_codec_ctx_t* c, const char* what)
{
    std::string s = std::string(what) + ": " + aom_codec_error(c);
    if (const char* d = aom_codec_error_detail(c)) s += std::string(" (") + d + ")";
    return s;
}

aom_img_fmt_t imageFormat(PixelFormat p)
{
    switch (p)
    {
    case PixelFormat::Gray:
    case PixelFormat::Yuv420: return AOM_IMG_FMT_I420;
    case PixelFormat::Yuv422: return AOM_IMG_FMT_I422;
    case PixelFormat::Yuv444:
    case PixelFormat::Rgb: return AOM_IMG_FMT_I444;
    }
    return AOM_IMG_FMT_I420;
}

unsigned profile(PixelFormat p)
{
    switch (p)
    {
    case PixelFormat::Gray:
    case PixelFormat::Yuv420: return 0;  // main
    case PixelFormat::Yuv444:
    case PixelFormat::Rgb: return 1;  // high
    case PixelFormat::Yuv422: return 2;  // professional
    }
    return 0;
}

} // namespace

//-------------------------------------------------------------------------
// Encoder
//-------------------------------------------------------------------------

namespace
{

class Av1Encoder : public Encoder
{
public:
    Av1Encoder(const VideoFormat& format, const EncoderOptions& options) : format_(format), options_(options) {}

    bool prepare(const std::vector<Picture>& pictures, std::string* error) override
    {
        stats_.clear();
        return run(pictures, 1, nullptr, 1000.0 * pictures.size(), nullptr, &stats_, error);
    }

    bool encode(const std::vector<Picture>& pictures, double bitsForGop, std::vector<Bytes>& units,
                std::string* error) override
    {
        if (stats_.empty() && !prepare(pictures, error)) return false;
        return run(pictures, 2, &stats_, bitsForGop, &units, nullptr, error) && !units.empty();
    }

private:
    bool run(const std::vector<Picture>& pictures, int pass, const std::vector<uint8_t>* statsIn, double bitsForGop,
             std::vector<Bytes>* packets, std::vector<uint8_t>* statsOut, std::string* error);

    VideoFormat format_;
    EncoderOptions options_;
    std::vector<uint8_t> stats_;
};

bool Av1Encoder::run(const std::vector<Picture>& pictures, int pass, const std::vector<uint8_t>* statsIn,
                     double bitsForGop, std::vector<Bytes>* packets, std::vector<uint8_t>* statsOut,
                     std::string* error)
{
    if (packets) packets->clear();
    if (statsOut) statsOut->clear();
    if (pictures.empty())
    {
        setError(error, "no pictures");
        return false;
    }
    aom_codec_iface_t* iface = aom_codec_av1_cx();
    aom_codec_enc_cfg_t cfg;
    if (aom_codec_enc_config_default(iface, &cfg, AOM_USAGE_GOOD_QUALITY) != AOM_CODEC_OK)
    {
        setError(error, "libaom: no default configuration");
        return false;
    }
    const PixelFormat px = format_.pixels;
    const int n = (int)pictures.size();
    cfg.g_w = (unsigned)format_.width;
    cfg.g_h = (unsigned)format_.height;
    cfg.g_profile = profile(px);
    cfg.monochrome = px == PixelFormat::Gray;
    cfg.g_timebase = {1, TICKS};
    cfg.g_threads = 1;         // GOPs run side by side instead
    // B frames: libaom looks ahead over the GOP and codes alt-ref pictures
    // (shown later) that the pictures before them refer to.
    cfg.g_lag_in_frames = options_.bFrames ? (unsigned)std::min(n, 35) : 0;
    cfg.g_pass = pass == 1 ? AOM_RC_FIRST_PASS : AOM_RC_LAST_PASS;
    cfg.rc_end_usage = AOM_VBR;
    // bits for the GOP over the n ticks it lasts, in kbps
    double kbps = bitsForGop * TICKS / n / 1000.0;
    cfg.rc_target_bitrate = (unsigned)std::max(1.0, std::round(kbps));
    cfg.rc_undershoot_pct = 100;
    cfg.rc_overshoot_pct = 10;
    // The rate buffer spans the GOP (n ticks of a millisecond).
    cfg.rc_buf_sz = cfg.rc_buf_initial_sz = cfg.rc_buf_optimal_sz = (unsigned)n;
    cfg.kf_mode = AOM_KF_DISABLED;  // one keyframe, the first, forced below
    cfg.kf_min_dist = cfg.kf_max_dist = (unsigned)n;
    if (statsIn)
    {
        cfg.rc_twopass_stats_in.buf = const_cast<uint8_t*>(statsIn->data());
        cfg.rc_twopass_stats_in.sz = statsIn->size();
    }

    aom_codec_ctx_t ctx;
    if (aom_codec_enc_init(&ctx, iface, &cfg, 0) != AOM_CODEC_OK)
    {
        setError(error, aomError(&ctx, "libaom init"));
        return false;
    }
    aom_codec_control(&ctx, AOME_SET_CPUUSED, 0);  // the slowest: we have the CPU
    aom_codec_control(&ctx, AOME_SET_ENABLEAUTOALTREF, options_.bFrames ? 1u : 0u);
    if (px == PixelFormat::Rgb)
    {
        aom_codec_control(&ctx, AV1E_SET_COLOR_PRIMARIES, (int)AOM_CICP_CP_BT_709);
        aom_codec_control(&ctx, AV1E_SET_TRANSFER_CHARACTERISTICS, (int)AOM_CICP_TC_SRGB);
        aom_codec_control(&ctx, AV1E_SET_MATRIX_COEFFICIENTS, (int)AOM_CICP_MC_IDENTITY);
        aom_codec_control(&ctx, AV1E_SET_COLOR_RANGE, (int)AOM_CR_FULL_RANGE);
    }
    else
    {
        aom_codec_control(&ctx, AV1E_SET_COLOR_RANGE, (int)AOM_CR_STUDIO_RANGE);
    }

    aom_image_t img;
    if (!aom_img_alloc(&img, imageFormat(px), (unsigned)format_.width, (unsigned)format_.height, 1))
    {
        aom_codec_destroy(&ctx);
        setError(error, "libaom: no image");
        return false;
    }
    img.monochrome = px == PixelFormat::Gray;
    int cw = 0, ch = 0;
    Picture::chromaSize(px, format_.width, format_.height, cw, ch);
    if (px == PixelFormat::Gray)
    {
        // libaom still reads 4:2:0 chroma; neutral grey
        int gw = (format_.width + 1) / 2, gh = (format_.height + 1) / 2;
        for (int p = 1; p < 3; p++)
            for (int y = 0; y < gh; y++) std::memset(img.planes[p] + y * img.stride[p], 128, (size_t)gw);
    }

    auto collect = [&]() {
        aom_codec_iter_t it = nullptr;
        while (const aom_codec_cx_pkt_t* pkt = aom_codec_get_cx_data(&ctx, &it))
        {
            if (pkt->kind == AOM_CODEC_CX_FRAME_PKT && packets)
            {
                const uint8_t* b = static_cast<const uint8_t*>(pkt->data.frame.buf);
                packets->emplace_back(b, b + pkt->data.frame.sz);
            }
            else if (pkt->kind == AOM_CODEC_STATS_PKT && statsOut)
            {
                const uint8_t* b = static_cast<const uint8_t*>(pkt->data.twopass_stats.buf);
                statsOut->insert(statsOut->end(), b, b + pkt->data.twopass_stats.sz);
            }
        }
    };

    bool ok = true;
    for (int i = 0; i < n && ok; i++)
    {
        const Picture& pic = pictures[(size_t)i];
        if (pic.width != format_.width || pic.height != format_.height || pic.pixels != px)
        {
            setError(error, "a picture of the wrong size or format");
            ok = false;
            break;
        }
        for (int y = 0; y < format_.height; y++)
            std::memcpy(img.planes[0] + y * img.stride[0], pic.plane[0].data() + (size_t)y * format_.width,
                        (size_t)format_.width);
        for (int p = 1; p < 3 && cw > 0; p++)
            for (int y = 0; y < ch; y++)
                std::memcpy(img.planes[p] + y * img.stride[p], pic.plane[p].data() + (size_t)y * cw, (size_t)cw);
        aom_enc_frame_flags_t flags = i == 0 ? AOM_EFLAG_FORCE_KF : 0;
        if (aom_codec_encode(&ctx, &img, i, 1, flags) != AOM_CODEC_OK)
        {
            setError(error, aomError(&ctx, "libaom encode"));
            ok = false;
        }
        collect();
    }
    // Flush until nothing more comes.
    while (ok)
    {
        if (aom_codec_encode(&ctx, nullptr, -1, 1, 0) != AOM_CODEC_OK)
        {
            setError(error, aomError(&ctx, "libaom flush"));
            ok = false;
            break;
        }
        size_t before = (packets ? packets->size() : 0) + (statsOut ? statsOut->size() : 0);
        collect();
        size_t after = (packets ? packets->size() : 0) + (statsOut ? statsOut->size() : 0);
        if (after == before) break;
    }
    aom_img_free(&img);
    aom_codec_destroy(&ctx);
    return ok;
}

//-------------------------------------------------------------------------
// Decoder
//-------------------------------------------------------------------------

// One decoded image, from what it says about itself.
Decoder::Image imageOf(const aom_image_t* img, int order)
{
    Decoder::Image out;
    out.width = (int)img->d_w;
    out.height = (int)img->d_h;
    out.order = order;
    const bool high = (img->fmt & AOM_IMG_FMT_HIGHBITDEPTH) != 0;
    out.rgb = detail::toRgb(out.width, out.height, img->monochrome, img->mc == AOM_CICP_MC_IDENTITY,
                            img->range == AOM_CR_FULL_RANGE, [&](int p, int x, int y) -> int {
                                int sx = p ? x >> img->x_chroma_shift : x;
                                int sy = p ? y >> img->y_chroma_shift : y;
                                const uint8_t* row = img->planes[p] + (ptrdiff_t)sy * img->stride[p];
                                if (high) return reinterpret_cast<const uint16_t*>(row)[sx] >> (img->bit_depth - 8);
                                return row[sx];
                            });
    return out;
}

// Each temporal unit shows one picture, in display order: the alt-refs
// travel hidden and are shown later from the decoder's buffers.
class Av1Decoder : public Decoder
{
public:
    ~Av1Decoder() override
    {
        if (open_) aom_codec_destroy(&ctx_);
    }

    void reset() override
    {
        start(nullptr);
        shown_ = 0;
    }

    std::vector<Image> decode(const Bytes& unit, std::string* error) override
    {
        std::vector<Image> out;
        if (!open_ && !start(error)) return out;
        if (aom_codec_decode(&ctx_, unit.data(), unit.size(), nullptr) != AOM_CODEC_OK)
        {
            setError(error, aomError(&ctx_, "libaom decode"));
            return out;
        }
        aom_codec_iter_t it = nullptr;
        while (aom_image_t* img = aom_codec_get_frame(&ctx_, &it)) out.push_back(imageOf(img, shown_++));
        return out;
    }

    std::vector<Image> flush(std::string*) override { return {}; }

private:
    bool start(std::string* error)
    {
        if (open_) aom_codec_destroy(&ctx_);
        aom_codec_dec_cfg_t cfg{};
        cfg.threads = 1;
        cfg.allow_lowbitdepth = 1;
        open_ = aom_codec_dec_init(&ctx_, aom_codec_av1_dx(), &cfg, 0) == AOM_CODEC_OK;
        if (!open_) setError(error, "libaom decoder init failed");
        return open_;
    }

    aom_codec_ctx_t ctx_{};
    bool open_ = false;
    int shown_ = 0;
};

} // namespace

namespace detail
{

std::unique_ptr<Encoder> makeAv1Encoder(const VideoFormat& format, const EncoderOptions& options)
{
    return std::make_unique<Av1Encoder>(format, options);
}

std::unique_ptr<Decoder> makeAv1Decoder() { return std::make_unique<Av1Decoder>(); }

} // namespace detail

} // namespace fennectv
