#include "core/clock.hpp"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <timeapi.h>
#endif

namespace arq {

HighResTimerScope::HighResTimerScope() {
#ifdef _WIN32
    timeBeginPeriod(1);
#endif
}

HighResTimerScope::~HighResTimerScope() {
#ifdef _WIN32
    timeEndPeriod(1);
#endif
}

}  // namespace arq
