// VVC (H.266) through Fraunhofer's VVenC and VVdeC: preset slower, two-pass
// rate control, 10 bits inside. B frames: random access with a closed
// GOP (an IDR first, pictures coded out of order in hierarchical groups);
// without them, low delay (forward references only).
#include <vvdec/vvdec.h>
#include <vvenc/vvenc.h>

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstring>

#include "fennectv/codec.h"

namespace fennectv
{

namespace
{

using detail::setError;

vvencChromaFormat chromaOf(PixelFormat p)
{
    switch (p)
    {
    case PixelFormat::Gray: return VVENC_CHROMA_400;
    case PixelFormat::Yuv420: return VVENC_CHROMA_420;
    case PixelFormat::Yuv422: return VVENC_CHROMA_422;
    default: return VVENC_CHROMA_444;
    }
}

void quiet(void*, int, const char*, va_list) {}

class VvcEncoder : public Encoder
{
public:
    VvcEncoder(const VideoFormat& format, const EncoderOptions& options) : format_(format), options_(options) {}

    // VVenC's two passes run in one encoder, so nothing to share between tries.
    bool prepare(const std::vector<Picture>&, std::string*) override { return true; }

    bool encode(const std::vector<Picture>& pictures, double bitsForGop, std::vector<Bytes>& units,
                std::string* error) override
    {
        units.clear();
        const int n = (int)pictures.size();
        const int w = format_.width, h = format_.height;
        if (n == 0)
        {
            setError(error, "no pictures");
            return false;
        }
        const double seconds = n / (double)format_.fps;
        const int bps = std::max(100, (int)std::lround(bitsForGop / seconds));

        vvenc_config c;
        vvenc_init_default(&c, w, h, format_.fps, bps, 32, VVENC_SLOWER);
        vvenc_set_msg_callback(&c, nullptr, quiet);
        c.m_verbosity = VVENC_SILENT;
        c.m_internChromaFormat = chromaOf(format_.pixels);
        // VVenC takes its samples at the bit depth it codes at: 8-bit
        // pictures go in shifted up to 10 bits.
        c.m_inputBitDepth[0] = c.m_inputBitDepth[1] = 10;
        c.m_internalBitDepth[0] = c.m_internalBitDepth[1] = 10;
        c.m_numThreads = 1;  // GOPs run side by side instead
        c.m_framesToBeEncoded = n;
        c.m_RCNumPasses = 2;
        c.m_poc0idr = true;
        c.m_IntraPeriod = 256;  // past the GOP: its first picture is the only I
        if (options_.bFrames)
        {
            c.m_picReordering = true;
            c.m_GOPSize = 16;
            c.m_DecodingRefreshType = VVENC_DRT_IDR;
        }
        else
        {
            c.m_picReordering = false;
            c.m_GOPSize = 8;
            c.m_DecodingRefreshType = VVENC_DRT_NONE;
        }

        vvencEncoder* enc = vvenc_encoder_create();
        int rc = vvenc_encoder_open(enc, &c);
        if (rc != 0)
        {
            setError(error, std::string("vvenc: ") + vvenc_get_error_msg(rc) + " " + vvenc_get_last_error(enc));
            vvenc_encoder_close(enc);
            return false;
        }
        vvencAccessUnit* au = vvenc_accessUnit_alloc();
        vvenc_accessUnit_alloc_payload(au, std::max(1 << 16, w * h * 3));
        vvencYUVBuffer* yuv = vvenc_YUVBuffer_alloc();
        vvenc_YUVBuffer_alloc_buffer(yuv, c.m_internChromaFormat, w, h);
        int cw = 0, ch = 0;
        Picture::chromaSize(format_.pixels, w, h, cw, ch);
        const int64_t tick = c.m_TicksPerSecond > 0 ? c.m_TicksPerSecond / format_.fps : 1;

        bool ok = true;
        for (int pass = 0; pass < 2 && ok; pass++)
        {
            if ((rc = vvenc_init_pass(enc, pass, "")) != 0)
            {
                setError(error, std::string("vvenc pass: ") + vvenc_get_error_msg(rc) + " " + vvenc_get_last_error(enc));
                ok = false;
                break;
            }
            units.clear();
            bool done = false;
            for (int i = 0; i <= n && ok && !done; i++)
            {
                vvencYUVBuffer* in = nullptr;
                if (i < n)
                {
                    const Picture& pic = pictures[(size_t)i];
                    if (pic.width != w || pic.height != h || pic.pixels != format_.pixels)
                    {
                        setError(error, "a picture of the wrong size or format");
                        ok = false;
                        break;
                    }
                    for (int p = 0; p < (cw ? 3 : 1); p++)
                    {
                        const int pw = p ? cw : w, ph = p ? ch : h;
                        vvencYUVPlane& pl = yuv->planes[p];
                        for (int y = 0; y < ph; y++)
                            for (int x = 0; x < pw; x++)
                                pl.ptr[y * pl.stride + x] = (int16_t)(pic.plane[p][(size_t)y * pw + x] << 2);
                    }
                    yuv->sequenceNumber = (uint64_t)i;
                    yuv->cts = i * tick;
                    yuv->ctsValid = true;
                    in = yuv;
                }
                // With no input the encoder flushes: call until it's done.
                do
                {
                    if ((rc = vvenc_encode(enc, in, au, &done)) != 0)
                    {
                        setError(error, std::string("vvenc encode: ") + vvenc_get_error_msg(rc));
                        ok = false;
                        break;
                    }
                    if (au->payloadUsedSize > 0) units.emplace_back(au->payload, au->payload + au->payloadUsedSize);
                } while (!in && !done);
            }
        }
        vvenc_encoder_close(enc);
        vvenc_accessUnit_free(au, true);
        vvenc_YUVBuffer_free(yuv, true);
        if (ok && (int)units.size() != n)
        {
            setError(error, "vvenc: " + std::to_string(units.size()) + " access units for " + std::to_string(n) +
                                " pictures");
            ok = false;
        }
        return ok;
    }

private:
    VideoFormat format_;
    EncoderOptions options_;
};

// Pictures come out in display order, some after the units that follow
// them; the GOP's end flushes the rest. order is the picture order count,
// 0 at each GOP's IDR.
class VvcDecoder : public Decoder
{
public:
    ~VvcDecoder() override { close(); }

