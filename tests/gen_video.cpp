// Writes a small synthetic test video (moving shapes + noise, so the JPEG sizes
// are not trivially tiny). usage: gen_video OUT.avi [frames] [fps] [width] [height]
#include <cstdio>
#include <cstdlib>

#include <opencv2/imgproc.hpp>
#include <opencv2/videoio.hpp>

int main(int argc, char** argv)
{
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s OUT.avi [frames=90] [fps=30] [width=640] [height=360]\n", argv[0]);
        return 2;
    }
    int frames = argc > 2 ? std::atoi(argv[2]) : 90;
    double fps = argc > 3 ? std::atof(argv[3]) : 30.0;
    int w = argc > 4 ? std::atoi(argv[4]) : 640;
    int h = argc > 5 ? std::atoi(argv[5]) : 360;
    if (frames < 1 || fps <= 0 || w < 16 || h < 16) {
        std::fprintf(stderr, "error: invalid arguments\n");
        return 2;
    }

    cv::VideoWriter vw(argv[1], cv::VideoWriter::fourcc('M', 'J', 'P', 'G'), fps, cv::Size(w, h));
    if (!vw.isOpened()) {
        std::fprintf(stderr, "error: cannot open '%s' for writing\n", argv[1]);
        return 1;
    }
    cv::RNG rng(1); // fixed seed: same video every time
    cv::Mat noise(h, w, CV_8UC3);
    for (int i = 0; i < frames; i++) {
        cv::Mat img(h, w, CV_8UC3);
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++)
                img.at<cv::Vec3b>(y, x) = cv::Vec3b(static_cast<uchar>((x + 3 * i) / 3),
                                                    static_cast<uchar>(y * 255 / h),
                                                    static_cast<uchar>(128 + 100 * ((x / 40 + y / 40 + i / 5) % 2)));
        cv::circle(img, cv::Point((i * 9) % w, h / 2 + (i * 5) % (h / 3)), h / 6, cv::Scalar(30, 220, 240), -1);
        cv::putText(img, "netpkt " + std::to_string(i), cv::Point(10, 30), cv::FONT_HERSHEY_SIMPLEX, 1.0,
                    cv::Scalar(255, 255, 255), 2);
        rng.fill(noise, cv::RNG::NORMAL, 0, 12);
        cv::add(img, noise, img);
        vw << img;
    }
    std::printf("wrote %d frames (%dx%d @ %.1f fps) to %s\n", frames, w, h, fps, argv[1]);
    return 0;
}
