#include "engine/engine.hpp"

#include "base/clock.hpp"
#include "base/log.hpp"

#include <cmath>

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
    if (!source) {
        return;
    }
    // 注册顺序即合并优先级：先加的先被选中
    merger_.add(source.get());
    sources_.push_back(std::move(source));
}

void Engine::add_sink(std::unique_ptr<ISink> sink) {
    if (sink) {
        sinks_.push_back(std::move(sink));
    }
}

void Engine::add_processor(std::unique_ptr<IFaceProcessor> processor) {
    if (processor) {
        processors_.push_back(std::move(processor));
    }
}

bool Engine::restart_source(std::string_view name, std::string& error) {
    error.clear();
    for (auto& source : sources_) {
        if (source == nullptr || name != source->name()) {
            continue;
        }
        if (!source->restart()) {
            error = "该输入源不支持重启或重启失败";
            return false;
        }
        FGB_INFO(kTag, "输入源 '%s' 已重启", source->name());
        return true;
    }
    error = "找不到输入源";
    return false;
}

bool Engine::stop_source(std::string_view name, std::string& error) {
    error.clear();
    for (auto& source : sources_) {
        if (source == nullptr || name != source->name()) {
            continue;
        }
        source->stop();
        FGB_INFO(kTag, "输入源 '%s' 已停止", source->name());
        return true;
    }
    error = "找不到输入源";
    return false;
}

bool Engine::start_source(std::string_view name, std::string& error) {
    error.clear();
    for (auto& source : sources_) {
        if (source == nullptr || name != source->name()) {
            continue;
        }
        if (!source->start()) {
            error = "输入源启动失败";
            if (const auto status = source->module_status(); status.has_value() &&
                                                             !status->error.empty()) {
                error += "：" + status->error;
            }
            return false;
        }
        FGB_INFO(kTag, "输入源 '%s' 已启动", source->name());
        return true;
    }
    error = "找不到输入源";
    return false;
}

