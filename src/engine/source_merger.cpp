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

void SourceMerger::remove(ISource* source) {
    slots_.erase(std::remove_if(slots_.begin(), slots_.end(),
                                [source](const Slot& slot) { return slot.source == source; }),
                 slots_.end());
    // 策略里留下的旧名字不用清：pick() 找不到就按"没指定"回落到注册顺序，
    // 用户下次把同名实例加回来时策略仍然有效。
}

bool SourceMerger::fresh(const Slot& slot, std::uint64_t now_ns,                         std::uint64_t stale_ns) const noexcept {
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
    return pick_for(channel, *explicit_name, now_ns);
}

int SourceMerger::pick_for(int channel, const std::string& explicit_name,
                           std::uint64_t now_ns) const noexcept {
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

    if (!explicit_name.empty()) {
        const std::string wanted = lower(explicit_name);
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

int SourceMerger::pick_eye_side(bool right, std::uint64_t now_ns) const noexcept {
    const std::string& side = right ? policy_.eye_right_source : policy_.eye_left_source;
    return pick_for(kEyes, side.empty() ? policy_.eye_source : side, now_ns);
}

int SourceMerger::pick_gaze_side(bool right, std::uint64_t now_ns) const noexcept {
    const std::string& side = right ? policy_.gaze_right_source : policy_.gaze_left_source;
    return pick_for(kGaze, side.empty() ? policy_.gaze_source : side, now_ns);
}

const std::vector<std::string>* SourceMerger::attribute_list(std::string_view attribute) const {
    const std::string needle = lower(attribute);
    for (const AttributeSourceList& entry : policy_.attribute_sources) {
        if (lower(entry.attribute) == needle) {
            return &entry.sources;
        }
    }
    return nullptr;
}

bool SourceMerger::provides_attribute(const Slot& slot,
                                      std::string_view attribute) const noexcept {
    const std::string attr = lower(attribute);
    const EyeState* eye    = nullptr;
    if (attr.rfind("left-", 0) == 0) {
        eye = &slot.last.eyes.left;
    } else if (attr.rfind("right-", 0) == 0) {
        eye = &slot.last.eyes.right;
    }
    if (attr == "vergence") {
        return slot.last.eyes.hasVergence;
    }
    if (eye == nullptr || !eye->valid) {
        return false;
    }
    if (attr.size() >= 8 && attr.compare(attr.size() - 8, 8, "openness") == 0) {
        return eye->opennessValid;
    }
    if (attr.size() >= 5 && attr.compare(attr.size() - 5, 5, "pupil") == 0) {
        return eye->pupilValid;
    }
    if (attr.size() >= 4 && attr.compare(attr.size() - 4, 4, "gaze") == 0) {
        return eye->gazeValid;
    }
    return false;
}

std::vector<int> SourceMerger::attribute_candidates(std::string_view attribute,
                                                    std::uint64_t now_ns) const {
    std::vector<int> out;
    const std::vector<std::string>* sources = attribute_list(attribute);
    if (sources == nullptr || sources->empty()) {
        return out;
    }
    const std::uint64_t stale_ns = stale_for(kEyes);
    for (const std::string& wanted_name : *sources) {
        const std::string wanted = lower(wanted_name);
        for (std::size_t i = 0; i < slots_.size(); ++i) {
            const Slot& slot = slots_[i];
            if (slot.source == nullptr || lower(slot.source->name()) != wanted) {
                continue;
            }
            if (!fresh(slot, now_ns, stale_ns) || !provides_attribute(slot, attribute)) {
                continue;
            }
            if (std::find(out.begin(), out.end(), static_cast<int>(i)) == out.end()) {
                out.push_back(static_cast<int>(i));
            }
        }
    }
    return out;
}

int SourceMerger::pick_attribute(std::string_view attribute, int fallback_index,
                                 std::uint64_t now_ns) const noexcept {
    if (attribute_list(attribute) == nullptr) {
        return fallback_index;
    }
    const std::vector<int> candidates = attribute_candidates(attribute, now_ns);
    return candidates.empty() ? -1 : candidates.front();
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
    const int eye_left  = pick_eye_side(false, now_ns);
    const int eye_right = pick_eye_side(true, now_ns);
    const int gaze_left  = pick_gaze_side(false, now_ns);
    const int gaze_right = pick_gaze_side(true, now_ns);
    const int head = pick(kHead, now_ns);
    const int eyes = eye_left >= 0 ? eye_left : eye_right;
    const int gaze = gaze_left >= 0 ? gaze_left : gaze_right;

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

    merged.eyes = EyePair{};
    const auto slot_ts = [this](int index) -> std::uint64_t {
        if (index < 0) {
            return 0;
        }
        const FaceFrame& frame = slots_[static_cast<std::size_t>(index)].last;
        const auto& stamp = frame.channelStamps[channel::index(channel::kEyes)];
        return stamp.captureTsNs != 0 ? stamp.captureTsNs : frame.captureTsNs;
    };
    const auto stamp_eye = [](EyeState& eye, int index, std::uint64_t ts) {
        if (index < 0 || !eye.valid) {
            return;
        }
        const SourceId source = static_cast<SourceId>(index + 1);
        eye.opennessSource    = source;
        eye.pupilSource       = source;
        eye.gazeSource        = source;
        if (eye.opennessTsNs == 0) {
            eye.opennessTsNs = ts;
        }
        if (eye.pupilTsNs == 0) {
            eye.pupilTsNs = ts;
        }
        if (eye.gazeTsNs == 0) {
            eye.gazeTsNs = ts;
        }
    };
    const bool attribute_mode = !policy_.attribute_sources.empty();
    if (!attribute_mode) {
        if (eye_left >= 0) {
            mark(eye_left);
            merged.eyes.left = slots_[static_cast<std::size_t>(eye_left)].last.eyes.left;
            stamp_eye(merged.eyes.left, eye_left, slot_ts(eye_left));
        }
        if (eye_right >= 0) {
            mark(eye_right);
            merged.eyes.right = slots_[static_cast<std::size_t>(eye_right)].last.eyes.right;
            stamp_eye(merged.eyes.right, eye_right, slot_ts(eye_right));
        }
        if (eye_left >= 0 && slots_[static_cast<std::size_t>(eye_left)].last.eyes.hasVergence) {
            merged.eyes.hasVergence = true;
            merged.eyes.vergenceDistanceM =
                slots_[static_cast<std::size_t>(eye_left)].last.eyes.vergenceDistanceM;
            merged.eyes.vergenceSource = static_cast<SourceId>(eye_left + 1);
            merged.eyes.vergenceTsNs   = slot_ts(eye_left);
        } else if (eye_right >= 0 &&
                   slots_[static_cast<std::size_t>(eye_right)].last.eyes.hasVergence) {
            merged.eyes.hasVergence = true;
            merged.eyes.vergenceDistanceM =
                slots_[static_cast<std::size_t>(eye_right)].last.eyes.vergenceDistanceM;
            merged.eyes.vergenceSource = static_cast<SourceId>(eye_right + 1);
            merged.eyes.vergenceTsNs   = slot_ts(eye_right);
        }
    } else {
        // B.2：属性级选源。每个字段独立走“属性 → 有序源列表”；未配置的属性
        // 回退到该眼的整眼源。
        const auto attr_name = [](bool right, const char* field) {
            return std::string{right ? "right-" : "left-"} + field;
        };
        const auto source_eye = [this](int index, bool right) -> const EyeState& {
            const FaceFrame& frame = slots_[static_cast<std::size_t>(index)].last;
            return right ? frame.eyes.right : frame.eyes.left;
        };
        const auto merge_side = [&](bool right, int fallback) {
            EyeState& out = right ? merged.eyes.right : merged.eyes.left;
            out           = EyeState{};
            out.opennessValid = false;
            out.pupilValid    = false;
            out.gazeValid     = false;

            const auto candidates_for = [&](const char* field, int fb) {
                const std::string attr = attr_name(right, field);
                if (policy_.fuse_attributes && attribute_list(attr) != nullptr) {
                    return attribute_candidates(attr, now_ns);
                }
                const int idx = pick_attribute(attr, fb, now_ns);
                return idx >= 0 ? std::vector<int>{idx} : std::vector<int>{};
            };

            const std::vector<int> open_candidates = candidates_for("openness", fallback);
            if (!open_candidates.empty()) {
                if (policy_.fuse_attributes && open_candidates.size() > 1) {
                    float sum = 0.0f;
                    float weights = 0.0f;
                    float conf_sum = 0.0f;
                    std::uint64_t ts = 0;
                    for (const int index : open_candidates) {
                        const EyeState& src = source_eye(index, right);
                        if (!src.valid || !src.opennessValid) {
                            continue;
                        }
                        const float weight = std::max(0.001f, src.opennessConf);
                        sum += src.openness * weight;
                        weights += weight;
                        conf_sum += src.opennessConf;
                        ts = std::max(ts, src.opennessTsNs != 0 ? src.opennessTsNs
                                                                : slot_ts(index));
                        mark(index);
                    }
                    if (weights > 0.0f) {
                        out.openness       = clamp01(sum / weights);
                        out.opennessConf   = clamp01(conf_sum /
                                                     static_cast<float>(open_candidates.size()));
                        out.opennessTsNs   = ts;
                        out.opennessSource = kInvalidSource;
                        out.opennessValid  = true;
                    }
                } else {
                    const int index = open_candidates.front();
                    const EyeState& src = source_eye(index, right);
                    if (src.valid && src.opennessValid) {
                        out.openness       = src.openness;
                        out.opennessConf   = src.opennessConf;
                        out.opennessTsNs   = src.opennessTsNs != 0 ? src.opennessTsNs
                                                                   : slot_ts(index);
                        out.opennessSource = static_cast<SourceId>(index + 1);
                        out.opennessValid  = true;
                        mark(index);
                    }
                }
            }

            const std::vector<int> pupil_candidates = candidates_for("pupil", fallback);
            if (!pupil_candidates.empty()) {
                if (policy_.fuse_attributes && pupil_candidates.size() > 1) {
                    float diameter = 0.0f;
                    float dilation = 0.0f;
                    int count      = 0;
                    std::uint64_t ts = 0;
                    for (const int index : pupil_candidates) {
                        const EyeState& src = source_eye(index, right);
                        if (!src.valid || !src.pupilValid) {
                            continue;
                        }
                        diameter += src.pupilDiameterMm;
                        dilation += src.pupilDilation;
                        ++count;
                        ts = std::max(ts, src.pupilTsNs != 0 ? src.pupilTsNs : slot_ts(index));
                        mark(index);
                    }
                    if (count > 0) {
                        out.pupilDiameterMm = diameter / static_cast<float>(count);
                        out.pupilDilation   = clamp_sym(dilation / static_cast<float>(count));
                        out.pupilTsNs       = ts;
                        out.pupilSource     = kInvalidSource;
                        out.pupilValid      = true;
                    }
                } else {
                    const int index = pupil_candidates.front();
                    const EyeState& src = source_eye(index, right);
                    if (src.valid && src.pupilValid) {
                        out.pupilDiameterMm = src.pupilDiameterMm;
                        out.pupilDilation   = src.pupilDilation;
                        out.pupilTsNs       = src.pupilTsNs != 0 ? src.pupilTsNs
                                                                 : slot_ts(index);
                        out.pupilSource     = static_cast<SourceId>(index + 1);
                        out.pupilValid      = true;
                        mark(index);
                    }
                }
            }

            const std::vector<int> gaze_candidates = candidates_for("gaze", fallback);
            if (!gaze_candidates.empty()) {
                if (policy_.fuse_attributes && gaze_candidates.size() > 1) {
                    Vec3 dir{0.0f, 0.0f, 0.0f};
                    float weights  = 0.0f;
                    float conf_sum = 0.0f;
                    std::uint64_t ts = 0;
                    for (const int index : gaze_candidates) {
                        const EyeState& src = source_eye(index, right);
                        if (!src.valid || !src.gazeValid) {
                            continue;
                        }
                        const float weight = std::max(0.001f, src.gazeConf);
                        dir = dir + src.gazeDirLocal * weight;
                        weights += weight;
                        conf_sum += src.gazeConf;
                        ts = std::max(ts, src.gazeTsNs != 0 ? src.gazeTsNs : slot_ts(index));
                        mark(index);
                    }
                    if (weights > 0.0f) {
                        out.gazeDirLocal    = normalize(dir);
                        out.gazePitchYawDeg = dir_to_pitch_yaw_deg(out.gazeDirLocal);
                        out.gazeConf        = clamp01(conf_sum / weights);
                        out.gazeTsNs        = ts;
                        out.gazeSource      = kInvalidSource;
                        out.gazeValid       = true;
                    }
                } else {
                    const int index = gaze_candidates.front();
                    const EyeState& src = source_eye(index, right);
                    if (src.valid && src.gazeValid) {
                        out.gazeDirLocal    = src.gazeDirLocal;
                        out.gazePitchYawDeg = src.gazePitchYawDeg;
                        out.gazeConf        = src.gazeConf;
                        out.gazeTsNs        = src.gazeTsNs != 0 ? src.gazeTsNs : slot_ts(index);
                        out.gazeSource      = static_cast<SourceId>(index + 1);
                        out.gazeValid       = true;
                        mark(index);
                    }
                }
            }
            out.valid = out.opennessValid || out.pupilValid;
        };
        merge_side(false, eye_left);
        merge_side(true, eye_right);

        const int verg_fallback = eye_left >= 0 ? eye_left : eye_right;
        const int verg_idx = pick_attribute("vergence", verg_fallback, now_ns);
        if (verg_idx >= 0 &&
            slots_[static_cast<std::size_t>(verg_idx)].last.eyes.hasVergence) {
            const EyePair& src = slots_[static_cast<std::size_t>(verg_idx)].last.eyes;
            merged.eyes.hasVergence       = true;
            merged.eyes.vergenceDistanceM = src.vergenceDistanceM;
            merged.eyes.vergenceTsNs =
                src.vergenceTsNs != 0 ? src.vergenceTsNs : slot_ts(verg_idx);
            merged.eyes.vergenceSource = static_cast<SourceId>(verg_idx + 1);
            mark(verg_idx);
        }
    }

    if (merged.eyes.any_valid()) {
        ChannelStamp& entry = merged.channelStamps[channel::index(channel::kEyes)];
        if (!attribute_mode) {
            entry.source = (eye_left >= 0 && eye_left == eye_right)
                               ? static_cast<SourceId>(eye_left + 1)
                               : kInvalidSource;
            entry.captureTsNs = std::max(slot_ts(eye_left), slot_ts(eye_right));
        } else {
            SourceId unique = kInvalidSource;
            bool mixed      = false;
            const auto consider = [&unique, &mixed](SourceId value) {
                if (value == kInvalidSource) {
                    return;
                }
                if (unique == kInvalidSource) {
                    unique = value;
                } else if (unique != value) {
                    mixed = true;
                }
            };
            consider(merged.eyes.left.opennessSource);
            consider(merged.eyes.left.pupilSource);
            consider(merged.eyes.left.gazeSource);
            consider(merged.eyes.right.opennessSource);
            consider(merged.eyes.right.pupilSource);
            consider(merged.eyes.right.gazeSource);
            consider(merged.eyes.vergenceSource);
            entry.source = mixed ? kInvalidSource : unique;
            entry.captureTsNs = std::max(
                {merged.eyes.left.opennessTsNs, merged.eyes.left.pupilTsNs,
                 merged.eyes.left.gazeTsNs, merged.eyes.right.opennessTsNs,
                 merged.eyes.right.pupilTsNs, merged.eyes.right.gazeTsNs,
                 merged.eyes.vergenceTsNs});
        }
        merged.add_channel(channel::kEyes);
    } else {
        merged.clear_channel(channel::kEyes);
    }

    merged.gaze = GazePair{};
    const auto stamp_gaze = [this](GazeSample& sample, int index) {
        // 只给 2D 点的样本 valid 为假，但它照样有数据、也照样需要来源溯源
        if (index < 0 || (!sample.valid && !sample.pointValid)) {
            return;
        }
        const FaceFrame& frame = slots_[static_cast<std::size_t>(index)].last;
        sample.source          = static_cast<SourceId>(index + 1);
        if (sample.confidence <= 0.0f) {
            sample.confidence = frame.gaze.confidence;
        }
        if (sample.captureTsNs == 0) {
            sample.captureTsNs =
                frame.gaze.captureTsNs != 0 ? frame.gaze.captureTsNs : frame.captureTsNs;
        }
    };
    if (gaze_left >= 0) {
        mark(gaze_left);
        merged.gaze.left = slots_[static_cast<std::size_t>(gaze_left)].last.gaze.left;
        stamp_gaze(merged.gaze.left, gaze_left);
    }
    if (gaze_right >= 0) {
        mark(gaze_right);
        merged.gaze.right = slots_[static_cast<std::size_t>(gaze_right)].last.gaze.right;
        stamp_gaze(merged.gaze.right, gaze_right);
    }

    const bool same_gaze_source = gaze_left >= 0 && gaze_left == gaze_right;
    if (same_gaze_source) {
        const GazePair& source_gaze = slots_[static_cast<std::size_t>(gaze_left)].last.gaze;
        merged.gaze.combinedValid   = source_gaze.combinedValid;
        merged.gaze.combinedDirLocal = source_gaze.combinedDirLocal;
    }
    if (!merged.gaze.combinedValid) {
        if (merged.gaze.left.valid && merged.gaze.right.valid) {
            merged.gaze.combinedDirLocal =
                normalize(merged.gaze.left.dirLocal + merged.gaze.right.dirLocal);
            merged.gaze.combinedValid = true;
        } else if (merged.gaze.left.valid) {
            merged.gaze.combinedDirLocal = merged.gaze.left.dirLocal;
            merged.gaze.combinedValid    = true;
        } else if (merged.gaze.right.valid) {
            merged.gaze.combinedDirLocal = merged.gaze.right.dirLocal;
            merged.gaze.combinedValid    = true;
        }
    }
    merged.gaze.pointsValid = merged.gaze.left.pointValid && merged.gaze.right.pointValid;
    // 坐标空间取"两边都承认"的那一个：左右眼来自不同源时，只要有一边是原始坐标，
    // 合并结果就不能声称是视口坐标（ADR-0033）。
    {
        const auto viewport_of = [this](int index) {
            return index >= 0 && slots_[static_cast<std::size_t>(index)].last.gaze.pointViewport;
        };
        if (gaze_left >= 0 && gaze_right >= 0) {
            merged.gaze.pointViewport = viewport_of(gaze_left) && viewport_of(gaze_right);
        } else if (gaze_left >= 0) {
            merged.gaze.pointViewport = viewport_of(gaze_left);
        } else if (gaze_right >= 0) {
            merged.gaze.pointViewport = viewport_of(gaze_right);
        }
    }
    if (merged.gaze.left.valid && merged.gaze.right.valid) {
        merged.gaze.confidence = 0.5f * (merged.gaze.left.confidence + merged.gaze.right.confidence);
    } else if (merged.gaze.left.valid) {
        merged.gaze.confidence = merged.gaze.left.confidence;
    } else {
        merged.gaze.confidence = merged.gaze.right.confidence;
    }
    merged.gaze.captureTsNs = std::max(
        {merged.gaze.left.captureTsNs, merged.gaze.right.captureTsNs,
         same_gaze_source ? slots_[static_cast<std::size_t>(gaze_left)].last.gaze.captureTsNs : 0});
    if (merged.gaze.any_valid()) {
        ChannelStamp& entry = merged.channelStamps[channel::index(channel::kGaze)];
        entry.source        = same_gaze_source ? static_cast<SourceId>(gaze_left + 1)
                                               : kInvalidSource;
        entry.captureTsNs   = merged.gaze.captureTsNs;
        merged.add_channel(channel::kGaze);
    } else {
        merged.clear_channel(channel::kGaze);
    }

    // V2 阶段 A：显式开启时，头像眼睛保留 EYE 源的 openness/pupil，方向改用 Gaze 通道。
    // 默认关闭，所以“完整 A + 注视 B”的 v1.1 行为（头像眼睛仍看 A）不变。
    if (policy_.avatar_eye_from_gaze && merged.has_channel(channel::kGaze)) {
        const ChannelStamp& gaze_stamp = merged.channelStamps[channel::index(channel::kGaze)];
        const auto apply_gaze = [&merged](EyeState& eye, bool valid, Vec3 dir,
                                          float confidence, SourceId source,
                                          std::uint64_t ts) {
            if (!eye.valid || !valid) {
                return;
            }
            eye.gazeDirLocal    = dir;
            eye.gazePitchYawDeg = dir_to_pitch_yaw_deg(dir);
            eye.gazeConf        = confidence;
            eye.gazeValid       = true;
            eye.gazeSource      = source;
            eye.gazeTsNs        = ts;
            if (eye.opennessSource != source) {
                merged.channelStamps[channel::index(channel::kEyes)].source = kInvalidSource;
            }
        };
        apply_gaze(merged.eyes.left, merged.gaze.left.valid, merged.gaze.left.dirLocal,
                   merged.gaze.left.confidence, merged.gaze.left.source,
                   merged.gaze.left.captureTsNs);
        apply_gaze(merged.eyes.right, merged.gaze.right.valid, merged.gaze.right.dirLocal,
                   merged.gaze.right.confidence, merged.gaze.right.source,
                   merged.gaze.right.captureTsNs);
        if (!merged.gaze.left.valid && !merged.gaze.right.valid && merged.gaze.combinedValid) {
            apply_gaze(merged.eyes.left, true, merged.gaze.combinedDirLocal,
                       merged.gaze.confidence, gaze_stamp.source, merged.gaze.captureTsNs);
            apply_gaze(merged.eyes.right, true, merged.gaze.combinedDirLocal,
                       merged.gaze.confidence, gaze_stamp.source, merged.gaze.captureTsNs);
        }
        ChannelStamp& eyes_stamp = merged.channelStamps[channel::index(channel::kEyes)];
        eyes_stamp.captureTsNs =
            std::max(eyes_stamp.captureTsNs, merged.gaze.captureTsNs);
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
        if (channel == kGaze && frame.gaze.captureTsNs != 0) {
            return frame.gaze.captureTsNs;
        }
        // v2 模块按属性/通道给了时间戳时保留它；v1 帧没有就回到整帧时间戳。
        const auto& stamp = frame.channelStamps[static_cast<std::size_t>(channel)];
        return stamp.captureTsNs != 0 ? stamp.captureTsNs : frame.captureTsNs;
    };
    auto stamp = [&merged, &capture_ts_of](int index, std::uint8_t mask, int channel) {
        ChannelStamp& entry      = merged.channelStamps[channel::index(mask)];
        entry.source             = index >= 0 ? static_cast<SourceId>(index + 1) : kInvalidSource;
        entry.captureTsNs        = capture_ts_of(index, channel);
    };
    stamp(expr, channel::kExpression, kExpression);
    // eyes/gaze 的通道级时间戳已经按左右侧取最大值，不能再被单侧覆盖。
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
    const int eye_left   = pick_eye_side(false, now_ns);
    const int eye_right  = pick_eye_side(true, now_ns);
    const int gaze_left  = pick_gaze_side(false, now_ns);
    const int gaze_right = pick_gaze_side(true, now_ns);

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
        report.used_eyes_left      = eye_left == static_cast<int>(i);
        report.used_eyes_right     = eye_right == static_cast<int>(i);
        report.used_gaze_left      = gaze_left == static_cast<int>(i);
        report.used_gaze_right     = gaze_right == static_cast<int>(i);
        out.push_back(std::move(report));
    }
    return out;
}

} // namespace fgb
