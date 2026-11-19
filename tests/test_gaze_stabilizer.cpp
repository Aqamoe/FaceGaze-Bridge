// 稳定化层：眨眼保持。验证"闭眼期间注视点不跟着眼动向量跳"，以及各种该放行的情形。
#include "core/model/gaze_projection.hpp"
#include "core/process/gaze_stabilizer.hpp"
#include "core/process/one_euro.hpp"
#include "engine/engine.hpp"

#include "test_framework.hpp"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>

using namespace fgb;

namespace {

// 120 Hz：一次眨眼的完整波形（约 120 ms）能落到 14 帧里
constexpr std::uint64_t kTick = 8'333'333ULL;

FaceFrame eye_frame(float openness, bool openness_valid = true) {
    FaceFrame frame;
    frame.add_channel(channel::kEyes);
    frame.eyes.left.valid         = true;
    frame.eyes.left.openness      = openness;
    frame.eyes.left.opennessValid = openness_valid;
    frame.eyes.right              = frame.eyes.left;
    return frame;
}

// 一帧"已经落到视口坐标"的注视：方向/角度/点保持自洽（与 compute_gaze 的产出同构）
GazeState gaze_at(float pitch_deg, float yaw_deg, Vec2 point = {0.5f, 0.5f}) {
    GazeState gaze;
    gaze.valid        = true;
    gaze.dir_space    = GazeDirSpace::source;
    gaze.space        = GazeSpace::viewport;
    gaze.point_viewport = true;
    gaze.dir_local    = pitch_yaw_deg_to_dir(pitch_deg, yaw_deg);
    const Vec2 py     = dir_to_pitch_yaw_deg(gaze.dir_local);
    gaze.pitch_deg    = py.x;
    gaze.yaw_deg      = py.y;
    gaze.left_valid   = true;
    gaze.right_valid  = true;
    gaze.left_dir_local  = gaze.dir_local;
    gaze.right_dir_local = gaze.dir_local;
    gaze.left_pitch_deg  = py.x;
    gaze.left_yaw_deg    = py.y;
    gaze.right_pitch_deg = py.x;
    gaze.right_yaw_deg   = py.y;
    gaze.left_point_valid  = true;
    gaze.right_point_valid = true;
    gaze.point2d_valid     = true;
    gaze.left_point_2d     = point;
    gaze.right_point_2d    = point;
    return gaze;
}

// 手动喂帧的假源：poll 只吐一帧，吐完就空（与 test_merge 同一手法）
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

// 走 eyes 通道（眼睛表情反算注视）的一帧：openness + 俯仰角就是全部输入
FaceFrame eyes_frame(float openness, float pitch_deg, std::uint64_t ts) {
    FaceFrame frame = FaceFrame::neutral();
    frame.add_channel(channel::kEyes);
    frame.eyes.left.valid          = true;
    frame.eyes.right.valid         = true;
    frame.eyes.left.openness       = openness;
    frame.eyes.right.openness      = openness;
    frame.eyes.left.opennessValid  = true;
    frame.eyes.right.opennessValid = true;
    frame.eyes.left.gazeValid      = true;
    frame.eyes.right.gazeValid     = true;
    frame.eyes.left.gazeConf       = 1.0f;
    frame.eyes.right.gazeConf      = 1.0f;
    frame.eyes.left.gazePitchYawDeg  = Vec2{pitch_deg, 0.0f};
    frame.eyes.right.gazePitchYawDeg = Vec2{pitch_deg, 0.0f};
    frame.eyes.left.gazeDirLocal     = pitch_yaw_deg_to_dir(pitch_deg, 0.0f);
    frame.eyes.right.gazeDirLocal    = frame.eyes.left.gazeDirLocal;
    frame.captureTsNs                = ts;
    return frame;
}

} // namespace

