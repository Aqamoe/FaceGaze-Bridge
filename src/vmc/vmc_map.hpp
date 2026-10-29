// VMC 的 blendshape 名字 ↔ 协议层肌肉形状 的映射表。
//
// 放在独立的小库里，是因为它同时被两个方向用：
//   sources/vmc_source  输入：名字 → 形状（一个名字可能推多个形状）
//   sinks/vmc_sink      输出：形状 → 名字（一个名字取它映射到的形状的最大值）
//
// 为什么名字表不能各自维护一份：两边一旦不同步，就会出现"能收不能发"或者
// "发出去的名字对方不认"这种只在联调时才发现的错，而且看起来像对方的问题。
#pragma once

#include "core/model/frame.hpp"

#include <span>
#include <string_view>

namespace fgb::vmc {

struct BlendEntry {
    const char* name; // ARKit / PerfectSync 常见拼写
    Shape shape;
    float scale; // 双向形状用 +1 / -1 两条
};

// ARKit 的 52 个 blendshape 与协议层 88 个肌肉形状不是一一对应：
// 有的是单侧（mouthSmileLeft → MouthCornerPullLeft），有的是双侧统一
// （browInnerUp、cheekPuff），所以要允许一个输入名映射到多个形状。
std::span<const BlendEntry> blend_entries() noexcept;

// 一个输入名可能映射到多个肌肉形状，所以不能"找到第一条就返回"。
void apply_blend(FaceFrame& frame, std::string_view name, float value) noexcept;

} // namespace fgb::vmc
