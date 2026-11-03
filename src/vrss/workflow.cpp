#include "vrss/workflow.hpp"

#include "base/log.hpp"
#include "vrss/paths.hpp"

#include <algorithm>
#include <chrono>
#include <exception>

namespace fgb::vrss {
namespace {

StepStatus* find_step(WorkflowSnapshot& snapshot, StepId id) {
    const std::size_t index = static_cast<std::size_t>(id);
    return index < snapshot.steps.size() ? &snapshot.steps[index] : nullptr;
}

const StepStatus* find_step(const WorkflowSnapshot& snapshot, StepId id) {
    const std::size_t index = static_cast<std::size_t>(id);
    return index < snapshot.steps.size() ? &snapshot.steps[index] : nullptr;
}

bool is_done(StepState state) {
    return state == StepState::Pass || state == StepState::Skipped;
}

} // namespace

Workflow::Workflow() {
    for (std::size_t i = 0; i < snap_.steps.size(); ++i) {
        snap_.steps[i].id = static_cast<StepId>(i);
    }
}

Workflow::~Workflow() {
    cancel_ = true;
    join_worker();
    if (stopper_.joinable()) {
        stopper_.join();
    }
    if (area_worker_.joinable()) {
        area_worker_.join();
    }
    if (identity_worker_.joinable()) {
        identity_worker_.join();
    }
}

void Workflow::join_worker() {
    if (worker_.joinable()) {
        worker_.join();
    }
}

void Workflow::configure(bool enabled, int port, bool auto_launch_vrchat, float area_percent,
                         bool auto_start, bool persist_identity) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        enabled_          = enabled;
        port_             = port;
        auto_launch_      = auto_launch_vrchat;
        area_percent_     = area_percent;
        auto_start_       = auto_start;
        persist_identity_ = persist_identity;
        snap_.enabled     = enabled;
        snap_.auto_start  = auto_start;
        snap_.persist_identity = persist_identity;
        if (!enabled) {
            snap_.state  = "off";
            auto_session_ = false; // 关掉再打开时允许重新自动注册
        } else if (snap_.state == "off") {
            snap_.state = "blocked";
        }
    }
    if (!enabled) {
        cancel_ = true;
    }
}

void Workflow::set_step(StepId id, StepState state, const std::string& detail) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (StepStatus* step = find_step(snap_, id)) {
        step->state  = state;
        step->detail = detail;
    }
}

StepState Workflow::step_state(StepId id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const StepStatus* step = find_step(snap_, id);
    return step ? step->state : StepState::Pending;
}

bool Workflow::step_passed(StepId id) const { return is_done(step_state(id)); }

void Workflow::start() {
    if (!enabled()) {
        return;
    }
    if (busy_.exchange(true)) {
        return;
    }
    join_worker();
    cancel_ = false;
    worker_ = std::thread([this] {
        try {
            run_steps();
        } catch (const std::exception& exception) {
            std::lock_guard<std::mutex> lock(mutex_);
            snap_.state      = "failed";
            snap_.last_error = exception.what();
        } catch (...) {
            std::lock_guard<std::mutex> lock(mutex_);
            snap_.state      = "failed";
            snap_.last_error = "VRSS 流程异常终止";
        }
        busy_ = false;
    });
}

void Workflow::confirm_msaa() {
    msaa_confirmed_ = true;
    start();
}

void Workflow::request_launch_vrchat() {
    vrchat_requested_ = true;
    start();
}

void Workflow::apply_area(float percent) {
    if (area_worker_.joinable()) {
        area_worker_.join();
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        area_percent_      = percent;
        snap_.area_state   = "applying";
        snap_.area_detail  = "正在请求管理员并写入 NVIDIA VRChat profile";
    }
    area_worker_ = std::thread([this, percent] {
        std::string error;
        const bool ok = runtime_.set_area(percent, error);
        std::lock_guard<std::mutex> lock(mutex_);
        snap_.area_state  = ok ? "applied" : "failed";
        snap_.area_detail = ok ? "范围已写入；重启 VRChat 后生效"
                               : ("写入失败：" + error);
    });
}

