#include "core/clock.hpp"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <timeapi.h>
#endif
#ifdef __APPLE__
#include <pthread.h>
#include <pthread/qos.h>
#endif

namespace arq {

void preciseTimersForThisThread() {
#ifdef __APPLE__
    // User-interactive QoS threads are exempt from timer coalescing.
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
#endif
}

HighResTimerScope::HighResTimerScope() {
#ifdef _WIN32
    timeBeginPeriod(1);
#endif
    preciseTimersForThisThread();
}

HighResTimerScope::~HighResTimerScope() {
#ifdef _WIN32
    timeEndPeriod(1);
#endif
}

}  // namespace arq
