// VRSS 一键流程的六步状态机。
//
// 真正的硬件操作都在 Runtime；这里只负责"上一步通过才进下一步"、把结果变成
// UI 可显示的快照，并把需要用户参与的动作（MSAA 确认、UAC、启动 VRChat）停在
// blocked，而不是自己猜着继续。
#pragma once

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
    ProviderStatus provider;
    std::string area_state = "idle"; // idle | applying | applied | failed
    std::string area_detail;
    std::string last_error;
};

class Workflow {
public:
    Workflow();
    ~Workflow();

    void configure(bool enabled, int port, bool auto_launch_vrchat, float area_percent);
    bool enabled() const noexcept { return enabled_; }
    int port() const noexcept { return port_; }

    // 启动/继续一键流程（后台线程）。已通过且仍有效的步骤会跳过。
    void start();
    // 步骤 3 的一键确认：用户已开启 MSAA 并允许 VRChat 使用 VRSS。
    void confirm_msaa();
    void request_launch_vrchat();
    // 写 NVIDIA VRChat profile 的抗锯齿范围（会弹 UAC；不影响 provider 生命周期）。
    void apply_area(float percent);
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

    mutable std::mutex mutex_;
    WorkflowSnapshot snap_;
    Runtime runtime_;
    std::thread worker_;
    std::thread stopper_;
    std::thread area_worker_;
    std::atomic<bool> busy_{false};
    std::atomic<bool> cancel_{false};
    bool enabled_          = false;
    int port_              = 5556;
    bool auto_launch_      = false;
    float area_percent_    = 12.0f;
    bool msaa_confirmed_   = false;
    bool vrchat_requested_ = false;
};

} // namespace fgb::vrss
