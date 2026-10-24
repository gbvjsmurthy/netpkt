// video_server: reads a video, JPEG-encodes each frame, paces them at a target
// frame rate, and sends them to one client over TCP or UDP.

#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <string>
#include <vector>
#include <sys/socket.h>
#include <opencv2/videoio.hpp>
#include "bench/metrics.h"
#include "common/frame.h"
#include "common/protocol.h"
#include "common/util.h"
#include "net/tcp.h"
#include "net/udp.h"

namespace {

constexpr double DEFAULT_FPS = 30.0;
constexpr int END_REPEATS = 5; // UDP: END can be lost, so send it several times

struct Opts {
    std::string video;
    std::uint16_t port = 9000;
    bool udp = false;
    double fps = 0;               // 0 = use the video's own frame rate
    int quality = 80;
    int width = 0;                // 0 = keep the video's resolution
    unsigned long max_frames = 0; // 0 = whole video
    bool paced = true;
    unsigned long frag_gap_us = 0;
    std::string log;
    std::string run_id;
};

void usage(const char* prog){
    std::fprintf(stderr,
        "usage: %s --video FILE [options]\n"
        "  --video FILE       input video (required)\n"
        "  --port N           listen port, TCP or UDP (default 9000)\n"
        "  --protocol P       tcp | udp (default tcp)\n"
        "  --fps X            target frame rate (default: the video's own rate)\n"
        "  --quality Q        JPEG quality 1..100 (default 80)\n"
        "  --width W          resize to width W, keep aspect (default: no resize)\n"
        "  --max-frames N     stop after N frames (default: whole video)\n"
        "  --unpaced          send as fast as possible instead of at --fps\n"
        "  --frag-gap-us N    UDP only: sleep N microseconds between fragments (default 0)\n"
        "  --log FILE         write a per-frame send log (CSV)\n"
        "  --run-id ID        label stored in the CSV (default run<unix time>)\n",
        prog);
}

// Returns 0 = ok, 2 = usage error.
int parse_args(int argc, char** argv, Opts& o){

    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];

        auto value = [&]() -> const char* {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "error: %s needs a value\n", a.c_str());
                return nullptr;
            }
            return argv[++i];
        };

        unsigned long u = 0;

        if (a == "--unpaced") {
            o.paced = false;
        } else if (a == "--video") {
            const char* v = value();
            if (!v) return 2;
            o.video = v;
        } else if (a == "--log") {
            const char* v = value();
            if (!v) return 2;
            o.log = v;
        } else if (a == "--run-id") {
            const char* v = value();
            if (!v) return 2;
            o.run_id = v;
        } else if (a == "--protocol") {
            const char* v = value();
            if (!v) return 2;
            if (std::strcmp(v, "tcp") == 0) o.udp = false;
            else if (std::strcmp(v, "udp") == 0) o.udp = true;
            else { std::fprintf(stderr, "error: --protocol must be tcp or udp\n"); return 2; }
        } else if (a == "--port") {
            const char* v = value();
            if (!v || !parse_uint(v, 65535, u) || u == 0) {
                std::fprintf(stderr, "error: --port must be 1..65535\n");
                return 2;
            }
            o.port = static_cast<std::uint16_t>(u);
        } else if (a == "--fps") {
            const char* v = value();
            if (!v || !parse_double(v, o.fps) || o.fps <= 0 || o.fps > 1000) {
                std::fprintf(stderr, "error: --fps must be in (0, 1000]\n");
                return 2;
            }
        } else if (a == "--quality") {
            const char* v = value();
            if (!v || !parse_uint(v, 100, u) || u < 1) {
                std::fprintf(stderr, "error: --quality must be 1..100\n");
                return 2;
            }
            o.quality = static_cast<int>(u);
        } else if (a == "--width") {
            const char* v = value();
            if (!v || !parse_uint(v, 16384, u)) {
                std::fprintf(stderr, "error: --width must be 0..16384\n");
                return 2;
            }
            o.width = static_cast<int>(u);
        } else if (a == "--max-frames") {
            const char* v = value();
            if (!v || !parse_uint(v, MAX_FRAME_ID, u)) {
                std::fprintf(stderr, "error: --max-frames must be 0..%u\n", MAX_FRAME_ID);
                return 2;
            }
            o.max_frames = u;
        } else if (a == "--frag-gap-us") {
            const char* v = value();
            if (!v || !parse_uint(v, 1000000, u)) {
                std::fprintf(stderr, "error: --frag-gap-us must be 0..1000000\n");
                return 2;
            }
            o.frag_gap_us = u;
        } else {
            std::fprintf(stderr, "error: unknown argument '%s'\n", a.c_str());
            return 2;
        }
    }

    if (o.video.empty()) {
        std::fprintf(stderr, "error: --video is required\n");
        return 2;
    }

    if (o.run_id.empty())
        o.run_id = "run" + std::to_string(static_cast<long long>(std::time(nullptr)));

    return 0;
}

} // namespace

