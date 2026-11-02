// VRChat 原生面捕（Avatars SDK 3.10.6 / 客户端 2026.4.1 起）的输入格式。
//
// 只有一条消息：`/tracking/face/v1`，单个 OSC blob（类型标签 `b`），负载**固定 102 字节**，
// 每个 Unified Expressions tracking key 占一字节（0..255，VRChat 按 `byte / 255.0` 解码）。
// 长度或参数个数不对，VRChat 会**整条丢弃**，所以每帧都必须整包重发、缺的 key 填 0。
//
// 【顺序是硬契约，不是偏好】
//   0–13   眼：LookOut/In/Up/Down × Right/Left、Closed R/L、Dilation R/L、Constrict R/L
//   14–101 表情：正好是本项目 `core/model/shape.hpp` 的 88 个基础形状，且逐项同序
// 所以这里不需要名字表：`out[14 + i] = shapes[i]`。形状枚举一改协议就跟着变，
// 下面的 static_assert 在编译期把"多一个少一个"挡住。
//
// 注意 `Voice` 与 `Viseme_*` **不在**负载里，由 VRChat 自己提供。
#pragma once

#include "core/model/frame.hpp"
#include "core/model/shape.hpp"

#include <cstddef>
#include <cstdint>
#include <span>

namespace fgb {

inline constexpr char kAddrNativeFace[] = "/tracking/face/v1";
inline constexpr std::size_t kNativeFaceEyeBytes = 14;
inline constexpr std::size_t kNativeFaceBytes    = 102;

static_assert(kNativeFaceEyeBytes + kShapeCount == kNativeFaceBytes,
              "原生面捕负载 = 14 个眼字节 + 88 个形状字节；形状枚举变了必须重新对齐协议");

// 一帧数据 → 102 字节负载。未提供的形状按 0 写（位置不能省）。
void build_native_face_payload(const FaceFrame& frame, float eye_full_scale_deg,
                               std::span<std::uint8_t, kNativeFaceBytes> out) noexcept;

} // namespace fgb
