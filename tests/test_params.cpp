#include "params/v2_params.hpp"

#include "base/json_value.hpp"
#include "test_framework.hpp"

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>
#include <string>
#include <string_view>
#include <vector>

using namespace fgb;

namespace {

float eval(const FaceFrame& frame, std::string_view name, float scale = 30.0f) {
    params::EvalContext ctx{frame, scale};
    for (const auto& def : params::composite_params()) {
        if (def.name == name) {
            return def.eval(ctx);
        }
    }
    return std::nanf("");
}

EyePair make_eyes(float pitch_deg, float yaw_deg, float openness) {
    EyePair eyes;
    eyes.left.valid  = true;
    eyes.right.valid = true;
    eyes.left.gazePitchYawDeg  = {pitch_deg, yaw_deg};
    eyes.right.gazePitchYawDeg = {pitch_deg, yaw_deg};
    eyes.left.openness         = openness;
    eyes.right.openness        = openness;
    return eyes;
}

} // namespace

FGB_TEST(params_table_is_wellformed) {
    const auto table = params::composite_params();
    FGB_CHECK(table.size() > 60); // 眼追 6 + 瞳孔 4 + 开合 9 + 简化 8 + 合成 ~50
    FGB_CHECK(table.size() < params::kMaxParamCount);

    for (std::size_t i = 0; i < table.size(); ++i) {
        FGB_CHECK(!table[i].name.empty());
        FGB_CHECK(table[i].eval != nullptr);
        for (std::size_t j = i + 1; j < table.size(); ++j) {
            FGB_CHECK(table[i].name != table[j].name);
        }
    }
}

FGB_TEST(params_eye_gaze_signs) {
    FaceFrame frame = FaceFrame::neutral();
    frame.eyes      = make_eyes(20.0f, 0.0f, 1.0f);
    // 向上看：Y 为正
    FGB_CHECK_NEAR(eval(frame, "EyeLeftY"), 20.0f / 30.0f, 1e-4);
    FGB_CHECK_NEAR(eval(frame, "EyeY"), 20.0f / 30.0f, 1e-4);

    frame.eyes = make_eyes(0.0f, 30.0f, 1.0f);
    // 向右看：X 为正
    FGB_CHECK_NEAR(eval(frame, "EyeLeftX"), 1.0f, 1e-4);
    FGB_CHECK_NEAR(eval(frame, "EyeX"), 1.0f, 1e-4);

    frame.eyes = make_eyes(0.0f, -15.0f, 1.0f);
    FGB_CHECK_NEAR(eval(frame, "EyeLeftX"), -0.5f, 1e-4);

    // 超量程钳制
    frame.eyes = make_eyes(90.0f, 90.0f, 1.0f);
    FGB_CHECK_NEAR(eval(frame, "EyeLeftX"), 1.0f, 1e-6);
    FGB_CHECK_NEAR(eval(frame, "EyeLeftY"), 1.0f, 1e-6);
}

FGB_TEST(params_eyelid_blends_wide) {
    FaceFrame frame = FaceFrame::neutral();
    frame.eyes      = make_eyes(0.0f, 0.0f, 1.0f);
    // 完全睁开、没有 EyeWide → 0.75
    FGB_CHECK_NEAR(eval(frame, "EyeLidLeft"), 0.75f, 1e-5);

    frame.shapes[index_of(Shape::EyeWideLeft)] = 1.0f;
    frame.shapes[index_of(Shape::EyeWideRight)] = 1.0f;
    FGB_CHECK_NEAR(eval(frame, "EyeLidLeft"), 1.0f, 1e-5);

    // 闭眼时 EyeLid 应为 0（Openness=0 且无 EyeWide）
    frame.shapes[index_of(Shape::EyeWideLeft)] = 0.0f;
    frame.shapes[index_of(Shape::EyeWideRight)] = 0.0f;
    frame.eyes = make_eyes(0.0f, 0.0f, 0.0f);
    FGB_CHECK_NEAR(eval(frame, "EyeLidLeft"), 0.0f, 1e-5);
    FGB_CHECK_NEAR(eval(frame, "EyeOpenLeft"), 0.0f, 1e-5);
    FGB_CHECK_NEAR(eval(frame, "EyeClosedLeft"), 1.0f, 1e-5);
}

FGB_TEST(params_jaw_and_mouth_direction) {
    FaceFrame frame = FaceFrame::neutral();
    frame.shapes[index_of(Shape::JawRight)] = 1.0f;
    frame.shapes[index_of(Shape::JawLeft)]  = 0.3f;
    FGB_CHECK_NEAR(eval(frame, "JawX"), 0.7f, 1e-5);

    frame.shapes[index_of(Shape::JawForward)]  = 0.5f;
    frame.shapes[index_of(Shape::JawBackward)] = 0.1f;
    FGB_CHECK_NEAR(eval(frame, "JawZ"), 0.4f, 1e-5);

    frame.shapes[index_of(Shape::MouthUpperRight)] = 0.6f;
    frame.shapes[index_of(Shape::MouthUpperLeft)]  = 0.2f;
    FGB_CHECK_NEAR(eval(frame, "MouthUpperX"), 0.4f, 1e-5);
}

