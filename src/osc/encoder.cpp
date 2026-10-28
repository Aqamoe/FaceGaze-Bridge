#include "osc/encoder.hpp"

#include <cstring>

namespace fgb::osc {
namespace {
constexpr std::byte kZero[4] = {};
}

BundleWriter::BundleWriter(std::span<std::byte> buffer) noexcept : buffer_(buffer) {}

void BundleWriter::reset() noexcept {
    size_        = 0;
    msg_len_pos_ = kNoOpenMessage;
    msg_start_   = 0;
    in_bundle_   = false;
    overflow_    = false;
}

bool BundleWriter::put(const void* src, std::size_t n) noexcept {
    if (overflow_) {
        return false;
    }
    if (size_ + n > buffer_.size()) {
        overflow_ = true;
        return false;
    }
    if (n > 0) {
        std::memcpy(buffer_.data() + size_, src, n);
        size_ += n;
    }
    return true;
}

bool BundleWriter::put_string_raw(std::string_view s) noexcept {
    const std::size_t padded = pad4(s.size() + 1);
    if (!put(s.data(), s.size())) {
        return false;
    }
    return put(kZero, padded - s.size());
}

bool BundleWriter::begin_bundle(std::uint64_t timetag) noexcept {
    if (size_ != 0 || in_bundle_) {
        return false;
    }
    static constexpr char kMagic[kBundleMagicLen] = {'#', 'b', 'u', 'n', 'd', 'l', 'e', '\0'};
    std::byte head[kBundleHeaderSize];
    std::memcpy(head, kMagic, kBundleMagicLen);
    write_be64(head + 8, timetag);
    if (!put(head, sizeof(head))) {
        return false;
    }
    in_bundle_ = true;
    return true;
}

bool BundleWriter::begin_message(std::string_view address, std::string_view type_tags) noexcept {
    if (msg_len_pos_ != kNoOpenMessage) {
        return false; // 上一条未 end
    }
    if (address.size() < 2 || address.front() != '/') {
        return false;
    }
    if (type_tags.empty() || type_tags.front() != ',') {
        return false;
    }

    msg_start_ = size_;
    if (in_bundle_) {
        // 预留 4 字节长度，end_message 时回填
        msg_len_pos_ = size_;
        std::byte placeholder[4] = {};
        if (!put(placeholder, 4)) {
            msg_len_pos_ = kNoOpenMessage;
            return false;
        }
    } else {
        msg_len_pos_ = size_; // 无长度前缀，仅作状态标记
    }

    if (!put_string_raw(address) || !put_string_raw(type_tags)) {
        return false;
    }
    return true;
}

bool BundleWriter::add_float(float value) noexcept {
    if (msg_len_pos_ == kNoOpenMessage) {
        return false;
    }
    std::uint32_t bits = 0;
    static_assert(sizeof(bits) == sizeof(value), "float 必须是 32 位");
    std::memcpy(&bits, &value, sizeof(bits));
    std::byte buf[4];
    write_be32(buf, bits);
    return put(buf, sizeof(buf));
}

bool BundleWriter::add_int(std::int32_t value) noexcept {
    if (msg_len_pos_ == kNoOpenMessage) {
        return false;
    }
    std::byte buf[4];
    write_be32(buf, static_cast<std::uint32_t>(value));
    return put(buf, sizeof(buf));
}

bool BundleWriter::add_string(std::string_view value) noexcept {
    if (msg_len_pos_ == kNoOpenMessage) {
        return false;
    }
    return put_string_raw(value);
}

bool BundleWriter::end_message() noexcept {
    if (msg_len_pos_ == kNoOpenMessage) {
        return false;
    }
    if (in_bundle_) {
        const std::size_t body = size_ - msg_len_pos_ - 4;
        write_be32(buffer_.data() + msg_len_pos_, static_cast<std::uint32_t>(body));
    }
    msg_len_pos_ = kNoOpenMessage;
    msg_start_   = 0;
    return !overflow_;
}

bool BundleWriter::write_floats(std::string_view address, std::span<const float> values) noexcept {
    char tags[64];
    if (values.size() + 1 > sizeof(tags)) {
        return false;
    }
    tags[0] = ',';
    for (std::size_t i = 0; i < values.size(); ++i) {
        tags[1 + i] = kTagFloat;
    }

    if (!begin_message(address, std::string_view{tags, values.size() + 1})) {
        return false;
    }
    for (float v : values) {
        if (!add_float(v)) {
            return false;
        }
    }
    return end_message();
}

} // namespace fgb::osc
