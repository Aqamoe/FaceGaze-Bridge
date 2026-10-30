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

const char* level_tag(int level) {
    switch (level) {
    case FGB_LOG_TRACE:
        return "trace";
    case FGB_LOG_DEBUG:
        return "debug";
    case FGB_LOG_INFO:
        return "info";
    case FGB_LOG_WARN:
        return "warn";
    default:
        return "error";
    }
}

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
    : config_(std::move(config)) {
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

FgbFrameSlotV1* NativeModuleSource::host_acquire(void* ctx) {
    if (ctx == nullptr) {
        return nullptr;
    }
    auto* self = static_cast<NativeModuleSource*>(ctx);
    std::memset(&self->slot_, 0, sizeof(self->slot_));
    self->slot_.struct_size = sizeof(FgbFrameSlotV1);
    self->slot_.abi_version = FGB_MODULE_ABI_V1;
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

void NativeModuleSource::host_commit(void* ctx, FgbFrameSlotV1* slot,
                                     uint64_t capture_ts_ns) {
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

FgbFrameSlotV2* NativeModuleSource::host_acquire_v2(void* ctx) {
    if (ctx == nullptr) {
        return nullptr;
    }
    auto* self = static_cast<NativeModuleSource*>(ctx);
    std::memset(&self->slot_v2_, 0, sizeof(self->slot_v2_));
    self->slot_v2_.struct_size = sizeof(FgbFrameSlotV2);
    self->slot_v2_.abi_version = FGB_MODULE_ABI_V2;
    self->slot_v2_.shape_count = static_cast<std::uint32_t>(kShapeCount);
    return &self->slot_v2_;
}

void NativeModuleSource::host_commit_v2(void* ctx, FgbFrameSlotV2* slot,
                                        uint64_t capture_ts_ns) {
    if (ctx == nullptr || slot == nullptr) {
        return;
    }
    auto* self = static_cast<NativeModuleSource*>(ctx);
    if (slot != &self->slot_v2_) {
        self->last_error_ = "模块提交了宿主未分配的 v2 帧槽";
        return;
    }
    self->apply_slot_v2(*slot, capture_ts_ns);
}

void NativeModuleSource::apply_slot_v2(const FgbFrameSlotV2& slot,
                                       std::uint64_t capture_ts_ns) {
    FaceFrame frame;
    frame.source = kNativeSource;
    frame.captureTsNs =
        capture_ts_ns != 0 ? capture_ts_ns
                           : (slot.capture_ts_ns != 0 ? slot.capture_ts_ns : clock::now_ns());
    frame.qualityScore = 1.0f;
    const std::uint64_t attrs = slot.attributes;
    const auto ts_of = [&frame](std::uint64_t value) {
        return value != 0 ? value : frame.captureTsNs;
    };

    if ((attrs & FGB_ATTR_EXPRESSION) != 0) {
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
    // 只填 EyeState（头像眼睛）；注视样本由 native::apply_eye_gaze_v2 单独取，
    // 因为方向位与 2D 点位是两条独立的属性，不能用"眼睛有效"来门控。
    const auto apply_eye = [&](const FgbEyeAttributesV2& in, std::uint64_t openness_bit,
                               std::uint64_t pupil_bit, std::uint64_t gaze_bit,
                               EyeState& out) {
        const bool has_openness = (attrs & openness_bit) != 0;
        const bool has_pupil    = (attrs & pupil_bit) != 0;
        const bool has_gaze     = (attrs & gaze_bit) != 0;
        if (!has_openness && !has_pupil && !has_gaze) {
            return;
        }

        out.valid         = has_openness || has_pupil;
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

    apply_eye(slot.left_eye, FGB_ATTR_LEFT_OPENNESS, FGB_ATTR_LEFT_PUPIL, FGB_ATTR_LEFT_GAZE,
              frame.eyes.left);
    apply_eye(slot.right_eye, FGB_ATTR_RIGHT_OPENNESS, FGB_ATTR_RIGHT_PUPIL,
              FGB_ATTR_RIGHT_GAZE, frame.eyes.right);

    native::apply_eye_gaze_v2(slot.left_eye, attrs, FGB_ATTR_LEFT_GAZE, FGB_ATTR_LEFT_POINT,
                              frame.captureTsNs, frame.gaze.left);
    native::apply_eye_gaze_v2(slot.right_eye, attrs, FGB_ATTR_RIGHT_GAZE, FGB_ATTR_RIGHT_POINT,
                              frame.captureTsNs, frame.gaze.right);

    if (eyes_valid) {
        frame.add_channel(channel::kEyes);
        frame.channelStamps[channel::index(channel::kEyes)].captureTsNs =
            std::max({frame.eyes.left.gazeTsNs, frame.eyes.left.opennessTsNs,
                      frame.eyes.right.gazeTsNs, frame.eyes.right.opennessTsNs,
                      frame.eyes.left.pupilTsNs, frame.eyes.right.pupilTsNs});
    }

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

    if ((attrs & FGB_ATTR_VERGENCE) != 0) {
        frame.eyes.hasVergence       = true;
        frame.eyes.vergenceDistanceM = std::max(0.0f, slot.vergence_distance_m);
        frame.eyes.vergenceTsNs      = ts_of(slot.vergence_ts_ns);
    }

    if ((attrs & FGB_ATTR_HEAD) != 0) {
        frame.hasHeadPose  = true;
        frame.headRotLocal = normalized(Quat{slot.head_rotation[0], slot.head_rotation[1],
                                             slot.head_rotation[2], slot.head_rotation[3]});
        frame.headPosLocal = Vec3{slot.head_position[0], slot.head_position[1],
                                  slot.head_position[2]};
        frame.channelStamps[channel::index(channel::kHead)].captureTsNs = ts_of(slot.head_ts_ns);
        frame.add_channel(channel::kHead);
    }

    frame.sanitize();
    pending_   = frame;
    has_frame_ = true;
}

void NativeModuleSource::apply_slot(const FgbFrameSlotV1& slot,
                                    std::uint64_t capture_ts_ns) {
    FaceFrame frame;
    frame.source     = kNativeSource;
    frame.captureTsNs =
        capture_ts_ns != 0 ? capture_ts_ns : (slot.capture_ts_ns != 0 ? slot.capture_ts_ns
                                                                      : clock::now_ns());
    frame.qualityScore = 1.0f;

    const std::uint32_t capabilities = api_ != nullptr ? api_->capabilities : 0;
    const std::uint32_t shape_count =
        std::min<std::uint32_t>(slot.shape_count, static_cast<std::uint32_t>(kShapeCount));
    if ((capabilities & FGB_CAP_EXPRESSION) != 0) {
        bool any_valid = false;
        for (std::uint32_t i = 0; i < shape_count; ++i) {
            frame.shapes[i]     = clamp01(slot.shapes[i]);
            frame.shapeValid[i] = slot.shape_valid[i] != 0 ? 1 : 0;
            any_valid           = any_valid || frame.shapeValid[i] != 0;
        }
        if (any_valid) {
            frame.add_channel(channel::kExpression);
        }
    }

    auto apply_eye = [](const FgbEyeV1& source, EyeState& out) {
        out.valid               = source.valid != 0;
        if (!out.valid) {
            return;
        }
        out.openness            = clamp01(source.openness);
        out.opennessConf        = clamp01(source.openness_confidence);
        out.pupilDiameterMm     = std::max(0.0f, source.pupil_diameter_mm);
        out.pupilDilation       = clamp_sym(source.pupil_dilation);
        out.gazeDirLocal        = normalize(Vec3{source.gaze_dir[0], source.gaze_dir[1],
                                                 source.gaze_dir[2]});
        out.gazePitchYawDeg     = dir_to_pitch_yaw_deg(out.gazeDirLocal);
        out.gazeConf            = clamp01(source.gaze_confidence);
    };
    if ((capabilities & FGB_CAP_EYE) != 0) {
        apply_eye(slot.left_eye, frame.eyes.left);
        apply_eye(slot.right_eye, frame.eyes.right);
        if (frame.eyes.any_valid()) {
            frame.add_channel(channel::kEyes);
        }
        if (slot.has_vergence != 0) {
            frame.eyes.hasVergence          = true;
            frame.eyes.vergenceDistanceM    = std::max(0.0f, slot.vergence_distance_m);
        }
    }

    // gaze 是 v1.1 追加字段：旧模块的 struct_size 不含它，不能读。
    const std::size_t gaze_need = offsetof(FgbFrameSlotV1, gaze) + sizeof(FgbGazeV1);
    if ((capabilities & FGB_CAP_GAZE) != 0 && slot.struct_size >= gaze_need) {
        native::apply_gaze_v1(slot.gaze, frame.gaze);
        if (frame.gaze.any_valid()) {
            frame.add_channel(channel::kGaze);
        }
        // 两条"模块数据看着不对"的一次性提示。连续 30 帧才报（100 Hz 下约 0.3 s），
        // 避免真的盯正前方/视线刚好扫到边缘时误报；只记日志，不改数据。
        const auto warn_once = [this](bool suspicious, int& frames, bool& warned,
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
        };
        warn_once(native::gaze_direction_looks_placeholder(frame.gaze), placeholder_frames_,
                  placeholder_warned_,
                  config_.display_name +
                      ": 注视方向恒为 (0,0,-1) 却声明了方向位，同时带 2D 点；"
                      "没有方向模型的模块应只声明 FGB_GAZE_POINT_2D，"
                      "否则 valid/fixation 会一直为真（见 docs/05-native-module-guide.md §11.2）");
        warn_once(native::gaze_point_out_of_viewport(frame.gaze), range_frames_, range_warned_,
                  config_.display_name +
                      ": 2D 注视点持续越界（远在 [0,1] 之外），坐标空间可能不是视口归一化；"
                      "约定见 docs/adr/0033-gaze-point-2d-space.md");
    }

    if ((capabilities & FGB_CAP_HEAD) != 0 && slot.has_head_pose != 0) {
        frame.hasHeadPose  = true;
        frame.headRotLocal = normalized(Quat{slot.head_rotation[0], slot.head_rotation[1],
                                             slot.head_rotation[2], slot.head_rotation[3]});
        frame.headPosLocal = Vec3{slot.head_position[0], slot.head_position[1],
                                  slot.head_position[2]};
        frame.add_channel(channel::kHead);
    }

    frame.sanitize();
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

    // v2 优先：导出 fgb_module_query_v2 的模块走属性级帧；否则回退 v1。
    using QueryV2Fn = const FgbModuleV2* (*)(uint32_t);
    const auto query_v2 = reinterpret_cast<QueryV2Fn>(
        reinterpret_cast<void*>(::GetProcAddress(static_cast<HMODULE>(library_),
                                                 "fgb_module_query_v2")));
    if (query_v2 != nullptr) {
        api_v2_ = query_v2(FGB_MODULE_ABI_V2);
        if (api_v2_ != nullptr && api_v2_->abi_version == FGB_MODULE_ABI_V2 &&
            api_v2_->struct_size >= sizeof(FgbModuleV2) && api_v2_->initialize != nullptr &&
            api_v2_->update != nullptr) {
            v2_ = true;
        } else {
            api_v2_ = nullptr;
        }
    }

    if (v2_) {
        if ((api_v2_->capabilities &
             (FGB_CAP_EYE | FGB_CAP_EYE_ATTRIBUTES | FGB_CAP_EXPRESSION | FGB_CAP_HEAD |
              FGB_CAP_GAZE)) == 0) {
            last_error_ = "模块没有声明任何能力位";
            stop();
            return false;
        }
        if (api_v2_->id != nullptr && config_.module_id != api_v2_->id) {
            FGB_WARN(kTag, "模块自报 id=%s 与清单 id=%s 不一致", api_v2_->id,
                     config_.module_id.c_str());
        }

        std::memset(&slot_v2_, 0, sizeof(slot_v2_));
        slot_v2_.struct_size = sizeof(FgbFrameSlotV2);
        slot_v2_.abi_version = FGB_MODULE_ABI_V2;
        slot_v2_.shape_count = static_cast<std::uint32_t>(kShapeCount);

        services_v2_ = FgbHostServicesV2{};
        services_v2_.struct_size          = sizeof(FgbHostServicesV2);
        services_v2_.abi_version          = FGB_MODULE_ABI_V2;
        services_v2_.log                  = &NativeModuleSource::host_log;
        services_v2_.report_status        = &NativeModuleSource::host_report;
        services_v2_.acquire_frame_slot   = &NativeModuleSource::host_acquire_v2;
        services_v2_.commit_frame_slot    = &NativeModuleSource::host_commit_v2;
        services_v2_.shape_index          = &NativeModuleSource::host_shape_index;
        services_v2_.ctx                  = this;

        FgbInitParamsV2 params{};
        params.struct_size = sizeof(FgbInitParamsV2);
        params.abi_version = FGB_MODULE_ABI_V2;
        params.tick_hz     = static_cast<uint32_t>(std::max(1, config_.tick_hz));
        params.module_dir  = module_dir_utf8_.c_str();

        void* self = nullptr;
        if (api_v2_->initialize(&self, &services_v2_, &params) == 0) {
            if (last_error_.empty()) {
                last_error_ = "模块初始化失败";
            }
            stop();
            return false;
        }
        self_        = self;
        initialized_ = true;
        alive_.store(true, std::memory_order_release);
        FGB_INFO(kTag, "原生模块已加载（v2 属性帧）：%s (%s)", config_.module_name.c_str(),
                 api_v2_->id != nullptr ? api_v2_->id : config_.module_id.c_str());
        return true;
    }

    using QueryFn = const FgbModuleV1* (*)(uint32_t);
    const auto query = reinterpret_cast<QueryFn>(
        reinterpret_cast<void*>(::GetProcAddress(static_cast<HMODULE>(library_),
                                                 "fgb_module_query")));
    if (query == nullptr) {
        last_error_ = query_v2 != nullptr
                          ? "模块的 v2 ABI 不兼容，且没有导出 v1 fgb_module_query"
                          : "模块没有导出 fgb_module_query（不是 FaceGaze Bridge 原生模块）";
        stop();
        return false;
    }
    api_ = query(FGB_MODULE_ABI_V1);
    if (api_ == nullptr || api_->abi_version != FGB_MODULE_ABI_V1 ||
        api_->struct_size < sizeof(FgbModuleV1)) {
        last_error_ = "模块 ABI 版本不兼容";
        stop();
        return false;
    }
    if (api_->initialize == nullptr || api_->update == nullptr) {
        last_error_ = "模块缺少 initialize/update 回调";
        stop();
        return false;
    }
    if ((api_->capabilities &
         (FGB_CAP_EYE | FGB_CAP_EXPRESSION | FGB_CAP_HEAD | FGB_CAP_GAZE)) == 0) {
        last_error_ = "模块没有声明任何能力位";
        stop();
        return false;
    }
    if (api_->id != nullptr && config_.module_id != api_->id) {
        FGB_WARN(kTag, "模块自报 id=%s 与清单 id=%s 不一致", api_->id,
                 config_.module_id.c_str());
    }

    std::memset(&slot_, 0, sizeof(slot_));
    slot_.struct_size = sizeof(FgbFrameSlotV1);
    slot_.abi_version = FGB_MODULE_ABI_V1;
    slot_.shape_count = static_cast<std::uint32_t>(kShapeCount);

    services_ = FgbHostServicesV1{};
    services_.struct_size          = sizeof(FgbHostServicesV1);
    services_.abi_version          = FGB_MODULE_ABI_V1;
    services_.log                  = &NativeModuleSource::host_log;
    services_.report_status        = &NativeModuleSource::host_report;
    services_.acquire_frame_slot   = &NativeModuleSource::host_acquire;
    services_.commit_frame_slot    = &NativeModuleSource::host_commit;
    services_.shape_index          = &NativeModuleSource::host_shape_index;
    services_.ctx                  = this;

    FgbInitParamsV1 params{};
    params.struct_size = sizeof(FgbInitParamsV1);
    params.abi_version = FGB_MODULE_ABI_V1;
    params.tick_hz     = static_cast<uint32_t>(std::max(1, config_.tick_hz));
    params.module_dir  = module_dir_utf8_.c_str();

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
        if (v2_ && api_v2_ != nullptr && api_v2_->teardown != nullptr) {
            api_v2_->teardown(self_);
        } else if (api_ != nullptr && api_->teardown != nullptr) {
            api_->teardown(self_);
        }
    }
    initialized_ = false;
    v2_          = false;
    self_        = nullptr;
    api_         = nullptr;
    api_v2_      = nullptr;
    alive_.store(false, std::memory_order_release);
    has_frame_ = false;
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
    if (v2_) {
        if (api_v2_ == nullptr || api_v2_->update == nullptr) {
            return false;
        }
        api_v2_->update(self_);
    } else {
        if (api_ == nullptr || api_->update == nullptr) {
            return false;
        }
        api_->update(self_);
    }
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
    status.capabilities = v2_ ? (api_v2_ != nullptr ? api_v2_->capabilities : 0)
                              : (api_ != nullptr ? api_->capabilities : 0);
    status.error        = last_error_;
    return status;
}

} // namespace fgb
