#include "openvr/steamvr_watch.hpp"

namespace fgb::steamvr {

bool ExitWatch::observe(bool enabled, bool present, std::uint64_t now_ms) {
    if (!enabled) {
        // 用户中途关掉开关：计时与"看见过"都要清掉，重新打开时从头开始，
        // 否则关掉再打开会立刻触发上一次残留的倒计时。
        reset();
        return false;
    }
    if (fired_) {
        return false;
    }
    if (present) {
        seen_          = true;
        gone_since_ms_ = 0;
        return false;
    }
    if (!seen_) {
        return false; // 还没见过 SteamVR（用户先开的 FGB），不能算"它退出了"
    }
    if (gone_since_ms_ == 0 || now_ms < gone_since_ms_) {
        // 第二项挡住系统休眠恢复后的时间跳变：那不该被当成"已经消失了很久"。
        gone_since_ms_ = now_ms;
        return false;
    }
    if (now_ms - gone_since_ms_ < kGoneTimeoutMs) {
        return false;
    }
    fired_ = true;
    return true;
}

void ExitWatch::reset() {
    seen_          = false;
    fired_         = false;
    gone_since_ms_ = 0;
}

} // namespace fgb::steamvr
