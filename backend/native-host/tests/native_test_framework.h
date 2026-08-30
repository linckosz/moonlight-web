/*
 * MoonlightWeb — native capture & encoding engine, test suite.
 * Copyright (C) 2026 Bruno Martin <brunoocto@gmail.com>. GPLv3.
 */
#pragma once

#include <cstdio>

// Deliberately a copy of backend/tests/test_framework.h rather than an include
// of it. The module — tests included — has to stay extractable as a unit, and
// reaching up into the host application's test tree for forty lines of
// assertion macros would break that for no benefit. Kept identical in idiom so
// a reader moving between the two suites is never surprised.

struct NativeTestStats
{
    int passed = 0;
    int failed = 0;
};

extern NativeTestStats g_nativeStats;

inline void mw_native_check(bool ok, const char* expr, const char* file, int line)
{
    if (ok) {
        g_nativeStats.passed++;
    } else {
        g_nativeStats.failed++;
        std::fprintf(stderr, "  [FAIL] %s:%d - %s\n", file, line, expr);
    }
}

template <class A, class B>
inline void mw_native_check_eq(const A& a, const B& b, const char* expr, const char* file, int line)
{
    mw_native_check(a == b, expr, file, line);
}

#define CHECK(cond) mw_native_check((cond), #cond, __FILE__, __LINE__)
#define CHECK_EQ(a, b) mw_native_check_eq((a), (b), #a " == " #b, __FILE__, __LINE__)
#define SECTION(name) std::fprintf(stderr, "=== %s\n", name)
