#include "core/process/gaze_stabilizer.hpp"

#include <algorithm>
#include <cmath>

namespace fgb {
namespace {

constexpr float kNanosPerMs = 1'000'000.0f;

// 视口坐标 → 角度当量：只用于"交接前差多少"的比较，60° 是常见头显 FOV 的量级，
// 这里不需要精确——阈值本来就是个可调的经验值。
constexpr float kDegPerViewportUnit = 60.0f;

std::uint64_t ms_to_ns(float ms) noexcept {
    if (!(ms > 0.0f)) {
        return 0;
    }
    return static_cast<std::uint64_t>(static_cast<double>(ms) * kNanosPerMs);
}

} // namespace

void GazeStabilizer::set_config(const GazeStabilizerConfig& config) noexcept {
    config_ = config;
    // 手改设置文件也可能把 enter/release 写成反的：迟滞带必须有宽度，否则
    // "半睁"会在闭眼/睁眼之间每帧翻转一次，比不做保持还糟。
    config_.blink_enter   = std::clamp(config_.blink_enter, 0.0f, 0.90f);
    config_.blink_release = std::clamp(config_.blink_release,
                                       std::min(config_.blink_enter + 0.05f, 1.0f), 1.0f);
    config_.blink_drop    = std::clamp(config_.blink_drop, 0.05f, 1.0f);
    config_.baseline_ms   = std::clamp(config_.baseline_ms, 0.0f, 1000.0f);
    config_.recover_ms    = std::clamp(config_.recover_ms, 0.0f, 500.0f);
    config_.handoff_deg   = std::clamp(config_.handoff_deg, 0.0f, 30.0f);
    config_.tail_ms       = std::clamp(config_.tail_ms, config_.recover_ms, 2000.0f);
    config_.blend_ms      = std::clamp(config_.blend_ms, 0.0f, 300.0f);
    config_.max_hold_ms   = std::clamp(config_.max_hold_ms, 0.0f, 10'000.0f);
}

void GazeStabilizer::reset() noexcept {
    left_  = EyeTrack{};
    right_ = EyeTrack{};
}

void GazeStabilizer::push(EyeTrack& track, const Sample& sample) noexcept {
    track.history[track.head] = sample;
    track.head                = (track.head + 1) % kHistory;
    track.count               = std::min(track.count + 1, kHistory);
}

float GazeStabilizer::max_openness(const EyeTrack& track, std::uint64_t now_ns,
                                   std::uint64_t window_ns) noexcept {
    float best = 0.0f;
    bool seen  = false;
    // 历史按时间递增（都在末尾写入），所以从新往旧扫，出了窗口就能停。
    for (std::size_t i = 0; i < track.count; ++i) {
        const std::size_t idx = (track.head + kHistory - 1 - i) % kHistory;
        const Sample& sample  = track.history[idx];
        if (now_ns > sample.ts_ns + window_ns) {
            break;
        }
        best = std::max(best, sample.openness);
        seen = true;
    }
    if (!seen && track.count > 0) {
        // tick 比窗口还慢（例如 8 Hz）：窗口里一帧都没有，退回最新一帧。
        best = track.history[(track.head + kHistory - 1) % kHistory].openness;
    }
    return best;
}

const GazeStabilizer::Sample* GazeStabilizer::last_open(const EyeTrack& track,
                                                        float floor) noexcept {
    // 参照取"最近一次完全睁眼"的观察，而不是最新一帧：最新几帧正是闭眼过程，
    // 眼动向量已经在上翻了，拿它当参照等于把要消掉的那一跳又存了一遍。
    for (std::size_t i = 0; i < track.count; ++i) {
        const std::size_t idx = (track.head + kHistory - 1 - i) % kHistory;
        const Sample& sample  = track.history[idx];
        if (sample.openness >= floor && any_data(sample)) {
            return &sample;
        }
    }
    return nullptr;
}

float GazeStabilizer::deviation_deg(const Sample& a, const Sample& b) noexcept {
    if (a.dir_valid && b.dir_valid) {
        const float d = dot(normalize(a.dir), normalize(b.dir));
        return std::acos(std::clamp(d, -1.0f, 1.0f)) * kRadToDeg;
    }
    if (a.point_valid && b.point_valid) {
        const float dx = a.point.x - b.point.x;
        const float dy = a.point.y - b.point.y;
        return std::sqrt(dx * dx + dy * dy) * kDegPerViewportUnit;
    }
    // 没有可比量（一边只有方向、另一边只有点）：按"已经吻合"处理，早点放行。
    return 0.0f;
}

void GazeStabilizer::apply(GazeState& gaze, const FaceFrame& frame,
                           std::uint64_t now_ns) noexcept {
    if (!config_.blink_hold) {
        if (left_.holding || right_.holding || left_.blending || right_.blending) {
            reset();
        }
        gaze.blink_hold = false;
        return;
    }

    const std::uint64_t recover_ns  = ms_to_ns(config_.recover_ms);
    const std::uint64_t tail_ns     = ms_to_ns(config_.tail_ms);
    const std::uint64_t blend_ns    = ms_to_ns(config_.blend_ms);
    const std::uint64_t hold_cap_ns = ms_to_ns(config_.max_hold_ms);
    const std::uint64_t window_ns   = ms_to_ns(config_.baseline_ms);
    bool held_any                   = false;
    bool touched                    = false;

    for (std::size_t i = 0; i < 2; ++i) {
        const EyeState& eye = i == 0 ? frame.eyes.left : frame.eyes.right;
        EyeTrack& track     = i == 0 ? left_ : right_;

        // 逐眼映射到 GazeState 的字段上：两个分支完全对称，写成引用避免复制逻辑。
        bool& dir_valid   = i == 0 ? gaze.left_valid : gaze.right_valid;
        Vec3& dir         = i == 0 ? gaze.left_dir_local : gaze.right_dir_local;
        float& pitch      = i == 0 ? gaze.left_pitch_deg : gaze.right_pitch_deg;
        float& yaw        = i == 0 ? gaze.left_yaw_deg : gaze.right_yaw_deg;
        bool& point_valid = i == 0 ? gaze.left_point_valid : gaze.right_point_valid;
        Vec2& point       = i == 0 ? gaze.left_point_2d : gaze.right_point_2d;

        // 眼睑不可用（纯注视模块、2D-only 源、这一路没给 openness）：没有判据。
        // 不能拿 EyeState 默认的 openness=1.0 去判眨眼，那等于拿假数据做时间决策。
        if (!eye.valid || !eye.opennessValid) {
            track = EyeTrack{};
            continue;
        }

        // 这一路这帧没有注视数据：不保持"很久以前的位置"，历史一并作废。
        // 真相没法判断时宁可短暂丢点，也不要让 VRSS 停在一个假位置上。
        if (!dir_valid && !point_valid) {
            track = EyeTrack{};
            continue;
        }

        Sample sample;
        sample.ts_ns       = now_ns;
        sample.openness    = eye.openness;
        sample.dir_valid   = dir_valid;
        sample.dir         = dir;
        sample.pitch_deg   = pitch;
        sample.yaw_deg     = yaw;
        sample.point_valid = point_valid;
        sample.point       = point;
        push(track, sample);

        // 近期睁眼水位：快速下坠判据和"参照/释放线"都按它自适应。
        // 眼睑校准后来源的"全睁"可能只有 0.7，写死 1.0 就永远对不上。
        // 保持期间用"进入保持时的水位"兜底：否则窗口会被闭眼的低值自己拉下来，
        // 越闭越像是"已经睁开"。
        const float baseline   = max_openness(track, now_ns, window_ns);
        const float level      = track.holding ? std::max(baseline, track.held_baseline) : baseline;
        const float open_floor = std::max(config_.blink_enter + 0.05f, level - 0.05f);
        const float release_at = std::min(config_.blink_release, open_floor);
        const float dropped    = level - eye.openness;

        // 闭眼：来源如实报了闭眼，或在窗口内快速下坠（滤波把浅谷抹平的情形）。
        const bool closed = eye.openness < config_.blink_enter || dropped >= config_.blink_drop;
        // 睁开：回到水位附近才算真的睁了，避免半睁期间反复进出。
        const bool opened = eye.openness >= release_at && dropped < config_.blink_drop * 0.5f;

        if (track.holding) {
            if (now_ns > track.hold_start_ns + hold_cap_ns) {
                track.holding  = false;
                track.blocked  = true; // 别每帧重新进保持：那等于永久冻结
                track.blending = false;
                continue;
            }
            if (opened) {
                if (track.reopen_ns == 0) {
                    track.reopen_ns = now_ns;
                }
                const std::uint64_t since = now_ns - track.reopen_ns;
                if (since >= recover_ns) {
                    // 回到保持值附近才算"回弹结束"，否则继续等（最多等到 tail_ms）。
                    // 只按固定毫秒交接的话，来源那段"先低一点再回来"会原样漏出去，
                    // 用户看到的就是"眨眼时往下沉一下再回来"。
                    const bool back = deviation_deg(track.held, sample) <= config_.handoff_deg;
                    if (back || since >= tail_ns) {
                        track.holding        = false;
                        track.reopen_ns      = 0;
                        track.blending       = blend_ns > 0;
                        track.blend_from     = track.held;
                        track.blend_start_ns = now_ns;
                        // 落到下面的过渡分支：这一帧从保持值开始（t = 0）
                    }
                }
            } else {
                track.reopen_ns = 0; // 又闭上了：恢复计时清零
            }

            if (track.holding) {
                // 继续保持：把这一眼的点/方向/角度换回闭眼前的值。
                dir_valid   = track.held.dir_valid;
                dir         = track.held.dir;
                pitch       = track.held.pitch_deg;
                yaw         = track.held.yaw_deg;
                point_valid = track.held.point_valid;
                point       = track.held.point;
                held_any    = true;
                touched     = true;
                continue;
            }
        } else {
            if (!closed) {
                track.blocked = false; // 真正睁开过：超时封锁解除
            }
            if (closed && !track.blocked) {
                if (const Sample* reference = last_open(track, open_floor)) {
                    track.held          = *reference;
                    track.held_baseline = baseline;
                    track.holding       = true;
                    track.hold_start_ns = now_ns;
                    track.reopen_ns     = 0;
                    track.blending      = false; // 刚闭眼：任何交接过渡立刻中止

                    dir_valid   = track.held.dir_valid;
                    dir         = track.held.dir;
                    pitch       = track.held.pitch_deg;
                    yaw         = track.held.yaw_deg;
                    point_valid = track.held.point_valid;
                    point       = track.held.point;
                    held_any    = true;
                    touched     = true;
                    continue;
                }
            }
            // 没有可信参照或不在眨眼：走过渡（如果还在过渡里）或原样透传。
        }

        if (track.blending) {
            const float t =
                blend_ns == 0
                    ? 1.0f
                    : std::clamp(static_cast<float>(now_ns - track.blend_start_ns) /
                                     static_cast<float>(blend_ns),
                                 0.0f, 1.0f);
            if (t >= 1.0f) {
                track.blending = false; // 过渡结束：恢复完全透传
            } else {
                if (track.blend_from.dir_valid && dir_valid) {
                    const Vec3 mixed =
                        normalize(track.blend_from.dir * (1.0f - t) + dir * t);
                    dir           = mixed;
                    const Vec2 py = dir_to_pitch_yaw_deg(mixed);
                    pitch         = py.x;
                    yaw           = py.y;
                }
                if (track.blend_from.point_valid && point_valid) {
                    point = Vec2{track.blend_from.point.x * (1.0f - t) + point.x * t,
                                 track.blend_from.point.y * (1.0f - t) + point.y * t};
                }
                touched = true;
            }
        }
    }

    gaze.blink_hold = held_any;
    if (!touched) {
        return;
    }

    // 合并行必须跟着逐眼重算：否则读 dir_local / pitch_deg 的消费端拿到的仍然是
    // 眨眼时被顶上去的那个方向，只有逐眼字段是对的。
    Vec3 sum{0.0f, 0.0f, 0.0f};
    int count = 0;
    if (gaze.left_valid) {
        sum = sum + gaze.left_dir_local;
        ++count;
    }
    if (gaze.right_valid) {
        sum = sum + gaze.right_dir_local;
        ++count;
    }
    if (count > 0) {
        const Vec3 combined = normalize(sum);
        if (std::isfinite(combined.x) && std::isfinite(combined.y) && std::isfinite(combined.z)) {
            gaze.dir_local = combined;
            const Vec2 py  = dir_to_pitch_yaw_deg(combined);
            gaze.pitch_deg = py.x;
            gaze.yaw_deg   = py.y;
            gaze.valid     = true;
            if (gaze.dir_space == GazeDirSpace::none) {
                // 保持值来自来源本身或上一帧的标定结果；没有更细的信息时报 source，
                // 不冒充 calibrated。
                gaze.dir_space = GazeDirSpace::source;
            }
        }
    }
    gaze.point2d_valid = gaze.left_point_valid && gaze.right_point_valid;
}

} // namespace fgb
