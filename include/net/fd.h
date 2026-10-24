// RAII owner of a file descriptor (socket). Move-only; closes on destruction.
#pragma once

#include <unistd.h>

class Fd {
public:
    Fd() = default;
    explicit Fd(int fd) : fd_(fd) {}
    ~Fd() { reset(); }

    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
    Fd(Fd&& o) noexcept : fd_(o.release()) {}
    Fd& operator=(Fd&& o) noexcept
    {
        if (this != &o)
            reset(o.release());
        return *this;
    }

    int get() const { return fd_; }
    bool valid() const { return fd_ >= 0; }
    int release()
    {
        int f = fd_;
        fd_ = -1;
        return f;
    }
    void reset(int fd = -1)
    {
        if (fd_ >= 0)
            ::close(fd_);
        fd_ = fd;
    }

private:
    int fd_ = -1;
};
