#include "base/log_gate.hpp"

#include <algorithm>

namespace fgb::log {
namespace {

// 键只留前 256 字符：模块消息上限 1 KB，键比到尾部也认得出同类，
// 没必要为每条日志复制一整段。
constexpr std::size_t kMaxKeyLength = 256;

} // namespace

std::string repetition_key(std::string_view text) {
    std::string key;
    key.reserve(std::min(text.size(), kMaxKeyLength));
    bool in_digits = false;
    for (const char c : text) {
        if (key.size() >= kMaxKeyLength) {
            break;
        }
        const bool digit = c >= '0' && c <= '9';
        if (digit) {
            if (!in_digits) {
                key.push_back('#');
            }
        } else {
            key.push_back(c);
        }
        in_digits = digit;
    }
    return key;
}

bool RepetitionGate::window_expired(const Bucket& bucket, std::uint64_t now_ns) const noexcept {
    // 时钟回退（用系统时间做过比较、休眠唤醒）时按"到期"处理，别让消息被永久吞掉
    return now_ns < bucket.start_ns || now_ns - bucket.start_ns >= window_ns_;
}

std::string RepetitionGate::folded_text(const Bucket& bucket) {
    std::string out = "（同类消息又出现 " + std::to_string(bucket.folded) + " 次，已折叠）最后一条：";
    out += bucket.last;
    return out;
}

RepetitionGate::Bucket* RepetitionGate::find(std::string_view key) noexcept {
    for (auto& bucket : buckets_) {
        if (bucket.used && bucket.key == key) {
            return &bucket;
        }
    }
    return nullptr;
}

RepetitionGate::Bucket* RepetitionGate::slot_for(std::uint64_t now_ns,
                                                 std::string& evicted_folded) {
    for (auto& bucket : buckets_) {
        if (!bucket.used) {
            return &bucket;
        }
    }
    // 满了：顶掉窗口起点最早的那个，先把它欠下的条数结清
    Bucket* oldest = &buckets_[0];
    for (auto& bucket : buckets_) {
        if (bucket.start_ns < oldest->start_ns) {
            oldest = &bucket;
        }
    }
    if (oldest->folded > 0) {
        evicted_folded = folded_text(*oldest);
    }
    oldest->folded = 0;
    (void)now_ns;
    return oldest;
}

RepetitionGate::Emission RepetitionGate::offer(std::string_view text, std::uint64_t now_ns) {
    Emission out;
    const std::string key = repetition_key(text);
    Bucket* bucket        = find(key);
    if (bucket == nullptr) {
        std::string evicted;
        bucket = slot_for(now_ns, evicted);
        if (!evicted.empty()) {
            out.folded = std::move(evicted);
        }
        bucket->key      = key;
        bucket->last.assign(text);
        bucket->start_ns = now_ns;
        bucket->folded   = 0;
        bucket->used     = true;
        out.line.assign(text);
        return out;
    }

    if (!window_expired(*bucket, now_ns)) {
        ++bucket->folded;
        bucket->last.assign(text);
        return out;
    }

    if (bucket->folded > 0) {
        out.folded = folded_text(*bucket);
    }
    bucket->start_ns = now_ns;
    bucket->folded   = 0;
    bucket->last.assign(text);
    out.line.assign(text);
    return out;
}

std::vector<std::string> RepetitionGate::drain_expired(std::uint64_t now_ns) {
    std::vector<std::string> out;
    for (auto& bucket : buckets_) {
        if (!bucket.used || bucket.folded == 0 || !window_expired(bucket, now_ns)) {
            continue;
        }
        out.push_back(folded_text(bucket));
        bucket.folded   = 0;
        bucket.start_ns = now_ns;
    }
    return out;
}

std::vector<std::string> RepetitionGate::flush() {
    std::vector<std::string> out;
    for (auto& bucket : buckets_) {
        if (!bucket.used || bucket.folded == 0) {
            continue;
        }
        out.push_back(folded_text(bucket));
        bucket.folded = 0;
    }
    return out;
}

void RepetitionGate::reset() {
    for (auto& bucket : buckets_) {
        bucket = Bucket{};
    }
}

} // namespace fgb::log
