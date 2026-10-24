#include "bench/metrics.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <limits>

namespace {

const double NA = std::numeric_limits<double>::quiet_NaN();

double ms_between(std::uint64_t recv_us, std::uint64_t send_us)
{
    // Signed on purpose: across two hosts the clocks differ and this can be negative.
    return static_cast<double>(static_cast<std::int64_t>(recv_us) -
                               static_cast<std::int64_t>(send_us)) / 1000.0;
}

double mean_of(const std::vector<double>& v)
{
    if (v.empty())
        return NA;
    double sum = 0;
    for (double x : v)
        sum += x;
    return sum / static_cast<double>(v.size());
}

// Only report a percentile when there are enough samples for it to mean something.
double pct_if(const std::vector<double>& v, double p, std::size_t min_samples)
{
    return v.size() >= min_samples ? percentile(v, p) : NA;
}

// Number as text for CSV: empty when unknown.
std::string num(double v, int prec = 3)
{
    if (std::isnan(v))
        return "";
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.*f", prec, v);
    return buf;
}

std::string num_or_na(double v, int prec = 3)
{
    std::string s = num(v, prec);
    return s.empty() ? "n/a" : s;
}

// Smallest latency among complete frames: the clock-offset-free baseline.
double min_latency_ms(const std::vector<FrameRec>& table)
{
    double m = NA;
    for (const FrameRec& r : table) {
        if (r.status != RxStatus::COMPLETE || r.send_us == 0 || r.recv_us == 0)
            continue;
        double l = ms_between(r.recv_us, r.send_us);
        if (std::isnan(m) || l < m)
            m = l;
    }
    return m;
}

} // namespace

const char* rx_status_str(RxStatus s)
{
    switch (s) {
    case RxStatus::COMPLETE:   return "complete";
    case RxStatus::INCOMPLETE: return "incomplete";
    case RxStatus::MISSING:    return "missing";
    }
    return "?";
}

const char* decode_status_str(DecodeStatus s)
{
    switch (s) {
    case DecodeStatus::NONE: return "none";
    case DecodeStatus::OK:   return "ok";
    case DecodeStatus::FAIL: return "fail";
    }
    return "?";
}

void RxLog::add_complete(std::uint32_t id, std::uint32_t size, std::uint64_t send_us,
                         std::uint64_t recv_us)
{
    FrameRec& r = frames[id];
    r.frame_id = id;
    r.size = size;
    r.send_us = send_us;
    r.recv_us = recv_us;
    r.status = RxStatus::COMPLETE;
}

void RxLog::add_incomplete(std::uint32_t id, std::uint32_t size, std::uint64_t send_us)
{
    FrameRec& r = frames[id];
    if (r.status == RxStatus::COMPLETE)
        return; // never downgrade a frame that did arrive
    r.frame_id = id;
    r.size = size;
    r.send_us = send_us;
    r.status = RxStatus::INCOMPLETE;
}

void RxLog::mark_queue_drop(std::uint32_t id)
{
    auto it = frames.find(id);
    if (it != frames.end())
        it->second.queue_dropped = true;
}

double percentile(std::vector<double> v, double p)
{
    if (v.empty())
        return NA;
    std::sort(v.begin(), v.end());
    double rank = std::ceil(p / 100.0 * static_cast<double>(v.size()));
    std::size_t idx = rank < 1 ? 0 : static_cast<std::size_t>(rank) - 1;
    if (idx >= v.size())
        idx = v.size() - 1;
    return v[idx];
}

std::string csv_escape(const std::string& s)
{
    if (s.find_first_of(",\"\r\n") == std::string::npos)
        return s;
    std::string out = "\"";
    for (char c : s) {
        if (c == '"')
            out += '"'; // a quote inside a quoted field is doubled
        out += c;
    }
    out += '"';
    return out;
}

std::vector<FrameRec> build_frame_table(const RxLog& rx, const std::vector<PlayRec>& play)
{
    // Ids are bounded by MAX_FRAME_ID (checked when headers are decoded), so
    // this table cannot grow without limit.
    std::uint32_t total = 0;
    if (!rx.frames.empty())
        total = rx.frames.rbegin()->first + 1;
    if (rx.end_received && rx.end.frames > total)
        total = rx.end.frames;

    std::vector<FrameRec> t(total);
    for (std::uint32_t i = 0; i < total; i++)
        t[i].frame_id = i;
    for (const auto& kv : rx.frames)
        t[kv.first] = kv.second;
    for (const PlayRec& p : play) {
        if (p.frame_id >= total)
            continue;
        FrameRec& r = t[p.frame_id];
        r.decode = p.decode_ok ? DecodeStatus::OK : DecodeStatus::FAIL;
        r.decode_us = p.decode_us;
        r.play_us = p.play_us;
    }
    return t;
}

