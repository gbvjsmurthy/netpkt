#include "common/frame.h"

#include <cstdio>

#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

bool jpeg_encode(const cv::Mat& img, int quality, std::vector<std::uint8_t>& out)
{
    if (img.empty())
        return false;
    try {
        std::vector<int> params = {cv::IMWRITE_JPEG_QUALITY, quality};
        return cv::imencode(".jpg", img, out, params) && !out.empty();
    } catch (const cv::Exception& e) {
        std::fprintf(stderr, "netpkt: jpeg encode failed: %s\n", e.what());
        return false;
    }
}

bool jpeg_decode(const std::vector<std::uint8_t>& data, cv::Mat& img)
{
    if (data.empty())
        return false;
    try {
        // Wrap the bytes without copying; imdecode only reads them.
        cv::Mat raw(1, static_cast<int>(data.size()), CV_8UC1,
                    const_cast<std::uint8_t*>(data.data()));
        img = cv::imdecode(raw, cv::IMREAD_COLOR);
        return !img.empty();
    } catch (const cv::Exception& e) {
        std::fprintf(stderr, "netpkt: jpeg decode failed: %s\n", e.what());
        return false;
    }
}

void resize_to_width(const cv::Mat& src, int width, cv::Mat& dst)
{
    if (width <= 0 || width == src.cols) {
        dst = src;
        return;
    }
    int height = static_cast<int>(static_cast<double>(src.rows) * width / src.cols + 0.5);
    if (height < 1)
        height = 1;
    cv::resize(src, dst, cv::Size(width, height), 0, 0, cv::INTER_AREA);
}
