#include "net/mdns.hpp"
#include "params/oscquery.hpp"

#include "test_framework.hpp"

#include <cstdint>
#include <string>
#include <vector>

using namespace fgb;

namespace {

constexpr char kTree[] = R"({
  "DESCRIPTION":"root node","FULL_PATH":"/","ACCESS":0,
  "CONTENTS":{
    "avatar":{
      "FULL_PATH":"/avatar","ACCESS":0,
      "CONTENTS":{
        "change":{"DESCRIPTION":"Avatar ID","FULL_PATH":"/avatar/change",
                  "ACCESS":3,"TYPE":"s","VALUE":["avtr_6ce1b451-cc77-43db-ac3e-eec2f3258ec6"]},
        "name":{"DESCRIPTION":"Avatar name","FULL_PATH":"/avatar/name",
                "ACCESS":1,"TYPE":"s","VALUE":["Eku-FT2-SD"]},
        "parameters":{
          "FULL_PATH":"/avatar/parameters","ACCESS":0,
          "CONTENTS":{
            "VelocityX":{"FULL_PATH":"/avatar/parameters/VelocityX","ACCESS":1,
                         "TYPE":"f","VALUE":[0.0]},
            "FT":{
              "FULL_PATH":"/avatar/parameters/FT","ACCESS":0,
              "CONTENTS":{
                "v2":{
                  "FULL_PATH":"/avatar/parameters/FT/v2","ACCESS":0,
                  "CONTENTS":{
                    "JawOpen":{"FULL_PATH":"/avatar/parameters/FT/v2/JawOpen","ACCESS":3,
                               "TYPE":"f","VALUE":[0.0]},
                    "EyeLidLeft":{"FULL_PATH":"/avatar/parameters/FT/v2/EyeLidLeft","ACCESS":3,
                                  "TYPE":"T","VALUE":[false]},
                    "VRCEmote":{"FULL_PATH":"/avatar/parameters/FT/v2/VRCEmote","ACCESS":3,
                                "TYPE":"i","VALUE":[0]},
                    "Vector":{"FULL_PATH":"/avatar/parameters/FT/v2/Vector","ACCESS":3,
                              "TYPE":"fff"}
                  }
                }
              }
            },
            "pcs":{
              "FULL_PATH":"/avatar/parameters/pcs","ACCESS":0,
              "CONTENTS":{
                "contact":{
                  "FULL_PATH":"/avatar/parameters/pcs/contact","ACCESS":0,
                  "CONTENTS":{
                    "detect-hit":{"FULL_PATH":"/avatar/parameters/pcs/contact/detect-hit",
                                  "ACCESS":3,"TYPE":"T","VALUE":[false]}
                  }
                }
              }
            }
          }
        }
      }
    }
  }
})";

void push_u16(std::vector<std::uint8_t>& out, std::uint16_t value) {
    out.push_back(static_cast<std::uint8_t>(value >> 8));
    out.push_back(static_cast<std::uint8_t>(value & 0xFF));
}

void push_u32(std::vector<std::uint8_t>& out, std::uint32_t value) {
    out.push_back(static_cast<std::uint8_t>(value >> 24));
    out.push_back(static_cast<std::uint8_t>((value >> 16) & 0xFF));
    out.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFF));
    out.push_back(static_cast<std::uint8_t>(value & 0xFF));
}

void push_name(std::vector<std::uint8_t>& out, const std::string& name) {
    std::size_t begin = 0;
    while (begin < name.size()) {
        std::size_t dot = name.find('.', begin);
        if (dot == std::string::npos) {
            dot = name.size();
        }
        out.push_back(static_cast<std::uint8_t>(dot - begin));
        out.insert(out.end(), name.begin() + static_cast<std::ptrdiff_t>(begin),
                   name.begin() + static_cast<std::ptrdiff_t>(dot));
        begin = dot + 1;
    }
    out.push_back(0);
}

void push_record_header(std::vector<std::uint8_t>& out, std::uint16_t type, std::uint16_t rdlen) {
    push_u16(out, type);
    push_u16(out, 1); // IN
    push_u32(out, 120);
    push_u16(out, rdlen);
}

} // namespace

