#include "base/clock.hpp"

#include <thread>

#ifdef _WIN32
#include <windows.h>
#include <mmsystem.h>
#endif
#if defined(_MSC_VER)
#include <intrin.h>
#elif defined(__x86_64__) || defined(__i386__)
#include <immintrin.h>
#endif

namespace fgb::clock {
namespace {

#ifdef _WIN32
std::uint64_t qpc_freq() noexcept {
    static const std::uint64_t freq = [] {
        LARGE_INTEGER f{};
        QueryPerformanceFrequency(&f);
        return static_cast<std::uint64_t>(f.QuadPart);
    }();
    return freq;
}
#endif

} // namespace

Ns now_ns() noexcept {
#ifdef _WIN32
    LARGE_INTEGER c{};
    QueryPerformanceCounter(&c);
    const std::uint64_t ticks = static_cast<std::uint64_t>(c.QuadPart);
    const std::uint64_t freq  = qpc_freq();
    // 拆成整秒 + 余数，避免 ticks * 1e9 溢出
    return (ticks / freq) * 1'000'000'000ULL + ((ticks % freq) * 1'000'000'000ULL) / freq;
#else
    using namespace std::chrono;
    return static_cast<Ns>(duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count());
#endif
}

void raise_timer_resolution() noexcept {
#ifdef _WIN32
    timeBeginPeriod(1);
#endif
}

void restore_timer_resolution() noexcept {
#ifdef _WIN32
    timeEndPeriod(1);
#endif
}

void sleep_for_ns(Ns ns) noexcept {
    if (ns == 0) {
        return;
    }
    const Ns deadline = now_ns() + ns;
    sleep_until_ns(deadline);
}

void sleep_until_ns(Ns deadline) noexcept {
    for (;;) {
        const Ns now = now_ns();
        if (now >= deadline) {
            return;
        }
        const Ns remain = deadline - now;
        if (remain <= kSpinThresholdNs) {
            // 自旋收尾，避免被调度器睡过头
            while (now_ns() < deadline) {
#if defined(_MSC_VER)
                _mm_pause();
#elif defined(__x86_64__) || defined(__i386__)
                __builtin_ia32_pause();
#endif
            }
            return;
        }
        // 留出收尾时间再唤醒
        std::this_thread::sleep_for(std::chrono::nanoseconds(remain - kSpinThresholdNs / 2));
    }
}

} // namespace fgb::clock