// 眨眼时眼动向量被顶上去（Bell 现象），点必须停在闭眼前的位置；睁开后也要等一下
// 才恢复跟随（睁眼瞬间同样有回弹）。
FGB_TEST(gaze_stabilizer_holds_gaze_through_blink) {
    GazeStabilizer stabilizer;
    std::uint64_t now = 1'000'000'000;

    // 睁眼看正前方，建立"闭眼前"的参照
    for (int i = 0; i < 5; ++i) {
        GazeState gaze = gaze_at(0.0f, 0.0f);
        now += kTick;
        stabilizer.apply(gaze, eye_frame(1.0f), now);
        FGB_CHECK(!gaze.blink_hold);
    }

    // 闭眼：来源报 +20°（上翻）、点也跟着上移。这一帧必须被换回闭眼前的值
    GazeState closed = gaze_at(20.0f, 0.0f, Vec2{0.5f, 0.2f});
    now += kTick;
    stabilizer.apply(closed, eye_frame(0.10f), now);
    FGB_CHECK(closed.blink_hold);
    FGB_CHECK_NEAR(closed.left_pitch_deg, 0.0, 1e-3);
    FGB_CHECK_NEAR(closed.right_pitch_deg, 0.0, 1e-3);
    FGB_CHECK_NEAR(closed.pitch_deg, 0.0, 1e-3); // 合并行也必须跟着重算
    FGB_CHECK_NEAR(closed.left_point_2d.y, 0.5, 1e-4);
    FGB_CHECK(closed.left_valid);
    FGB_CHECK(closed.point2d_valid);

    // 闭眼期间一直保持（来源越翻越高也不动）
    for (int i = 0; i < 5; ++i) {
        GazeState held = gaze_at(25.0f, 0.0f, Vec2{0.5f, 0.1f});
        now += kTick;
        stabilizer.apply(held, eye_frame(0.05f), now);
        FGB_CHECK(held.blink_hold);
        FGB_CHECK_NEAR(held.left_pitch_deg, 0.0, 1e-3);
        FGB_CHECK_NEAR(held.left_point_2d.y, 0.5, 1e-4);
    }

    // 刚睁眼（recover 窗口内）：仍然保持，吸收睁眼回弹
    GazeState reopen = gaze_at(15.0f, 0.0f, Vec2{0.5f, 0.3f});
    now += kTick;
    stabilizer.apply(reopen, eye_frame(0.9f), now);
    FGB_CHECK(reopen.blink_hold);
    FGB_CHECK_NEAR(reopen.left_pitch_deg, 0.0, 1e-3);

    // 睁开后真实值一直停在 12°（离保持值 12° > handoff）：最多等 tail_ms（250）就交接，
    // 而且交接是滑过去的（blend），不是一跳。
    bool released = false;
    for (int i = 0; i < 60 && !released; ++i) {
        GazeState waiting = gaze_at(12.0f, 0.0f, Vec2{0.5f, 0.3f});
        now += kTick;
        stabilizer.apply(waiting, eye_frame(0.9f), now);
        if (waiting.blink_hold) {
            FGB_CHECK_NEAR(waiting.left_pitch_deg, 0.0, 1e-3); // 保持期间端坐不动
            FGB_CHECK_NEAR(waiting.left_point_2d.y, 0.5, 1e-4);
        } else {
            released = true;
            // 交接的第一帧仍停在保持值上：过渡从 t=0 开始，不会看到"跳一下"
            FGB_CHECK_NEAR(waiting.left_pitch_deg, 0.0, 1e-2);
            FGB_CHECK_NEAR(waiting.left_point_2d.y, 0.5, 1e-2);
        }
    }
    FGB_CHECK(released);

    // 过渡走完：跟随真实值
    GazeState settled = gaze_at(12.0f, 0.0f, Vec2{0.5f, 0.3f});
    for (int i = 0; i < 30; ++i) {
        settled = gaze_at(12.0f, 0.0f, Vec2{0.5f, 0.3f});
        now += kTick;
        stabilizer.apply(settled, eye_frame(0.9f), now);
    }
    FGB_CHECK_NEAR(settled.left_pitch_deg, 12.0, 1e-3);
    FGB_CHECK_NEAR(settled.left_point_2d.y, 0.3, 1e-3);
}

