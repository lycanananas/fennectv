#include "fennectv/media.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavdevice/avdevice.h>
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavformat/avformat.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
}

namespace fennectv
{

void FrameDeleter::operator()(AVFrame* f) const { av_frame_free(&f); }

namespace
{

std::string avError(int err)
{
    char buf[AV_ERROR_MAX_STRING_SIZE] = {0};
    av_strerror(err, buf, sizeof buf);
    return buf;
}

void setError(std::string* error, const std::string& what)
{
    if (error) *error = what;
}

AVPixelFormat avPixels(PixelFormat p)
{
    switch (p)
    {
    case PixelFormat::Gray: return AV_PIX_FMT_GRAY8;
    case PixelFormat::Yuv420: return AV_PIX_FMT_YUV420P;
    case PixelFormat::Yuv422: return AV_PIX_FMT_YUV422P;
    case PixelFormat::Yuv444: return AV_PIX_FMT_YUV444P;
    case PixelFormat::Rgb: return AV_PIX_FMT_GBRP;
    }
    return AV_PIX_FMT_NONE;
}

void quietLogs()
{
    static std::once_flag once;
    std::call_once(once, [] {
        av_log_set_level(AV_LOG_ERROR);
        avdevice_register_all();
    });
}

} // namespace

//-------------------------------------------------------------------------
// Source
//-------------------------------------------------------------------------

struct Source::Impl
{
    Options opt;
    VideoFormat format;
    AVFormatContext* fmt = nullptr;
    AVCodecContext* dec = nullptr;
    int stream = -1;
    AVFilterGraph* graph = nullptr;
    AVFilterContext* src = nullptr;
    AVFilterContext* sink = nullptr;
    int graphW = 0, graphH = 0, graphFmt = -1;
    AVPacket* pkt = av_packet_alloc();
    AVFrame* decoded = av_frame_alloc();
    bool eof = false, flushed = false;
    int64_t startPts = AV_NOPTS_VALUE;

    ~Impl()
    {
        avfilter_graph_free(&graph);
        avcodec_free_context(&dec);
        avformat_close_input(&fmt);
        av_packet_free(&pkt);
        av_frame_free(&decoded);
    }

    bool buildGraph(const AVFrame* f, std::string* error)
    {
        avfilter_graph_free(&graph);
        graph = avfilter_graph_alloc();
        AVStream* st = fmt->streams[stream];
        AVRational sar = f->sample_aspect_ratio.num ? f->sample_aspect_ratio : AVRational{1, 1};
        char args[256];
        std::snprintf(args, sizeof args, "video_size=%dx%d:pix_fmt=%d:time_base=%d/%d:pixel_aspect=%d/%d", f->width,
                      f->height, f->format, st->time_base.num, st->time_base.den, sar.num, sar.den);
        int r = avfilter_graph_create_filter(&src, avfilter_get_by_name("buffer"), "in", args, nullptr, graph);
        if (r >= 0) r = avfilter_graph_create_filter(&sink, avfilter_get_by_name("buffersink"), "out", nullptr, nullptr, graph);
        if (r < 0)
        {
            setError(error, "filter graph: " + avError(r));
            return false;
        }
        char chain[512];
        std::snprintf(chain, sizeof chain,
                      "fps=%d,scale=%d:%d:force_original_aspect_ratio=increase:flags=lanczos,crop=%d:%d,setsar=1%s,format=%s",
                      format.fps, format.width, format.height, format.width, format.height,
                      opt.denoise ? ",hqdn3d" : "", av_get_pix_fmt_name(avPixels(format.pixels)));
        AVFilterInOut* outputs = avfilter_inout_alloc();
        AVFilterInOut* inputs = avfilter_inout_alloc();
        outputs->name = av_strdup("in");
        outputs->filter_ctx = src;
        inputs->name = av_strdup("out");
        inputs->filter_ctx = sink;
        r = avfilter_graph_parse_ptr(graph, chain, &inputs, &outputs, nullptr);
        avfilter_inout_free(&inputs);
        avfilter_inout_free(&outputs);
        if (r >= 0) r = avfilter_graph_config(graph, nullptr);
        if (r < 0)
        {
            setError(error, std::string("filter graph \"") + chain + "\": " + avError(r));
            return false;
        }
        graphW = f->width;
        graphH = f->height;
        graphFmt = f->format;
        return true;
    }

