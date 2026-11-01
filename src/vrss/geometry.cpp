#include "vrss/geometry.hpp"

#include "core/model/math.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <sstream>

namespace fgb::vrss {
namespace {

bool finite_array(const float* values, std::size_t count) noexcept {
    for (std::size_t i = 0; i < count; ++i) {
        if (!std::isfinite(values[i])) {
            return false;
        }
    }
    return true;
}

bool parse_values(std::string_view text, std::size_t expected, float* out) noexcept {
    std::string normalized{text};
    std::replace(normalized.begin(), normalized.end(), ',', ' ');
    std::istringstream input{std::move(normalized)};
    for (std::size_t i = 0; i < expected; ++i) {
        if (!(input >> out[i]) || !std::isfinite(out[i])) {
            return false;
        }
    }
    float extra = 0.0f;
    return !(input >> extra); // 多出来的数说明格式不对
}

bool parse_u32(std::string_view text, std::uint32_t& out) noexcept {
    try {
        const unsigned long value = std::stoul(std::string(text));
        if (value > 0xFFFFFFFFul) {
            return false;
        }
        out = static_cast<std::uint32_t>(value);
        return true;
    } catch (...) {
        return false;
    }
}

} // namespace

bool EyeGeometry::valid() const noexcept {
    if (!finite_array(projection.data(), projection.size()) ||
        !finite_array(eye_to_head.data(), eye_to_head.size())) {
        return false;
    }
    // OpenVR 的投影必须跨过 0，否则逆投影没有意义。
    if (!(projection[0] < 0.0f && projection[1] > 0.0f && projection[2] < 0.0f &&
          projection[3] > 0.0f)) {
        return false;
    }
    const float* r = eye_to_head.data();
    const float det = r[0] * (r[4] * r[8] - r[5] * r[7]) -
                      r[1] * (r[3] * r[8] - r[5] * r[6]) +
                      r[2] * (r[3] * r[7] - r[4] * r[6]);
    return std::abs(det - 1.0f) <= 0.05f;
}

bool parse_geometry(std::string_view text, HmdGeometry& out, std::string& error) {
    HmdGeometry parsed;
    bool have_projection[2]{};
    bool have_rotation[2]{};
    std::istringstream lines{std::string(text)};
    std::string line;
    while (std::getline(lines, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        const size_t equal = line.find('=');
        if (equal == std::string::npos) {
            continue;
        }
        const std::string key   = line.substr(0, equal);
        const std::string value = line.substr(equal + 1);
        if (key == "vendor") {
            if (!parse_u32(value, parsed.vendor)) {
                error = "几何文件的 vendor 不合法";
                return false;
            }
        } else if (key == "product") {
            if (!parse_u32(value, parsed.product)) {
                error = "几何文件的 product 不合法";
                return false;
            }
        } else if (key == "identitySource") {
            parsed.identity_source = value;
        } else if (key == "identityFingerprint") {
            parsed.fingerprint = value;
        } else if (key.rfind("projection", 0) == 0 && key.size() == 11) {
            const int eye = key[10] - '0';
            if (eye < 0 || eye > 1 ||
                !parse_values(value, 4, parsed.eyes[eye].projection.data())) {
                error = "几何文件的投影格式不对";
                return false;
            }
            have_projection[eye] = true;
        } else if (key.rfind("eyeToHead", 0) == 0 && key.size() == 10) {
            const int eye = key[9] - '0';
            if (eye < 0 || eye > 1 ||
                !parse_values(value, 9, parsed.eyes[eye].eye_to_head.data())) {
                error = "几何文件的 eyeToHead 格式不对";
                return false;
            }
            have_rotation[eye] = true;
        }
    }
    if (!have_projection[0] || !have_projection[1] || !have_rotation[0] ||
        !have_rotation[1]) {
        error = "几何文件缺少 projection 或 eyeToHead";
        return false;
    }
    if (!parsed.valid()) {
        error = "几何文件的投影或旋转不合法";
        return false;
    }
    out = std::move(parsed);
    return true;
}

bool load_geometry(const std::filesystem::path& path, HmdGeometry& out, std::string& error) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        error = "几何文件不存在：" + path.string();
        return false;
    }
    std::string body((std::istreambuf_iterator<char>(file)), {});
    return parse_geometry(body, out, error);
}

} // namespace fgb::vrss
