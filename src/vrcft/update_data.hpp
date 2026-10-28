// ReplyUpdate 的数据结构、解码与到 FaceFrame 的转换。
//
// 结构体布局直接对应 VRCFaceTracking 的 UpdateDataContiguous（420 字节）：
//   16 个 float（眼球 + 头部）+ 89 个 float（Expression_Shapes）
//
// INVALID_FLOAT 的坑：C# 里写的是 `const float INVALID_FLOAT = 0xFFFFFFFF;`，
// 走的是 int -> float 隐式转换，实际位模式是 0x4F800000（4294967296.0f），
// **不是 NaN**。所以判空必须比位模式，用 std::isnan 会全部漏过。
#pragma once

#include "core/model/frame.hpp"

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <span>

namespace fgb::vrcft {

// UnifiedExpressions.Max = 88，数组长度 = Max + 1
inline constexpr std::size_t kExpressionCount  = 89;
inline constexpr std::size_t kUpdateStructSize = (16 + kExpressionCount) * 4; // 420

inline bool is_invalid(float v) noexcept {
    return std::bit_cast<std::uint32_t>(v) == 0x4F800000u;
}

struct UpdateData {
    float max_dilation    = 0.0f;
    float min_dilation    = 0.0f;
    float left_gaze_x     = 0.0f;
    float left_gaze_y     = 0.0f;
    float left_pupil_mm   = 0.0f;
    float left_openness   = 0.0f;
    float right_gaze_x    = 0.0f;
    float right_gaze_y    = 0.0f;
    float right_pupil_mm  = 0.0f;
    float right_openness  = 0.0f;
    float head_yaw        = 0.0f;
    float head_pitch      = 0.0f;
    float head_roll       = 0.0f;
    float head_pos_x      = 0.0f;
    float head_pos_y      = 0.0f;
    float head_pos_z      = 0.0f;
    std::array<float, kExpressionCount> shapes{};

    // 是否至少有一个有效字段（用于判断"模块连上了但硬件没数据"）
    bool has_any_valid() const noexcept;

    bool has_eye_data() const noexcept;
    bool has_expression_data() const noexcept;
};

// body 为包体（magic 之后的部分），要求以 structSize(int32) 开头。
bool decode_update(std::span<const std::byte> body, UpdateData& out) noexcept;

struct ConvertOptions {
    // Gaze 归一化值 ±1 对应的角度。30° 是常见眼动满量程。
    float eye_full_scale_deg = 30.0f;
};

// 把 UpdateData 写进 FaceFrame。无效字段保持原值不动（便于多源融合）。
// 返回是否写入了任何眼睛数据。
bool apply_to_frame(const UpdateData& data, const ConvertOptions& options, FaceFrame& frame) noexcept;

} // namespace fgb::vrcft