FGB_TEST(oscquery_parses_avatar_and_writable_params) {
    params::AvatarConfig config;
    std::string error;
    FGB_CHECK(params::parse_oscquery_tree(kTree, config, error));
    FGB_CHECK(error.empty());
    FGB_CHECK(config.id == "avtr_6ce1b451-cc77-43db-ac3e-eec2f3258ec6");
    FGB_CHECK(config.name == "Eku-FT2-SD");
    FGB_CHECK(config.params.size() == 3); // 读只读 VelocityX、int、vector 都不算

    bool jaw_float = false;
    bool lid_bool  = false;
    bool hit_bool  = false;
    for (const params::AvatarParam& param : config.params) {
        if (param.address == "/avatar/parameters/FT/v2/JawOpen") {
            jaw_float = param.kind == params::ParamKind::Float;
        } else if (param.address == "/avatar/parameters/FT/v2/EyeLidLeft") {
            lid_bool = param.kind == params::ParamKind::Bool;
        } else if (param.address == "/avatar/parameters/pcs/contact/detect-hit") {
            hit_bool = param.kind == params::ParamKind::Bool;
        }
    }
    FGB_CHECK(jaw_float);
    FGB_CHECK(lid_bool);
    FGB_CHECK(hit_bool);
}

FGB_TEST(oscquery_decodes_escaped_avatar_name) {
    constexpr char kSnippet[] = R"({
      "FULL_PATH":"/","ACCESS":0,
      "CONTENTS":{
        "avatar":{"FULL_PATH":"/avatar","ACCESS":0,"CONTENTS":{
          "name":{"FULL_PATH":"/avatar/name","ACCESS":1,"TYPE":"s","VALUE":["\u3042\u3044\ud83d\ude00"]},
          "parameters":{"FULL_PATH":"/avatar/parameters","ACCESS":0,"CONTENTS":{
            "P":{"FULL_PATH":"/avatar/parameters/P","ACCESS":3,"TYPE":"f"}
          }}
        }}
      }
    })";

    params::AvatarConfig config;
    std::string error;
    FGB_CHECK(params::parse_oscquery_tree(kSnippet, config, error));
    FGB_CHECK(config.name == "\xE3\x81\x82\xE3\x81\x84\xF0\x9F\x98\x80"); // あい😀
}

FGB_TEST(oscquery_accepts_empty_parameter_table) {
    constexpr char kEmptyParams[] = R"({
      "FULL_PATH":"/","ACCESS":0,
      "CONTENTS":{"avatar":{"FULL_PATH":"/avatar","ACCESS":0,"CONTENTS":{
        "parameters":{"FULL_PATH":"/avatar/parameters","ACCESS":0,"CONTENTS":{}}
      }}}
    })";
    params::AvatarConfig config;
    std::string error;
    // 没有可写参数也是有效结果：绑成空表，而不是回退到固定前缀
    FGB_CHECK(params::parse_oscquery_tree(kEmptyParams, config, error));
    FGB_CHECK(config.params.empty());
}

FGB_TEST(oscquery_normalizes_base_url) {
    FGB_CHECK(params::normalize_oscquery_base_url("127.0.0.1:65393") ==
              "http://127.0.0.1:65393/");
    FGB_CHECK(params::normalize_oscquery_base_url("http://127.0.0.1:65393") ==
              "http://127.0.0.1:65393/");
    FGB_CHECK(params::normalize_oscquery_base_url("") == "");
}

FGB_TEST(oscquery_rejects_tree_without_params) {
    constexpr char kEmpty[] = R"({"FULL_PATH":"/","ACCESS":0,"CONTENTS":{}})";
    params::AvatarConfig config;
    std::string error;
    FGB_CHECK(!params::parse_oscquery_tree(kEmpty, config, error));
    FGB_CHECK(!error.empty());
}

