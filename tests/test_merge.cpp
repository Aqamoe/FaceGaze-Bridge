// 多源合并：通道拆分、优先级、显式指定、过期、以及"后处理只跑一次"。
//
// 这些用例存在的理由：合并逻辑错了不会崩，只会"眼睛不动"或者"表情变肉"，
// 靠肉眼在 VR 里试非常贵。所以把每个选择分支都钉在单测里。
#include "engine/engine.hpp"
#include "engine/source_merger.hpp"

#include "test_framework.hpp"

#include <cmath>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>

using namespace fgb;

namespace {

// 可以手动喂帧的假源：poll 只吐一帧，吐完就空
class FakeSource final : public ISource {
public:
    explicit FakeSource(std::string name) : name_(std::move(name)) {}

    const char* name() const noexcept override { return name_.c_str(); }
    bool start() override { return true; }
    void stop() noexcept override {}
    SourceStats stats() const noexcept override { return stats_; }

    bool poll(FaceFrame& out) override {
        if (!pending_) {
            return false;
        }
        out      = next_;
        pending_ = false;
        ++stats_.frames;
        return true;
    }

    void push(const FaceFrame& frame) {
        next_    = frame;
        pending_ = true;
    }

private:
    std::string name_;
    FaceFrame next_{};
    bool pending_ = false;
    SourceStats stats_{};
};

class CountingProcessor final : public IFaceProcessor {
public:
    const char* name() const noexcept override { return "count"; }
    void process(FaceFrame&) noexcept override { ++calls; }
    int calls = 0;
};

FaceFrame expression_frame(float jaw_open) {
    FaceFrame frame = FaceFrame::neutral();
    frame.set_shape(Shape::JawOpen, jaw_open);
    frame.add_channel(channel::kExpression);
    frame.captureTsNs = 1000;
    return frame;
}

FaceFrame eyes_frame(float yaw_deg) {
    FaceFrame frame = FaceFrame::neutral();
    frame.eyes.left.valid           = true;
    frame.eyes.left.gazeConf        = 1.0f;
    frame.eyes.left.gazePitchYawDeg = Vec2{0.0f, yaw_deg};
    frame.eyes.left.gazeDirLocal    = Vec3{0.0f, yaw_deg / 30.0f, -1.0f};
    frame.add_channel(channel::kEyes);
    frame.captureTsNs = 2000;
    return frame;
}

FaceFrame gaze_frame(float yaw_deg) {
    FaceFrame frame = FaceFrame::neutral();
    frame.gaze.left.valid  = true;
    frame.gaze.right.valid = true;
    frame.gaze.left.dirLocal =
        Vec3{std::sin(yaw_deg * kDegToRad), 0.0f, -std::cos(yaw_deg * kDegToRad)};
    frame.gaze.right.dirLocal = frame.gaze.left.dirLocal;
    frame.gaze.combinedValid  = true;
    frame.gaze.combinedDirLocal = frame.gaze.left.dirLocal;
    frame.gaze.left.pointValid  = true;
    frame.gaze.right.pointValid = true;
    frame.gaze.pointsValid      = true;
    frame.gaze.left.point2d   = Vec2{0.4f, 0.6f};
    frame.gaze.right.point2d  = Vec2{0.45f, 0.55f};
    frame.gaze.confidence     = 0.9f;
    frame.gaze.left.confidence  = 0.9f;
    frame.gaze.right.confidence = 0.9f;
    frame.gaze.captureTsNs    = 5000;
    frame.gaze.left.captureTsNs  = 5000;
    frame.gaze.right.captureTsNs = 5000;
    frame.add_channel(channel::kGaze);
    frame.captureTsNs = 4000;
    return frame;
}

FaceFrame eyes_pair_frame(float left_open, float right_open, float left_yaw, float right_yaw) {
    FaceFrame frame = FaceFrame::neutral();
    frame.eyes.left.valid  = true;
    frame.eyes.right.valid = true;
    frame.eyes.left.openness  = left_open;
    frame.eyes.right.openness = right_open;
    frame.eyes.left.gazeDirLocal =
        Vec3{std::sin(left_yaw * kDegToRad), 0.0f, -std::cos(left_yaw * kDegToRad)};
    frame.eyes.right.gazeDirLocal =
        Vec3{std::sin(right_yaw * kDegToRad), 0.0f, -std::cos(right_yaw * kDegToRad)};
    frame.eyes.left.gazePitchYawDeg  = Vec2{0.0f, left_yaw};
    frame.eyes.right.gazePitchYawDeg = Vec2{0.0f, right_yaw};
    frame.eyes.left.gazeConf         = 0.9f;
    frame.eyes.right.gazeConf        = 0.9f;
    frame.add_channel(channel::kEyes);
    frame.captureTsNs = 2000;
    return frame;
}

FaceFrame eye_attrs_frame(bool left_open, bool right_open, bool left_pupil, bool right_pupil,
                          float left_open_value, float right_open_value,
                          float left_pupil_value, float right_pupil_value,
                          float left_yaw, float right_yaw, bool gaze) {
    FaceFrame frame = FaceFrame::neutral();
    frame.eyes.left.opennessValid  = left_open;
    frame.eyes.right.opennessValid = right_open;
    frame.eyes.left.pupilValid     = left_pupil;
    frame.eyes.right.pupilValid    = right_pupil;
    frame.eyes.left.gazeValid      = gaze;
    frame.eyes.right.gazeValid     = gaze;
    frame.eyes.left.valid          = left_open || left_pupil;
    frame.eyes.right.valid         = right_open || right_pupil;
    frame.eyes.left.openness       = left_open_value;
    frame.eyes.right.openness      = right_open_value;
    frame.eyes.left.pupilDiameterMm  = left_pupil_value;
    frame.eyes.right.pupilDiameterMm = right_pupil_value;
    frame.eyes.left.gazeDirLocal =
        Vec3{std::sin(left_yaw * kDegToRad), 0.0f, -std::cos(left_yaw * kDegToRad)};
    frame.eyes.right.gazeDirLocal =
        Vec3{std::sin(right_yaw * kDegToRad), 0.0f, -std::cos(right_yaw * kDegToRad)};
    frame.eyes.left.gazePitchYawDeg  = Vec2{0.0f, left_yaw};
    frame.eyes.right.gazePitchYawDeg = Vec2{0.0f, right_yaw};
    frame.add_channel(channel::kEyes);
    frame.captureTsNs = 2000;
    return frame;
}

FaceFrame head_frame() {
    FaceFrame frame    = FaceFrame::neutral();
    frame.hasHeadPose  = true;
    frame.headPosLocal = Vec3{0.1f, 0.2f, 0.3f};
    frame.add_channel(channel::kHead);
    frame.captureTsNs = 3000;
    return frame;
}

} // namespace

