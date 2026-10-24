// A frame as it travels through the system, plus the OpenCV JPEG helpers.
#pragma once

#include <cstdint>
#include <vector>

#include <opencv2/core.hpp>

struct Frame {
    std::uint32_t id = 0;            // 0,1,2,... assigned by the server
    std::uint64_t timestamp_us = 0;  // server steady-clock time, taken just before the first byte is sent
    std::vector<std::uint8_t> data;  // one complete JPEG image
};

// Same quality setting is used for TCP and UDP runs.
bool jpeg_encode(const cv::Mat& img, int quality, std::vector<std::uint8_t>& out);
bool jpeg_decode(const std::vector<std::uint8_t>& data, cv::Mat& img);

// Resize keeping the aspect ratio. width <= 0 or equal to the source: plain copy.
void resize_to_width(const cv::Mat& src, int width, cv::Mat& dst);
