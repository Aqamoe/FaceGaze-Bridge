// 递归 JSON 树。
//
// base/json.hpp 只做"从一层对象里按 key 取标量"，够读 module.json 和 UI 命令；
// 原生模块库（registry schema v2）是深结构：modules[] 里套 author/requires/stats/
// latest/versions/i18n，用"一层标量"读不了。这里补一棵最小树，解析完由调用方
// 按路径取，避免再写第二套专用解析器。
//
// 仍然不追求通用：不支持注释、不校验 UTF-8 合法性，数字统一按 double 存
// （清单里的 size/downloads 都远小于 2^53）。
#pragma once

#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace fgb::json {

struct Value {
    enum class Type { Null, Bool, Number, String, Array, Object };

    Type kind     = Type::Null;
    bool boolean  = false;
    double number = 0.0;
    std::string string;
    std::vector<Value> array;
    std::vector<std::pair<std::string, Value>> object; // 保序：清单字段少，线性查够用

    bool is_object() const noexcept { return kind == Type::Object; }
    bool is_array() const noexcept { return kind == Type::Array; }
    bool is_string() const noexcept { return kind == Type::String; }
    bool is_number() const noexcept { return kind == Type::Number; }
    bool is_bool() const noexcept { return kind == Type::Bool; }

    const Value* find(std::string_view key) const noexcept;

    // 类型不符或缺键时返回调用方给的默认值：清单里的可选字段很多，
    // 让每处调用都写一遍 find() 只会把"缺字段"和"字段类型错"混在一起。
    std::string string_at(std::string_view key, std::string fallback = {}) const;
    double number_at(std::string_view key, double fallback = 0.0) const;
    bool bool_at(std::string_view key, bool fallback = false) const;
    std::vector<std::string> string_array_at(std::string_view key) const;
};

// 整串必须是"一个值 + 可选空白"，否则返回 false 并给出人类可读原因。
bool parse(std::string_view text, Value& out, std::string& error);

} // namespace fgb::json
