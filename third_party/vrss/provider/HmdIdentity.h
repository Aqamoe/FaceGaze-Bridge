#pragma once
#include <cstdint>
#include <optional>
#include <string_view>

namespace hmd {
enum class Profile { Standard, SteamFrame };
enum class PropertyState { Available, Missing, Error };
struct Property { PropertyState state; int value; };
struct Identity { uint32_t vendor{},product{}; bool unspecified{}; };
inline bool CanRegister(const Identity& identity) {
    return !identity.unspecified && identity.vendor>0 && identity.product>0 &&
        identity.vendor<=65535 && identity.product<=65535;
}
inline std::optional<Identity> SelectSessionIdentity(Profile profile,
        const Identity& observed,bool ownerLifetime,bool profileTransaction) {
    if(!ownerLifetime || !profileTransaction || profile!=Profile::SteamFrame || !observed.unspecified ||
       observed.vendor || observed.product)return {};
    // Private provider/consumer key, not an acquired EDID or another HMD's identity.
    return Identity{0xffff,0xffff,false};
}
inline std::optional<Profile> ParseProfile(std::wstring_view name) {
    if(name==L"standard")return Profile::Standard;
    if(name==L"steam-frame")return Profile::SteamFrame;
    return {};
}
inline const wchar_t* ProfileName(Profile profile) {
    return profile==Profile::SteamFrame ? L"steam-frame" : L"standard";
}
inline std::optional<Identity> SelectIdentity(Profile profile,std::string_view model,
        std::string_view maker,Property vendor,Property product) {
    if(profile==Profile::SteamFrame &&
       ((model!="Steam Frame" && model!="Deckard MP") || maker!="Valve"))return {};
    if(vendor.state==PropertyState::Available && product.state==PropertyState::Available &&
       vendor.value>0 && product.value>0 && vendor.value<=65535 && product.value<=65535)
        return Identity{uint32_t(vendor.value),uint32_t(product.value),false};
    // An unspecified registration key is not an acquired EDID. Never substitute another HMD's ID.
    if(profile==Profile::SteamFrame && vendor.state==PropertyState::Missing && product.state==PropertyState::Missing)
        return Identity{0,0,true};
    return {};
}
}
