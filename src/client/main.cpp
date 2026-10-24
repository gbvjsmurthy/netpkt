// video_client: receives frames over TCP or UDP, reassembles them, decodes and
// shows them, and records what it observed.
//
// Threads:
//   receive thread - owns the socket and the RxLog; pushes complete frames into
//                    a FrameQueue and never waits for the display.
//   main thread    - pops frames, decodes, shows (OpenCV windows want the main
//                    thread), and records PlayRecs.
// The two threads share only the FrameQueue (mutex inside). RxLog and the
// PlayRec vector are each written by one thread and read after join().
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include <sys/socket.h>

#include <opencv2/highgui.hpp>

#include "bench/metrics.h"
#include "common/frame.h"
#include "common/frame_queue.h"
#include "common/protocol.h"
#include "common/util.h"
#include "net/tcp.h"
#include "net/udp.h"

namespace {

constexpr int HELLO_RETRY_MS = 500;
constexpr int HELLO_MAX_TRIES = 10;
constexpr int UDP_POLL_MS = 100;
constexpr int PLAY_POLL_MS = 100;
constexpr char WINDOW_NAME[] = "netpkt";

struct Opts {
    std::string host = "127.0.0.1";
    std::uint16_t port = 9000;
    bool udp = false;
    std::string log;
    std::string summary;
    std::string run_id;
    bool display = true;
    unsigned long queue_cap = 32;
    unsigned long stall_ms = 100;
    unsigned long reasm_ms = 500;
    unsigned long idle_ms = 3000;
};

void usage(const char* prog)
{
    std::fprintf(stderr,
        "usage: %s [options]\n"
        "  --host H           server address (default 127.0.0.1)\n"
        "  --port N           server port (default 9000)\n"
        "  --protocol P       tcp | udp (default tcp; must match the server)\n"
        "  --log FILE         per-frame CSV\n"
        "  --summary FILE     one-row summary CSV (default: FILE with .csv -> _summary.csv)\n"
        "  --run-id ID        label stored in the CSV (default run<unix time>)\n"
        "  --no-display       decode but do not open a window (headless / SSH)\n"
        "  --queue N          play queue size in frames (default 32)\n"
        "  --stall-ms N       a frame shown N ms later than the sender's spacing is a stall (default 100)\n"
        "  --reasm-ms N       UDP: abandon an incomplete frame after N ms (default 500)\n"
        "  --idle-ms N        UDP: stop after N ms of silence if END was lost (default 3000)\n",
        prog);
}

int parse_args(int argc, char** argv, Opts& o)
{
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
        if (a == "--no-display") {
            o.display = false;
        } else if (a == "--host") {
            const char* v = value();
            if (!v) return 2;
            o.host = v;
        } else if (a == "--log") {
            const char* v = value();
            if (!v) return 2;
            o.log = v;
        } else if (a == "--summary") {
            const char* v = value();
            if (!v) return 2;
            o.summary = v;
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
        } else if (a == "--queue") {
            const char* v = value();
            if (!v || !parse_uint(v, 100000, u) || u == 0) {
                std::fprintf(stderr, "error: --queue must be 1..100000\n");
                return 2;
            }
            o.queue_cap = u;
        } else if (a == "--stall-ms" || a == "--reasm-ms" || a == "--idle-ms") {
            const char* v = value();
            if (!v || !parse_uint(v, 3600000, u)) {
                std::fprintf(stderr, "error: %s must be 0..3600000\n", a.c_str());
                return 2;
            }
            (a == "--stall-ms" ? o.stall_ms : a == "--reasm-ms" ? o.reasm_ms : o.idle_ms) = u;
        } else {
            std::fprintf(stderr, "error: unknown argument '%s'\n", a.c_str());
            return 2;
        }
    }
    if (o.run_id.empty())
        o.run_id = "run" + std::to_string(static_cast<long long>(std::time(nullptr)));
    if (o.summary.empty() && !o.log.empty()) {
        o.summary = o.log;
        const std::string ext = ".csv";
        if (o.summary.size() > ext.size() &&
            o.summary.compare(o.summary.size() - ext.size(), ext.size(), ext) == 0)
            o.summary.erase(o.summary.size() - ext.size());
        o.summary += "_summary.csv";
    }
    return 0;
}

// ---- receive side -------------------------------------------------------------

