// 错误处理。
//
// 纪律：运行时链路不用异常。异常只用于"初始化失败"这类无法继续的场景，
// 逐帧路径一律返回 Result<T>，避免栈展开带来的延迟不确定性。
#pragma once

#include <string>
#include <utility>
#include <variant>

namespace fgb {

enum class Errc {
    ok = 0,
    invalid_argument,
    out_of_range,
    buffer_too_small,
    malformed_packet,
    unsupported_version,
    not_initialized,
    io_error,
    timeout,
    not_found,
    already_exists,
};

struct Error {
    Errc code = Errc::ok;
    std::string message;

    Error() = default;
    Error(Errc c, std::string msg) : code(c), message(std::move(msg)) {}
};

inline const char* to_string(Errc e) noexcept {
    switch (e) {
    case Errc::ok:
        return "ok";
    case Errc::invalid_argument:
        return "invalid_argument";
    case Errc::out_of_range:
        return "out_of_range";
    case Errc::buffer_too_small:
        return "buffer_too_small";
    case Errc::malformed_packet:
        return "malformed_packet";
    case Errc::unsupported_version:
        return "unsupported_version";
    case Errc::not_initialized:
        return "not_initialized";
    case Errc::io_error:
        return "io_error";
    case Errc::timeout:
        return "timeout";
    case Errc::not_found:
        return "not_found";
    case Errc::already_exists:
        return "already_exists";
    }
    return "unknown";
}

// 极简 expected。C++23 的 std::expected 在 MSVC 19.29 上不可用。
template <typename T>
class Result {
public:
    Result(T value) : storage_(std::move(value)) {}
    Result(Error err) : storage_(std::move(err)) {}

    bool ok() const noexcept { return storage_.index() == 0; }
    explicit operator bool() const noexcept { return ok(); }

    T& value() & noexcept { return std::get<0>(storage_); }
    const T& value() const& noexcept { return std::get<0>(storage_); }
    T&& value() && noexcept { return std::get<0>(std::move(storage_)); }

    Error& error() & noexcept { return std::get<1>(storage_); }
    const Error& error() const& noexcept { return std::get<1>(storage_); }

private:
    std::variant<T, Error> storage_;
};

class Status {
public:
    Status() = default;
    Status(Error err) : error_(std::move(err)) {}

    bool ok() const noexcept { return error_.code == Errc::ok; }
    explicit operator bool() const noexcept { return ok(); }
    const Error& error() const noexcept { return error_; }

    static Status success() noexcept { return Status{}; }

private:
    Error error_;
};

} // namespace fgb
