#include "core/model/gaze_profile.hpp"

namespace fgb {

const char* gaze_source_kind_name(GazeSourceKind kind) noexcept {
    switch (kind) {
    case GazeSourceKind::none: return "none";
    case GazeSourceKind::points: return "points";
    case GazeSourceKind::angles: return "angles";
    }
    return "none";
}

GazeSourceKind gaze_source_kind_from_name(const std::string& name) noexcept {
    if (name == "points") {
        return GazeSourceKind::points;
    }
    if (name == "angles") {
        return GazeSourceKind::angles;
    }
    return GazeSourceKind::none;
}

} // namespace fgb
