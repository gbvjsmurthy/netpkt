#include "common/protocol.h"

namespace {

void put16(std::uint8_t* p, std::uint16_t v)
{
    p[0] = static_cast<std::uint8_t>(v >> 8);
    p[1] = static_cast<std::uint8_t>(v);
}

void put32(std::uint8_t* p, std::uint32_t v)
{
    for (int i = 0; i < 4; i++)
        p[i] = static_cast<std::uint8_t>(v >> (24 - 8 * i));
}

void put64(std::uint8_t* p, std::uint64_t v)
{
    for (int i = 0; i < 8; i++)
        p[i] = static_cast<std::uint8_t>(v >> (56 - 8 * i));
}

std::uint16_t get16(const std::uint8_t* p)
{
    return static_cast<std::uint16_t>((p[0] << 8) | p[1]);
}

std::uint32_t get32(const std::uint8_t* p)
{
    std::uint32_t v = 0;
    for (int i = 0; i < 4; i++)
        v = (v << 8) | p[i];
    return v;
}

std::uint64_t get64(const std::uint8_t* p)
{
    std::uint64_t v = 0;
    for (int i = 0; i < 8; i++)
        v = (v << 8) | p[i];
    return v;
}

} // namespace

void encode_tcp_hdr(const TcpHdr& h, std::uint8_t* out)
{
    put16(out + 0, PKT_MAGIC);
    out[2] = h.type;
    out[3] = 0;
    put32(out + 4, h.frame_id);
    put64(out + 8, h.timestamp_us);
    put32(out + 16, h.len);
}

ProtoStatus decode_tcp_hdr(const std::uint8_t* buf, std::size_t len, TcpHdr& h)
{
    if (len < TCP_HDR_LEN)
        return ProtoStatus::SHORT;
    if (get16(buf) != PKT_MAGIC)
        return ProtoStatus::BAD_MAGIC;
    if (buf[2] != MSG_FRAME && buf[2] != MSG_END) // HELLO is UDP-only
        return ProtoStatus::BAD_TYPE;
    if (buf[3] != 0)
        return ProtoStatus::BAD_RESERVED;

    TcpHdr t;
    t.type = buf[2];
    t.frame_id = get32(buf + 4);
    t.timestamp_us = get64(buf + 8);
    t.len = get32(buf + 16);

    if (t.type == MSG_FRAME) {
        if (t.len == 0 || t.len > MAX_FRAME_SIZE)
            return ProtoStatus::BAD_LEN;
        if (t.frame_id >= MAX_FRAME_ID)
            return ProtoStatus::BAD_FRAME_ID;
    } else if (t.len != END_PAYLOAD_LEN) {
        return ProtoStatus::BAD_LEN;
    }
    h = t;
    return ProtoStatus::OK;
}

void encode_udp_hdr(const UdpHdr& h, std::uint8_t* out)
{
    put16(out + 0, PKT_MAGIC);
    out[2] = h.type;
    out[3] = 0;
    put32(out + 4, h.session);
    put32(out + 8, h.frame_id);
    put64(out + 12, h.timestamp_us);
    put32(out + 20, h.frame_len);
    put16(out + 24, h.frag_idx);
    put16(out + 26, h.frag_cnt);
    put16(out + 28, h.payload_len);
}

ProtoStatus decode_udp_hdr(const std::uint8_t* dgram, std::size_t dgram_len, UdpHdr& h)
{
    if (dgram_len < UDP_HDR_LEN)
        return ProtoStatus::SHORT;
    if (get16(dgram) != PKT_MAGIC)
        return ProtoStatus::BAD_MAGIC;
    if (dgram[2] != MSG_FRAME && dgram[2] != MSG_END && dgram[2] != MSG_HELLO)
        return ProtoStatus::BAD_TYPE;
    if (dgram[3] != 0)
        return ProtoStatus::BAD_RESERVED;

    UdpHdr u;
    u.type = dgram[2];
    u.session = get32(dgram + 4);
    u.frame_id = get32(dgram + 8);
    u.timestamp_us = get64(dgram + 12);
    u.frame_len = get32(dgram + 20);
    u.frag_idx = get16(dgram + 24);
    u.frag_cnt = get16(dgram + 26);
    u.payload_len = get16(dgram + 28);

    // The declared payload must be exactly what arrived (catches truncation too).
    if (u.payload_len != dgram_len - UDP_HDR_LEN)
        return ProtoStatus::BAD_LEN;

    if (u.type == MSG_HELLO) {
        if (u.payload_len != 0)
            return ProtoStatus::BAD_LEN;
    } else if (u.type == MSG_END) {
        if (u.payload_len != END_PAYLOAD_LEN)
            return ProtoStatus::BAD_LEN;
    } else {
        if (u.frame_id >= MAX_FRAME_ID)
            return ProtoStatus::BAD_FRAME_ID;
        if (u.frame_len == 0 || u.frame_len > MAX_FRAME_SIZE)
            return ProtoStatus::BAD_LEN;
        if (u.frag_cnt != udp_frag_count(u.frame_len) || u.frag_idx >= u.frag_cnt)
            return ProtoStatus::BAD_FRAG;
        if (u.payload_len != udp_frag_len(u.frame_len, u.frag_idx))
            return ProtoStatus::BAD_LEN;
    }
    h = u;
    return ProtoStatus::OK;
}

void encode_end_info(const EndInfo& e, std::uint8_t* out)
{
    put32(out, e.frames);
    put64(out + 4, e.bytes);
}

bool decode_end_info(const std::uint8_t* buf, std::size_t len, EndInfo& e)
{
    if (len != END_PAYLOAD_LEN)
        return false;
    EndInfo t;
    t.frames = get32(buf);
    t.bytes = get64(buf + 4);
    if (t.frames > MAX_FRAME_ID)
        return false;
    e = t;
    return true;
}

std::size_t udp_frag_count(std::size_t frame_len)
{
    return (frame_len + MAX_UDP_PAYLOAD - 1) / MAX_UDP_PAYLOAD;
}

std::size_t udp_frag_len(std::size_t frame_len, std::size_t idx)
{
    std::size_t cnt = udp_frag_count(frame_len);
    if (idx >= cnt)
        return 0;
    if (idx + 1 < cnt)
        return MAX_UDP_PAYLOAD;
    return frame_len - idx * MAX_UDP_PAYLOAD;
}

const char* proto_status_str(ProtoStatus s)
{
    switch (s) {
    case ProtoStatus::OK:           return "ok";
    case ProtoStatus::SHORT:        return "header too short";
    case ProtoStatus::BAD_MAGIC:    return "bad magic";
    case ProtoStatus::BAD_TYPE:     return "bad message type";
    case ProtoStatus::BAD_RESERVED: return "reserved byte not zero";
    case ProtoStatus::BAD_LEN:      return "bad length";
    case ProtoStatus::BAD_FRAME_ID: return "frame id out of range";
    case ProtoStatus::BAD_FRAG:     return "bad fragment index/count";
    }
    return "unknown";
}