void Workflow::clear_identity() {
    if (identity_worker_.joinable()) {
        identity_worker_.join();
    }
    identity_worker_ = std::thread([this] {
        std::string error;
        const bool ok = clear_persistent_identity(error);
        const PersistentIdentity state = persistent_identity();
        std::lock_guard<std::mutex> lock(mutex_);
        snap_.identity  = state;
        identity_ready_ = false;
        if (!ok) {
            snap_.last_error = "清除持久身份失败：" + error;
        }
    });
}

void Workflow::cancel() {
    cancel_ = true;
}

void Workflow::stop() {
    cancel_ = true;
    // 用户显式停止：本轮 SteamVR 会话内不要再自动拉起，否则"停止"形同虚设。
    auto_session_ = true;
    if (stopper_.joinable()) {
        stopper_.join();
    }
    stopper_ = std::thread([this] {
        std::string error;
        runtime_.stop_provider(error);
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& step : snap_.steps) {
            step.state  = StepState::Pending;
            step.detail.clear();
        }
        snap_.provider = ProviderStatus{};
        snap_.state    = "stopped";
        snap_.last_error = error;
    });
}

void Workflow::finish_state() {
    std::lock_guard<std::mutex> lock(mutex_);
    bool any_fail    = false;
    bool any_blocked = false;
    bool all_done    = true;
    for (const auto& step : snap_.steps) {
        if (step.state == StepState::Fail) {
            any_fail = true;
            if (snap_.last_error.empty()) {
                snap_.last_error = step.detail;
            }
        }
        if (step.state == StepState::Blocked || step.state == StepState::Running ||
            step.state == StepState::Pending) {
            all_done = false;
            if (step.state == StepState::Blocked) {
                any_blocked = true;
            }
        }
    }
    snap_.state = any_fail ? "failed" : any_blocked ? "blocked" : all_done ? "running" : "blocked";
}

WorkflowSnapshot Workflow::snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return snap_;
}

void Workflow::refresh() {
    runtime_.write_lease();
    ProviderStatus status;
    runtime_.read_status(status);
    const bool vrchat  = runtime_.vrchat_running();
    const bool steamvr = runtime_.steamvr_running();
    const PersistentIdentity identity = persistent_identity();

    bool trigger_auto   = false;
    bool verify_changed = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        snap_.provider       = status;
        snap_.vrchat_running = vrchat;
        snap_.steamvr        = steamvr;
        snap_.identity       = identity;
        // 用户手动删掉/改了变量时，"持久身份已就绪"必须跟着失效，
        // 否则下一步会误判"可以直接从 Steam 启动游戏"。
        if (identity_ready_ && (!identity.set || !identity.ours)) {
            identity_ready_ = false;
        }
        // 必须确认 provider 真的在跑：软件关掉后 status.json 会停在最后一帧，
        // 里面的 consumerCount=1 会让页面误报"VRChat 已连接、正在提交"。
        if (StepStatus* verify = find_step(snap_, StepId::Verify);
            verify && verify->state != StepState::Pass && status.running &&
            status.consumer_count > 0) {
            if (status.fresh) {
                verify->state  = StepState::Pass;
                verify->detail = "VRChat 已连接，注视数据正在提交";
            } else if (status.sent == 0) {
                // consumer 有、但一帧都没提交过：驱动只会把区域留在上一次/默认位置，
                // 头显里看就是"固定在一个角上不跟随"。这里必须报失败，不能报通过。
                verify->state  = StepState::Fail;
                verify->detail = "VRChat 已连接，但 provider 一帧注视都没收到（FGB 没有发送）";
            } else {
                verify->state  = StepState::Blocked;
                verify->detail = "VRChat 已连接，等待新的注视数据";
            }
            verify_changed = true;
        }
        if (snap_.state == "running" && !status.running) {
            // 只有 provider 步骤已经通过、随后进程消失，才算"provider 退出"。
            // 流程刚起步时 provider 本来就没运行，refresh 不能把它误判成失败。
            StepStatus* provider = find_step(snap_, StepId::Provider);
            if (provider != nullptr && provider->state == StepState::Pass) {
                snap_.state      = "failed";
                snap_.last_error = "provider 已退出";
                provider->state  = StepState::Fail;
                provider->detail = "provider 已退出";
            }
        }
        // 用户按提示正常关掉 VRChat 之后自动继续，不必再点一次「一键启用」。
        if (auto_start_ && !busy_ && !vrchat) {
            if (StepStatus* provider = find_step(snap_, StepId::Provider);
                provider && provider->state == StepState::Blocked &&
                provider->detail.rfind("VRChat 仍持有", 0) == 0) {
                provider->state = StepState::Pending;
                provider->detail.clear();
                auto_session_ = false; // 允许下一拍重新自动跑
            }
        }
        // 跟随 SteamVR 自动注册：SteamVR 不在时清会话标记，下次出现可以再自动跑一次。
        // 用户取消 UAC 后本轮不再重试，避免每 2 秒弹一次。
        // 即使 provider 已经注册也跑一轮：FGB 重启后环境变量可能被清掉，
        // 流程本身是幂等的（provider 已就绪时沿用几何、不重复 UAC）。
        if (!steamvr) {
            auto_session_ = false;
        } else if (auto_start_ && enabled_ && !auto_session_ && !busy_) {
            auto_session_ = true;
            trigger_auto  = true;
        }
    }
    if (trigger_auto) {
        FGB_INFO("vrss", "检测到 SteamVR，自动启动 VRSS 注册流程");
        start();
    }
    if (verify_changed) {
        // 验证结果变了要重算整体状态，否则消费方连上后页面仍停在 blocked。
        finish_state();
    }
}

