/*
 * Moonlight-Web — TNR suite. Copyright (C) 2026 Bruno Martin. GPLv3.
 *
 * Tiny assertion framework shared by the backend unit tests. Free-function
 * style (no Q_OBJECT test classes) to keep the moc/compile surface minimal.
 * Each test file exposes a run_*_tests() entry summed by main().
 *
 * Output goes through fprintf(stderr), NOT qInfo()/qCritical(): the production
 * Logger installs a Qt message handler that would otherwise swallow our lines.
 */
#pragma once

#include <cstdio>

struct TestStats
{
    int passed = 0;
    int failed = 0;
};

extern TestStats g_stats;

inline void mw_check(bool ok, const char* expr, const char* file, int line)
{
    if (ok) {
        g_stats.passed++;
    } else {
        g_stats.failed++;
        std::fprintf(stderr, "  [FAIL] %s:%d - %s\n", file, line, expr);
    }
}

template <class A, class B>
inline void mw_check_eq(const A& a, const B& b, const char* expr, const char* file, int line)
{
    mw_check(a == b, expr, file, line);
}

#define CHECK(cond) mw_check((cond), #cond, __FILE__, __LINE__)
#define CHECK_EQ(a, b) mw_check_eq((a), (b), #a " == " #b, __FILE__, __LINE__)
#define SECTION(name) std::fprintf(stderr, "=== %s\n", name)
