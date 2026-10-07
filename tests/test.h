#pragma once
// Minimal assertion helpers for the test programs.
//
// A test framework would be one more thing to fetch and keep in step with three
// toolchains; these few macros cover what is needed and print a useful message
// when something fails.

#include <cstdio>
#include <string>

namespace newisp_test {

    inline int& Failures()
    {
        static int failures = 0;
        return failures;
    }

    inline void Report(const char* file, int line, const std::string& what)
    {
        std::fprintf(stderr, "FAIL %s:%d: %s\n", file, line, what.c_str());
        ++Failures();
    }

    inline void ReportEq(const char* file, int line, const std::string& what,
        const std::string& got, const std::string& want)
    {
        std::fprintf(stderr, "FAIL %s:%d: %s\n      got:  %s\n      want: %s\n",
            file, line, what.c_str(), got.c_str(), want.c_str());
        ++Failures();
    }

} // namespace newisp_test

#define CHECK(cond)                                                       \
    do {                                                                  \
        if (!(cond)) ::newisp_test::Report(__FILE__, __LINE__, #cond);    \
    } while (0)

#define CHECK_EQ(got, want)                                               \
    do {                                                                  \
        auto g_ = (got);                                                  \
        auto w_ = (want);                                                 \
        if (!(g_ == w_)) {                                                \
            ::newisp_test::ReportEq(__FILE__, __LINE__, #got " == " #want,\
                std::to_string(g_), std::to_string(w_));                  \
        }                                                                 \
    } while (0)

#define CHECK_STREQ(got, want)                                            \
    do {                                                                  \
        std::string g_ = (got);                                           \
        std::string w_ = (want);                                          \
        if (g_ != w_) {                                                   \
            ::newisp_test::ReportEq(__FILE__, __LINE__,                   \
                #got " == " #want, g_, w_);                               \
        }                                                                 \
    } while (0)
