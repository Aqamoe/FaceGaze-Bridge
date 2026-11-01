#include "gaze_calib/fit.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

namespace fgb::gaze_calib {
namespace {

constexpr std::size_t kMinPairs = 6;
// 输入点在一个方向上的标准差下限：低于它就等于"所有人都在同一条线上",
// 那个方向的斜率是纯噪声放大出来的。
constexpr double kMinSpread = 0.02;
constexpr double kDetEpsilon = 1e-12;

// 3×3 高斯消元（列主元）。返回 false 表示矩阵奇异。
bool solve3(double m[3][3], double rhs[3], double out[3]) noexcept {
    int pivot_row[3] = {0, 1, 2};
    for (int col = 0; col < 3; ++col) {
        int best = col;
        for (int row = col + 1; row < 3; ++row) {
            if (std::fabs(m[pivot_row[row]][col]) > std::fabs(m[pivot_row[best]][col])) {
                best = row;
            }
        }
        std::swap(pivot_row[col], pivot_row[best]);
        const double pivot = m[pivot_row[col]][col];
        if (std::fabs(pivot) < kDetEpsilon) {
            return false;
        }
        for (int row = col + 1; row < 3; ++row) {
            const double factor = m[pivot_row[row]][col] / pivot;
            for (int k = col; k < 3; ++k) {
                m[pivot_row[row]][k] -= factor * m[pivot_row[col]][k];
            }
            rhs[pivot_row[row]] -= factor * rhs[pivot_row[col]];
        }
    }
    for (int col = 2; col >= 0; --col) {
        double value = rhs[pivot_row[col]];
        for (int k = col + 1; k < 3; ++k) {
            value -= m[pivot_row[col]][k] * out[k];
        }
        out[col] = value / m[pivot_row[col]][col];
    }
    for (int i = 0; i < 3; ++i) {
        if (!std::isfinite(out[i])) {
            return false;
        }
    }
    return true;
}

// 拟合结果是否"物理上讲得通"。手抖拟合出来的映射往往放大 10 倍以上，
// 那种映射喂给渲染比不标定还糟，所以宁可判失败。
bool plausible(const GazeEyeMap& map) noexcept {
    const auto within = [](float value, float lo, float hi) {
        return std::isfinite(value) && value >= lo && value <= hi;
    };
    return within(map.a, 0.2f, 5.0f) && within(map.e, 0.2f, 5.0f) &&
           within(map.b, -1.5f, 1.5f) && within(map.d, -1.5f, 1.5f) &&
           within(map.c, -1.5f, 1.5f) && within(map.f, -1.5f, 1.5f);
}

float percentile(std::vector<float>& values, double fraction) noexcept {
    if (values.empty()) {
        return -1.0f;
    }
    std::sort(values.begin(), values.end());
    const auto index = static_cast<std::size_t>(
        fraction * static_cast<double>(values.size() - 1) + 0.5);
    return values[std::min(index, values.size() - 1)];
}

} // namespace

EyeFit fit_affine(const std::vector<Pair>& pairs) noexcept {
    EyeFit out;
    out.used = pairs.size();
    if (pairs.size() < kMinPairs) {
        out.reason = "too-few-samples";
        return out;
    }

    double mean_x = 0.0;
    double mean_y = 0.0;
    for (const Pair& pair : pairs) {
        mean_x += static_cast<double>(pair.observed.x);
        mean_y += static_cast<double>(pair.observed.y);
    }
    mean_x /= static_cast<double>(pairs.size());
    mean_y /= static_cast<double>(pairs.size());

    double var_x = 0.0;
    double var_y = 0.0;
    for (const Pair& pair : pairs) {
        const double dx = static_cast<double>(pair.observed.x) - mean_x;
        const double dy = static_cast<double>(pair.observed.y) - mean_y;
        var_x += dx * dx;
        var_y += dy * dy;
    }
    var_x /= static_cast<double>(pairs.size());
    var_y /= static_cast<double>(pairs.size());
    // 仿射要两个方向都有变化才定得住：只有 x 有变化（所有点落在一条水平线上）时，
    // y 方向的斜率是纯噪声。用 min 而不是 max 就是为了抓这种"一条线"的输入。
    if (std::min(std::sqrt(var_x), std::sqrt(var_y)) < kMinSpread) {
        out.reason = "degenerate-input";
        return out;
    }

    double sum_x = 0.0, sum_y = 0.0, sum_xx = 0.0, sum_xy = 0.0, sum_yy = 0.0;
    double sum_tx = 0.0, sum_ty = 0.0, sum_xtx = 0.0, sum_ytx = 0.0;
    double sum_xty = 0.0, sum_yty = 0.0;
    for (const Pair& pair : pairs) {
        const double x  = static_cast<double>(pair.observed.x);
        const double y  = static_cast<double>(pair.observed.y);
        const double tx = static_cast<double>(pair.target.x);
        const double ty = static_cast<double>(pair.target.y);
        sum_x += x;
        sum_y += y;
        sum_xx += x * x;
        sum_xy += x * y;
        sum_yy += y * y;
        sum_tx += tx;
        sum_ty += ty;
        sum_xtx += x * tx;
        sum_ytx += y * tx;
        sum_xty += x * ty;
        sum_yty += y * ty;
    }
    const double n = static_cast<double>(pairs.size());

    double normal[3][3] = {{sum_xx, sum_xy, sum_x}, {sum_xy, sum_yy, sum_y}, {sum_x, sum_y, n}};
    double rhs_x[3]     = {sum_xtx, sum_ytx, sum_tx};
    double rhs_y[3]     = {sum_xty, sum_yty, sum_ty};

    double solution_x[3] = {0.0, 0.0, 0.0};
    double solution_y[3] = {0.0, 0.0, 0.0};
    double work[3][3]    = {{normal[0][0], normal[0][1], normal[0][2]},
                            {normal[1][0], normal[1][1], normal[1][2]},
                            {normal[2][0], normal[2][1], normal[2][2]}};
    if (!solve3(work, rhs_x, solution_x)) {
        out.reason = "singular-system";
        return out;
    }
    work[0][0] = normal[0][0];
    work[0][1] = normal[0][1];
    work[0][2] = normal[0][2];
    work[1][0] = normal[1][0];
    work[1][1] = normal[1][1];
    work[1][2] = normal[1][2];
    work[2][0] = normal[2][0];
    work[2][1] = normal[2][1];
    work[2][2] = normal[2][2];
    if (!solve3(work, rhs_y, solution_y)) {
        out.reason = "singular-system";
        return out;
    }

    GazeEyeMap map;
    map.active = true;
    map.a      = static_cast<float>(solution_x[0]);
    map.b      = static_cast<float>(solution_x[1]);
    map.c      = static_cast<float>(solution_x[2]);
    map.d      = static_cast<float>(solution_y[0]);
    map.e      = static_cast<float>(solution_y[1]);
    map.f      = static_cast<float>(solution_y[2]);
    if (!plausible(map)) {
        out.reason = "implausible-map";
        return out;
    }

    std::vector<float> errors;
    errors.reserve(pairs.size());
    for (const Pair& pair : pairs) {
        float px = 0.0f;
        float py = 0.0f;
        map.apply(pair.observed.x, pair.observed.y, px, py);
        errors.push_back(std::hypot(px - pair.target.x, py - pair.target.y));
    }

    out.ok           = true;
    out.map          = map;
    out.residual_p95 = percentile(errors, 0.95);
    out.reason       = "";
    return out;
}

bool build_profile(GazeSourceKind kind, const EyeFit& left, const EyeFit& right,
                   std::uint32_t version, std::string created_utc, std::string hmd_identity,
                   GazeProfile& out) {
    if (!left.ok || !right.ok || kind == GazeSourceKind::none) {
        return false;
    }
    GazeProfile profile;
    profile.version      = version;
    profile.source_kind  = kind;
    profile.created_utc  = std::move(created_utc);
    profile.hmd_identity = std::move(hmd_identity);
    profile.eyes[0]      = left.map;
    profile.eyes[1]      = right.map;
    // 整份档案只报一个残差：取两眼里更差的那只，避免"平均 1.5% 但一只眼 4%"被藏起来
    profile.residual_p95 = std::max(left.residual_p95, right.residual_p95);
    out                  = profile;
    return true;
}

} // namespace fgb::gaze_calib