FGB_TEST(merge_splits_channels_by_capability) {
    FakeSource expression{"expr"};
    FakeSource eyes{"eyes"};

    SourceMerger merger;
    merger.add(&expression);
    merger.add(&eyes);

    expression.push(expression_frame(0.5f));
    eyes.push(eyes_frame(10.0f));

    FaceFrame merged;
    FGB_CHECK(merger.poll(10'000'000ULL, merged));
    // 表情来自第一个源
    FGB_CHECK_NEAR(merged.shape(Shape::JawOpen), 0.5, 1e-6);
    // 眼睛来自第二个源——这正是"谁最后 poll 谁赢"会丢掉的东西
    FGB_CHECK(merged.eyes.left.valid);
    FGB_CHECK_NEAR(merged.eyes.left.gazePitchYawDeg.y, 10.0, 1e-5);
    // 两路都算贡献者
    FGB_CHECK(merged.contributingSources == 0b11u);
    // 时间戳取用到的通道里最新的那个
    FGB_CHECK(merged.captureTsNs == 2000);
}

FGB_TEST(merge_priority_wins_when_both_provide) {
    FakeSource first{"first"};
    FakeSource second{"second"};

    SourceMerger merger; // 默认策略 = 注册顺序
    merger.add(&first);
    merger.add(&second);

    first.push(expression_frame(0.9f));
    second.push(expression_frame(0.1f));

    FaceFrame merged;
    FGB_CHECK(merger.poll(10'000'000ULL, merged));
    FGB_CHECK_NEAR(merged.shape(Shape::JawOpen), 0.9, 1e-6);
}

FGB_TEST(merge_explicit_source_overrides_priority) {
    FakeSource first{"first"};
    FakeSource second{"second"};

    SourceMerger merger;
    MergePolicy policy;
    policy.expression_source = "second"; // 名字匹配，不看顺序
    merger.set_policy(policy);
    merger.add(&first);
    merger.add(&second);

    first.push(expression_frame(0.9f));
    second.push(expression_frame(0.1f));

    FaceFrame merged;
    FGB_CHECK(merger.poll(10'000'000ULL, merged));
    FGB_CHECK_NEAR(merged.shape(Shape::JawOpen), 0.1, 1e-6);
}

FGB_TEST(merge_unknown_explicit_name_falls_back) {
    FakeSource only{"only"};

    SourceMerger merger;
    MergePolicy policy;
    policy.eye_source = "typo"; // 拼错不该让整路数据消失
    merger.set_policy(policy);
    merger.add(&only);

    only.push(eyes_frame(5.0f));

    FaceFrame merged;
    FGB_CHECK(merger.poll(10'000'000ULL, merged));
    FGB_CHECK(merged.eyes.left.valid);
}

