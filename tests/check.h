// Minimal test helper: CHECK() records a failure and keeps going.
#pragma once

#include <cstdio>

inline int g_fails = 0;

#define CHECK(cond)                                                                   \
    do {                                                                              \
        if (!(cond)) {                                                                \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
            ++g_fails;                                                                \
        }                                                                             \
    } while (0)

#define TEST_RESULT()                                                                 \
    (g_fails == 0 ? (std::printf("OK\n"), 0) : (std::printf("%d check(s) FAILED\n", g_fails), 1))