    bool seekToStart()
    {
        int64_t ts = (int64_t)(opt.start * AV_TIME_BASE);
        if (fmt->start_time != AV_NOPTS_VALUE) ts += fmt->start_time;
        int r = avformat_seek_file(fmt, -1, INT64_MIN, ts, ts, 0);
        avcodec_flush_buffers(dec);
        avfilter_graph_free(&graph);
        graphFmt = -1;
        eof = flushed = false;
        AVStream* st = fmt->streams[stream];
        startPts = opt.start > 0 ? av_rescale_q(ts, AVRational{1, AV_TIME_BASE}, st->time_base) : AV_NOPTS_VALUE;
        return r >= 0;
    }
};

Source::Source() : d_(new Impl) {}
Source::~Source() = default;

bool Source::open(const Options& options, const VideoFormat& format, std::string* error)
{
    quietLogs();
    d_->opt = options;
    d_->format = format;
    const AVInputFormat* in = nullptr;
    if (!options.inputFormat.empty() && !(in = av_find_input_format(options.inputFormat.c_str())))
    {
        setError(error, "no input format " + options.inputFormat);
        return false;
    }
    AVDictionary* o = nullptr;
    int r = avformat_open_input(&d_->fmt, options.url.c_str(), in, &o);
    av_dict_free(&o);
    if (r < 0)
    {
        setError(error, options.url + ": " + avError(r));
        return false;
    }
    if ((r = avformat_find_stream_info(d_->fmt, nullptr)) < 0)
    {
        setError(error, options.url + ": " + avError(r));
        return false;
    }
    const AVCodec* codec = nullptr;
    d_->stream = av_find_best_stream(d_->fmt, AVMEDIA_TYPE_VIDEO, -1, -1, &codec, 0);
    if (d_->stream < 0 || !codec)
    {
        setError(error, options.url + ": no video");
        return false;
    }
    d_->dec = avcodec_alloc_context3(codec);
    avcodec_parameters_to_context(d_->dec, d_->fmt->streams[d_->stream]->codecpar);
    d_->dec->thread_count = 0;
    if ((r = avcodec_open2(d_->dec, codec, nullptr)) < 0)
    {
        setError(error, "decoder: " + avError(r));
        return false;
    }
    if (options.start > 0 && !d_->seekToStart())
    {
        setError(error, "can't seek to " + std::to_string(options.start) + " s");
        return false;
    }
    return true;
}

double Source::duration() const
{
    if (!d_->fmt || d_->fmt->duration == AV_NOPTS_VALUE || d_->fmt->duration <= 0) return 0;
    return d_->fmt->duration / (double)AV_TIME_BASE;
}

FramePtr Source::next()
{
    Impl& d = *d_;
    if (!d.fmt) return nullptr;
    while (true)
    {
        if (d.graph)
        {
            FramePtr out(av_frame_alloc());
            int r = av_buffersink_get_frame(d.sink, out.get());
            if (r >= 0) return out;
            if (r == AVERROR_EOF)
            {
                if (d.opt.loop && duration() > 0 && d.seekToStart()) continue;
                return nullptr;
            }
        }
        // Decoded pictures into the graph.
        int r = avcodec_receive_frame(d.dec, d.decoded);
        if (r >= 0)
        {
            int64_t pts = d.decoded->best_effort_timestamp;
            d.decoded->pts = pts;
            if (d.startPts != AV_NOPTS_VALUE && pts != AV_NOPTS_VALUE && pts < d.startPts)
            {
                av_frame_unref(d.decoded);
                continue;
            }
            if (!d.graph || d.decoded->width != d.graphW || d.decoded->height != d.graphH || d.decoded->format != d.graphFmt)
            {
                // A new shape (a stream changing mid-way): start the graph over.
                if (!d.buildGraph(d.decoded, nullptr)) return nullptr;
            }
            if (av_buffersrc_add_frame_flags(d.src, d.decoded, AV_BUFFERSRC_FLAG_KEEP_REF) < 0)
            {
                av_frame_unref(d.decoded);
                return nullptr;
            }
            av_frame_unref(d.decoded);
            continue;
        }
        if (r == AVERROR_EOF)
        {
            if (d.graph && !d.flushed)
            {
                d.flushed = true;
                if (av_buffersrc_add_frame(d.src, nullptr) < 0) return nullptr;
                continue;
            }
            if (d.opt.loop && duration() > 0 && d.seekToStart()) continue;
            return nullptr;
        }
        // More packets.
        if (d.eof) continue;
        r = av_read_frame(d.fmt, d.pkt);
        if (r < 0)
        {
            d.eof = true;
            avcodec_send_packet(d.dec, nullptr);
            continue;
        }
        if (d.pkt->stream_index == d.stream) avcodec_send_packet(d.dec, d.pkt);
        av_packet_unref(d.pkt);
    }
}

std::optional<Picture> Source::nextPicture()
{
    FramePtr f = next();
    if (!f) return std::nullopt;
    Picture p;
    p.width = f->width;
    p.height = f->height;
    p.pixels = d_->format.pixels;
    p.plane[0].resize((size_t)p.width * p.height);
    for (int y = 0; y < p.height; y++)
        std::memcpy(p.plane[0].data() + (size_t)y * p.width, f->data[0] + (ptrdiff_t)y * f->linesize[0], (size_t)p.width);
    int cw = 0, ch = 0;
    Picture::chromaSize(p.pixels, p.width, p.height, cw, ch);
    for (int c = 1; c < 3 && cw > 0; c++)
    {
        p.plane[c].resize((size_t)cw * ch);
        for (int y = 0; y < ch; y++)
            std::memcpy(p.plane[c].data() + (size_t)y * cw, f->data[c] + (ptrdiff_t)y * f->linesize[c], (size_t)cw);
    }
    return p;
}

} // namespace fennectv