// 这就是用户报的残余：睁开之后来源先往下沉一点（比原处低 3°）再回来。
// 固定 60 ms 的恢复窗口会把这"沉一下"原样放出去；现在要等它回到原处才交接。
FGB_TEST(gaze_stabilizer_waits_out_post_blink_dip) {
    GazeStabilizer stabilizer;
    std::uint64_t now = 1'000'000'000;

    for (int i = 0; i < 5; ++i) {
        GazeState gaze = gaze_at(0.0f, 0.0f);
        now += kTick;
        stabilizer.apply(gaze, eye_frame(1.0f), now);
        FGB_CHECK(!gaze.blink_hold);
    }

    // 闭眼（Bell 上翻）
    for (int i = 0; i < 4; ++i) {
        GazeState closed = gaze_at(20.0f, 0.0f);
        now += kTick;
        stabilizer.apply(closed, eye_frame(0.05f), now);
        FGB_CHECK(closed.blink_hold);
    }

    // 睁开，但来源回弹到 −3°（比原处低），持续约 100 ms
    for (int i = 0; i < 12; ++i) {
        GazeState dip = gaze_at(-3.0f, 0.0f);
        now += kTick;
        stabilizer.apply(dip, eye_frame(0.9f), now);
        FGB_CHECK(dip.blink_hold);                       // 还没回到原处：继续保持
        FGB_CHECK_NEAR(dip.left_pitch_deg, 0.0, 1e-3);   // 点不许往下沉
    }

    // 回到原处附近（0.5° < handoff 2.5°）：等 recover 走完就交接，不必等满 tail
    bool released = false;
    std::uint64_t since_back = 0;
    for (int i = 0; i < 20 && !released; ++i) {
        GazeState back = gaze_at(0.5f, 0.0f);
        now += kTick;
        since_back += kTick;
        stabilizer.apply(back, eye_frame(0.9f), now);
        if (!back.blink_hold) {
            released = true;
        }
    }
    FGB_CHECK(released);
    FGB_CHECK(since_back <= 20ULL * kTick); // 略大于 recover（60 ms）即可，不用等到 250 ms
}

// 交接前差得很少（1° 以内）时，recover 一过就交，不必等满 tail：正常眨眼后立刻能跟随。
FGB_TEST(gaze_stabilizer_hands_off_promptly_when_back_to_origin) {
    GazeStabilizer stabilizer;
    std::uint64_t now = 1'000'000'000;

    for (int i = 0; i < 5; ++i) {
        GazeState gaze = gaze_at(0.0f, 0.0f);
        now += kTick;
        stabilizer.apply(gaze, eye_frame(1.0f), now);
    }
    GazeState closed = gaze_at(18.0f, 0.0f);
    now += kTick;
    stabilizer.apply(closed, eye_frame(0.05f), now);
    FGB_CHECK(closed.blink_hold);

    int ticks = 0;
    bool released = false;
    while (ticks < 20 && !released) {
        GazeState back = gaze_at(0.8f, 0.0f);
        now += kTick;
        ++ticks;
        stabilizer.apply(back, eye_frame(0.9f), now);
        released = !back.blink_hold;
    }
    FGB_CHECK(released);
    FGB_CHECK(ticks <= 10); // recover(60 ms) + 1 帧，不需要等到 tail(250 ms)
}

// 单眼眨不能连坐另一眼：受伤/单眼遮挡时另一眼必须继续跟手。
FGB_TEST(gaze_stabilizer_keeps_eyes_independent) {
    GazeStabilizer stabilizer;
    std::uint64_t now = 1'000'000'000;

    const auto open_both = [&]() {
        GazeState gaze = gaze_at(0.0f, 0.0f);
        now += kTick;
        stabilizer.apply(gaze, eye_frame(1.0f), now);
    };

    for (int i = 0; i < 4; ++i) {
        open_both();
    }

    FaceFrame frame           = eye_frame(1.0f);
    frame.eyes.left.openness  = 0.05f; // 只有左眼闭上
    frame.eyes.right.openness = 1.0f;
    GazeState gaze            = gaze_at(20.0f, 0.0f);
    now += kTick;
    stabilizer.apply(gaze, frame, now);
    FGB_CHECK(gaze.blink_hold);
    FGB_CHECK_NEAR(gaze.left_pitch_deg, 0.0, 1e-3);  // 左眼保持
    FGB_CHECK_NEAR(gaze.right_pitch_deg, 20.0, 1e-3); // 右眼照常跟随
}

