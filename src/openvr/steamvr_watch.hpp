// SteamVR 的"还在不在"判定，给「随 SteamVR 启动」的配套开关用（ADR-0047）。
//
// 【为什么要去抖】
// vrserver.exe 会自己重启（换驱动、崩溃恢复、SteamVR 内部重载）。一次瞬时消失就退出
// FGB，等于用户每次重启 SteamVR 都被踢出来一次；而 SteamVR 真的关掉之后 FGB 还占着
// 摄像头与模块也没有意义。
//
// 【为什么必须"先看见过"】
// 用户手动双击启动 FGB 时 SteamVR 可能还没起来。这时看不见 vrserver 是正常状态，
// 不能判成"SteamVR 退出了"——否则下次用户一开 FGB 就被自己关掉。
#pragma once

#include <cstdint>

namespace fgb::steamvr {

// 纯状态机：喂"此刻是否在跑"，而不是自己去查进程。这样单测可以驱动任意时间轴，
// 也不会因为测试机上刚好装了 SteamVR 就让用例变成随机通过。
class ExitWatch {
public:
    // SteamVR 关掉后要连续这么久看不见 vrserver.exe 才认定为退出。
    // 5 秒足够跨过 SteamVR 自己重启 vrserver 的空窗，又不至于让用户在椅子里等。
    static constexpr std::uint64_t kGoneTimeoutMs = 5000;

    // enabled：用户是否打开了「SteamVR 退出后自动退出」；
    // present：此刻 vrserver.exe 是否在跑；now_ms：单调毫秒。
    // 返回 true 表示这一拍该退出 FGB，且只返回一次——调用方没退成也不会反复报。
    bool observe(bool enabled, bool present, std::uint64_t now_ms);

    void reset();

    // 调试与测试用：区分"还没起来"和"退出过"。
    bool saw_steamvr() const noexcept { return seen_; }

private:
    bool seen_  = false;
    bool fired_ = false;
    // 0 = 没在计时（vrserver 在跑，或者压根还没看见过）
    std::uint64_t gone_since_ms_ = 0;
};

} // namespace fgb::steamvr
