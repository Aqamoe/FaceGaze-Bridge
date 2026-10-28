// 轻量日志。刻意不用 spdlog：core 层要保持零第三方依赖，
// 而 M1 阶段只需要一个能上色的同步日志。
#pragma once

#include <functional>
#include <string_view>

namespace fgb::log {

enum class Level : int {
    trace = 0,
    debug = 1,
    info  = 2,
    warn  = 3,
    error = 4,
    off   = 5,
};

void set_level(Level l) noexcept;
Level level() noexcept;
bool enabled(Level l) noexcept;

// printf 风格。仅用于控制面与低频路径，热路径禁止调用。
void write(Level l, std::string_view tag, const char* fmt, ...) noexcept;

// 日志订阅。UI 面板要显示引擎日志，但日志本身不该知道"面板"这个东西存在，
// 所以只提供一个回调口子；传空回调即取消订阅。
using Callback = std::function<void(Level, std::string_view tag, std::string_view message)>;
void set_callback(Callback callback);

} // namespace fgb::log

#define FGB_LOG(level, tag, ...) ::fgb::log::write(level, tag, __VA_ARGS__)
#define FGB_TRACE(tag, ...) FGB_LOG(::fgb::log::Level::trace, tag, __VA_ARGS__)
#define FGB_DEBUG(tag, ...) FGB_LOG(::fgb::log::Level::debug, tag, __VA_ARGS__)
#define FGB_INFO(tag, ...) FGB_LOG(::fgb::log::Level::info, tag, __VA_ARGS__)
#define FGB_WARN(tag, ...) FGB_LOG(::fgb::log::Level::warn, tag, __VA_ARGS__)
#define FGB_ERROR(tag, ...) FGB_LOG(::fgb::log::Level::error, tag, __VA_ARGS__)
