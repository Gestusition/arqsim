#pragma once
#include <chrono>
#include <cstdint>

namespace arq {

using Clock = std::chrono::steady_clock;
using TimePoint = Clock::time_point;
using Micros = std::chrono::microseconds;

inline double msBetween(TimePoint a, TimePoint b) {
    return std::chrono::duration<double, std::milli>(b - a).count();
}

inline TimePoint afterMs(TimePoint t, double ms) {
    return t + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double, std::milli>(ms));
}

/// Raises the OS scheduler/timer resolution to ~1 ms for the lifetime of the
/// object (Windows defaults to 15.6 ms, which would distort RTT emulation).
class HighResTimerScope {
public:
    HighResTimerScope();
    ~HighResTimerScope();
    HighResTimerScope(const HighResTimerScope&) = delete;
    HighResTimerScope& operator=(const HighResTimerScope&) = delete;
};

}  // namespace arq
