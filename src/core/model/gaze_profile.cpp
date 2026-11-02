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

const char* trim_state_name(TrimState state) noexcept {
    switch (state) {
    case TrimState::unknown: return "unknown";
    case TrimState::matches: return "match";
    case TrimState::stale: return "stale";
    }
    return "unknown";
}

TrimState trim_state(const GazeProfile& profile, const EyeAngleTrim& current) noexcept {
    if (!profile.have_trim) {
        return TrimState::unknown;
    }
    return profile.trim.same_effect(current) ? TrimState::matches : TrimState::stale;
}

} // namespace fgb
