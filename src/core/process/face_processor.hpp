// 帧后处理链。
//
// 数据从 source 出来之后、发布快照之前会依次经过这里。放在这个位置而不是 sink 里，
// 是因为它处理的是"表情数据"本身，和输出到哪里无关——以后加 VMC sink 时，
// 滤波和校准应该同样生效，而不是只有 VRChat 那一路被处理。
//
// 约定：process 必须幂等且不分配内存（每帧都调，120 Hz）。
#pragma once

#include "core/model/frame.hpp"

namespace fgb {

class IFaceProcessor {
public:
    virtual ~IFaceProcessor() = default;
    virtual const char* name() const noexcept = 0;
    virtual void process(FaceFrame& frame) noexcept = 0;
};

} // namespace fgb
