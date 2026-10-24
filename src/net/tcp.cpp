#include "net/tcp.h"

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/types.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <vector>

#include "common/util.h"

namespace {

// Frames are written in one send() burst; without NODELAY the kernel may hold
// the last small segment of a frame back (Nagle) and distort latency numbers.
void set_nodelay(int fd)
{
    int one = 1;
    if (setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one) < 0)
        log_errno("setsockopt TCP_NODELAY (continuing)");
}

} // namespace

Fd tcp_listen(std::uint16_t port)
{
    Fd fd(::socket(AF_INET, SOCK_STREAM, 0));
    if (!fd.valid()) {
        log_errno("socket");
        return Fd();
    }
    // Lets the server restart right away instead of waiting out TIME_WAIT.
    int one = 1;
    if (setsockopt(fd.get(), SOL_SOCKET, SO_REUSEADDR, &one, sizeof one) < 0) {
        log_errno("setsockopt SO_REUSEADDR");
        return Fd();
    }
    sockaddr_in addr;
    std::memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);
    if (bind(fd.get(), reinterpret_cast<sockaddr*>(&addr), sizeof addr) < 0) {
        std::fprintf(stderr, "netpkt: cannot bind TCP port %u: %s\n", port, std::strerror(errno));
        return Fd();
    }
    if (listen(fd.get(), 1) < 0) { // one client is enough for this project
        log_errno("listen");
        return Fd();
    }
    return fd;
}

Fd tcp_accept(const Fd& listener, std::string& peer)
{
    for (;;) {
        sockaddr_in addr;
        socklen_t alen = sizeof addr;
        int c = accept(listener.get(), reinterpret_cast<sockaddr*>(&addr), &alen);
        if (c >= 0) {
            Fd fd(c);
            char ip[INET_ADDRSTRLEN] = "?";
            inet_ntop(AF_INET, &addr.sin_addr, ip, sizeof ip);
            peer = std::string(ip) + ":" + std::to_string(ntohs(addr.sin_port));
            set_nodelay(fd.get());
            return fd;
        }
        if (errno == EINTR && !stop_requested())
            continue;
        if (errno != EINTR)
            log_errno("accept");
        return Fd();
    }
}

Fd tcp_connect(const std::string& host, std::uint16_t port)
{
    addrinfo hints;
    std::memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET; // the server listens on IPv4 only
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* res = nullptr;
    int rc = getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &res);
    if (rc != 0) {
        std::fprintf(stderr, "netpkt: cannot resolve '%s': %s\n", host.c_str(), gai_strerror(rc));
        return Fd();
    }
    Fd fd;
    int last_err = 0;
    for (addrinfo* p = res; p != nullptr; p = p->ai_next) {
        Fd s(::socket(p->ai_family, p->ai_socktype, p->ai_protocol));
        if (!s.valid()) {
            last_err = errno;
            continue;
        }
        // EINTR here only comes from our SIGINT/SIGTERM handler: treat as "give up".
        if (connect(s.get(), p->ai_addr, p->ai_addrlen) == 0) {
            fd = std::move(s);
            break;
        }
        last_err = errno;
    }
    freeaddrinfo(res);
    if (!fd.valid()) {
        std::fprintf(stderr, "netpkt: cannot connect to %s:%u: %s\n", host.c_str(), port,
                     std::strerror(last_err));
        return Fd();
    }
    set_nodelay(fd.get());
    return fd;
}

bool send_all(int fd, const std::uint8_t* buf, std::size_t len)
{
    std::size_t done = 0;
    while (done < len) {
        // MSG_NOSIGNAL: report a dead peer as EPIPE instead of raising SIGPIPE.
        ssize_t n = send(fd, buf + done, len - done, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR && !stop_requested())
                continue;
            if (errno != EINTR)
                log_errno("send");
            return false;
        }
        done += static_cast<std::size_t>(n); // n may be less than requested
    }
    return true;
}

int recv_all(int fd, std::uint8_t* buf, std::size_t len)
{
    std::size_t done = 0;
    while (done < len) {
        ssize_t n = recv(fd, buf + done, len - done, 0);
        if (n == 0) {
            if (done == 0)
                return 0;
            std::fprintf(stderr, "netpkt: peer closed connection mid-message (%zu of %zu bytes)\n",
                         done, len);
            return -1;
        }
        if (n < 0) {
            if (errno == EINTR && !stop_requested())
                continue;
            if (errno != EINTR)
                log_errno("recv");
            return -1;
        }
        done += static_cast<std::size_t>(n);
    }
    return 1;
}

bool tcp_send_frame(int fd, const Frame& f)
{
    if (f.data.empty() || f.data.size() > MAX_FRAME_SIZE) {
        std::fprintf(stderr, "netpkt: frame %u has invalid size %zu\n", f.id, f.data.size());
        return false;
    }
    TcpHdr h;
    h.type = MSG_FRAME;
    h.frame_id = f.id;
    h.timestamp_us = f.timestamp_us;
    h.len = static_cast<std::uint32_t>(f.data.size());

    // One buffer, one send_all(): avoids a tiny separate segment for the header.
    std::vector<std::uint8_t> msg(TCP_HDR_LEN + f.data.size());
    encode_tcp_hdr(h, msg.data());
    std::memcpy(msg.data() + TCP_HDR_LEN, f.data.data(), f.data.size());
    return send_all(fd, msg.data(), msg.size());
}

bool tcp_send_end(int fd, const EndInfo& e)
{
    std::uint8_t msg[TCP_HDR_LEN + END_PAYLOAD_LEN];
    TcpHdr h;
    h.type = MSG_END;
    h.len = END_PAYLOAD_LEN;
    encode_tcp_hdr(h, msg);
    encode_end_info(e, msg + TCP_HDR_LEN);
    return send_all(fd, msg, sizeof msg);
}

TcpMsg tcp_recv_msg(int fd, Frame& f, EndInfo& end)
{
    std::uint8_t hdr_buf[TCP_HDR_LEN];
    int rc = recv_all(fd, hdr_buf, sizeof hdr_buf);
    if (rc == 0)
        return TcpMsg::CLOSED;
    if (rc < 0)
        return TcpMsg::ERROR;

    TcpHdr h;
    ProtoStatus st = decode_tcp_hdr(hdr_buf, sizeof hdr_buf, h);
    if (st != ProtoStatus::OK) {
        std::fprintf(stderr, "netpkt: invalid TCP header: %s\n", proto_status_str(st));
        return TcpMsg::ERROR;
    }

    if (h.type == MSG_END) {
        std::uint8_t body[END_PAYLOAD_LEN];
        if (recv_all(fd, body, sizeof body) != 1 || !decode_end_info(body, sizeof body, end)) {
            std::fprintf(stderr, "netpkt: invalid END message\n");
            return TcpMsg::ERROR;
        }
        return TcpMsg::END;
    }

    // h.len was validated against MAX_FRAME_SIZE, so this allocation is bounded.
    f.id = h.frame_id;
    f.timestamp_us = h.timestamp_us;
    f.data.resize(h.len);
    if (recv_all(fd, f.data.data(), f.data.size()) != 1) {
        std::fprintf(stderr, "netpkt: frame %u payload truncated\n", h.frame_id);
        return TcpMsg::ERROR;
    }
    return TcpMsg::FRAME;
}
