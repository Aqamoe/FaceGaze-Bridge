// 注视管线：规范化（结构）与标定（口径）两段。只验证"该变成什么"，不碰 UDP/NVIDIA。
#include "core/model/gaze_profile.hpp"
#include "core/model/gaze_projection.hpp"
#include "core/process/gaze_calibrator.hpp"
#include "core/process/gaze_normalizer.hpp"
#include "params/gaze_profile.hpp"

#include "test_framework.hpp"

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string>

using namespace fgb;
using namespace fgb::params;

namespace {

constexpr std::uint64_t kNow = 1'000'000'000;

GazeState viewport_points() {
    GazeState gaze;
    gaze.point_viewport    = true;
    gaze.capture_ts_ns     = kNow;
    gaze.left_point_valid  = true;
    gaze.right_point_valid = true;
    gaze.left_point_2d     = {0.25f, 0.5f};
    gaze.right_point_2d    = {0.75f, 0.5f};
    return gaze;
}

GazeProjection unit_projection() {
    // ±45°，与真实头显的"中心不在 0"无关，测的是换算本身
    return GazeProjection::symmetric(1.0f, 1.0f);
}

std::string temp_path(const char* name) {
    std::error_code ec;
    const auto dir = std::filesystem::temp_directory_path(ec) / "fgb-tests";
    std::filesystem::create_directories(dir, ec);
    return (dir / name).string();
}

} // namespace

FGB_TEST(gaze_normalizer_tags_space_from_declaration) {
    GazeNormalizer normalizer;

    GazeState source = viewport_points();
    source.point_viewport = false;
    normalizer.apply(source, kNow);
    FGB_CHECK(source.space == GazeSpace::source);
    FGB_CHECK(!source.point_viewport);

    GazeState declared = viewport_points();
    normalizer.apply(declared, kNow);
    FGB_CHECK(declared.space == GazeSpace::viewport);
    FGB_CHECK(declared.point_viewport);
}

FGB_TEST(gaze_normalizer_drops_stale_points_per_eye) {
    GazeNormalizer normalizer;

    // 两眼都过期：没有可镜像的来源，必须都判无效（不能拿旧位置当"现在看向哪"）
    GazeState both_stale = viewport_points();
    both_stale.left_capture_ts_ns  = kNow - 100'000'000;
    both_stale.right_capture_ts_ns = kNow - 100'000'000;
    normalizer.apply(both_stale, kNow);
    FGB_CHECK(!both_stale.left_point_valid);
    FGB_CHECK(!both_stale.right_point_valid);
    FGB_CHECK(!both_stale.point2d_valid);

    // 一眼过期、一眼新鲜：新鲜那眼镜像过去，消费端不会因为单眼掉线整帧停发
    GazeState mixed = viewport_points();
    mixed.left_capture_ts_ns  = kNow - 100'000'000;
    mixed.right_capture_ts_ns = kNow;
    normalizer.apply(mixed, kNow);
    FGB_CHECK(mixed.left_point_valid);
    FGB_CHECK(mixed.right_point_valid);
    FGB_CHECK_NEAR(mixed.left_point_2d.x, mixed.right_point_2d.x, 1e-6);
}

FGB_TEST(gaze_normalizer_keeps_untimed_points) {
    GazeNormalizer normalizer;
    GazeState gaze = viewport_points();
    gaze.capture_ts_ns       = 0;
    gaze.left_capture_ts_ns  = 0;
    gaze.right_capture_ts_ns = 0;
    normalizer.apply(gaze, kNow);
    FGB_CHECK(gaze.left_point_valid);
    FGB_CHECK(gaze.right_point_valid);
}

FGB_TEST(gaze_normalizer_does_not_mirror_source_space_points) {
    GazeNormalizer normalizer;
    GazeState gaze;
    gaze.capture_ts_ns    = kNow;
    gaze.left_point_valid = true;
    gaze.left_point_2d    = {0.73f, 0.48f};
    normalizer.apply(gaze, kNow);
    // 原始坐标不是屏幕位置，凭一眼编另一眼没有意义
    FGB_CHECK(gaze.left_point_valid);
    FGB_CHECK(!gaze.right_point_valid);
    FGB_CHECK(gaze.space == GazeSpace::source);
}