bool Engine::start() {
    for (auto& source : sources_) {
        if (!source->start()) {
            // 模块给的失败原因（ABI 不兼容、缺 DLL、初始化返回 0）必须进日志，
            // 否则命令行用户只能看到"启动失败"，诊断要回界面翻模块卡片。
            const auto status = source->module_status();
            if (status.has_value() && !status->error.empty()) {
                FGB_ERROR(kTag, "source '%s' 启动失败：%s", source->name(),
                          status->error.c_str());
            } else {
                FGB_ERROR(kTag, "source '%s' 启动失败", source->name());
            }
            return false;
        }
    }
    for (auto& sink : sinks_) {
        if (!sink->start()) {
            FGB_ERROR(kTag, "sink '%s' 启动失败", sink->name());
            return false;
        }
    }
    gaze_estimator_.reset();
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

    // 渲染注视优先用独立 gaze 通道；没有时才回退到眼睛反算。
    // 头像输出不走这里（VrchatSink 只读 eyes），所以 B 的注视不会接管 A 的眼睛。
    if (frame.has_channel(channel::kGaze) && frame.gaze.any_valid()) {
        const GazePair& g         = frame.gaze;
        const ChannelStamp& stamp = frame.channelStamps[channel::index(channel::kGaze)];
        gaze.left_valid        = g.left.valid;
        gaze.right_valid       = g.right.valid;
        gaze.left_dir_local    = g.left.dirLocal;
        gaze.right_dir_local   = g.right.dirLocal;
        // 点的有效性与方向无关：只给 2D 点的源也算"有数据"，但 valid 只表示有方向
        gaze.left_point_valid  = g.left.pointValid;
        gaze.right_point_valid = g.right.pointValid;
        gaze.point2d_valid     = g.pointsValid;
        gaze.point_viewport    = g.pointViewport;
        gaze.left_point_2d     = g.left.point2d;
        gaze.right_point_2d    = g.right.point2d;
        gaze.confidence        = g.confidence;
        gaze.capture_ts_ns     = g.captureTsNs != 0 ? g.captureTsNs : stamp.captureTsNs;
        if (gaze.capture_ts_ns == 0) {
            gaze.capture_ts_ns = frame.captureTsNs;
        }
        gaze.source      = stamp.source;
        gaze.source_mask = frame.contributingSources;
        gaze.left_source  = g.left.source;
        gaze.right_source = g.right.source;
        gaze.left_capture_ts_ns =
            g.left.captureTsNs != 0 ? g.left.captureTsNs : gaze.capture_ts_ns;
        gaze.right_capture_ts_ns =
            g.right.captureTsNs != 0 ? g.right.captureTsNs : gaze.capture_ts_ns;

        // 只有 2D 点、没有方向：不拿默认的 -Z 冒充视线。
        // valid 在文档里就是"有可用的合并方向"，这里必须为假，否则
        // GazeEstimator 会把恒定的 -Z 当成"一直在注视"，第三方也会拿到假方向。
        if (!g.left.valid && !g.right.valid && !g.combinedValid) {
            return gaze;
        }
        gaze.valid = true;

        if (g.combinedValid) {
            gaze.dir_local = g.combinedDirLocal;
        } else if (g.left.valid && g.right.valid) {
            gaze.dir_local = normalize(g.left.dirLocal + g.right.dirLocal);
        } else {
            gaze.dir_local = g.left.valid ? g.left.dirLocal : g.right.dirLocal;
        }
        if (!std::isfinite(gaze.dir_local.x) || !std::isfinite(gaze.dir_local.y) ||
            !std::isfinite(gaze.dir_local.z)) {
            gaze.dir_local = Vec3{0.0f, 0.0f, -1.0f};
            gaze.valid     = false;
            return gaze;
        }
        gaze.dir_local = normalize(gaze.dir_local);
        const Vec2 pitch_yaw = dir_to_pitch_yaw_deg(gaze.dir_local);
        gaze.pitch_deg       = pitch_yaw.x;
        gaze.yaw_deg         = pitch_yaw.y;
        // 逐眼角度从各自方向反算：gaze 通道没有缓存角度，合并角度也是同一个函数出来的，
        // 放在这里算，界面和脚本就不必各自实现一遍（符号约定只守一处）。
        const Vec2 left_py   = dir_to_pitch_yaw_deg(gaze.left_dir_local);
        const Vec2 right_py  = dir_to_pitch_yaw_deg(gaze.right_dir_local);
        gaze.left_pitch_deg  = left_py.x;
        gaze.left_yaw_deg    = left_py.y;
        gaze.right_pitch_deg = right_py.x;
        gaze.right_yaw_deg   = right_py.y;

        if (frame.eyes.hasVergence) {
            gaze.vergence_m = frame.eyes.vergenceDistanceM;
        }
        return gaze;
    }

    const EyeState& l = frame.eyes.left;
    const EyeState& r = frame.eyes.right;
    const bool l_gaze = l.valid && l.gazeValid;
    const bool r_gaze = r.valid && r.gazeValid;

    if (!l_gaze && !r_gaze) {
        return gaze;
    }

    const ChannelStamp& stamp = frame.channelStamps[channel::index(channel::kEyes)];
    gaze.valid                = true;
    gaze.left_valid           = l_gaze;
    gaze.right_valid          = r_gaze;
    gaze.left_dir_local       = l.gazeDirLocal;
    gaze.right_dir_local      = r.gazeDirLocal;
    gaze.capture_ts_ns        = stamp.captureTsNs != 0 ? stamp.captureTsNs : frame.captureTsNs;
    gaze.source               = stamp.source;
    gaze.source_mask          = frame.contributingSources;
    gaze.left_source          = l.gazeSource;
    gaze.right_source         = r.gazeSource;
    gaze.left_capture_ts_ns   = l.gazeTsNs != 0 ? l.gazeTsNs : gaze.capture_ts_ns;
    gaze.right_capture_ts_ns  = r.gazeTsNs != 0 ? r.gazeTsNs : gaze.capture_ts_ns;

    // 逐眼角度读来源缓存的 gazePitchYawDeg：下面的合并角度也是平均它，
    // 显示与合并同源，不会出现"两眼都是 10°、合并却是 0°"这种自相矛盾。
    if (l_gaze) {
        gaze.left_pitch_deg = l.gazePitchYawDeg.x;
        gaze.left_yaw_deg   = l.gazePitchYawDeg.y;
    }
    if (r_gaze) {
        gaze.right_pitch_deg = r.gazePitchYawDeg.x;
        gaze.right_yaw_deg   = r.gazePitchYawDeg.y;
    }

    if (l_gaze && r_gaze) {
        gaze.pitch_deg = 0.5f * (l.gazePitchYawDeg.x + r.gazePitchYawDeg.x);
        gaze.yaw_deg   = 0.5f * (l.gazePitchYawDeg.y + r.gazePitchYawDeg.y);
        gaze.dir_local = normalize(l.gazeDirLocal + r.gazeDirLocal);
        gaze.confidence = 0.5f * (l.gazeConf + r.gazeConf);
    } else {
        const EyeState& use = l_gaze ? l : r;
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
    const std::uint64_t previous = last_tick_ns_;
    last_tick_ns_                 = now_ns;
    // 休眠/调试打断后，上一帧的速度、注视状态和耗时窗口都失去意义，
    // 不清掉会出现"刚唤醒就判成扫视"或 p95 被一次几小时的停顿拉爆。
    if (previous != 0 && now_ns > previous + 1'000'000'000ULL) {
        ++stats_.resumes;
        gaze_estimator_.reset();
        tick_stats_.reset();
        FGB_WARN(kTag, "检测到 %.1f 秒停顿（休眠/调试），已复位注视与耗时统计",
                 static_cast<double>(now_ns - previous) / 1'000'000'000.0);
    }

    const std::uint64_t work_start = clock::now_ns();

    // 先合并，再后处理，最后发布。
    // 顺序不能反：两个源频率不同时，"每个源各处理一遍"等于把滤波跑了两遍，
    // 参数会显得又慢又迟钝，而且很难看出是这里的锅。
    FaceFrame merged;
    if (merger_.poll(now_ns, merged)) {
        for (auto& processor : processors_) {
            processor->process(merged);
        }
        merged.publishTsNs = now_ns;
        merged.sequence    = ++sequence_;
        last_frame_        = merged;
        snapshot_.publish(last_frame_);
        last_gaze_ = compute_gaze(last_frame_);
        gaze_estimator_.update(last_frame_, last_gaze_, now_ns);
    }

    for (auto& sink : sinks_) {
        sink->submit(last_frame_, last_gaze_, now_ns);
    }

    tick_stats_.record(clock::now_ns() - work_start);
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
