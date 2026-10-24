// TCP helper logic over socketpair(): same stream semantics as TCP, no ports needed.
#include <sys/socket.h>
#include <unistd.h>

#include <cstring>
#include <thread>
#include <vector>

#include "check.h"
#include "net/fd.h"
#include "net/tcp.h"

static void make_pair(Fd& a, Fd& b)
{
    int sv[2];
    int rc = socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
    CHECK(rc == 0);
    a.reset(sv[0]);
    b.reset(sv[1]);
}

static std::vector<std::uint8_t> pattern(std::size_t n)
{
    std::vector<std::uint8_t> v(n);
    for (std::size_t i = 0; i < n; i++)
        v[i] = static_cast<std::uint8_t>((i * 31 + 7) & 0xFF);
    return v;
}

// The writer uses tiny writes so the reader sees many partial recv() results.
static void test_partial_reads()
{
    Fd a, b;
    make_pair(a, b);
    auto data = pattern(50000);
    std::thread w([&] {
        for (std::size_t off = 0; off < data.size(); off += 7) {
            std::size_t n = std::min<std::size_t>(7, data.size() - off);
            CHECK(send_all(a.get(), data.data() + off, n));
        }
    });
    std::vector<std::uint8_t> got(data.size());
    CHECK(recv_all(b.get(), got.data(), got.size()) == 1);
    w.join();
    CHECK(got == data);
}

// 4 MiB is far larger than the socket buffer: send_all must loop over partial sends.
static void test_large_send()
{
    Fd a, b;
    make_pair(a, b);
    auto data = pattern(4u << 20);
    std::thread w([&] { CHECK(send_all(a.get(), data.data(), data.size())); });
    std::vector<std::uint8_t> got(data.size());
    CHECK(recv_all(b.get(), got.data(), got.size()) == 1);
    w.join();
    CHECK(got == data);
}

static void test_eof_handling()
{
    std::uint8_t buf[100];
    {   // clean close before any byte: a message boundary
        Fd a, b;
        make_pair(a, b);
        a.reset();
        CHECK(recv_all(b.get(), buf, sizeof buf) == 0);
    }
    {   // close in the middle: error, not a short success
        Fd a, b;
        make_pair(a, b);
        CHECK(send_all(a.get(), buf, 40));
        a.reset();
        CHECK(recv_all(b.get(), buf, sizeof buf) == -1);
    }
    {   // zero-length request is trivially satisfied
        Fd a, b;
        make_pair(a, b);
        CHECK(recv_all(b.get(), buf, 0) == 1);
        CHECK(send_all(a.get(), buf, 0));
    }
}

static void test_message_roundtrip_and_end()
{
    Fd a, b;
    make_pair(a, b);
    Frame f1, f2;
    f1.id = 0;
    f1.timestamp_us = 123456789012ull;
    f1.data = pattern(3000);
    f2.id = 1;
    f2.timestamp_us = 123456799012ull;
    f2.data = pattern(1);
    EndInfo e;
    e.frames = 2;
    e.bytes = 3001;

    CHECK(tcp_send_frame(a.get(), f1));
    CHECK(tcp_send_frame(a.get(), f2));
    CHECK(tcp_send_end(a.get(), e));
    a.reset(); // after END the sender closes

    Frame r;
    EndInfo re;
    CHECK(tcp_recv_msg(b.get(), r, re) == TcpMsg::FRAME);
    CHECK(r.id == 0 && r.timestamp_us == f1.timestamp_us && r.data == f1.data);
    CHECK(tcp_recv_msg(b.get(), r, re) == TcpMsg::FRAME);
    CHECK(r.id == 1 && r.timestamp_us == f2.timestamp_us && r.data == f2.data); // message boundary kept
    CHECK(tcp_recv_msg(b.get(), r, re) == TcpMsg::END);
    CHECK(re.frames == 2 && re.bytes == 3001);
    CHECK(tcp_recv_msg(b.get(), r, re) == TcpMsg::CLOSED);
}

static void test_bad_input()
{
    Frame r;
    EndInfo re;
    {   // oversized length is rejected from the header alone
        Fd a, b;
        make_pair(a, b);
        TcpHdr h;
        h.type = MSG_FRAME;
        h.len = static_cast<std::uint32_t>(MAX_FRAME_SIZE) + 1;
        std::uint8_t hb[TCP_HDR_LEN];
        encode_tcp_hdr(h, hb);
        CHECK(send_all(a.get(), hb, sizeof hb));
        CHECK(tcp_recv_msg(b.get(), r, re) == TcpMsg::ERROR);
        CHECK(r.data.empty()); // nothing was allocated for it
    }
    {   // header promises 1000 bytes, peer sends 10 and closes
        Fd a, b;
        make_pair(a, b);
        TcpHdr h;
        h.type = MSG_FRAME;
        h.len = 1000;
        std::uint8_t hb[TCP_HDR_LEN];
        encode_tcp_hdr(h, hb);
        std::uint8_t part[10] = {0};
        CHECK(send_all(a.get(), hb, sizeof hb));
        CHECK(send_all(a.get(), part, sizeof part));
        a.reset();
        CHECK(tcp_recv_msg(b.get(), r, re) == TcpMsg::ERROR);
    }
    {   // garbage where a header should be
        Fd a, b;
        make_pair(a, b);
        std::uint8_t junk[TCP_HDR_LEN];
        std::memset(junk, 0x55, sizeof junk);
        CHECK(send_all(a.get(), junk, sizeof junk));
        CHECK(tcp_recv_msg(b.get(), r, re) == TcpMsg::ERROR);
    }
    {   // refusing to send an empty or oversized frame
        Fd a, b;
        make_pair(a, b);
        Frame empty;
        CHECK(!tcp_send_frame(a.get(), empty));
    }
}

int main()
{
    test_partial_reads();
    test_large_send();
    test_eof_handling();
    test_message_roundtrip_and_end();
    test_bad_input();
    return TEST_RESULT();
}