// Record a complete frame and hand it to the display without ever blocking.
void deliver(Frame&& f, std::uint64_t recv_us, FrameQueue& q, RxLog& rx)
{
    rx.add_complete(f.id, static_cast<std::uint32_t>(f.data.size()), f.timestamp_us, recv_us);
    std::uint32_t dropped = 0;
    if (q.push(std::move(f), dropped))
        rx.mark_queue_drop(dropped);
}

bool receive_tcp(int fd, FrameQueue& q, RxLog& rx)
{
    for (;;) {
        Frame f;
        EndInfo end;
        TcpMsg m = tcp_recv_msg(fd, f, end);
        std::uint64_t t = now_us(); // the frame is complete right now
        switch (m) {
        case TcpMsg::FRAME:
            deliver(std::move(f), t, q, rx);
            break;
        case TcpMsg::END:
            rx.end = end;
            rx.end_received = true;
            return true;
        case TcpMsg::CLOSED:
            if (!stop_requested())
                std::fprintf(stderr, "warning: server closed the connection without END\n");
            return stop_requested();
        case TcpMsg::ERROR:
            if (!stop_requested())
                std::fprintf(stderr, "error: TCP receive failed\n");
            return false;
        }
    }
}

void drain_expired(UdpAssembler& reasm, RxLog& rx)
{
    std::vector<ExpiredFrame> gone;
    reasm.take_expired(gone);
    for (const ExpiredFrame& e : gone)
        rx.add_incomplete(e.frame_id, e.frame_len, e.timestamp_us);
}

bool receive_udp(int fd, std::uint32_t session, const Opts& o, FrameQueue& q, RxLog& rx)
{
    UdpAssembler reasm(o.reasm_ms * 1000ull);
    // Bigger than any valid datagram so an oversized one is rejected, not truncated into validity.
    std::uint8_t buf[2048];
    bool got_first = false, ok = true;
    int hello_tries = 0;
    std::uint64_t last_hello = 0, last_rx = 0;

    while (!stop_requested()) {
        std::uint64_t now = now_us();
        // Until the server answers, repeat HELLO: the first one may be lost or
        // may arrive before the server is listening.
        if (!got_first && (hello_tries == 0 || now - last_hello >= HELLO_RETRY_MS * 1000ull)) {
            if (hello_tries >= HELLO_MAX_TRIES) {
                std::fprintf(stderr, "error: no response from %s:%u after %d HELLOs "
                                     "(is video_server running with --protocol udp?)\n",
                             o.host.c_str(), o.port, HELLO_MAX_TRIES);
                ok = false;
                break;
            }
            if (!udp_send_hello(fd, session)) {
                ok = false;
                break;
            }
            hello_tries++;
            last_hello = now;
        }

        int n = udp_recv(fd, buf, sizeof buf, UDP_POLL_MS);
        now = now_us();
        if (n < 0) {
            ok = false;
            break;
        }
        if (n == 0) { // silence
            reasm.expire(now);
            drain_expired(reasm, rx);
            if (rx.end_received)
                break; // END seen and the line has gone quiet: stragglers had their chance
            if (got_first && now - last_rx > o.idle_ms * 1000ull) {
                std::fprintf(stderr, "warning: no data for %lu ms and no END; stopping "
                                     "(frames after the last one seen cannot be counted)\n",
                             o.idle_ms);
                break;
            }
            continue;
        }

        reasm.stats().dgrams++;
        UdpHdr h;
        if (decode_udp_hdr(buf, static_cast<std::size_t>(n), h) != ProtoStatus::OK) {
            reasm.stats().bad_hdr++;
            continue;
        }
        if (h.session != session) { // leftover traffic from another run
            reasm.stats().foreign++;
            continue;
        }
        got_first = true;
        last_rx = now;

        if (h.type == MSG_END) {
            EndInfo e;
            if (decode_end_info(buf + UDP_HDR_LEN, h.payload_len, e)) {
                rx.end = e;
                rx.end_received = true;
            } else {
                reasm.stats().bad_hdr++;
            }
        } else if (h.type == MSG_FRAME) {
            Frame f;
            if (reasm.add(h, buf + UDP_HDR_LEN, now, f) == UdpAddResult::COMPLETE)
                deliver(std::move(f), now, q, rx);
            reasm.expire(now);
            drain_expired(reasm, rx);
        }
    }

    reasm.flush(); // whatever is still incomplete now never will be
    drain_expired(reasm, rx);
    rx.udp = reasm.stats();
    return ok;
}

