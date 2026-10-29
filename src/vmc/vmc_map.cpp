#include "vmc/vmc_map.hpp"

namespace fgb::vmc {
namespace {

constexpr BlendEntry kMappings[] = {
    {"browDownLeft", Shape::BrowLowererLeft, 1.0f},
    {"browDownRight", Shape::BrowLowererRight, 1.0f},
    {"browInnerUp", Shape::BrowInnerUpLeft, 1.0f},
    {"browInnerUp", Shape::BrowInnerUpRight, 1.0f},
    {"browOuterUpLeft", Shape::BrowOuterUpLeft, 1.0f},
    {"browOuterUpRight", Shape::BrowOuterUpRight, 1.0f},
    {"cheekPuff", Shape::CheekPuffLeft, 1.0f},
    {"cheekPuff", Shape::CheekPuffRight, 1.0f},
    {"cheekSquintLeft", Shape::CheekSquintLeft, 1.0f},
    {"cheekSquintRight", Shape::CheekSquintRight, 1.0f},
    {"eyeSquintLeft", Shape::EyeSquintLeft, 1.0f},
    {"eyeSquintRight", Shape::EyeSquintRight, 1.0f},
    {"eyeWideLeft", Shape::EyeWideLeft, 1.0f},
    {"eyeWideRight", Shape::EyeWideRight, 1.0f},
    {"jawForward", Shape::JawForward, 1.0f},
    {"jawLeft", Shape::JawLeft, 1.0f},
    {"jawRight", Shape::JawRight, 1.0f},
    {"jawOpen", Shape::JawOpen, 1.0f},
    {"mouthClose", Shape::MouthClosed, 1.0f},
    {"mouthDimpleLeft", Shape::MouthDimpleLeft, 1.0f},
    {"mouthDimpleRight", Shape::MouthDimpleRight, 1.0f},
    {"mouthFrownLeft", Shape::MouthFrownLeft, 1.0f},
    {"mouthFrownRight", Shape::MouthFrownRight, 1.0f},
    {"mouthFunnel", Shape::LipFunnelUpperLeft, 1.0f},
    {"mouthFunnel", Shape::LipFunnelUpperRight, 1.0f},
    {"mouthFunnel", Shape::LipFunnelLowerLeft, 1.0f},
    {"mouthFunnel", Shape::LipFunnelLowerRight, 1.0f},
    {"mouthLeft", Shape::MouthUpperLeft, 1.0f},
    {"mouthLeft", Shape::MouthLowerLeft, 1.0f},
    {"mouthRight", Shape::MouthUpperRight, 1.0f},
    {"mouthRight", Shape::MouthLowerRight, 1.0f},
    {"mouthLowerDownLeft", Shape::MouthLowerDownLeft, 1.0f},
    {"mouthLowerDownRight", Shape::MouthLowerDownRight, 1.0f},
    {"mouthPressLeft", Shape::MouthPressLeft, 1.0f},
    {"mouthPressRight", Shape::MouthPressRight, 1.0f},
    {"mouthPucker", Shape::LipPuckerUpperLeft, 1.0f},
    {"mouthPucker", Shape::LipPuckerUpperRight, 1.0f},
    {"mouthPucker", Shape::LipPuckerLowerLeft, 1.0f},
    {"mouthPucker", Shape::LipPuckerLowerRight, 1.0f},
    {"mouthShrugLower", Shape::MouthRaiserLower, 1.0f},
    {"mouthShrugUpper", Shape::MouthRaiserUpper, 1.0f},
    {"mouthSmileLeft", Shape::MouthCornerPullLeft, 1.0f},
    {"mouthSmileRight", Shape::MouthCornerPullRight, 1.0f},
    {"mouthStretchLeft", Shape::MouthStretchLeft, 1.0f},
    {"mouthStretchRight", Shape::MouthStretchRight, 1.0f},
    {"mouthUpperUpLeft", Shape::MouthUpperUpLeft, 1.0f},
    {"mouthUpperUpRight", Shape::MouthUpperUpRight, 1.0f},
    {"noseSneerLeft", Shape::NoseSneerLeft, 1.0f},
    {"noseSneerRight", Shape::NoseSneerRight, 1.0f},
    {"tongueOut", Shape::TongueOut, 1.0f},
};

} // namespace

std::span<const BlendEntry> blend_entries() noexcept {
    return {kMappings, sizeof(kMappings) / sizeof(kMappings[0])};
}

void apply_blend(FaceFrame& frame, std::string_view name, float value) noexcept {
    for (const BlendEntry& entry : kMappings) {
        if (name != entry.name) {
            continue;
        }
        const std::size_t index = index_of(entry.shape);
        frame.shapes[index] += value * entry.scale;
        frame.shapeValid[index] = 1;
    }
}

} // namespace fgb::vmc
