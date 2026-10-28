// 极简 JSON 读写。
//
// 只覆盖本项目真正需要的两个场景：
//   1. 写：控制面消息（WS）与配置导出——只需要对象/数组/字符串/数字/布尔
//   2. 读：从"一层对象"里取标量——`module.json`、UI 发来的命令都是这个形状
//
// 刻意不引 JSON 库：解析器要能容错读 VRCFT 产出的文件（BOM、多余字段、键顺序不定），
// 一旦上库就得为"库的严格性"再包一层；而我们的读需求只有"按 key 取标量"。
//
// 局限（有意为之）：不处理嵌套路径、不处理 `\uXXXX`、不校验整串合法性。
// 需要这些的时候再升级，而不是现在写一个半吊子解析器假装通用。
#pragma once

#include <string>
#include <string_view>

namespace fgb::json {

void append_escaped(std::string& out, std::string_view text);

class JsonWriter {
public:
    explicit JsonWriter(std::string& out) noexcept : out_(out) {}

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

// 从一层对象里取标量。找不到就返回 false，不猜默认值。
bool field_string(std::string_view text, std::string_view key, std::string& out);
bool field_number(std::string_view text, std::string_view key, double& out);
bool field_bool(std::string_view text, std::string_view key, bool& out);

} // namespace fgb::json
