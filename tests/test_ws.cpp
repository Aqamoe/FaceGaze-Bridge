// WebSocket 编解码的单测。
//
// 这一层的价值全在"逐字节对不对"：握手摘要错一个字节，Chromium 就会静默断开，
// 现场只看到"页面一直重连"。所以这里有 RFC 的原文向量，也有构造出来的帧。
#include "net/ws_codec.hpp"

#include "test_framework.hpp"

#include <string>
#include <vector>

namespace {

std::string hex(std::string_view bytes) {
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string out;
    out.reserve(bytes.size() * 2);
    for (const char c : bytes) {
        const auto value = static_cast<unsigned char>(c);
        out.push_back(kDigits[value >> 4]);
        out.push_back(kDigits[value & 0x0F]);
    }
    return out;
}

// 客户端帧必须掩码，这里按 RFC 的样子拼一个
std::string masked_frame(std::uint8_t opcode, bool fin, std::string_view payload,
                         const std::vector<unsigned char>& mask) {
    std::string frame;
    frame.push_back(static_cast<char>((fin ? 0x80 : 0x00) | opcode));
    const std::size_t size = payload.size();
    if (size < 126) {
        frame.push_back(static_cast<char>(0x80 | size));
    } else {
        frame.push_back(static_cast<char>(0x80 | 126));
        frame.push_back(static_cast<char>((size >> 8) & 0xFF));
        frame.push_back(static_cast<char>(size & 0xFF));
    }
    for (std::size_t i = 0; i < 4; ++i) {
        frame.push_back(static_cast<char>(mask[i]));
    }
    for (std::size_t i = 0; i < payload.size(); ++i) {
        frame.push_back(static_cast<char>(static_cast<unsigned char>(payload[i]) ^
                                          mask[i % 4]));
    }
    return frame;
}

} // namespace

FGB_TEST(ws_sha1_known_vectors) {
    FGB_CHECK(hex(fgb::net::ws::sha1("abc")) == "a9993e364706816aba3e25717850c26c9cd0d89d");
    FGB_CHECK(hex(fgb::net::ws::sha1("")) == "da39a3ee5e6b4b0d3255bfef95601890afd80709");
    // 跨过 56 字节边界，走两轮分组
    FGB_CHECK(hex(fgb::net::ws::sha1("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq")) ==
              "84983e441c3bd26ebaae4aa1f95129e5e54670f1");
}

FGB_TEST(ws_base64_known_vectors) {
    FGB_CHECK(fgb::net::ws::base64("") == "");
    FGB_CHECK(fgb::net::ws::base64("f") == "Zg==");
    FGB_CHECK(fgb::net::ws::base64("fo") == "Zm8=");
    FGB_CHECK(fgb::net::ws::base64("foo") == "Zm9v");
    FGB_CHECK(fgb::net::ws::base64("foob") == "Zm9vYg==");
}

FGB_TEST(ws_accept_key_rfc_example) {
    // RFC 6455 §1.3 的示例
    FGB_CHECK(fgb::net::ws::accept_key("dGhlIHNhbXBsZSBub25jZQ==") ==
              "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=");
}

FGB_TEST(ws_parse_masked_text_frame) {
    const std::string raw = masked_frame(fgb::net::ws::kOpText, true, "hello", {1, 2, 3, 4});

    fgb::net::ws::Frame frame;
    std::string error;
    FGB_CHECK(fgb::net::ws::parse_frame(raw, frame, error) == fgb::net::ws::ParseResult::ok);
    FGB_CHECK(frame.fin);
    FGB_CHECK(frame.opcode == fgb::net::ws::kOpText);
    FGB_CHECK(frame.payload == "hello");
    FGB_CHECK(frame.consumed == raw.size());
}

FGB_TEST(ws_parse_incomplete_frame) {
    const std::string raw = masked_frame(fgb::net::ws::kOpText, true, "hello", {9, 9, 9, 9});

    fgb::net::ws::Frame frame;
    std::string error;
    // 少一个字节就得说"还不够"，不能当成错误把连接踢掉
    FGB_CHECK(fgb::net::ws::parse_frame(raw.substr(0, raw.size() - 1), frame, error) ==
              fgb::net::ws::ParseResult::incomplete);
    FGB_CHECK(fgb::net::ws::parse_frame("", frame, error) ==
              fgb::net::ws::ParseResult::incomplete);
}

