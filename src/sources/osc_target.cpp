#include "sources/osc_target.hpp"

#include <cctype>
#include <string>

namespace fgb {
namespace {

bool equals_ci(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i]))) {
            return false;
        }
    }
    return true;
}

bool shape_from_name_ci(std::string_view name, Shape& out) noexcept {
    if (shape_from_name(name, out)) {
        return true;
    }
    for (std::size_t i = 0; i < kShapeCount; ++i) {
        if (equals_ci(name, kShapeNames[i])) {
            out = static_cast<Shape>(i);
            return true;
        }
    }
    return false;
}

} // namespace

bool parse_osc_target(std::string_view name, OscTarget& out) noexcept {
    struct Special {
        const char* name;
        OscTargetKind kind;
    };
    static constexpr Special kSpecials[] = {
        {"EyeLidLeft", OscTargetKind::EyeOpenLeft},   {"EyeLidRight", OscTargetKind::EyeOpenRight},
        {"EyeLeftX", OscTargetKind::EyeGazeXLeft},    {"EyeLeftY", OscTargetKind::EyeGazeYLeft},
        {"EyeRightX", OscTargetKind::EyeGazeXRight},  {"EyeRightY", OscTargetKind::EyeGazeYRight},
    };

    for (const Special& special : kSpecials) {
        if (equals_ci(name, special.name)) {
            out.kind = special.kind;
            return true;
        }
    }

    Shape shape{};
    if (shape_from_name_ci(name, shape)) {
        out.kind  = OscTargetKind::Shape;
        out.shape = shape;
        return true;
    }
    return false;
}

} // namespace fgb
