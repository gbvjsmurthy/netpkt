#include "net/udp.h"

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/types.h>

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>

#include "common/util.h"

namespace {

// A burst of fragments can overflow the default receive buffer (~200 KB) and
// cause loss that has nothing to do with the network. Linux silently caps this
// request at net.core.rmem_max; the client prints what it actually got.
constexpr int UDP_RCVBUF_REQUEST = 4 * 1024 * 1024;

constexpr int END_REPEAT_GAP_MS = 20;

bool send_dgram(int fd, const std::uint8_t* buf, std::size_t len)
{
    for (;;) {
        ssize_t n = send(fd, buf, len, 0);
        if (n >= 0)
            return true;
        if (errno == EINTR && !stop_requested())
            continue;
        if (errno != EINTR)
            log_errno("send (UDP)");
        return false;
    }
}

} // namespace

Fd udp_wait_hello(std::uint16_t port, std::uint32_t& session, std::string& peer)
{
    Fd fd(::socket(AF_INET, SOCK_DGRAM, 0));
    if (!fd.valid()) {
        log_errno("socket");
        return Fd();
    }
    sockaddr_in addr;
    std::memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);
    if (bind(fd.get(), reinterpret_cast<sockaddr*>(&addr), sizeof addr) < 0) {
        std::fprintf(stderr, "netpkt: cannot bind UDP port %u: %s\n", port, std::strerror(errno));
        return Fd();
    }

    for (;;) {
        std::uint8_t buf[MAX_PKT_SIZE + 1];
        sockaddr_in from;
        socklen_t flen = sizeof from;
        ssize_t n = recvfrom(fd.get(), buf, sizeof buf, 0, reinterpret_cast<sockaddr*>(&from), &flen);
        if (n < 0) {
            if (errno == EINTR && !stop_requested())
                continue;
            if (errno != EINTR)
                log_errno("recvfrom");
            return Fd();
        }
        UdpHdr h;
        if (decode_udp_hdr(buf, static_cast<std::size_t>(n), h) != ProtoStatus::OK ||
            h.type != MSG_HELLO)
            continue; // stray traffic: keep waiting
        if (connect(fd.get(), reinterpret_cast<sockaddr*>(&from), flen) < 0) {
            log_errno("connect (UDP)");
            return Fd();
        }
        char ip[INET_ADDRSTRLEN] = "?";
        inet_ntop(AF_INET, &from.sin_addr, ip, sizeof ip);
        peer = std::string(ip) + ":" + std::to_string(ntohs(from.sin_port));
        session = h.session;
        return fd;
    }
}

Fd udp_connect_client(const std::string& host, std::uint16_t port)
{
    addrinfo hints;
    std::memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    addrinfo* res = nullptr;
    int rc = getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &res);
    if (rc != 0) {
        std::fprintf(stderr, "netpkt: cannot resolve '%s': %s\n", host.c_str(), gai_strerror(rc));
        return Fd();
    }
    Fd fd(::socket(res->ai_family, res->ai_socktype, res->ai_protocol));
    if (!fd.valid()) {
        log_errno("socket");
        freeaddrinfo(res);
        return Fd();
    }
    // connect() on UDP only fixes the peer address; nothing is sent yet.
    if (connect(fd.get(), res->ai_addr, res->ai_addrlen) < 0) {
        log_errno("connect (UDP)");
        freeaddrinfo(res);
        return Fd();
    }
    freeaddrinfo(res);

    int want = UDP_RCVBUF_REQUEST;
    if (setsockopt(fd.get(), SOL_SOCKET, SO_RCVBUF, &want, sizeof want) < 0)
        log_errno("setsockopt SO_RCVBUF (continuing)");
    int got = 0;
    socklen_t glen = sizeof got;
    if (getsockopt(fd.get(), SOL_SOCKET, SO_RCVBUF, &got, &glen) == 0)
        std::fprintf(stderr, "netpkt: UDP receive buffer: requested %d, kernel granted %d bytes\n",
                     want, got);
    return fd;
}

bool udp_send_hello(int fd, std::uint32_t session)
{
    std::uint8_t buf[UDP_HDR_LEN];
    UdpHdr h;
    h.type = MSG_HELLO;
    h.session = session;
    encode_udp_hdr(h, buf);
    if (send(fd, buf, sizeof buf, 0) < 0) {
        // ECONNREFUSED = an ICMP "port unreachable" from an earlier HELLO: the
        // server is not listening yet. The caller retries on a timer.
        if (errno == ECONNREFUSED || errno == EINTR)
            return true;
        log_errno("send HELLO");
        return false;
    }
    return true;
}

