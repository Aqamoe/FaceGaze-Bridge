#include "core/model/frame.hpp"

#include <cmath>

namespace fgb {

FaceFrame FaceFrame::neutral() noexcept {
    FaceFrame f;
    f.eyes.left.valid     = true;
    f.eyes.left.openness  = 1.0f;
    f.eyes.right          = f.eyes.left;
    f.eyes.left.gazeDirLocal  = {0.0f, 0.0f, -1.0f};
    f.eyes.right.gazeDirLocal = {0.0f, 0.0f, -1.0f};
    f.eyes.left.gazePitchYawDeg  = {0.0f, 0.0f};
    f.eyes.right.gazePitchYawDeg = {0.0f, 0.0f};
    f.qualityScore = 0.0f;
    return f;
}

void FaceFrame::sanitize() noexcept {
    for (std::size_t i = 0; i < kShapeCount; ++i) {
        float v = shapes[i];
        if (!std::isfinite(v)) {
            v = 0.0f;
            shapeValid[i] = 0;
        }
        shapes[i] = clamp01(v);
    }

    EyeState* const both[2] = {&eyes.left, &eyes.right};
    for (EyeState* e : both) {
        if (!std::isfinite(e->openness)) {
            e->openness = 1.0f;
            e->valid    = false;
        }
        e->openness = clamp01(e->openness);

        if (!std::isfinite(e->gazeDirLocal.x) || !std::isfinite(e->gazeDirLocal.y) ||
            !std::isfinite(e->gazeDirLocal.z)) {
            e->gazeDirLocal = {0.0f, 0.0f, -1.0f};
            e->valid        = false;
        }
        e->gazeDirLocal = normalize(e->gazeDirLocal);

        if (!std::isfinite(e->pupilDiameterMm) || e->pupilDiameterMm < 0.0f) {
            e->pupilDiameterMm = 0.0f;
        }
        e->pupilDilation = clamp_sym(e->pupilDilation);
    }

    if (!std::isfinite(qualityScore)) {
        qualityScore = 0.0f;
    }
    qualityScore = clamp01(qualityScore);
}

} // namespace fgb
