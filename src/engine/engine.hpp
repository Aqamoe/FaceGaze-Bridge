// 引擎：把 source 的帧合并、发布快照、扇出到 sink。
//
// 合并细节在 engine/source_merger.hpp：一帧按"表情 / 眼追 / 头姿"三个通道分别选提供者，
// 这样硬件眼追模块和摄像头表情模块可以同时喂，而不是谁最后 poll 到谁赢。
//
// 多个源各自有新帧时，合并结果每 tick 只发布一次；后处理（校准/滤波）也在合并之后
// 跑一次——否则两个源不同频率时会被滤波两次，参数值会莫名其妙地变得"更慢"。
#pragma once

#include "core/cont/seqlock.hpp"
#include "core/process/face_processor.hpp"
#include "core/process/gaze_estimator.hpp"
#include "engine/source_merger.hpp"
#include "sinks/sink.hpp"
#include "sources/source.hpp"

#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

namespace fgb {

struct EngineConfig {
    int tick_hz = 120;
};

struct EngineStats {
    std::uint64_t ticks        = 0;
    std::uint64_t late_ticks   = 0;
    std::uint64_t frames_in    = 0;
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

    bool start();
    void stop() noexcept;

    // 阻塞主循环，直到 should_stop 置位。定时精度由 clock 层保证。
    void run(const std::atomic<bool>& should_stop);

    // 单次 tick，供测试与嵌入式使用。
    void tick_once(std::uint64_t now_ns);

    FaceFrame latest() const;
    GazeState current_gaze() const;
    EngineStats stats() const noexcept {
        // frames_in 由合并器统计（它是真正 poll 每个源的地方）
        EngineStats stats = stats_;
        stats.frames_in   = merger_.frames_in();
        return stats;
    }

private:
    GazeState compute_gaze(const FaceFrame& frame) const noexcept;

    EngineConfig config_;
    std::vector<std::unique_ptr<ISource>> sources_;
    std::vector<std::unique_ptr<ISink>> sinks_;
    std::vector<std::unique_ptr<IFaceProcessor>> processors_;
    SourceMerger merger_;
    GazeEstimator gaze_estimator_;
    std::uint64_t last_tick_ns_ = 0;

    SeqLock<FaceFrame> snapshot_;
    FaceFrame last_frame_{};
    GazeState last_gaze_{};
    std::uint64_t sequence_ = 0;
    bool started_           = false;
    EngineStats stats_{};
};

} // namespace fgb