FGB_TEST(gaze_calibrator_projects_direction_when_geometry_present) {
    GazeCalibrator calibrator;
    calibrator.set_projection(unit_projection());

    GazeState gaze;
    gaze.capture_ts_ns  = kNow;
    gaze.left_valid     = true;
    gaze.right_valid    = true;
    gaze.left_dir_local = {0.0f, 0.0f, -1.0f};
    gaze.right_dir_local = {0.0f, 0.0f, -1.0f};

    calibrator.apply(gaze);
    FGB_CHECK(gaze.space == GazeSpace::viewport);
    FGB_CHECK(gaze.left_point_valid);
    FGB_CHECK(gaze.right_point_valid);
    // 正前方：对称投影下视口中心
    FGB_CHECK_NEAR(gaze.left_point_2d.x, 0.5, 1e-5);
    FGB_CHECK_NEAR(gaze.left_point_2d.y, 0.5, 1e-5);
    FGB_CHECK(!gaze.geometry_missing);

    // 看右上：x 变大、y 变小（视口 y 向下）
    GazeState up_right;
    up_right.capture_ts_ns    = kNow;
    up_right.left_valid       = true;
    up_right.right_valid      = true;
    up_right.left_dir_local   = {0.5f, 0.5f, -0.866f};
    up_right.right_dir_local  = {0.5f, 0.5f, -0.866f};
    calibrator.apply(up_right);
    FGB_CHECK(up_right.left_point_valid);
    FGB_CHECK(up_right.left_point_2d.x > 0.5f);
    FGB_CHECK(up_right.left_point_2d.y < 0.5f);
}

FGB_TEST(gaze_calibrator_marks_geometry_missing) {
    GazeCalibrator calibrator; // 没有投影

    GazeState gaze;
    gaze.capture_ts_ns  = kNow;
    gaze.left_valid     = true;
    gaze.left_dir_local = {0.0f, 0.0f, -1.0f};

    calibrator.apply(gaze);
    FGB_CHECK(gaze.geometry_missing);
    FGB_CHECK(!gaze.left_point_valid);
    FGB_CHECK(gaze.space == GazeSpace::source);
}

FGB_TEST(gaze_calibrator_keeps_source_points_without_profile) {
    GazeCalibrator calibrator;
    GazeState gaze;
    gaze.capture_ts_ns     = kNow;
    gaze.left_point_valid  = true;
    gaze.right_point_valid = true;
    gaze.left_point_2d     = {0.73f, 0.48f};
    gaze.right_point_2d    = {0.43f, 0.48f};

    calibrator.apply(gaze);
    // 没有档案就不能假装它是屏幕位置：标签留在 source，消费端据此拒用
    FGB_CHECK(gaze.space == GazeSpace::source);
    FGB_CHECK(gaze.profile_version == 0);
}

FGB_TEST(gaze_calibrator_applies_fitted_points_profile) {
    GazeCalibrator calibrator;

    GazeProfile profile;
    profile.version     = 7;
    profile.source_kind = GazeSourceKind::points;
    profile.residual_p95 = 0.012f;
    profile.eyes[0].active = true;
    profile.eyes[0].a = 2.0f; // X' = 2x - 0.1
    profile.eyes[0].c = -0.1f;
    profile.eyes[1].active = true;
    profile.eyes[1].a = 2.0f;
    profile.eyes[1].c = -0.1f;
    calibrator.set_profile(profile);

    GazeState gaze;
    gaze.capture_ts_ns     = kNow;
    gaze.left_point_valid  = true;
    gaze.right_point_valid = true;
    gaze.left_point_2d     = {0.30f, 0.40f};
    gaze.right_point_2d    = {0.40f, 0.40f};

    calibrator.apply(gaze);
    FGB_CHECK(gaze.space == GazeSpace::calibrated);
    FGB_CHECK(gaze.profile_version == 7u);
    FGB_CHECK_NEAR(gaze.residual, 0.012f, 1e-6f);
    FGB_CHECK_NEAR(gaze.left_point_2d.x, 0.5, 1e-5);  // 2*0.30-0.1
    FGB_CHECK_NEAR(gaze.right_point_2d.x, 0.7, 1e-5); // 2*0.40-0.1
    FGB_CHECK_NEAR(gaze.left_point_2d.y, 0.40, 1e-5); // 未映射的轴按单位阵
}

