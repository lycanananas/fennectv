// fennectv-rx: listens to a data2g-host's FENNECTV group, puts each
// burst's frames together, decodes every GOP whose keyframe arrived (AV1
// with libaom, VVC with VVdeC), as far as its bytes go, and shows it (--play) at the stream's
// pace, holding the last picture over what was lost; or writes the stream
// (--out). No FFmpeg here.
#include <QApplication>
#include <QPainter>
#include <QTimer>
#include <QWidget>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <string>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

#include "../common/args.h"
#include "fennectv/host_client.h"
#include "fennectv/codec.h"
#include "fennectv/protocol.h"

using namespace fennectv;

namespace
{

const char* USAGE = R"(fennectv-rx [options]

  --host ADDR        data2g-host (default 127.0.0.1)
  --cmd PORT         its command port (default 8300)
  --kiss PORT        its KISS port (default 8100)
  --play             show the picture in a window
  --out FILE         write the stream; - for stdout
)";

using Clock = std::chrono::steady_clock;

// The picture, scaled up with its shape kept, and a status line.
class Screen : public QWidget
{
public:
    Screen()
    {
        setWindowTitle("FennecTV");
        resize(640, 520);
        setStyleSheet("background: black;");
    }

    void show(QImage image)
    {
        image_ = std::move(image);
        update();
    }
    void setOsd(const QString& t) { osd_ = t; }
    void setStatus(const QString& t)
    {
        status_ = t;
        update();
    }
    void setNoSignal(bool on)
    {
        noSignal_ = on;
        update();
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.fillRect(rect(), Qt::black);
        const int bar = 28;
        QRect area(0, 0, width(), height() - bar);
        if (!image_.isNull())
        {
            QSize s = image_.size().scaled(area.size(), Qt::KeepAspectRatio);
            QRect target(area.x() + (area.width() - s.width()) / 2, area.y() + (area.height() - s.height()) / 2,
                         s.width(), s.height());
            p.setRenderHint(QPainter::SmoothPixmapTransform, true);
            p.drawImage(target, image_);
            p.setPen(Qt::white);
            p.drawText(target.adjusted(8, 6, -8, -6), Qt::AlignLeft | Qt::AlignTop, osd_);
            if (noSignal_)
            {
                QFont f = p.font();
                f.setPointSize(18);
                f.setBold(true);
                p.setFont(f);
                p.setPen(QColor(255, 80, 80));
                p.drawText(target, Qt::AlignCenter, "NO SIGNAL");
                p.setFont(font());
            }
        }
        else
        {
            p.setPen(Qt::gray);
            p.drawText(area, Qt::AlignCenter, "waiting for FENNECTV...");
        }
        p.setPen(QColor(200, 200, 200));
        p.drawText(QRect(8, height() - bar, width() - 16, bar), Qt::AlignVCenter | Qt::AlignLeft, status_);
    }

private:
    QImage image_;
    QString osd_, status_;
    bool noSignal_ = false;
};

struct Burst
{
    std::vector<std::optional<DecodedFrame>> frames;
    int total = 0;  // from the frame marked last
    int arrived = 0;
    Clock::time_point last;
};

class Receiver
{
public:
    Receiver(HostClient& host, int port, Screen* screen, FILE* out)
        : host_(host), port_(port), screen_(screen), out_(out)
    {
    }

    // Reads the host; false once it's gone.
    bool pump()
    {
        std::vector<CommandEvent> ev;
        std::vector<KissFrame> kf;
        host_.poll(0, ev, kf);
        for (auto& e : ev)
        {
            if (e.type == CommandEvent::Type::BcastHeard && e.number == port_)
            {
                lastSnr_ = e.snr;
                std::fprintf(stderr, "burst from %s%s\n", e.text.empty() ? "?" : e.text.c_str(),
                             e.snr ? (", S/N " + std::to_string(*e.snr).substr(0, 4) + " dB").c_str() : "");
            }
            if (e.type == CommandEvent::Type::BcastLost && e.number == port_)
                std::fprintf(stderr, "the air lost at least %d frames of a burst\n", e.count);
        }
        for (auto& f : kf) take(f);
        // The last frame lost: a burst's frames come together at its end,
        // so quiet for a second means it's done.
        for (auto o = bursts_.begin(); o != bursts_.end();)
        {
            if (Clock::now() - o->second.last > std::chrono::seconds(1))
            {
                finish(o->first, o->second);
                o = bursts_.erase(o);
            }
            else
                ++o;
        }
        return host_.connected();
    }

