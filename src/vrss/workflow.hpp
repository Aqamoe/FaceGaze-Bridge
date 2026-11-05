// VRSS 一键流程的六步状态机。
//
// 真正的硬件操作都在 Runtime；这里只负责"上一步通过才进下一步"、把结果变成
// UI 可显示的快照，并把需要用户参与的动作（MSAA 确认、UAC、启动 VRChat）停在
// blocked，而不是自己猜着继续。
#pragma once

#include "vrss/identity.hpp"
#include "vrss/runtime.hpp"

#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

namespace fgb::vrss {

enum class StepId : std::uint8_t { Env, Driver, DriverAudit, Admin, Provider, Vrchat, Verify };
inline constexpr std::size_t kStepCount = 7;

enum class StepState : std::uint8_t { Pending, Running, Pass, Fail, Blocked, Skipped };

struct StepStatus {
    StepId id     = StepId::Env;
    StepState state = StepState::Pending;
    std::string detail;
};

struct WorkflowSnapshot {
    bool enabled = false;
    std::string state = "off"; // off | running | blocked | failed | stopped
    std::array<StepStatus, kStepCount> steps{};
    DriverInfo driver;
    bool steamvr = false;
    bool geometry = false;
    bool admin = false;
    bool vrchat_running = false;
    // 无 EDID 头显的持久身份：写入后 Steam 直接启动 VRChat 也能对上 provider。
    PersistentIdentity identity;
    std::string identity_source; // 最近一次几何导出的 identitySource
    bool auto_start = false;     // 跟随 SteamVR 自动注册
    bool persist_identity = true;
    ProviderStatus provider;
    std::string area_state = "idle"; // idle | applying | applied | failed
    std::string area_detail;
    std::string last_error;
};

class Workflow {
public:
    Workflow();
    ~Workflow();

    void configure(bool enabled, int port, bool auto_launch_vrchat, float area_percent,
                   bool auto_start, bool persist_identity);
    bool enabled() const noexcept { return enabled_; }
    int port() const noexcept { return port_; }

    // 启动/继续一键流程（后台线程）。已通过且仍有效的步骤会跳过。
    void start();
    // 步骤 3 的一键确认：用户已开启 MSAA 并允许 VRChat 使用 VRSS。
    void confirm_msaa();
    void request_launch_vrchat();
    // 写 NVIDIA VRChat profile 的抗锯齿范围（会弹 UAC；不影响 provider 生命周期）。
    void apply_area(float percent);
    // 清除 FGB 自己写入的持久 HMD 身份（后台线程：广播环境变化可能阻塞）。
    void clear_identity();
    // 停止 provider 并清空流程；不阻塞调用线程。
    void stop();
    void cancel();

    // 由 IPC 以 2 Hz 调用：刷新 provider 状态与验证步骤。
    void refresh();

    WorkflowSnapshot snapshot() const;

private:
    void run_steps();
    void set_step(StepId id, StepState state, const std::string& detail);
    StepState step_state(StepId id) const;
    bool step_passed(StepId id) const;
    void finish_state();
    void join_worker();
    // 用"上次写入成功的记录 + 当前设置值"回填 area_state（重启后不再一律显示未应用）
    void remember_area_state(float configured);

    mutable std::mutex mutex_;
    WorkflowSnapshot snap_;
    Runtime runtime_;
    std::thread worker_;
    std::thread stopper_;
    std::thread area_worker_;
    std::thread identity_worker_;
    std::atomic<bool> busy_{false};
    std::atomic<bool> cancel_{false};
    bool enabled_           = false;
    int port_               = 5556;
    bool auto_launch_       = false;
    float area_percent_     = 10.0f;
    // 已知"已经写进 NVIDIA profile"的范围；0 = 还没写过/不知道。
    // 只在内存里，重启后由 remember_area_state() 从磁盘记录恢复。
    float area_applied_percent_ = 0.0f;
    bool msaa_confirmed_    = false;
    bool vrchat_requested_  = false;
    // 跟随 SteamVR 自动注册：只在"这一轮 SteamVR 会话"里自动跑一次，避免
    // 用户取消 UAC 之后每 2 秒再弹一次。
    bool auto_start_        = false;
    bool auto_session_      = false;
    bool persist_identity_  = true;
    // 本次流程里持久身份是否与 provider 的注册身份一致。
    bool identity_ready_    = false;
};

} // namespace fgb::vrss