RunSummary compute_summary(const std::string& protocol, const std::string& run_id,
                           const std::vector<FrameRec>& table, const RxLog& rx,
                           const std::vector<PlayRec>& play, std::uint64_t stall_threshold_us)
{
    RunSummary s;
    s.protocol = protocol;
    s.run_id = run_id;
    s.is_udp = (protocol == "udp");
    s.udp = rx.udp;
    s.frames_offered = static_cast<std::uint32_t>(table.size());
    s.end_received = rx.end_received;
    s.tx_bytes = rx.end_received ? rx.end.bytes : 0;

    std::vector<double> lat, rel;
    std::uint64_t first_recv = 0, last_recv = 0, first_size = 0;
    bool have_recv = false;
    double base = min_latency_ms(table);

    for (const FrameRec& r : table) {
        if (r.queue_dropped)
            s.queue_drops++;
        if (r.status == RxStatus::INCOMPLETE) {
            s.frames_incomplete++;
            continue;
        }
        if (r.status == RxStatus::MISSING) {
            s.frames_missing++;
            continue;
        }
        s.frames_complete++;
        s.rx_bytes += r.size;
        double l = ms_between(r.recv_us, r.send_us);
        lat.push_back(l);
        rel.push_back(l - base);
        if (!have_recv || r.recv_us < first_recv) {
            first_recv = r.recv_us;
            first_size = r.size;
        }
        if (!have_recv || r.recv_us > last_recv)
            last_recv = r.recv_us;
        have_recv = true;
    }

    // Loss = frames that never became complete / frames the sender offered.
    s.frame_loss_pct = s.frames_offered == 0
        ? 0.0
        : 100.0 * (s.frames_offered - s.frames_complete) / s.frames_offered;

    // Goodput: bytes of frames 2..N over the time between first and last arrival
    // (frame 1's bytes arrived before the clock started). bits/us == Mbit/s.
    s.throughput_mbps = NA;
    s.arrival_fps = NA;
    if (s.frames_complete >= 2 && last_recv > first_recv) {
        double span_us = static_cast<double>(last_recv - first_recv);
        s.throughput_mbps = static_cast<double>(s.rx_bytes - first_size) * 8.0 / span_us;
        s.arrival_fps = (s.frames_complete - 1) / (span_us / 1e6);
    }

    s.lat_mean = mean_of(lat);
    s.lat_median = percentile(lat, 50);
    s.lat_p95 = pct_if(lat, 95, MIN_SAMPLES_P95);
    s.lat_p99 = pct_if(lat, 99, MIN_SAMPLES_P99);
    s.rel_mean = mean_of(rel);
    s.rel_median = percentile(rel, 50);
    s.rel_p95 = pct_if(rel, 95, MIN_SAMPLES_P95);
    s.rel_p99 = pct_if(rel, 99, MIN_SAMPLES_P99);

    // Playback: frames are shown in the order the display thread popped them.
    std::vector<double> dec_ms;
    bool have_prev = false;
    std::uint64_t prev_play = 0, prev_send = 0, first_play = 0;
    for (const PlayRec& p : play) {
        if (!p.decode_ok) {
            s.decode_failures++;
            continue;
        }
        dec_ms.push_back(static_cast<double>(p.decode_us) / 1000.0);
        s.frames_shown++;
        if (have_prev) {
            // A stall = the frame appeared later than the sender's own spacing
            // says it should have, beyond a tolerance. Using the sender's
            // timestamps means skipped (lost) frames do not count as stalls;
            // they are already counted as loss.
            std::int64_t actual = static_cast<std::int64_t>(p.play_us - prev_play);
            std::int64_t expected = p.send_us > prev_send
                ? static_cast<std::int64_t>(p.send_us - prev_send) : 0;
            std::int64_t extra = actual - expected;
            if (extra > static_cast<std::int64_t>(stall_threshold_us)) {
                s.stall_count++;
                s.stall_total_ms += static_cast<double>(extra) / 1000.0;
            }
        } else {
            first_play = p.play_us;
        }
        have_prev = true;
        prev_play = p.play_us;
        prev_send = p.send_us;
    }
    s.decode_mean_ms = mean_of(dec_ms);
    s.shown_fps = NA;
    if (s.frames_shown >= 2 && prev_play > first_play)
        s.shown_fps = (s.frames_shown - 1) / (static_cast<double>(prev_play - first_play) / 1e6);
    return s;
}