void Workflow::run_steps() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        snap_.last_error.clear();
        snap_.state = "running";
    }

    ProviderStatus status;
    runtime_.read_status(status);
    const bool provider_ready = status.running && status.registered;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        snap_.provider       = status;
        snap_.vrchat_running = runtime_.vrchat_running();
        snap_.admin          = runtime_.admin_enabled();
    }

    if (cancel_) {
        return;
    }

    // 1. 环境自检：SteamVR / HMD / 几何 / 身份。provider 已在跑时不再重导。
    if (!step_passed(StepId::Env)) {
        set_step(StepId::Env, StepState::Running, "检查 SteamVR 与头显几何");
        HmdGeometry geometry;
        std::string detail;
        bool have_geometry = false;
        if (provider_ready) {
            std::string parse_error;
            have_geometry = load_geometry(run_dir() / "geometry.txt", geometry, parse_error);
            detail        = "provider 已运行，沿用现有几何";
        } else {
            std::string error;
            if (!runtime_.export_geometry(geometry, error)) {
                set_step(StepId::Env, StepState::Fail, error);
                finish_state();
                return;
            }
            have_geometry = true;
            detail        = "身份 " + geometry.identity_source + " / " +
                     std::to_string(geometry.vendor) + ":" + std::to_string(geometry.product);
        }

        std::string identity_detail;
        if (have_geometry) {
            const bool override_identity = needs_identity_override(geometry.identity_source);
            {
                std::lock_guard<std::mutex> lock(mutex_);
                snap_.identity_source = geometry.identity_source;
            }
            if (!persist_identity_) {
                // 关掉开关要顺手把自己写过的变量清掉：否则游戏还会上报假身份，
                // provider 那边却按用户预期不再管，结果是"看起来能连、其实对不上"。
                std::string clear_error;
                clear_persistent_identity(clear_error);
                identity_ready_ = false;
                identity_detail = override_identity
                                      ? "；持久身份已关闭，需从本页启动 VRChat"
                                      : "；头显自带 EDID";
            } else if (override_identity) {
                std::string error;
                if (!apply_persistent_identity(geometry.vendor, geometry.product, error)) {
                    set_step(StepId::Env, StepState::Fail, "写入持久 HMD 身份失败：" + error);
                    finish_state();
                    return;
                }
                const PersistentIdentity state = persistent_identity();
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    snap_.identity = state;
                }
                identity_ready_ = state.set && state.vendor == geometry.vendor &&
                                  state.product == geometry.product;
                identity_detail = "；持久身份 " + identity_env_text(geometry.vendor) + ":" +
                                  identity_env_text(geometry.product);
                if (state.steam_restart_required) {
                    identity_detail += "（需重启 Steam 生效）";
                }
            } else {
                // 真 EDID 头显：清掉以前为无 EDID 设备写下的变量，否则游戏上报的假身份
                // 会和 provider 的真身份对不上。
                std::string clear_error;
                if (!clear_persistent_identity(clear_error)) {
                    FGB_WARN("vrss", "清除持久身份失败：%s", clear_error.c_str());
                }
                identity_ready_ = false;
                identity_detail = "；头显自带 EDID（未使用持久身份）";
            }
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            snap_.steamvr  = true;
            snap_.geometry = have_geometry;
        }
        set_step(StepId::Env, StepState::Pass, detail + identity_detail);
    }
    if (cancel_) {
        return;
    }

    // 2. 驱动检查：白名单直接通过；否则自动只读探测并缓存合同。
    if (!step_passed(StepId::Driver)) {
        set_step(StepId::Driver, StepState::Running, "检查 NVIDIA 驱动合同");
        if (provider_ready) {
            set_step(StepId::Driver, StepState::Pass, "沿用已注册的 provider");
            set_step(StepId::DriverAudit, StepState::Skipped, "provider 已注册");
        } else {
            DriverInfo info;
            std::string error;
            if (!runtime_.check_driver(info, error)) {
                set_step(StepId::Driver, StepState::Fail, error);
                set_step(StepId::DriverAudit, StepState::Fail, info.message);
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    snap_.driver = info;
                }
                finish_state();
                return;
            }
            {
                std::lock_guard<std::mutex> lock(mutex_);
                snap_.driver = info;
            }
            set_step(StepId::Driver, StepState::Pass,
                     (info.experimental ? "实验合同 " : "白名单 ") + info.version);
            set_step(StepId::DriverAudit,
                     info.experimental ? StepState::Pass : StepState::Skipped,
                     info.experimental ? "只读探测通过；需真机四方向确认" : "未使用实验合同");
        }
    }
    if (cancel_) {
        return;
    }

    // 3. 权限与互斥：MSAA/VRSS allow 由用户一键确认。
    if (!step_passed(StepId::Admin)) {
        set_step(StepId::Admin, StepState::Running, "检查端口与管理员权限");
        // 自动模式不能停下来等人点确认，否则"跟随 SteamVR 自动注册"永远走不到下一步。
        // MSAA 仍由验证步骤兜底：没开 x4 时 consumer 一直是 0，页面会明确要求去开。
        if (!msaa_confirmed_ && !auto_start_) {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                snap_.admin = runtime_.admin_enabled();
            }
            set_step(StepId::Admin, StepState::Blocked,
                     "请确认：VRChat 已开 MSAA x4，NVIDIA 配置允许 VRChat 使用 VRSS");
            finish_state();
            return;
        }
        if (!status.running && !runtime_.port_available(port_)) {
            set_step(StepId::Admin, StepState::Fail,
                     "端口 " + std::to_string(port_) + " 被占用（可能有另一个 VRSS 提供程序）");
            finish_state();
            return;
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            snap_.admin = runtime_.admin_enabled();
        }
        set_step(StepId::Admin, StepState::Pass,
                 msaa_confirmed_ ? "启动时将请求管理员权限"
                                 : "自动模式：跳过 MSAA 确认（请保持 VRChat 内 MSAA x4）");
    }
    if (cancel_) {
        return;
    }

    // 4. 启动注视路径。
    if (!step_passed(StepId::Provider)) {
        set_step(StepId::Provider, StepState::Running, "启动 provider（会弹出 UAC）");
        if (!provider_ready) {
            // 上一轮 VRChat 还持有连接时，NVIDIA 会拒绝重注册（-175）。先拦下来：
            // 省掉一次没意义的 UAC，并明确告诉用户下一步该做什么。
            if (!status.running && status.consumer_count > 0 && runtime_.vrchat_running()) {
                set_step(StepId::Provider, StepState::Blocked,
                         auto_start_ ? "VRChat 仍持有上一轮 VRSS 连接：请先正常关闭 VRChat，"
                                       "关闭后会自动继续"
                                     : "VRChat 仍持有上一轮 VRSS 连接：请先正常关闭 VRChat，"
                                       "然后点「一键启用」重试");
                finish_state();
                return;
            }
            std::string error;
            if (!runtime_.start_provider(port_, runtime_.contract(), area_percent_, error)) {
                set_step(StepId::Provider, StepState::Fail, error);
                finish_state();
                return;
            }
        }
        ProviderStatus started;
        bool registered = false;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(6);
        while (std::chrono::steady_clock::now() < deadline) {
            runtime_.read_status(started);
            {
                std::lock_guard<std::mutex> lock(mutex_);
                snap_.provider = started;
            }
            if (started.registered) {
                registered = true;
                break;
            }
            std::chrono::milliseconds(100);
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            if (cancel_) {
                return;
            }
        }
        if (!registered) {
            set_step(StepId::Provider, StepState::Fail,
                     runtime_.last_error_text().empty() ? "provider 未完成注册"
                                                        : runtime_.last_error_text());
            finish_state();
            return;
        }
        set_step(StepId::Provider, StepState::Pass,
                 "pid=" + std::to_string(started.pid) + " 已注册");
    }
    if (cancel_) {
        return;
    }

    // 5. 启动 VRChat。
    if (!step_passed(StepId::Vrchat)) {
        set_step(StepId::Vrchat, StepState::Running, "检查 VRChat 进程");
        if (runtime_.vrchat_running()) {
            set_step(StepId::Vrchat, StepState::Pass,
                     "VRChat 已运行；若刚改范围，需重启 VRChat 才生效");
        } else if (identity_ready_) {
            // 持久身份已经把 provider 的键写进了用户环境变量，Steam 正常启动的游戏
            // 也能对上，这里不再需要代起游戏，直接进入"等它连上来"。
            set_step(StepId::Vrchat, StepState::Pass,
                     "持久身份已生效：可从 Steam 直接启动 VRChat（本页不再代启）");
        } else if (vrchat_requested_ || auto_launch_) {
            std::string error;
            if (!runtime_.launch_vrchat(error)) {
                set_step(StepId::Vrchat, StepState::Fail, error);
                finish_state();
                return;
            }
            bool appeared = false;
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
            while (std::chrono::steady_clock::now() < deadline) {
                if (runtime_.vrchat_running()) {
                    appeared = true;
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
            }
            if (!appeared) {
                set_step(StepId::Vrchat, StepState::Fail, "VRChat 未在 10 秒内启动");
                finish_state();
                return;
            }
            set_step(StepId::Vrchat, StepState::Pass,
                     "已用 HMD 身份注入启动 VRChat（无 EDID 头显必需）");
        } else {
            set_step(StepId::Vrchat, StepState::Blocked, "请启动 VRChat（或打开自动启动开关）");
            finish_state();
            return;
        }
    }
    if (cancel_) {
        return;
    }

    // 6. 验证：VRChat 成为 consumer，并且注视包真的在往里送。
    set_step(StepId::Verify, StepState::Running, "等待 VRChat 连接并提交注视");
    ProviderStatus live;
    bool consumer_seen = false;
    bool submitting    = false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
    while (std::chrono::steady_clock::now() < deadline) {
        runtime_.read_status(live);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            snap_.provider = live;
        }
        if (live.consumer_count > 0) {
            consumer_seen = true;
        }
        // consumer 只是"有人接了"，fresh 才是"真的收到包"。
        if (live.consumer_count > 0 && live.fresh) {
            submitting = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        if (cancel_) {
            return;
        }
    }
    if (submitting) {
        set_step(StepId::Verify, StepState::Pass, "VRChat 已连接，注视数据正在提交");
    } else if (consumer_seen) {
        set_step(StepId::Verify, StepState::Fail,
                 live.sent == 0
                     ? "VRChat 已连接，但 provider 一帧注视都没收到（FGB 没有发送）"
                     : "VRChat 已连接，但注视包已过期（FGB 停止发送）");
    } else {
        set_step(StepId::Verify, StepState::Blocked,
                 live.running ? "等待 VRChat 连接（provider 已注册）；若持续为 0，重启 VRChat 再试"
                              : "provider 未运行");
    }
    finish_state();
}

} // namespace fgb::vrss
