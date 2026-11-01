#include "gaze_calib/session.hpp"

#include "base/log.hpp"
#include "gaze_calib/fit.hpp"
#include "openvr/overlay.hpp"
#include "params/gaze_profile.hpp"

#include <cmath>
#include <cstdio>
#include <ctime>
#include <utility>
#include <vector>

namespace fgb::gaze_calib {
namespace {

constexpr char kTag[] = "gaze-calib";

// 目标出现后先给 0.5 s 让人看过去：立刻开采会把扫视途中的点算进该目标。
constexpr std::uint64_t kSettleNs = 500'000'000ULL;
// 单点最长等待：超过就判失败（多半是注视源没数据或没有几何）。
constexpr std::uint64_t kTargetTimeoutNs = 8'000'000'000ULL;
constexpr float kDotRadiusDeg = 1.2f;
constexpr float kDotDistanceM = 1.5f;

std::string utc_now() {
    const std::time_t now = std::time(nullptr);
    std::tm parts{};
#ifdef _WIN32
    gmtime_s(&parts, &now);
#else
    gmtime_r(&now, &parts);
#endif
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%04d-%02d-%02dT%02d:%02d:%02dZ",
                  parts.tm_year + 1900, parts.tm_mon + 1, parts.tm_mday, parts.tm_hour,
                  parts.tm_min, parts.tm_sec);
    return buffer;
}

} // namespace

const char* state_name(State state) noexcept {
    switch (state) {
    case State::idle: return "idle";
    case State::running: return "running";
    case State::done: return "done";
    case State::failed: return "failed";
    }
    return "idle";
}

void Session::configure(std::string profile_path, std::string hmd_identity) {
    profile_path_ = std::move(profile_path);
    hmd_identity_ = std::move(hmd_identity);
}

bool Session::start(std::string& error) {
    error.clear();
    if (status_.state == State::running) {
        error = "已经在标定中";
        return false;
    }
    if (!projection_.valid) {
        error = "没有头显几何（先让 SteamVR 运行并导出几何）";
        return false;
    }
    if (dots_ == nullptr) {
        error = "overlay 不可用";
        return false;
    }
    // 标定只要一个白点；探针有它自己的一组彩色点（彼此 key 不冲突）
    if (!dots_->open(1, std::vector<overlay::Color>{{255, 255, 255}}, error)) {
        return false;
    }

    // 回滚用：记下开始前的档案，取消时原样恢复
    previous_ = GazeProfile{};
    std::string load_error;
    params::load_gaze_profile(profile_path_, previous_, load_error);

    // 关键：清空档案。采集必须是"标定前"的观察值，否则拟合出来的是二次校准。
    if (apply_) {
        apply_(GazeProfile{});
    }

    collector_.reset();
    branch_          = GazeSourceKind::none;
    branch_decided_  = false;
    result_          = GazeProfile{};
    status_.state    = State::running;
    status_.done     = 0;
    status_.samples  = 0;
    status_.needed   = Collector::kMinSamples;
    status_.progress = 0.0f;
    status_.residual = -1.0f;
    status_.target   = 0;
    status_.message  = "请注视圆点";
    target_started_ns_ = 0; // 由第一帧 tick 填，避免用过期时钟
    FGB_INFO(kTag, "开始注视标定：%d 点", static_cast<int>(kTargetCount));
    return true;
}

void Session::cancel() noexcept {
    if (status_.state == State::running && apply_) {
        apply_(previous_); // 回到开始前的状态，不留下"半套标定"
    }
    collector_.reset();
    status_.state   = State::idle;
    status_.message = "已取消";
    if (dots_ != nullptr) {
        dots_->hide_all();
    }
    target_started_ns_ = 0;
}

void Session::begin_target(std::uint64_t now_ns) noexcept {
    target_started_ns_ = now_ns;
    status_.samples    = 0;
    status_.needed     = Collector::kMinSamples;
    status_.target     = status_.done;
    place_dot();
}

void Session::place_dot() noexcept {
    if (dots_ == nullptr || status_.target >= kTargetCount) {
        return;
    }
    const Target& target = targets()[status_.target];
    std::string error;
    if (!dots_->show(0, target.yaw_deg, target.pitch_deg, kDotRadiusDeg, kDotDistanceM, error)) {
        fail("摆放标定点失败：" + error);
        return;
    }
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "第 %d/%d 点：请注视圆点",
                  static_cast<int>(status_.target + 1), static_cast<int>(kTargetCount));
    status_.message = buffer;
}

