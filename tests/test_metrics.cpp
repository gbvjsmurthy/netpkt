// Summary math is checked against numbers computed by hand.
#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "bench/metrics.h"
#include "check.h"

static bool near(double a, double b, double tol = 1e-6) { return std::fabs(a - b) < tol; }

// Split one CSV line honoring RFC 4180 quoting.
static std::vector<std::string> split_csv(const std::string& line)
{
    std::vector<std::string> out;
    std::string cur;
    bool q = false;
    for (std::size_t i = 0; i < line.size(); i++) {
        char c = line[i];
        if (q) {
            if (c == '"' && i + 1 < line.size() && line[i + 1] == '"') { cur += '"'; i++; }
            else if (c == '"') q = false;
            else cur += c;
        } else if (c == '"') q = true;
        else if (c == ',') { out.push_back(cur); cur.clear(); }
        else cur += c;
    }
    out.push_back(cur);
    return out;
}

static void test_percentile_and_csv_escape()
{
    std::vector<double> v;
    for (int i = 100; i >= 1; i--)
        v.push_back(i); // unsorted input on purpose
    CHECK(percentile(v, 50) == 50);
    CHECK(percentile(v, 95) == 95);
    CHECK(percentile(v, 99) == 99);
    CHECK(percentile(v, 100) == 100);
    CHECK(percentile({7.0}, 99) == 7.0);
    CHECK(std::isnan(percentile({}, 50)));
    CHECK(percentile({1, 2, 3, 4}, 50) == 2); // nearest-rank: ceil(0.5*4)=2nd value

    CHECK(csv_escape("plain") == "plain");
    CHECK(csv_escape("a,b") == "\"a,b\"");
    CHECK(csv_escape("say \"hi\"") == "\"say \"\"hi\"\"\"");
    CHECK(csv_escape("two\nlines") == "\"two\nlines\"");
}

// 10 frames offered. 0..7 complete (frame 3 arrives 50 ms late), 8 incomplete, 9 missing.
static void build_scenario(RxLog& rx, std::vector<PlayRec>& play)
{
    const std::uint64_t base = 5000000;
    rx.end_received = true;
    rx.end.frames = 10;
    rx.end.bytes = 10 * 1000;
    for (std::uint32_t i = 0; i < 8; i++) {
        std::uint64_t send = base + i * 33000;
        std::uint64_t recv = send + 4000 + (i == 3 ? 50000 : 0);
        rx.add_complete(i, 1000, send, recv);

        PlayRec p;
        p.frame_id = i;
        p.send_us = send;
        p.decode_ok = true;
        p.decode_us = 2000;
        // Frame 3 shows 50 ms late; frame 4 is shown on time again.
        p.play_us = recv + 100;
        play.push_back(p);
    }
    rx.add_incomplete(8, 1000, base + 8 * 33000);
}

static void test_summary()
{
    RxLog rx;
    std::vector<PlayRec> play;
    build_scenario(rx, play);
    auto table = build_frame_table(rx, play);
    CHECK(table.size() == 10);
    CHECK(table[8].status == RxStatus::INCOMPLETE && table[9].status == RxStatus::MISSING);
    CHECK(table[2].decode == DecodeStatus::OK && table[9].decode == DecodeStatus::NONE);

    RunSummary s = compute_summary("udp", "t1", table, rx, play, 20000 /*20 ms*/);
    CHECK(s.frames_offered == 10 && s.frames_complete == 8);
    CHECK(s.frames_incomplete == 1 && s.frames_missing == 1);
    CHECK(near(s.frame_loss_pct, 20.0)); // (10 - 8) / 10
    CHECK(s.tx_bytes == 10000 && s.rx_bytes == 8000);

    // first arrival: frame 0 at base+4000; last: frame 7 at base+7*33000+4000.
    // throughput = bytes of frames 1..7 (7000 B) * 8 bits / span (231000 us) = 0.242424 Mbps
    CHECK(near(s.throughput_mbps, 7000.0 * 8 / 231000.0, 1e-9));
    CHECK(near(s.arrival_fps, 7 / 0.231, 1e-6));

    // latencies: 4 ms for seven frames, 54 ms for frame 3
    CHECK(near(s.lat_mean, (7 * 4.0 + 54.0) / 8));
    CHECK(near(s.lat_median, 4.0));
    CHECK(std::isnan(s.lat_p95) && std::isnan(s.lat_p99)); // 8 samples: not enough
    CHECK(near(s.rel_median, 0.0) && near(s.rel_mean, 50.0 / 8)); // minimum (4 ms) subtracted

    // Stall: frame 3 shown 50 ms after the sender's spacing; the burst-recovery
    // frame 4 is early, not a stall. 20 ms threshold -> exactly one stall of 50 ms.
    CHECK(s.stall_count == 1);
    CHECK(near(s.stall_total_ms, 50.0));
    CHECK(s.frames_shown == 8 && s.decode_failures == 0);
    CHECK(near(s.decode_mean_ms, 2.0));

    // Raising the threshold above 50 ms removes the stall.
    RunSummary s2 = compute_summary("udp", "t1", table, rx, play, 60000);
    CHECK(s2.stall_count == 0);
}

