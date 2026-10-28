// 极简 INI 读写。
//
// 不引 JSON 库：配置项都是"标量 → 标量"，INI 足够，而且手改时不担心括号逗号。
// 只支持 `[节]`、`键 = 值`、`#`/`;` 行注释——超出的语法一律当错误，不做猜测。
#pragma once

#include <functional>
#include <string>
#include <string_view>

namespace fgb::config {

struct IniEntry {
    std::string section; // 顶层键为空串
    std::string key;
    std::string value;
};

// 逐行回调。返回 false 表示文件打不开。
bool ini_load(const std::string& path, const std::function<void(const IniEntry&)>& on_entry);

class IniWriter {
public:
    bool open(const std::string& path);
    void close();
    void section(std::string_view name);
    void kv(std::string_view key, std::string_view value);
    void kv(std::string_view key, double value, int decimals = 4);
    // 刻意不叫 kv 重载：`const char*` 会优先匹配 bool（指针→bool 是标准转换），
    // 结果是"写进去一个 1"而不是字符串，而且不报错。名字区分开就没这个坑。
    void kv_bool(std::string_view key, bool value);
    void comment(std::string_view text);
    bool ok() const noexcept { return file_ != nullptr; }

private:
    std::FILE* file_ = nullptr;
};

// 解析辅助：失败时保持原值
float parse_float(std::string_view text, float fallback) noexcept;
bool parse_bool(std::string_view text, bool fallback) noexcept;

} // namespace fgb::config
