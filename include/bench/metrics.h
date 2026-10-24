// Per-frame records, run summary, and CSV output. Everything here is computed
// from what the client observed; nothing assumes how TCP or UDP "should" behave.
#pragma once

#include <cstdint>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

#include "common/protocol.h"
#include "net/udp.h" // UdpStats

enum class RxStatus {
    COMPLETE,    // every byte of the frame arrived
    INCOMPLETE,  // some fragments arrived, reassembly was abandoned (UDP)
    MISSING,     // nothing of this frame was ever seen
};
enum class DecodeStatus { NONE, OK, FAIL };

const char* rx_status_str(RxStatus s);
const char* decode_status_str(DecodeStatus s);

struct FrameRec {
    std::uint32_t frame_id = 0;
    std::uint32_t size = 0;        // JPEG bytes; 0 if unknown (MISSING)
    std::uint64_t send_us = 0;     // sender clock; 0 if unknown
    std::uint64_t recv_us = 0;     // client clock when the frame became complete; 0 if never
    RxStatus status = RxStatus::MISSING;
    bool queue_dropped = false;    // complete, but discarded because the play queue was full
    DecodeStatus decode = DecodeStatus::NONE;
    std::uint64_t decode_us = 0;
    std::uint64_t play_us = 0;     // client clock when handed to the display; 0 if never
};

// Written by the display thread, one per frame it popped from the queue.
struct PlayRec {
    std::uint32_t frame_id = 0;
    std::uint64_t send_us = 0;
    bool decode_ok = false;
    std::uint64_t decode_us = 0;
    std::uint64_t play_us = 0;
};

// Written only by the receive thread; read after that thread is joined.
struct RxLog {
    std::map<std::uint32_t, FrameRec> frames; // only frames of which something arrived
    bool end_received = false;
    EndInfo end;
    UdpStats udp;

    void add_complete(std::uint32_t id, std::uint32_t size, std::uint64_t send_us,
                      std::uint64_t recv_us);
    void add_incomplete(std::uint32_t id, std::uint32_t size, std::uint64_t send_us);
    void mark_queue_drop(std::uint32_t id);
};

struct RunSummary {
    std::string protocol;
    std::string run_id;
    std::uint32_t frames_offered = 0;
    bool end_received = false;
    std::uint32_t frames_complete = 0;
    std::uint32_t frames_incomplete = 0;
    std::uint32_t frames_missing = 0;
    double frame_loss_pct = 0;
    std::uint64_t tx_bytes = 0;      // from the END message; 0 if END never arrived
    std::uint64_t rx_bytes = 0;      // JPEG bytes of complete frames
    double throughput_mbps = 0;
    double arrival_fps = 0;
    // Transfer latency of complete frames, in ms. NaN = not enough samples.
    double lat_mean = 0, lat_median = 0, lat_p95 = 0, lat_p99 = 0;
    double rel_mean = 0, rel_median = 0, rel_p95 = 0, rel_p99 = 0;
    std::uint32_t decode_failures = 0;
    double decode_mean_ms = 0;
    std::uint32_t queue_drops = 0;
    std::uint32_t frames_shown = 0;
    double shown_fps = 0;
    std::uint32_t stall_count = 0;
    double stall_total_ms = 0;
    UdpStats udp;
    bool is_udp = false;
};

constexpr std::size_t MIN_SAMPLES_P95 = 20;
constexpr std::size_t MIN_SAMPLES_P99 = 100;

// Nearest-rank percentile: sorted[ceil(p/100 * n) - 1]. NaN for an empty input.
double percentile(std::vector<double> v, double p);
std::string csv_escape(const std::string& s);

// Dense table indexed by frame id: 0 .. max(END.frames, highest id seen + 1) - 1.
std::vector<FrameRec> build_frame_table(const RxLog& rx, const std::vector<PlayRec>& play);

RunSummary compute_summary(const std::string& protocol, const std::string& run_id,
                           const std::vector<FrameRec>& table, const RxLog& rx,
                           const std::vector<PlayRec>& play, std::uint64_t stall_threshold_us);

bool write_frame_csv(const std::string& path, const std::string& protocol,
                     const std::string& run_id, const std::vector<FrameRec>& table);
bool write_summary_csv(const std::string& path, const RunSummary& s);
void print_summary(std::FILE* out, const RunSummary& s);

// Server-side record of what was sent.
struct SendRec {
    std::uint32_t frame_id = 0;
    std::uint32_t size = 0;
    std::uint64_t send_us = 0;       // timestamp put in the frame header
    std::uint64_t send_call_us = 0;  // how long the send call took
};
bool write_send_csv(const std::string& path, const std::string& protocol,
                    const std::string& run_id, const std::vector<SendRec>& recs);
