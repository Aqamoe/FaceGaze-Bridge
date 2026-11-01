// 只改 NVIDIA 的 VRChat profile：VRSS 中央超采样区域（隐藏 setting 0x00524254）。
//
// 这是 FGB 相对上游新增的本地头文件。设置写入后需要重启游戏才生效；
// 只写 area，不伪造 allow/mode，也不碰其它 profile 值。
#pragma once

#include <windows.h>
#include "nvapi.h"

#include <cmath>
#include <cstring>
#include <iterator>
#include <string>

namespace profile_area {

inline constexpr NvU32 kAreaSetting = 0x00524254u;
inline constexpr NvU32 kAllowSetting = 0x00D5E9C6u;
inline constexpr NvU32 kModeSetting = 0x00D5E9C7u;
inline constexpr NvU32 kHmdGateSetting = 0x00524252u;
inline constexpr NvU32 kVrLayoutSetting = 0x00524253u;
inline constexpr NvU32 kAspectSetting = 0x00524255u;

inline NvU32 float_bits(float value) {
    NvU32 bits = 0;
    static_assert(sizeof(bits) == sizeof(value));
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

inline float bits_float(NvU32 bits) {
    float value = 0.0f;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

inline std::string status_text(NvAPI_Status status, const char* action) {
    return std::string(action) + " failed (NVAPI status " + std::to_string(status) + ")";
}

struct Session {
    NvDRSSessionHandle session = nullptr;
    NvDRSProfileHandle profile = nullptr;
    bool initialized = false;

    ~Session() {
        if (session != nullptr) {
            NvAPI_DRS_DestroySession(session);
        }
        if (initialized) {
            NvAPI_Unload();
        }
    }

    NvAPI_Status open() {
        const NvAPI_Status init = NvAPI_Initialize();
        if (init != NVAPI_OK) {
            return init;
        }
        initialized = true;
        auto* module = GetModuleHandleW(L"nvapi64.dll");
        auto query = module ? reinterpret_cast<void* (__cdecl*)(NvU32)>(
                                  GetProcAddress(module, "nvapi_QueryInterface"))
                            : nullptr;
        if (!query) {
            return NVAPI_NO_IMPLEMENTATION;
        }
        get_ = reinterpret_cast<GetEx>(query(0xEA99498Du));
        if (!get_) {
            get_ = reinterpret_cast<GetEx>(query(0x73BF8338u));
        }
        set_ = reinterpret_cast<SetEx>(query(0x8A2CF5F5u));
        if (!set_) {
            set_ = reinterpret_cast<SetEx>(query(0x577DD202u));
        }
        if (!get_ || !set_) {
            return NVAPI_NO_IMPLEMENTATION;
        }
        NvAPI_Status status = NvAPI_DRS_CreateSession(&session);
        if (status != NVAPI_OK) {
            return status;
        }
        status = NvAPI_DRS_LoadSettings(session);
        if (status != NVAPI_OK) {
            return status;
        }
        NvAPI_UnicodeString name{};
        constexpr wchar_t profile_name[] = L"VRChat";
        for (size_t i = 0; i < std::size(profile_name); ++i) {
            name[i] = static_cast<NvU16>(profile_name[i]);
        }
        status = NvAPI_DRS_FindProfileByName(session, name, &profile);
        if (status == NVAPI_OK) {
            return NVAPI_OK;
        }
        // 用户自建的 VRChat.exe profile 可能遮蔽官方 profile；只在 allow 门已开时用它。
        NvAPI_UnicodeString app_name{};
        constexpr wchar_t executable[] = L"VRChat.exe";
        for (size_t i = 0; i < std::size(executable); ++i) {
            app_name[i] = static_cast<NvU16>(executable[i]);
        }
        NVDRS_APPLICATION app{};
        app.version = NVDRS_APPLICATION_VER;
        NvDRSProfileHandle candidate = nullptr;
        if (NvAPI_DRS_FindApplicationByName(session, app_name, &candidate, &app) != NVAPI_OK) {
            return status;
        }
        if (read_raw(candidate, kAllowSetting, nullptr) != NVAPI_OK) {
            return status;
        }
        profile = candidate;
        return NVAPI_OK;
    }

    NvAPI_Status read_area(bool& found, float& percent) const {
        NvU32 raw = 0;
        const NvAPI_Status status = read_raw(profile, kAreaSetting, &raw);
        if (status == NVAPI_SETTING_NOT_FOUND) {
            found = false;
            return NVAPI_OK;
        }
        if (status != NVAPI_OK) {
            return status;
        }
        found = true;
        percent = bits_float(raw);
        return NVAPI_OK;
    }

    NvAPI_Status write_area(float percent) const {
        NVDRS_SETTING setting{};
        setting.version = NVDRS_SETTING_VER;
        setting.settingId = kAreaSetting;
        setting.settingType = NVDRS_DWORD_TYPE;
        setting.u32CurrentValue = float_bits(percent);
        return set_(session, profile, &setting, 0, 0);
    }

    NvAPI_Status read_setting(NvU32 id, NvU32& raw, bool& found) const {
        const NvAPI_Status status = read_raw(profile, id, &raw);
        if (status == NVAPI_SETTING_NOT_FOUND) {
            found = false;
            raw = 0;
            return NVAPI_OK;
        }
        found = status == NVAPI_OK;
        return status;
    }

private:
    using SetEx = NvAPI_Status(__cdecl*)(NvDRSSessionHandle, NvDRSProfileHandle, NVDRS_SETTING*,
                                        NvU32, NvU32);
    using GetEx = NvAPI_Status(__cdecl*)(NvDRSSessionHandle, NvDRSProfileHandle, NvU32,
                                        NVDRS_SETTING*, NvU32*);

    NvAPI_Status read_raw(NvDRSProfileHandle target, NvU32 id, NvU32* raw) const {
        NVDRS_SETTING setting{};
        setting.version = NVDRS_SETTING_VER;
        NvU32 extra = 0;
        const NvAPI_Status status = get_(session, target, id, &setting, &extra);
        if (status == NVAPI_OK && raw != nullptr) {
            *raw = setting.u32CurrentValue;
        }
        return status;
    }

    SetEx set_ = nullptr;
    GetEx get_ = nullptr;
};

inline bool set_percent(float percent, std::string& error) {
    if (!std::isfinite(percent) || percent < 1.0f || percent > 100.0f) {
        error = "VRSS 抗锯齿范围必须在 1..100%";
        return false;
    }
    Session session;
    const NvAPI_Status open = session.open();
    if (open != NVAPI_OK) {
        error = status_text(open, "打开 NVIDIA VRChat profile");
        return false;
    }
    const NvAPI_Status write = session.write_area(percent);
    if (write != NVAPI_OK) {
        error = status_text(write, "写入 VRSS 抗锯齿范围");
        return false;
    }
    const NvAPI_Status save = NvAPI_DRS_SaveSettings(session.session);
    if (save != NVAPI_OK) {
        error = status_text(save, "保存 NVIDIA profile");
        return false;
    }
    return true;
}

inline bool get_percent(float& percent, bool& found, std::string& error) {
    found = false;
    percent = 0.0f;
    Session session;
    const NvAPI_Status open = session.open();
    if (open != NVAPI_OK) {
        error = status_text(open, "读取 NVIDIA VRChat profile");
        return false;
    }
    const NvAPI_Status read = session.read_area(found, percent);
    if (read != NVAPI_OK) {
        error = status_text(read, "读取 VRSS 抗锯齿范围");
        return false;
    }
    return true;
}

// 诊断用：把 VRChat profile 里与 VRSS 相关的值全部读出来。
inline bool read_profile_text(std::string& text, std::string& error) {
    Session session;
    const NvAPI_Status open = session.open();
    if (open != NVAPI_OK) {
        error = status_text(open, "读取 NVIDIA VRChat profile");
        return false;
    }
    struct Entry {
        const char* name;
        NvU32 id;
        bool as_float;
    };
    constexpr Entry entries[] = {
        {"allow", kAllowSetting, false},   {"mode", kModeSetting, false},
        {"area", kAreaSetting, true},      {"aspect", kAspectSetting, true},
        {"gate", kHmdGateSetting, false},  {"layout", kVrLayoutSetting, false},
    };
    text = "ok=1\n";
    for (const Entry& entry : entries) {
        NvU32 raw = 0;
        bool found = false;
        const NvAPI_Status status = session.read_setting(entry.id, raw, found);
        if (status != NVAPI_OK) {
            error = status_text(status, "读取 VRSS profile 字段");
            return false;
        }
        text += std::string(entry.name) + "Found=" + (found ? "1" : "0") + "\n";
        if (found) {
            text += std::string(entry.name) + "=" +
                    (entry.as_float ? std::to_string(bits_float(raw)) : std::to_string(raw)) + "\n";
        }
    }
    return true;
}

} // namespace profile_area