static void test_summary_edge_cases()
{
    RxLog rx;
    std::vector<PlayRec> play;
    auto table = build_frame_table(rx, play);
    RunSummary s = compute_summary("tcp", "empty", table, rx, play, 100000);
    CHECK(table.empty() && s.frames_offered == 0 && s.frame_loss_pct == 0);
    CHECK(std::isnan(s.throughput_mbps) && std::isnan(s.lat_mean));

    // END lost: totals come from the highest id seen, and are flagged as such.
    RxLog rx2;
    rx2.add_complete(0, 100, 10, 20);
    rx2.add_complete(4, 100, 50, 60);
    auto t2 = build_frame_table(rx2, play);
    RunSummary s2 = compute_summary("udp", "noend", t2, rx2, play, 100000);
    CHECK(!s2.end_received && s2.frames_offered == 5 && s2.frames_missing == 3);

    // 100 samples are enough for p99; 20 for p95.
    RxLog rx3;
    for (std::uint32_t i = 0; i < 100; i++)
        rx3.add_complete(i, 10, 1000 + i * 100, 1000 + i * 100 + 1000 + i * 10); // latency 1.0..1.99 ms
    auto t3 = build_frame_table(rx3, play);
    RunSummary s3 = compute_summary("tcp", "many", t3, rx3, play, 100000);
    CHECK(near(s3.lat_p99, 1.0 + 98 * 0.01) && near(s3.lat_p95, 1.0 + 94 * 0.01));

    // a decode failure is counted and the frame is not "shown"
    PlayRec bad;
    bad.frame_id = 0;
    bad.decode_ok = false;
    std::vector<PlayRec> p4 = {bad};
    auto t4 = build_frame_table(rx2, p4);
    RunSummary s4 = compute_summary("udp", "dec", t4, rx2, p4, 100000);
    CHECK(s4.decode_failures == 1 && s4.frames_shown == 0 && t4[0].decode == DecodeStatus::FAIL);
}

static void test_csv_files()
{
    RxLog rx;
    std::vector<PlayRec> play;
    build_scenario(rx, play);
    auto table = build_frame_table(rx, play);
    const std::string weird_id = "run,\"A\"";

    const std::string path = "test_metrics_frames.csv";
    CHECK(write_frame_csv(path, "udp", weird_id, table));
    std::ifstream in(path);
    std::string line;
    std::getline(in, line);
    std::size_t ncols = split_csv(line).size();
    CHECK(ncols == 13);
    int rows = 0;
    while (std::getline(in, line)) {
        auto f = split_csv(line);
        CHECK(f.size() == ncols);          // quoting must not shift columns
        CHECK(f[1] == weird_id);           // ... and must round-trip exactly
        CHECK(f[2] == std::to_string(rows));
        if (rows == 9) CHECK(f[8] == "missing" && f[3].empty() && f[5].empty());
        if (rows == 8) CHECK(f[8] == "incomplete" && f[3] == "1000" && f[5].empty());
        if (rows == 0) CHECK(f[8] == "complete" && f[9] == "ok");
        rows++;
    }
    CHECK(rows == 10);
    std::remove(path.c_str());

    RunSummary s = compute_summary("udp", weird_id, table, rx, play, 20000);
    const std::string spath = "test_metrics_summary.csv";
    CHECK(write_summary_csv(spath, s));
    std::ifstream sin(spath);
    std::string head, row;
    std::getline(sin, head);
    std::getline(sin, row);
    auto h = split_csv(head), r = split_csv(row);
    CHECK(h.size() == r.size() && h.size() > 20);
    CHECK(r[1] == weird_id);
    for (std::size_t i = 0; i < h.size(); i++) {
        if (h[i] == "frames_complete") CHECK(r[i] == "8");
        if (h[i] == "frame_loss_pct") CHECK(r[i] == "20.000");
        if (h[i] == "latency_p95_ms") CHECK(r[i].empty()); // unknown, not 0
    }
    std::remove(spath.c_str());

    CHECK(!write_frame_csv("/nonexistent_dir/x.csv", "tcp", "r", table)); // error is reported
}

int main()
{
    test_percentile_and_csv_escape();
    test_summary();
    test_summary_edge_cases();
    test_csv_files();
    return TEST_RESULT();
}
