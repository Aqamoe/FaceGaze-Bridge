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

FGB_TEST(gaze_calibrator_refines_direction_from_calibrated_points) {
    // 角度分支：点已经过仿射，方向必须跟着换口径，否则读方向的消费端拿到未校准值。
    GazeCalibrator calibrator;
    const GazeProjection projection = unit_projection();
    calibrator.set_projection(projection);

    GazeProfile profile;
    profile.version      = 5;
    profile.source_kind  = GazeSourceKind::angles;
    profile.residual_p95 = 0.015f;
    for (GazeEyeMap& eye : profile.eyes) {
        eye.active = true;
        eye.a      = 1.3f; // X' = 1.3x - 0.1
        eye.c      = -0.1f;
        eye.e      = 1.3f; // Y' = 1.3y - 0.05
        eye.f      = -0.05f;
    }
    calibrator.set_profile(profile);

    GazeState gaze;
    gaze.capture_ts_ns   = kNow;
    gaze.valid           = true;
    gaze.dir_space       = GazeDirSpace::source;
    gaze.left_valid      = true;
    gaze.right_valid     = true;
    const Vec3 start_dir = pitch_yaw_deg_to_dir(0.0f, 20.0f);
    gaze.left_dir_local  = start_dir;
    gaze.right_dir_local = start_dir;

    calibrator.apply(gaze);

    FGB_CHECK(gaze.space == GazeSpace::calibrated);
    FGB_CHECK(gaze.dir_space == GazeDirSpace::calibrated);
    // 映射把点往右推，方向必须跟着往右；只看点不看方向就漏了这件事
    FGB_CHECK(gaze.left_dir_local.x > start_dir.x + 0.05f);
    FGB_CHECK(gaze.left_pitch_deg < -1.0f); // 点下移 → 俯仰为负
    // 方向必须正好是"校准后那个点"反投影出来的方向
    Vec3 want{};
    FGB_CHECK(unproject_viewport(gaze.left_point_2d, projection.eyes[0], want));
    FGB_CHECK_NEAR(gaze.left_dir_local.x, want.x, 1e-4f);
    FGB_CHECK_NEAR(gaze.left_dir_local.y, want.y, 1e-4f);
    FGB_CHECK_NEAR(gaze.left_dir_local.z, want.z, 1e-4f);
    // 合并方向/角度跟着重算，不能留在映射前
    FGB_CHECK_NEAR(gaze.yaw_deg, gaze.left_yaw_deg, 1e-4f);
    FGB_CHECK_NEAR(gaze.pitch_deg, gaze.left_pitch_deg, 1e-4f);
}

FGB_TEST(gaze_calibrator_leaves_direction_on_points_branch) {
    // 点型分支没有角度模型：点被校准了，方向仍是来源的，不能拿几何凭空编一个
    GazeCalibrator calibrator;
    calibrator.set_projection(unit_projection());

    GazeProfile profile;
    profile.version     = 2;
    profile.source_kind = GazeSourceKind::points;
    for (GazeEyeMap& eye : profile.eyes) {
        eye.active = true;
        eye.a      = 2.0f; // X' = 2x - 0.1
        eye.c      = -0.1f;
    }
    calibrator.set_profile(profile);

    GazeState gaze;
    gaze.capture_ts_ns     = kNow;
    gaze.point_viewport    = true; // 模块声明的视口点
    gaze.valid             = true;
    gaze.dir_space         = GazeDirSpace::source;
    gaze.left_valid        = true;
    gaze.right_valid       = true;
    gaze.left_dir_local    = {0.3f, 0.0f, -0.954f};
    gaze.right_dir_local   = gaze.left_dir_local;
    gaze.left_point_valid  = true;
    gaze.right_point_valid = true;
    gaze.left_point_2d     = {0.30f, 0.40f};
    gaze.right_point_2d    = {0.40f, 0.40f};

    GazeNormalizer normalizer;
    normalizer.apply(gaze, kNow);
    calibrator.apply(gaze);

    FGB_CHECK(gaze.space == GazeSpace::calibrated);
    FGB_CHECK(gaze.dir_space == GazeDirSpace::source);
    FGB_CHECK_NEAR(gaze.left_dir_local.x, 0.3f, 1e-6f);
    FGB_CHECK_NEAR(gaze.left_point_2d.x, 0.5, 1e-5);
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
    profile.have_trim        = true;
    profile.trim.active      = true;
    profile.trim.gain_x      = 1.25f;
    profile.trim.gain_y      = 0.9f;
    profile.trim.center_x    = -1.5f;
    profile.trim.center_y    = -8.0f;

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
    FGB_CHECK(loaded.have_trim);
    FGB_CHECK(loaded.trim.active);
    FGB_CHECK_NEAR(loaded.trim.gain_x, 1.25f, 1e-4f);
    FGB_CHECK_NEAR(loaded.trim.center_y, -8.0f, 1e-4f);
}

