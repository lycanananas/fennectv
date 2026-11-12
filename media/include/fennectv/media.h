// Pictures in through the FFmpeg libraries (libavformat, libavdevice,
// libavfilter): any file, camera or URL, at the stream's frame rate, scaled
// to fill the picture and cut to it, in its pixel format. FFmpeg does
// only this; coding is the codec libraries' own (fennectv/codec.h).
#ifndef FENNECTV_MEDIA_H
#define FENNECTV_MEDIA_H

#include <memory>
#include <optional>
#include <string>

#include "fennectv/codec.h"

struct AVFrame;

namespace fennectv
{

// An AVFrame that frees itself.
struct FrameDeleter
{
    void operator()(AVFrame* f) const;
};
using FramePtr = std::unique_ptr<AVFrame, FrameDeleter>;

class Source
{
public:
    Source();
    ~Source();
    Source(const Source&) = delete;
    Source& operator=(const Source&) = delete;

    struct Options
    {
        std::string url;            // a path, udp://@239.1.1.1:1234, /dev/video0, video=...
        std::string inputFormat;    // "" to probe; v4l2, dshow, avfoundation for a camera
        double start = 0;           // seconds into a file
        bool loop = false;          // a file starts again at its end
        bool denoise = false;       // hqdn3d, which pays at these rates
    };

    bool open(const Options& options, const VideoFormat& format, std::string* error);

    // The next picture, ready for the encoder; nothing at the end.
    std::optional<Picture> nextPicture();
    // The same as an AVFrame.
    FramePtr next();

    // The source's length in seconds, 0 for a live one.
    double duration() const;

private:
    struct Impl;
    std::unique_ptr<Impl> d_;
};

} // namespace fennectv

#endif