FGB_TEST(merge_stale_source_is_skipped) {
    FakeSource expression{"expr"};
    FakeSource fallback{"fallback"};

    SourceMerger merger;
    MergePolicy policy;
    policy.expression_stale_ns = 100'000'000ULL; // 100 ms
    merger.set_policy(policy);
    merger.add(&expression);
    merger.add(&fallback);

    // t=0：两个源都新鲜 → 表情用第一个
    expression.push(expression_frame(0.8f));
    fallback.push(expression_frame(0.2f));
    FaceFrame merged;
    FGB_CHECK(merger.poll(1'000'000'000ULL, merged));
    FGB_CHECK_NEAR(merged.shape(Shape::JawOpen), 0.8, 1e-6);

    // t=200ms：只有 fallback 还在更新，expr 已经过期 → 表情改用它
    fallback.push(expression_frame(0.2f));
    FGB_CHECK(merger.poll(1'200'000'000ULL, merged));
    FGB_CHECK_NEAR(merged.shape(Shape::JawOpen), 0.2, 1e-6);
}

FGB_TEST(merge_missing_channel_is_cleared) {
    FakeSource expression{"expr"};

    SourceMerger merger;
    merger.add(&expression);
    expression.push(expression_frame(0.4f));

    FaceFrame merged;
    FGB_CHECK(merger.poll(10'000'000ULL, merged));
    FGB_CHECK_NEAR(merged.shape(Shape::JawOpen), 0.4, 1e-6);
    // 没有眼追/注视/头姿来源 → 明确清空，而不是留着上一帧的值
    FGB_CHECK(!merged.eyes.any_valid());
    FGB_CHECK(!merged.gaze.any_valid());
    FGB_CHECK(!merged.hasHeadPose);
}

FGB_TEST(merge_head_channel_can_come_from_third_source) {
    FakeSource expression{"expr"};
    FakeSource eyes{"eyes"};
    FakeSource head{"head"};

    SourceMerger merger;
    merger.add(&expression);
    merger.add(&eyes);
    merger.add(&head);

    expression.push(expression_frame(0.3f));
    eyes.push(eyes_frame(-7.0f));
    head.push(head_frame());

    FaceFrame merged;
    FGB_CHECK(merger.poll(10'000'000ULL, merged));
    FGB_CHECK(merged.hasHeadPose);
    FGB_CHECK_NEAR(merged.headPosLocal.y, 0.2, 1e-6);
    FGB_CHECK(merged.contributingSources == 0b111u);
}

FGB_TEST(merge_no_new_frame_does_not_publish) {
    FakeSource only{"only"};
    SourceMerger merger;
    merger.add(&only);

    FaceFrame merged;
    FGB_CHECK(!merger.poll(10'000'000ULL, merged)); // 一帧都没来

    only.push(expression_frame(0.5f));
    FGB_CHECK(merger.poll(11'000'000ULL, merged));
    // 这一帧已经用掉了，下一个 tick 不该再发布
    FGB_CHECK(!merger.poll(12'000'000ULL, merged));
}

FGB_TEST(engine_processors_run_once_per_merged_frame) {
    Engine engine;
    auto a = std::make_unique<FakeSource>("a");
    auto b = std::make_unique<FakeSource>("b");
    FakeSource* a_raw = a.get();
    FakeSource* b_raw = b.get();
    engine.add_source(std::move(a));
    engine.add_source(std::move(b));

    auto counter = std::make_unique<CountingProcessor>();
    CountingProcessor* counter_raw = counter.get();
    engine.add_processor(std::move(counter));

    for (int i = 0; i < 5; ++i) {
        a_raw->push(expression_frame(0.5f));
        b_raw->push(eyes_frame(1.0f));
        engine.tick_once(static_cast<std::uint64_t>(i) * 8'000'000ULL + 1);
    }

    // 两个源各 5 帧，但后处理只能跑 5 次：跑 10 次等于把滤波做了两遍
    FGB_CHECK(counter_raw->calls == 5);
}

FGB_TEST(merge_splits_gaze_from_eyes) {
    FakeSource eyes{"eyes"};
    FakeSource gaze{"gaze"};

    SourceMerger merger;
    merger.add(&eyes);
    merger.add(&gaze);

    eyes.push(eyes_frame(10.0f));
    gaze.push(gaze_frame(20.0f));

    FaceFrame merged;
    FGB_CHECK(merger.poll(10'000'000ULL, merged));
    FGB_CHECK(merged.eyes.left.valid);
    FGB_CHECK_NEAR(merged.eyes.left.gazePitchYawDeg.y, 10.0, 1e-5);
    FGB_CHECK(merged.gaze.any_valid());
    FGB_CHECK_NEAR(merged.gaze.combinedDirLocal.x, std::sin(20.0f * kDegToRad), 1e-5);
    FGB_CHECK(merged.has_channel(channel::kEyes));
    FGB_CHECK(merged.has_channel(channel::kGaze));
    FGB_CHECK(merged.channelStamps[channel::index(channel::kEyes)].source == 1);
    FGB_CHECK(merged.channelStamps[channel::index(channel::kGaze)].source == 2);
    // gaze 允许带自己的捕获时间，不能被整帧时间戳覆盖
    FGB_CHECK(merged.channelStamps[channel::index(channel::kGaze)].captureTsNs == 5000);
    FGB_CHECK(merged.captureTsNs == 5000);
}

FGB_TEST(merge_explicit_gaze_source_overrides_priority) {
    FakeSource first{"first"};
    FakeSource second{"second"};

    SourceMerger merger;
    MergePolicy policy;
    policy.gaze_source = "second";
    merger.set_policy(policy);
    merger.add(&first);
    merger.add(&second);

    first.push(gaze_frame(5.0f));
    second.push(gaze_frame(-25.0f));

    FaceFrame merged;
    FGB_CHECK(merger.poll(10'000'000ULL, merged));
    FGB_CHECK_NEAR(merged.gaze.combinedDirLocal.x, std::sin(-25.0f * kDegToRad), 1e-5);
}

FGB_TEST(merge_gaze_has_shorter_stale_window) {
    FakeSource eyes{"eyes"};
    FakeSource gaze{"gaze"};

    SourceMerger merger;
    MergePolicy policy;
    policy.eye_stale_ns  = 250'000'000ULL;
    policy.gaze_stale_ns = 100'000'000ULL;
    merger.set_policy(policy);
    merger.add(&eyes);
    merger.add(&gaze);

    eyes.push(eyes_frame(1.0f));
    gaze.push(gaze_frame(2.0f));
    FaceFrame merged;
    FGB_CHECK(merger.poll(1'000'000'000ULL, merged));
    FGB_CHECK(merged.has_channel(channel::kGaze));

    // 150 ms 后：眼睛仍新鲜（<=250），注视已过期（>100）
    eyes.push(eyes_frame(3.0f));
    FGB_CHECK(merger.poll(1'150'000'000ULL, merged));
    FGB_CHECK(merged.has_channel(channel::kEyes));
    FGB_CHECK(!merged.has_channel(channel::kGaze));
    FGB_CHECK(!merged.gaze.any_valid());
}

FGB_TEST(merge_avatar_eye_can_take_openness_from_one_source_and_gaze_from_another) {
    FakeSource eyes{"eye"};
    FakeSource gaze{"gaze"};

    SourceMerger merger;
    MergePolicy policy;
    policy.eye_source            = "eye";
    policy.gaze_source           = "gaze";
    policy.avatar_eye_from_gaze  = true;
    merger.set_policy(policy);
    merger.add(&eyes);
    merger.add(&gaze);

    eyes.push(eyes_pair_frame(0.2f, 0.8f, -10.0f, -10.0f));
    gaze.push(gaze_frame(20.0f));

    FaceFrame merged;
    FGB_CHECK(merger.poll(10'000'000ULL, merged));
    FGB_CHECK_NEAR(merged.eyes.left.openness, 0.2, 1e-6);
    FGB_CHECK_NEAR(merged.eyes.right.openness, 0.8, 1e-6);
    // 方向来自 Gaze 源，openness 仍来自 Eyes 源
    FGB_CHECK_NEAR(merged.eyes.left.gazePitchYawDeg.y, 20.0, 1e-4);
    FGB_CHECK_NEAR(merged.eyes.right.gazePitchYawDeg.y, 20.0, 1e-4);
    FGB_CHECK(merged.eyes.left.opennessSource == 1);
    FGB_CHECK(merged.eyes.left.gazeSource == 2);
    FGB_CHECK(merged.eyes.left.opennessTsNs == 2000);
    FGB_CHECK(merged.eyes.left.gazeTsNs == 5000);
    FGB_CHECK(merged.channelStamps[channel::index(channel::kEyes)].source == kInvalidSource);
    FGB_CHECK(merged.channelStamps[channel::index(channel::kGaze)].source == 2);
}

FGB_TEST(merge_eye_sides_can_come_from_different_sources) {
    FakeSource left{"left"};
    FakeSource right{"right"};

    SourceMerger merger;
    MergePolicy policy;
    policy.eye_left_source  = "left";
    policy.eye_right_source = "right";
    merger.set_policy(policy);
    merger.add(&left);
    merger.add(&right);

    left.push(eyes_pair_frame(0.2f, 0.9f, -12.0f, -12.0f));
    right.push(eyes_pair_frame(0.8f, 0.3f, 12.0f, 12.0f));

    FaceFrame merged;
    FGB_CHECK(merger.poll(10'000'000ULL, merged));
    FGB_CHECK_NEAR(merged.eyes.left.openness, 0.2, 1e-6);
    FGB_CHECK_NEAR(merged.eyes.right.openness, 0.3, 1e-6);
    FGB_CHECK_NEAR(merged.eyes.left.gazePitchYawDeg.y, -12.0, 1e-4);
    FGB_CHECK_NEAR(merged.eyes.right.gazePitchYawDeg.y, 12.0, 1e-4);
    FGB_CHECK(merged.eyes.left.opennessSource == 1);
    FGB_CHECK(merged.eyes.right.opennessSource == 2);
    FGB_CHECK(merged.contributingSources == 0b11u);
    FGB_CHECK(merged.channelStamps[channel::index(channel::kEyes)].source == kInvalidSource);
}

FGB_TEST(merge_default_keeps_avatar_eyes_on_eye_source) {
    FakeSource eyes{"eye"};
    FakeSource gaze{"gaze"};

    SourceMerger merger;
    MergePolicy policy;
    policy.eye_source  = "eye";
    policy.gaze_source = "gaze";
    merger.set_policy(policy);
    merger.add(&eyes);
    merger.add(&gaze);

    eyes.push(eyes_pair_frame(0.4f, 0.4f, -10.0f, -10.0f));
    gaze.push(gaze_frame(20.0f));

    FaceFrame merged;
    FGB_CHECK(merger.poll(10'000'000ULL, merged));
    // 默认 avatar_eye_from_gaze=false：头像方向仍是 Eyes 源的 -10°，渲染注视仍是 20°
    FGB_CHECK_NEAR(merged.eyes.left.gazePitchYawDeg.y, -10.0, 1e-4);
    FGB_CHECK(merged.eyes.left.gazeSource == 1);
    FGB_CHECK_NEAR(merged.gaze.combinedDirLocal.x, std::sin(20.0f * kDegToRad), 1e-5);
    FGB_CHECK(merged.channelStamps[channel::index(channel::kEyes)].source == 1);
}

FGB_TEST(merge_avatar_eye_ignores_stale_gaze_source) {
    FakeSource eyes{"eye"};
    FakeSource gaze{"gaze"};

    SourceMerger merger;
    MergePolicy policy;
    policy.eye_source           = "eye";
    policy.gaze_source          = "gaze";
    policy.avatar_eye_from_gaze = true;
    policy.gaze_stale_ns        = 100'000'000ULL;
    policy.eye_stale_ns         = 250'000'000ULL;
    merger.set_policy(policy);
    merger.add(&eyes);
    merger.add(&gaze);

    eyes.push(eyes_pair_frame(0.5f, 0.5f, -10.0f, -10.0f));
    gaze.push(gaze_frame(20.0f));
    FaceFrame merged;
    FGB_CHECK(merger.poll(1'000'000'000ULL, merged));
    FGB_CHECK_NEAR(merged.eyes.left.gazePitchYawDeg.y, 20.0, 1e-4);

    // 150 ms 后注视过期、眼睛仍新鲜：头像不能继续用已过期的注视方向
    eyes.push(eyes_pair_frame(0.5f, 0.5f, -10.0f, -10.0f));
    FGB_CHECK(merger.poll(1'150'000'000ULL, merged));
    FGB_CHECK(!merged.has_channel(channel::kGaze));
    FGB_CHECK_NEAR(merged.eyes.left.gazePitchYawDeg.y, -10.0, 1e-4);
    FGB_CHECK(merged.eyes.left.gazeSource == 1);
}

FGB_TEST(merge_gaze_sides_can_come_from_different_sources) {
    FakeSource left{"left"};
    FakeSource right{"right"};

    SourceMerger merger;
    MergePolicy policy;
    policy.gaze_left_source  = "left";
    policy.gaze_right_source = "right";
    merger.set_policy(policy);
    merger.add(&left);
    merger.add(&right);

    left.push(gaze_frame(10.0f));
    right.push(gaze_frame(-10.0f));

    FaceFrame merged;
    FGB_CHECK(merger.poll(10'000'000ULL, merged));
    FGB_CHECK_NEAR(merged.gaze.left.dirLocal.x, std::sin(10.0f * kDegToRad), 1e-5);
    FGB_CHECK_NEAR(merged.gaze.right.dirLocal.x, std::sin(-10.0f * kDegToRad), 1e-5);
    FGB_CHECK(merged.gaze.left.source == 1);
    FGB_CHECK(merged.gaze.right.source == 2);
    // 左右来自不同源时，合并方向由两侧平均得到
    FGB_CHECK_NEAR(merged.gaze.combinedDirLocal.x, 0.0, 1e-5);
    FGB_CHECK(merged.channelStamps[channel::index(channel::kGaze)].source == kInvalidSource);
}

FGB_TEST(merge_attribute_openness_and_pupil_can_come_from_different_sources) {
    FakeSource openness{"open"};
    FakeSource pupil{"pupil"};

    SourceMerger merger;
    MergePolicy policy;
    policy.attribute_sources = {
        {"left-openness", {"open"}}, {"right-openness", {"open"}},
        {"left-pupil", {"pupil"}},   {"right-pupil", {"pupil"}},
    };
    merger.set_policy(policy);
    merger.add(&openness);
    merger.add(&pupil);

    openness.push(eye_attrs_frame(true, true, false, false, 0.2f, 0.8f, 0.0f, 0.0f,
                                  -10.0f, -10.0f, false));
    pupil.push(eye_attrs_frame(false, false, true, true, 0.0f, 0.0f, 4.0f, 5.0f, 0.0f, 0.0f,
                               false));

    FaceFrame merged;
    FGB_CHECK(merger.poll(10'000'000ULL, merged));
    FGB_CHECK_NEAR(merged.eyes.left.openness, 0.2, 1e-6);
    FGB_CHECK_NEAR(merged.eyes.right.openness, 0.8, 1e-6);
    FGB_CHECK_NEAR(merged.eyes.left.pupilDiameterMm, 4.0, 1e-6);
    FGB_CHECK_NEAR(merged.eyes.right.pupilDiameterMm, 5.0, 1e-6);
    FGB_CHECK(merged.eyes.left.opennessSource == 1);
    FGB_CHECK(merged.eyes.left.pupilSource == 2);
    FGB_CHECK(merged.eyes.right.opennessSource == 1);
    FGB_CHECK(merged.eyes.right.pupilSource == 2);
    FGB_CHECK(merged.contributingSources == 0b11u);
    FGB_CHECK(merged.channelStamps[channel::index(channel::kEyes)].source == kInvalidSource);
}

FGB_TEST(merge_attribute_ordered_list_skips_stale_source) {
    FakeSource first{"first"};
    FakeSource second{"second"};

    SourceMerger merger;
    MergePolicy policy;
    policy.eye_stale_ns = 100'000'000ULL;
    policy.attribute_sources = {{"left-openness", {"first", "second"}}};
    merger.set_policy(policy);
    merger.add(&first);
    merger.add(&second);

    first.push(eye_attrs_frame(true, true, false, false, 0.2f, 0.2f, 0, 0, 0, 0, false));
    second.push(eye_attrs_frame(true, true, false, false, 0.4f, 0.4f, 0, 0, 0, 0, false));
    FaceFrame merged;
    FGB_CHECK(merger.poll(1'000'000'000ULL, merged));
    FGB_CHECK_NEAR(merged.eyes.left.openness, 0.2, 1e-6);

    // 150 ms 后第一个源过期，第二仍在列表里且新鲜；属性不能继续用 first。
    second.push(eye_attrs_frame(true, true, false, false, 0.6f, 0.6f, 0, 0, 0, 0, false));
    FGB_CHECK(merger.poll(1'150'000'000ULL, merged));
    FGB_CHECK_NEAR(merged.eyes.left.openness, 0.6, 1e-6);
    FGB_CHECK(merged.eyes.left.opennessSource == 2);
}

FGB_TEST(merge_attribute_can_fuse_sources_by_confidence) {
    FakeSource high{"high"};
    FakeSource low{"low"};

    SourceMerger merger;
    MergePolicy policy;
    policy.fuse_attributes = true;
    policy.attribute_sources = {{"left-openness", {"high", "low"}}};
    merger.set_policy(policy);
    merger.add(&high);
    merger.add(&low);

    FaceFrame high_frame =
        eye_attrs_frame(true, true, false, false, 0.2f, 0.2f, 0, 0, 0, 0, false);
    high_frame.eyes.left.opennessConf = 0.9f;
    high_frame.eyes.right.opennessConf = 0.9f;
    FaceFrame low_frame =
        eye_attrs_frame(true, true, false, false, 0.8f, 0.8f, 0, 0, 0, 0, false);
    low_frame.eyes.left.opennessConf = 0.1f;
    low_frame.eyes.right.opennessConf = 0.1f;
    high.push(high_frame);
    low.push(low_frame);

    FaceFrame merged;
    FGB_CHECK(merger.poll(10'000'000ULL, merged));
    FGB_CHECK_NEAR(merged.eyes.left.openness, 0.26, 1e-5);
    FGB_CHECK(merged.eyes.left.opennessSource == kInvalidSource);
    FGB_CHECK_NEAR(merged.eyes.left.opennessConf, 0.5, 1e-5);
    FGB_CHECK(merged.contributingSources == 0b11u);
}

FGB_TEST(merge_attribute_gaze_can_come_from_another_source) {
    FakeSource first{"first"};
    FakeSource second{"second"};

    SourceMerger merger;
    MergePolicy policy;
    policy.attribute_sources = {{"left-openness", {"first"}}, {"left-gaze", {"second"}}};
    merger.set_policy(policy);
    merger.add(&first);
    merger.add(&second);

    first.push(eye_attrs_frame(true, true, false, false, 0.3f, 0.3f, 0, 0, -10.0f, -10.0f,
                               true));
    second.push(eye_attrs_frame(true, true, false, false, 0.9f, 0.9f, 0, 0, 20.0f, 20.0f,
                                true));

    FaceFrame merged;
    FGB_CHECK(merger.poll(10'000'000ULL, merged));
    FGB_CHECK_NEAR(merged.eyes.left.openness, 0.3, 1e-6);
    FGB_CHECK(merged.eyes.left.opennessSource == 1);
    FGB_CHECK(merged.eyes.left.gazeSource == 2);
    FGB_CHECK_NEAR(merged.eyes.left.gazePitchYawDeg.y, 20.0, 1e-4);
}

FGB_TEST(merge_attribute_missing_source_clears_only_that_field) {
    FakeSource only{"only"};

    SourceMerger merger;
    MergePolicy policy;
    policy.attribute_sources = {{"left-openness", {"missing"}}};
    merger.set_policy(policy);
    merger.add(&only);

    only.push(eye_attrs_frame(true, true, true, true, 0.5f, 0.6f, 4.0f, 4.5f, 0, 0, false));

    FaceFrame merged;
    FGB_CHECK(merger.poll(10'000'000ULL, merged));
    // 显式列表里没有可用源：left openness 必须空，而不是偷偷回退到 only。
    FGB_CHECK(!merged.eyes.left.opennessValid);
    // 未配置的 pupil/right 字段仍按整眼回退。
    FGB_CHECK(merged.eyes.left.pupilValid);
    FGB_CHECK_NEAR(merged.eyes.left.pupilDiameterMm, 4.0, 1e-6);
    FGB_CHECK(merged.eyes.right.opennessValid);
    FGB_CHECK_NEAR(merged.eyes.right.openness, 0.6, 1e-6);
}

FGB_TEST(engine_uses_gaze_channel_for_render_gaze) {
    Engine engine;
    auto face = std::make_unique<FakeSource>("face");
    auto gaze = std::make_unique<FakeSource>("gaze");
    FakeSource* face_raw = face.get();
    FakeSource* gaze_raw = gaze.get();
    engine.add_source(std::move(face));
    engine.add_source(std::move(gaze));

    face_raw->push(expression_frame(0.7f));
    gaze_raw->push(gaze_frame(18.0f));
    engine.tick_once(1'000'000ULL);

    const FaceFrame latest = engine.latest();
    FGB_CHECK(latest.has_channel(channel::kExpression));
    FGB_CHECK(latest.has_channel(channel::kGaze));
    FGB_CHECK(!latest.eyes.any_valid()); // 头像眼睛没有被注视源伪造

    const GazeState render = engine.current_gaze();
    FGB_CHECK(render.valid);
    FGB_CHECK_NEAR(render.dir_local.x, std::sin(18.0f * kDegToRad), 1e-5);
    FGB_CHECK(render.left_valid && render.right_valid);
    FGB_CHECK(render.point2d_valid);
    FGB_CHECK(render.capture_ts_ns == 5000);
    FGB_CHECK(render.source == 2); // 第二个注册的源
    // 逐眼角度由引擎反算：与合并方向同一套 sign 约定（pitch 正 = 上，yaw 正 = 右）
    FGB_CHECK_NEAR(render.left_yaw_deg, 18.0, 1e-3);
    FGB_CHECK_NEAR(render.right_yaw_deg, 18.0, 1e-3);
    FGB_CHECK_NEAR(render.left_pitch_deg, 0.0, 1e-3);
}

FGB_TEST(engine_reports_per_eye_angles_separately) {
    Engine engine;
    auto source = std::make_unique<FakeSource>("gaze");
    FakeSource* raw = source.get();
    engine.add_source(std::move(source));

    FaceFrame frame = FaceFrame::neutral();
    frame.add_channel(channel::kGaze);
    frame.gaze.left.valid       = true;
    frame.gaze.left.dirLocal    = pitch_yaw_deg_to_dir(5.0f, 10.0f);  // 向外偏 10°
    frame.gaze.right.valid      = true;
    frame.gaze.right.dirLocal   = pitch_yaw_deg_to_dir(5.0f, -10.0f); // 另一侧 10°
    frame.gaze.combinedValid    = true;
    frame.gaze.combinedDirLocal = pitch_yaw_deg_to_dir(5.0f, 0.0f);
    raw->push(frame);
    engine.tick_once(1'000'000ULL);

    GazeState gaze = engine.current_gaze();
    FGB_CHECK(gaze.valid);
    FGB_CHECK(gaze.left_valid && gaze.right_valid);
    FGB_CHECK_NEAR(gaze.left_pitch_deg, 5.0, 1e-3);
    FGB_CHECK_NEAR(gaze.right_pitch_deg, 5.0, 1e-3);
    FGB_CHECK_NEAR(gaze.left_yaw_deg, 10.0, 1e-3);
    FGB_CHECK_NEAR(gaze.right_yaw_deg, -10.0, 1e-3);
    // 两眼对称外展时合并方向在正中：只看合并值会以为"没有聚散"
    FGB_CHECK_NEAR(gaze.yaw_deg, 0.0, 1e-3);

    // 单眼掉线：左眼失效必须如实标记，不能让界面拿旧方向继续画
    FaceFrame lossy = FaceFrame::neutral();
    lossy.add_channel(channel::kGaze);
    lossy.gaze.right.valid      = true;
    lossy.gaze.right.dirLocal   = pitch_yaw_deg_to_dir(0.0f, 4.0f);
    lossy.gaze.combinedValid    = true;
    lossy.gaze.combinedDirLocal = lossy.gaze.right.dirLocal;
    raw->push(lossy);
    engine.tick_once(2'000'000ULL);

    gaze = engine.current_gaze();
    FGB_CHECK(gaze.valid);
    FGB_CHECK(!gaze.left_valid);
    FGB_CHECK(gaze.right_valid);
    FGB_CHECK_NEAR(gaze.right_yaw_deg, 4.0, 1e-3);
    FGB_CHECK_NEAR(gaze.left_yaw_deg, 0.0, 1e-6); // 无效眼的残值不参与显示
}

FGB_TEST(engine_per_eye_angles_follow_eyes_fallback) {
    Engine engine;
    auto source = std::make_unique<FakeSource>("eyes");
    FakeSource* raw = source.get();
    engine.add_source(std::move(source));

    FaceFrame frame = FaceFrame::neutral();
    frame.eyes.left.valid           = true;
    frame.eyes.left.gazePitchYawDeg = Vec2{6.0f, 3.0f};
    frame.eyes.left.gazeDirLocal    = pitch_yaw_deg_to_dir(6.0f, 3.0f);
    frame.eyes.right.valid           = true;
    frame.eyes.right.gazePitchYawDeg = Vec2{-2.0f, -7.0f};
    frame.eyes.right.gazeDirLocal    = pitch_yaw_deg_to_dir(-2.0f, -7.0f);
    frame.add_channel(channel::kEyes);
    raw->push(frame);
    engine.tick_once(1'000'000ULL);

    const GazeState gaze = engine.current_gaze();
    FGB_CHECK(gaze.valid);
    // 回退到 eyes 时读来源缓存的角度，和合并角度同源
    FGB_CHECK_NEAR(gaze.left_pitch_deg, 6.0, 1e-4);
    FGB_CHECK_NEAR(gaze.left_yaw_deg, 3.0, 1e-4);
    FGB_CHECK_NEAR(gaze.right_pitch_deg, -2.0, 1e-4);
    FGB_CHECK_NEAR(gaze.right_yaw_deg, -7.0, 1e-4);
    FGB_CHECK_NEAR(gaze.pitch_deg, 2.0, 1e-4);
    FGB_CHECK_NEAR(gaze.yaw_deg, -2.0, 1e-4);
}

// 只给 2D 点的源（PICO Connect 私有通道）：点必须透传，但方向不能拿默认 -Z 冒充。
// 以前这种帧会被整帧丢弃，模块只能伪造方向，副作用是永远"注视中"。
FGB_TEST(engine_points_only_source_does_not_fake_direction) {
    Engine engine;
    auto source = std::make_unique<FakeSource>("gaze");
    FakeSource* raw = source.get();
    engine.add_source(std::move(source));

    FaceFrame frame = FaceFrame::neutral();
    frame.add_channel(channel::kGaze);
    frame.gaze.left.pointValid  = true;
    frame.gaze.right.pointValid = true;
    frame.gaze.pointsValid      = true;
    frame.gaze.left.point2d     = Vec2{0.82f, 0.90f};
    frame.gaze.right.point2d    = Vec2{0.91f, 0.90f};
    frame.gaze.left.confidence  = 0.9f;
    frame.gaze.right.confidence = 0.9f;
    frame.gaze.confidence       = 0.9f;
    frame.gaze.captureTsNs      = 5000;

    for (int i = 0; i <= 24; ++i) {
        raw->push(frame);
        engine.tick_once(1'000'000ULL + static_cast<std::uint64_t>(i) * 8'333'333ULL);
    }

    const GazeState gaze = engine.current_gaze();
    FGB_CHECK(!gaze.valid); // valid 只表示"有合并方向"
    FGB_CHECK(!gaze.left_valid && !gaze.right_valid);
    FGB_CHECK(gaze.point2d_valid);
    FGB_CHECK(gaze.left_point_valid && gaze.right_point_valid);
    FGB_CHECK_NEAR(gaze.left_point_2d.x, 0.82, 1e-6);
    FGB_CHECK_NEAR(gaze.right_point_2d.x, 0.91, 1e-6);
    FGB_CHECK_NEAR(gaze.confidence, 0.9, 1e-6);
    // 恒定的 -Z 不能再被算成"一直在注视"
    FGB_CHECK(!gaze.fixation);
    FGB_CHECK_NEAR(gaze.fixation_ms, 0.0, 1e-3);
    // 通道本身仍在：有点也算这一路在出数据
    FGB_CHECK(engine.latest().has_channel(channel::kGaze));
}

FGB_TEST(engine_single_source_is_passthrough) {
    Engine engine;
    auto source = std::make_unique<FakeSource>("solo");
    FakeSource* raw = source.get();
    engine.add_source(std::move(source));

    FaceFrame frame = expression_frame(0.42f);
    frame.eyes      = eyes_frame(3.0f).eyes;
    frame.hasHeadPose  = true;
    frame.headPosLocal = Vec3{0.0f, 0.0f, 1.0f};
    frame.add_channel(channel::kEyes);
    frame.add_channel(channel::kHead);
    raw->push(frame);

    engine.tick_once(1'000'000ULL);
    const FaceFrame latest = engine.latest();
    FGB_CHECK_NEAR(latest.shape(Shape::JawOpen), 0.42, 1e-6);
    FGB_CHECK(latest.eyes.left.valid);
    FGB_CHECK(latest.hasHeadPose);
    FGB_CHECK(latest.captureTsNs == 1000); // 单源时时间戳原样带过去（frame 里写的是 1000）
    FGB_CHECK(engine.stats().frames_in == 1);
}