FGB_TEST(oscquery_parses_avatar_change_value) {
    constexpr char kChange[] =
        R"({"FULL_PATH":"/avatar/change","ACCESS":3,"TYPE":"s","VALUE":["avtr_abc"]})";
    std::string id;
    FGB_CHECK(params::parse_oscquery_avatar_change(kChange, id));
    FGB_CHECK(id == "avtr_abc");
    FGB_CHECK(params::parse_oscquery_avatar_change(
        R"({"FULL_PATH":"/avatar/change","VALUE":["avtr_abc","extra"]})", id));
    FGB_CHECK(id == "avtr_abc");
    FGB_CHECK(!params::parse_oscquery_avatar_change(R"({"FULL_PATH":"/avatar/change"})", id));
}

FGB_TEST(mdns_builds_ptr_query) {
    const auto packet = net::build_mdns_ptr_query("_oscjson._tcp.local");
    FGB_CHECK(packet.size() >= 12 + 4);
    FGB_CHECK(packet[4] == 0 && packet[5] == 1); // QDCOUNT = 1
    FGB_CHECK(packet[packet.size() - 4] == 0 && packet[packet.size() - 3] == 12); // PTR
    FGB_CHECK(packet[packet.size() - 2] == 0 && packet[packet.size() - 1] == 1);  // IN
}

FGB_TEST(mdns_parses_and_assembles_vrchat_service) {
    const std::string instance = "VRChat-Client-TEST._oscjson._tcp.local";
    const std::string host     = "VRChat-Client-TEST.oscjson.local";
    const std::string service  = "_oscjson._tcp.local";

    std::vector<std::uint8_t> packet;
    push_u16(packet, 0);      // ID
    push_u16(packet, 0x8400); // response + authoritative
    push_u16(packet, 0);
    push_u16(packet, 4); // ANCOUNT
    push_u16(packet, 0);
    push_u16(packet, 0);

    // PTR（名字不压缩，后面用它测压缩指针）
    push_name(packet, service);
    push_record_header(packet, 12, static_cast<std::uint16_t>(instance.size() + 2));
    push_name(packet, instance);
    const std::size_t instance_offset = packet.size() - instance.size() - 2;

    // SRV：owner 用压缩指针指回 PTR 的 rdata
    push_u16(packet, static_cast<std::uint16_t>(0xC000 | instance_offset));
    push_record_header(packet, 33, static_cast<std::uint16_t>(6 + host.size() + 2));
    push_u16(packet, 0);
    push_u16(packet, 0);
    push_u16(packet, 65393);
    push_name(packet, host);
    const std::size_t host_offset = packet.size() - host.size() - 2;

    // A：owner 用压缩指针指回 SRV 的 target
    push_u16(packet, static_cast<std::uint16_t>(0xC000 | host_offset));
    push_record_header(packet, 1, 4);
    packet.insert(packet.end(), {127, 0, 0, 1});

    // TXT
    push_u16(packet, static_cast<std::uint16_t>(0xC000 | instance_offset));
    push_record_header(packet, 16, 10);
    packet.push_back(9);
    const std::string txt = "txtvers=1";
    packet.insert(packet.end(), txt.begin(), txt.end());

    net::MdnsRecords records;
    FGB_CHECK(net::parse_mdns_response(packet.data(), packet.size(), records));
    FGB_CHECK(records.ptrs.size() == 1);
    FGB_CHECK(records.srvs.size() == 1);
    FGB_CHECK(records.addrs.size() == 1);
    FGB_CHECK(records.txts.size() == 1);

    const auto services = net::assemble_mdns_services(records);
    FGB_CHECK(services.size() == 1);
    if (!services.empty()) {
        FGB_CHECK(services[0].instance == instance);
        FGB_CHECK(services[0].host == host);
        FGB_CHECK(services[0].address == "127.0.0.1");
        FGB_CHECK(services[0].port == 65393);
        FGB_CHECK(services[0].txt.size() == 1 && services[0].txt[0] == "txtvers=1");
    }
}

FGB_TEST(mdns_ignores_truncated_packet) {
    const std::uint8_t packet[6] = {0, 0, 0x84, 0, 0, 1};
    net::MdnsRecords records;
    FGB_CHECK(!net::parse_mdns_response(packet, sizeof(packet), records));
}
