#include "net/ws_codec.hpp"

#include <array>
#include <cstring>

namespace fgb::net::ws {
namespace {

constexpr char kGuid[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

constexpr std::uint32_t rol(std::uint32_t value, int bits) noexcept {
    return (value << bits) | (value >> (32 - bits));
}

char lower_ascii(char c) noexcept {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

std::string to_lower(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (const char c : text) {
        out.push_back(lower_ascii(c));
    }
    return out;
}

std::string_view trim(std::string_view text) noexcept {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) {
        text.remove_prefix(1);
    }
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r')) {
        text.remove_suffix(1);
    }
    return text;
}

bool iequals(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (lower_ascii(a[i]) != lower_ascii(b[i])) {
            return false;
        }
    }
    return true;
}

std::string_view reason_phrase(int status) noexcept {
    switch (status) {
    case 101:
        return "Switching Protocols";
    case 200:
        return "OK";
    case 400:
        return "Bad Request";
    case 404:
        return "Not Found";
    case 500:
        return "Internal Server Error";
    default:
        return "Unknown";
    }
}

} // namespace

std::string sha1(std::string_view data) {
    std::uint32_t h[5] = {0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u, 0xC3D2E1F0u};

    std::string message{data};
    const std::uint64_t bit_len = static_cast<std::uint64_t>(data.size()) * 8u;
    message.push_back(static_cast<char>(0x80));
    while (message.size() % 64u != 56u) {
        message.push_back('\0');
    }
    for (int i = 7; i >= 0; --i) {
        message.push_back(static_cast<char>((bit_len >> (i * 8)) & 0xFFu));
    }

    for (std::size_t chunk = 0; chunk < message.size(); chunk += 64) {
        std::uint32_t w[80]{};
        for (int i = 0; i < 16; ++i) {
            const auto* p = reinterpret_cast<const unsigned char*>(message.data() + chunk +
                                                                   static_cast<std::size_t>(i) * 4);
            w[i] = (static_cast<std::uint32_t>(p[0]) << 24) |
                   (static_cast<std::uint32_t>(p[1]) << 16) |
                   (static_cast<std::uint32_t>(p[2]) << 8) | static_cast<std::uint32_t>(p[3]);
        }
        for (int i = 16; i < 80; ++i) {
            w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        }

        std::uint32_t a = h[0];
        std::uint32_t b = h[1];
        std::uint32_t c = h[2];
        std::uint32_t d = h[3];
        std::uint32_t e = h[4];

        for (int i = 0; i < 80; ++i) {
            std::uint32_t f = 0;
            std::uint32_t k = 0;
            if (i < 20) {
                f = (b & c) | ((~b) & d);
                k = 0x5A827999u;
            } else if (i < 40) {
                f = b ^ c ^ d;
                k = 0x6ED9EBA1u;
            } else if (i < 60) {
                f = (b & c) | (b & d) | (c & d);
                k = 0x8F1BBCDCu;
            } else {
                f = b ^ c ^ d;
                k = 0xCA62C1D6u;
            }
            const std::uint32_t tmp = rol(a, 5) + f + e + k + w[i];
            e                       = d;
            d                       = c;
            c                       = rol(b, 30);
            b                       = a;
            a                       = tmp;
        }

        h[0] += a;
        h[1] += b;
        h[2] += c;
        h[3] += d;
        h[4] += e;
    }

    std::string digest(20, '\0');
    for (int i = 0; i < 5; ++i) {
        digest[static_cast<std::size_t>(i) * 4 + 0] = static_cast<char>((h[i] >> 24) & 0xFFu);
        digest[static_cast<std::size_t>(i) * 4 + 1] = static_cast<char>((h[i] >> 16) & 0xFFu);
        digest[static_cast<std::size_t>(i) * 4 + 2] = static_cast<char>((h[i] >> 8) & 0xFFu);
        digest[static_cast<std::size_t>(i) * 4 + 3] = static_cast<char>(h[i] & 0xFFu);
    }
    return digest;
}

