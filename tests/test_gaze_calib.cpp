// 圆盘标定：点位、采集、拟合。全部是纯数据逻辑，不碰 OpenVR。
#include "core/model/gaze_projection.hpp"
#include "gaze_calib/collector.hpp"
#include "gaze_calib/fit.hpp"
#include "gaze_calib/plan.hpp"

#include "test_framework.hpp"

#include <cmath>
#include <cstdint>
#include <vector>

using namespace fgb;
using namespace fgb::gaze_calib;

namespace {

GazeState point_state(float left_x, float left_y, float right_x, float right_y) {
    GazeState gaze;
    gaze.left_point_valid  = true;
    gaze.right_point_valid = true;
    gaze.left_point_2d     = {left_x, left_y};
    gaze.right_point_2d    = {right_x, right_y};
    return gaze;
}

// 已知映射：用它生成"目标 = map(观察)"的数据，再看拟合能不能把参数找回来。
GazeEyeMap known_map() {
    GazeEyeMap map;
    map.active = true;
    map.a      = 1.15f;
    map.b      = 0.04f;
    map.c      = -0.06f;
    map.d      = -0.03f;
    map.e      = 1.08f;
    map.f      = 0.02f;
    return map;
}

std::vector<Pair> synthetic_pairs(const GazeEyeMap& map, float noise = 0.0f) {
    std::vector<Pair> pairs;
    float n = 0.0f;
    for (const Target& target : targets()) {
        // 观察点故意用另一处位置，避免"输入=输出"这种退化情形
        const float ox = 0.5f + 0.30f * std::sin(target.yaw_deg * 0.31f);
        const float oy = 0.5f + 0.28f * std::cos(target.pitch_deg * 0.27f);
        Pair pair;
        pair.observed = Vec2{ox, oy};
        map.apply(ox, oy, pair.target.x, pair.target.y);
        // 一点噪声：验证残差能算出来而不是恒 0
        n += 0.37f;
        pair.target.x += noise * std::sin(n);
        pair.target.y += noise * std::cos(n * 1.7f);
        pairs.push_back(pair);
    }
    return pairs;
}

} // namespace

FGB_TEST(gaze_calib_plan_has_center_and_two_rings) {
    FGB_CHECK(targets().size() == kTargetCount);
    FGB_CHECK_NEAR(targets()[0].yaw_deg, 0.0, 1e-6);
    FGB_CHECK_NEAR(targets()[0].pitch_deg, 0.0, 1e-6);

    // 内环四点 + 外环八点：外环半径明显大于内环
    float inner_max = 0.0f;
    float outer_max = 0.0f;
    for (std::size_t i = 1; i < 5; ++i) {
        const float r = std::hypot(targets()[i].yaw_deg, targets()[i].pitch_deg);
        inner_max     = std::max(inner_max, r);
    }
    for (std::size_t i = 5; i < kTargetCount; ++i) {
        const float r = std::hypot(targets()[i].yaw_deg, targets()[i].pitch_deg);
        outer_max     = std::max(outer_max, r);
    }
    FGB_CHECK_NEAR(inner_max, kInnerRingDeg, 1e-3);
    FGB_CHECK_NEAR(outer_max, kOuterRingDeg, 1e-3);
}

FGB_TEST(gaze_calib_targets_project_inside_viewport) {
    const GazeProjection projection = GazeProjection::symmetric(1.0f, 1.0f);
    for (const Target& target : targets()) {
        Vec2 left{};
        Vec2 right{};
        FGB_CHECK(target_viewport(target, projection.eyes[0], left));
        FGB_CHECK(target_viewport(target, projection.eyes[1], right));
        FGB_CHECK(left.x >= 0.0f && left.x <= 1.0f);
        FGB_CHECK(left.y >= 0.0f && left.y <= 1.0f);
        // 对称投影下两眼的视口坐标一致
        FGB_CHECK_NEAR(left.x, right.x, 1e-6f);
    }
    // 中心目标 → 视口中心；向右看 → x 变大；向上看 → y 变小（视口 y 向下）
    Vec2 center{};
    FGB_CHECK(target_viewport(targets()[0], projection.eyes[0], center));
    FGB_CHECK_NEAR(center.x, 0.5f, 1e-5f);
    FGB_CHECK_NEAR(center.y, 0.5f, 1e-5f);
    Vec2 right_target{};
    FGB_CHECK(target_viewport(targets()[2], projection.eyes[0], right_target));
    FGB_CHECK(right_target.x > 0.5f);
    Vec2 up_target{};
    FGB_CHECK(target_viewport(targets()[1], projection.eyes[0], up_target));
    FGB_CHECK(up_target.y < 0.5f);
}

