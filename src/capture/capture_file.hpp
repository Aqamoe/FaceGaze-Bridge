// 抓包文件格式与读写。
//
// 【为什么要录"原始包"而不是"解码后的帧"】
// 录制回放的价值在于能复现问题。如果录的是解码后的 FaceFrame，那么解码层的 bug
// 会被永久性地"录进文件里"，以后再也复现不出来。所以这里存的是 ReplyUpdate 的
// body 原样字节（structSize + 420 字节），回放时走**完全相同的**解码路径。
//
// 文件布局（小端）：
//   "FGBREC01"       8 字节 magic
//   version          u32  (当前 1)
//   payload_size     u32  (当前 424)
//   reserved         u64  (0)
//   ---- 记录序列 ----
//   timestamp_ns     u64  从首帧起的相对时间
//   payload_size     字节
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace fgb::capture {

inline constexpr char kMagic[8]      = {'F', 'G', 'B', 'R', 'E', 'C', '0', '1'};
inline constexpr std::uint32_t kVersion = 1;

// ReplyUpdate 的 body：structSize(4) + 420 字节结构体
inline constexpr std::uint32_t kPayloadSize = 424;

class Writer {
public:
    ~Writer();
    Writer() = default;
    Writer(const Writer&)            = delete;
    Writer& operator=(const Writer&) = delete;

    bool open(const std::string& path);
    void close();

    // ts_ns 是相对首帧的时间，由调用方保证单调
    bool write(std::uint64_t ts_ns, const std::byte* payload, std::size_t size);

    bool is_open() const noexcept { return file_ != nullptr; }
    std::size_t frames() const noexcept { return frames_; }
    const std::string& path() const noexcept { return path_; }

private:
    std::FILE* file_ = nullptr;
    std::string path_;
    std::size_t frames_ = 0;
    bool header_ok_     = false;
};

// 只读迭代器。payload() 指向内部缓冲，下次 next() 会失效。
class Reader {
public:
    ~Reader();
    Reader() = default;
    Reader(const Reader&)            = delete;
    Reader& operator=(const Reader&) = delete;

    bool open(const std::string& path);
    void close();

    // 顺序读取。读到末尾返回 false；出错时 error() 非空。
    bool next();

    std::uint64_t timestamp_ns() const noexcept { return ts_ns_; }
    const std::byte* payload() const noexcept { return buffer_.data(); }
    std::size_t payload_size() const noexcept { return payload_size_; }
    std::size_t index() const noexcept { return index_; }

    // 打开时就已知的帧数（按文件大小推算，只用于进度显示）
    std::size_t estimated_frames() const noexcept { return estimated_frames_; }
    const std::string& error() const noexcept { return error_; }
    bool is_open() const noexcept { return file_ != nullptr; }

private:
    std::FILE* file_ = nullptr;
    std::vector<std::byte> buffer_;
    std::size_t payload_size_     = 0;
    std::size_t index_            = 0;
    std::size_t estimated_frames_ = 0;
    std::uint64_t ts_ns_          = 0;
    std::string error_;
};

} // namespace fgb::capture
