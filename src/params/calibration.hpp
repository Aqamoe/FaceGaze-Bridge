// 逐形状校准。
//
// 【为什么需要】
// 每个人的脸不一样，同一个模块输出的"嘴张到最大"可能是 0.6，也可能是 0.95。
// 不做映射，表情要么永远张不开，要么一直像在喊叫。VRCFT 把这件事交给模块自己的
// 配置界面（比如 PICO 模块的 config.ini 里有 jaw floor/gain），但那是模块私有格式，
// 换个模块就得重来。放在核心层做，一次校准对所有模块生效。
//
// 【怎么算】
// 两份抓包：一份放松（neutral），一份尽量夸张（express）。
//   lo = neutral 的中位数      —— 用中位数不是最小值，抗单帧抖动
//   hi = express 的 95 分位     —— 同理，躲开偶发尖峰
// 映射：t = clamp((raw - lo) / (hi - lo), 0, 1)，out = curve(t)
//
// 【为什么用抓包而不是实时向导】
// 实时向导要在引擎循环里插状态机、还要在 CLI 里做倒计时提示，代码量和出错面都大。
// 抓包已经是现成的（ADR-0007），推导过程还能反复重跑、单测覆盖。用户多敲一条命令，
// 换来的是一条完全确定的离线流程。
//
// 【lo/hi 与曲线是两件事】
// lo/hi 回答"哪里算 0、哪里算 1"，曲线只回答"中间这段怎么分配"。曲线作用在归一化
// 后的 t 上，两端锚点固定 (0,0)→(1,1)，永远不会出现"最高只到 0.8"这种没法解释的状态
// （见 ADR-0031）。把它俩混为一谈，用户就会拿曲线去干 lo/hi 的活，越调越乱。
#pragma once

#include "core/cont/seqlock.hpp"
#include "core/model/frame.hpp"
#include "core/process/face_processor.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace fgb::params {

// 曲线种类。数值进档案，别重排：只在末尾追加。
enum class CurveKind : std::uint8_t {
    linear = 0,  // 恒等
    ease_in = 1, // t²，小动作压住
    ease_out = 2, // 1-(1-t)²，轻动作就明显
    smooth = 3,  // smoothstep，中段最灵敏
    step = 4,    // 过阈值即到位（带窄过渡带，避免一帧硬跳）
    count
};

constexpr std::size_t kMaxCurvePoints = 8;
constexpr std::size_t kMaxCustomCurves = 8;
// curve 字段 >= 这个值表示"自定义曲线槽"，槽号 = curve - kCurveCustomBase。
// 低于它的值是 CurveKind：预设只占 1 字节，档案里也就只占 1 字节。
constexpr std::uint8_t kCurveCustomBase = 8;

// 启用判定的硬下限：也是界面上两端能被拖到最近的距离。
// 与推导阈值 kMinDeriveSpan 分开：前者是"能不能用"，后者是"这段数据值不值得标定"。
constexpr float kMinSpan = 0.02f;
constexpr float kMinDeriveSpan = 0.05f;

struct CurvePoint {
    float x = 0.0f;
    float y = 0.0f;
};

// 自定义曲线：单调三次 Hermite，按 x 升序，首尾固定在 (0,0)/(1,1)。
//
// 【为什么是单调 Hermite，不是折线也不是普通样条】
// 这是面捕表情的映射，输出必须连续——折线在每个控制点处有折角，放大到脸上就是"一顿一顿"。
// 但普通三次样条会过冲：曲线可能冲出相邻控制点的范围，甚至局部反向，表情就会抽。
// Fritsch–Carlson 的单调保形切线（monotone cubic Hermite）两头都占：C1 连续、不过冲、
// 求值是 4 次乘加。控制点最多 8 个，每帧重算切线也就几十次浮点运算，热路径无压力，
// 所以不缓存切线——省掉"点动了切线没动"这个必然会出现的不一致。
struct CurveShape {
    std::uint8_t count = 0;
    std::array<CurvePoint, kMaxCurvePoints> points{};
};

struct ShapeRange {
    float lo            = 0.0f;
    float hi            = 1.0f;
    bool active         = false; // false 表示这个形状不参与校准（原样透传）
    std::uint8_t curve  = 0;     // CurveKind 或 kCurveCustomBase + 槽号
    float curve_param   = 0.5f;  // step 的阈值，其余预设忽略
};

