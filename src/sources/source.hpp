// 输入端统一接口。
//
// poll 模型而非 push：引擎按固定 tick 拉取，天然限频，也让多源合并有统一入口。
// 需要收包的源（UDP）自己在内部起线程，对外仍然只暴露 poll。
#pragma once

#include "core/model/frame.hpp"

#include <cstdint>

namespace fgb {

struct SourceStats {
    std::uint64_t frames       = 0;
    std::uint64_t packets      = 0;
    std::uint64_t parse_errors = 0;
};

class ISource {
public:
    virtual ~ISource() = default;
    virtual const char* name() const noexcept  = 0;
    virtual bool start()                        = 0;
    virtual void stop() noexcept                = 0;
    // 有新帧返回 true 并写入 out；无新数据返回 false。
    virtual bool poll(FaceFrame& out)           = 0;
    virtual SourceStats stats() const noexcept  = 0;
};

} // namespace fgb
