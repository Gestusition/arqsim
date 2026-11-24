#pragma once
// Thread-safe, optional event trace (enabled with --verbose).
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <mutex>

#include "core/clock.hpp"

namespace arq {

class Trace {
public:
    static Trace& instance() {
        static Trace t;
        return t;
    }
    void enable(bool on) {
        enabled_ = on;
        start_ = Clock::now();
    }
    bool enabled() const { return enabled_; }

    void log(const char* who, const char* fmt, ...) {
        if (!enabled_) return;
        std::lock_guard<std::mutex> lock(mu_);
        std::fprintf(stderr, "[%10.3f ms] %-4s ", msBetween(start_, Clock::now()), who);
        va_list args;
        va_start(args, fmt);
        std::vfprintf(stderr, fmt, args);
        va_end(args);
        std::fputc('\n', stderr);
    }

private:
    std::atomic<bool> enabled_{false};
    TimePoint start_ = Clock::now();
    std::mutex mu_;
};

#define ARQ_TRACE(who, ...)                                                          \
    do {                                                                             \
        if (::arq::Trace::instance().enabled()) ::arq::Trace::instance().log(who, __VA_ARGS__); \
    } while (0)

}  // namespace arq