struct CalibrationProfile {
    std::array<ShapeRange, kShapeCount> shapes{};
    // 自定义曲线表：多个形状可以指向同一条，档案不会因为 88 个形状各存 8 个点而膨胀
    std::array<CurveShape, kMaxCustomCurves> custom_curves{};
    // 视线：以中性时的角度为中心，乘以增益。正常人是 1.0，眼动幅度小的可以调大
    float eye_gain_x    = 1.0f;
    float eye_gain_y    = 1.0f;
    float eye_center_x  = 0.0f;
    float eye_center_y  = 0.0f;
    bool has_eye        = false;

    std::size_t active_shape_count() const noexcept;
};

// 把归一化值过一遍曲线。t 会被夹到 0…1；未知 id 一律按线性处理（旧档案/手改档案不该炸）。
float apply_curve(const CurveShape* custom, std::size_t custom_count, std::uint8_t curve,
                  float param, float t) noexcept;

// 曲线 id ↔ 档案/JSON 里的文字形式（linear / ease_in / … / custom:N）。
// 认不出来退回 linear：手改档案写错一个字母，不该让整份校准失效。
std::string curve_to_text(std::uint8_t curve);
std::uint8_t curve_from_text(std::string_view text);

// 修一遍控制点：x 单调、y 夹到 0…1、首尾锚定 (0,0)/(1,1)、点数夹到 2…kMaxCurvePoints。
// 界面拖点、读档案、收 IPC 三处都走它，保证进到 profile 里的永远是能求值的形状。
void sanitize_curve(CurveShape& curve) noexcept;

// 界面拿它判断"有没有未保存的改动"：逐字段比较，不引入版本号
bool operator==(const CalibrationProfile& a, const CalibrationProfile& b) noexcept;
inline bool operator!=(const CalibrationProfile& a, const CalibrationProfile& b) noexcept {
    return !(a == b);
}

bool save_profile(const std::string& path, const CalibrationProfile& profile);
bool load_profile(const std::string& path, CalibrationProfile& profile);

// 界面保存用的路径：%LOCALAPPDATA%\FaceGazeBridge\calibration.ini。
// --calibration 显式指定的档案优先于它（见 ADR-0021 §"默认路径与优先级"）。
std::filesystem::path default_calibration_path();

// 先把内容写进 .tmp，再把旧档案留一份 .bak，最后替换正式文件。
// 直接覆写有个很难看的失败模式：写到一半断电/被杀，档案变成半截，下次启动校准全丢。
bool save_profile_atomic(const std::string& path, const CalibrationProfile& profile,
                         std::string& error);

// 从两份抓包推导。失败时 error 非空。
bool derive_profile(const std::string& neutral_capture, const std::string& express_capture,
                    CalibrationProfile& out, std::string& error);

// 把校准应用到帧上。没有 active 的形状原样透传。
//
// 【为什么是 seqlock 而不是 mutex】
// process() 在 120 Hz 热路径上，界面上拖滑杆时控制线程会不停 set_profile。
// 拿锁会让引擎等 UI，抖动比延迟更伤观感（见 core/cont/seqlock.hpp）。档案大约
// 2 KB（88 × 16 B + 8 条自定义曲线），整块拷贝仍然比加锁便宜得多。
// 曲线刻意不展开进 ShapeRange：那样 88 个形状各带 8 个控制点会把档案撑到 7 KB 以上，
// 每次拖动滑杆的拷贝成本就要重新算一遍账了。
class CalibrationApplier final : public IFaceProcessor {
public:
    explicit CalibrationApplier(CalibrationProfile profile = {});

    const char* name() const noexcept override { return "calibration"; }
    void process(FaceFrame& frame) noexcept override;

    // 任意线程调用：界面拖滑杆时实时生效，不需要重启引擎。
    void set_profile(const CalibrationProfile& profile) noexcept;
    // 返回拷贝：档案随时可能被控制线程换掉，返回引用会让调用方读到半截。
    CalibrationProfile profile() const noexcept;

private:
    SeqLock<CalibrationProfile> profile_snapshot_{};
    // 只由引擎线程读写：读快照失败（正在被写）时沿用上一帧，避免一次抖动
    CalibrationProfile engine_copy_{};
};

} // namespace fgb::params
