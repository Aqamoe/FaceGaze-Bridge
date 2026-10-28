// 引擎：把 source 的帧合并、发布快照、扇出到 sink。
//
// M1 阶段合并策略是"取最后一个有效帧"。真正的多源融合（硬件眼追 + 摄像头表情）
// 需要按能力位拆分来源，留到 M2 之后。
#pragma once

#include "core/cont/seqlock.hpp"
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

    bool start();
    void stop() noexcept;

    // 阻塞主循环，直到 should_stop 置位。定时精度由 clock 层保证。
    void run(const std::atomic<bool>& should_stop);

    // 单次 tick，供测试与嵌入式使用。
    void tick_once(std::uint64_t now_ns);

    FaceFrame latest() const;
    GazeState current_gaze() const;
    EngineStats stats() const noexcept { return stats_; }

private:
    GazeState compute_gaze(const FaceFrame& frame) const noexcept;

    EngineConfig config_;
    std::vector<std::unique_ptr<ISource>> sources_;
    std::vector<std::unique_ptr<ISink>> sinks_;

    SeqLock<FaceFrame> snapshot_;
    FaceFrame last_frame_{};
    GazeState last_gaze_{};
    std::uint64_t sequence_ = 0;
    bool started_           = false;
    EngineStats stats_{};
};

} // namespace fgb
