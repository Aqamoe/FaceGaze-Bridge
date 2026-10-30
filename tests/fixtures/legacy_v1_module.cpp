// 测试夹具：模拟 ABI v1 时代的模块 DLL。
//
// ABI v1 已移除（docs/adr/0034-native-module-v2-only.md），SDK 头里不再有
// `FgbModuleV1`。这里用本地最小 POD 复现旧导出，只为了验证宿主的失败路径：
// 老 DLL 必须被拒绝，并给出"用当前 SDK 头重新编译"的提示，而不是被误读。
#include <cstddef>
#include <cstdint>

namespace {

struct LegacyModuleV1 {
    std::uint32_t struct_size;
    std::uint32_t abi_version;
    const char* id;
    const char* display_name;
    std::uint32_t capabilities;
    int (*initialize)(void**, const void*, const void*);
    void (*update)(void*);
    void (*teardown)(void*);
};

const LegacyModuleV1 kLegacyModule = {
    sizeof(LegacyModuleV1),
    1, // ABI v1
    "com.example.legacy-v1",
    "Legacy V1 Fixture",
    0x2, // FGB_CAP_EXPRESSION（v1 的取值）
    nullptr,
    nullptr,
    nullptr,
};

} // namespace

extern "C" __declspec(dllexport) const LegacyModuleV1* fgb_module_query(
    std::uint32_t host_abi_version) {
    if (host_abi_version != 1) {
        return nullptr;
    }
    return &kLegacyModule;
}
