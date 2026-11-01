#pragma once

#include <chrono>
#include <mutex>
#include <string_view>

#include "gaze_parse.h"

using GazeClock = std::chrono::steady_clock;

struct StereoGaze {
  ParsedEye left;
  ParsedEye right;
  double max_age_seconds = 0.0;
};

// The BrokenEye stream reports each eye's validity independently. Preserve a
// brief last-good sample for one eye while the other remains available.
class GazeState {
 public:
  void publish_frame(std::string_view json, GazeClock::time_point received_at) {
    ParsedEye left, right;
    const bool left_valid = parse_eye(json, "left", left);
    const bool right_valid = parse_eye(json, "right", right);
    std::lock_guard<std::mutex> lock(mutex_);
    if (left_valid) left_ = {left, received_at, true};
    if (right_valid) right_ = {right, received_at, true};
  }

  void clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    left_ = {};
    right_ = {};
  }

  bool read_fresh(GazeClock::time_point now, StereoGaze& out) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!left_.valid || !right_.valid) return false;
    const double left_age = std::chrono::duration<double>(now - left_.received_at).count();
    const double right_age = std::chrono::duration<double>(now - right_.received_at).count();
    if (left_age < 0.0 || right_age < 0.0 || left_age > kMaxAgeSeconds ||
        right_age > kMaxAgeSeconds) return false;
    out = {left_.gaze, right_.gaze, left_age > right_age ? left_age : right_age};
    return true;
  }

 private:
  struct Eye {
    ParsedEye gaze;
    GazeClock::time_point received_at{};
    bool valid = false;
  };
  static constexpr double kMaxAgeSeconds = 0.150;
  mutable std::mutex mutex_;
  Eye left_;
  Eye right_;
};
