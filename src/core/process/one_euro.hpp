// One Euro 滤波。
//
// 全名 "1€ filter"（Casiez 等，CHI 2012）：截止频率随速度自适应——手慢时强滤波去抖，
// 动得快时弱滤波保延迟。表情追踪正好是这种分布：静止时嘴几乎不动，说话时移动很快。
//
// 参数与 VRCFaceTracking 的 Mutation/Filter.cs 保持一致（minCutoff=1.0、beta=0.5、
// dCutoff=0.1、hz=10），这样两边的"手感"是同一个东西，而不是看起来像。
//
// 【关于 hz】
// VRCFT 把采样率写死 10 Hz，而数据实际是约 100 Hz 到达的。这在数学上不严谨，
// 但 beta 的实际强度就建立在这个数值上。跟随上游而不是"修正"它，理由只有一个：
// 目标是替代品的行为一致，不是论文式的正确。要真实 dt 可以改 kUseMeasuredDt。
#pragma once

#include "core/model/frame.hpp"
#include "core/process/face_processor.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace fgb {

struct OneEuroConfig {
    float min_cutoff = 1.0f;
    float beta       = 0.5f;
    float d_cutoff   = 0.1f;
    float hz         = 10.0f; // 见文件头说明
};

// 单个一欧元滤波器
class OneEuroFilter {
public:
    void reset() noexcept { x_prev_ = 0.0f; dx_prev_ = 0.0f; primed_ = false; }
    void set_config(const OneEuroConfig& cfg) noexcept { config_ = cfg; }
    float filter(float x) noexcept;

private:
    static float alpha(float hz, float cutoff) noexcept;
    OneEuroConfig config_{};
    float x_prev_  = 0.0f;
    float dx_prev_ = 0.0f;
    bool primed_   = false;
};

// 表情帧的完整滤波组：88 形状 + 眼球 + 头部
class FaceFilter final : public IFaceProcessor {
public:
    explicit FaceFilter(OneEuroConfig config = {});

    const char* name() const noexcept override { return "filter"; }
    void process(FaceFrame& frame) noexcept override;

    void reset() noexcept;
    // 改参数时不清状态：线上调参不该让画面跳一下
    void set_config(const OneEuroConfig& config) noexcept;

    // 以下三个可被其他线程调用（面板滑杆）。process() 在引擎线程里读，单写单读。
    void set_enabled(bool enabled) noexcept;
    bool enabled() const noexcept { return enabled_.load(std::memory_order_relaxed); }
    void set_live_params(float min_cutoff, float beta) noexcept;
    float min_cutoff() const noexcept { return live_min_cutoff_.load(std::memory_order_relaxed); }
    float beta() const noexcept { return live_beta_.load(std::memory_order_relaxed); }

private:
    OneEuroConfig config_{};
    std::array<OneEuroFilter, kShapeCount> shapes_{};
    OneEuroFilter gaze_left_x_, gaze_left_y_;
    OneEuroFilter gaze_right_x_, gaze_right_y_;
    OneEuroFilter openness_left_, openness_right_;
    OneEuroFilter pupil_left_, pupil_right_;

    std::atomic<bool> enabled_{true};
    std::atomic<float> live_min_cutoff_{1.0f};
    std::atomic<float> live_beta_{0.5f};
};

} // namespace fgb
