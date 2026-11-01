// 注视管线第一段：统一**结构**与有效性，不做单位换算（ADR-0042 §1）。
//
// 为什么分成三段而不是一个大函数：
//   规范化回答"这条数据是什么、还有效吗"；标定回答"它在屏幕哪儿"；稳定化回答
//   "要不要平滑/算不算注视"。混在一起会出现"平滑参数用什么单位"这种无解的问题。
//
// 这一段的产出必须与来源无关：两条分支（模块 2D 点 / 面捕角度+方向）出去之后，
// 下游只看 GazeState 的字段，不再自己判断"我这个源有没有方向"。
#pragma once

#include "core/model/gaze.hpp"

#include <cstdint>

namespace fgb {

class GazeNormalizer {
public:
    // 与 VRSS provider 的判定一致：超过 150 ms 驱动就丢弃，所以 50 ms 以上不该再
    // 被当成"当前注视"。留 50 ms 是为了让合并/滤波不至于把轻微抖动放大成掉帧。
    static constexpr std::uint64_t kStaleLimitNs = 50'000'000;

    // 就地整理：置空间标签、丢过期数据、单眼点时镜像到另一眼。
    // 纯函数（不存状态），便于回放与单测。
    void apply(GazeState& gaze, std::uint64_t now_ns) const noexcept;
};

} // namespace fgb
