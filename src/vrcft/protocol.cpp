#include "vrcft/protocol.hpp"

#include <cstring>

namespace fgb::vrcft {
namespace {

constexpr std::size_t kNotFound = static_cast<std::size_t>(-1);

std::uint32_t read_u32(const std::byte* p) noexcept {
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}

std::int32_t read_i32(const std::byte* p) noexcept { return static_cast<std::int32_t>(read_u32(p)); }

void write_u32(std::byte* p, std::uint32_t v) noexcept {
    p[0] = static_cast<std::byte>(v & 0xFF);
    p[1] = static_cast<std::byte>((v >> 8) & 0xFF);
    p[2] = static_cast<std::byte>((v >> 16) & 0xFF);
    p[3] = static_cast<std::byte>((v >> 24) & 0xFF);
}

std::size_t find_nul(std::span<const std::byte> data, std::size_t from) noexcept {
    for (std::size_t i = from; i < data.size(); ++i) {
        if (data[i] == std::byte{0}) {
            return i;
        }
    }
    return kNotFound;
}

// 写入 8 字节包头，返回 false 表示缓冲不足
bool put_header(PacketType type, std::span<std::byte> buffer) noexcept {
    if (buffer.size() < kHeaderSize) {
        return false;
    }
    std::memcpy(buffer.data(), kMagic.data(), kMagic.size());
    write_u32(buffer.data() + 4, static_cast<std::uint32_t>(type));
    return true;
}

} // namespace

const char* to_string(PacketType type) noexcept {
    switch (type) {
    case PacketType::Handshake:
        return "Handshake";
    case PacketType::SplitPacketChunk:
        return "SplitPacketChunk";
    case PacketType::MetadataUpdate:
        return "MetadataUpdate";
    case PacketType::LipUpdate:
        return "LipUpdate";
    case PacketType::EyeUpdate:
        return "EyeUpdate";
    case PacketType::EventGetSupported:
        return "EventGetSupported";
    case PacketType::EventInit:
        return "EventInit";
    case PacketType::EventTeardown:
        return "EventTeardown";
    case PacketType::EventUpdate:
        return "EventUpdate";
    case PacketType::EventUpdateStatus:
        return "EventUpdateStatus";
    case PacketType::EventLog:
        return "EventLog";
    case PacketType::ReplyGetSupported:
        return "ReplyGetSupported";
    case PacketType::ReplyInit:
        return "ReplyInit";
    case PacketType::ReplyUpdate:
        return "ReplyUpdate";
    case PacketType::ReplyTeardown:
        return "ReplyTeardown";
    case PacketType::DebugStreamFrame:
        return "DebugStreamFrame";
    }
    return "Unknown";
}

bool parse_header(std::span<const std::byte> packet, PacketHeader& out) noexcept {
    if (packet.size() < kHeaderSize) {
        return false;
    }
    if (std::memcmp(packet.data(), kMagic.data(), kMagic.size()) != 0) {
        return false;
    }
    out.type = static_cast<PacketType>(read_u32(packet.data() + 4));
    out.body = packet.subspan(kHeaderSize);
    return true;
}

std::size_t write_simple(PacketType type, std::span<std::byte> buffer) noexcept {
    if (!put_header(type, buffer)) {
        return 0;
    }
    return kHeaderSize;
}

std::size_t write_flags(PacketType type, bool a, bool b, std::span<std::byte> buffer) noexcept {
    if (buffer.size() < kHeaderSize + 1 || !put_header(type, buffer)) {
        return 0;
    }
    const std::uint8_t packed = static_cast<std::uint8_t>((a ? 1 : 0) | (b ? 2 : 0));
    buffer[kHeaderSize]       = static_cast<std::byte>(packed);
    return kHeaderSize + 1;
}

std::size_t write_status(ModuleState state, bool using_eye, bool using_expression,
                         std::span<std::byte> buffer) noexcept {
    if (buffer.size() < kHeaderSize + 6 || !put_header(PacketType::EventUpdateStatus, buffer)) {
        return 0;
    }
    write_u32(buffer.data() + 8, static_cast<std::uint32_t>(state));
    buffer[12] = static_cast<std::byte>(using_eye ? 1 : 0);
    buffer[13] = static_cast<std::byte>(using_expression ? 1 : 0);
    return kHeaderSize + 6; // int32 + 2 个 bool（C# 的 bool 占 1 字节）
}

bool parse_handshake(std::span<const std::byte> packet, HandshakeInfo& out) noexcept {
    // magic(4) type(4) challenge(5) pid(4) pathLen(4) path
    // 注意偏移 13 / 17 是非对齐的，C# 直接 BlockCopy 拼接
    constexpr std::size_t kMinSize = 21;
    if (packet.size() < kMinSize) {
        return false;
    }
    PacketHeader header{};
    if (!parse_header(packet, header) || header.type != PacketType::Handshake) {
        return false;
    }

    out.challenge_ok = std::memcmp(packet.data() + 8, kChallenge.data(), kChallenge.size()) == 0;
    out.pid          = read_i32(packet.data() + 13);

    const std::int32_t path_len = read_i32(packet.data() + 17);
    if (path_len < 0 || 21 + static_cast<std::size_t>(path_len) > packet.size()) {
        out.module_path = {};
        return out.challenge_ok;
    }
    out.module_path = {reinterpret_cast<const char*>(packet.data() + 21),
                       static_cast<std::size_t>(path_len)};
    return out.challenge_ok;
}

bool parse_reply_supported(std::span<const std::byte> packet, ReplySupported& out) noexcept {
    if (packet.size() < kHeaderSize + 1) {
        return false;
    }
    PacketHeader header{};
    if (!parse_header(packet, header) || header.type != PacketType::ReplyGetSupported) {
        return false;
    }
    const auto flags = static_cast<std::uint8_t>(packet[kHeaderSize]);
    out.eye          = (flags & 1) != 0;
    out.expression   = (flags & 2) != 0;
    return true;
}

bool parse_reply_init(std::span<const std::byte> packet, ReplyInitInfo& out) noexcept {
    // magic(4) type(4) flags(1) nameLen(4) name streamCount(4) [streamLen(4) data]*
    if (packet.size() < kHeaderSize + 1 + 4) {
        return false;
    }
    PacketHeader header{};
    if (!parse_header(packet, header) || header.type != PacketType::ReplyInit) {
        return false;
    }

    const auto flags       = static_cast<std::uint8_t>(packet[8]);
    out.eye_success        = (flags & 1) != 0;
    out.expression_success = (flags & 2) != 0;

    const std::int32_t name_len = read_i32(packet.data() + 9);
    if (name_len < 0 || 13 + static_cast<std::size_t>(name_len) + 4 > packet.size()) {
        return false;
    }
    out.module_name = {reinterpret_cast<const char*>(packet.data() + 13),
                       static_cast<std::size_t>(name_len)};

    out.icon_count = read_i32(packet.data() + 13 + name_len);
    return true;
}

bool parse_event_log(std::span<const std::byte> packet, EventLogEntry& out) noexcept {
    if (packet.size() < kHeaderSize + 8) {
        return false;
    }
    PacketHeader header{};
    if (!parse_header(packet, header) || header.type != PacketType::EventLog) {
        return false;
    }

    out.level                   = read_i32(packet.data() + 8);
    const std::int32_t text_len = read_i32(packet.data() + 12);
    if (text_len < 0 || 16 + static_cast<std::size_t>(text_len) > packet.size()) {
        return false;
    }
    out.text = {reinterpret_cast<const char*>(packet.data() + 16), static_cast<std::size_t>(text_len)};
    return true;
}

} // namespace fgb::vrcft
