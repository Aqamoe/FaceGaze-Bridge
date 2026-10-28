#include "core/model/shape.hpp"

#include "test_framework.hpp"

#include <cstring>
#include <string_view>

using namespace fgb;

FGB_TEST(shape_count_matches_spec) {
    // Unified Expressions v2 基础形状集。这个数字变了必须是有意识的决定，
    // 因为它是线上协议的隐式版本号。
    FGB_CHECK(kShapeCount == 86);
    FGB_CHECK(index_of(Shape::Count) == kShapeCount);
}

FGB_TEST(shape_names_are_nonempty_and_unique) {
    for (std::size_t i = 0; i < kShapeCount; ++i) {
        const std::string_view name = shape_name(static_cast<Shape>(i));
        FGB_CHECK(!name.empty());
        for (std::size_t j = i + 1; j < kShapeCount; ++j) {
            FGB_CHECK(name != shape_name(static_cast<Shape>(j)));
        }
    }
}

FGB_TEST(shape_lookup_by_name) {
    Shape s{};
    FGB_CHECK(shape_from_name("JawOpen", s));
    FGB_CHECK(s == Shape::JawOpen);

    FGB_CHECK(shape_from_name("TongueOut", s));
    FGB_CHECK(s == Shape::TongueOut);

    FGB_CHECK(shape_from_name("BrowDownLeft", s));
    FGB_CHECK(s == Shape::BrowDownLeft);

    FGB_CHECK(!shape_from_name("NotARealShape", s));
    FGB_CHECK(!shape_from_name("j awopen", s));
    FGB_CHECK(!shape_from_name("", s));
}

FGB_TEST(shape_index_is_stable) {
    // 索引一旦发布就不能变，否则录制的 .fgbcap 全部失效
    FGB_CHECK(index_of(Shape::BrowDownLeft) == 0);
    FGB_CHECK(static_cast<std::size_t>(Shape::JawOpen) == 20);
}
