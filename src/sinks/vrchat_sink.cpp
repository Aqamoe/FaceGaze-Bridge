#include "sinks/vrchat_sink.hpp"

#include "base/log.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <utility>

namespace fgb {
namespace {

constexpr char kTag[] = "vrchat";

constexpr char kAddrNativePitchYaw[] = "/tracking/eye/LeftRightPitchYaw";

// avatar_source 用稳定 token 存，界面再翻译；日志里给中文。
const char* source_label(std::string_view token) noexcept {
    if (token == "oscquery") {
        return "OSCQuery";
    }
    if (token == "config") {
        return "配置文件";
    }
    if (token == "local") {
        return "本地配置";
    }
    if (token == "heuristic") {
        return "本地配置（启发式）";
    }
    if (token == "prefix") {
        return "固定前缀";
    }
    return "未知";
}
constexpr char kAddrNativeClosed[]   = "/tracking/eye/EyesClosedAmount";

// 一条参数消息的经验长度：地址 + 对齐 + 类型标签 + 参数 + bundle 长度前缀
constexpr std::size_t kEstimatedMessageSize = 72;

bool wants_group(ParamsMode mode, std::size_t group_index) noexcept {
    const bool is_v1 = group_index >= 2; // 0=v2 基础, 1=v2 派生, 2=v1
    switch (mode) {
    case ParamsMode::v2:
        return !is_v1;
    case ParamsMode::v1:
        return is_v1;
    case ParamsMode::both:
        return true;
    }
    return true;
}

} // namespace

const char* to_string(EyeOutputMode mode) noexcept {
    switch (mode) {
    case EyeOutputMode::parameters:
        return "parameters";
    case EyeOutputMode::native:
        return "native";
    case EyeOutputMode::both:
        return "both";
    }
    return "parameters";
}

bool parse_eye_output_mode(std::string_view text, EyeOutputMode& out) noexcept {
    if (text == "parameters") {
        out = EyeOutputMode::parameters;
        return true;
    }
    if (text == "native") {
        out = EyeOutputMode::native;
        return true;
    }
    if (text == "both") {
        out = EyeOutputMode::both;
        return true;
    }
    return false;
}

const char* to_string(ParamsMode mode) noexcept {
    switch (mode) {
    case ParamsMode::v2:
        return "v2";
    case ParamsMode::v1:
        return "v1";
    case ParamsMode::both:
        return "both";
    }
    return "v2";
}

bool parse_params_mode(std::string_view text, ParamsMode& out) noexcept {
    if (text == "v2") {
        out = ParamsMode::v2;
        return true;
    }
    if (text == "v1" || text == "legacy") {
        out = ParamsMode::v1;
        return true;
    }
    if (text == "both") {
        out = ParamsMode::both;
        return true;
    }
    return false;
}

VrchatSink::VrchatSink(VrchatSinkConfig config) : config_(std::move(config)) {
    reset_dedup();
}

VrchatSink::~VrchatSink() { stop(); }

void VrchatSink::map_target_groups() {
    // 目标列表与 catalog 同序，按参数逐个对齐即可，不需要额外查找
    target_group_.assign(router_.targets().size(), 0);
    std::size_t slot             = 0;
    const auto groups            = params::groups();
    const auto& targets          = router_.targets();
    for (std::size_t g = 0; g < groups.size(); ++g) {
        for (const params::ParamEntry& entry : groups[g].entries) {
            while (slot < targets.size() && targets[slot].entry == &entry) {
                target_group_[slot] = static_cast<std::uint8_t>(g);
                ++slot;
            }
        }
    }
}

void VrchatSink::reset_dedup() noexcept {
    last_values_.assign(router_.targets().size(), 0.0f);
    sent_.assign(router_.targets().size(), 0);
    for (float& v : last_native_pitch_yaw_) {
        v = 0.0f;
    }
    last_native_closed_ = 0.0f;
    native_sent_        = false;
    throttle_.reset();
    last_heartbeat_ns_  = 0;
}

void VrchatSink::bind_avatar_locked(const params::AvatarConfig& config, std::string_view source) {
    router_.bind(config);
    avatar_name_   = config.name;
    avatar_id_     = config.id;
    avatar_source_ = source;
    bound_via_oscquery_.store(source == "oscquery");
}

void VrchatSink::use_fallback() {
    router_.use_fallback(config_.fallback_prefix);
    avatar_name_.clear();
    avatar_id_.clear();
    avatar_source_ = "prefix";
}

void VrchatSink::resolve_addresses() {
    using Clock = std::chrono::steady_clock;

    if (!config_.discover_avatar && config_.avatar_config.empty()) {
        std::unique_lock lock(router_mutex_);
        use_fallback();
        return;
    }

    // 1. 显式指定的配置文件优先
    if (!config_.avatar_config.empty()) {
        params::AvatarConfig cfg;
        if (params::load_avatar_file(config_.avatar_config, cfg)) {
            std::unique_lock lock(router_mutex_);
            bind_avatar_locked(cfg, "config");
            return;
        }
        FGB_WARN(kTag, "无法解析指定的头像配置 %s，改用自动发现", config_.avatar_config.c_str());
    }

    // 2. OSCQuery：mDNS 找到 VRChat 的动态 HTTP 端口，树里直接带当前头像与参数
    if (config_.use_oscquery) {
        std::string base = params::normalize_oscquery_base_url(config_.oscquery_url);
        std::string error;
        if (base.empty()) {
            params::OscQueryEndpoint endpoint;
            if (params::discover_oscquery_endpoint(endpoint, error, config_.oscquery_timeout_ms)) {
                base = endpoint.base_url();
            } else {
                next_discovery_ = Clock::now() + std::chrono::seconds(10);
            }
        }
        if (!base.empty()) {
            // 端点留着：头像可能只是还没加载完，watcher 每 2 s 直接重试 HTTP，
            // 不用重新走一遍 mDNS；端点真的失效时 watcher 会清掉它。
            oscquery_base_url_ = base;
            params::AvatarConfig cfg;
            const int timeout = std::max(config_.oscquery_timeout_ms, 1500);
            if (params::fetch_oscquery_avatar(base, cfg, error, timeout)) {
                next_discovery_ = {};
                std::unique_lock lock(router_mutex_);
                bind_avatar_locked(cfg, "oscquery");
                FGB_INFO(kTag, "OSCQuery 已绑定头像「%s」：%zu 个参数 / %zu 条 OSC 目标（%s）",
                         cfg.name.c_str(), router_.param_count(), router_.targets().size(),
                         oscquery_base_url_.c_str());
                return;
            }
            FGB_INFO(kTag, "OSCQuery 端点可用但拉取参数表失败（%s），先用本地配置",
                     error.c_str());
        }
    }

    // 3. 从 VRChat 日志读出当前穿着的头像 ID，精确定位
    //
    // 这里不能靠"最近修改的文件"：加载头像不更新 config.json 的 mtime，
    // 实测那份配置停在半个月前。日志里的 `Saving Avatar Data:` 才是当下的事实。
    const std::string root       = params::default_osc_root();
    const std::string current_id = params::current_avatar_id();
    params::AvatarConfig current;
    if (!current_id.empty() && params::load_avatar_by_id(root, current_id, current)) {
        std::unique_lock lock(router_mutex_);
        bind_avatar_locked(current, "local");
        if (router_.matched() == 0) {
            FGB_WARN(kTag, "当前头像「%s」没有任何面捕参数——表情不会动是正常的，"
                           "需要给这个模型加 FT 参数（或用支持面捕的模型）",
                     current.name.c_str());
        }
        return;
    }
    FGB_INFO(kTag, "未能从 VRChat 日志确认当前头像（%s），改用启发式挑选",
             current_id.empty() ? "日志里没有 Saving Avatar Data" : current_id.c_str());

    // 4. 兜底：在最近改动的若干配置里挑最像面捕头像的那份
    const params::AvatarPick pick = params::pick_avatar(root);

    if (pick.valid && pick.v2_param_count >= 8) {
        std::unique_lock lock(router_mutex_);
        bind_avatar_locked(pick.config, "heuristic");
        return;
    }

    FGB_WARN(kTag, "没找到带面捕参数的头像配置（%s，最近 %zu 份里最多只有 %zu 个 v2 参数），"
                   "回退到固定前缀 %s——如果表情不动，多半是这个前缀和头像里的参数名不一致",
             root.c_str(), static_cast<std::size_t>(8), pick.v2_param_count,
             config_.fallback_prefix.c_str());
    std::unique_lock lock(router_mutex_);
    use_fallback();
}

bool VrchatSink::start() {
    if (!socket_.open_sender()) {
        FGB_ERROR(kTag, "无法创建 UDP socket");
        return false;
    }
    resolve_addresses();
    {
        std::unique_lock lock(router_mutex_);
        map_target_groups();
        reset_dedup();
        last_seen_avatar_id_ = avatar_id_;
    }
    started_ = true;
    if (config_.discover_avatar && config_.avatar_poll_ms > 0) {
        start_watcher();
    }
    FGB_INFO(kTag, "输出到 %s:%u，眼追模式 %s，参数集 %s", config_.host.c_str(),
             static_cast<unsigned>(config_.port), to_string(config_.eye_mode),
             to_string(config_.params_mode));
    return true;
}

void VrchatSink::stop() noexcept {
    started_ = false;
    watcher_stop_.store(true);
    watcher_wake_.notify_all();
    if (watcher_.joinable()) {
        watcher_.join();
    }
    watcher_stop_.store(false);
    reload_requested_.store(false);
    socket_.close();
}

void VrchatSink::reload_avatar() {
    // 实际重绑发生在引擎线程：控制线程直接改 router_ 会和 120 Hz 的 submit 抢数据
    reload_requested_.store(true);
    watcher_wake_.notify_all();
}

void VrchatSink::start_watcher() {
    watcher_stop_.store(false);
    watcher_ = std::thread([this] { watch_avatars(); });
}

void VrchatSink::watch_avatars() {
    std::unique_lock lock(watcher_mutex_);
    while (!watcher_stop_.load(std::memory_order_acquire)) {
        watcher_wake_.wait_for(lock, std::chrono::milliseconds(config_.avatar_poll_ms), [this] {
            return watcher_stop_.load() || reload_requested_.load();
        });
        if (watcher_stop_.load()) {
            break;
        }
        const bool force = reload_requested_.exchange(false);
        lock.unlock();
        poll_avatar_once(force);
        lock.lock();
    }
}

void VrchatSink::poll_avatar_once(bool force) {
    using Clock = std::chrono::steady_clock;
    const auto now = Clock::now();

    if (config_.use_oscquery) {
        if (oscquery_base_url_.empty()) {
            if (!config_.oscquery_url.empty()) {
                oscquery_base_url_ = params::normalize_oscquery_base_url(config_.oscquery_url);
            } else if (force || now >= next_discovery_) {
                params::OscQueryEndpoint endpoint;
                std::string error;
                if (params::discover_oscquery_endpoint(endpoint, error, config_.oscquery_timeout_ms)) {
                    oscquery_base_url_ = endpoint.base_url();
                    next_discovery_    = {};
                } else {
                    next_discovery_ = now + std::chrono::seconds(10);
                }
            }
        }

        if (!oscquery_base_url_.empty()) {
            std::string id;
            std::string error;
            if (params::fetch_oscquery_avatar_change(oscquery_base_url_, id, error,
                                                     config_.oscquery_timeout_ms)) {
                // 同一个头像 ID 通常不用动；但如果当前绑的是本地 JSON（OSCQuery 刚恢复），
                // 仍要拉一次树，把来源升级成 OSCQuery。
                if (!force && bound_via_oscquery_.load() && !id.empty() &&
                    id == last_seen_avatar_id_) {
                    return;
                }

                params::AvatarConfig cfg;
                const int timeout = std::max(config_.oscquery_timeout_ms, 1500);
                if (params::fetch_oscquery_avatar(oscquery_base_url_, cfg, error, timeout)) {
                    if (cfg.id.empty()) {
                        cfg.id = id;
                    }
                    const std::string previous = last_seen_avatar_id_;
                    last_seen_avatar_id_       = cfg.id;
                    if (!previous.empty() && previous != cfg.id) {
                        FGB_INFO(kTag, "头像已切换：%s → %s（OSCQuery）", previous.c_str(),
                                 cfg.id.c_str());
                    }
                    publish_avatar(std::move(cfg), "oscquery");
                    return;
                }

                // 树拉不到时退回本地配置（Build & Test 头像有时没有本地 JSON）
                params::AvatarConfig local;
                if (!id.empty() && params::load_avatar_by_id(params::default_osc_root(), id, local)) {
                    last_seen_avatar_id_ = id;
                    publish_avatar(std::move(local), "local");
                    return;
                }

                if (config_.oscquery_url.empty()) {
                    oscquery_base_url_.clear();
                    next_discovery_ = now + std::chrono::seconds(2);
                }
            } else if (config_.oscquery_url.empty()) {
                // VRChat 重启后 OSCQuery 端口会变，清掉端点让下一轮重新 mDNS
                oscquery_base_url_.clear();
                next_discovery_ = now + std::chrono::seconds(2);
            }
        }
    }

    poll_avatar_from_log(force);
}

void VrchatSink::poll_avatar_from_log(bool force) {
    const std::string id = params::current_avatar_id();
    if (id.empty() || (!force && id == last_seen_avatar_id_)) {
        return;
    }
    params::AvatarConfig cfg;
    if (!params::load_avatar_by_id(params::default_osc_root(), id, cfg)) {
        return;
    }
    last_seen_avatar_id_ = id;
    publish_avatar(std::move(cfg), "local");
}

void VrchatSink::publish_avatar(params::AvatarConfig config, std::string source) {
    {
        std::lock_guard lock(pending_mutex_);
        pending_.config = std::move(config);
        pending_.source = std::move(source);
    }
    pending_seq_.fetch_add(1, std::memory_order_release);
}

void VrchatSink::apply_pending_avatar() {
    if (pending_seq_.load(std::memory_order_acquire) == applied_seq_) {
        return;
    }

    PendingAvatar pending;
    std::uint64_t seq = 0;
    {
        std::lock_guard lock(pending_mutex_);
        pending = pending_;
        seq     = pending_seq_.load(std::memory_order_relaxed);
    }
    std::unique_lock lock(router_mutex_);
    bind_avatar_locked(pending.config, pending.source);
    map_target_groups();
    reset_dedup();
    applied_seq_ = seq;
    FGB_INFO(kTag, "头像参数已更新（%s）：「%s」%zu 个参数 / %zu 条 OSC 目标",
             source_label(pending.source), pending.config.name.c_str(), router_.param_count(),
             router_.targets().size());
}

RouterStatus VrchatSink::status() const {
    std::shared_lock lock(router_mutex_);
    RouterStatus status;
    status.bound        = router_.bound_to_avatar();
    status.matched      = router_.matched();
    status.param_count  = router_.param_count();
    status.target_count = router_.targets().size();
    status.binary_count = router_.binary_param_count();
    status.avatar_name  = avatar_name_;
    status.avatar_id    = avatar_id_;
    status.avatar_source = avatar_source_;
    return status;
}

std::vector<std::string> VrchatSink::bound_addresses() const {
    std::shared_lock lock(router_mutex_);
    std::vector<std::string> out;
    out.reserve(router_.total());
    for (std::size_t i = 0; i < router_.total(); ++i) {
        out.push_back(router_.address(i));
    }
    return out;
}

void VrchatSink::flush(osc::BundleWriter& writer) noexcept {
    if (writer.empty()) {
        return;
    }
    const auto data = writer.data();
    if (socket_.send_to(data, config_.host, config_.port)) {
        ++stats_.messages;
        stats_.bytes += data.size();
    } else {
        ++stats_.errors;
    }
    writer.reset();
    writer.begin_bundle();
}

void VrchatSink::emit_value(osc::BundleWriter& writer, std::size_t slot, std::string_view address,
                            params::ParamKind kind, float value, bool force) noexcept {
    if (address.empty() || slot >= sent_.size()) {
        ++stats_.suppressed;
        return;
    }
    if (!std::isfinite(value)) {
        return;
    }

    const bool changed = kind == params::ParamKind::Bool
                             ? (sent_[slot] == 0) || ((value >= 0.5f) != (last_values_[slot] >= 0.5f))
                             : (sent_[slot] == 0) ||
                                   std::fabs(value - last_values_[slot]) >= config_.shape_epsilon;
    if (!force && !changed) {
        ++stats_.suppressed;
        return;
    }

    if (writer.size() + kEstimatedMessageSize > config_.max_datagram) {
        flush(writer);
    }

    const bool ok = kind == params::ParamKind::Bool
                        ? writer.write_bool(address, value >= 0.5f)
                        : [&]() {
                              const float one[1] = {value};
                              return writer.write_floats(address, one);
                          }();
    if (ok) {
        last_values_[slot] = value;
        sent_[slot]        = 1;
    }
}

void VrchatSink::emit_params(osc::BundleWriter& writer, const params::EvalContext& ctx,
                             bool force) noexcept {
    const auto& targets = router_.targets();
    const params::ParamEntry* cached  = nullptr;
    float value                       = 0.0f;

    for (std::size_t slot = 0; slot < targets.size(); ++slot) {
        const params::OutputTarget& target = targets[slot];
        if (target.entry == nullptr || slot >= target_group_.size() ||
            !wants_group(config_.params_mode, target_group_[slot])) {
            continue;
        }
        // 目标按参数聚在一起，同一个参数的多条目标只算一次
        if (target.entry != cached) {
            value  = target.entry->eval(ctx);
            cached = target.entry;
        }

        using Kind = params::OutputTarget::Kind;
        switch (target.kind) {
        case Kind::Float:
        case Kind::Bool:
            emit_value(writer, slot, target.address,
                       target.kind == Kind::Bool ? params::ParamKind::Bool
                                                 : params::ParamKind::Float,
                       value, force);
            break;
        case Kind::Negative:
            emit_value(writer, slot, target.address, params::ParamKind::Bool,
                       value < 0.0f ? 1.0f : 0.0f, force);
            break;
        case Kind::BinaryBit: {
            const bool bit = params::binary_bit(value, target.shift, target.bit_count,
                                                target.has_negative);
            emit_value(writer, slot, target.address, params::ParamKind::Bool, bit ? 1.0f : 0.0f,
                       force);
            break;
        }
        }
    }
}

void VrchatSink::emit_eye_native(osc::BundleWriter& writer, const FaceFrame& frame,
                                 bool force) noexcept {
    // VRChat 约定：pitch 正值 = 向下。我们内部正值 = 向上，这里取负。
    const float vals[4] = {
        -frame.eyes.left.gazePitchYawDeg.x,
        frame.eyes.left.gazePitchYawDeg.y,
        -frame.eyes.right.gazePitchYawDeg.x,
        frame.eyes.right.gazePitchYawDeg.y,
    };
    const float closed = clamp01(0.5f * ((1.0f - frame.eyes.left.openness) +
                                         (1.0f - frame.eyes.right.openness)));

    bool changed = !native_sent_ || force;
    for (std::size_t i = 0; i < 4; ++i) {
        if (std::fabs(vals[i] - last_native_pitch_yaw_[i]) > config_.gaze_epsilon) {
            changed = true;
        }
    }
    if (std::fabs(closed - last_native_closed_) > config_.gaze_epsilon) {
        changed = true;
    }
    if (!changed) {
        ++stats_.suppressed;
        return;
    }

    if (writer.size() + kEstimatedMessageSize * 2 > config_.max_datagram) {
        flush(writer);
    }
    if (writer.write_floats(kAddrNativePitchYaw, vals)) {
        const float one[1] = {closed};
        if (writer.write_floats(kAddrNativeClosed, one)) {
            for (std::size_t i = 0; i < 4; ++i) {
                last_native_pitch_yaw_[i] = vals[i];
            }
            last_native_closed_ = closed;
            native_sent_        = true;
        }
    }
}

void VrchatSink::submit(const FaceFrame& frame, const GazeState& /*gaze*/, std::uint64_t now_ns) {
    if (!started_) {
        return;
    }
    // 头像切换/手动重读在引擎线程落地，控制面只发布"待应用"的配置
    apply_pending_avatar();
    std::shared_lock avatar_lock(router_mutex_);

    if (!throttle_.due(now_ns, config_.min_interval_ns)) {
        return;
    }

    const bool heartbeat =
        last_heartbeat_ns_ == 0 || (now_ns - last_heartbeat_ns_) >= config_.heartbeat_ns;

    std::byte buffer[kScratchSize];
    osc::BundleWriter writer{buffer};
    if (!writer.begin_bundle()) {
        ++stats_.errors;
        return;
    }

    const bool want_params =
        config_.eye_mode == EyeOutputMode::parameters || config_.eye_mode == EyeOutputMode::both;
    const bool want_native =
        config_.eye_mode == EyeOutputMode::native || config_.eye_mode == EyeOutputMode::both;

    if (want_params) {
        const params::EvalContext ctx{frame, config_.eye_full_scale_deg};
        emit_params(writer, ctx, heartbeat);
    }

    if (want_native) {
        emit_eye_native(writer, frame, heartbeat);
    }

    flush(writer);

    if (heartbeat) {
        last_heartbeat_ns_ = now_ns;
    }
}

} // namespace fgb