bool write_frame_csv(const std::string& path, const std::string& protocol,
                     const std::string& run_id, const std::vector<FrameRec>& table)
{
    std::ofstream f(path);
    if (!f) {
        std::fprintf(stderr, "netpkt: cannot open '%s' for writing\n", path.c_str());
        return false;
    }
    double base = min_latency_ms(table);
    f << "protocol,run_id,frame_id,size_bytes,send_us,recv_us,latency_ms,rel_delay_ms,"
         "rx_status,decode_status,decode_us,play_us,queue_dropped\n";
    for (const FrameRec& r : table) {
        bool done = r.status == RxStatus::COMPLETE;
        double lat = done ? ms_between(r.recv_us, r.send_us) : NA;
        // Unknown values are left empty rather than written as 0.
        f << csv_escape(protocol) << ',' << csv_escape(run_id) << ',' << r.frame_id << ','
          << (r.size ? std::to_string(r.size) : "") << ','
          << (r.send_us ? std::to_string(r.send_us) : "") << ','
          << (r.recv_us ? std::to_string(r.recv_us) : "") << ','
          << num(lat) << ',' << num(done ? lat - base : NA) << ','
          << rx_status_str(r.status) << ',' << decode_status_str(r.decode) << ','
          << (r.decode == DecodeStatus::NONE ? "" : std::to_string(r.decode_us)) << ','
          << (r.play_us ? std::to_string(r.play_us) : "") << ','
          << (r.queue_dropped ? 1 : 0) << '\n';
    }
    f.flush();
    if (!f) {
        std::fprintf(stderr, "netpkt: write error on '%s'\n", path.c_str());
        return false;
    }
    return true;
}

bool write_summary_csv(const std::string& path, const RunSummary& s)
{
    std::ofstream f(path);
    if (!f) {
        std::fprintf(stderr, "netpkt: cannot open '%s' for writing\n", path.c_str());
        return false;
    }
    f << "protocol,run_id,frames_offered,end_received,frames_complete,frames_incomplete,"
         "frames_missing,frame_loss_pct,tx_payload_bytes,rx_payload_bytes,throughput_mbps,"
         "arrival_fps,latency_mean_ms,latency_median_ms,latency_p95_ms,latency_p99_ms,"
         "reldelay_mean_ms,reldelay_median_ms,reldelay_p95_ms,reldelay_p99_ms,"
         "decode_failures,decode_mean_ms,queue_drops,frames_shown,shown_fps,stall_count,"
         "stall_total_ms,udp_datagrams,udp_bad_hdr,udp_foreign,udp_invalid,udp_dup,udp_late,"
         "udp_reordered\n";
    auto u = [&](std::uint64_t v) { return s.is_udp ? std::to_string(v) : std::string(); };
    f << csv_escape(s.protocol) << ',' << csv_escape(s.run_id) << ',' << s.frames_offered << ','
      << (s.end_received ? 1 : 0) << ',' << s.frames_complete << ',' << s.frames_incomplete << ','
      << s.frames_missing << ',' << num(s.frame_loss_pct) << ','
      << (s.end_received ? std::to_string(s.tx_bytes) : "") << ',' << s.rx_bytes << ','
      << num(s.throughput_mbps) << ',' << num(s.arrival_fps) << ','
      << num(s.lat_mean) << ',' << num(s.lat_median) << ',' << num(s.lat_p95) << ','
      << num(s.lat_p99) << ',' << num(s.rel_mean) << ',' << num(s.rel_median) << ','
      << num(s.rel_p95) << ',' << num(s.rel_p99) << ',' << s.decode_failures << ','
      << num(s.decode_mean_ms) << ',' << s.queue_drops << ',' << s.frames_shown << ','
      << num(s.shown_fps) << ',' << s.stall_count << ',' << num(s.stall_total_ms) << ','
      << u(s.udp.dgrams) << ',' << u(s.udp.bad_hdr) << ',' << u(s.udp.foreign) << ','
      << u(s.udp.invalid) << ',' << u(s.udp.dup) << ',' << u(s.udp.late) << ','
      << u(s.udp.reordered) << '\n';
    f.flush();
    if (!f) {
        std::fprintf(stderr, "netpkt: write error on '%s'\n", path.c_str());
        return false;
    }
    return true;
}

