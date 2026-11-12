// fennectv-tx: codes video (VVC with VVenC, or AV1 with libaom) and sends
// it to a data2g-host's FENNECTV4 group (protocol 4), a burst at a time.
//
// A burst carries --gop seconds long closed GOPs (each from a keyframe),
// as many as fill the time it takes; the burst's room is shared among them
// by their pictures. FFmpeg reads, scales and converts the source once (a
// thread of its own); the encoder codes each GOP again until it fills its
// share to --fill, on --jobs threads; the burst is made of the codings
// that fill it best, and a GOP that can't fit gets what the others left,
// then loses its last pictures.
#include <QCoreApplication>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <thread>

#include "../common/args.h"
#include "fennectv/codec.h"
#include "fennectv/budget.h"
#include "fennectv/host_client.h"
#include "fennectv/media.h"
#include "fennectv/protocol.h"

using namespace fennectv;

namespace
{

const char* USAGE = R"(fennectv-tx --source URL [options]

Picture
  --source URL       a file, udp://@239.1.1.1:1234 (multicast MPEG-TS), a camera...
  --input-format F   for a camera: v4l2 (Linux), dshow (Windows); else probed
  --start S          seconds into a file (default 0)
  --loop             a file starts again at its end
  --codec NAME       vvc (H.266, VVenC; default) or av1 (libaom)
  --b-frames on|off  pictures referring forward too, coded out of order
                     within the GOP (default on)
  --width N          (default 160 for z8k, 96 for q3k)
  --height N         (default 120 for z8k, 72 for q3k)
  --fps N            (default 5 for z8k, 3 for q3k)
  --color MODE       gray, yuv420, yuv422, yuv444 or rgb (default gray);
                     vvc: gray or yuv420
  --denoise          hqdn3d before coding
  --gop S            seconds from keyframe to keyframe (default 3)
  --jobs N           GOPs coded at once (default 8)
  --fill SHARE       how full each GOP's coding aims to make its share of
                     the burst (default 1.0)
  --tolerance SHARE  below --fill by more than this, a GOP is coded again (default 0.04)
  --tries N          codings of a GOP at most (default 6); the burst is then
                     made of the codings of its GOPs that fill it best
  --preset P         encoder speed: vvc faster, fast, medium, slow (default),
                     slower; av1 cpu-used 0 (slowest) to 9, default 2
  --param K=V        straight to the encoder, again for more: libaom options
                     (tune=ssim, enable-qm=1), VVenC options (qpa=0, ALF=0)
  The bit rate is the burst's, set by --mode.

Air
  --call CALL        your callsign, sent with every burst (default N0CALL)
  --mode NAME        z8k, q3k or a Data2G mode name (default z8k)
  --gap S            seconds between bursts (default 0.3)
  --osd TEXT         text for receivers to show (default: the callsign)
  --bursts N         stop after N bursts (default: no end)
  --host ADDR        data2g-host (default 127.0.0.1)
  --cmd PORT         its command port (default 8300)
  --kiss PORT        its KISS port (default 8100)
  --version          the version and the broadcast group (the protocol)

Modes (2400 Hz wide, a 12 s burst every 12.3 s)
                   z8k                       q3k
  Data2G mode      w48-256l-r5/8             w48-16qam-r1/2
  Modulation       256-point constellation   16-QAM
  FEC a codeword   QC-LDPC 5/8 + CRC-32      QC-LDPC 1/2 + CRC-32
  Modem rate       8.2 kbps                  3.3 kbps
  Left for video   7.16 kbps (10998 B)       2.83 kbps (4351 B a burst)
  Default picture  160x120 gray, 5 fps       96x72 gray, 3 fps
  S/N where 1 codeword in 10 is lost (noise in 2500 Hz):
    quiet band     18.5 dB                   7.6 dB
    good HF        25.8 dB                   13.8 dB
    poor HF        26.3 dB                   12.4 dB
    disturbed HF   never                     11.9 dB

