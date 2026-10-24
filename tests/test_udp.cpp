// UDP fragmentation + reassembly, without needing a real network.
#include <sys/socket.h>

#include <algorithm>
#include <random>
#include <vector>

#include "check.h"
#include "common/protocol.h"
#include "net/udp.h"

constexpr std::uint64_t TIMEOUT_US = 500000;

static Frame make_frame(std::uint32_t id, std::size_t len, std::uint64_t ts = 1000)
{
    Frame f;
    f.id = id;
    f.timestamp_us = ts;
    f.data.resize(len);
    for (std::size_t i = 0; i < len; i++)
        f.data[i] = static_cast<std::uint8_t>((i * 13 + id * 5) & 0xFF);
    return f;
}

struct Dgram {
    UdpHdr hdr;
    std::vector<std::uint8_t> bytes;
};

// Build datagrams with the real sender code and parse them with the real decoder.
static std::vector<Dgram> fragment(const Frame& f, std::uint32_t session = 1)
{
    std::vector<Dgram> out;
    for (auto& d : udp_make_dgrams(session, f)) {
        Dgram g;
        CHECK(decode_udp_hdr(d.data(), d.size(), g.hdr) == ProtoStatus::OK);
        g.bytes = std::move(d);
        out.push_back(std::move(g));
    }
    return out;
}

static UdpAddResult feed(UdpAssembler& a, const Dgram& g, std::uint64_t now, Frame& out)
{
    return a.add(g.hdr, g.bytes.data() + UDP_HDR_LEN, now, out);
}

static void test_in_order()
{
    Frame f = make_frame(5, 10000, 4242);
    auto dg = fragment(f);
    CHECK(dg.size() == udp_frag_count(10000));
    for (const auto& g : dg)
        CHECK(g.bytes.size() <= MAX_PKT_SIZE);

    UdpAssembler a(TIMEOUT_US);
    Frame out;
    for (std::size_t i = 0; i < dg.size(); i++) {
        UdpAddResult r = feed(a, dg[i], 100 + i, out);
        CHECK(r == (i + 1 == dg.size() ? UdpAddResult::COMPLETE : UdpAddResult::STORED));
    }
    CHECK(out.id == 5 && out.timestamp_us == 4242 && out.data == f.data);
    CHECK(a.pending_count() == 0);
    CHECK(a.stats().reordered == 0 && a.stats().dup == 0);
}

static void test_out_of_order()
{
    Frame f = make_frame(9, 20000);
    auto dg = fragment(f);
    std::mt19937 rng(12345); // fixed seed: deterministic test
    std::shuffle(dg.begin(), dg.end(), rng);

    UdpAssembler a(TIMEOUT_US);
    Frame out;
    int completes = 0;
    for (std::size_t i = 0; i < dg.size(); i++) {
        UdpAddResult r = feed(a, dg[i], 10, out);
        if (r == UdpAddResult::COMPLETE) {
            completes++;
            CHECK(i + 1 == dg.size()); // never before the last fragment
        }
    }
    CHECK(completes == 1);
    CHECK(out.data == f.data); // content is right regardless of arrival order
    CHECK(a.stats().reordered > 0);

    // fully reversed
    UdpAssembler b(TIMEOUT_US);
    std::sort(dg.begin(), dg.end(), [](const Dgram& x, const Dgram& y) { return x.hdr.frag_idx > y.hdr.frag_idx; });
    for (const auto& g : dg)
        feed(b, g, 10, out);
    CHECK(out.data == f.data);
    CHECK(b.stats().reordered == dg.size() - 1);
}

static void test_duplicates()
{
    Frame f = make_frame(1, 3000);
    auto dg = fragment(f); // 3 fragments
    UdpAssembler a(TIMEOUT_US);
    Frame out;
    CHECK(feed(a, dg[0], 1, out) == UdpAddResult::STORED);
    CHECK(feed(a, dg[0], 2, out) == UdpAddResult::DUPLICATE);
    CHECK(feed(a, dg[0], 3, out) == UdpAddResult::DUPLICATE);
    CHECK(feed(a, dg[1], 4, out) == UdpAddResult::STORED); // duplicates did not count toward completion
    CHECK(feed(a, dg[2], 5, out) == UdpAddResult::COMPLETE);
    CHECK(out.data == f.data);
    CHECK(a.stats().dup == 2);
    // A duplicate of a finished frame is "late"; it must not deliver the frame twice.
    CHECK(feed(a, dg[2], 6, out) == UdpAddResult::LATE);
    CHECK(a.pending_count() == 0);
}

