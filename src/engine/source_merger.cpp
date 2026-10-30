#include "engine/source_merger.hpp"

#include <algorithm>
#include <cctype>

namespace fgb {
namespace {

enum Channel : int {
    kExpression = 0,
    kEyes       = 1,
    kHead       = 2,
    kGaze       = 3,
};

std::string lower(std::string_view text) {
    std::string out{text};
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return out;
}

} // namespace

void SourceMerger::add(ISource* source) {
    if (source == nullptr) {
        return;
    }
    Slot slot;
    slot.source = source;
    slots_.push_back(slot);
}

bool SourceMerger::fresh(const Slot& slot, std::uint64_t now_ns,
                         std::uint64_t stale_ns) const noexcept {
    if (!slot.has_frame) {
        return false;
    }
    return now_ns >= slot.updated_ns && (now_ns - slot.updated_ns) <= stale_ns;
}

std::uint64_t SourceMerger::stale_for(int channel) const noexcept {
    switch (channel) {
    case kExpression:
        return policy_.expression_stale_ns;
    case kEyes:
        return policy_.eye_stale_ns;
    case kGaze:
        return policy_.gaze_stale_ns;
    default:
        return policy_.head_stale_ns;
    }
}

// 只认源自己声明的通道位：neutral() 里眼睛也是 valid，靠值猜会误判（见 frame.hpp）
bool SourceMerger::provides_expression(const Slot& slot) const noexcept {
    return slot.last.has_channel(channel::kExpression);
}

bool SourceMerger::provides_eyes(const Slot& slot) const noexcept {
    return slot.last.has_channel(channel::kEyes);
}

bool SourceMerger::provides_gaze(const Slot& slot) const noexcept {
    return slot.last.has_channel(channel::kGaze);
}

bool SourceMerger::provides_head(const Slot& slot) const noexcept {
    return slot.last.has_channel(channel::kHead);
}

int SourceMerger::pick(int channel, std::uint64_t now_ns) const noexcept {
    const std::string* explicit_name = nullptr;
    switch (channel) {
    case kExpression:
        explicit_name = &policy_.expression_source;
        break;
    case kEyes:
        explicit_name = &policy_.eye_source;
        break;
    case kGaze:
        explicit_name = &policy_.gaze_source;
        break;
    case kHead:
        explicit_name = &policy_.head_source;
        break;
    default:
        return -1;
    }

    auto provides = [this, channel](const Slot& slot) {
        switch (channel) {
        case kExpression:
            return provides_expression(slot);
        case kEyes:
            return provides_eyes(slot);
        case kGaze:
            return provides_gaze(slot);
        default:
            return provides_head(slot);
        }
    };
    const std::uint64_t stale_ns = stale_for(channel);

    if (!explicit_name->empty()) {
        const std::string wanted = lower(*explicit_name);
        for (std::size_t i = 0; i < slots_.size(); ++i) {
            const Slot& slot = slots_[i];
            if (slot.source == nullptr || lower(slot.source->name()) != wanted) {
                continue;
            }
            if (fresh(slot, now_ns, stale_ns) && provides(slot)) {
                return static_cast<int>(i);
            }
        }
        // 名字写错或那个源没有这一路：不整路哑掉，退回自动选择
    }

    for (std::size_t i = 0; i < slots_.size(); ++i) {
        const Slot& slot = slots_[i];
        if (slot.source == nullptr) {
            continue;
        }
        if (fresh(slot, now_ns, stale_ns) && provides(slot)) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

bool SourceMerger::poll(std::uint64_t now_ns, FaceFrame& out) {
    bool got_new = false;
    for (Slot& slot : slots_) {
        if (slot.source == nullptr) {
            continue;
        }
        FaceFrame frame;
        if (!slot.source->poll(frame)) {
            continue;
        }
        frame.sanitize();
        slot.last       = frame;
        slot.updated_ns = now_ns;
        slot.has_frame  = true;
        ++slot.frames;
        ++frames_in_;
        got_new = true;
    }

    if (!got_new) {
        return false;
    }

    const int expr = pick(kExpression, now_ns);
    const int eyes = pick(kEyes, now_ns);
    const int gaze = pick(kGaze, now_ns);
    const int head = pick(kHead, now_ns);

    // 以"提供表情的源"为基底：它通常字段最全（含时间戳、置信度）
    const int base   = expr >= 0 ? expr : (eyes >= 0 ? eyes : (gaze >= 0 ? gaze : head));
    FaceFrame merged = base >= 0 ? slots_[static_cast<std::size_t>(base)].last
                                 : FaceFrame::neutral();

    std::uint32_t bits = 0;
    auto mark          = [&bits](int index) {
        if (index >= 0 && index < 32) {
            bits |= (1u << index);
        }
    };

    // 通道声明也跟着通道走：清掉某一路时，channels 位必须一起清，
    // 否则下游（以及下一级合并）会以为这一帧还带着它
    merged.channels = 0;
    merged.channelStamps.fill(ChannelStamp{});

    if (expr >= 0) {
        mark(expr);
        if (expr != base) {
            merged.shapes     = slots_[static_cast<std::size_t>(expr)].last.shapes;
            merged.shapeValid = slots_[static_cast<std::size_t>(expr)].last.shapeValid;
        }
        merged.add_channel(channel::kExpression);
    } else {
        merged.shapes.fill(0.0f);
        merged.shapeValid.fill(0);
        merged.clear_channel(channel::kExpression);
    }

    if (eyes >= 0) {
        mark(eyes);
        merged.eyes = slots_[static_cast<std::size_t>(eyes)].last.eyes;
        merged.add_channel(channel::kEyes);
    } else {
        merged.eyes = EyePair{};
        merged.clear_channel(channel::kEyes);
    }

    if (gaze >= 0) {
        mark(gaze);
        merged.gaze = slots_[static_cast<std::size_t>(gaze)].last.gaze;
        merged.add_channel(channel::kGaze);
    } else {
        merged.gaze = GazePair{};
        merged.clear_channel(channel::kGaze);
    }

    if (head >= 0) {
        mark(head);
        merged.hasHeadPose  = slots_[static_cast<std::size_t>(head)].last.hasHeadPose;
        merged.headRotLocal = slots_[static_cast<std::size_t>(head)].last.headRotLocal;
        merged.headPosLocal = slots_[static_cast<std::size_t>(head)].last.headPosLocal;
        merged.add_channel(channel::kHead);
    } else {
        merged.hasHeadPose  = false;
        merged.headRotLocal = Quat{};
        merged.headPosLocal = Vec3{};
        merged.clear_channel(channel::kHead);
    }

    // 逐通道记录"谁供的、什么时候捕获的"。gaze 允许带自己的捕获时间，
    // 它通常比整帧时间戳更接近设备出数据的时刻。
    auto capture_ts_of = [this](int index, int channel) -> std::uint64_t {
        if (index < 0) {
            return 0;
        }
        const FaceFrame& frame = slots_[static_cast<std::size_t>(index)].last;
        if (channel == kGaze) {
            return frame.gaze.captureTsNs != 0 ? frame.gaze.captureTsNs : frame.captureTsNs;
        }
        return frame.captureTsNs;
    };
    auto stamp = [&merged, &capture_ts_of](int index, std::uint8_t mask, int channel) {
        ChannelStamp& entry      = merged.channelStamps[channel::index(mask)];
        entry.source             = index >= 0 ? static_cast<SourceId>(index + 1) : kInvalidSource;
        entry.captureTsNs        = capture_ts_of(index, channel);
    };
    stamp(expr, channel::kExpression, kExpression);
    stamp(eyes, channel::kEyes, kEyes);
    stamp(gaze, channel::kGaze, kGaze);
    stamp(head, channel::kHead, kHead);

    std::uint64_t capture_ts = 0;
    for (const ChannelStamp& entry : merged.channelStamps) {
        capture_ts = std::max(capture_ts, entry.captureTsNs);
    }
    merged.captureTsNs         = capture_ts;
    merged.contributingSources = bits;
    merged.source              = base >= 0 ? static_cast<SourceId>(base + 1) : kInvalidSource;

    out = merged;
    return true;
}

std::vector<SourceReport> SourceMerger::report(std::uint64_t now_ns) const {
    const int expr = pick(kExpression, now_ns);
    const int eyes = pick(kEyes, now_ns);
    const int gaze = pick(kGaze, now_ns);
    const int head = pick(kHead, now_ns);

    std::vector<SourceReport> out;
    out.reserve(slots_.size());
    for (std::size_t i = 0; i < slots_.size(); ++i) {
        const Slot& slot = slots_[i];
        SourceReport report;
        report.name      = slot.source != nullptr ? slot.source->name() : "";
        report.frames    = slot.frames;
        report.age_ns    = slot.has_frame && now_ns >= slot.updated_ns ? now_ns - slot.updated_ns : 0;
        // fresh 是"该源当前能不能用"，取最宽松的面捕阈值；逐通道的新鲜度由 used_* 体现。
        report.fresh               = fresh(slot, now_ns, policy_.expression_stale_ns);
        report.provides_expression = slot.has_frame && provides_expression(slot);
        report.provides_eyes       = slot.has_frame && provides_eyes(slot);
        report.provides_gaze       = slot.has_frame && provides_gaze(slot);
        report.provides_head       = slot.has_frame && provides_head(slot);
        report.used_expression     = expr == static_cast<int>(i);
        report.used_eyes           = eyes == static_cast<int>(i);
        report.used_gaze           = gaze == static_cast<int>(i);
        report.used_head           = head == static_cast<int>(i);
        out.push_back(std::move(report));
    }
    return out;
}

} // namespace fgb
