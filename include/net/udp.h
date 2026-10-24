// UDP transport: frames are cut into fragments that each fit in one datagram.
// There is NO retransmission, acknowledgement, or congestion control: UDP does
// not promise delivery or order, and this project measures exactly that.
#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <string>
#include <unordered_set>
#include <vector>

#include "common/frame.h"
#include "common/protocol.h"
#include "net/fd.h"

// ---- sockets ----------------------------------------------------------------

// Server: bind, wait for a valid HELLO, then connect() the socket to that
// client so the kernel drops datagrams from anyone else. session = client's id.
Fd udp_wait_hello(std::uint16_t port, std::uint32_t& session, std::string& peer);

// Client: socket connected to the server, with a larger receive buffer.
Fd udp_connect_client(const std::string& host, std::uint16_t port);

bool udp_send_hello(int fd, std::uint32_t session);

// Cut a frame into ready-to-send datagrams (empty vector if the size is invalid).
std::vector<std::vector<std::uint8_t>> udp_make_dgrams(std::uint32_t session, const Frame& f);

// Send all fragments of a frame back to back; gap_us > 0 sleeps between them.
// n_dgrams (optional) receives the number of datagrams sent.
bool udp_send_frame(int fd, std::uint32_t session, const Frame& f, unsigned gap_us,
                    std::size_t* n_dgrams);

// END is sent several times because a single lost datagram would hide it.
bool udp_send_end(int fd, std::uint32_t session, const EndInfo& e, int repeats);

// Wait up to timeout_ms for one datagram.
//   >0 bytes received, 0 timeout / interrupted / transient error, -1 fatal error
int udp_recv(int fd, std::uint8_t* buf, std::size_t cap, int timeout_ms);

// ---- reassembly -------------------------------------------------------------

struct UdpStats {
    std::uint64_t dgrams = 0;     // datagrams read from the socket
    std::uint64_t bad_hdr = 0;    // failed header validation
    std::uint64_t foreign = 0;    // valid but wrong session id
    std::uint64_t invalid = 0;    // valid header, but inconsistent with its frame
    std::uint64_t dup = 0;        // fragment already received
    std::uint64_t late = 0;       // fragment of a frame already completed/expired
    std::uint64_t reordered = 0;  // arrived after a later (frame, fragment) position
};

enum class UdpAddResult {
    STORED,     // kept; frame still incomplete
    COMPLETE,   // last missing fragment arrived; `out` holds the frame
    DUPLICATE,
    LATE,
    INVALID,
};

struct ExpiredFrame {
    std::uint32_t frame_id = 0;
    std::uint32_t frame_len = 0;
    std::uint64_t timestamp_us = 0;
    std::uint32_t frags_got = 0;
    std::uint32_t frags_total = 0;
};

// Collects fragments into frames. It never hands a partial frame to the caller.
// Time is passed in by the caller so tests are deterministic.
class UdpAssembler {
public:
    static constexpr std::size_t MAX_PENDING = 32;   // frames being assembled at once
    static constexpr std::size_t DONE_WINDOW = 4096; // finished ids remembered

    explicit UdpAssembler(std::uint64_t timeout_us) : timeout_us_(timeout_us) {}

    UdpAddResult add(const UdpHdr& h, const std::uint8_t* payload, std::uint64_t now_us,
                     Frame& out);

    // Give up on frames first seen at least timeout_us ago.
    void expire(std::uint64_t now_us);
    // Give up on everything still pending (end of stream).
    void flush();
    // Move the list of abandoned frames to `out` (appends) and clear it.
    void take_expired(std::vector<ExpiredFrame>& out);

    std::size_t pending_count() const { return pending_.size(); }
    UdpStats& stats() { return stats_; }

private:
    struct Pending {
        std::uint32_t frame_len = 0;
        std::uint32_t frag_cnt = 0;
        std::uint32_t got = 0;
        std::uint64_t timestamp_us = 0;
        std::uint64_t first_us = 0;
        std::vector<std::vector<std::uint8_t>> parts; // empty vector = not received yet
    };

    void abandon(std::map<std::uint32_t, Pending>::iterator it);
    void remember_done(std::uint32_t id);

    std::uint64_t timeout_us_;
    std::map<std::uint32_t, Pending> pending_;
    std::deque<std::uint32_t> done_order_;
    std::unordered_set<std::uint32_t> done_;
    std::vector<ExpiredFrame> expired_;
    std::uint64_t highest_pos_ = 0;
    bool any_pos_ = false;
    UdpStats stats_;
};
