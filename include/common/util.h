// Small helpers shared by both executables: clock, stop flag, argument parsing.
#pragma once

#include <cstdint>

// Microseconds on std::chrono::steady_clock. On Linux this is CLOCK_MONOTONIC:
// shared by all processes on one host, but unrelated between two hosts.
std::uint64_t now_us();

// Sleep until now_us() reaches deadline_us. Used for pacing; not for sync.
void sleep_until_us(std::uint64_t deadline_us);

// SIGINT/SIGTERM set a stop flag. The handler is installed without SA_RESTART
// so blocking calls (accept, recv, poll) return EINTR and can notice the flag.
void install_stop_handler();
void request_stop();
bool stop_requested();

// Print "netpkt: <what>: <strerror(errno)>" to stderr.
void log_errno(const char* what);

// Strict number parsing: whole string must be a number within range.
bool parse_uint(const char* s, unsigned long max, unsigned long& out);
bool parse_double(const char* s, double& out);
