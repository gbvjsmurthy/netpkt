// Wire protocol shared by TCP and UDP. All integers are big-endian (network
// byte order) and serialized field by field; structs are never sent raw.
//
// TCP message (20-byte header, then `len` payload bytes):
//    0  u16 magic        0x4E50 ("NP")
//    2  u8  type         1 = FRAME, 2 = END
//    3  u8  reserved     must be 0
//    4  u32 frame_id
//    8  u64 timestamp_us sender steady clock, taken just before sending (0 for END)
//   16  u32 len          FRAME: JPEG size (1..MAX_FRAME_SIZE). END: 12
//
// UDP datagram (30-byte header, then payload_len payload bytes):
//    0  u16 magic
//    2  u8  type         1 = FRAME, 2 = END, 3 = HELLO
//    3  u8  reserved     must be 0
//    4  u32 session      chosen by the client in HELLO; echoed in every datagram
//    8  u32 frame_id
//   12  u64 timestamp_us
//   20  u32 frame_len    total JPEG size of the frame this fragment belongs to
//   24  u16 frag_idx     0-based
//   26  u16 frag_cnt     total fragments in this frame
//   28  u16 payload_len  bytes after the header in this datagram
//
// END payload (12 bytes, both transports): u32 frames_sent, u64 payload_bytes_sent.
// HELLO has no payload; the client sends it so the server learns its address.
#pragma once

#include <cstddef>
#include <cstdint>

constexpr std::uint16_t PKT_MAGIC = 0x4E50;

constexpr std::size_t TCP_HDR_LEN = 20;
constexpr std::size_t UDP_HDR_LEN = 30;
constexpr std::size_t END_PAYLOAD_LEN = 12;

// Whole UDP datagram budget (header + payload), safely under a 1500-byte MTU.
constexpr std::size_t MAX_PKT_SIZE = 1200;
constexpr std::size_t MAX_UDP_PAYLOAD = MAX_PKT_SIZE - UDP_HDR_LEN;

// Limits checked BEFORE any allocation so a bad header cannot exhaust memory.
constexpr std::size_t MAX_FRAME_SIZE = 8u * 1024 * 1024;
constexpr std::uint32_t MAX_FRAME_ID = 1u << 20; // ~9.7 hours at 30 fps

enum MsgType : std::uint8_t {
    MSG_FRAME = 1,
    MSG_END = 2,
    MSG_HELLO = 3,
};

enum class ProtoStatus {
    OK,
    SHORT,          // fewer bytes than the header needs
    BAD_MAGIC,
    BAD_TYPE,
    BAD_RESERVED,
    BAD_LEN,        // length field disagrees with limits or with the datagram size
    BAD_FRAME_ID,
    BAD_FRAG,       // fragment index/count inconsistent with the frame size
};

struct TcpHdr {
    std::uint8_t type = 0;
    std::uint32_t frame_id = 0;
    std::uint64_t timestamp_us = 0;
    std::uint32_t len = 0;
};

struct UdpHdr {
    std::uint8_t type = 0;
    std::uint32_t session = 0;
    std::uint32_t frame_id = 0;
    std::uint64_t timestamp_us = 0;
    std::uint32_t frame_len = 0;
    std::uint16_t frag_idx = 0;
    std::uint16_t frag_cnt = 0;
    std::uint16_t payload_len = 0;
};

struct EndInfo {
    std::uint32_t frames = 0;  // frames the server offered
    std::uint64_t bytes = 0;   // JPEG payload bytes the server offered
};

// out must have room for TCP_HDR_LEN / UDP_HDR_LEN / END_PAYLOAD_LEN bytes.
void encode_tcp_hdr(const TcpHdr& h, std::uint8_t* out);
void encode_udp_hdr(const UdpHdr& h, std::uint8_t* out);
void encode_end_info(const EndInfo& e, std::uint8_t* out);

// decode_* validate everything they can without state.
ProtoStatus decode_tcp_hdr(const std::uint8_t* buf, std::size_t len, TcpHdr& h);
// dgram_len is the size of the whole received datagram.
ProtoStatus decode_udp_hdr(const std::uint8_t* dgram, std::size_t dgram_len, UdpHdr& h);
bool decode_end_info(const std::uint8_t* buf, std::size_t len, EndInfo& e);

// Fragmentation rule: every fragment carries MAX_UDP_PAYLOAD bytes except the
// last. Receivers use it to check each fragment's declared length.
std::size_t udp_frag_count(std::size_t frame_len);
std::size_t udp_frag_len(std::size_t frame_len, std::size_t idx);

const char* proto_status_str(ProtoStatus s);
