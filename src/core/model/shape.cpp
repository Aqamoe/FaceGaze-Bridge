#include "core/model/shape.hpp"

namespace fgb {

bool shape_from_name(std::string_view name, Shape& out) noexcept {
    for (std::size_t i = 0; i < kShapeCount; ++i) {
        if (name == kShapeNames[i]) {
            out = static_cast<Shape>(i);
            return true;
        }
    }
    return false;
}

} // namespace fgb
