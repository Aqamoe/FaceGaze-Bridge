// 注视估计：不依赖硬件，用合成方向序列验证扫视速度与注视迟滞。
#include "core/process/gaze_estimator.hpp"

#include "test_framework.hpp"

#include <cstdint>

using namespace fgb;

namespace {

constexpr std::uint64_t kTick = 8'333'333; // 120 Hz

FaceFrame eyes_frame() {
    FaceFrame frame = FaceFrame::neutral();
    frame.eyes.left.valid  = true;
    frame.eyes.right.valid = true;
    frame.add_channel(channel::kEyes);
    return frame;
}

GazeState gaze_at(float pitch_deg, float yaw_deg) {
    GazeState gaze;
    gaze.valid      = true;
    gaze.pitch_deg  = pitch_deg;
    gaze.yaw_deg    = yaw_deg;
    gaze.dir_local  = pitch_yaw_deg_to_dir(pitch_deg, yaw_deg);
    gaze.confidence = 1.0f;
    return gaze;
}

} // namespace

FGB_TEST(gaze_estimator_enters_fixation_after_dwell) {
    GazeEstimator estimator;
    FaceFrame frame = eyes_frame();
    GazeState gaze  = gaze_at(0.0f, 0.0f);

    estimator.update(frame, gaze, 0);
    FGB_CHECK(!gaze.fixation);
    FGB_CHECK_NEAR(gaze.saccade_vel_deg_s, 0.0, 1e-3);

    // 保持同一方向 100 ms：80 ms 后应进入 fixation
    for (int i = 1; i <= 12; ++i) {
        estimator.update(frame, gaze, static_cast<std::uint64_t>(i) * kTick);
    }
    FGB_CHECK(gaze.fixation);
    FGB_CHECK(gaze.fixation_ms >= 80.0f);
    FGB_CHECK_NEAR(gaze.saccade_vel_deg_s, 0.0, 1e-3);
}

FGB_TEST(gaze_estimator_detects_saccade_and_releases_fixation) {
    GazeEstimator estimator;
    FaceFrame frame = eyes_frame();
    GazeState gaze  = gaze_at(0.0f, 0.0f);

    // 先稳定注视
    for (int i = 0; i <= 12; ++i) {
        estimator.update(frame, gaze, static_cast<std::uint64_t>(i) * kTick);
    }
    FGB_CHECK(gaze.fixation);

    // 一帧内跳 20°：角速度远高于扫视阈值，fixation 必须退出
    gaze = gaze_at(20.0f, 0.0f);
    estimator.update(frame, gaze, 13 * kTick);
    FGB_CHECK(gaze.saccade_vel_deg_s > 100.0f);
    FGB_CHECK(!gaze.fixation);
    FGB_CHECK_NEAR(gaze.fixation_ms, 0.0, 1e-3);

    // 新位置停住 100 ms，再次进入注视
    for (int i = 14; i <= 26; ++i) {
        estimator.update(frame, gaze, static_cast<std::uint64_t>(i) * kTick);
    }
    FGB_CHECK(gaze.fixation);
}

FGB_TEST(gaze_estimator_resets_on_invalid_eyes_and_gaps) {
    GazeEstimator estimator;
    FaceFrame frame = eyes_frame();
    GazeState gaze  = gaze_at(0.0f, 0.0f);

    for (int i = 0; i <= 12; ++i) {
        estimator.update(frame, gaze, static_cast<std::uint64_t>(i) * kTick);
    }
    FGB_CHECK(gaze.fixation);

    // 眼追失效：立刻清状态
    FaceFrame invalid = FaceFrame::neutral();
    GazeState bad;
    bad.valid = false;
    estimator.update(invalid, bad, 13 * kTick);
    FGB_CHECK(!bad.fixation);
    FGB_CHECK_NEAR(bad.saccade_vel_deg_s, 0.0, 1e-3);

    // 恢复后先重新累计，而不是继承失效前的 fixation
    GazeState again = gaze_at(0.0f, 0.0f);
    estimator.update(frame, again, 14 * kTick);
    FGB_CHECK(!again.fixation);

    // 1 秒断档：不跨 gap 直接判定注视
    GazeState after_gap = gaze_at(0.0f, 0.0f);
    estimator.update(frame, after_gap, 14 * kTick + 1'000'000'000ULL);
    FGB_CHECK(!after_gap.fixation);
}