static void test_missing_and_timeout()
{
    Frame f = make_frame(2, 5000, 777); // 5 fragments
    auto dg = fragment(f);
    UdpAssembler a(TIMEOUT_US);
    Frame out;
    for (std::size_t i = 0; i < dg.size(); i++)
        if (i != 2) // fragment 2 is lost
            CHECK(feed(a, dg[i], 1000, out) == UdpAddResult::STORED);
    CHECK(a.pending_count() == 1);

    std::vector<ExpiredFrame> gone;
    a.expire(1000 + TIMEOUT_US - 1);
    a.take_expired(gone);
    CHECK(gone.empty() && a.pending_count() == 1); // not yet

    a.expire(1000 + TIMEOUT_US);
    a.take_expired(gone);
    CHECK(gone.size() == 1);
    CHECK(gone[0].frame_id == 2 && gone[0].frags_got == 4 && gone[0].frags_total == 5);
    CHECK(gone[0].frame_len == 5000 && gone[0].timestamp_us == 777);
    CHECK(a.pending_count() == 0); // memory released

    // The missing fragment shows up after the timeout: it must NOT resurrect the frame.
    Frame untouched;
    CHECK(feed(a, dg[2], 1000 + TIMEOUT_US + 5, untouched) == UdpAddResult::LATE);
    CHECK(untouched.data.empty());
    CHECK(a.pending_count() == 0);
    gone.clear();
    a.take_expired(gone);
    CHECK(gone.empty()); // already reported once
}

static void test_invalid_fragments()
{
    Frame f = make_frame(3, 3000);
    auto dg = fragment(f);
    UdpAssembler a(TIMEOUT_US);
    Frame out;
    CHECK(feed(a, dg[0], 1, out) == UdpAddResult::STORED);

    UdpHdr h = dg[1].hdr;
    h.frag_idx = h.frag_cnt; // index out of range
    CHECK(a.add(h, dg[1].bytes.data() + UDP_HDR_LEN, 2, out) == UdpAddResult::INVALID);

    h = dg[1].hdr;
    h.frag_cnt = 7; // count that contradicts frame_len
    CHECK(a.add(h, dg[1].bytes.data() + UDP_HDR_LEN, 2, out) == UdpAddResult::INVALID);

    h = dg[1].hdr;
    h.payload_len = 10; // wrong length for this fragment
    CHECK(a.add(h, dg[1].bytes.data() + UDP_HDR_LEN, 2, out) == UdpAddResult::INVALID);

    h = dg[1].hdr;
    h.frame_len = 99999; // internally consistent, but disagrees with fragment 0's frame
    h.frag_cnt = static_cast<std::uint16_t>(udp_frag_count(h.frame_len));
    CHECK(a.add(h, dg[1].bytes.data() + UDP_HDR_LEN, 2, out) == UdpAddResult::INVALID);

    h = dg[1].hdr;
    h.timestamp_us += 1; // same frame id, different send time
    CHECK(a.add(h, dg[1].bytes.data() + UDP_HDR_LEN, 2, out) == UdpAddResult::INVALID);

    h = dg[1].hdr;
    h.type = MSG_END; // not a frame fragment
    CHECK(a.add(h, dg[1].bytes.data() + UDP_HDR_LEN, 2, out) == UdpAddResult::INVALID);

    CHECK(a.stats().invalid == 6);
    // None of that poisoned the frame: the real fragments still complete it.
    CHECK(feed(a, dg[1], 3, out) == UdpAddResult::STORED);
    CHECK(feed(a, dg[2], 4, out) == UdpAddResult::COMPLETE);
    CHECK(out.data == f.data);
}