  quiet band    noise only, no fading (AWGN)
  good HF       slow fading, a short echo (CCIR good: 0.1 Hz, 0.5 ms)
  poor HF       faster fading, a longer echo (CCIR poor: 1 Hz, 2 ms)
  disturbed HF  fast fading, a long echo, as near the aurora (2 Hz, 4 ms)
  Below that S/N more is lost, above it less. Data2G corrects errors
  within each codeword and drops one that fails its CRC; FennecTV adds
  no FEC of its own. A lost codeword loses its frames: the GOP it cuts
  plays up to there, the next GOP starts clean.
)";

std::string fullModeName(const std::string& name)
{
    return name == "z8k" ? z8k().name : name == "q3k" ? q3k().name : name;
}

std::optional<ModeInfo> pickMode(const std::string& full, const std::vector<ModeInfo>& offered)
{
    for (const ModeInfo& m : offered)
        if (m.name == full) return m;
    return knownGear(full);
}

// A burst's pictures cut into GOPs of about gopPictures each.
std::vector<int> gopSplit(int burstPictures, double gopPictures)
{
    int n = std::max(1, (int)std::lround(burstPictures / std::max(1.0, gopPictures)));
    n = std::min(n, burstPictures);
    std::vector<int> counts((size_t)n, burstPictures / n);
    for (int i = 0; i < burstPictures % n; i++) counts[(size_t)(n - 1 - i)]++;
    return counts;
}

struct CodedGop
{
    std::vector<Bytes> packets;
    Encoder::Fit fit;
    size_t share = 0;
    int pictures = 0;
};

struct CodedBurst
{
    std::vector<Gop> gops;
    std::vector<CodedGop> coded;
    std::string error;
    size_t bytes = 0;
    double seconds = 0;
};

// Reads bursts of pictures from the source, codes their GOPs on several
// threads, and hands the bursts back in order.
class Coder
{
public:
    struct Aim
    {
        double fill = 1.0, tolerance = 0.04;
        int tries = 6;
    };

    Coder(Source& source, Codec codec, const VideoFormat& fmt, const EncoderOptions& options, std::string osd,
          const BurstPlan& plan, Aim aim, std::vector<int> split, int jobs, int maxBursts)
        : source_(source), codec_(codec), fmt_(fmt), options_(options), osd_(std::move(osd)), plan_(plan), aim_(aim),
          split_(std::move(split)), maxBursts_(maxBursts), jobs_(jobs)
    {
        for (int c : split_) burstPictures_ += c;
        capacity_ = plan_.capacityBytes((int)split_.size());
        reader_ = std::thread([this] { read(); });
        for (int i = 0; i < jobs; i++) workers_.emplace_back([this] { work(); });
    }

    ~Coder()
    {
        {
            std::lock_guard<std::mutex> l(m_);
            stop_ = true;
        }
        cv_.notify_all();
        reader_.join();
        for (auto& w : workers_) w.join();
    }

    // Burst n, waiting for it; nothing once the source has ended before it.
    std::optional<CodedBurst> take(int n)
    {
        std::unique_lock<std::mutex> l(m_);
        cv_.wait(l, [&] { return done_.count(n) || (readEnded_ && n >= burstsRead_); });
        auto it = done_.find(n);
        if (it == done_.end()) return std::nullopt;
        CodedBurst b = std::move(it->second);
        done_.erase(it);
        taken_ = n + 1;
        cv_.notify_all();
        return b;
    }

private:
    struct Job
    {
        int burst, gop;
        std::vector<Picture> pictures;
    };
    struct Pending
    {
        std::vector<CodedGop> gops;
        int left = 0;
        std::chrono::steady_clock::time_point start;
    };

    static long key(int burst, int gop) { return (long)burst * 256 + gop; }

    GopHeader header(int first, int count) const
    {
        GopHeader h;
        h.fps = (uint8_t)fmt_.fps;
        h.first = (uint8_t)first;
        h.count = (uint8_t)count;
        h.burstPictures = (uint8_t)burstPictures_;
        if (first == 0) h.osd = osd_;  // once a burst
        return h;
    }

    int firstOf(int gop) const
    {
        int f = 0;
        for (int i = 0; i < gop; i++) f += split_[(size_t)i];
        return f;
    }

