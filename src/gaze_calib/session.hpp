// 圆盘标定的会话状态机（ADR-0042 §5）。
//
// 谁驱动它：控制面（`ipc->pump`）每帧调一次 `tick`，把最新注视传进来。
// 为什么不起线程：`tick` 要读引擎注视、要动 overlay、还要在结束时热加载档案，
// 三件事都在控制面线程上，起线程只会多出一份需要加锁的共享状态。
//
// 关键约定：**开始前先把当前档案清空**。否则采到的是"已校准后"的值，
// 再拟合一次就是二次校准，残差看着很小但实际是错的。
#pragma once

#include "core/model/gaze.hpp"
#include "core/model/gaze_profile.hpp"
#include "core/model/gaze_projection.hpp"
#include "gaze_calib/collector.hpp"

#include <cstdint>
#include <functional>
#include <string>

namespace fgb::overlay {
class Dots;
}

namespace fgb::gaze_calib {

enum class State : std::uint8_t {
    idle = 0,
    running,
    done,
    failed,
};

const char* state_name(State state) noexcept;

struct Status {
    State state          = State::idle;
    std::size_t target   = 0; // 当前目标（0..12）
    std::size_t done     = 0; // 已完成的目标数
    std::size_t samples  = 0; // 当前目标已采样本
    std::size_t needed   = 0; // 当前目标还差多少（0 = 够了）
    float progress       = 0.0f;
    std::uint32_t profile_version = 0;
    float residual       = -1.0f;
    std::string message; // 一行中文状态，UI 直接显示
};

class Session {
public:
    using ApplyFn = std::function<void(const GazeProfile&)>;
    // 采集时生效的角度粗调。拟合结果建在它之上，所以必须一并记进档案，
    // 否则"标定完又调了增益"这件事在运行时无从发现。
    using TrimFn = std::function<EyeAngleTrim()>;

    void configure(std::string profile_path, std::string hmd_identity);
    void set_projection(const GazeProjection& projection) noexcept { projection_ = projection; }
    void set_overlay(overlay::Dots* dots) noexcept { dots_ = dots; }
    void set_apply(ApplyFn fn) { apply_ = std::move(fn); }
    void set_trim_source(TrimFn fn) { trim_source_ = std::move(fn); }

    // 开始标定：清空档案、打开 overlay、摆第一个点。失败时 error 说明原因。
    bool start(std::string& error);
    // 中止：恢复开始前的档案、隐藏 overlay。
    void cancel() noexcept;

    // 引擎/控制面每帧调用。now_ns 用引擎时钟。
    void tick(std::uint64_t now_ns, const GazeState& gaze) noexcept;

    Status status() const noexcept { return status_; }
    bool active() const noexcept { return status_.state == State::running; }

private:
    void begin_target(std::uint64_t now_ns) noexcept;
    void advance_target(std::uint64_t now_ns) noexcept;
    void finish(std::uint64_t now_ns) noexcept;
    void fail(std::string message) noexcept;
    void place_dot() noexcept;

    std::string profile_path_;
    std::string hmd_identity_;
    GazeProjection projection_{};
    overlay::Dots* dots_ = nullptr;
    ApplyFn apply_;
    TrimFn trim_source_;

    Collector collector_{};
    GazeProfile previous_{}; // 开始前的档案，用于回滚
    GazeProfile result_{};
    GazeSourceKind branch_ = GazeSourceKind::none;
    EyeAngleTrim trim_at_start_{};   // 开始标定那一刻生效的角度粗调
    bool have_trim_at_start_ = false;

    std::uint64_t target_started_ns_ = 0;
    Status status_{};
    bool branch_decided_ = false;
};

} // namespace fgb::gaze_calib