void Session::advance_target(std::uint64_t now_ns) noexcept {
    ++status_.done;
    status_.progress =
        static_cast<float>(status_.done) / static_cast<float>(kTargetCount);
    if (status_.done >= kTargetCount) {
        finish(now_ns);
        return;
    }
    begin_target(now_ns);
}

void Session::finish(std::uint64_t /*now_ns*/) noexcept {
    // 组装拟合样本：每个目标的中位数观察值 ↔ 该眼看目标时的视口坐标
    std::vector<Pair> left_pairs;
    std::vector<Pair> right_pairs;
    left_pairs.reserve(kTargetCount);
    right_pairs.reserve(kTargetCount);
    for (std::size_t i = 0; i < kTargetCount; ++i) {
        const TargetSamples samples = collector_.result(i);
        if (!samples.ok) {
            fail("采样不足或抖动过大，请重新标定");
            return;
        }
        Vec2 left_target{};
        Vec2 right_target{};
        if (!target_viewport(targets()[i], projection_.eyes[0], left_target) ||
            !target_viewport(targets()[i], projection_.eyes[1], right_target)) {
            fail("目标无法投影到视口（几何无效）");
            return;
        }
        left_pairs.push_back(Pair{samples.left, left_target});
        right_pairs.push_back(Pair{samples.right, right_target});
    }

    const EyeFit left  = fit_affine(left_pairs);
    const EyeFit right = fit_affine(right_pairs);
    if (!left.ok || !right.ok) {
        fail(std::string{"拟合失败："} + (left.ok ? right.reason : left.reason));
        return;
    }

    const std::uint32_t version = previous_.version + 1;
    if (!build_profile(branch_, left, right, version, utc_now(), hmd_identity_, result_)) {
        fail("组装档案失败");
        return;
    }

    std::string save_error;
    if (!params::save_gaze_profile(profile_path_, result_, save_error)) {
        fail("保存档案失败：" + save_error);
        return;
    }

    // 热加载：写盘成功只代表"下次启动能用"，现在这一局必须立刻切到新映射，
    // 否则页面上会显示"已完成"但档案仍是"未标定"（消费者继续用旧/空档案）。
    if (apply_) {
        apply_(result_);
    }

    status_.state           = State::done;
    status_.profile_version = result_.version;
    status_.residual        = result_.residual_p95;
    status_.progress        = 1.0f;
    status_.message         = "标定完成";
    if (dots_ != nullptr) {
        dots_->hide_all();
    }
    FGB_INFO(kTag, "标定完成：v%u，残差 P95 %.4f", result_.version,
             static_cast<double>(result_.residual_p95));
}

void Session::fail(std::string message) noexcept {
    status_.state   = State::failed;
    status_.message = std::move(message);
    if (dots_ != nullptr) {
        dots_->hide_all();
    }
    // 失败不留半套档案：回到开始前（可能是空）
    if (apply_) {
        apply_(previous_);
    }
    FGB_WARN(kTag, "标定失败：%s", status_.message.c_str());
}

void Session::tick(std::uint64_t now_ns, const GazeState& gaze) noexcept {
    if (status_.state != State::running) {
        return;
    }
    if (dots_ == nullptr) {
        fail("overlay 丢失");
        return;
    }
    if (target_started_ns_ == 0) {
        begin_target(now_ns);
        return;
    }

    // 目标切换后的静置期：人还在看过去，先不采
    if (now_ns > target_started_ns_ && now_ns - target_started_ns_ < kSettleNs) {
        status_.message = "看住圆点…";
        return;
    }

    // 分支在第一个有效样本时定下来：原始点走点分支，投影出来的点走角度分支。
    // 两者量纲不同，档案里必须记清楚，否则下次装配时会被拒绝或被硬套。
    if (!branch_decided_ && gaze.left_point_valid && gaze.right_point_valid) {
        branch_ = gaze.space == GazeSpace::source ? GazeSourceKind::points
                                                  : GazeSourceKind::angles;
        branch_decided_ = true;
    }

    if (collector_.add(status_.target, gaze)) {
        const TargetSamples samples = collector_.result(status_.target);
        status_.samples = samples.count;
        status_.needed  = samples.count >= Collector::kMinSamples ? 0 : Collector::kMinSamples - samples.count;
        if (samples.ok) {
            advance_target(now_ns);
            return;
        }
    }

    if (now_ns > target_started_ns_ && now_ns - target_started_ns_ > kTargetTimeoutNs) {
        fail("该点采集超时：检查注视源是否有数据、眼睛是否盯着圆点");
    }
}

} // namespace fgb::gaze_calib
