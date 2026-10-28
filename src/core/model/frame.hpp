// 统一帧：引擎内部与所有 Sink 之间唯一的数据契约。
//
// 相对 VRCFT 的 UnifiedTracking.Data 补了三样东西：时间戳、置信度、来源溯源。
// 这三样是延迟补偿、跨源融合和故障定位的前提，缺了就只能靠猜。
#pragma once

#include "core/model/math.hpp"
#include "core/model/shape.hpp"

#include <array>
#include <cstdint>

namespace fgb {

using SourceId = std::uint32_t;
inline constexpr SourceId kInvalidSource = 0;

// 单眼状态。valid 与数值分离，避免用 0 冒充"不可用"。
struct EyeState {
    bool valid = false;

    float openness     = 1.0f; // 0 = 闭合，1 = 完全睁开
    float opennessConf = 0.0f;

    float pupilDiameterMm = 0.0f; // 0 表示该来源不提供
    float pupilDilation   = 0.0f; // -1..1，0 为基线

    Vec3 gazeDirLocal{0.0f, 0.0f, -1.0f}; // HMD 局部，单位向量
    Vec2 gazePitchYawDeg{0.0f, 0.0f};     // 缓存，避免每帧重复三角函数
    float gazeConf = 0.0f;
};

struct EyePair {
    EyeState left;
    EyeState right;

    bool hasVergence = false;
    float vergenceDistanceM = 0.0f;

    bool any_valid() const noexcept { return left.valid || right.valid; }
};

struct FaceFrame {
    std::uint64_t captureTsNs = 0; // 数据在物理世界发生的时刻
    std::uint64_t publishTsNs = 0; // 引擎发布时刻
    std::uint64_t sequence    = 0;

    EyePair eyes;

    std::array<float, kShapeCount> shapes{};      // 0..1
    std::array<std::uint8_t, kShapeCount> shapeValid{};

    SourceId source = kInvalidSource;
    std::uint32_t contributingSources = 0; // 位掩码
    float qualityScore = 1.0f;

    bool hasHeadPose = false;
    Quat headRotLocal{};
    Vec3 headPosLocal{};

    float shape(Shape s) const noexcept {
        const auto i = index_of(s);
        return i < kShapeCount ? shapes[i] : 0.0f;
    }

    void set_shape(Shape s, float v, bool valid = true) noexcept {
        const auto i = index_of(s);
        if (i < kShapeCount) {
            shapes[i]      = clamp01(v);
            shapeValid[i]  = valid ? 1 : 0;
        }
    }

    bool shape_is_valid(Shape s) const noexcept {
        const auto i = index_of(s);
        return i < kShapeCount && shapeValid[i] != 0;
    }

    // 中性姿态：所有形状归零、眼睛睁开注视正前。
    // 降级时发它，而不是停发——详见架构文档 §7.3。
    static FaceFrame neutral() noexcept;

    // 越界钳制 + 非法值清理（NaN 归零）。每次 publish 前调用。
    void sanitize() noexcept;
};

} // namespace fgb