// 开着一欧滤波的用户：openness 的谷被抹平（一次眨眼只掉到 0.45），光看"低于阈值"
// 永远判不出闭眼。快速下坠判据要能兜住这种情况，否则这段保护在滤波开启时形同不存在。
FGB_TEST(gaze_stabilizer_detects_filtered_shallow_dip) {
    GazeStabilizer stabilizer;
    std::uint64_t now = 1'000'000'000;

    for (int i = 0; i < 6; ++i) {
        GazeState gaze = gaze_at(0.0f, 0.0f);
        now += kTick;
        stabilizer.apply(gaze, eye_frame(0.95f), now);
        FGB_CHECK(!gaze.blink_hold);
    }

    // 谷只有 0.45（高于 blink_enter），但在 150 ms 内掉了 0.5
    GazeState dipped = gaze_at(22.0f, 0.0f);
    now += kTick;
    stabilizer.apply(dipped, eye_frame(0.45f), now);
    FGB_CHECK(dipped.blink_hold);
    FGB_CHECK_NEAR(dipped.left_pitch_deg, 0.0, 1e-3);

    // 闭眼期间一直保持：滤波后的 openness 在谷底徘徊，不能被"窗口水位自己掉下来"骗成睁开
    for (int i = 0; i < 20; ++i) {
        GazeState held = gaze_at(24.0f, 0.0f);
        now += kTick;
        stabilizer.apply(held, eye_frame(0.45f), now);
        FGB_CHECK(held.blink_hold);
        FGB_CHECK_NEAR(held.left_pitch_deg, 0.0, 1e-3);
    }

    // 真睁开（回到 0.93）、而且看向离原处 5° 的地方：等 tail 到点后滑过去
    bool released = false;
    GazeState last = gaze_at(5.0f, 0.0f);
    for (int i = 0; i < 60; ++i) {
        last = gaze_at(5.0f, 0.0f);
        now += kTick;
        stabilizer.apply(last, eye_frame(0.93f), now);
        released = !last.blink_hold;
        if (released) {
            break;
        }
        FGB_CHECK_NEAR(last.left_pitch_deg, 0.0, 1e-3);
    }
    FGB_CHECK(released);
    for (int i = 0; i < 20; ++i) {
        last = gaze_at(5.0f, 0.0f);
        now += kTick;
        stabilizer.apply(last, eye_frame(0.93f), now);
    }
    FGB_CHECK_NEAR(last.left_pitch_deg, 5.0, 1e-3);
}

// 慢慢眯眼（1 秒内从 1.0 降到 0.50）不是眨眼：不能把注视冻住，否则眯眼时看哪儿都不动。
FGB_TEST(gaze_stabilizer_ignores_slow_squint) {
    GazeStabilizer stabilizer;
    std::uint64_t now = 1'000'000'000;

    float openness = 1.0f;
    for (int i = 0; i < 4; ++i) {
        GazeState gaze = gaze_at(0.0f, 0.0f);
        now += kTick;
        stabilizer.apply(gaze, eye_frame(openness), now);
    }
    for (int i = 0; i < 125; ++i) { // 0.004/帧 ≈ 1 秒降 0.5
        openness = std::max(0.50f, openness - 0.004f);
        GazeState gaze = gaze_at(0.0f, 0.0f);
        now += kTick;
        stabilizer.apply(gaze, eye_frame(openness), now);
        FGB_CHECK(!gaze.blink_hold);
    }
}

// 眼睑信号缺失（纯注视模块/2D-only 源）时不能判眨眼：一判就会把正常数据冻住。
FGB_TEST(gaze_stabilizer_ignores_missing_openness) {
    GazeStabilizer stabilizer;
    std::uint64_t now = 1'000'000'000;

    for (int i = 0; i < 6; ++i) {
        GazeState gaze = gaze_at(10.0f, 0.0f);
        now += kTick;
        // openness 数值是 0（看起来"闭着"），但来源明确表示不提供
        stabilizer.apply(gaze, eye_frame(0.0f, /*openness_valid=*/false), now);
        FGB_CHECK(!gaze.blink_hold);
        FGB_CHECK_NEAR(gaze.left_pitch_deg, 10.0, 1e-3);
    }
}