// ---- play side ------------------------------------------------------------------

// Policy for both protocols: show each complete frame as soon as it is popped,
// in completion order. No jitter buffer, no catch-up.
void play_loop(FrameQueue& q, bool show, std::vector<PlayRec>& play)
{
    Frame f;
    while (!stop_requested()) {
        FrameQueue::Pop st = q.pop(f, PLAY_POLL_MS);
        if (st == FrameQueue::Pop::CLOSED)
            break;
        if (st == FrameQueue::Pop::TIMEOUT)
            continue;

        PlayRec pr;
        pr.frame_id = f.id;
        pr.send_us = f.timestamp_us;
        cv::Mat img;
        std::uint64_t t0 = now_us();
        pr.decode_ok = jpeg_decode(f.data, img);
        pr.decode_us = now_us() - t0;
        if (pr.decode_ok) {
            if (show) {
                cv::imshow(WINDOW_NAME, img);
                pr.play_us = now_us();
                if (cv::waitKey(1) == 27) // ESC
                    request_stop();
            } else {
                pr.play_us = now_us();
            }
        } else {
            std::fprintf(stderr, "warning: frame %u could not be decoded\n", f.id);
        }
        play.push_back(pr);
    }
}

} // namespace

int main(int argc, char** argv)
{
    Opts o;
    int rc = parse_args(argc, argv, o);
    if (rc != 0) {
        usage(argv[0]);
        return rc;
    }
    if (o.display && std::getenv("DISPLAY") == nullptr && std::getenv("WAYLAND_DISPLAY") == nullptr) {
        std::fprintf(stderr, "error: no display available (DISPLAY/WAYLAND_DISPLAY unset); "
                             "use --no-display for headless runs\n");
        return 1;
    }
    install_stop_handler();
    const char* proto = o.udp ? "udp" : "tcp";

    std::printf("client settings: protocol=%s host=%s port=%u display=%s queue=%lu stall_ms=%lu "
                "run_id=%s", proto, o.host.c_str(), o.port, o.display ? "yes" : "no",
                o.queue_cap, o.stall_ms, o.run_id.c_str());
    if (o.udp)
        std::printf(" reasm_ms=%lu idle_ms=%lu", o.reasm_ms, o.idle_ms);
    std::printf("\n");

    // ---- connect ------------------------------------------------------------------
    Fd conn;
    std::uint32_t session = 0;
    if (o.udp) {
        std::random_device rd;
        session = rd();
        if (session == 0)
            session = 1;
        conn = udp_connect_client(o.host, o.port);
    } else {
        conn = tcp_connect(o.host, o.port);
    }
    if (!conn.valid())
        return 1;
    if (!o.udp)
        std::printf("connected to %s:%u\n", o.host.c_str(), o.port);

    // ---- run -----------------------------------------------------------------------
    FrameQueue queue(o.queue_cap);
    RxLog rx;
    std::vector<PlayRec> play;
    bool rx_ok = true;

    std::thread receiver([&] {
        rx_ok = o.udp ? receive_udp(conn.get(), session, o, queue, rx)
                      : receive_tcp(conn.get(), queue, rx);
        queue.close(); // lets the play loop finish after the last frame
    });

    play_loop(queue, o.display, play);
    if (stop_requested() && !o.udp)
        shutdown(conn.get(), SHUT_RDWR); // unblock a recv() in the receive thread
    receiver.join();
    if (o.display)
        cv::destroyAllWindows();

    // ---- report ----------------------------------------------------------------------
    if (!o.udp && !rx.end_received && rx.frames.empty() && !rx_ok)
        return 1; // nothing was received at all
    if (o.udp && rx.frames.empty() && !rx.end_received && !rx_ok)
        return 1;

    std::vector<FrameRec> table = build_frame_table(rx, play);
    RunSummary sum = compute_summary(proto, o.run_id, table, rx, play, o.stall_ms * 1000ull);
    print_summary(stdout, sum);
    if (stop_requested())
        std::printf("NOTE: run was interrupted; the numbers above are partial.\n");

    bool wrote_ok = true;
    if (!o.log.empty())
        wrote_ok = write_frame_csv(o.log, proto, o.run_id, table) && wrote_ok;
    if (!o.summary.empty())
        wrote_ok = write_summary_csv(o.summary, sum) && wrote_ok;
    return (rx_ok && wrote_ok) ? 0 : 1;
}
