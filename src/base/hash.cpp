#include "base/hash.hpp"

#include <windows.h>
#include <bcrypt.h>

#include <fstream>
#include <string>

namespace fgb::hash {
namespace {

std::string digest_file(const wchar_t* algorithm, const std::filesystem::path& path) {
    BCRYPT_ALG_HANDLE provider = nullptr;
    if (::BCryptOpenAlgorithmProvider(&provider, algorithm, nullptr, 0) < 0) {
        return {};
    }

    DWORD hash_length = 0;
    DWORD written     = 0;
    ::BCryptGetProperty(provider, BCRYPT_HASH_LENGTH,
                        reinterpret_cast<PUCHAR>(&hash_length), sizeof(hash_length), &written, 0);

    BCRYPT_HASH_HANDLE hash = nullptr;
    if (hash_length == 0 || ::BCryptCreateHash(provider, &hash, nullptr, 0, nullptr, 0, 0) < 0) {
        ::BCryptCloseAlgorithmProvider(provider, 0);
        return {};
    }

    std::ifstream input(path, std::ios::binary);
    char buffer[64 * 1024];
    bool failed = !input;
    while (!failed && input) {
        input.read(buffer, sizeof(buffer));
        const std::streamsize got = input.gcount();
        if (got > 0) {
            failed = ::BCryptHashData(hash, reinterpret_cast<PUCHAR>(buffer),
                                      static_cast<ULONG>(got), 0) < 0;
        }
    }

    std::string result;
    if (!failed) {
        std::string digest(static_cast<std::size_t>(hash_length), '\0');
        if (::BCryptFinishHash(hash, reinterpret_cast<PUCHAR>(digest.data()), hash_length, 0) >= 0) {
            static const char* hex = "0123456789abcdef";
            result.reserve(digest.size() * 2);
            for (const unsigned char byte : digest) {
                result.push_back(hex[byte >> 4]);
                result.push_back(hex[byte & 0x0F]);
            }
        }
    }

    ::BCryptDestroyHash(hash);
    ::BCryptCloseAlgorithmProvider(provider, 0);
    return result;
}

} // namespace

std::string sha256_file(const std::filesystem::path& path) {
    return digest_file(BCRYPT_SHA256_ALGORITHM, path);
}

std::string md5_file(const std::filesystem::path& path) {
    return digest_file(BCRYPT_MD5_ALGORITHM, path);
}

} // namespace fgb::hash