    // The next picture to show, at the stream's rate.
    void tick()
    {
        if (!screen_) return;
        bool idle = Clock::now() - lastBurst_ > std::chrono::milliseconds((int)(burstSeconds_ * 2000 + 3000));
        screen_->setNoSignal(haveShown_ && idle && pictures_.empty());
        if (pictures_.empty())
        {
            playing_ = false;  // ran dry: wait for a cushion again
            return;
        }
        // Bursts arrive a burst apart, give or take: hold PREROLL of
        // pictures back before playing, so a late one doesn't stop the picture.
        if (!playing_)
        {
            if (Clock::now() - firstQueued_ < PREROLL) return;
            playing_ = true;
        }
        screen_->show(pictures_.front());
        haveShown_ = true;
        pictures_.pop_front();
    }

    int fps() const { return fps_; }
    void summary() const { std::fprintf(stderr, "bursts whole %d, partly %d, lost %d\n", whole_, partly_, lost_); }

private:
    void take(const KissFrame& f)
    {
        if (f.port != port_ || f.command != ragtime::link::KISS_DATA) return;
        auto d = decodeFrame(f.payload);
        if (!d) return;
        const FrameHeader h = d->header;
        auto [it, fresh] = bursts_.try_emplace(h.burst);
        Burst& b = it->second;
        if (fresh)
        {
            // A new burst: whatever is older is as complete as it gets.
            for (auto o = bursts_.begin(); o != bursts_.end();)
            {
                if (o->first != h.burst)
                {
                    finish(o->first, o->second);
                    o = bursts_.erase(o);
                }
                else
                    ++o;
            }
        }
        b.arrived++;
        b.last = Clock::now();
        if (h.last) b.total = h.idx + 1;
        if (b.frames.size() <= h.idx) b.frames.resize((size_t)h.idx + 1);
        b.frames[h.idx] = std::move(*d);
        // Every frame of the burst in: no need to wait for the quiet.
        if (b.total && b.arrived >= b.total)
        {
            finish(h.burst, b);
            bursts_.erase(h.burst);
        }
    }

    void finish(uint8_t id, Burst& b)
    {
        lastBurst_ = Clock::now();
        if (b.total > (int)b.frames.size()) b.frames.resize((size_t)b.total);
        int kept = 0;
        for (auto& f : b.frames) kept += f.has_value();
        std::vector<ReceivedGop> gops = parseBurst(b.frames);

        // The burst's pictures in their places; what didn't come stays empty.
        int total = 0;
        for (auto& g : gops) total = std::max(total, (int)g.header.burstPictures);
        std::vector<std::optional<QImage>> slots((size_t)total);
        std::string line;
        int shown = 0;
        bool whole = !gops.empty();
        for (const ReceivedGop& g : gops)
        {
            if (g.header.fps) fps_ = g.header.fps;
            if (screen_) screen_->setOsd(QString::fromStdString(g.header.osd));
            whole = whole && g.complete;
            Decoder* dec = decoderFor(g.codec);
            int decoded = 0;
            // Pictures by display order: with B frames they come out of order.
            auto place = [&](std::vector<Decoder::Image>& imgs) {
                for (auto& pic : imgs)
                {
                    decoded++;
                    size_t at = (size_t)g.header.first + (size_t)std::max(0, pic.order);
                    if (!screen_ || at >= slots.size()) continue;
                    QImage img(pic.width, pic.height, QImage::Format_RGB32);
                    for (int y = 0; y < pic.height; y++)
                        std::memcpy(img.scanLine(y), pic.rgb.data() + (size_t)y * pic.width, (size_t)pic.width * 4);
                    slots[at] = std::move(img);
                }
            };
            if (dec)
            {
                dec->reset();  // every GOP stands alone, from its keyframe
                std::string err;
                for (const Bytes& u : g.units)
                {
                    if (out_) std::fwrite(u.data(), 1, u.size(), out_);
                    auto imgs = dec->decode(u, &err);
                    if (!err.empty()) break;
                    place(imgs);
                }
                auto rest = dec->flush(err.empty() ? &err : nullptr);
                place(rest);
                if (!err.empty()) std::fprintf(stderr, "burst %u: %s\n", id, err.c_str());
            }
            shown += decoded;
            line += " " + std::to_string(decoded) + "/" + std::to_string(g.header.count);
        }
        if (out_) std::fflush(out_);
        whole = whole && shown == total;
        if (whole) whole_++;
        else if (shown) partly_++;
        else lost_++;

        char head[160];
        std::snprintf(head, sizeof head, "burst %u: frames %d/%d, %d/%d pictures, GOPs", id, kept,
                      std::max(b.total, (int)b.frames.size()), shown, total);
        std::string status = head + (line.empty() ? std::string(" none") : line);
        if (lastSnr_) status += ", S/N " + std::to_string(*lastSnr_).substr(0, 4) + " dB";
        std::fprintf(stderr, "%s  [whole %d, partly %d, lost %d]\n", status.c_str(), whole_, partly_, lost_);
        if (screen_) screen_->setStatus(QString::fromStdString(status));
        if (total) burstSeconds_ = total / (double)std::max(1, fps_);
        if (!screen_ || shown == 0) return;

        // Play the burst at its pace: a lost picture shows the one before.
        for (auto& s : slots)
        {
            if (s) last_ = *s;
            if (last_.isNull()) continue;
            if (pictures_.empty() && !playing_) firstQueued_ = Clock::now();
            pictures_.push_back(last_);
        }
        // Keep no more than two bursts waiting, or the delay grows.
        while ((int)pictures_.size() > 2 * total + 1) pictures_.pop_front();
    }

