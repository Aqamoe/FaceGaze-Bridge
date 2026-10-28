// 逐形状校准。
//
// 【为什么需要】
// 每个人的脸不一样，同一个模块输出的"嘴张到最大"可能是 0.6，也可能是 0.95。
// 不做映射，表情要么永远张不开，要么一直像在喊叫。VRCFT 把这件事交给模块自己的
// 配置界面（比如 PICO 模块的 config.ini 里有 jaw floor/gain），但那是模块私有格式，
// 换个模块就得重来。放在核心层做，一次校准对所有模块生效。
//
// 【怎么算】
// 两份抓包：一份放松（neutral），一份尽量夸张（express）。
//   lo = neutral 的中位数      —— 用中位数不是最小值，抗单帧抖动
//   hi = express 的 95 分位     —— 同理，躲开偶发尖峰
// 映射：out = clamp((raw - lo) / (hi - lo), 0, 1)
//
// 【为什么用抓包而不是实时向导】
// 实时向导要在引擎循环里插状态机、还要在 CLI 里做倒计时提示，代码量和出错面都大。
// 抓包已经是现成的（ADR-0007），推导过程还能反复重跑、单测覆盖。用户多敲一条命令，
// 换来的是一条完全确定的离线流程。
#pragma once

#include "core/model/frame.hpp"
#include "core/process/face_processor.hpp"

#include <array>
#include <string>

namespace fgb::params {

struct ShapeRange {
    float lo     = 0.0f;
    float hi     = 1.0f;
    bool active  = false; // false 表示这个形状不参与校准（原样透传）
};

struct CalibrationProfile {
    std::array<ShapeRange, kShapeCount> shapes{};
    // 视线：以中性时的角度为中心，乘以增益。正常人是 1.0，眼动幅度小的可以调大
    float eye_gain_x    = 1.0f;
    float eye_gain_y    = 1.0f;
    float eye_center_x  = 0.0f;
    float eye_center_y  = 0.0f;
    bool has_eye        = false;

    std::size_t active_shape_count() const noexcept;
};

bool save_profile(const std::string& path, const CalibrationProfile& profile);
bool load_profile(const std::string& path, CalibrationProfile& profile);

// 从两份抓包推导。失败时 error 非空。
bool derive_profile(const std::string& neutral_capture, const std::string& express_capture,
                    CalibrationProfile& out, std::string& error);

// 把校准应用到帧上。没有 active 的形状原样透传。
class CalibrationApplier final : public IFaceProcessor {
public:
    explicit CalibrationApplier(CalibrationProfile profile = {});

    const char* name() const noexcept override { return "calibration"; }
    void process(FaceFrame& frame) noexcept override;

    void set_profile(const CalibrationProfile& profile) noexcept { profile_ = profile; }
    const CalibrationProfile& profile() const noexcept { return profile_; }

private:
    CalibrationProfile profile_{};
};

} // namespace fgb::params
