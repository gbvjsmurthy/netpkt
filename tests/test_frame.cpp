// JPEG helpers and the play queue.
#include <chrono>
#include <thread>

#include <opencv2/core.hpp>

#include "check.h"
#include "common/frame.h"
#include "common/frame_queue.h"

static void test_jpeg()
{
    cv::Mat img(48, 64, CV_8UC3);
    for (int y = 0; y < img.rows; y++)
        for (int x = 0; x < img.cols; x++)
            img.at<cv::Vec3b>(y, x) = cv::Vec3b(static_cast<uchar>(x * 4), static_cast<uchar>(y * 5), 128);

    std::vector<std::uint8_t> jpg;
    CHECK(jpeg_encode(img, 90, jpg));
    CHECK(jpg.size() > 4 && jpg[0] == 0xFF && jpg[1] == 0xD8); // JPEG start marker

    cv::Mat back;
    CHECK(jpeg_decode(jpg, back));
    CHECK(back.cols == 64 && back.rows == 48 && back.channels() == 3);
    CHECK(cv::norm(img, back, cv::NORM_L1) / (img.total() * 3) < 8.0); // lossy, but close

    // lower quality must give a smaller file
    std::vector<std::uint8_t> small;
    CHECK(jpeg_encode(img, 10, small));
    CHECK(small.size() < jpg.size());

    // garbage and empty input must fail cleanly, not crash
    std::vector<std::uint8_t> junk(500, 0x42);
    cv::Mat none;
    CHECK(!jpeg_decode(junk, none));
    CHECK(!jpeg_decode({}, none));
    CHECK(!jpeg_encode(cv::Mat(), 80, jpg));

    // a JPEG cut in half must not be reported as a good frame
    std::vector<std::uint8_t> cut(jpg.begin(), jpg.begin() + 20);
    CHECK(!jpeg_decode(cut, none));

    cv::Mat r;
    resize_to_width(img, 32, r);
    CHECK(r.cols == 32 && r.rows == 24);
    resize_to_width(img, 0, r);
    CHECK(r.cols == 64);
}

static Frame fr(std::uint32_t id)
{
    Frame f;
    f.id = id;
    f.data = {1, 2, 3};
    return f;
}

static void test_queue()
{
    FrameQueue q(3);
    std::uint32_t dropped = 99;
    CHECK(!q.push(fr(0), dropped) && !q.push(fr(1), dropped) && !q.push(fr(2), dropped));
    CHECK(q.push(fr(3), dropped) && dropped == 0); // full: oldest goes
    CHECK(q.push(fr(4), dropped) && dropped == 1);

    Frame out;
    for (std::uint32_t want : {2u, 3u, 4u}) {
        CHECK(q.pop(out, 10) == FrameQueue::Pop::FRAME);
        CHECK(out.id == want); // FIFO order preserved
    }
    CHECK(q.pop(out, 10) == FrameQueue::Pop::TIMEOUT);

    // close() must wake a blocked pop() promptly
    auto t0 = std::chrono::steady_clock::now();
    std::thread closer([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        q.close();
    });
    CHECK(q.pop(out, 5000) == FrameQueue::Pop::CLOSED);
    closer.join();
    CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::seconds(2));

    // frames queued before close() are still delivered
    FrameQueue q2(4);
    q2.push(fr(7), dropped);
    q2.close();
    CHECK(q2.pop(out, 10) == FrameQueue::Pop::FRAME && out.id == 7);
    CHECK(q2.pop(out, 10) == FrameQueue::Pop::CLOSED);
}

int main()
{
    test_jpeg();
    test_queue();
    return TEST_RESULT();
}
