// 极简 mDNS / DNS-SD 发现。
//
// 只做一件事：在本地链路问一句 "谁提供 <服务类型>"，把 PTR/SRV/A/TXT 拼成
// (实例名, 主机, IPv4, 端口)。VRChat 的 OSCQuery HTTP 端口是动态的，只能这样找；
// 固定探测 9001 会失败（实测本机是 65393）。
//
// 【为什么不用 DnsServiceBrowse】
// 那是 Windows 的异步回调 API，跨线程收结果、超时取消都要自己包一层，而且没法用
// 一段固定报文做单测。这里用 UDP 组播 + 自己解析，报文解析是纯函数，离线可测。
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace fgb::net {

struct MdnsService {
    std::string instance;   // VRChat-Client-XXXXXX._oscjson._tcp.local
    std::string host;       // VRChat-Client-XXXXXX.oscjson.local
    std::string address;    // IPv4，点分十进制
    std::uint16_t port = 0; // OSCQuery HTTP 端口
    std::vector<std::string> txt;
};

// 一条报文里解析出来的记录。一次发现可能收到多条响应，调用方跨报文累积。
struct MdnsRecords {
    struct Ptr {
        std::string name;
        std::string target;
    };
    struct Srv {
        std::string name;
        std::uint16_t port = 0;
        std::string target;
    };
    struct Addr {
        std::string name;
        std::string address;
    };
    struct Txt {
        std::string name;
        std::vector<std::string> values;
    };

    std::vector<Ptr> ptrs;
    std::vector<Srv> srvs;
    std::vector<Addr> addrs;
    std::vector<Txt> txts;
};

// 供测试：解析 DNS 响应报文（含名字压缩）。坏报文返回 false，已解析的记录保留。
bool parse_mdns_response(const std::uint8_t* data, std::size_t size, MdnsRecords& out);

// 供测试：把累积记录拼成服务列表（同一个实例的多条记录合并）。
std::vector<MdnsService> assemble_mdns_services(const MdnsRecords& records);

// 供测试：构造 PTR 查询报文。
std::vector<std::uint8_t> build_mdns_ptr_query(std::string_view service_type);

// 查询本地链路。service_type 形如 "_oscjson._tcp.local"。
// 至少解析出一个服务时返回 true；即使超时也会返回已收到的结果。
bool mdns_discover(std::string_view service_type, std::vector<MdnsService>& out,
                   int timeout_ms = 1000);

} // namespace fgb::net
