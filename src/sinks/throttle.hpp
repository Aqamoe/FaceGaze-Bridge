// 按截止时间调度输出，避免"每次都比上次晚一个 tick"的取整误差。
//
// 引擎 tick 是 120 Hz（8.333 ms），而输出常见 90/60/30 Hz。用
// "now - last >= interval" 判断时，非整数倍间隔会被量化到上一档：
//   30 Hz（33.333 ms）在 120 Hz 下要等 5 tick = 41.7 ms → 实际约 24 Hz
//   90 Hz（11.111 ms）要等 2 tick = 16.7 ms → 实际 60 Hz
// 这里改成维护 next_send，允许 1 ms 的调度松弛；落后太多时重新起算，避免
// 调试/暂停后一次性补发一串帧。
#pragma once

#include <algorithm>
#include <cstdint>

namespace fgb {

class SinkThrottle {
public:
    bool due(std::uint64_t now_ns, std::uint64_t interval_ns) noexcept {
        if (interval_ns == 0) {
            return true; // 每 tick
        }
        if (next_ == 0) {
            next_ = now_ns + interval_ns;
            return true;
        }

        // 1 ms 松弛让 120 Hz tick 能落在 33.333/11.111 ms 的目标附近
        const std::uint64_t slack = std::min<std::uint64_t>(1'000'000, interval_ns / 4);
        if (now_ns + slack < next_) {
            return false;
        }

        if (now_ns > next_ + interval_ns * 4) {
            next_ = now_ns + interval_ns; // 暂停/调试后不要追发积压帧
        } else {
            next_ += interval_ns;
        }
        return true;
    }

    void reset() noexcept { next_ = 0; }

private:
    std::uint64_t next_ = 0;
};

} // namespace fgb