FGB_TEST(ws_parse_extended_length_frame) {
    // 200 字节 → 必须走 126 扩展长度分支
    const std::string payload(200, 'x');
    const std::string raw = masked_frame(fgb::net::ws::kOpText, true, payload, {7, 6, 5, 4});

    fgb::net::ws::Frame frame;
    std::string error;
    FGB_CHECK(fgb::net::ws::parse_frame(raw, frame, error) == fgb::net::ws::ParseResult::ok);
    FGB_CHECK(frame.payload == payload);
}

FGB_TEST(ws_append_frame_roundtrip) {
    std::string frame;
    fgb::net::ws::append_frame(frame, fgb::net::ws::kOpText, "hi");

    FGB_CHECK(static_cast<unsigned char>(frame[0]) == 0x81); // FIN + text
    FGB_CHECK(static_cast<unsigned char>(frame[1]) == 2);    // 服务端不掩码
    FGB_CHECK(frame.substr(2) == "hi");

    // 长 payload 走 126
    std::string big;
    fgb::net::ws::append_frame(big, fgb::net::ws::kOpText, std::string(300, 'y'));
    FGB_CHECK(static_cast<unsigned char>(big[1]) == 126);
    FGB_CHECK(big.size() == 300 + 4);
}

FGB_TEST(ws_parse_rejects_oversized_frame) {
    std::string frame;
    frame.push_back(static_cast<char>(0x81));
    frame.push_back(static_cast<char>(0x80 | 127));
    // 8 字节长度，故意写成 8 MB
    const std::uint64_t length = 8u * 1024u * 1024u;
    for (int i = 7; i >= 0; --i) {
        frame.push_back(static_cast<char>((length >> (i * 8)) & 0xFF));
    }
    frame.append(4, '\0'); // 掩码键
    frame.append(64, 'z');

    fgb::net::ws::Frame parsed;
    std::string error;
    FGB_CHECK(fgb::net::ws::parse_frame(frame, parsed, error) == fgb::net::ws::ParseResult::error);
    FGB_CHECK(!error.empty());
}

FGB_TEST(ws_parse_http_upgrade_head) {
    const std::string raw =
        "GET /ws HTTP/1.1\r\n"
        "Host: 127.0.0.1:8787\r\n"
        "Upgrade: WebSocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
        "Sec-WebSocket-Version: 13\r\n\r\n";

    fgb::net::ws::HttpHead head;
    FGB_CHECK(fgb::net::ws::parse_http_head(raw, head) == fgb::net::ws::HttpParse::ok);
    FGB_CHECK(head.method == "GET");
    FGB_CHECK(head.path == "/ws");
    FGB_CHECK(head.upgrade == "websocket"); // 大小写归一化，Upgrade 头各家写法不一
    FGB_CHECK(head.ws_key == "dGhlIHNhbXBsZSBub25jZQ==");
    FGB_CHECK(head.head_bytes == raw.size());
}

FGB_TEST(ws_parse_http_incomplete_and_garbage) {
    fgb::net::ws::HttpHead head;
    FGB_CHECK(fgb::net::ws::parse_http_head("GET / HTTP/1.1\r\nHost: x\r\n", head) ==
              fgb::net::ws::HttpParse::incomplete);
    FGB_CHECK(fgb::net::ws::parse_http_head("GET\r\n\r\n", head) ==
              fgb::net::ws::HttpParse::error);
    FGB_CHECK(fgb::net::ws::parse_http_head(std::string(20000, 'a'), head) ==
              fgb::net::ws::HttpParse::error);
}

FGB_TEST(ws_http_response_has_content_length) {
    const std::string response = fgb::net::ws::http_response(200, "application/json", "{\"ok\":true}");
    FGB_CHECK(response.rfind("HTTP/1.1 200 OK\r\n", 0) == 0);
    FGB_CHECK(response.find("Content-Length: 11\r\n") != std::string::npos);
    FGB_CHECK(response.find("\r\n\r\n{\"ok\":true}") != std::string::npos);
}