    HostClient& host_;
    int port_;
    Screen* screen_;
    FILE* out_;
    std::map<uint8_t, Burst> bursts_;
    std::optional<double> lastSnr_;
    std::map<Codec, std::unique_ptr<Decoder>> decoders_;

    Decoder* decoderFor(Codec c)
    {
        auto it = decoders_.find(c);
        if (it != decoders_.end()) return it->second.get();
        std::string err;
        auto d = makeDecoder(c, &err);
        if (!d) std::fprintf(stderr, "%s\n", err.c_str());
        return (decoders_[c] = std::move(d)).get();
    }
    std::deque<QImage> pictures_;
    QImage last_;
    int fps_ = 5;
    double burstSeconds_ = 12.3;
    bool haveShown_ = false;
    bool playing_ = false;
    Clock::time_point firstQueued_;
    static constexpr std::chrono::seconds PREROLL{3};
    Clock::time_point lastBurst_ = Clock::now();
    int whole_ = 0, partly_ = 0, lost_ = 0;
};

} // namespace

int main(int argc, char** argv)
{
    QApplication app(argc, argv);
    Args a(argc, argv, {"play", "help"});
    if (!a.bad().empty() || a.has("help"))
    {
        if (!a.bad().empty()) std::fprintf(stderr, "%s\n\n", a.bad().c_str());
        std::fputs(USAGE, a.has("help") ? stdout : stderr);
        return a.has("help") ? 0 : 2;
    }

    HostClient host;
    if (!host.connectTo(a.str("host", "127.0.0.1"), a.integer("cmd", 8300), a.integer("kiss", 8100)))
    {
        std::fprintf(stderr, "can't reach data2g-host: %s\n", host.error().c_str());
        return 1;
    }
    int port = host.openGroup(GROUP);
    if (port < 0)
    {
        std::fprintf(stderr, "data2g-host refused BCAST OPEN %s\n", GROUP);
        return 1;
    }
    std::fprintf(stderr, "listening on %s, port %d\n", GROUP, port);

    FILE* out = nullptr;
    if (a.str("out") == "-")
    {
#ifdef _WIN32
        _setmode(_fileno(stdout), _O_BINARY);
#endif
        out = stdout;
    }
    else if (a.has("out") && !(out = std::fopen(a.str("out").c_str(), "wb")))
    {
        std::fprintf(stderr, "can't write %s\n", a.str("out").c_str());
        return 1;
    }

    std::unique_ptr<Screen> screen;
    if (a.has("play"))
    {
        screen = std::make_unique<Screen>();
        screen->setStatus(QString("listening on %1, port %2").arg(GROUP).arg(port));
        screen->QWidget::show();
    }
    Receiver rx(host, port, screen.get(), out);

    QTimer net;
    QObject::connect(&net, &QTimer::timeout, [&] {
        if (!rx.pump())
        {
            std::fprintf(stderr, "data2g-host went away\n");
            app.quit();
        }
    });
    net.start(50);
    QTimer frames;
    QObject::connect(&frames, &QTimer::timeout, [&] {
        rx.tick();
        int ms = 1000 / std::max(1, rx.fps());
        if (frames.interval() != ms) frames.setInterval(ms);
    });
    frames.start(100);
    int r = app.exec();
    rx.summary();
    if (out && out != stdout) std::fclose(out);
    return r;
}
