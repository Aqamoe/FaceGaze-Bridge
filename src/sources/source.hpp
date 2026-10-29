// 输入端统一接口。
//
// poll 模型而非 push：引擎按固定 tick 拉取，天然限频，也让多源合并有统一入口。
// 需要收包的源（UDP）自己在内部起线程，对外仍然只暴露 poll。
#pragma once

#include "core/model/frame.hpp"

#include <cstdint>
#include <optional>
#include <string>

namespace fgb {

struct SourceStats {
    std::uint64_t frames       = 0;
    std::uint64_t packets      = 0;
    std::uint64_t parse_errors = 0;
};

// 模块型输入源（VRCFT / 原生 DLL）的运行态，供界面显示"为什么没数据"。
struct ModuleRuntimeStatus {
    bool alive      = false;
    std::uint64_t frames = 0;
    int restarts    = 0;
    std::string error; // 模块自己报的错优先，其次宿主错误
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

    // 只有"宿主管理的模块"才有运行态；UDP/VMC 等返回 nullopt。
    virtual std::optional<ModuleRuntimeStatus> module_status() const { return std::nullopt; }
};

} // namespace fgb