    void read()
    {
        // Keep the coders busy and no more: a burst's GOPs at a time, one
        // burst ahead.
        const int ahead = (jobs_ + (int)split_.size() - 1) / (int)split_.size() + 1;
        for (int n = 0; maxBursts_ == 0 || n < maxBursts_; n++)
        {
            {
                std::unique_lock<std::mutex> l(m_);
                cv_.wait(l, [&] { return stop_ || n < taken_ + ahead; });
                if (stop_) break;
            }
            std::vector<Job> jobs;
            bool ended = false;
            for (size_t g = 0; g < split_.size() && !ended; g++)
            {
                Job j{n, (int)g, {}};
                while ((int)j.pictures.size() < split_[g])
                {
                    auto p = source_.nextPicture();
                    if (!p)
                    {
                        ended = true;  // the end: a short burst isn't sent
                        break;
                    }
                    j.pictures.push_back(std::move(*p));
                }
                jobs.push_back(std::move(j));
            }
            if (ended) break;
            std::lock_guard<std::mutex> l(m_);
            Pending& p = pending_[n];
            p.gops.resize(split_.size());
            p.left = (int)split_.size();
            p.start = std::chrono::steady_clock::now();
            for (auto& j : jobs) queue_.push_back(std::move(j));
            burstsRead_ = n + 1;
            cv_.notify_all();
        }
        std::lock_guard<std::mutex> l(m_);
        readEnded_ = true;
        cv_.notify_all();
    }

    CodedGop code(int gop, const std::vector<Picture>& pictures, const GopHeader& h, size_t share, std::string& error)
    {
        CodedGop c;
        c.share = share;
        c.pictures = (int)pictures.size();
        // Aimed by what this GOP's place taught in the bursts before.
        double rate;
        {
            std::lock_guard<std::mutex> l(m_);
            rate = rate_[gop];
        }
        auto enc = makeEncoder(codec_, fmt_, options_, &error);
        // the sequence's parameters go with the burst's first GOP only
        auto sizeOf = [&](const std::vector<Bytes>& p) { return gopBytes(codec_, h, p, gop == 0); };
        if (!enc || !enc->encodeToFit(pictures, share, aim_.fill, aim_.tolerance, aim_.tries, sizeOf, c.packets,
                                      c.fit, &error, &rate))
            c.fit.fits = false;
        else
        {
            std::lock_guard<std::mutex> l(m_);
            rate_[gop] = rate;
        }
        return c;
    }

    // The burst's GOPs from their codings (bestFill): each GOP's own
    // result (which may have dropped pictures) is one choice, its whole
    // codings the others. false if no choice fits.
    bool choose(std::vector<CodedGop>& gops) const
    {
        struct Choice
        {
            const std::vector<Bytes>* units;
            int dropped;
            double target;
        };
        std::vector<std::vector<Choice>> choices(gops.size());
        std::vector<std::vector<Coding>> sizes(gops.size());
        for (size_t g = 0; g < gops.size(); g++)
        {
            const CodedGop& c = gops[g];
            auto add = [&](const std::vector<Bytes>& u, int dropped, double target) {
                const size_t bytes = gopBytes(codec_, header(firstOf((int)g), (int)u.size()), u, g == 0);
                if (bytes > capacity_) return;
                choices[g].push_back({&u, dropped, target});
                sizes[g].push_back({bytes, dropped});
            };
            if (c.fit.fits) add(c.packets, c.fit.dropped, c.fit.target);
            for (const Encoder::Try& t : c.fit.codings) add(t.units, 0, t.target);
        }
        const std::vector<size_t> best = bestFill(sizes, capacity_);
        if (best.empty()) return false;
        for (size_t g = 0; g < gops.size(); g++)
        {
            const Choice ch = choices[g][best[g]];
            CodedGop& c = gops[g];
            if (ch.units != &c.packets) c.packets = *ch.units;
            c.fit.dropped = ch.dropped;
            c.fit.bytes = sizes[g][best[g]].bytes;
            c.fit.target = ch.target;
            c.fit.fits = true;
        }
        return true;
    }

