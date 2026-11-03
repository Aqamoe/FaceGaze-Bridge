// HMD 身份覆盖（无 EDID 头显的"持久 EDID"）。
//
// 【为什么需要】
// VRSS 的 provider 与游戏进程必须用同一个 (vendor, product) 路由键，NVIDIA 驱动才会把
// 注视数据转给游戏。有 EDID 的头显两边都从 OpenVR 读同一个值；PICO/串流这类虚拟 HMD
// 没有 EDID，只能由 FGB 自己造一个键（candidate-auto），此时游戏那边必须被"告诉"同一个键。
//
// 【为什么是环境变量，而不是写显示器 EDID】
// 匹配用的是**游戏进程上报的值**，不是 Windows 显示器 EDID：注入一个虚构 vendor
// 0xFEEE 也能连上，就说明驱动认的是应用侧上报。显示器 EDID 属于另一层，串流驱动是否
// 透传不可控。上游 VRSS-Tuner 的受保护启动分支同样用 TEST_VENDOR_ID / TEST_PRODUCT_ID。
//
// 【为什么可以持久化】
// 变量由游戏进程读取，而 Steam 启动游戏时子进程继承 Steam 的环境块；把两个变量写成
// 用户级环境变量并重启 Steam 之后，从 Steam 正常启动 VRChat 也能带上，不再需要 FGB
// 亲自拉起游戏。
//
// 边界（必须写进文档）：
//   - 变量是用户级的，会影响该用户下**所有** OpenVR 应用；换回真 EDID 头显必须清除。
//   - Steam 客户端在进程启动时固定环境块，改完变量必须完全退出并重启 Steam 才生效。
//   - 读取点在 SteamVR/VRChat 的明文里找不到（大概率在加壳的受保护启动器里），属于
//     实证有效、机制未确认；本模块只负责把值放到位。
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace fgb::vrss {

// 无 EDID 头显使用的固定会话键。0xFEEE 是本地私有 vendor，不是真实 EDID 分配；
// 固定 product 是为了让持久化的环境变量不随串流会话序列号变化而失效。
inline constexpr std::uint32_t kSessionVendor  = 0xFEEE;
inline constexpr std::uint32_t kSessionProduct = 0x0001;

struct PersistentIdentity {
    bool read_ok                = false; // 注册表读取成功（不代表已设置）
    bool set                    = false; // 两个变量都在
    bool ours                   = false; // 与 marker 一致，即由 FGB 写入
    bool steam_restart_required = false; // 已写入，但当前 Steam 进程早于写入时间
    std::uint32_t vendor        = 0;
    std::uint32_t product       = 0;
    std::string error;
};

// 写入格式，与 runtime.cpp 的注入保持一致："0xFEEE"。
std::string identity_env_text(std::uint32_t value);

// 接受 "0xFEEE" / "0Xfeee" / "65262"；超过 0..65535 或含其它字符返回 false。
bool parse_identity_env_text(std::string_view text, std::uint32_t& out);

// 该几何身份是否必须由 FGB 覆盖：openvr-edid 之外（candidate-auto / candidate-manual /
// tuner-session）都算"头显自己给不出可用身份"。
bool needs_identity_override(std::string_view identity_source) noexcept;

// 把身份写成用户级环境变量（HKCU\Environment + 当前进程 + 广播）。
// 不需要管理员权限；失败时 error 写具体原因。
bool apply_persistent_identity(std::uint32_t vendor, std::uint32_t product, std::string& error);

// 只清除由 FGB 自己写入的那一对（marker 不匹配时不动，避免覆盖用户手动设置的值）。
bool clear_persistent_identity(std::string& error);

// 读当前状态并判断"要不要重启 Steam"。
PersistentIdentity persistent_identity();

} // namespace fgb::vrss