// 角度粗调被改动后，档案必须被判成过期：仿射本身不会报错，只能靠这条比对发现。
FGB_TEST(gaze_profile_detects_stale_eye_trim) {
    GazeProfile profile;
    profile.version        = 1;
    profile.source_kind    = GazeSourceKind::angles;
    profile.eyes[0].active = true;
    profile.eyes[1].active = true;
    profile.have_trim      = true;
    profile.trim.active    = true;
    profile.trim.gain_x    = 1.2f;
    profile.trim.center_y  = -5.0f;

    EyeAngleTrim current;
    current.active   = true;
    current.gain_x   = 1.2f;
    current.center_y = -5.0f;
    FGB_CHECK(trim_state(profile, current) == TrimState::matches);

    current.gain_y = 1.1f; // 标定完又拖了一下垂直增益
    FGB_CHECK(trim_state(profile, current) == TrimState::stale);

    // 关掉开关 = 恒等：与"active + 恒等"效果相同，不该被判过期
    GazeProfile identity_trim;
    identity_trim.version        = 1;
    identity_trim.source_kind    = GazeSourceKind::angles;
    identity_trim.eyes[0].active = true;
    identity_trim.have_trim      = true; // trim 全默认 = 恒等
    EyeAngleTrim on_identity;
    on_identity.active = true; // gain 1 / center 0
    FGB_CHECK(trim_state(identity_trim, on_identity) == TrimState::matches);
    FGB_CHECK(trim_state(identity_trim, EyeAngleTrim{}) == TrimState::matches);

    // 旧档案没写这条：只能是"未知"，不能冒充一致或过期
    GazeProfile old_profile;
    old_profile.version        = 1;
    old_profile.source_kind    = GazeSourceKind::angles;
    old_profile.eyes[0].active = true;
    FGB_CHECK(trim_state(old_profile, on_identity) == TrimState::unknown);
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

// 饱和 = "贴边" 且 "值被钉住"，两个条件缺一不可。
//
// 只用"出框"判饱和会把标定采样范围当成传感器量程：标定窗口只有屏幕中间一块
// （13 点圆盘 ±35° 也只到 x 0.27..0.90），正常看一眼边缘就出框 → 位置被锁在中心
// 小矩形里，正是"标定后反而到不了边缘"的症状。
FGB_TEST(gaze_calibrator_freezes_only_when_raw_is_pinned) {
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

    const auto frame = [](float left_x, float right_x) {
        GazeState gaze;
        gaze.capture_ts_ns     = kNow;
        gaze.left_point_valid  = true;
        gaze.right_point_valid = true;
        gaze.left_point_2d     = {left_x, 0.52f};
        gaze.right_point_2d    = {right_x, 0.52f};
        return gaze;
    };

    // 先来一帧范围内：建立"最后一对好位置"，也让上一帧原始值有意义
    GazeState inside = frame(0.52f, 0.52f);
    calibrator.apply(inside);
    FGB_CHECK(!inside.saturated);
    FGB_CHECK(inside.raw_point_valid);
    FGB_CHECK_NEAR(inside.raw_left_point_2d.x, 0.52f, 1e-6f);

    // 出框但值还在变（正常看屏幕边缘）：不判饱和，位置照常跟随
    GazeState reaching = frame(0.90f, 0.88f);
    calibrator.apply(reaching);
    FGB_CHECK(!reaching.saturated);
    FGB_CHECK_NEAR(reaching.left_point_2d.x, 0.90f, 1e-6f);
    FGB_CHECK_NEAR(reaching.right_point_2d.x, 0.88f, 1e-6f);

    // 贴边且被钉住：连续 kSaturatedHoldFrames 帧同一个值才判饱和
    for (int i = 0; i + 1 < GazeCalibrator::kSaturatedHoldFrames; ++i) {
        GazeState spinning = frame(0.90f, 0.88f);
        calibrator.apply(spinning);
        FGB_CHECK(!spinning.saturated); // 还没到帧数，不能提前冻结
    }
    GazeState pinned = frame(0.90f, 0.88f);
    calibrator.apply(pinned);
    FGB_CHECK(pinned.saturated);

    // 冻结后位置保持"最后一处非饱和位置"，两眼一起停（不会各按各的边界越走越开）
    GazeState held = frame(0.90f, 0.62f);
    calibrator.apply(held);
    FGB_CHECK(held.saturated);
    FGB_CHECK_NEAR(held.left_point_2d.x, pinned.left_point_2d.x, 1e-6f);
    FGB_CHECK_NEAR(held.right_point_2d.x, pinned.right_point_2d.x, 1e-6f);

    // 回到范围内：恢复跟随
    GazeState back = frame(0.60f, 0.60f);
    calibrator.apply(back);
    FGB_CHECK(!back.saturated);
    FGB_CHECK_NEAR(back.left_point_2d.x, 0.60f, 1e-6f);
}

// 极值参照 = 本会话观测到的包络，而不是档案里的标定窗口。
//
// 窄范围标定的窗口只有屏幕中间一块（左眼 0.42..0.75）。用户真正看到更远之后，
// 窗口边缘处的稳定注视不该再被判成"传感器到极限"——否则又回到"标定后只能在中
// 间一小块里动"的老问题上。
FGB_TEST(gaze_calibrator_learns_wider_range_than_profile_bounds) {
    GazeCalibrator calibrator;

    GazeProfile profile;
    profile.version         = 3;
    profile.source_kind     = GazeSourceKind::points;
    profile.have_bounds     = true;
    profile.input_bounds[0] = {0.42f, 0.75f, 0.31f, 0.69f};
    profile.input_bounds[1] = {0.25f, 0.58f, 0.31f, 0.69f};
    profile.eyes[0].active  = true;
    profile.eyes[0].a       = 1.0f;
    profile.eyes[1].active  = true;
    profile.eyes[1].a       = 1.0f;
    calibrator.set_profile(profile);

    const auto frame = [](float left_x, float right_x) {
        GazeState gaze;
        gaze.capture_ts_ns     = kNow;
        gaze.left_point_valid  = true;
        gaze.right_point_valid = true;
        gaze.left_point_2d     = {left_x, 0.52f};
        gaze.right_point_2d    = {right_x, 0.52f};
        return gaze;
    };

    // 1) 真看到远处：包络长到 0.90。值一直在变，不该判饱和
    for (float x = 0.78f; x <= 0.9001f; x += 0.03f) {
        GazeState moving = frame(x, x);
        calibrator.apply(moving);
        FGB_CHECK(!moving.saturated);
    }

    // 2) 回到"旧窗口边缘"并稳住：已落在包络内部，不该冻结
    for (int i = 0; i < GazeCalibrator::kSaturatedHoldFrames * 2; ++i) {
        GazeState steady = frame(0.72f, 0.72f);
        calibrator.apply(steady);
        FGB_CHECK(!steady.saturated);
        FGB_CHECK_NEAR(steady.left_point_2d.x, 0.72f, 1e-6f);
    }

    // 3) 停在真正的极限上（0.90 不再变化）→ 这时才是饱和
    for (int i = 0; i < GazeCalibrator::kSaturatedHoldFrames; ++i) {
        GazeState pinned = frame(0.90f, 0.90f);
        calibrator.apply(pinned);
    }
    GazeState pinned = frame(0.90f, 0.90f);
    calibrator.apply(pinned);
    FGB_CHECK(pinned.saturated);
}