void print_summary(std::FILE* out, const RunSummary& s)
{
    std::fprintf(out, "---- run summary: protocol=%s run_id=%s ----\n", s.protocol.c_str(),
                 s.run_id.c_str());
    std::fprintf(out, "frames offered by server : %u%s\n", s.frames_offered,
                 s.end_received ? "" : "  (END not received: counted up to highest id seen)");
    std::fprintf(out, "frames complete          : %u\n", s.frames_complete);
    std::fprintf(out, "frames incomplete        : %u\n", s.frames_incomplete);
    std::fprintf(out, "frames missing           : %u\n", s.frames_missing);
    std::fprintf(out, "frame loss               : %.3f %% (offered - complete) / offered\n",
                 s.frame_loss_pct);
    if (s.end_received)
        std::fprintf(out, "payload bytes sent       : %llu\n",
                     static_cast<unsigned long long>(s.tx_bytes));
    else
        std::fprintf(out, "payload bytes sent       : n/a\n");
    std::fprintf(out, "payload bytes received   : %llu\n",
                 static_cast<unsigned long long>(s.rx_bytes));
    std::fprintf(out, "app throughput           : %s Mbps (JPEG payload only)\n",
                 num_or_na(s.throughput_mbps).c_str());
    std::fprintf(out, "arrival frame rate       : %s fps\n", num_or_na(s.arrival_fps).c_str());
    std::fprintf(out, "latency ms  (raw)        : mean %s  median %s  p95 %s  p99 %s\n",
                 num_or_na(s.lat_mean).c_str(), num_or_na(s.lat_median).c_str(),
                 num_or_na(s.lat_p95).c_str(), num_or_na(s.lat_p99).c_str());
    std::fprintf(out, "delay ms (minus min)     : mean %s  median %s  p95 %s  p99 %s\n",
                 num_or_na(s.rel_mean).c_str(), num_or_na(s.rel_median).c_str(),
                 num_or_na(s.rel_p95).c_str(), num_or_na(s.rel_p99).c_str());
    std::fprintf(out, "decoded+shown frames     : %u (decode failures %u, mean decode %s ms, "
                      "dropped by full queue %u)\n",
                 s.frames_shown, s.decode_failures, num_or_na(s.decode_mean_ms).c_str(),
                 s.queue_drops);
    std::fprintf(out, "shown frame rate         : %s fps\n", num_or_na(s.shown_fps).c_str());
    std::fprintf(out, "playback stalls          : %u (total %.1f ms)\n", s.stall_count,
                 s.stall_total_ms);
    if (s.is_udp)
        std::fprintf(out, "udp datagrams            : %llu read, %llu bad header, %llu foreign "
                          "session, %llu inconsistent, %llu duplicate, %llu late, %llu reordered\n",
                     static_cast<unsigned long long>(s.udp.dgrams),
                     static_cast<unsigned long long>(s.udp.bad_hdr),
                     static_cast<unsigned long long>(s.udp.foreign),
                     static_cast<unsigned long long>(s.udp.invalid),
                     static_cast<unsigned long long>(s.udp.dup),
                     static_cast<unsigned long long>(s.udp.late),
                     static_cast<unsigned long long>(s.udp.reordered));
    std::fprintf(out, "note: 'raw' latency compares two clocks; valid on one host only. "
                      "'delay minus min' cancels a constant clock offset.\n");
}

bool write_send_csv(const std::string& path, const std::string& protocol,
                    const std::string& run_id, const std::vector<SendRec>& recs)
{
    std::ofstream f(path);
    if (!f) {
        std::fprintf(stderr, "netpkt: cannot open '%s' for writing\n", path.c_str());
        return false;
    }
    f << "protocol,run_id,frame_id,size_bytes,send_us,send_call_us\n";
    for (const SendRec& r : recs)
        f << csv_escape(protocol) << ',' << csv_escape(run_id) << ',' << r.frame_id << ','
          << r.size << ',' << r.send_us << ',' << r.send_call_us << '\n';
    f.flush();
    if (!f) {
        std::fprintf(stderr, "netpkt: write error on '%s'\n", path.c_str());
        return false;
    }
    return true;
}