FGB_TEST(gaze_calib_collector_needs_enough_stable_samples) {
    Collector collector;

    for (int i = 0; i < 19; ++i) {
        FGB_CHECK(collector.add(0, point_state(0.5f, 0.5f, 0.5f, 0.5f)));
    }
    TargetSamples few = collector.result(0);
    FGB_CHECK(few.count == 19);
    FGB_CHECK(!few.ok); // 数量不够

    for (int i = 0; i < 10; ++i) {
        collector.add(0, point_state(0.5f, 0.5f, 0.5f, 0.5f));
    }
    TargetSamples stable = collector.result(0);
    FGB_CHECK(stable.ok);
    FGB_CHECK_NEAR(stable.left.x, 0.5f, 1e-6f);

    // 噪声大：数量够但离散度超标，同样不能算有效
    Collector noisy;
    for (int i = 0; i < 80; ++i) {
        const float jitter = (i % 2 == 0) ? 0.5f : -0.5f; // 方差巨大
        noisy.add(1, point_state(0.5f + jitter, 0.5f, 0.5f + jitter, 0.5f));
    }
    FGB_CHECK(!noisy.result(1).ok);
}

FGB_TEST(gaze_calib_collector_rejects_single_eye_frames) {
    Collector collector;
    GazeState one_eye;
    one_eye.left_point_valid  = true;
    one_eye.left_point_2d     = {0.5f, 0.5f};
    one_eye.right_point_valid = false;
    FGB_CHECK(!collector.add(0, one_eye));
    FGB_CHECK(collector.accepted() == 0);
}

FGB_TEST(gaze_calib_fit_recovers_known_affine) {
    const GazeEyeMap map = known_map();
    const std::vector<Pair> pairs = synthetic_pairs(map);
    const EyeFit fit = fit_affine(pairs);
    FGB_CHECK(fit.ok);
    FGB_CHECK(fit.used == kTargetCount);
    FGB_CHECK_NEAR(fit.map.a, map.a, 1e-3f);
    FGB_CHECK_NEAR(fit.map.b, map.b, 1e-3f);
    FGB_CHECK_NEAR(fit.map.c, map.c, 1e-3f);
    FGB_CHECK_NEAR(fit.map.d, map.d, 1e-3f);
    FGB_CHECK_NEAR(fit.map.e, map.e, 1e-3f);
    FGB_CHECK_NEAR(fit.map.f, map.f, 1e-3f);
    FGB_CHECK(fit.residual_p95 >= 0.0f);
    FGB_CHECK(fit.residual_p95 < 1e-3f); // 无噪声时几乎为 0
}

FGB_TEST(gaze_calib_fit_reports_residual_with_noise) {
    const std::vector<Pair> pairs = synthetic_pairs(known_map(), 0.01f);
    const EyeFit fit              = fit_affine(pairs);
    FGB_CHECK(fit.ok);
    FGB_CHECK(fit.residual_p95 > 0.0f);
    FGB_CHECK(fit.residual_p95 < 0.03f); // 1% 噪声的 P95 应在几厘米以内
}

FGB_TEST(gaze_calib_fit_rejects_bad_input) {
    std::vector<Pair> too_few(3);
    FGB_CHECK(!fit_affine(too_few).ok);

    // 共线：所有观察点落在一条水平线上，y 方向没有信息
    std::vector<Pair> collinear;
    for (int i = 0; i < 8; ++i) {
        Pair pair;
        pair.observed = Vec2{0.1f + 0.1f * static_cast<float>(i), 0.5f};
        pair.target   = pair.observed;
        collinear.push_back(pair);
    }
    const EyeFit degenerate = fit_affine(collinear);
    FGB_CHECK(!degenerate.ok);
    FGB_CHECK(std::string(degenerate.reason) == "degenerate-input");
}

FGB_TEST(gaze_calib_build_profile_requires_both_eyes) {
    const std::vector<Pair> pairs = synthetic_pairs(known_map());
    const EyeFit left             = fit_affine(pairs);
    EyeFit right                  = left;
    FGB_CHECK(left.ok && right.ok);

    GazeProfile profile;
    FGB_CHECK(build_profile(GazeSourceKind::points, left, right, 9, "2026-11-02T03:00:00Z",
                            "fp", profile));
    FGB_CHECK(profile.ready());
    FGB_CHECK(profile.version == 9u);
    FGB_CHECK(profile.source_kind == GazeSourceKind::points);
    FGB_CHECK(profile.eyes[0].active && profile.eyes[1].active);
    FGB_CHECK(profile.residual_p95 >= 0.0f);

    right.ok = false;
    GazeProfile rejected;
    FGB_CHECK(!build_profile(GazeSourceKind::points, left, right, 10, "", "", rejected));
    FGB_CHECK(!build_profile(GazeSourceKind::none, left, left, 10, "", "", rejected));
}
