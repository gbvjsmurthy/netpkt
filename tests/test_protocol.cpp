// Header serialization, validation limits, and the fragmentation arithmetic.
#include <cstring>
#include <vector>

#include "check.h"
#include "common/protocol.h"

static std::vector<std::uint8_t> udp_dgram(const UdpHdr& h, std::size_t extra_payload)
{
    std::vector<std::uint8_t> d(UDP_HDR_LEN + extra_payload, 0xAB);
    encode_udp_hdr(h, d.data());
    return d;
}

static UdpHdr frag(std::uint32_t frame_len, std::uint16_t idx)
{
    UdpHdr h;
    h.type = MSG_FRAME;
    h.session = 7;
    h.frame_id = 3;
    h.timestamp_us = 99;
    h.frame_len = frame_len;
    h.frag_idx = idx;
    h.frag_cnt = static_cast<std::uint16_t>(udp_frag_count(frame_len));
    h.payload_len = static_cast<std::uint16_t>(udp_frag_len(frame_len, idx));
    return h;
}

static void test_tcp_header()
{
    TcpHdr h;
    h.type = MSG_FRAME;
    h.frame_id = 0x000A0B0C; // distinctive bytes, but below MAX_FRAME_ID
    h.timestamp_us = 0x1112131415161718ull;
    h.len = 5000;
    std::uint8_t b[TCP_HDR_LEN];
    encode_tcp_hdr(h, b);

    // The documented layout, byte by byte (big-endian).
    CHECK(b[0] == 0x4E && b[1] == 0x50);
    CHECK(b[2] == MSG_FRAME && b[3] == 0);
    CHECK(b[4] == 0 && b[5] == 0x0A && b[6] == 0x0B && b[7] == 0x0C);
    CHECK(b[8] == 0x11 && b[15] == 0x18);
    CHECK(b[16] == 0 && b[17] == 0 && b[18] == 0x13 && b[19] == 0x88);

    TcpHdr r;
    CHECK(decode_tcp_hdr(b, sizeof b, r) == ProtoStatus::OK);
    CHECK(r.type == MSG_FRAME && r.frame_id == h.frame_id);
    CHECK(r.timestamp_us == h.timestamp_us && r.len == 5000);
}

static void test_tcp_invalid()
{
    TcpHdr good;
    good.type = MSG_FRAME;
    good.len = 100;
    std::uint8_t b[TCP_HDR_LEN];
    TcpHdr r;

    encode_tcp_hdr(good, b);
    CHECK(decode_tcp_hdr(b, TCP_HDR_LEN - 1, r) == ProtoStatus::SHORT);
    CHECK(decode_tcp_hdr(b, 0, r) == ProtoStatus::SHORT);

    std::uint8_t c[TCP_HDR_LEN];
    std::memcpy(c, b, sizeof b);
    c[0] = 0;
    CHECK(decode_tcp_hdr(c, sizeof c, r) == ProtoStatus::BAD_MAGIC);

    std::memcpy(c, b, sizeof b);
    c[2] = 0;
    CHECK(decode_tcp_hdr(c, sizeof c, r) == ProtoStatus::BAD_TYPE);
    c[2] = MSG_HELLO; // HELLO is UDP only
    CHECK(decode_tcp_hdr(c, sizeof c, r) == ProtoStatus::BAD_TYPE);
    c[2] = 200;
    CHECK(decode_tcp_hdr(c, sizeof c, r) == ProtoStatus::BAD_TYPE);

    std::memcpy(c, b, sizeof b);
    c[3] = 1;
    CHECK(decode_tcp_hdr(c, sizeof c, r) == ProtoStatus::BAD_RESERVED);

    TcpHdr h = good;
    h.len = 0;
    encode_tcp_hdr(h, b);
    CHECK(decode_tcp_hdr(b, sizeof b, r) == ProtoStatus::BAD_LEN);
    h.len = static_cast<std::uint32_t>(MAX_FRAME_SIZE);
    encode_tcp_hdr(h, b);
    CHECK(decode_tcp_hdr(b, sizeof b, r) == ProtoStatus::OK); // exactly at the limit
    h.len = static_cast<std::uint32_t>(MAX_FRAME_SIZE) + 1;
    encode_tcp_hdr(h, b);
    CHECK(decode_tcp_hdr(b, sizeof b, r) == ProtoStatus::BAD_LEN);
    h.len = 0xFFFFFFFFu; // a hostile "4 GiB frame"
    encode_tcp_hdr(h, b);
    CHECK(decode_tcp_hdr(b, sizeof b, r) == ProtoStatus::BAD_LEN);

    h = good;
    h.frame_id = MAX_FRAME_ID;
    encode_tcp_hdr(h, b);
    CHECK(decode_tcp_hdr(b, sizeof b, r) == ProtoStatus::BAD_FRAME_ID);

    // END must carry exactly the 12-byte payload.
    h = TcpHdr();
    h.type = MSG_END;
    h.len = END_PAYLOAD_LEN;
    encode_tcp_hdr(h, b);
    CHECK(decode_tcp_hdr(b, sizeof b, r) == ProtoStatus::OK && r.type == MSG_END);
    h.len = 11;
    encode_tcp_hdr(h, b);
    CHECK(decode_tcp_hdr(b, sizeof b, r) == ProtoStatus::BAD_LEN);
}