// 来源的"全睁"不等于 1.0（眼睑校准扣过基线），参照要按观测到的最大 openness 自适应，
// 否则永远找不到参照 → 眨眼保持形同不存在。
FGB_TEST(gaze_stabilizer_adapts_reference_to_open_level) {
    GazeStabilizer stabilizer;
    std::uint64_t now = 1'000'000'000;

    for (int i = 0; i < 4; ++i) {
        GazeState gaze = gaze_at(0.0f, 0.0f);
        now += kTick;
        stabilizer.apply(gaze, eye_frame(0.70f), now); // 全睁只有 0.70
        FGB_CHECK(!gaze.blink_hold);
    }

    GazeState closed = gaze_at(25.0f, 0.0f);
    now += kTick;
    stabilizer.apply(closed, eye_frame(0.10f), now);
    FGB_CHECK(closed.blink_hold);
    FGB_CHECK_NEAR(closed.left_pitch_deg, 0.0, 1e-3);
}

// 眼一直闭着（睡着、眼睑信号坏掉）：保持到上限就放行，且不能每帧重新进保持。
FGB_TEST(gaze_stabilizer_caps_hold_time) {
    GazeStabilizer stabilizer;
    std::uint64_t now = 1'000'000'000;

    for (int i = 0; i < 4; ++i) {
        GazeState gaze = gaze_at(0.0f, 0.0f);
        now += kTick;
        stabilizer.apply(gaze, eye_frame(1.0f), now);
    }

    bool released = false;
    for (int i = 0; i < 260; ++i) { // 约 2.2 s，超过 1.5 s 上限
        GazeState gaze = gaze_at(30.0f, 0.0f);
        now += kTick;
        stabilizer.apply(gaze, eye_frame(0.02f), now);
        if (!gaze.blink_hold) {
            released = true;
            FGB_CHECK_NEAR(gaze.left_pitch_deg, 30.0, 1e-3); // 放行后如实透传
            break;
        }
    }
    FGB_CHECK(released);

    // 仍然闭着：不能因为"还闭着"又从头保持一次
    for (int i = 0; i < 10; ++i) {
        GazeState gaze = gaze_at(30.0f, 0.0f);
        now += kTick;
        stabilizer.apply(gaze, eye_frame(0.02f), now);
        FGB_CHECK(!gaze.blink_hold);
    }

    // 真正睁开过一次之后，下一次眨眼才重新保持
    for (int i = 0; i < 4; ++i) {
        GazeState gaze = gaze_at(0.0f, 0.0f);
        now += kTick;
        stabilizer.apply(gaze, eye_frame(1.0f), now);
    }
    GazeState again = gaze_at(20.0f, 0.0f);
    now += kTick;
    stabilizer.apply(again, eye_frame(0.05f), now);
    FGB_CHECK(again.blink_hold);
    FGB_CHECK_NEAR(again.left_pitch_deg, 0.0, 1e-3);
}

// 关掉开关 = 恒等透传：出问题时要能一键排除这一层。
FGB_TEST(gaze_stabilizer_passthrough_when_disabled) {
    GazeStabilizerConfig config;
    config.blink_hold = false;
    GazeStabilizer stabilizer;
    stabilizer.set_config(config);

    std::uint64_t now = 1'000'000'000;
    for (int i = 0; i < 6; ++i) {
        GazeState gaze = gaze_at(20.0f, 0.0f);
        now += kTick;
        stabilizer.apply(gaze, eye_frame(0.0f), now);
        FGB_CHECK(!gaze.blink_hold);
        FGB_CHECK_NEAR(gaze.left_pitch_deg, 20.0, 1e-3);
    }
}

