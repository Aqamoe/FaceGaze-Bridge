// 注视管线第三段：稳定化策略（ADR-0042 §1 的 GazeStabilizer）。
//
// 目前只做一件事：**眨眼保持**。用眼睛表情（面捕 look 参数 / 眼镜眼动向量）反算
// 注视点时，眨眼会让点上下跳一次——闭眼过程里眼球按 Bell 现象上翻，多数来源把
// 这段上翻照实报成 eyeLookUp / EyeY，投影到屏幕上就是"眨眼瞬间往上一窜、睁开窜回来"。
// 人眨眼时视觉并没有中断（大脑抑制），屏幕上的点也不该跟着抖。
//
// 做法（逐眼，互不连坐）：
//   1. 保留最近若干帧的逐眼观察（方向/角度/点 + 当时的 openness）；
//   2. 判闭眼看两条，满足任一条就算：
//      a. openness 掉到 blink_enter 以下（来源如实报闭眼）；
//      b. openness 在 baseline_ms 内相对"近期最高水位"掉了 blink_drop 以上。
//      b 是为开着一欧滤波的用户准备的：滤波会把一次 100 ms 的眨眼从 0 抹到 0.4 左右，
//      光看 a 永远判不出来（那等于这段保护在滤波开启时形同不存在）。
//   3. 冻结"最后一次完全睁眼"的那一帧；openness 回到接近水位才解除（迟滞防半睁抖动），
//      解除后再保持 recover_ms，吸收睁眼瞬间的回弹；
//   4. 兜底：连续保持超过 max_hold_ms 就放行，且必须真正睁开过一次才允许再次保持
//      （睡着或眼睑信号坏掉时，不能把注视永久冻在某个位置）。
//
// 为什么放在标定之后：这一段的阈值是"视口比例/角度"，只有标定后才有可解释的量纲；
// 而且消费端只看 GazeState，保持动作放在管线最后一段才对所有 sink 一致。
// 为什么不动 FaceFrame 里的眼动参数：那头像输出（VRChat 眼追）也会跟着停，
// 超出"注视点不要跳"的范围，是另一件事。
#pragma once

#include "core/model/frame.hpp"
#include "core/model/gaze.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace fgb {

struct GazeStabilizerConfig {
    bool blink_hold = true; // 关掉 = 恒等透传（排障用）

    // 判闭眼的水位线：openness 低于 enter 判为闭眼，高于 release 才可能判为重新睁开。
    // 两值之间是迟滞带，半睁/眯眼不会让保持状态每帧翻转（那比不做保持更糟）。
    float blink_enter   = 0.25f;
    float blink_release = 0.55f;

    // 快速下坠判据：baseline_ms 内比"近期最高 openness"掉了这么多也算闭眼。
    float blink_drop = 0.30f;
    float baseline_ms = 150.0f;

    float recover_ms  = 60.0f;   // 睁开后至少再保持多久（吸收睁眼瞬间的回弹）
    // 睁开之后来源往往还有一段"回弹"（先比原处低一点、再回来）。固定计时器只能覆盖
    // 一小段，所以交接条件改成"回到保持值附近才算交接"，等不到就按 tail_ms 到点交接。
    float handoff_deg = 2.5f;    // 与保持值差这么多以内算已回到原处
    float tail_ms     = 250.0f;  // 睁开后最多等这么久（再多就宁可交接，不能一直冻着）
    float blend_ms    = 60.0f;   // 交接过渡时长：把残差抹成缓慢滑动，而不是"一跳"
    float max_hold_ms = 1500.0f; // 单次保持上限：超时放行，不让坏信号永久冻结注视
};

class GazeStabilizer {
public:
    // 32 帧 @120 Hz ≈ 265 ms；30 Hz 来源下也够覆盖一次眨眼的完整波形。
    static constexpr std::size_t kHistory = 32;

    void set_config(const GazeStabilizerConfig& config) noexcept;
    const GazeStabilizerConfig& config() const noexcept { return config_; }

    void reset() noexcept;

    // 就地稳定化。需要 frame 是为了拿逐眼 openness（眨眼判据只有它）。
    void apply(GazeState& gaze, const FaceFrame& frame, std::uint64_t now_ns) noexcept;

private:
    struct Sample {
        std::uint64_t ts_ns = 0;
        float openness      = 1.0f;
        bool dir_valid      = false;
        Vec3 dir{0.0f, 0.0f, -1.0f};
        float pitch_deg     = 0.0f;
        float yaw_deg       = 0.0f;
        bool point_valid    = false;
        Vec2 point{0.0f, 0.0f};
    };

    struct EyeTrack {
        std::array<Sample, kHistory> history{};
        std::size_t head  = 0; // 下一个写入位置
        std::size_t count = 0;

        bool holding                = false;
        std::uint64_t hold_start_ns = 0;
        std::uint64_t reopen_ns     = 0; // 本轮"重新睁开"的时刻；0 = 还没睁
        // 进入保持那一刻的睁眼水位。保持期间窗口里的水位会被闭眼值自己的低值拉下来，
        // 不记住它就会在"还闭着"时误判成已睁开。
        float held_baseline = 1.0f;
        // 保持超时放行后的封锁：眼睑一直闭着时不能每帧重新进保持（等于永久冻结），
        // 必须真正睁开过一次才解除。
        bool blocked = false;
        Sample held{};

        // 交接过渡：保持结束时不直接跳到真实值，而是在 blend_ms 内滑过去。
        // 残差小于一度时看不出来，大于几度时"滑过去"也远好过"跳一下"。
        bool blending                = false;
        std::uint64_t blend_start_ns = 0;
        Sample blend_from{};
    };

    static void push(EyeTrack& track, const Sample& sample) noexcept;
    // 窗口内的最高 openness（"近期睁眼水位"）：判快速下坠与"完全睁眼"都用它。
    static float max_openness(const EyeTrack& track, std::uint64_t now_ns,
                              std::uint64_t window_ns) noexcept;
    // 最近一帧"完全睁眼且有注视数据"的观察；没有则返回 nullptr（不保持，不编位置）。
    static const Sample* last_open(const EyeTrack& track, float floor) noexcept;
    // 两个观察的差异，换算成"角度当量"（点按视口 60° 折算），交接判定用。
    static float deviation_deg(const Sample& a, const Sample& b) noexcept;
    static bool any_data(const Sample& sample) noexcept {
        return sample.dir_valid || sample.point_valid;
    }

    EyeTrack left_;
    EyeTrack right_;
    GazeStabilizerConfig config_{};
};

} // namespace fgb
