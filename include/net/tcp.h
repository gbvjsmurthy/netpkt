// TCP transport: listen/accept/connect plus length-prefixed message framing.
// TCP is a byte stream, so one send() never corresponds to one recv();
// send_all/recv_all loop until the exact byte count has moved.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "common/frame.h"
#include "common/protocol.h"
#include "net/fd.h"

// All return an invalid Fd on failure after printing a diagnostic to stderr.
Fd tcp_listen(std::uint16_t port);
Fd tcp_accept(const Fd& listener, std::string& peer);
Fd tcp_connect(const std::string& host, std::uint16_t port);

// Send exactly len bytes. false on error (message already printed).
bool send_all(int fd, const std::uint8_t* buf, std::size_t len);
// Receive exactly len bytes.
//   1  = got all bytes
//   0  = peer closed cleanly before the first byte (a message boundary)
//  -1  = error, or peer closed in the middle of the data
int recv_all(int fd, std::uint8_t* buf, std::size_t len);

bool tcp_send_frame(int fd, const Frame& f);
bool tcp_send_end(int fd, const EndInfo& e);

enum class TcpMsg {
    FRAME,   // f is filled in
    END,     // end is filled in
    CLOSED,  // peer closed cleanly between messages, without an END message
    ERROR,   // I/O error, protocol violation, or truncated message
};
TcpMsg tcp_recv_msg(int fd, Frame& f, EndInfo& end);
