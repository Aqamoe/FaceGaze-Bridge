#include "params/catalog.hpp"

#include "params/v1_params.hpp"
#include "params/v2_params.hpp"

#include "core/model/shape.hpp"

namespace fgb::params {
namespace {

template <Shape S>
float shape_eval(const EvalContext& c) noexcept {
    return c.frame.shapes[index_of(S)];
}

// clang-format off
#define FGB_CATALOG_ENTRY(name) {#name, ParamKind::Float, &shape_eval<Shape::name>},
constexpr ParamEntry kBase[] = { FGB_SHAPE_LIST(FGB_CATALOG_ENTRY) };
#undef FGB_CATALOG_ENTRY
// clang-format on

static_assert(sizeof(kBase) / sizeof(kBase[0]) == kShapeCount,
              "基础形状目录必须与形状枚举同长");

} // namespace

std::span<const ParamEntry> v2_base() noexcept { return {kBase, kShapeCount}; }

std::span<const ParamEntry> v2_composite() noexcept { return composite_params(); }

std::span<const Group> groups() noexcept {
    // 顺序固定：v2 基础 → v2 派生 → v1。sink 与路由都按这个顺序遍历，
    // 只要两边用同一个函数就不会错位。
    static const Group kGroups[] = {
        {"v2/", "FT/v2/", v2_base()},
        {"v2/", "FT/v2/", v2_composite()},
        {"", "", v1_all()},
    };
    return {kGroups, sizeof(kGroups) / sizeof(kGroups[0])};
}

} // namespace fgb::params
