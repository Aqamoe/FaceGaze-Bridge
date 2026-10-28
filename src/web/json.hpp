// 极简 JSON 输出与 HTTP 请求解析。
//
// 面板的 JSON 都是我们自己拼的，结构固定，不需要通用序列化库。
// 单独成文件是为了能被单测覆盖——转义和请求行解析是最容易出低级错误的地方。
#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace fgb::web {

// 追加一个 JSON 字符串（含引号与转义）
void json_append_escaped(std::string& out, std::string_view text);

class JsonWriter {
public:
    explicit JsonWriter(std::string& out) : out_(out) {}

    JsonWriter& obj_begin();
    JsonWriter& obj_end();
    JsonWriter& arr_begin();
    JsonWriter& arr_end();
    JsonWriter& key(std::string_view name);
    JsonWriter& str(std::string_view value);
    JsonWriter& num(double value, int decimals = 3);
    JsonWriter& integer(long long value);
    JsonWriter& boolean(bool value);

private:
    void separate();
    std::string& out_;
    bool need_comma_ = false;
    bool after_key_  = false;
};

struct HttpRequest {
    std::string method;
    std::string path;  // 不含查询串
    std::string query; // "?a=1" 之后的部分，不含 '?'
    std::string body;

    bool is_get() const noexcept { return method == "GET"; }
    bool is_post() const noexcept { return method == "POST"; }
};

// 解析原始请求。返回 false 表示格式不合法（不是 HTTP）。
bool parse_request(const char* data, std::size_t size, HttpRequest& out);

// 取查询串或 JSON body 里的数字/布尔（面板只用到这两种，够用就好）
bool query_number(std::string_view query, std::string_view key, double& out);
bool query_bool(std::string_view query, std::string_view key, bool& out);
bool body_number(std::string_view body, std::string_view key, double& out);
bool body_bool(std::string_view body, std::string_view key, bool& out);

} // namespace fgb::web