static void test_interleaved_frames_and_bounds()
{
    UdpAssembler a(TIMEOUT_US);
    Frame fa = make_frame(10, 4000), fb = make_frame(11, 4000);
    auto da = fragment(fa), db = fragment(fb);
    Frame out;
    int done = 0;
    std::uint32_t order[2] = {0, 0};
    for (std::size_t i = 0; i < da.size(); i++) {
        if (feed(a, db[i], 1, out) == UdpAddResult::COMPLETE) order[done++] = out.id; // B first
        if (feed(a, da[i], 1, out) == UdpAddResult::COMPLETE) order[done++] = out.id;
    }
    CHECK(done == 2 && order[0] == 11 && order[1] == 10);

    // Memory bound: starting many frames never keeps more than MAX_PENDING.
    UdpAssembler b(TIMEOUT_US);
    for (std::uint32_t id = 100; id < 100 + 3 * UdpAssembler::MAX_PENDING; id++) {
        Frame f = make_frame(id, 3000);
        auto d = fragment(f);
        feed(b, d[0], id, out); // only the first fragment
        CHECK(b.pending_count() <= UdpAssembler::MAX_PENDING);
    }
    std::vector<ExpiredFrame> gone;
    b.take_expired(gone);
    CHECK(gone.size() == 2 * UdpAssembler::MAX_PENDING); // the evicted ones are reported, oldest first
    CHECK(gone.front().frame_id == 100);
}

static void test_flush_and_end_of_stream()
{
    UdpAssembler a(TIMEOUT_US);
    Frame out;
    auto d1 = fragment(make_frame(1, 3000));
    auto d2 = fragment(make_frame(2, 3000));
    feed(a, d1[0], 1, out);
    feed(a, d2[0], 1, out);
    a.flush(); // END reached: everything pending is now known to be incomplete
    std::vector<ExpiredFrame> gone;
    a.take_expired(gone);
    CHECK(gone.size() == 2 && a.pending_count() == 0);
    CHECK(feed(a, d1[1], 2, out) == UdpAddResult::LATE);
}

// Full path over a datagram socketpair: sender code -> bytes -> decode -> assembler.
static void test_end_to_end_socketpair()
{
    int sv[2];
    CHECK(socketpair(AF_UNIX, SOCK_DGRAM, 0, sv) == 0);
    Fd tx(sv[0]), rx(sv[1]);

    Frame f1 = make_frame(0, 45000, 111), f2 = make_frame(1, 800, 222);
    std::size_t n = 0;
    CHECK(udp_send_frame(tx.get(), 77, f1, 0, &n) && n == udp_frag_count(45000));
    CHECK(udp_send_frame(tx.get(), 77, f2, 0, &n) && n == 1);
    EndInfo e;
    e.frames = 2;
    e.bytes = f1.data.size() + f2.data.size();
    CHECK(udp_send_end(tx.get(), 77, e, 2));

    UdpAssembler a(TIMEOUT_US);
    std::vector<Frame> got;
    bool end_seen = false;
    EndInfo end_got;
    std::uint8_t buf[2048];
    for (;;) {
        int len = udp_recv(rx.get(), buf, sizeof buf, 50);
        if (len <= 0)
            break; // queue drained
        UdpHdr h;
        CHECK(decode_udp_hdr(buf, static_cast<std::size_t>(len), h) == ProtoStatus::OK);
        CHECK(h.session == 77);
        if (h.type == MSG_END) {
            end_seen = decode_end_info(buf + UDP_HDR_LEN, h.payload_len, end_got);
        } else {
            Frame out;
            if (a.add(h, buf + UDP_HDR_LEN, 1, out) == UdpAddResult::COMPLETE)
                got.push_back(out);
        }
    }
    CHECK(got.size() == 2);
    if (got.size() == 2) {
        CHECK(got[0].data == f1.data && got[0].timestamp_us == 111);
        CHECK(got[1].data == f2.data && got[1].id == 1);
    }
    CHECK(end_seen && end_got.frames == 2 && end_got.bytes == e.bytes);
    CHECK(a.stats().dup == 0 && a.stats().invalid == 0);
}

int main()
{
    test_in_order();
    test_out_of_order();
    test_duplicates();
    test_missing_and_timeout();
    test_invalid_fragments();
    test_interleaved_frames_and_bounds();
    test_flush_and_end_of_stream();
    test_end_to_end_socketpair();
    return TEST_RESULT();
}
