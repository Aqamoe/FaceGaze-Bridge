// 输出端统一接口。
//
// 每个 Sink 独立节流：VRChat 要 90 Hz，VMC 要 60 Hz 稳定，共享内存要每帧全量。
// 这就是为什么节流不能放在 SinkManager 里统一做。
#pragma once

#include "core/model/frame.hpp"
#include "core/model/gaze.hpp"

#include <cstdint>

namespace fgb {

struct SinkStats {
    std::uint64_t messages  = 0;
    std::uint64_t bytes     = 0;
    std::uint64_t suppressed = 0;
    std::uint64_t errors    = 0;
};

class ISink {
public:
    virtual ~ISink() = default;
    virtual const char* name() const noexcept = 0;
    virtual bool start() = 0;
    virtual void stop() noexcept = 0;
    virtual void submit(const FaceFrame& frame, const GazeState& gaze, std::uint64_t now_ns) = 0;
    virtual SinkStats stats() const noexcept = 0;
};

} // namespace fgb
