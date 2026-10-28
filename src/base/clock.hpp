// 单调时钟与定时等待。
//
// 为什么不用 std::chrono::steady_clock：Windows 上它的分辨率依赖系统时钟中断
// （默认 15.6 ms），对 120 Hz（8.33 ms）的 tick 完全不够。这里统一封装 QPC，
// 并把 timeBeginPeriod 的影响收敛在一个地方。
#pragma once

#include <cstdint>

namespace fgb::clock {

using Ns = std::uint64_t;

// 单调递增，单位纳秒。进程内所有时间戳都用它。
Ns now_ns() noexcept;

// 提升当前进程的系统定时器分辨率（Windows 生效，其他平台 no-op）。
// 进程退出前应调用 restore_timer_resolution()。
void raise_timer_resolution() noexcept;
void restore_timer_resolution() noexcept;

// 忙等收尾的阈值：剩余时间小于该值时自旋而非睡眠，把抖动压到亚毫秒。
constexpr Ns kSpinThresholdNs = 700'000; // 0.7 ms

void sleep_for_ns(Ns ns) noexcept;
void sleep_until_ns(Ns deadline) noexcept;

} // namespace fgb::clock
