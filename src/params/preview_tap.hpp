// 校准预览取样：把"校准之前"的原始值存一份给界面看。
//
// 【为什么单独一个 processor】
// /api/params 与 10 Hz 的 params 推送给的是校准 + 滤波之后的值。校准页要回答的是
// "我这条 lo/hi 画在原始尺度上的哪里"，没有原始值就只能靠用户猜——所以必须在
// CalibrationApplier 之前取一份快照，而且不能影响校准本身的输出。
//
// 【为什么默认关闭】
// 引擎热路径是 120 Hz、零分配。快照是 88 个 float 的整块拷贝（约 400 字节），
// 单次代价很小，但"没人看的时候也拷"没有道理。校准页打开时置位，关了立即停。
#pragma once

#include "core/cont/seqlock.hpp"
#include "core/model/frame.hpp"
#include "core/process/face_processor.hpp"

#include <array>
#include <atomic>
#include <cstdint>

namespace fgb::params {

struct PreviewFrame {
    std::uint64_t sequence = 0;
    bool valid             = false; // 取样器还没收到过帧时为 false
    std::array<float, kShapeCount> shapes{};

    // 眼睛在快照里保持"原始"语义：校准会改中心与增益，界面要看到改之前的角度
    bool eyes_valid   = false;
    float openness_l  = 1.0f;
    float openness_r  = 1.0f;
    float gaze_x_deg  = 0.0f; // yaw，左右
    float gaze_y_deg  = 0.0f; // pitch，上下
};

class PreviewTap final : public IFaceProcessor {
public:
    const char* name() const noexcept override { return "preview"; }
    void process(FaceFrame& frame) noexcept override;

    void set_enabled(bool enabled) noexcept { enabled_.store(enabled, std::memory_order_relaxed); }
    bool enabled() const noexcept { return enabled_.load(std::memory_order_relaxed); }

    // 读快照。返回 false 表示竞争过于激烈或从未取样，调用方应沿用自己的上一帧。
    bool latest(PreviewFrame& out) const noexcept { return snapshot_.read(out); }

private:
    std::atomic<bool> enabled_{false};
    SeqLock<PreviewFrame> snapshot_{};
};

} // namespace fgb::params
