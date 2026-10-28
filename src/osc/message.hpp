// OSC 1.0 公共定义。
#pragma once

#include <cstddef>
#include <cstdint>

namespace fgb::osc {

// 字符串与 blob 都按 4 字节对齐填充
constexpr std::size_t pad4(std::size_t n) noexcept { return (n + 3) & ~static_cast<std::size_t>(3); }

// "#bundle\0" (8) + timetag (8)
inline constexpr std::size_t kBundleHeaderSize = 16;
inline constexpr std::size_t kBundleMagicLen   = 8;

inline constexpr char kTagInt    = 'i';
inline constexpr char kTagFloat  = 'f';
inline constexpr char kTagString = 's';
inline constexpr char kTagBlob   = 'b';

inline constexpr std::uint32_t read_be32(const std::byte* p) noexcept {
    return (static_cast<std::uint32_t>(p[0]) << 24) | (static_cast<std::uint32_t>(p[1]) << 16) |
           (static_cast<std::uint32_t>(p[2]) << 8) | static_cast<std::uint32_t>(p[3]);
}

inline constexpr void write_be32(std::byte* p, std::uint32_t v) noexcept {
    p[0] = static_cast<std::byte>((v >> 24) & 0xFFu);
    p[1] = static_cast<std::byte>((v >> 16) & 0xFFu);
    p[2] = static_cast<std::byte>((v >> 8) & 0xFFu);
    p[3] = static_cast<std::byte>(v & 0xFFu);
}

inline constexpr std::uint64_t read_be64(const std::byte* p) noexcept {
    return (static_cast<std::uint64_t>(read_be32(p)) << 32) | static_cast<std::uint64_t>(read_be32(p + 4));
}

inline constexpr void write_be64(std::byte* p, std::uint64_t v) noexcept {
    write_be32(p, static_cast<std::uint32_t>(v >> 32));
    write_be32(p + 4, static_cast<std::uint32_t>(v & 0xFFFFFFFFULL));
}

} // namespace fgb::osc