    void reset() override { close(); }

    std::vector<Image> decode(const Bytes& unit, std::string* error) override
    {
        std::vector<Image> out;
        if (!dec_ && !open(error)) return out;
        vvdecAccessUnit au;
        vvdec_accessUnit_default(&au);
        buffer_ = unit;
        au.payload = buffer_.data();
        au.payloadSize = au.payloadUsedSize = (int)buffer_.size();
        vvdecFrame* frame = nullptr;
        int rc = vvdec_decode(dec_, &au, &frame);
        if (rc != VVDEC_OK && rc != VVDEC_TRY_AGAIN)
        {
            setError(error, std::string("vvdec: ") + vvdec_get_error_msg(rc) + " " + vvdec_get_last_error(dec_));
            return out;
        }
        take(frame, out);
        return out;
    }

    std::vector<Image> flush(std::string* error) override
    {
        std::vector<Image> out;
        if (!dec_) return out;
        for (int i = 0; i < 1024; i++)
        {
            vvdecFrame* frame = nullptr;
            int rc = vvdec_flush(dec_, &frame);
            if (rc == VVDEC_EOF || !frame)
            {
                take(frame, out);
                break;
            }
            if (rc != VVDEC_OK)
            {
                setError(error, std::string("vvdec flush: ") + vvdec_get_error_msg(rc));
                break;
            }
            take(frame, out);
        }
        close();  // a flushed decoder starts again for the next GOP
        return out;
    }

private:
    bool open(std::string* error)
    {
        vvdecParams p;
        vvdec_params_default(&p);
        p.threads = 1;
        p.logLevel = VVDEC_SILENT;
        dec_ = vvdec_decoder_open(&p);
        if (!dec_) setError(error, "vvdec: can't open a decoder");
        return dec_ != nullptr;
    }

    void close()
    {
        if (dec_) vvdec_decoder_close(dec_);
        dec_ = nullptr;
    }

    void take(vvdecFrame* f, std::vector<Image>& out)
    {
        if (!f) return;
        Image im;
        im.width = (int)f->width;
        im.height = (int)f->height;
        im.order = f->picAttributes ? (int)f->picAttributes->poc : (int)out.size();
        const bool mono = f->colorFormat == VVDEC_CF_YUV400_PLANAR || f->numPlanes < 3;
        const int sx = f->colorFormat == VVDEC_CF_YUV444_PLANAR ? 0 : 1;
        const int sy = f->colorFormat == VVDEC_CF_YUV420_PLANAR ? 1 : 0;
        const bool full = f->picAttributes && f->picAttributes->vui && f->picAttributes->vui->videoFullRangeFlag;
        const int shift = (int)f->bitDepth - 8;
        im.rgb = detail::toRgb(im.width, im.height, mono, false, full, [&](int p, int x, int y) -> int {
            const vvdecPlane& pl = f->planes[p];
            int px = p ? x >> sx : x, py = p ? y >> sy : y;
            const unsigned char* row = pl.ptr + (size_t)py * pl.stride;
            int v = pl.bytesPerSample == 2 ? reinterpret_cast<const uint16_t*>(row)[px] : row[px];
            return shift > 0 ? (v + (1 << (shift - 1))) >> shift : v;
        });
        out.push_back(std::move(im));
        vvdec_frame_unref(dec_, f);
    }

    vvdecDecoder* dec_ = nullptr;
    Bytes buffer_;
};

} // namespace

namespace detail
{

std::unique_ptr<Encoder> makeVvcEncoder(const VideoFormat& format, const EncoderOptions& options)
{
    return std::make_unique<VvcEncoder>(format, options);
}

std::unique_ptr<Decoder> makeVvcDecoder() { return std::make_unique<VvcDecoder>(); }

} // namespace detail

} // namespace fennectv