FGB_TEST(params_browdown_is_weighted_mix) {
    FaceFrame frame = FaceFrame::neutral();
    frame.shapes[index_of(Shape::BrowLowererLeft)] = 1.0f;
    frame.shapes[index_of(Shape::BrowPinchLeft)]   = 0.0f;
    // 0.75 * lowerer + 0.25 * pinch
    FGB_CHECK_NEAR(eval(frame, "BrowDown"), 0.375f, 1e-5); // 只压了左眉，BrowDown 取两侧均值

    frame.shapes[index_of(Shape::BrowLowererRight)] = 1.0f;
    FGB_CHECK_NEAR(eval(frame, "BrowDown"), 0.75f, 1e-5);

    frame.shapes[index_of(Shape::BrowPinchLeft)]  = 1.0f;
    frame.shapes[index_of(Shape::BrowPinchRight)] = 1.0f;
    FGB_CHECK_NEAR(eval(frame, "BrowDown"), 1.0f, 1e-5);
}

FGB_TEST(params_smile_frown_composition) {
    FaceFrame frame = FaceFrame::neutral();
    frame.shapes[index_of(Shape::MouthCornerPullLeft)] = 1.0f;
    // smile = pull*0.8 + slant*0.2 = 0.8
    FGB_CHECK_NEAR(eval(frame, "SmileFrownLeft"), 0.8f, 1e-5);

    frame.shapes[index_of(Shape::MouthFrownLeft)] = 0.3f;
    FGB_CHECK_NEAR(eval(frame, "SmileFrownLeft"), 0.5f, 1e-5);

    // MouthSad 取 max(frown, stretch) 而非相加
    frame.shapes[index_of(Shape::MouthStretchLeft)] = 0.9f;
    FGB_CHECK_NEAR(eval(frame, "SmileSadLeft"), 0.8f - 0.9f, 1e-5);
}

FGB_TEST(params_all_finite_for_neutral_frame) {
    FaceFrame frame = FaceFrame::neutral();
    params::EvalContext ctx{frame, 30.0f};
    for (const auto& def : params::composite_params()) {
        const float v = def.eval(ctx);
        FGB_CHECK(std::isfinite(v));
    }
}

FGB_TEST(params_eye_scale_is_configurable) {
    FaceFrame frame = FaceFrame::neutral();
    frame.eyes      = make_eyes(15.0f, 15.0f, 1.0f);
    // 满量程 30° → 0.5；满量程 60° → 0.25
    FGB_CHECK_NEAR(eval(frame, "EyeLeftX", 30.0f), 0.5f, 1e-4);
    FGB_CHECK_NEAR(eval(frame, "EyeLeftX", 60.0f), 0.25f, 1e-4);
}

// ---------------------------------------------------------------------------
// 参数卡片的说明表
// ---------------------------------------------------------------------------

namespace {

// 读 ui/*.json 里某个对象段的键集合
std::set<std::string> json_keys(const std::filesystem::path& path, std::string_view section) {
    std::set<std::string> keys;
    std::ifstream file{path, std::ios::binary};
    if (!file) {
        return keys;
    }
    const std::string text{std::istreambuf_iterator<char>(file),
                           std::istreambuf_iterator<char>()};
    json::Value root;
    std::string error;
    if (!json::parse(text, root, error)) {
        return keys;
    }
    const json::Value* node = root.find(section);
    if (node == nullptr || !node->is_object()) {
        return keys;
    }
    for (const auto& entry : node->object) {
        keys.insert(entry.first);
    }
    return keys;
}

} // namespace

// 每张参数卡片都要有"人话"说明，否则用户只能看到 JawZ、LipSuckFunnelUpperRight 这种名字。
// 基础 88 个形状的名字在 shapes.json（校准页共用），其余由 tools/gen_param_desc.py 生成。
// 新增参数忘了配说明时这里会红——比等用户发现卡片是空的要早。
FGB_TEST(params_every_card_has_a_description) {
    const std::filesystem::path ui = std::filesystem::path{FGB_SOURCE_DIR} / "ui";
    const std::set<std::string> shapes = json_keys(ui / "shapes.json", "shapes");
    const std::set<std::string> desc   = json_keys(ui / "param_desc.json", "params");
    FGB_CHECK(shapes.size() == 88);
    FGB_CHECK(!desc.empty());

    std::vector<std::string> missing;
    for (const params::Group& group : params::groups()) {
        for (const params::ParamEntry& entry : group.entries) {
            const std::string name{entry.name};
            if (shapes.count(name) == 0 && desc.count(name) == 0) {
                missing.emplace_back(name);
            }
        }
    }
    for (const std::string& name : missing) {
        std::fprintf(stderr, "    缺说明：%s\n", name.c_str());
    }
    FGB_CHECK(missing.empty());
}