std::vector<std::vector<std::uint8_t>> udp_make_dgrams(std::uint32_t session, const Frame& f)
{
    std::vector<std::vector<std::uint8_t>> out;
    std::size_t len = f.data.size();
    if (len == 0 || len > MAX_FRAME_SIZE)
        return out;
    std::size_t cnt = udp_frag_count(len);
    out.reserve(cnt);
    for (std::size_t i = 0; i < cnt; i++) {
        UdpHdr h;
        h.type = MSG_FRAME;
        h.session = session;
        h.frame_id = f.id;
        h.timestamp_us = f.timestamp_us;
        h.frame_len = static_cast<std::uint32_t>(len);
        h.frag_idx = static_cast<std::uint16_t>(i);
        h.frag_cnt = static_cast<std::uint16_t>(cnt);
        h.payload_len = static_cast<std::uint16_t>(udp_frag_len(len, i));

        std::vector<std::uint8_t> d(UDP_HDR_LEN + h.payload_len);
        encode_udp_hdr(h, d.data());
        std::memcpy(d.data() + UDP_HDR_LEN, f.data.data() + i * MAX_UDP_PAYLOAD, h.payload_len);
        out.push_back(std::move(d));
    }
    return out;
}

bool udp_send_frame(int fd, std::uint32_t session, const Frame& f, unsigned gap_us,
                    std::size_t* n_dgrams)
{
    auto dgrams = udp_make_dgrams(session, f);
    if (dgrams.empty()) {
        std::fprintf(stderr, "netpkt: frame %u has invalid size %zu\n", f.id, f.data.size());
        return false;
    }
    std::size_t sent = 0;
    for (std::size_t i = 0; i < dgrams.size(); i++) {
        if (!send_dgram(fd, dgrams[i].data(), dgrams[i].size()))
            return false;
        sent++;
        if (gap_us > 0 && i + 1 < dgrams.size())
            std::this_thread::sleep_for(std::chrono::microseconds(gap_us));
    }
    if (n_dgrams != nullptr)
        *n_dgrams = sent;
    return true;
}

bool udp_send_end(int fd, std::uint32_t session, const EndInfo& e, int repeats)
{
    std::uint8_t buf[UDP_HDR_LEN + END_PAYLOAD_LEN];
    UdpHdr h;
    h.type = MSG_END;
    h.session = session;
    h.payload_len = END_PAYLOAD_LEN;
    encode_udp_hdr(h, buf);
    encode_end_info(e, buf + UDP_HDR_LEN);
    for (int i = 0; i < repeats; i++) {
        if (!send_dgram(fd, buf, sizeof buf))
            return false;
        if (i + 1 < repeats)
            std::this_thread::sleep_for(std::chrono::milliseconds(END_REPEAT_GAP_MS));
    }
    return true;
}

int udp_recv(int fd, std::uint8_t* buf, std::size_t cap, int timeout_ms)
{
    pollfd p;
    p.fd = fd;
    p.events = POLLIN;
    p.revents = 0;
    int rc = poll(&p, 1, timeout_ms);
    if (rc == 0)
        return 0;
    if (rc < 0) {
        if (errno == EINTR)
            return 0;
        log_errno("poll");
        return -1;
    }
    ssize_t n = recv(fd, buf, cap, 0);
    if (n < 0) {
        // ECONNREFUSED: ICMP error from an earlier datagram of ours; harmless here.
        if (errno == EINTR || errno == EAGAIN || errno == ECONNREFUSED)
            return 0;
        log_errno("recv (UDP)");
        return -1;
    }
    return static_cast<int>(n);
}

// ---- UdpAssembler -----------------------------------------------------------

namespace {

// Re-check the fields add() relies on, so the assembler is safe even if a
// caller skipped decode_udp_hdr().
bool frag_fields_ok(const UdpHdr& h)
{
    return h.type == MSG_FRAME && h.frame_id < MAX_FRAME_ID && h.frame_len > 0 &&
           h.frame_len <= MAX_FRAME_SIZE && h.frag_cnt == udp_frag_count(h.frame_len) &&
           h.frag_idx < h.frag_cnt && h.payload_len == udp_frag_len(h.frame_len, h.frag_idx);
}

} // namespace

