#include "sources/replay_source.hpp"

#include "base/clock.hpp"
#include "base/log.hpp"

#include <cstring>
#include <utility>

namespace fgb {
namespace {
constexpr char kTag[] = "replay";
} // namespace

ReplaySource::ReplaySource(ReplayConfig config) : config_(std::move(config)) {}

bool ReplaySource::start() {
    if (started_) {
        return true;
    }
    if (!reader_.open(config_.path)) {
        FGB_ERROR(kTag, "打开抓包文件失败：%s（%s）", config_.path.c_str(), reader_.error().c_str());
        return false;
    }
    payload_.assign(capture::kPayloadSize, std::byte{0});
    total_ = reader_.estimated_frames();
    stats_ = SourceStats{};
    finished_ = false;
    t0_       = clock::now_ns();

    have_next_ = load_next();
    if (!have_next_) {
        FGB_WARN(kTag, "抓包文件里没有帧：%s", config_.path.c_str());
        finished_ = true;
    } else {
        FGB_INFO(kTag, "回放 %s：约 %zu 帧，速度 %s", config_.path.c_str(), total_,
                 config_.speed == 0.0f ? "不限" : std::to_string(config_.speed).c_str());
    }
    started_ = true;
    return true;
}

void ReplaySource::stop() noexcept {
    started_ = false;
    reader_.close();
}

bool ReplaySource::load_next() noexcept {
    if (!reader_.next()) {
        if (config_.loop && !config_.path.empty()) {
            const SourceStats keep = stats_;
            reader_.close();
            if (reader_.open(config_.path)) {
                payload_.assign(capture::kPayloadSize, std::byte{0});
                stats_ = keep;
                t0_    = clock::now_ns();
                if (reader_.next()) {
                    const std::size_t n = reader_.payload_size();
                    if (n > payload_.size()) {
                        payload_.resize(n);
                    }
                    std::memcpy(payload_.data(), reader_.payload(), n);
                    next_ts_ = reader_.timestamp_ns();
                    return true;
                }
            }
        }
        finished_ = true;
        return false;
    }
    // 立刻拷贝：reader_ 的内部缓冲会被下一次 next() 覆盖
    const std::size_t n = reader_.payload_size();
    if (n > payload_.size()) {
        payload_.resize(n);
    }
    std::memcpy(payload_.data(), reader_.payload(), n);
    next_ts_ = reader_.timestamp_ns();
    return true;
}

std::uint64_t ReplaySource::due_ns() const noexcept {
    if (config_.speed <= 0.0f) {
        return 0; // 不限制
    }
    const double scaled = static_cast<double>(next_ts_) / static_cast<double>(config_.speed);
    return static_cast<std::uint64_t>(scaled);
}

bool ReplaySource::poll(FaceFrame& out) {
    if (!started_ || !have_next_) {
        return false;
    }

    const std::uint64_t elapsed = clock::now_ns() - t0_;
    bool produced               = false;

    // 一次 tick 可能已经过了好几帧的时间，全部消费掉，只保留最后一个状态
    while (have_next_) {
        if (elapsed < due_ns()) {
            break;
        }

        ++stats_.packets;
        vrcft::UpdateData data{};
        if (vrcft::decode_update(std::span<const std::byte>{payload_.data(), payload_.size()},
                                 data)) {
            if (data.has_any_valid()) {
                vrcft::ConvertOptions options;
                options.eye_full_scale_deg = config_.eye_full_scale_deg;
                if (vrcft::apply_to_frame(data, options, pending_)) {
                    produced = true;
                }
            }
        } else {
            ++stats_.parse_errors;
        }

        if (!load_next()) {
            have_next_ = false;
            break;
        }
    }

    if (produced) {
        // 回放的包与实时模块同源，通道声明也照 vrcft 的规则来
        pending_.channels = 0;
        for (std::size_t i = 0; i < kShapeCount; ++i) {
            if (pending_.shapeValid[i] != 0) {
                pending_.add_channel(channel::kExpression);
                break;
            }
        }
        if (pending_.eyes.any_valid()) {
            pending_.add_channel(channel::kEyes);
        }

        pending_.captureTsNs = clock::now_ns();
        pending_.sequence    = ++stats_.frames;
        pending_.sanitize();
        snapshot_.publish(pending_);
        out = pending_;
    }
    return produced;
}

SourceStats ReplaySource::stats() const noexcept { return stats_; }

} // namespace fgb
