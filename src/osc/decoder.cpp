#include "osc/decoder.hpp"

namespace fgb::osc {
namespace {

// 从 offset 起找 '\0'，返回其相对起点长度；找不到返回 kInvalidLen
constexpr std::size_t kNotFound = static_cast<std::size_t>(-1);

std::size_t find_nul(std::span<const std::byte> data, std::size_t offset) noexcept {
    for (std::size_t i = offset; i < data.size(); ++i) {
        if (data[i] == std::byte{0}) {
            return i - offset;
        }
    }
    return kNotFound;
}

} // namespace

MessageView::MessageView(std::span<const std::byte> packet) noexcept : data_(packet) {
    if (data_.size() < 4 || data_.front() != std::byte{'/'}) {
        return;
    }

    addr_len_ = find_nul(data_, 0);
    if (addr_len_ == kNotFound) {
        return;
    }

    tags_off_ = pad4(addr_len_ + 1);
    if (tags_off_ >= data_.size() || data_[tags_off_] != std::byte{','}) {
        return;
    }

    tags_len_ = find_nul(data_, tags_off_);
    if (tags_len_ == kNotFound || tags_len_ < 1) {
        return;
    }

    args_off_ = tags_off_ + pad4(tags_len_ + 1);
    if (args_off_ > data_.size()) {
        return;
    }

    valid_ = true;
}

std::string_view MessageView::address() const noexcept {
    if (!valid_) {
        return {};
    }
    return {reinterpret_cast<const char*>(data_.data()), addr_len_};
}

std::string_view MessageView::type_tags() const noexcept {
    if (!valid_) {
        return {};
    }
    return {reinterpret_cast<const char*>(data_.data() + tags_off_), tags_len_};
}

std::size_t MessageView::arg_count() const noexcept {
    return valid_ && tags_len_ >= 1 ? tags_len_ - 1 : 0;
}

std::size_t MessageView::byte_size() const noexcept {
    if (!valid_) {
        return 0;
    }

    const std::string_view tags = type_tags();
    std::size_t pos             = args_off_;

    for (std::size_t i = 1; i < tags.size(); ++i) {
        switch (tags[i]) {
        case kTagInt:
        case kTagFloat:
            pos += 4;
            break;
        case 'T':
        case 'F':
        case 'N':
        case 'I':
            break;
        case kTagString:
        case 'S': {
            const std::size_t len = find_nul(data_, pos);
            if (len == kNotFound) {
                return 0;
            }
            pos += pad4(len + 1);
            break;
        }
        case kTagBlob: {
            if (pos + 4 > data_.size()) {
                return 0;
            }
            const std::uint32_t len = read_be32(data_.data() + pos);
            pos += 4 + pad4(len);
            break;
        }
        default:
            return 0;
        }
        if (pos > data_.size()) {
            return 0;
        }
    }
    return pos;
}

std::size_t MessageView::arg_offset(std::size_t index, char& tag) const noexcept {
    if (!valid_ || index >= arg_count()) {
        return kInvalid;
    }

    const std::string_view tags = type_tags();
    std::size_t pos             = args_off_;

    for (std::size_t i = 1; i < tags.size(); ++i) {
        const char t = tags[i];
        if (pos > data_.size()) {
            return kInvalid;
        }

        if (i == index + 1) {
            tag = t;
            return pos;
        }

        switch (t) {
        case kTagInt:
        case kTagFloat:
            pos += 4;
            break;
        case 'T':
        case 'F':
        case 'N':
        case 'I':
            break; // 无载荷
        case kTagString:
        case 'S': {
            const std::size_t len = find_nul(data_, pos);
            if (len == kNotFound) {
                return kInvalid;
            }
            pos += pad4(len + 1);
            break;
        }
        case kTagBlob: {
            if (pos + 4 > data_.size()) {
                return kInvalid;
            }
            const std::uint32_t len = read_be32(data_.data() + pos);
            pos += 4 + pad4(len);
            break;
        }
        default:
            return kInvalid; // 未知类型，放弃解析
        }
    }
    return kInvalid;
}

bool MessageView::get_float(std::size_t index, float& out) const noexcept {
    char tag          = 0;
    const std::size_t off = arg_offset(index, tag);
    if (off == kInvalid || tag != kTagFloat || off + 4 > data_.size()) {
        return false;
    }
    const std::uint32_t bits = read_be32(data_.data() + off);
    std::memcpy(&out, &bits, sizeof(out));
    return true;
}

bool MessageView::get_int(std::size_t index, std::int32_t& out) const noexcept {
    char tag          = 0;
    const std::size_t off = arg_offset(index, tag);
    if (off == kInvalid || tag != kTagInt || off + 4 > data_.size()) {
        return false;
    }
    out = static_cast<std::int32_t>(read_be32(data_.data() + off));
    return true;
}

bool MessageView::get_string(std::size_t index, std::string_view& out) const noexcept {
    char tag          = 0;
    const std::size_t off = arg_offset(index, tag);
    if (off == kInvalid || (tag != kTagString && tag != 'S')) {
        return false;
    }
    const std::size_t len = find_nul(data_, off);
    if (len == kNotFound) {
        return false;
    }
    out = {reinterpret_cast<const char*>(data_.data() + off), len};
    return true;
}

} // namespace fgb::osc
