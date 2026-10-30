#include "sources/native_source.hpp"

#include "base/clock.hpp"
#include "base/log.hpp"
#include "sources/native_gaze_parse.hpp"

#ifdef _WIN32
#include <windows.h>
#endif

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <string>

namespace fgb {
namespace {

constexpr char kTag[]             = "native";
constexpr SourceId kNativeSource = 3;
constexpr char kQueryV2[]         = "fgb_module_query_v2";

#ifndef FGB_VERSION
#define FGB_VERSION "0.0.0"
#endif

Quat normalized(Quat q) noexcept {
    const float len = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
    if (len <= 1e-8f) {
        return Quat{};
    }
    const float inv = 1.0f / len;
    return Quat{q.x * inv, q.y * inv, q.z * inv, q.w * inv};
}

} // namespace

NativeModuleSource::NativeModuleSource(NativeSourceConfig config)
    : config_(std::move(config)), host_version_utf8_(FGB_VERSION) {
    const auto u8 = config_.dll.parent_path().u8string();
    module_dir_utf8_.assign(reinterpret_cast<const char*>(u8.data()), u8.size());
}

NativeModuleSource::~NativeModuleSource() { stop(); }

void NativeModuleSource::host_log(void* ctx, int level, const char* message) {
    if (ctx == nullptr || message == nullptr) {
        return;
    }
    auto* self = static_cast<NativeModuleSource*>(ctx);
    const std::string detail = std::string{self->config_.module_name} + ": " + message;
    switch (level) {
    case FGB_LOG_TRACE:
        FGB_TRACE(kTag, "%s", detail.c_str());
        break;
    case FGB_LOG_DEBUG:
        FGB_DEBUG(kTag, "%s", detail.c_str());
        break;
    case FGB_LOG_WARN:
        FGB_WARN(kTag, "%s", detail.c_str());
        break;
    case FGB_LOG_ERROR:
        FGB_ERROR(kTag, "%s", detail.c_str());
        break;
    default:
        FGB_INFO(kTag, "%s", detail.c_str());
        break;
    }
    if (self->config_.log_sink) {
        self->config_.log_sink(level, detail.c_str());
    }
}

void NativeModuleSource::host_report(void* ctx, int state, const char* detail) {
    if (ctx == nullptr) {
        return;
    }
    auto* self = static_cast<NativeModuleSource*>(ctx);
    switch (state) {
    case FGB_STATUS_ERROR:
        self->alive_.store(true, std::memory_order_release);
        self->last_error_ = (detail != nullptr && detail[0] != '\0') ? detail : "模块报告错误";
        FGB_WARN(kTag, "%s 报告错误：%s", self->config_.module_name.c_str(),
                 self->last_error_.c_str());
        break;
    case FGB_STATUS_STOPPED:
        self->alive_.store(false, std::memory_order_release);
        break;
    case FGB_STATUS_ACTIVE:
        self->last_error_.clear();
        self->alive_.store(true, std::memory_order_release);
        break;
    default:
        self->alive_.store(true, std::memory_order_release);
        break;
    }
}

FgbFrameSlot* NativeModuleSource::host_acquire(void* ctx) {
    if (ctx == nullptr) {
        return nullptr;
    }
    auto* self = static_cast<NativeModuleSource*>(ctx);
    std::memset(&self->slot_, 0, sizeof(self->slot_));
    self->slot_.struct_size = sizeof(FgbFrameSlot);
    self->slot_.abi_version = FGB_MODULE_ABI_V2;
    self->slot_.shape_count = static_cast<std::uint32_t>(kShapeCount);
    return &self->slot_;
}

std::int32_t NativeModuleSource::host_shape_index(void* ctx, const char* name) {
    (void)ctx;
    if (name == nullptr) {
        return -1;
    }
    for (std::size_t i = 0; i < kShapeCount; ++i) {
        if (std::strcmp(kShapeNames[i], name) == 0) {
            return static_cast<std::int32_t>(i);
        }
    }
    return -1;
}

void NativeModuleSource::host_commit(void* ctx, FgbFrameSlot* slot, uint64_t capture_ts_ns) {
    if (ctx == nullptr || slot == nullptr) {
        return;
    }
    auto* self = static_cast<NativeModuleSource*>(ctx);
    if (slot != &self->slot_) {
        self->last_error_ = "模块提交了宿主未分配的帧槽";
        return;
    }
    self->apply_slot(*slot, capture_ts_ns);
}

void NativeModuleSource::warn_once(bool suspicious, int& frames, bool& warned,
                                   const std::string& text) {
    if (warned) {
        return;
    }
    frames = suspicious ? frames + 1 : 0;
    if (frames < 30) {
        return;
    }
    warned = true;
    FGB_WARN(kTag, "%s", text.c_str());
    // 跨进程宿主把这条也转进引擎日志，不然只有 native-host-*.log 里能看到
    if (config_.log_sink) {
        config_.log_sink(FGB_LOG_WARN, text.c_str());
    }
}

void NativeModuleSource::check_capabilities(std::uint64_t attributes) {
    if (capability_warned_ || api_ == nullptr) {
        return;
    }
    const std::uint32_t caps = api_->capabilities;
    constexpr std::uint64_t kEyeAttrs =
        FGB_ATTR_LEFT_OPENNESS | FGB_ATTR_RIGHT_OPENNESS | FGB_ATTR_LEFT_PUPIL |
        FGB_ATTR_RIGHT_PUPIL | FGB_ATTR_LEFT_GAZE | FGB_ATTR_RIGHT_GAZE | FGB_ATTR_VERGENCE;
    constexpr std::uint64_t kGazeAttrs =
        FGB_ATTR_COMBINED_GAZE | FGB_ATTR_LEFT_POINT | FGB_ATTR_RIGHT_POINT;

    std::string missing;
    if ((attributes & FGB_ATTR_EXPRESSION) != 0 && (caps & FGB_CAP_EXPRESSION) == 0) {
        missing += "expression ";
    }
    if ((attributes & kEyeAttrs) != 0 && (caps & FGB_CAP_EYES) == 0) {
        missing += "eyes ";
    }
    if ((attributes & kGazeAttrs) != 0 && (caps & FGB_CAP_GAZE) == 0) {
        missing += "gaze ";
    }
    if ((attributes & FGB_ATTR_HEAD) != 0 && (caps & FGB_CAP_HEAD) == 0) {
        missing += "head ";
    }
    if (missing.empty()) {
        return;
    }
    missing.pop_back(); // 去掉结尾空格
    capability_warned_ = true;

    const std::string text = config_.display_name +
                             ": 写了 " + missing +
                             " 属性但没有在 capabilities 里声明；"
                             "数据仍会被采用，但模块列表/角色选择看不到这一路。"
                             "见 docs/05-native-module-guide.md §5";
    FGB_WARN(kTag, "%s", text.c_str());
    if (config_.log_sink) {
        config_.log_sink(FGB_LOG_WARN, text.c_str());
    }
}

void NativeModuleSource::check_gaze_diagnostics(const FaceFrame& frame) {
    if (!frame.has_channel(channel::kGaze)) {
        return;
    }
    warn_once(native::gaze_direction_looks_placeholder(frame.gaze), placeholder_frames_,
              placeholder_warned_,
              config_.display_name +
                  ": 注视方向恒为 (0,0,-1) 却声明了方向属性，同时带 2D 点；"
                  "没有方向模型的模块应只置 FGB_ATTR_LEFT/RIGHT_POINT，"
                  "否则 valid/fixation 会一直为真（见 docs/05-native-module-guide.md §6.2）");
    warn_once(native::gaze_point_out_of_viewport(frame.gaze), range_frames_, range_warned_,
              config_.display_name +
                  ": 2D 注视点持续越界（远在 [0,1] 之外），坐标空间可能不是视口归一化；"
                  "约定见 docs/adr/0033-gaze-point-2d-space.md");
}

void NativeModuleSource::apply_slot(const FgbFrameSlot& slot, std::uint64_t capture_ts_ns) {
    FaceFrame frame;
    frame.source = kNativeSource;
    frame.captureTsNs =
        capture_ts_ns != 0 ? capture_ts_ns
                           : (slot.capture_ts_ns != 0 ? slot.capture_ts_ns : clock::now_ns());
    frame.qualityScore = 1.0f;
    const std::uint64_t attrs = slot.attributes;
    check_capabilities(attrs);

    // 能力位决定这一路进哪个通道：EYES → 头像眼睛，GAZE → 渲染注视。
    // 光有属性位不算——界面按能力位展示角色，合并层也按通道选源，两条规则必须一致。
    const std::uint32_t caps = api_ != nullptr ? api_->capabilities : 0;
    const bool caps_expression = (caps & FGB_CAP_EXPRESSION) != 0;
    const bool caps_eyes       = (caps & FGB_CAP_EYES) != 0;
    const bool caps_gaze       = (caps & FGB_CAP_GAZE) != 0;
    const bool caps_head       = (caps & FGB_CAP_HEAD) != 0;
    const auto ts_of = [&frame](std::uint64_t value) {
        return value != 0 ? value : frame.captureTsNs;
    };

    if (caps_expression && (attrs & FGB_ATTR_EXPRESSION) != 0) {
        bool any_valid = false;
        const std::uint32_t shape_count =
            std::min<std::uint32_t>(slot.shape_count, static_cast<std::uint32_t>(kShapeCount));
        for (std::uint32_t i = 0; i < shape_count; ++i) {
            frame.shapes[i]     = clamp01(slot.shapes[i]);
            frame.shapeValid[i] = slot.shape_valid[i] != 0 ? 1 : 0;
            any_valid           = any_valid || frame.shapeValid[i] != 0;
        }
        if (any_valid) {
            frame.add_channel(channel::kExpression);
            frame.channelStamps[channel::index(channel::kExpression)].captureTsNs =
                ts_of(slot.expression_ts_ns);
        }
    }

    bool eyes_valid = false; // 头像可用的眼睛（至少给 openness/pupil）
    // 只填 EyeState（头像眼睛）；注视样本由 native::apply_eye_gaze 单独取，
    // 因为方向属性与 2D 点属性是两条独立的数据，不能用"眼睛有效"来门控。
    const auto apply_eye = [&](const FgbEyeAttributes& in, std::uint64_t openness_bit,
                               std::uint64_t pupil_bit, std::uint64_t gaze_bit,
                               EyeState& out) {
        const bool has_openness = (attrs & openness_bit) != 0;
        const bool has_pupil    = (attrs & pupil_bit) != 0;
        const bool has_gaze     = (attrs & gaze_bit) != 0;
        if (!has_openness && !has_pupil && !has_gaze) {
            return;
        }

        // 这一段只在模块声明了 EYES 时才会被调用，所以三样属性都算"有眼睛数据"；
        // 逐字段的 opennessValid/pupilValid/gazeValid 才是下游用来判"这项真的给了"的依据。
        // 只给注视的模块声明的是 GAZE，根本走不到这里（见 apply_slot 的能力位判断）。
        out.valid         = has_openness || has_pupil || has_gaze;
        out.opennessValid = has_openness;
        out.pupilValid    = has_pupil;
        out.gazeValid     = has_gaze;
        if (has_openness) {
            out.openness       = clamp01(in.openness);
            out.opennessConf   = clamp01(in.openness_confidence);
            out.opennessTsNs   = ts_of(in.openness_ts_ns);
        }
        if (has_pupil) {
            out.pupilDiameterMm = std::max(0.0f, in.pupil_diameter_mm);
            out.pupilDilation   = clamp_sym(in.pupil_dilation);
            out.pupilTsNs       = ts_of(in.pupil_ts_ns);
        }
        if (has_gaze) {
            out.gazeDirLocal    = normalize(Vec3{in.gaze_dir[0], in.gaze_dir[1], in.gaze_dir[2]});
            out.gazePitchYawDeg = dir_to_pitch_yaw_deg(out.gazeDirLocal);
            out.gazeConf        = clamp01(in.gaze_confidence);
            out.gazeTsNs        = ts_of(in.gaze_ts_ns);
        }
        if (out.valid) {
            eyes_valid = true;
        }
    };

    if (caps_eyes) {
        apply_eye(slot.left_eye, FGB_ATTR_LEFT_OPENNESS, FGB_ATTR_LEFT_PUPIL,
                  FGB_ATTR_LEFT_GAZE, frame.eyes.left);
        apply_eye(slot.right_eye, FGB_ATTR_RIGHT_OPENNESS, FGB_ATTR_RIGHT_PUPIL,
                  FGB_ATTR_RIGHT_GAZE, frame.eyes.right);
    }
    if (caps_gaze) {
        native::apply_eye_gaze(slot.left_eye, attrs, FGB_ATTR_LEFT_GAZE, FGB_ATTR_LEFT_POINT,
                               frame.captureTsNs, frame.gaze.left);
        native::apply_eye_gaze(slot.right_eye, attrs, FGB_ATTR_RIGHT_GAZE, FGB_ATTR_RIGHT_POINT,
                               frame.captureTsNs, frame.gaze.right);
    }

    if (eyes_valid) {
        frame.add_channel(channel::kEyes);
        frame.channelStamps[channel::index(channel::kEyes)].captureTsNs =
            std::max({frame.eyes.left.gazeTsNs, frame.eyes.left.opennessTsNs,
                      frame.eyes.right.gazeTsNs, frame.eyes.right.opennessTsNs,
                      frame.eyes.left.pupilTsNs, frame.eyes.right.pupilTsNs});
    }

    if (caps_gaze) {
        if ((attrs & FGB_ATTR_COMBINED_GAZE) != 0) {
            frame.gaze.combinedDirLocal =
                normalize(Vec3{slot.combined_gaze_dir[0], slot.combined_gaze_dir[1],
                               slot.combined_gaze_dir[2]});
            frame.gaze.combinedValid = true;
            frame.gaze.confidence    = clamp01(slot.combined_gaze_confidence);
            frame.gaze.captureTsNs   = ts_of(slot.combined_gaze_ts_ns);
        }
        if (!frame.gaze.combinedValid) {
            if (frame.gaze.left.valid && frame.gaze.right.valid) {
                frame.gaze.combinedDirLocal =
                    normalize(frame.gaze.left.dirLocal + frame.gaze.right.dirLocal);
                frame.gaze.combinedValid = true;
            } else if (frame.gaze.left.valid) {
                frame.gaze.combinedDirLocal = frame.gaze.left.dirLocal;
                frame.gaze.combinedValid    = true;
            } else if (frame.gaze.right.valid) {
                frame.gaze.combinedDirLocal = frame.gaze.right.dirLocal;
                frame.gaze.combinedValid    = true;
            }
        }
        frame.gaze.pointsValid   = frame.gaze.left.pointValid && frame.gaze.right.pointValid;
        frame.gaze.pointViewport = (attrs & FGB_ATTR_POINT_VIEWPORT) != 0;
        frame.gaze.captureTsNs =
            std::max({frame.gaze.captureTsNs, frame.gaze.left.captureTsNs,
                      frame.gaze.right.captureTsNs});
        if (frame.gaze.any_valid()) {
            frame.add_channel(channel::kGaze);
            frame.channelStamps[channel::index(channel::kGaze)].captureTsNs =
                frame.gaze.captureTsNs;
        }
    }

    // 聚散是眼睛属性：跟着 EYES 走，避免只有渲染注视的模块改掉头像眼距
    if (caps_eyes && (attrs & FGB_ATTR_VERGENCE) != 0) {
        frame.eyes.hasVergence       = true;
        frame.eyes.vergenceDistanceM = std::max(0.0f, slot.vergence_distance_m);
        frame.eyes.vergenceTsNs      = ts_of(slot.vergence_ts_ns);
    }

    if (caps_head && (attrs & FGB_ATTR_HEAD) != 0) {
        frame.hasHeadPose  = true;
        frame.headRotLocal = normalized(Quat{slot.head_rotation[0], slot.head_rotation[1],
                                             slot.head_rotation[2], slot.head_rotation[3]});
        frame.headPosLocal = Vec3{slot.head_position[0], slot.head_position[1],
                                  slot.head_position[2]};
        frame.channelStamps[channel::index(channel::kHead)].captureTsNs = ts_of(slot.head_ts_ns);
        frame.add_channel(channel::kHead);
    }

    frame.sanitize();
    check_gaze_diagnostics(frame);
    pending_   = frame;
    has_frame_ = true;
}

bool NativeModuleSource::start() {
    if (initialized_) {
        return true;
    }
    last_error_.clear();

#ifdef _WIN32
    std::error_code ec;
    if (!std::filesystem::exists(config_.dll, ec)) {
        last_error_ = "找不到模块 DLL：" + config_.dll.string();
        return false;
    }
    library_ = ::LoadLibraryW(config_.dll.c_str());
    if (library_ == nullptr) {
        last_error_ = "加载模块 DLL 失败（错误码 " + std::to_string(::GetLastError()) + "）";
        return false;
    }

    using QueryFn = const FgbModule* (*)(uint32_t);
    const auto query = reinterpret_cast<QueryFn>(
        reinterpret_cast<void*>(::GetProcAddress(static_cast<HMODULE>(library_), kQueryV2)));
    if (query == nullptr) {
        // v1 已移除：旧 DLL 的 fgb_module_query 不再被接受，这里说清楚怎么修。
        const bool legacy = ::GetProcAddress(static_cast<HMODULE>(library_),
                                             "fgb_module_query") != nullptr;
        last_error_ = legacy
                          ? "模块用的是已移除的 ABI v1，请用当前 sdk/fgb_module_api.h 重新编译"
                          : "模块没有导出 fgb_module_query_v2（不是 FaceGaze Bridge 原生模块）";
        stop();
        return false;
    }
    api_ = query(FGB_MODULE_ABI_V2);
    if (api_ == nullptr) {
        last_error_ = "模块 ABI 版本不兼容（模块不支持 ABI v2）";
        stop();
        return false;
    }
    if (api_->abi_version != FGB_MODULE_ABI_V2) {
        last_error_ = "模块 ABI 版本不兼容（模块自报 " + std::to_string(api_->abi_version) + "）";
        stop();
        return false;
    }
    // 只要求模块结构覆盖到回调函数；后续追加的尾部字段可能不在旧模块里，
    // 读取前按 struct_size 判断（ADR-0018 §2 的兼容规则）。
    const std::size_t min_module_size = offsetof(FgbModule, teardown) + sizeof(void*);
    if (api_->struct_size < min_module_size) {
        last_error_ = "模块结构过小（struct_size=" + std::to_string(api_->struct_size) +
                      "），请用当前 SDK 头重新编译";
        stop();
        return false;
    }
    if (api_->initialize == nullptr || api_->update == nullptr) {
        last_error_ = "模块缺少 initialize/update 回调";
        stop();
        return false;
    }
    if ((api_->capabilities &
         (FGB_CAP_EYES | FGB_CAP_EXPRESSION | FGB_CAP_HEAD | FGB_CAP_GAZE)) == 0) {
        last_error_ = "模块没有声明任何能力位";
        stop();
        return false;
    }
    if (api_->id != nullptr && config_.module_id != api_->id) {
        FGB_WARN(kTag, "模块自报 id=%s 与清单 id=%s 不一致", api_->id,
                 config_.module_id.c_str());
    }

    std::memset(&slot_, 0, sizeof(slot_));
    slot_.struct_size = sizeof(FgbFrameSlot);
    slot_.abi_version = FGB_MODULE_ABI_V2;
    slot_.shape_count = static_cast<std::uint32_t>(kShapeCount);

    services_ = FgbHostServices{};
    services_.struct_size        = sizeof(FgbHostServices);
    services_.abi_version        = FGB_MODULE_ABI_V2;
    services_.log                = &NativeModuleSource::host_log;
    services_.report_status      = &NativeModuleSource::host_report;
    services_.acquire_frame_slot = &NativeModuleSource::host_acquire;
    services_.commit_frame_slot  = &NativeModuleSource::host_commit;
    services_.shape_index        = &NativeModuleSource::host_shape_index;
    services_.ctx                = this;

    FgbInitParams params{};
    params.struct_size = sizeof(FgbInitParams);
    params.abi_version = FGB_MODULE_ABI_V2;
    params.tick_hz     = static_cast<uint32_t>(std::max(1, config_.tick_hz));
    params.module_dir  = module_dir_utf8_.c_str();
    params.host_version = host_version_utf8_.empty() ? nullptr : host_version_utf8_.c_str();

    void* self = nullptr;
    if (api_->initialize(&self, &services_, &params) == 0) {
        if (last_error_.empty()) {
            last_error_ = "模块初始化失败";
        }
        stop();
        return false;
    }
    self_        = self;
    initialized_ = true;
    alive_.store(true, std::memory_order_release);
    FGB_INFO(kTag, "原生模块已加载：%s (%s)", config_.module_name.c_str(),
             api_->id != nullptr ? api_->id : config_.module_id.c_str());
    return true;
#else
    last_error_ = "当前平台不支持原生模块";
    return false;
#endif
}

void NativeModuleSource::stop() noexcept {
    if (initialized_) {
        if (api_ != nullptr && api_->teardown != nullptr) {
            api_->teardown(self_);
        }
    }
    initialized_ = false;
    self_        = nullptr;
    api_         = nullptr;
    alive_.store(false, std::memory_order_release);
    has_frame_ = false;
    placeholder_frames_  = 0;
    placeholder_warned_  = false;
    range_frames_        = 0;
    range_warned_        = false;
    capability_warned_   = false;
#ifdef _WIN32
    if (library_ != nullptr) {
        ::FreeLibrary(static_cast<HMODULE>(library_));
        library_ = nullptr;
    }
#endif
}

bool NativeModuleSource::poll(FaceFrame& out) {
    if (!initialized_) {
        return false;
    }
    if (api_ == nullptr || api_->update == nullptr) {
        return false;
    }
    api_->update(self_);
    if (!has_frame_) {
        return false;
    }
    out        = pending_;
    has_frame_ = false;
    frames_.fetch_add(1, std::memory_order_relaxed);
    return true;
}

SourceStats NativeModuleSource::stats() const noexcept {
    SourceStats stats;
    stats.frames = frames_.load(std::memory_order_relaxed);
    return stats;
}

std::optional<ModuleRuntimeStatus> NativeModuleSource::module_status() const {
    ModuleRuntimeStatus status;
    status.alive        = alive_.load(std::memory_order_acquire);
    status.frames       = stats().frames;
    status.restarts     = 0;
    status.capabilities = api_ != nullptr ? api_->capabilities : 0;
    status.error        = last_error_;
    return status;
}

} // namespace fgb