static void test_udp_header()
{
    UdpHdr h = frag(3000, 1); // 3 fragments of 1170,1170,660
    CHECK(h.frag_cnt == 3 && h.payload_len == MAX_UDP_PAYLOAD);
    auto d = udp_dgram(h, h.payload_len);
    CHECK(d.size() <= MAX_PKT_SIZE);

    CHECK(d[0] == 0x4E && d[1] == 0x50 && d[2] == MSG_FRAME);
    CHECK(d[4] == 0 && d[7] == 7);                // session
    CHECK(d[8] == 0 && d[11] == 3);               // frame id
    CHECK(d[12] == 0 && d[19] == 99);             // timestamp
    CHECK(d[22] == 0x0B && d[23] == 0xB8);        // frame_len 3000 = 0x0BB8
    CHECK(d[24] == 0 && d[25] == 1);              // frag_idx
    CHECK(d[26] == 0 && d[27] == 3);              // frag_cnt

    UdpHdr r;
    CHECK(decode_udp_hdr(d.data(), d.size(), r) == ProtoStatus::OK);
    CHECK(r.session == 7 && r.frame_id == 3 && r.timestamp_us == 99);
    CHECK(r.frame_len == 3000 && r.frag_idx == 1 && r.frag_cnt == 3);
    CHECK(r.payload_len == MAX_UDP_PAYLOAD);

    // last fragment is shorter
    UdpHdr last = frag(3000, 2);
    CHECK(last.payload_len == 3000 - 2 * MAX_UDP_PAYLOAD);
    auto dl = udp_dgram(last, last.payload_len);
    CHECK(decode_udp_hdr(dl.data(), dl.size(), r) == ProtoStatus::OK);
}

static void test_udp_invalid()
{
    UdpHdr r;
    UdpHdr h = frag(3000, 1);

    // datagram size must equal header + declared payload (truncation / padding)
    auto d = udp_dgram(h, h.payload_len);
    CHECK(decode_udp_hdr(d.data(), d.size() - 1, r) == ProtoStatus::BAD_LEN);
    d.push_back(0);
    CHECK(decode_udp_hdr(d.data(), d.size(), r) == ProtoStatus::BAD_LEN);
    CHECK(decode_udp_hdr(d.data(), UDP_HDR_LEN - 1, r) == ProtoStatus::SHORT);

    UdpHdr bad = h;
    bad.frag_idx = bad.frag_cnt; // index == count: out of range
    auto d1 = udp_dgram(bad, bad.payload_len);
    CHECK(decode_udp_hdr(d1.data(), d1.size(), r) == ProtoStatus::BAD_FRAG);

    bad = h;
    bad.frag_cnt = 4; // disagrees with frame_len
    d1 = udp_dgram(bad, bad.payload_len);
    CHECK(decode_udp_hdr(d1.data(), d1.size(), r) == ProtoStatus::BAD_FRAG);

    bad = h;
    bad.frag_cnt = 0;
    d1 = udp_dgram(bad, bad.payload_len);
    CHECK(decode_udp_hdr(d1.data(), d1.size(), r) == ProtoStatus::BAD_FRAG);

    bad = h;
    bad.frame_len = 0;
    d1 = udp_dgram(bad, bad.payload_len);
    CHECK(decode_udp_hdr(d1.data(), d1.size(), r) == ProtoStatus::BAD_LEN);

    bad = h;
    bad.frame_len = static_cast<std::uint32_t>(MAX_FRAME_SIZE) + 1;
    d1 = udp_dgram(bad, bad.payload_len);
    CHECK(decode_udp_hdr(d1.data(), d1.size(), r) == ProtoStatus::BAD_LEN);

    bad = h;
    bad.payload_len = 100; // wrong length for a middle fragment
    d1 = udp_dgram(bad, 100);
    CHECK(decode_udp_hdr(d1.data(), d1.size(), r) == ProtoStatus::BAD_LEN);

    bad = h;
    bad.frame_id = MAX_FRAME_ID;
    d1 = udp_dgram(bad, bad.payload_len);
    CHECK(decode_udp_hdr(d1.data(), d1.size(), r) == ProtoStatus::BAD_FRAME_ID);

    d1 = udp_dgram(h, h.payload_len);
    d1[0] = 0xFF;
    CHECK(decode_udp_hdr(d1.data(), d1.size(), r) == ProtoStatus::BAD_MAGIC);

    // HELLO: valid with no payload, invalid with one
    UdpHdr hello;
    hello.type = MSG_HELLO;
    hello.session = 42;
    auto dh = udp_dgram(hello, 0);
    CHECK(decode_udp_hdr(dh.data(), dh.size(), r) == ProtoStatus::OK && r.session == 42);
    hello.payload_len = 3;
    dh = udp_dgram(hello, 3);
    CHECK(decode_udp_hdr(dh.data(), dh.size(), r) == ProtoStatus::BAD_LEN);
}

