#pragma once
#include "projection.h"
namespace frame {
inline const char* DeliveryState(bool fresh,int queryStatus,uint64_t consumers,int updateStatus) {
    if(queryStatus!=0 || updateStatus!=0)return "driver-error";
    if(!fresh)return "waiting-gaze";
    return consumers ? "submitting" : "waiting-consumer";
}
inline bool ShouldSubmit(bool fresh,int queryStatus,uint64_t consumerState,
                         uint64_t sequence,uint64_t lastSequence,uint64_t timestamp,uint64_t lastTimestamp) {
    return fresh && queryStatus==0 && consumerState!=0 &&
           sequence!=lastSequence && timestamp>lastTimestamp;
}
inline bool MapNdc(bool valid, const std::array<float,4>& ndc, const Gain& gain,
                   std::array<float,4>& result,bool steamFrame=false) {
    if (!valid) return false;
    for (float value : ndc) if (!std::isfinite(value)) return false;
    // Verified Frame + VRSS layout 4 uses the opposite vertical convention.
    const double y=gain.y*(steamFrame?-1.:1.);
    std::array<float,4> mapped{
        float(ndc[0]*gain.x+gain.leftDegrees/45.), float(ndc[1]*y),
        float(ndc[2]*gain.x+gain.rightDegrees/45.), float(ndc[3]*y)};
    for (auto& value : mapped) {
        if (!std::isfinite(value)) return false;
        value=std::clamp(value,-1.f,1.f);
    }
    result=mapped;
    return true;
}
}
