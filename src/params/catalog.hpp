// 参数目录（catalog）——把"协议层形状"翻译成"avatar 参数"的统一入口。
//
// 一共有三代参数，名字空间互不重叠：
//
//   v2 基础    FT/v2/<88 个肌肉形状名>      原样透传，不做换算
//   v2 派生    FT/v2/<合成名>               由形状 + 眼球状态按 VRCFT 公式算出
//   v1 legacy  <旧名>                       无前缀，2022–2023 年老模型绑的就是这些
//
// v1 是 VRCFT 的兼容包袱：SRanipal 的 37 个唇形 + 91 条合成规则 + 眼追的
// Expanded/ExpandedSqueeze 一族。它们本该被 v2 取代，但生态里仍有大量老模型
// 只认 v1，所以必须保留——只是默认不发送（数量太大，见 README）。
//
// 【为什么要 kind】
// VRCFT 的 EParam 对同一个名字至少生成 bool 与 float 两个参数，地址完全相同，
// 靠"avatar 声明了哪个类型"来决定发哪个。本项目的 catalog 只描述值，
// 由 sink 按发现到的类型选择；没有发现信息时用 Float（绝大多数情况）。
#pragma once

#include "core/model/frame.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace fgb::params {

struct EvalContext {
    const FaceFrame& frame;
    float eye_full_scale_deg = 30.0f;
};

enum class ParamKind : std::uint8_t {
    Float,
    Bool,
};

struct ParamEntry {
    std::string_view name; // 不含地址前缀
    ParamKind kind;
    float (*eval)(const EvalContext&) noexcept;
};

// v2 基础形状：88 条，值直接取自 frame.shapes
std::span<const ParamEntry> v2_base() noexcept;

// v2 派生参数（含眼追 / 瞳孔 / 简化 / 合成）
std::span<const ParamEntry> v2_composite() noexcept;

// v1 legacy：眼追 + 唇形（37 SRanipal + 91 合成）
std::span<const ParamEntry> v1_all() noexcept;

// 一组参数。
//
// name_prefix 参与"和头像声明匹配"：v2 参数的真实逻辑名是 `v2/JawOpen`，而不是
// `JawOpen`——这一点必须体现在匹配里，否则 v1 的 JawOpen 会抢走 FT/v2/JawOpen。
// 换句话说，VRCFT 的 v2 名字里那个 "v2/" 是名字的一部分，不是目录前缀。
//
// fallback_prefix 只在拿不到头像参数表时用来拼兜底地址。
struct Group {
    std::string_view name_prefix;
    std::string_view fallback_prefix;
    std::span<const ParamEntry> entries;
};

// 顺序固定：v2 基础 → v2 派生 → v1。sink 与地址路由都按它遍历。
std::span<const Group> groups() noexcept;

// 上限，供 sink 分配去重数组。v2(168) + v1(168) 再留余量。
inline constexpr std::size_t kMaxParamCount = 512;

} // namespace fgb::params