// 接线（engine.compute_gaze 的顺序）必须真的把这一层跑起来：单测过但没挂到管线上，
// 用户看到的还是跳。这里走完整引擎：输入 → 合并 → 规范化 → 标定（几何投影）→ 稳定化。
FGB_TEST(gaze_stabilizer_wired_into_engine_pipeline) {
    Engine engine;
    engine.set_gaze_projection(GazeProjection::symmetric(1.0f, 1.0f));

    auto source          = std::make_unique<FakeSource>("eyes");
    FakeSource* raw      = source.get();
    engine.add_source(std::move(source));
    FGB_CHECK(engine.start());

    std::uint64_t now = 1'000'000'000;
    const auto tick   = [&](float openness, float pitch_deg) {
        now += kTick;
        raw->push(eyes_frame(openness, pitch_deg, now));
        engine.tick_once(now);
        return engine.current_gaze();
    };

    for (int i = 0; i < 5; ++i) {
        const GazeState gaze = tick(1.0f, 0.0f);
        FGB_CHECK(!gaze.blink_hold);
        FGB_CHECK_NEAR(gaze.left_pitch_deg, 0.0, 1e-3);
    }

    // 眨眼：来源报 +20°，管线必须把它压回闭眼前的 0°，点也不许上移
    const GazeState blinked = tick(0.10f, 20.0f);
    FGB_CHECK(blinked.blink_hold);
    FGB_CHECK(blinked.valid);
    FGB_CHECK_NEAR(blinked.left_pitch_deg, 0.0, 1e-3);
    FGB_CHECK(blinked.left_point_valid);
    FGB_CHECK_NEAR(blinked.left_point_2d.y, 0.5, 1e-3);
    FGB_CHECK(blinked.space != GazeSpace::source); // 几何投影照常给了视口点

    engine.stop();
}

// 一欧元滤波开着时（用户为了稳会把 min_cutoff 调小）：闭眼那段上翻一旦进了滤波器状态，
// 睁开后要用几百毫秒"回味"，表现就是注视点先往下沉一点再回到原处。冻结状态比事后补偿干净。
FGB_TEST(face_filter_freezes_gaze_while_blinking) {
    OneEuroConfig config;
    config.min_cutoff = 0.5f; // 回味最明显的档位
    FaceFilter filter(config);
    filter.set_enabled(true);
    filter.set_blink_gate(0.25f);

    FaceFrame open = eye_frame(1.0f);
    for (int i = 0; i < 6; ++i) {
        filter.process(open);
    }
    FGB_CHECK_NEAR(open.eyes.left.gazePitchYawDeg.x, 0.0, 1e-3);

    // 闭眼 + Bell 上翻 20°：输出冻结在闭眼前，滤波器状态也没被推进
    for (int i = 0; i < 8; ++i) {
        FaceFrame blink = eye_frame(0.05f);
        blink.eyes.left.gazePitchYawDeg  = Vec2{20.0f, 0.0f};
        blink.eyes.right.gazePitchYawDeg = Vec2{20.0f, 0.0f};
        blink.eyes.left.gazeDirLocal     = pitch_yaw_deg_to_dir(20.0f, 0.0f);
        blink.eyes.right.gazeDirLocal    = blink.eyes.left.gazeDirLocal;
        filter.process(blink);
        FGB_CHECK_NEAR(blink.eyes.left.gazePitchYawDeg.x, 0.0, 1e-3);
        // 方向跟着角度走：同一只眼不能出现两套值（ADR-0042 P0）
        FGB_CHECK_NEAR(blink.eyes.left.gazeDirLocal.y, 0.0, 1e-3);
    }

    // 睁开并回到正前方：状态是干净的，第一帧就在原处
    FaceFrame back = eye_frame(1.0f);
    filter.process(back);
    FGB_CHECK_NEAR(back.eyes.left.gazePitchYawDeg.x, 0.0, 1e-3);

    // 对照：关掉闸门时同样的输入会把状态拖走，上面那条不是因为"滤波太慢"才碰巧通过
    FaceFilter ungated(config);
    ungated.set_enabled(true);
    ungated.set_blink_gate(0.0f);
    FaceFrame open2 = eye_frame(1.0f);
    for (int i = 0; i < 6; ++i) {
        ungated.process(open2);
    }
    for (int i = 0; i < 8; ++i) {
        FaceFrame blink = eye_frame(0.05f);
        blink.eyes.left.gazePitchYawDeg = Vec2{20.0f, 0.0f};
        blink.eyes.right                = blink.eyes.left;
        ungated.process(blink);
    }
    FaceFrame back2 = eye_frame(1.0f);
    ungated.process(back2);
    FGB_CHECK(back2.eyes.left.gazePitchYawDeg.x > 1.0f); // 还在回味这段污染
}
