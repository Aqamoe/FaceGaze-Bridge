#include "capture/capture_file.hpp"

#include <cstring>

namespace fgb::capture {
namespace {

constexpr std::size_t kHeaderSize = 8 + 4 + 4 + 8;
constexpr std::size_t kRecordHeader = 8; // u64 时间戳
constexpr std::size_t kRecordSize   = kRecordHeader + kPayloadSize;

} // namespace

Writer::~Writer() { close(); }

bool Writer::open(const std::string& path) {
    close();
    path_ = path;
#ifdef _WIN32
    if (fopen_s(&file_, path.c_str(), "wb") != 0) {
        file_ = nullptr;
    }
#else
    file_ = std::fopen(path.c_str(), "wb");
#endif
    if (file_ == nullptr) {
        return false;
    }

    std::byte header[kHeaderSize]{};
    std::memcpy(header, kMagic, sizeof(kMagic));
    std::uint32_t version      = kVersion;
    std::uint32_t payload_size = kPayloadSize;
    std::uint64_t reserved     = 0;
    std::memcpy(header + 8, &version, sizeof(version));
    std::memcpy(header + 12, &payload_size, sizeof(payload_size));
    std::memcpy(header + 16, &reserved, sizeof(reserved));

    header_ok_ = std::fwrite(header, 1, sizeof(header), file_) == sizeof(header);
    if (!header_ok_) {
        close();
        return false;
    }
    frames_ = 0;
    return true;
}

bool Writer::write(std::uint64_t ts_ns, const std::byte* payload, std::size_t size) {
    if (file_ == nullptr || !header_ok_ || payload == nullptr) {
        return false;
    }
    if (size != kPayloadSize) {
        return false; // 只接受固定长度的 ReplyUpdate body
    }
    if (std::fwrite(&ts_ns, 1, sizeof(ts_ns), file_) != sizeof(ts_ns)) {
        return false;
    }
    if (std::fwrite(payload, 1, size, file_) != size) {
        return false;
    }
    ++frames_;
    return true;
}

void Writer::close() {
    if (file_ != nullptr) {
        std::fclose(file_);
        file_ = nullptr;
    }
}

Reader::~Reader() { close(); }

bool Reader::open(const std::string& path) {
    close();
    error_.clear();
    index_            = 0;
    ts_ns_            = 0;
    estimated_frames_ = 0;

#ifdef _WIN32
    if (fopen_s(&file_, path.c_str(), "rb") != 0) {
        file_ = nullptr;
    }
#else
    file_ = std::fopen(path.c_str(), "rb");
#endif
    if (file_ == nullptr) {
        error_ = "无法打开文件";
        return false;
    }

    std::byte header[kHeaderSize]{};
    if (std::fread(header, 1, sizeof(header), file_) != sizeof(header)) {
        error_ = "文件过短，缺少文件头";
        close();
        return false;
    }
    if (std::memcmp(header, kMagic, sizeof(kMagic)) != 0) {
        error_ = "不是 FGB 抓包文件（magic 不匹配）";
        close();
        return false;
    }

    std::uint32_t version      = 0;
    std::uint32_t payload_size = 0;
    std::memcpy(&version, header + 8, sizeof(version));
    std::memcpy(&payload_size, header + 12, sizeof(payload_size));

    if (version != kVersion) {
        error_ = "抓包文件版本不支持: " + std::to_string(version);
        close();
        return false;
    }
    if (payload_size != kPayloadSize) {
        error_ = "载荷长度不匹配: " + std::to_string(payload_size);
        close();
        return false;
    }
    payload_size_ = payload_size;
    buffer_.resize(payload_size_);

    if (std::fseek(file_, 0, SEEK_END) == 0) {
        const long size = std::ftell(file_);
        const long body = size > static_cast<long>(kHeaderSize) ? size - static_cast<long>(kHeaderSize) : 0;
        estimated_frames_ = static_cast<std::size_t>(body / static_cast<long>(kRecordSize));
        std::fseek(file_, static_cast<long>(kHeaderSize), SEEK_SET);
    }
    return true;
}

bool Reader::next() {
    if (file_ == nullptr) {
        return false;
    }
    if (std::fread(&ts_ns_, 1, sizeof(ts_ns_), file_) != sizeof(ts_ns_)) {
        return false; // 正常结束
    }
    if (std::fread(buffer_.data(), 1, payload_size_, file_) != payload_size_) {
        error_ = "记录被截断";
        return false;
    }
    ++index_;
    return true;
}

void Reader::close() {
    if (file_ != nullptr) {
        std::fclose(file_);
        file_ = nullptr;
    }
}

} // namespace fgb::capture