int main(int argc, char** argv){
    Opts o;
    int rc = parse_args(argc, argv, o);

    if (rc != 0) {
        usage(argv[0]);
        return rc;
    }

    install_stop_handler();

    if (!std::ifstream(o.video)) {
        std::fprintf(stderr, "error: cannot read video file '%s'\n", o.video.c_str());
        return 1;
    }

    cv::VideoCapture cap(o.video);

    if (!cap.isOpened()) {

        std::fprintf(
            stderr,
            "error: OpenCV cannot decode '%s' (unsupported codec/container?)\n",
            o.video.c_str()
        );

        return 1;
    }

    double fps = o.fps;

    if (fps <= 0) {
        fps = cap.get(cv::CAP_PROP_FPS);

        if (!(fps >= 1.0 && fps <= 1000.0)) {

            std::fprintf(
                stderr,
                "warning: video reports no usable frame rate, using %.0f fps\n",
                DEFAULT_FPS
            );

            fps = DEFAULT_FPS;
        }
    }

    const std::uint64_t period_us = static_cast<std::uint64_t>(std::llround(1e6 / fps));
    const char* proto = o.udp ? "udp" : "tcp";

    // The settings that must be identical between a TCP run and a UDP run.
    std::printf(
        "server settings: protocol=%s video=%s fps=%.3f %s jpeg_quality=%d width=%d "
            "max_frames=%lu udp_frag_gap_us=%lu run_id=%s\n",
        proto, o.video.c_str(),
        fps, o.paced ? "paced" : "UNPACED",
        o.quality, o.width,
        o.max_frames, o.frag_gap_us, o.run_id.c_str()
    );

    Fd conn;
    std::uint32_t session = 0;
    std::string peer;

    if (o.udp) {
        std::printf("waiting for a UDP HELLO on port %u ...\n", o.port);
        std::fflush(stdout);
        conn = udp_wait_hello(o.port, session, peer);
    } else {
        Fd listener = tcp_listen(o.port);

        if (!listener.valid()) return 1;

        std::printf("waiting for a TCP client on port %u ...\n", o.port);
        std::fflush(stdout);
        conn = tcp_accept(listener, peer);
    }

    if (!conn.valid()) return stop_requested() ? 0 : 1;

    std::printf("client connected: %s\n", peer.c_str());

    std::vector<SendRec> send_log;
    std::uint64_t tx_bytes = 0, tx_dgrams = 0, behind = 0;
    std::uint32_t sent = 0;
    bool ok = true;

    // One period of slack so frame 0 (read + encode) is not instantly "late".
    const std::uint64_t t_start = now_us();
    cv::Mat img, scaled;
    std::uint32_t id = 0;

    while (!stop_requested()) {

        if (o.max_frames != 0 && id >= o.max_frames) break;

        if (!cap.read(img) || img.empty()) break; // end of video

        resize_to_width(img, o.width, scaled);

        Frame f;
        f.id = id;

        if (!jpeg_encode(scaled, o.quality, f.data)) {
            std::fprintf(stderr, "error: JPEG encoding of frame %u failed\n", id);
            ok = false;
            break;
        }

        if (id >= MAX_FRAME_ID || f.data.size() > MAX_FRAME_SIZE) {

            std::fprintf(
                stderr, 
                "error: frame %u is too large for the protocol (%zu bytes)\n",
                id, f.data.size()
            );

            ok = false;
            break;
        }

        if (o.paced) {
            std::uint64_t deadline = t_start + (
                static_cast<std::uint64_t>(id) + 1
            ) * period_us;
            
            // encoding is slower than the target rate; send immediately
            if (now_us() > deadline) behind++;
            else sleep_until_us(deadline);
        }

        // The timestamp is taken after encoding, so latency covers transport only.
        f.timestamp_us = now_us();
        bool sent_ok;

        if (o.udp) {
            std::size_t n = 0;
            sent_ok = udp_send_frame(
                conn.get(), session, f,
                static_cast<unsigned>(o.frag_gap_us), &n
            );

            tx_dgrams += n;
        } else sent_ok = tcp_send_frame(conn.get(), f);

        std::uint64_t t_done = now_us();

        if (!sent_ok) {
            std::fprintf(stderr, "error: sending frame %u failed; client gone?\n", id);
            ok = false;
            break;
        }

        tx_bytes += f.data.size();
        sent++;

        if (!o.log.empty())
            send_log.push_back({
                id, static_cast<std::uint32_t>(f.data.size()), f.timestamp_us,
                                t_done - f.timestamp_us
            });
            
        id++;
    }

    const std::uint64_t t_end = now_us();

    if (ok) {
        EndInfo end;
        end.frames = sent;
        end.bytes = tx_bytes;
        bool end_ok = o.udp ? udp_send_end(conn.get(), session, end, END_REPEATS)
                            : tcp_send_end(conn.get(), end);

        if (!end_ok) {
            std::fprintf(stderr, "error: could not send END message\n");
            ok = false;
        }

        if (!o.udp)
            shutdown(conn.get(), SHUT_WR); // FIN after all queued data
    }

    double secs = static_cast<double>(t_end - t_start) / 1e6;

    std::printf(
        "server summary: protocol=%s frames_sent=%u payload_bytes_sent=%llu "
            "elapsed_s=%.3f send_rate_mbps=%.3f behind_schedule_frames=%llu",
        proto, sent, static_cast<unsigned long long>(tx_bytes), secs,
        secs > 0 ? static_cast<double>(tx_bytes) * 8.0 / 1e6 / secs : 0.0,
        static_cast<unsigned long long>(behind)
    );

    if (o.udp)
        std::printf(" datagrams_sent=%llu", static_cast<unsigned long long>(tx_dgrams));

    std::printf("\n");

    if (stop_requested())
        std::printf("interrupted: the stream was cut short\n");

    if (!o.log.empty() && !write_send_csv(o.log, proto, o.run_id, send_log))
        ok = false;
        
    return ok ? 0 : 1;
}