std::string base64(std::string_view data) {
    constexpr char kAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((data.size() + 2) / 3 * 4);

    std::size_t i = 0;
    while (i + 3 <= data.size()) {
        const std::uint32_t block = (static_cast<std::uint32_t>(static_cast<unsigned char>(data[i])) << 16) |
                                    (static_cast<std::uint32_t>(static_cast<unsigned char>(data[i + 1])) << 8) |
                                    static_cast<std::uint32_t>(static_cast<unsigned char>(data[i + 2]));
        out.push_back(kAlphabet[(block >> 18) & 0x3F]);
        out.push_back(kAlphabet[(block >> 12) & 0x3F]);
        out.push_back(kAlphabet[(block >> 6) & 0x3F]);
        out.push_back(kAlphabet[block & 0x3F]);
        i += 3;
    }

    const std::size_t rest = data.size() - i;
    if (rest == 1) {
        const std::uint32_t block = static_cast<std::uint32_t>(static_cast<unsigned char>(data[i])) << 16;
        out.push_back(kAlphabet[(block >> 18) & 0x3F]);
        out.push_back(kAlphabet[(block >> 12) & 0x3F]);
        out.push_back('=');
        out.push_back('=');
    } else if (rest == 2) {
        const std::uint32_t block =
            (static_cast<std::uint32_t>(static_cast<unsigned char>(data[i])) << 16) |
            (static_cast<std::uint32_t>(static_cast<unsigned char>(data[i + 1])) << 8);
        out.push_back(kAlphabet[(block >> 18) & 0x3F]);
        out.push_back(kAlphabet[(block >> 12) & 0x3F]);
        out.push_back(kAlphabet[(block >> 6) & 0x3F]);
        out.push_back('=');
    }
    return out;
}

std::string accept_key(std::string_view client_key) {
    std::string joined{client_key};
    joined.append(kGuid);
    return base64(sha1(joined));
}

ParseResult parse_frame(std::string_view buffer, Frame& out, std::string& error) {
    if (buffer.size() < 2) {
        return ParseResult::incomplete;
    }

    const auto* bytes = reinterpret_cast<const unsigned char*>(buffer.data());
    out.fin           = (bytes[0] & 0x80u) != 0;
    out.opcode        = bytes[0] & 0x0Fu;
    out.masked        = (bytes[1] & 0x80u) != 0;

    std::uint64_t length = bytes[1] & 0x7Fu;
    std::size_t offset   = 2;
    if (length == 126) {
        if (buffer.size() < offset + 2) {
            return ParseResult::incomplete;
        }
        length = (static_cast<std::uint64_t>(bytes[offset]) << 8) |
                 static_cast<std::uint64_t>(bytes[offset + 1]);
        offset += 2;
    } else if (length == 127) {
        if (buffer.size() < offset + 8) {
            return ParseResult::incomplete;
        }
        length = 0;
        for (int i = 0; i < 8; ++i) {
            length = (length << 8) | static_cast<std::uint64_t>(bytes[offset + static_cast<std::size_t>(i)]);
        }
        offset += 8;
    }

    if (length > kMaxPayload) {
        error = "帧过大";
        return ParseResult::error;
    }

    std::array<unsigned char, 4> mask{};
    if (out.masked) {
        if (buffer.size() < offset + 4) {
            return ParseResult::incomplete;
        }
        for (std::size_t i = 0; i < 4; ++i) {
            mask[i] = bytes[offset + i];
        }
        offset += 4;
    }

    const std::size_t total = offset + static_cast<std::size_t>(length);
    if (buffer.size() < total) {
        return ParseResult::incomplete;
    }

    out.payload.assign(buffer.data() + offset, static_cast<std::size_t>(length));
    if (out.masked) {
        for (std::size_t i = 0; i < out.payload.size(); ++i) {
            out.payload[i] = static_cast<char>(static_cast<unsigned char>(out.payload[i]) ^
                                               mask[i % 4]);
        }
    }
    out.consumed = total;
    return ParseResult::ok;
}

