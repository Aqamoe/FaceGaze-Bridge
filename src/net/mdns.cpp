#include "net/mdns.hpp"

#include "net/udp_socket.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <string_view>
#include <thread>
#include <unordered_map>

namespace fgb::net {
namespace {

constexpr char kMdnsGroup[] = "224.0.0.251";
constexpr std::uint16_t kMdnsPort = 5353;

constexpr std::uint16_t kTypeA    = 1;
constexpr std::uint16_t kTypePtr  = 12;
constexpr std::uint16_t kTypeTxt  = 16;
constexpr std::uint16_t kTypeSrv  = 33;

std::uint16_t read_u16(const std::uint8_t* data, std::size_t size, std::size_t pos,
                       bool& ok) noexcept {
    if (pos + 2 > size) {
        ok = false;
        return 0;
    }
    return static_cast<std::uint16_t>((data[pos] << 8) | data[pos + 1]);
}

std::uint32_t read_u32(const std::uint8_t* data, std::size_t size, std::size_t pos,
                       bool& ok) noexcept {
    if (pos + 4 > size) {
        ok = false;
        return 0;
    }
    return (static_cast<std::uint32_t>(data[pos]) << 24) |
           (static_cast<std::uint32_t>(data[pos + 1]) << 16) |
           (static_cast<std::uint32_t>(data[pos + 2]) << 8) |
           static_cast<std::uint32_t>(data[pos + 3]);
}

std::string lower_ascii(std::string_view text) {
    std::string out{text};
    for (char& c : out) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return out;
}

// 读 DNS 名字（支持 0xC0 压缩指针）。pos 只在"未跳转"时前进到名字之后的字节。
bool read_name(const std::uint8_t* data, std::size_t size, std::size_t& pos, std::string& out,
               int depth = 0) {
    out.clear();
    if (depth > 8) {
        return false;
    }

    std::size_t cursor = pos;
    bool jumped        = false;
    int jumps          = 0;

    for (;;) {
        if (cursor >= size) {
            return false;
        }
        const std::uint8_t length = data[cursor];
        if ((length & 0xC0) == 0xC0) {
            if (cursor + 1 >= size || ++jumps > 32) {
                return false;
            }
            const std::size_t pointer =
                (static_cast<std::size_t>(length & 0x3F) << 8) | data[cursor + 1];
            if (pointer >= size) {
                return false;
            }
            if (!jumped) {
                pos    = cursor + 2;
                jumped = true;
            }
            cursor = pointer;
            continue;
        }
        if ((length & 0xC0) != 0) {
            return false; // 保留位，不是合法标签长度
        }
        ++cursor;
        if (length == 0) {
            if (!jumped) {
                pos = cursor;
            }
            return true;
        }
        if (cursor + length > size) {
            return false;
        }
        if (!out.empty()) {
            out.push_back('.');
        }
        out.append(reinterpret_cast<const char*>(data + cursor), length);
        cursor += length;
    }
}

std::string_view rdata_view(const std::uint8_t* data, std::size_t size, std::size_t pos,
                            std::size_t length) noexcept {
    if (pos > size || length > size - pos) {
        return {};
    }
    return {reinterpret_cast<const char*>(data + pos), length};
}

void encode_name(const std::string& name, std::vector<std::uint8_t>& out) {
    std::size_t begin = 0;
    while (begin < name.size()) {
        std::size_t dot = name.find('.', begin);
        if (dot == std::string::npos) {
            dot = name.size();
        }
        const std::size_t length = dot - begin;
        if (length == 0 || length > 63) {
            return;
        }
        out.push_back(static_cast<std::uint8_t>(length));
        out.insert(out.end(), name.begin() + static_cast<std::ptrdiff_t>(begin),
                   name.begin() + static_cast<std::ptrdiff_t>(dot));
        begin = dot + 1;
    }
    out.push_back(0);
}

} // namespace

bool parse_mdns_response(const std::uint8_t* data, std::size_t size, MdnsRecords& out) {
    if (data == nullptr || size < 12) {
        return false;
    }

    bool ok        = true;
    const auto qd  = read_u16(data, size, 4, ok);
    const auto an  = read_u16(data, size, 6, ok);
    const auto ns  = read_u16(data, size, 8, ok);
    const auto ar  = read_u16(data, size, 10, ok);
    if (!ok) {
        return false;
    }

    std::size_t pos = 12;
    for (std::uint16_t i = 0; i < qd; ++i) {
        std::string ignored;
        if (!read_name(data, size, pos, ignored) || pos + 4 > size) {
            return false;
        }
        pos += 4;
    }

    const std::size_t total = static_cast<std::size_t>(an) + ns + ar;
    for (std::size_t i = 0; i < total; ++i) {
        std::string name;
        if (!read_name(data, size, pos, name)) {
            return false;
        }
        const auto type   = read_u16(data, size, pos, ok);
        /* class */ read_u16(data, size, pos + 2, ok);
        /* ttl */ read_u32(data, size, pos + 4, ok);
        const auto length = read_u16(data, size, pos + 8, ok);
        if (!ok || pos + 10 > size) {
            return false;
        }
        pos += 10;

        const auto rdata = rdata_view(data, size, pos, length);
        if (rdata.empty() && length != 0) {
            return false;
        }

        switch (type) {
        case kTypePtr: {
            std::size_t cursor = pos;
            std::string target;
            if (!read_name(data, size, cursor, target)) {
                return false;
            }
            out.ptrs.push_back({std::move(name), std::move(target)});
            break;
        }
        case kTypeSrv: {
            if (length < 7) {
                return false;
            }
            std::size_t cursor = pos + 6;
            std::string target;
            if (!read_name(data, size, cursor, target)) {
                return false;
            }
            const std::uint16_t port = read_u16(data, size, pos + 4, ok);
            if (!ok) {
                return false;
            }
            out.srvs.push_back({std::move(name), port, std::move(target)});
            break;
        }
        case kTypeA: {
            if (length != 4) {
                break;
            }
            const std::string address = std::to_string(data[pos]) + "." +
                                        std::to_string(data[pos + 1]) + "." +
                                        std::to_string(data[pos + 2]) + "." +
                                        std::to_string(data[pos + 3]);
            out.addrs.push_back({std::move(name), address});
            break;
        }
        case kTypeTxt: {
            std::vector<std::string> values;
            std::size_t cursor = pos;
            const std::size_t end = pos + length;
            while (cursor < end) {
                const std::size_t part = data[cursor++];
                if (cursor + part > end) {
                    return false;
                }
                if (part != 0) {
                    values.emplace_back(reinterpret_cast<const char*>(data + cursor), part);
                }
                cursor += part;
            }
            out.txts.push_back({std::move(name), std::move(values)});
            break;
        }
        default:
            break;
        }

        pos += length;
    }
    return true;
}

std::vector<MdnsService> assemble_mdns_services(const MdnsRecords& records) {
    std::unordered_map<std::string, const MdnsRecords::Srv*> srvs;
    std::unordered_map<std::string, std::string> addrs;
    std::unordered_map<std::string, const MdnsRecords::Txt*> txts;

    for (const auto& srv : records.srvs) {
        srvs.emplace(lower_ascii(srv.name), &srv);
    }
    for (const auto& addr : records.addrs) {
        addrs.emplace(lower_ascii(addr.name), addr.address);
    }
    for (const auto& txt : records.txts) {
        txts.emplace(lower_ascii(txt.name), &txt);
    }

    std::vector<MdnsService> services;
    for (const auto& ptr : records.ptrs) {
        const auto srv_it = srvs.find(lower_ascii(ptr.target));
        if (srv_it == srvs.end()) {
            continue; // SRV 可能在另一条报文里，调用方累积后再拼
        }
        const MdnsRecords::Srv& srv = *srv_it->second;

        MdnsService service;
        service.instance = ptr.target;
        service.host     = srv.target;
        service.port     = srv.port;
        const auto addr_it = addrs.find(lower_ascii(srv.target));
        service.address    = addr_it == addrs.end() ? "127.0.0.1" : addr_it->second;

        const auto txt_it = txts.find(lower_ascii(ptr.target));
        if (txt_it != txts.end()) {
            service.txt = txt_it->second->values;
        }

        const bool duplicate =
            std::any_of(services.begin(), services.end(), [&](const MdnsService& existing) {
                return lower_ascii(existing.instance) == lower_ascii(service.instance) &&
                       existing.port == service.port;
            });
        if (!duplicate) {
            services.push_back(std::move(service));
        }
    }
    return services;
}

std::vector<std::uint8_t> build_mdns_ptr_query(std::string_view service_type) {
    std::vector<std::uint8_t> packet;
    packet.reserve(64);
    // ID 0、flags 0、QDCOUNT 1、其余 0
    packet.insert(packet.end(), {0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0});
    encode_name(std::string{service_type}, packet);
    packet.insert(packet.end(), {0, static_cast<std::uint8_t>(kTypePtr), 0, 1});
    return packet;
}

bool mdns_discover(std::string_view service_type, std::vector<MdnsService>& out,
                   int timeout_ms) {
    out.clear();
    if (service_type.empty() || timeout_ms <= 0) {
        return false;
    }

    UdpSocket socket;
    if (!socket.open_receiver(kMdnsPort, true)) {
        // 5353 被 Bonjour 等占用时退回临时端口；RFC 6762 的 legacy 查询允许单播回复
        if (!socket.open_receiver(0, true)) {
            return false;
        }
    }
    socket.join_multicast_group(kMdnsGroup);
    socket.set_multicast_ttl(255);
    socket.set_nonblocking(true);

    const std::vector<std::uint8_t> query = build_mdns_ptr_query(service_type);
    if (query.empty()) {
        return false;
    }
    const auto as_bytes = [](const std::vector<std::uint8_t>& data) {
        return std::span<const std::byte>{reinterpret_cast<const std::byte*>(data.data()),
                                          data.size()};
    };
    socket.send_to(as_bytes(query), kMdnsGroup, kMdnsPort);

    MdnsRecords records;
    std::array<std::byte, 9000> buffer{};
    const auto start    = std::chrono::steady_clock::now();
    const auto deadline = start + std::chrono::milliseconds(timeout_ms);
    // 丢包兜底：半程还没结果就再问一次
    const auto resend_at =
        start + std::chrono::milliseconds(std::min(timeout_ms / 2, 300));
    bool resent = false;

    // 第一个响应不一定是 VRChat（同机还有 OVRToolkit 等），收满一小段窗口再返回；
    // 否则会拿着 "只有 OVRToolkit" 的结果去选 VRChat，直接判定失败。
    constexpr auto kSettleWindow = std::chrono::milliseconds(150);
    std::chrono::steady_clock::time_point first_hit{};

    while (std::chrono::steady_clock::now() < deadline) {
        std::string from_ip;
        std::uint16_t from_port = 0;
        const int received = socket.recv_from(buffer, from_ip, from_port);
        if (received > 0) {
            parse_mdns_response(reinterpret_cast<const std::uint8_t*>(buffer.data()),
                                static_cast<std::size_t>(received), records);
            out = assemble_mdns_services(records);
            if (!out.empty()) {
                const auto now = std::chrono::steady_clock::now();
                if (first_hit.time_since_epoch().count() == 0) {
                    first_hit = now;
                }
                if (now - first_hit >= kSettleWindow) {
                    return true;
                }
            }
        } else if (received < 0) {
            break;
        }

        if (!resent && std::chrono::steady_clock::now() >= resend_at) {
            socket.send_to(as_bytes(query), kMdnsGroup, kMdnsPort);
            resent = true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    out = assemble_mdns_services(records);
    return !out.empty();
}

} // namespace fgb::net
