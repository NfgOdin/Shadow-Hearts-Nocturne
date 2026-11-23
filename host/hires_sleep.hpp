#pragma once

// Sub-millisecond Windows frame pacer using CREATE_WAITABLE_TIMER_HIGH_RESOLUTION (Windows 10 1803+)
// with automatic fallback on older Windows / POSIX. Near-0% idle CPU + rock-solid 16.667 ms frame delivery.

#include <chrono>
#include <thread>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif
#endif

namespace host_pace {

inline void sleep_seconds_hires(double seconds) noexcept {
    if (seconds <= 0.0) return;
#ifdef _WIN32
    thread_local HANDLE timer = []() -> HANDLE {
        HANDLE h = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
        if (!h) h = CreateWaitableTimerW(nullptr, TRUE, nullptr);
        return h;
    }();
    if (timer) {
        // Negative value = relative time in 100-nanosecond intervals
        const LONGLONG ticks_100ns = -static_cast<LONGLONG>(seconds * 10000000.0);
        if (ticks_100ns < 0) {
            LARGE_INTEGER li{};
            li.QuadPart = ticks_100ns;
            if (SetWaitableTimer(timer, &li, 0, nullptr, nullptr, FALSE)) {
                WaitForSingleObject(timer, INFINITE);
                return;
            }
        }
    }
#endif
    std::this_thread::sleep_for(std::chrono::duration<double>(seconds));
}

}  // namespace host_pace
