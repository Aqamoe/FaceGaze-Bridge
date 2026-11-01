#include "openvr/overlay.hpp"

#include "base/log.hpp"
#include "openvr/steamvr.hpp"

#include <cmath>

#ifdef _WIN32
#include <windows.h>
#endif

#if defined(_MSC_VER)
#pragma warning(push, 0)
#endif
#include "openvr/openvr.h"
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

namespace fgb::overlay {
namespace {

constexpr char kTag[] = "overlay";
constexpr char kOverlayKey[] = "com.facegazebridge.gazecalib";
constexpr char kOverlayName[] = "FaceGaze Bridge 注视校准";

// 圆点纹理：白色 + alpha 渐变。纯白的好处是无论底层是 RGBA 还是 BGRA，看起来都一样。
constexpr int kTextureSize = 128;

struct ApiImpl {
    using InitFn = std::uint32_t(VR_CALLTYPE*)(vr::EVRInitError*, vr::EVRApplicationType, const char*);
    using ShutdownFn = void(VR_CALLTYPE*)();
    using InterfaceFn = void*(VR_CALLTYPE*)(const char*, vr::EVRInitError*);
    using DescribeFn = const char*(VR_CALLTYPE*)(vr::EVRInitError);

    InitFn init           = nullptr;
    ShutdownFn shutdown   = nullptr;
    InterfaceFn interface = nullptr;
    DescribeFn describe   = nullptr;
};

} // namespace

struct GazeDot::Api {
    ApiImpl impl;
    bool initialized = false;
};

GazeDot::GazeDot() = default;

GazeDot::~GazeDot() { close(); }

bool GazeDot::ready() const noexcept { return overlay_ != nullptr && handle_ != 0; }

bool GazeDot::open(std::string& error) {
#ifdef _WIN32
    error.clear();
    if (ready()) {
        return true;
    }
    if (api_ == nullptr) {
        const std::filesystem::path runtime = steamvr::find_runtime_dir();
        if (runtime.empty()) {
            error = "没找到 SteamVR 运行库（标定需要 SteamVR 正在运行）";
            return false;
        }
        const std::filesystem::path dll = steamvr::dll_path_for(runtime);
        auto* module = static_cast<HMODULE>(::LoadLibraryW(dll.wstring().c_str()));
        if (module == nullptr) {
            error = "加载 openvr_api.dll 失败";
            return false;
        }
        api_ = new Api{};
        api_->impl.init = reinterpret_cast<ApiImpl::InitFn>(::GetProcAddress(module, "VR_InitInternal2"));
        api_->impl.shutdown =
            reinterpret_cast<ApiImpl::ShutdownFn>(::GetProcAddress(module, "VR_ShutdownInternal"));
        api_->impl.interface = reinterpret_cast<ApiImpl::InterfaceFn>(
            ::GetProcAddress(module, "VR_GetGenericInterface"));
        api_->impl.describe = reinterpret_cast<ApiImpl::DescribeFn>(
            ::GetProcAddress(module, "VR_GetVRInitErrorAsEnglishDescription"));
        if (api_->impl.init == nullptr || api_->impl.shutdown == nullptr ||
            api_->impl.interface == nullptr) {
            ::FreeLibrary(module);
            delete api_;
            api_    = nullptr;
            error = "openvr_api.dll 缺少必要导出";
            return false;
        }
        module_ = module;
    }

    vr::EVRInitError init_error = vr::VRInitError_None;
    api_->impl.init(&init_error, vr::VRApplication_Background, nullptr);
    if (init_error != vr::VRInitError_None) {
        const char* text = api_->impl.describe != nullptr ? api_->impl.describe(init_error) : nullptr;
        error = std::string{"OpenVR 初始化失败："} + (text != nullptr ? text : "未知错误");
        return false;
    }
    api_->initialized = true;

    vr::EVRInitError interface_error = vr::VRInitError_None;
    overlay_ = static_cast<vr::IVROverlay*>(api_->impl.interface(vr::IVROverlay_Version,
                                                                &interface_error));
    if (overlay_ == nullptr || interface_error != vr::VRInitError_None) {
        error = "IVROverlay 不可用";
        overlay_ = nullptr;
        return false;
    }

    const vr::EVROverlayError created =
        overlay_->CreateOverlay(kOverlayKey, kOverlayName, &handle_);
    if (created != vr::VROverlayError_None) {
        FGB_WARN(kTag, "创建 overlay 失败：%s", overlay_->GetOverlayErrorNameFromEnum(created));
        handle_ = 0;
        error   = "创建 overlay 失败";
        return false;
    }
    // 圆点本身是白点 + 透明底，不作为 dashboard 标签、也不吃鼠标输入
    overlay_->SetOverlayInputMethod(handle_, vr::VROverlayInputMethod_None);
    overlay_->SetOverlayAlpha(handle_, 0.85f);

    // 一次性生成纹理：中间白、边缘按半径做抗锯齿过渡
    pixels_.assign(static_cast<std::size_t>(kTextureSize) * kTextureSize * 4, 0);
    const float center = (kTextureSize - 1) * 0.5f;
    const float radius = kTextureSize * 0.42f;
    for (int y = 0; y < kTextureSize; ++y) {
        for (int x = 0; x < kTextureSize; ++x) {
            const float dx   = static_cast<float>(x) - center;
            const float dy   = static_cast<float>(y) - center;
            const float dist = std::sqrt(dx * dx + dy * dy);
            float alpha      = (radius - dist) / 1.5f; // 1.5 px 过渡带
            alpha            = alpha > 1.0f ? 1.0f : (alpha < 0.0f ? 0.0f : alpha);
            const auto index = static_cast<std::size_t>(y * kTextureSize + x) * 4;
            pixels_[index + 0] = 255;
            pixels_[index + 1] = 255;
            pixels_[index + 2] = 255;
            pixels_[index + 3] = static_cast<unsigned char>(alpha * 255.0f);
        }
    }
    const vr::EVROverlayError uploaded = overlay_->SetOverlayRaw(
        handle_, pixels_.data(), static_cast<std::uint32_t>(kTextureSize),
        static_cast<std::uint32_t>(kTextureSize), 4);
    if (uploaded != vr::VROverlayError_None) {
        FGB_WARN(kTag, "上传 overlay 纹理失败：%s",
                 overlay_->GetOverlayErrorNameFromEnum(uploaded));
        error = "上传 overlay 纹理失败";
        return false;
    }
    FGB_INFO(kTag, "标定 overlay 就绪");
    return true;
#else
    (void)error;
    return false;
#endif
}

void GazeDot::hide() noexcept {
#ifdef _WIN32
    if (overlay_ != nullptr && handle_ != 0) {
        overlay_->HideOverlay(handle_);
    }
#endif
}

void GazeDot::set_alpha(float alpha) noexcept {
#ifdef _WIN32
    if (overlay_ != nullptr && handle_ != 0) {
        overlay_->SetOverlayAlpha(handle_, alpha < 0.0f ? 0.0f : (alpha > 1.0f ? 1.0f : alpha));
    }
#else
    (void)alpha;
#endif
}

bool GazeDot::show_at(float yaw_deg, float pitch_deg, float radius_deg, float distance_m,
                      std::string& error) {
#ifdef _WIN32
    if (!ready()) {
        error = "overlay 未就绪";
        return false;
    }
    const float yaw   = yaw_deg * 3.14159265358979323846f / 180.0f;
    const float pitch = pitch_deg * 3.14159265358979323846f / 180.0f;
    const float dist  = distance_m > 0.1f ? distance_m : 1.5f;

    // 头显坐标系：+x 右、+y 上、-z 前（与 gazeDirLocal 同一套）
    vr::HmdMatrix34_t transform{};
    transform.m[0][0] = 1.0f;
    transform.m[1][1] = 1.0f;
    transform.m[2][2] = 1.0f;
    transform.m[0][3] = std::tan(yaw) * dist;
    transform.m[1][3] = std::tan(pitch) * dist;
    transform.m[2][3] = -dist;

    // 角半径 → 物理宽度：宽度 = 2·d·tan(r)，这样换个距离看起来一样大
    const float radius_rad = radius_deg * 3.14159265358979323846f / 180.0f;
    const float width_m    = 2.0f * dist * std::tan(radius_rad);
    overlay_->SetOverlayWidthInMeters(handle_, width_m);

    const vr::EVROverlayError moved = overlay_->SetOverlayTransformTrackedDeviceRelative(
        handle_, vr::k_unTrackedDeviceIndex_Hmd, &transform);
    if (moved != vr::VROverlayError_None) {
        error = std::string{"摆放 overlay 失败："} +
                overlay_->GetOverlayErrorNameFromEnum(moved);
        return false;
    }
    const vr::EVROverlayError shown = overlay_->ShowOverlay(handle_);
    if (shown != vr::VROverlayError_None) {
        error = std::string{"显示 overlay 失败："} +
                overlay_->GetOverlayErrorNameFromEnum(shown);
        return false;
    }
    error.clear();
    return true;
#else
    (void)yaw_deg;
    (void)pitch_deg;
    (void)radius_deg;
    (void)distance_m;
    (void)error;
    return false;
#endif
}

void GazeDot::close() noexcept {
#ifdef _WIN32
    if (overlay_ != nullptr && handle_ != 0) {
        overlay_->HideOverlay(handle_);
        overlay_->DestroyOverlay(handle_);
        handle_ = 0;
    }
    overlay_ = nullptr;
    if (api_ != nullptr) {
        if (api_->initialized && api_->impl.shutdown != nullptr) {
            api_->impl.shutdown();
            api_->initialized = false;
        }
        delete api_;
        api_ = nullptr;
    }
    if (module_ != nullptr) {
        ::FreeLibrary(static_cast<HMODULE>(module_));
        module_ = nullptr;
    }
    pixels_.clear();
#endif
}

} // namespace fgb::overlay
