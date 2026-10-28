#include "core/model/shape.hpp"

#include "test_framework.hpp"

#include <cstring>
#include <string_view>

using namespace fgb;

FGB_TEST(shape_count_matches_spec) {
    // Unified Expressions 协议层形状集，必须与 VRCFaceTracking 的
    // `enum UnifiedExpressions` 完全一致（Max = 88）。
    // 这个数字变了必须是有意识的决定，因为它是线上协议的隐式版本号。
    FGB_CHECK(kShapeCount == 88);
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

    FGB_CHECK(shape_from_name("BrowLowererLeft", s));
    FGB_CHECK(s == Shape::BrowLowererLeft);

    // 合成参数不属于协议层，不能反查到——这是分层的分界线
    FGB_CHECK(!shape_from_name("BrowDownLeft", s));
    FGB_CHECK(!shape_from_name("MouthSmileLeft", s));

    FGB_CHECK(!shape_from_name("NotARealShape", s));
    FGB_CHECK(!shape_from_name("j awopen", s));
    FGB_CHECK(!shape_from_name("", s));
}

FGB_TEST(shape_index_is_stable) {
    // 索引一旦发布就不能变，否则 IPC 解码和录制的 .fgbcap 全部失效
    FGB_CHECK(index_of(Shape::EyeSquintRight) == 0);
    FGB_CHECK(index_of(Shape::JawOpen) == 22);
    FGB_CHECK(index_of(Shape::MouthClosed) == 29);
    FGB_CHECK(index_of(Shape::NeckFlexLeft) == 87);
}