FGB_TEST(gaze_calibrator_ignores_profile_from_other_branch) {
    GazeCalibrator calibrator;

    GazeProfile profile;
    profile.version     = 3;
    profile.source_kind = GazeSourceKind::angles; // 档案是角度分支拟合的
    profile.eyes[0].active = true;
    profile.eyes[0].a = 3.0f;
    calibrator.set_profile(profile);

    GazeState gaze;
    gaze.capture_ts_ns    = kNow;
    gaze.point_viewport   = true; // 这条是模块声明的视口点（点分支）
    gaze.left_point_valid = true;
    gaze.right_point_valid = true;
    gaze.left_point_2d    = {0.2f, 0.2f};
    gaze.right_point_2d   = {0.2f, 0.2f};

    // 按管线顺序先规范化（space 由 point_viewport 得出），再标定
    GazeNormalizer normalizer;
    normalizer.apply(gaze, kNow);

    calibrator.apply(gaze);
    FGB_CHECK(gaze.space == GazeSpace::viewport); // 没被硬套
    FGB_CHECK_NEAR(gaze.left_point_2d.x, 0.2f, 1e-6f);
    FGB_CHECK(gaze.profile_version == 0u);
}

FGB_TEST(gaze_profile_roundtrip) {
    GazeProfile profile;
    profile.version      = 4;
    profile.source_kind  = GazeSourceKind::points;
    profile.residual_p95 = 0.0234f;
    profile.created_utc  = "2026-11-02T03:00:00Z";
    profile.hmd_identity = "e5d6f1ebff6c3a67";
    profile.eyes[0].active = true;
    profile.eyes[0].a = 1.1f;
    profile.eyes[0].b = 0.02f;
    profile.eyes[0].c = -0.05f;
    profile.eyes[1].active = true;
    profile.eyes[1].e = 0.9f;
    profile.eyes[1].f = 0.01f;
    profile.have_bounds      = true;
    profile.input_bounds[0]  = {0.31f, 0.74f, 0.32f, 0.71f};
    profile.input_bounds[1]  = {0.30f, 0.73f, 0.31f, 0.70f};

    const std::string path = temp_path("gaze.ini");
    std::string error;
    FGB_CHECK(save_gaze_profile(path, profile, error));

    GazeProfile loaded;
    FGB_CHECK(load_gaze_profile(path, loaded, error));
    FGB_CHECK(loaded.version == 4u);
    FGB_CHECK(loaded.source_kind == GazeSourceKind::points);
    FGB_CHECK_NEAR(loaded.residual_p95, 0.0234f, 1e-5f);
    FGB_CHECK(loaded.hmd_identity == "e5d6f1ebff6c3a67");
    FGB_CHECK(loaded.eyes[0].active);
    FGB_CHECK_NEAR(loaded.eyes[0].a, 1.1f, 1e-5f);
    FGB_CHECK_NEAR(loaded.eyes[1].e, 0.9f, 1e-5f);
    FGB_CHECK(loaded.ready());
    FGB_CHECK(loaded.have_bounds);
    FGB_CHECK_NEAR(loaded.input_bounds[0][1], 0.74f, 1e-4f);
    FGB_CHECK_NEAR(loaded.input_bounds[1][2], 0.31f, 1e-4f);
}

FGB_TEST(gaze_profile_rejects_incomplete_file) {
    const std::string path = temp_path("gaze_empty.ini");
    {
        std::FILE* file = std::fopen(path.c_str(), "wb");
        FGB_CHECK(file != nullptr);
        std::fputs("[left]\nactive = 1\n", file); // 没有 version/map
        std::fclose(file);
    }
    GazeProfile loaded;
    std::string error;
    FGB_CHECK(!load_gaze_profile(path, loaded, error));
    FGB_CHECK(!loaded.ready());
}