    void work()
    {
        while (true)
        {
            Job j;
            {
                std::unique_lock<std::mutex> l(m_);
                cv_.wait(l, [&] { return stop_ || !queue_.empty() || readEnded_; });
                if (stop_ || (queue_.empty() && readEnded_)) return;
                j = std::move(queue_.front());
                queue_.pop_front();
            }
            const size_t share = capacity_ * split_[(size_t)j.gop] / burstPictures_;
            std::string error;
            CodedGop c = code(j.gop, j.pictures, header(firstOf(j.gop), split_[(size_t)j.gop]), share, error);
            bool last = false;
            {
                std::lock_guard<std::mutex> l(m_);
                held_[key(j.burst, j.gop)] = std::move(j.pictures);
                Pending& p = pending_[j.burst];
                p.gops[(size_t)j.gop] = std::move(c);
                last = --p.left == 0;
                if (!error.empty()) errors_[j.burst] += error + "; ";
            }
            if (last) finish(j.burst);
        }
    }

    // All GOPs of a burst coded: the codings that fill the burst best are
    // chosen among all the tries (one GOP's too big a try can take what
    // another left). A GOP that still loses pictures is coded again with
    // whatever room is left, and the choice made again.
    void finish(int n)
    {
        Pending p;
        std::string error;
        {
            std::lock_guard<std::mutex> l(m_);
            p = std::move(pending_[n]);
            pending_.erase(n);
            error = errors_[n];
            errors_.erase(n);
        }
        choose(p.gops);
        auto bytesOf = [&](size_t g) {
            const CodedGop& c = p.gops[g];
            return c.fit.fits ? gopBytes(codec_, header(firstOf((int)g), (int)c.packets.size()), c.packets, g == 0) : 0;
        };
        for (size_t g = 0; g < p.gops.size(); g++)
        {
            if (p.gops[g].fit.fits && p.gops[g].fit.dropped == 0) continue;
            size_t others = 0;
            for (size_t o = 0; o < p.gops.size(); o++)
                if (o != g) others += bytesOf(o);
            const size_t room = capacity_ > others ? capacity_ - others : 0;
            if (room <= p.gops[g].fit.bytes) continue;
            std::vector<Picture> pics;
            {
                std::lock_guard<std::mutex> l(m_);
                pics = held_[key(n, (int)g)];
            }
            std::string e;
            CodedGop again = code((int)g, pics, header(firstOf((int)g), split_[g]), room, e);
            int before = p.gops[g].fit.fits ? p.gops[g].fit.dropped : 1 << 20;
            if (again.fit.fits && again.fit.dropped < before)
            {
                again.fit.tries += p.gops[g].fit.tries;
                for (auto& t : p.gops[g].fit.codings) again.fit.codings.push_back(std::move(t));
                again.share = p.gops[g].share;
                p.gops[g] = std::move(again);
                choose(p.gops);
            }
        }

        CodedBurst b;
        b.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - p.start).count();
        b.error = error;
        for (size_t g = 0; g < p.gops.size(); g++)
        {
            {
                std::lock_guard<std::mutex> l(m_);
                held_.erase(key(n, (int)g));
            }
            if (!p.gops[g].fit.fits) continue;
            Gop gop;
            gop.header = header(firstOf((int)g), (int)p.gops[g].packets.size());
            gop.units = p.gops[g].packets;
            // Parameters only with GOP 0; the others' must be the same, or
            // a receiver would decode them with GOP 0's (it never happens
            // with one encoder setting; if it did, they go too).
            gop.params = g == 0;
            if (g > 0 && p.gops[0].fit.fits && !gop.units.empty() &&
                paramsOf(codec_, gop.units[0]) != paramsOf(codec_, p.gops[0].packets[0]))
            {
                gop.params = true;
                b.error += "GOP " + std::to_string(g) + " has parameters of its own; ";
            }
            b.bytes += gopBytes(codec_, gop.header, gop.units, gop.params);
            b.gops.push_back(std::move(gop));
        }
        b.coded = std::move(p.gops);
        std::lock_guard<std::mutex> l(m_);
        done_[n] = std::move(b);
        cv_.notify_all();
    }

    Source& source_;
    Codec codec_;
    VideoFormat fmt_;
    EncoderOptions options_;
    std::string osd_;
    BurstPlan plan_;
    Aim aim_;
    std::vector<int> split_;
    int burstPictures_ = 0;
    size_t capacity_ = 0;  // the burst's, with its GOPs
    int maxBursts_;
    int jobs_;

    std::mutex m_;
    std::condition_variable cv_;
    std::deque<Job> queue_;
    std::map<int, Pending> pending_;
    std::map<int, std::string> errors_;
    std::map<long, std::vector<Picture>> held_;
    std::map<int, CodedBurst> done_;
    std::map<int, double> rate_;  // a GOP's place: what its codings taught (encodeToFit's learned)
    int burstsRead_ = 0, taken_ = 0;
    bool readEnded_ = false, stop_ = false;
    std::thread reader_;
    std::vector<std::thread> workers_;
};

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    Args a(argc, argv, {"loop", "denoise", "help", "version"});
    if (a.has("version"))
    {
        std::printf("fennectv-tx %s, broadcast group %s\n", FENNECTV_VERSION, GROUP);
        return 0;
    }
    if (!a.bad().empty() || a.has("help") || !a.has("source"))
    {
        if (!a.bad().empty()) std::fprintf(stderr, "%s\n\n", a.bad().c_str());
        std::fputs(USAGE, a.has("help") ? stdout : stderr);
        return a.has("help") ? 0 : 2;
    }

    const std::string modeName = fullModeName(a.str("mode", "z8k"));
    const bool slow = modeName == q3k().name;  // Q3k carries 2 kbps: a smaller, slower picture
    VideoFormat fmt;
    fmt.width = a.integer("width", slow ? 96 : 160);
    fmt.height = a.integer("height", slow ? 72 : 120);
    fmt.fps = a.integer("fps", slow ? 3 : 5);
    const std::string codecArg = a.str("codec", "vvc");
    const auto codec = codecNamed(codecArg);
    if (!codec)
    {
        std::fprintf(stderr, "--codec: av1 or vvc%s\n", codecArg == "h264" ? " (h264 is to come)" : "");
        return 2;
    }
    EncoderOptions encOptions;
    const std::string bArg = a.str("b-frames", "on");
    if (bArg != "on" && bArg != "off")
    {
        std::fprintf(stderr, "--b-frames: on or off\n");
        return 2;
    }
    encOptions.bFrames = bArg == "on";
    encOptions.preset = a.str("preset");
    for (const std::string& kv : a.all("param"))
    {
        const auto eq = kv.find('=');
        if (eq == std::string::npos || eq == 0)
        {
            std::fprintf(stderr, "--param: KEY=VALUE, not %s\n", kv.c_str());
            return 2;
        }
        encOptions.params.push_back({kv.substr(0, eq), kv.substr(eq + 1)});
    }
    const std::string color = a.str("color", "gray");
    auto pixels = pixelFormatNamed(color);
    if (!pixels)
    {
        std::fprintf(stderr, "--color is gray, yuv420, yuv422, yuv444 or rgb\n");
        return 2;
    }
    fmt.pixels = *pixels;
    {
        // Two black pictures through the encoder: a wrong --preset or
        // --param shows now, not at the first burst.
        std::string e;
        auto enc = makeEncoder(*codec, fmt, encOptions, &e);
        std::vector<Bytes> units;
        std::vector<Picture> black(2);
        for (Picture& p : black)
        {
            p.width = fmt.width;
            p.height = fmt.height;
            p.pixels = fmt.pixels;
            int cw = 0, ch = 0;
            Picture::chromaSize(fmt.pixels, fmt.width, fmt.height, cw, ch);
            const bool rgb = fmt.pixels == PixelFormat::Rgb;
            p.plane[0].assign((size_t)fmt.width * fmt.height, rgb ? 0 : 16);
            for (int k = 1; k < 3 && cw; k++) p.plane[k].assign((size_t)cw * ch, rgb ? 0 : 128);
        }
        if (!enc || !enc->prepare(black, &e) || !enc->encode(black, 8000, units, &e))
        {
            std::fprintf(stderr, "%s\n", e.c_str());
            return 2;
        }
    }
    const std::string call = a.str("call", "N0CALL");
    const int jobs = std::max(1, a.integer("jobs", 8));

    HostClient host;
    if (!host.connectTo(a.str("host", "127.0.0.1"), a.integer("cmd", 8300), a.integer("kiss", 8100)))
    {
        std::fprintf(stderr, "can't reach data2g-host: %s\n", host.error().c_str());
        return 1;
    }
    auto mode = pickMode(modeName, host.modes());
    if (!mode)
    {
        std::fprintf(stderr, "unknown mode %s\n", modeName.c_str());
        return 2;
    }
    auto plan = planBurst(*mode);
    if (!plan)
    {
        std::fprintf(stderr, "mode %s has no room for a picture\n", mode->name.c_str());
        return 2;
    }
    const double burstSeconds = plan->airSeconds + a.num("gap", 0.3);
    const int burstPictures = std::max(1, (int)std::lround(burstSeconds * fmt.fps));
    if (fmt.fps < 1 || burstPictures > 255)
    {
        std::fprintf(stderr, "--fps: 1 to %d in %s\n", (int)(255 / burstSeconds), mode->name.c_str());
        return 2;
    }
    const std::vector<int> split = gopSplit(burstPictures, a.num("gop", 3.0) * fmt.fps);

    std::string err;
    Source source;
    Source::Options so;
    so.url = a.str("source");
    so.inputFormat = a.str("input-format");
    so.start = a.num("start", 0);
    so.loop = a.has("loop");
    so.denoise = a.has("denoise");
    if (!source.open(so, fmt, &err))
    {
        std::fprintf(stderr, "%s\n", err.c_str());
        return 1;
    }
    const int port = host.openGroup(GROUP, call);
    if (port < 0 || !host.setMode(port, mode->name))
    {
        std::fprintf(stderr, "data2g-host refused BCAST OPEN %s / BCAST MODE %s\n", GROUP, mode->name.c_str());
        return 1;
    }

    std::string gops;
    for (int c : split) gops += (gops.empty() ? "" : "+") + std::to_string(c);
    std::fprintf(stderr,
                 "port %d, %s: %d slots, %.2f s, %d data codewords x %d frames of %zu B\n"
                 "%s%s %dx%d %s @ %d fps, %d pictures a burst every %.2f s in GOPs of %s, "
                 "%zu B (%.2f kbps), %d jobs\n",
                 port, mode->name.c_str(), plan->slots,
                 plan->airSeconds, plan->dataCodewords, plan->framing.framesPerCodeword, plan->framing.frameBytes,
                 *codec == Codec::Vvc ? "VVenC" : "libaom", encOptions.bFrames ? " with B frames" : "", fmt.width,
                 fmt.height, color.c_str(), fmt.fps, burstPictures, burstSeconds, gops.c_str(),
                 plan->capacityBytes((int)split.size()),
                 plan->capacityBytes((int)split.size()) * 8.0 / burstSeconds / 1000.0, jobs);

    Coder::Aim aim;
    aim.fill = a.num("fill", 1.0);
    aim.tolerance = a.num("tolerance", 0.04);
    aim.tries = a.integer("tries", 6);
    Coder coder(source, *codec, fmt, encOptions, a.str("osd", call), *plan, aim, split, jobs, a.integer("bursts", 0));
    int sent = 0;
    uint8_t burst = 0;
    auto lastSend = std::chrono::steady_clock::now();

    // What went on the air: the host acks each frame (ACKMODE) once its
    // burst has gone. Data2G's own counters are for ARQ connections and
    // never show broadcast, so the counting is here.
    struct OnAir
    {
        std::map<uint8_t, int> acked;      // frames acked, by burst
        std::map<uint8_t, size_t> bytes;   // picture bytes, by burst
        int bursts = 0;
        long frames = 0;
        size_t picture = 0;
        std::chrono::steady_clock::time_point first;
    } air;
    auto takeAcks = [&](const std::vector<KissFrame>& kf) {
        for (const KissFrame& f : kf)
        {
            if (f.command != ragtime::link::KISS_ACKMODE || f.payload.size() != 2) continue;
            const uint8_t b = f.payload[0];
            if (!air.bytes.count(b)) continue;  // not one of ours (or counted already)
            if (++air.acked[b] < plan->frames) continue;
            const auto now = std::chrono::steady_clock::now();
            if (air.bursts == 0) air.first = now;
            air.bursts++;
            air.frames += plan->frames;
            air.picture += air.bytes[b];
            // the rate from the first burst's end to this one's: the bursts between
            char rate[32] = "";
            const double secs = std::chrono::duration<double>(now - air.first).count();
            if (air.bursts > 1 && secs > 0)
                std::snprintf(rate, sizeof rate, ", %.2f kbps", (air.picture - air.bytes[b]) * 8.0 / secs / 1000.0);
            std::fprintf(stderr, "burst %u on the air: %d frames, %zu B of picture; %d bursts, %.1f kB so far%s\n", b,
                         plan->frames, air.bytes[b], air.bursts, air.picture / 1000.0, rate);
            air.acked.erase(b);
            air.bytes.erase(b);
        }
    };
    for (int n = 0;; n++)
    {
        auto b = coder.take(n);
        if (!b) break;
        if (sent > 0)
        {
            // Queue the burst once the one before it is on the air, so the
            // host builds it straight after.
            bool started = false;
            for (int waited = 0; !started && waited < 60000; waited += 200)
            {
                std::vector<CommandEvent> ev;
                std::vector<KissFrame> kf;
                host.poll(200, ev, kf);
                takeAcks(kf);
                for (auto& e : ev)
                {
                    if (e.type == CommandEvent::Type::Ptt && e.on) started = true;
                    if (e.type == CommandEvent::Type::BcastDropped)
                        std::fprintf(stderr, "host dropped %d frames\n", e.count);
                }
                if (!host.connected())
                {
                    std::fprintf(stderr, "data2g-host went away\n");
                    return 1;
                }
            }
            if (!started) std::fprintf(stderr, "no PTT ON in 60 s: queueing anyway\n");
        }
        double sinceLast = std::chrono::duration<double>(std::chrono::steady_clock::now() - lastSend).count();
        lastSend = std::chrono::steady_clock::now();

        int kept = 0, dropped = 0;
        std::string tries;
        for (const CodedGop& c : b->coded)
        {
            kept += c.fit.fits ? (int)c.packets.size() : 0;
            dropped += c.fit.fits ? c.fit.dropped : c.pictures;
            tries += (tries.empty() ? "" : "+") + std::to_string(c.fit.tries);
        }
        auto frames = packBurst(burst, *codec, b->gops, plan->framing.payloadBytes, plan->frames);
        std::fprintf(stderr, "burst %u: %d/%d pictures in %zu GOPs, %zu of %zu B (%.0f%%), tries %s, coded in %.1f s%s%s%s\n",
                     burst, kept, burstPictures, b->gops.size(), b->bytes, plan->capacityBytes((int)split.size()),
                     100.0 * b->bytes / plan->capacityBytes((int)split.size()), tries.c_str(), b->seconds,
                     dropped ? ("  [" + std::to_string(dropped) + " pictures dropped to fit]").c_str() : "",
                     dropped * 10 > burstPictures ? "  too much for the mode: lower --fps or the picture" : "",
                     sent > 0 && sinceLast > burstSeconds + 1.0 ? "  (late: the coders fell behind; more --jobs)" : "");
        if (!b->error.empty()) std::fprintf(stderr, "   encoder: %s\n", b->error.c_str());
        if (!frames)
        {
            std::fprintf(stderr, "   can't pack the burst: not sent\n");
            burst++;
            continue;
        }
        air.bytes[burst] = b->bytes;
        air.acked[burst] = 0;
        for (size_t i = 0; i < frames->size(); i++)
            host.sendKissAck(port, (uint16_t)(burst << 8 | i), (*frames)[i]);
        sent++;
        burst++;
    }
    if (sent > 0)
    {
        // Wait for the bursts still queued to go.
        std::fprintf(stderr, "waiting for the last bursts to go...\n");
        for (int waited = 0; waited < 60000 && air.bursts < sent && host.connected(); waited += 200)
        {
            std::vector<CommandEvent> ev;
            std::vector<KissFrame> kf;
            host.poll(200, ev, kf);
            takeAcks(kf);
        }
    }
    std::fprintf(stderr, "%d bursts queued, %d on the air (%ld frames, %.1f kB of picture)\n", sent, air.bursts,
                 air.frames, air.picture / 1000.0);
    return air.bursts == sent ? 0 : 1;
}