UdpAddResult UdpAssembler::add(const UdpHdr& h, const std::uint8_t* payload, std::uint64_t now_us,
                               Frame& out)
{
    if (!frag_fields_ok(h)) {
        stats_.invalid++;
        return UdpAddResult::INVALID;
    }
    if (done_.count(h.frame_id) != 0) {
        stats_.late++;
        return UdpAddResult::LATE;
    }

    auto it = pending_.find(h.frame_id);
    if (it == pending_.end()) {
        if (pending_.size() >= MAX_PENDING) {
            // Memory bound: give up on the oldest frame to make room.
            auto oldest = pending_.begin();
            for (auto i = pending_.begin(); i != pending_.end(); ++i)
                if (i->second.first_us < oldest->second.first_us)
                    oldest = i;
            abandon(oldest);
        }
        Pending p;
        p.frame_len = h.frame_len;
        p.frag_cnt = h.frag_cnt;
        p.timestamp_us = h.timestamp_us;
        p.first_us = now_us;
        p.parts.resize(h.frag_cnt); // empty vectors only: memory grows with bytes actually received
        it = pending_.emplace(h.frame_id, std::move(p)).first;
    } else if (it->second.frame_len != h.frame_len || it->second.frag_cnt != h.frag_cnt ||
               it->second.timestamp_us != h.timestamp_us) {
        stats_.invalid++; // fragments of one frame must agree with each other
        return UdpAddResult::INVALID;
    }

    Pending& p = it->second;
    if (!p.parts[h.frag_idx].empty()) {
        stats_.dup++;
        return UdpAddResult::DUPLICATE;
    }

    // The sender emits fragments in increasing (frame, fragment) order, so
    // anything arriving before the highest position seen so far was reordered.
    std::uint64_t pos = (static_cast<std::uint64_t>(h.frame_id) << 16) | h.frag_idx;
    if (any_pos_ && pos < highest_pos_) {
        stats_.reordered++;
    } else {
        highest_pos_ = pos;
        any_pos_ = true;
    }

    p.parts[h.frag_idx].assign(payload, payload + h.payload_len);
    p.got++;
    if (p.got < p.frag_cnt)
        return UdpAddResult::STORED;

    out.id = h.frame_id;
    out.timestamp_us = p.timestamp_us;
    out.data.clear();
    out.data.reserve(p.frame_len);
    for (const auto& part : p.parts)
        out.data.insert(out.data.end(), part.begin(), part.end());
    bool size_ok = out.data.size() == p.frame_len;
    remember_done(h.frame_id);
    pending_.erase(it);
    if (!size_ok) { // cannot happen after the per-fragment length checks; never pass it on
        out.data.clear();
        stats_.invalid++;
        return UdpAddResult::INVALID;
    }
    return UdpAddResult::COMPLETE;
}

void UdpAssembler::expire(std::uint64_t now_us)
{
    for (auto it = pending_.begin(); it != pending_.end();) {
        auto cur = it++;
        if (now_us >= cur->second.first_us && now_us - cur->second.first_us >= timeout_us_)
            abandon(cur);
    }
}

void UdpAssembler::flush()
{
    while (!pending_.empty())
        abandon(pending_.begin());
}

void UdpAssembler::take_expired(std::vector<ExpiredFrame>& out)
{
    out.insert(out.end(), expired_.begin(), expired_.end());
    expired_.clear();
}

void UdpAssembler::abandon(std::map<std::uint32_t, Pending>::iterator it)
{
    ExpiredFrame e;
    e.frame_id = it->first;
    e.frame_len = it->second.frame_len;
    e.timestamp_us = it->second.timestamp_us;
    e.frags_got = it->second.got;
    e.frags_total = it->second.frag_cnt;
    expired_.push_back(e);
    remember_done(it->first); // later fragments of this frame are "late", not a new frame
    pending_.erase(it);
}

void UdpAssembler::remember_done(std::uint32_t id)
{
    if (!done_.insert(id).second)
        return;
    done_order_.push_back(id);
    if (done_order_.size() > DONE_WINDOW) {
        done_.erase(done_order_.front());
        done_order_.pop_front();
    }
}