void append_frame(std::string& out, std::uint8_t opcode, std::string_view payload) {
    out.push_back(static_cast<char>(0x80u | opcode)); // FIN + opcode，服务端不分片

    const std::size_t size = payload.size();
    if (size < 126) {
        out.push_back(static_cast<char>(size));
    } else if (size <= 0xFFFF) {
        out.push_back(static_cast<char>(126));
        out.push_back(static_cast<char>((size >> 8) & 0xFF));
        out.push_back(static_cast<char>(size & 0xFF));
    } else {
        out.push_back(static_cast<char>(127));
        for (int i = 7; i >= 0; --i) {
            out.push_back(static_cast<char>((static_cast<std::uint64_t>(size) >> (i * 8)) & 0xFF));
        }
    }
    out.append(payload.data(), payload.size());
}

HttpParse parse_http_head(std::string_view raw, HttpHead& out) {
    const std::size_t head_end = raw.find("\r\n\r\n");
    if (head_end == std::string_view::npos) {
        // 升级请求头很小，超过 16 KB 还没结束就是垃圾数据
        return raw.size() > 16384 ? HttpParse::error : HttpParse::incomplete;
    }

    out                     = HttpHead{};
    out.head_bytes          = head_end + 4;
    const std::string_view head = raw.substr(0, head_end);

    std::size_t line_begin = 0;
    bool first             = true;
    while (line_begin <= head.size()) {
        std::size_t line_end = head.find("\r\n", line_begin);
        if (line_end == std::string_view::npos) {
            line_end = head.size();
        }
        const std::string_view line = head.substr(line_begin, line_end - line_begin);

        if (first) {
            first = false;
            const std::size_t sp1 = line.find(' ');
            const std::size_t sp2 = sp1 == std::string_view::npos ? std::string_view::npos
                                                                  : line.find(' ', sp1 + 1);
            if (sp1 == std::string_view::npos || sp2 == std::string_view::npos) {
                return HttpParse::error;
            }
            out.method = std::string{line.substr(0, sp1)};
            const std::string_view target = line.substr(sp1 + 1, sp2 - sp1 - 1);
            const std::size_t qmark       = target.find('?');
            if (qmark == std::string_view::npos) {
                out.path = std::string{target};
            } else {
                out.path  = std::string{target.substr(0, qmark)};
                out.query = std::string{target.substr(qmark + 1)};
            }
        } else if (!line.empty()) {
            const std::size_t colon = line.find(':');
            if (colon == std::string_view::npos) {
                return HttpParse::error;
            }
            const std::string_view name  = trim(line.substr(0, colon));
            const std::string_view value = trim(line.substr(colon + 1));
            if (iequals(name, "Upgrade")) {
                out.upgrade = to_lower(value);
            } else if (iequals(name, "Sec-WebSocket-Key")) {
                out.ws_key = std::string{value};
            } else if (iequals(name, "Sec-WebSocket-Version")) {
                out.ws_version = std::string{value};
            }
        }

        if (line_end >= head.size()) {
            break;
        }
        line_begin = line_end + 2;
    }

    if (out.method.empty() || out.path.empty()) {
        return HttpParse::error;
    }
    return HttpParse::ok;
}

std::string http_response(int status, std::string_view content_type, std::string_view body) {
    std::string out;
    out.reserve(body.size() + 192);
    out.append("HTTP/1.1 ");
    out.append(std::to_string(status));
    out.push_back(' ');
    out.append(reason_phrase(status));
    out.append("\r\nContent-Type: ");
    out.append(content_type);
    out.append("\r\nContent-Length: ");
    out.append(std::to_string(body.size()));
    out.append("\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n");
    out.append(body.data(), body.size());
    return out;
}

} // namespace fgb::net::ws
