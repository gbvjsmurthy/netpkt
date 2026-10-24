#include "common/util.h"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

namespace {

// A signal handler cannot receive arguments, so the stop flag has to be global.
// A lock-free atomic is safe to set from a handler and to read from any thread.
std::atomic<bool> g_stop{false};
static_assert(std::atomic<bool>::is_always_lock_free, "stop flag must be lock-free");

void on_signal(int)
{
    g_stop.store(true);
}

} // namespace

std::uint64_t now_us()
{
    using namespace std::chrono;
    return static_cast<std::uint64_t>(
        duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count());
}

void sleep_until_us(std::uint64_t deadline_us)
{
    using namespace std::chrono;
    std::this_thread::sleep_until(steady_clock::time_point(microseconds(deadline_us)));
}

void install_stop_handler()
{
    struct sigaction sa;
    std::memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0; // deliberately no SA_RESTART
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);
    // A dead peer should give send() an EPIPE error, not kill the process.
    std::signal(SIGPIPE, SIG_IGN);
}

void request_stop() { g_stop.store(true); }
bool stop_requested() { return g_stop.load(); }

void log_errno(const char* what)
{
    int err = errno;
    std::fprintf(stderr, "netpkt: %s: %s\n", what, std::strerror(err));
}

bool parse_uint(const char* s, unsigned long max, unsigned long& out)
{
    // strtoul silently accepts "-5" (wraps around) and leading spaces.
    if (s == nullptr || *s < '0' || *s > '9')
        return false;
    errno = 0;
    char* end = nullptr;
    unsigned long v = std::strtoul(s, &end, 10);
    if (errno != 0 || *end != '\0' || v > max)
        return false;
    out = v;
    return true;
}

bool parse_double(const char* s, double& out)
{
    if (s == nullptr || *s == '\0')
        return false;
    errno = 0;
    char* end = nullptr;
    double v = std::strtod(s, &end);
    if (errno != 0 || *end != '\0' || !(v == v)) // v == v rejects NaN
        return false;
    out = v;
    return true;
}
