// 引擎：把 source 的帧合并、发布快照、扇出到 sink。
//
// 合并细节在 engine/source_merger.hpp：一帧按"表情 / 眼追 / 头姿"三个通道分别选提供者，
// 这样硬件眼追模块和摄像头表情模块可以同时喂，而不是谁最后 poll 到谁赢。
//
// 多个源各自有新帧时，合并结果每 tick 只发布一次；后处理（校准/滤波）也在合并之后
// 跑一次——否则两个源不同频率时会被滤波两次，参数值会莫名其妙地变得"更慢"。
#pragma once

#include "core/cont/seqlock.hpp"
#include "core/model/gaze_profile.hpp"
#include "core/model/gaze_projection.hpp"
#include "core/process/face_processor.hpp"
#include "core/process/gaze_calibrator.hpp"
#include "core/process/gaze_estimator.hpp"
#include "core/process/gaze_normalizer.hpp"
#include "engine/source_merger.hpp"
#include "engine/tick_stats.hpp"
#include "sinks/sink.hpp"
#include "sources/source.hpp"

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace fgb {

struct EngineConfig {
    int tick_hz = 120;
};

struct EngineStats {
    std::uint64_t ticks        = 0;
    std::uint64_t late_ticks   = 0;
    std::uint64_t frames_in    = 0;
    std::uint64_t resumes      = 0; // 休眠/长时间停顿后恢复的次数
};

class Engine {
public:
    explicit Engine(EngineConfig config = {});
    ~Engine();

    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    void add_source(std::unique_ptr<ISource> source);
    void add_sink(std::unique_ptr<ISink> sink);
    // 后处理按添加顺序执行（先校准、再滤波是常规顺序）
    void add_processor(std::unique_ptr<IFaceProcessor> processor);

    // 必须在 start() 之前调用；之后改策略也不会炸，但不推荐
    void set_merge_policy(const MergePolicy& policy) { merger_.set_policy(policy); }
    const MergePolicy& merge_policy() const noexcept { return merger_.policy(); }
    // 每个源的贡献情况，供界面显示"哪一路在喂数据"
    std::vector<SourceReport> source_report() const { return merger_.report(last_tick_ns_); }

    // 模块型输入的重启/停止/启动（跨进程宿主用于崩溃恢复、热替换 DLL）。
    bool restart_source(std::string_view name, std::string& error);
    bool stop_source(std::string_view name, std::string& error);
    bool start_source(std::string_view name, std::string& error);
    // 彻底摘掉一个输入源（模块被删除时用）：停止、从合并器与源列表里移除。
    // 只 stop 不摘会让界面继续显示一个已经不存在的模块。
    bool remove_source(std::string_view name, std::string& error);

    bool start();
    void stop() noexcept;

    // 阻塞主循环，直到 should_stop 置位。定时精度由 clock 层保证。
    void run(const std::atomic<bool>& should_stop);

    // 单次 tick，供测试与嵌入式使用。
    void tick_once(std::uint64_t now_ns);

    FaceFrame latest() const;
    GazeState current_gaze() const;

    // 注视管线（ADR-0042）：档案与几何都由控制面注入，引擎只负责按帧跑。
    void set_gaze_profile(const GazeProfile& profile) noexcept { calibrator_.set_profile(profile); }
    void set_gaze_projection(const GazeProjection& projection) noexcept {
        calibrator_.set_projection(projection);
    }
    GazeProfile gaze_profile() const noexcept { return calibrator_.profile(); }
    GazeProjection gaze_projection() const noexcept { return calibrator_.projection(); }
    TickStats::Snapshot tick_stats() const noexcept { return tick_stats_.snapshot(); }
    EngineStats stats() const noexcept {
        // frames_in 由合并器统计（它是真正 poll 每个源的地方）
        EngineStats stats = stats_;
        stats.frames_in   = merger_.frames_in();
        return stats;
    }

private:
    // 合并后的原始注视（两条分支各自的字段，未规范化）
    GazeState assemble_gaze(const FaceFrame& frame) const noexcept;
    // 规范注视点：assemble → 规范化 → 标定（ADR-0042 §1 的固定顺序）
    GazeState compute_gaze(const FaceFrame& frame, std::uint64_t now_ns);

    EngineConfig config_;
    std::vector<std::unique_ptr<ISource>> sources_;
    std::vector<std::unique_ptr<ISink>> sinks_;
    std::vector<std::unique_ptr<IFaceProcessor>> processors_;
    SourceMerger merger_;
    GazeEstimator gaze_estimator_;
    GazeNormalizer normalizer_;
    GazeCalibrator calibrator_;
    TickStats tick_stats_;
    std::uint64_t last_tick_ns_ = 0;

    SeqLock<FaceFrame> snapshot_;
    FaceFrame last_frame_{};
    GazeState last_gaze_{};
    std::uint64_t sequence_ = 0;
    bool started_           = false;
    EngineStats stats_{};
};

} // namespace fgb