static void test_end_info()
{
    EndInfo e;
    e.frames = 1234;
    e.bytes = 0x0102030405060708ull;
    std::uint8_t b[END_PAYLOAD_LEN];
    encode_end_info(e, b);
    CHECK(b[0] == 0 && b[3] == 0xD2 && b[4] == 1 && b[11] == 8);
    EndInfo r;
    CHECK(decode_end_info(b, sizeof b, r) && r.frames == 1234 && r.bytes == e.bytes);
    CHECK(!decode_end_info(b, sizeof b - 1, r));
    e.frames = MAX_FRAME_ID + 1;
    encode_end_info(e, b);
    CHECK(!decode_end_info(b, sizeof b, r)); // would make the client allocate a huge table

    // END datagram
    UdpHdr h;
    h.type = MSG_END;
    h.session = 5;
    h.payload_len = END_PAYLOAD_LEN;
    std::vector<std::uint8_t> d(UDP_HDR_LEN + END_PAYLOAD_LEN);
    encode_udp_hdr(h, d.data());
    e.frames = 10;
    e.bytes = 777;
    encode_end_info(e, d.data() + UDP_HDR_LEN);
    UdpHdr rh;
    CHECK(decode_udp_hdr(d.data(), d.size(), rh) == ProtoStatus::OK && rh.type == MSG_END);
    CHECK(decode_end_info(d.data() + UDP_HDR_LEN, rh.payload_len, r) && r.frames == 10 && r.bytes == 777);
}

static void test_fragment_math()
{
    CHECK(udp_frag_count(0) == 0);
    CHECK(udp_frag_count(1) == 1);
    CHECK(udp_frag_count(MAX_UDP_PAYLOAD) == 1);
    CHECK(udp_frag_count(MAX_UDP_PAYLOAD + 1) == 2);
    CHECK(udp_frag_len(MAX_UDP_PAYLOAD + 1, 1) == 1);
    CHECK(udp_frag_len(100, 1) == 0); // out of range
    // Invariant: fragment lengths always add up to the frame size, none empty or too big.
    for (std::size_t len : {1u, 1169u, 1170u, 1171u, 2340u, 2341u, 100000u,
                            static_cast<unsigned>(MAX_FRAME_SIZE)}) {
        std::size_t sum = 0, cnt = udp_frag_count(len);
        for (std::size_t i = 0; i < cnt; i++) {
            std::size_t l = udp_frag_len(len, i);
            CHECK(l > 0 && l <= MAX_UDP_PAYLOAD);
            sum += l;
        }
        CHECK(sum == len);
    }
    CHECK(udp_frag_count(MAX_FRAME_SIZE) <= 65535); // frag_cnt is a u16
    CHECK(UDP_HDR_LEN + MAX_UDP_PAYLOAD == MAX_PKT_SIZE);
}

int main()
{
    test_tcp_header();
    test_tcp_invalid();
    test_udp_header();
    test_udp_invalid();
    test_end_info();
    test_fragment_math();
    return TEST_RESULT();
}
