#include "engine/engine.hpp"

#include "base/clock.hpp"
#include "base/log.hpp"

namespace fgb {
namespace {
constexpr char kTag[] = "engine";
}

Engine::Engine(EngineConfig config) : config_(config) {
    if (config_.tick_hz < 1) {
        config_.tick_hz = 120;
    }
    last_frame_ = FaceFrame::neutral();
    snapshot_.publish(last_frame_);
}

Engine::~Engine() { stop(); }

void Engine::add_source(std::unique_ptr<ISource> source) {
    if (source) {
        sources_.push_back(std::move(source));
    }
}

void Engine::add_sink(std::unique_ptr<ISink> sink) {
    if (sink) {
        sinks_.push_back(std::move(sink));
    }
}

bool Engine::start() {
    for (auto& source : sources_) {
        if (!source->start()) {
            FGB_ERROR(kTag, "source '%s' 启动失败", source->name());
            return false;
        }
    }
    for (auto& sink : sinks_) {
        if (!sink->start()) {
            FGB_ERROR(kTag, "sink '%s' 启动失败", sink->name());
            return false;
        }
    }
    started_ = true;
    FGB_INFO(kTag, "引擎启动，tick %d Hz，%zu 个输入，%zu 个输出", config_.tick_hz, sources_.size(),
             sinks_.size());
    return true;
}

void Engine::stop() noexcept {
    if (!started_) {
        return;
    }
    // 先停输出，避免收到半帧；再停输入
    for (auto& sink : sinks_) {
        sink->stop();
    }
    for (auto& source : sources_) {
        source->stop();
    }
    started_ = false;
    FGB_INFO(kTag, "引擎停止，共 %llu tick", static_cast<unsigned long long>(stats_.ticks));
}

GazeState Engine::compute_gaze(const FaceFrame& frame) const noexcept {
    GazeState gaze;
    const EyeState& l = frame.eyes.left;
    const EyeState& r = frame.eyes.right;

    if (!l.valid && !r.valid) {
        return gaze;
    }

    gaze.valid = true;
    if (l.valid && r.valid) {
        gaze.pitch_deg = 0.5f * (l.gazePitchYawDeg.x + r.gazePitchYawDeg.x);
        gaze.yaw_deg   = 0.5f * (l.gazePitchYawDeg.y + r.gazePitchYawDeg.y);
        gaze.dir_local = normalize(l.gazeDirLocal + r.gazeDirLocal);
        gaze.confidence = 0.5f * (l.gazeConf + r.gazeConf);
    } else {
        const EyeState& use = l.valid ? l : r;
        gaze.pitch_deg      = use.gazePitchYawDeg.x;
        gaze.yaw_deg        = use.gazePitchYawDeg.y;
        gaze.dir_local      = use.gazeDirLocal;
        gaze.confidence     = use.gazeConf;
    }

    if (frame.eyes.hasVergence) {
        gaze.vergence_m = frame.eyes.vergenceDistanceM;
    }
    return gaze;
}

void Engine::tick_once(std::uint64_t now_ns) {
    bool got_new = false;

    for (auto& source : sources_) {
        FaceFrame frame;
        if (source->poll(frame)) {
            frame.sanitize();
            last_frame_ = frame;
            got_new     = true;
            ++stats_.frames_in;
        }
    }

    if (got_new) {
        last_frame_.publishTsNs = now_ns;
        last_frame_.sequence    = ++sequence_;
        snapshot_.publish(last_frame_);
        last_gaze_ = compute_gaze(last_frame_);
    }

    for (auto& sink : sinks_) {
        sink->submit(last_frame_, last_gaze_, now_ns);
    }

    ++stats_.ticks;
}

void Engine::run(const std::atomic<bool>& should_stop) {
    const std::uint64_t period = 1'000'000'000ULL / static_cast<std::uint64_t>(config_.tick_hz);
    std::uint64_t next         = clock::now_ns() + period;

    while (!should_stop.load(std::memory_order_acquire)) {
        const std::uint64_t now = clock::now_ns();
        tick_once(now);

        clock::sleep_until_ns(next);
        next += period;

        const std::uint64_t after = clock::now_ns();
        if (next < after) {
            // 落后超过一个周期：跳帧重新对齐，避免误差累积
            next = after + period;
            ++stats_.late_ticks;
        }
    }
}

FaceFrame Engine::latest() const {
    FaceFrame frame;
    if (!snapshot_.read(frame)) {
        return last_frame_;
    }
    return frame;
}

GazeState Engine::current_gaze() const { return last_gaze_; }

} // namespace fgb
