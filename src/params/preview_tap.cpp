#include "params/preview_tap.hpp"

namespace fgb::params {

void PreviewTap::process(FaceFrame& frame) noexcept {
    if (!enabled_.load(std::memory_order_relaxed)) {
        return;
    }

    PreviewFrame snapshot;
    // sequence 用引擎发布的帧号：界面能据此判断"这一帧是不是新的"，
    // 也不需要我们再维护一个独立计数器（多源合并后帧号本来就要人给）。
    snapshot.sequence = frame.sequence;
    snapshot.valid    = true;
    snapshot.shapes   = frame.shapes;

    snapshot.eyes_valid  = frame.eyes.any_valid();
    snapshot.openness_l  = frame.eyes.left.openness;
    snapshot.openness_r  = frame.eyes.right.openness;
    snapshot.gaze_x_deg  = 0.5f * (frame.eyes.left.gazePitchYawDeg.y + frame.eyes.right.gazePitchYawDeg.y);
    snapshot.gaze_y_deg  = 0.5f * (frame.eyes.left.gazePitchYawDeg.x + frame.eyes.right.gazePitchYawDeg.x);

    snapshot_.publish(snapshot);
}

} // namespace fgb::params