// 投影与反投影必须严格互逆：探针把"系统认为的注视点"画回头显，靠的就是这条回路。
FGB_TEST(gaze_projection_roundtrip) {
    GazeProjection projection = GazeProjection::symmetric(1.1f, 0.9f);
    // 再来一只不对称的（真实头显就是这样：左眼看得更靠左）
    projection.eyes[0].tangent = {-1.303f, 0.916f, -0.936f, 0.936f};

    const Vec3 dirs[] = {{0.0f, 0.0f, -1.0f}, {0.3f, 0.0f, -0.954f}, {0.0f, 0.4f, -0.916f},
                         {-0.5f, -0.2f, -0.842f}};
    for (const Vec3& dir : dirs) {
        for (const EyeProjection& eye : projection.eyes) {
            float x = 0.0f;
            float y = 0.0f;
            FGB_CHECK(project_dir_to_viewport(dir, eye, x, y));
            Vec3 back{};
            FGB_CHECK(unproject_viewport(Vec2{x, y}, eye, back));
            const Vec3 expected = normalize(dir);
            FGB_CHECK_NEAR(back.x, expected.x, 1e-4f);
            FGB_CHECK_NEAR(back.y, expected.y, 1e-4f);
            FGB_CHECK_NEAR(back.z, expected.z, 1e-4f);
        }
    }
}

// 来源的可达范围不到 [0,1]（实测 PICO 原生点只有 0.31..0.74）：贴到边界时判饱和，
// 并冻结在最后一对有效位置上——否则两眼各自的边界不同，会越走越开。
FGB_TEST(gaze_calibrator_freezes_when_input_saturates) {
    GazeCalibrator calibrator;

    GazeProfile profile;
    profile.version        = 2;
    profile.source_kind    = GazeSourceKind::points;
    profile.have_bounds    = true;
    profile.input_bounds[0] = {0.31f, 0.74f, 0.31f, 0.74f};
    profile.input_bounds[1] = {0.31f, 0.74f, 0.31f, 0.74f};
    profile.eyes[0].active = true;
    profile.eyes[0].a = 1.0f;
    profile.eyes[1].active = true;
    profile.eyes[1].a = 1.0f;
    calibrator.set_profile(profile);

    GazeState inside;
    inside.capture_ts_ns     = kNow;
    inside.left_point_valid  = true;
    inside.right_point_valid = true;
    inside.left_point_2d     = {0.52f, 0.52f};
    inside.right_point_2d    = {0.52f, 0.52f};
    calibrator.apply(inside);
    FGB_CHECK(!inside.saturated);
    FGB_CHECK(inside.raw_point_valid);
    FGB_CHECK_NEAR(inside.raw_left_point_2d.x, 0.52f, 1e-6f);

    // 走到边界：判饱和，位置保持上一次
    GazeState edge;
    edge.capture_ts_ns     = kNow;
    edge.left_point_valid  = true;
    edge.right_point_valid = true;
    edge.left_point_2d     = {0.74f, 0.52f};
    edge.right_point_2d    = {0.55f, 0.52f}; // 右眼还没到边界：这正是"分开"的来源
    calibrator.apply(edge);
    FGB_CHECK(edge.saturated);
    FGB_CHECK_NEAR(edge.left_point_2d.x, inside.left_point_2d.x, 1e-6f);
    FGB_CHECK_NEAR(edge.right_point_2d.x, inside.right_point_2d.x, 1e-6f);

    // 回到范围内：恢复跟随
    GazeState back;
    back.capture_ts_ns     = kNow;
    back.left_point_valid  = true;
    back.right_point_valid = true;
    back.left_point_2d     = {0.60f, 0.52f};
    back.right_point_2d    = {0.60f, 0.52f};
    calibrator.apply(back);
    FGB_CHECK(!back.saturated);
    FGB_CHECK_NEAR(back.left_point_2d.x, 0.60f, 1e-6f);
}
