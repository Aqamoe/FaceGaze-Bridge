#pragma once
#include "../openvr/openvr.h"
#include "FrameGaze.h"
#include "HmdIdentity.h"

class OpenVrFrameSource {
    vr::IVRSystem* system_{};
    bool steamFrame_{};
public:
    OpenVrFrameSource() = default;
    OpenVrFrameSource(const OpenVrFrameSource&) = delete;
    OpenVrFrameSource& operator=(const OpenVrFrameSource&) = delete;
    ~OpenVrFrameSource() { if(system_)vr::VR_Shutdown(); }
    hmd::Identity Open(EyeGeometry (&eyes)[2],hmd::Profile profile) {
        steamFrame_=profile==hmd::Profile::SteamFrame;
        vr::EVRInitError error{};
        system_=vr::VR_Init(&error,vr::VRApplication_Background);
        if(!system_ || error!=vr::VRInitError_None)
            throw std::runtime_error("SteamVR eye-tracking interface unavailable: "+std::to_string(error));
        if(!system_->IsTrackedDeviceConnected(vr::k_unTrackedDeviceIndex_Hmd))
            throw std::runtime_error("Connect Steam Frame to PC SteamVR first");
        vr::ETrackedPropertyError a{},b{};
        const int vid=system_->GetInt32TrackedDeviceProperty(0,vr::Prop_EdidVendorID_Int32,&a);
        const int pid=system_->GetInt32TrackedDeviceProperty(0,vr::Prop_EdidProductID_Int32,&b);
        char model[256]{},maker[256]{};vr::ETrackedPropertyError modelError{},makerError{};
        system_->GetStringTrackedDeviceProperty(0,vr::Prop_ModelNumber_String,model,sizeof(model),&modelError);
        system_->GetStringTrackedDeviceProperty(0,vr::Prop_ManufacturerName_String,maker,sizeof(maker),&makerError);
        if(modelError!=vr::TrackedProp_Success || makerError!=vr::TrackedProp_Success)
            throw std::runtime_error("Cannot verify the connected HMD model and manufacturer");
        const auto state=[](vr::ETrackedPropertyError value) {
            return value==vr::TrackedProp_Success ? hmd::PropertyState::Available :
                value==vr::TrackedProp_UnknownProperty ? hmd::PropertyState::Missing : hmd::PropertyState::Error;
        };
        const auto identity=hmd::SelectIdentity(profile,model,maker,{state(a),vid},{state(b),pid});
        std::cout<<"hmdProfile="<<(profile==hmd::Profile::SteamFrame?"steam-frame":"standard")
                 <<" model="<<model<<" manufacturer="<<maker
                 <<" edidVendorStatus="<<system_->GetPropErrorNameFromEnum(a)
                 <<" edidProductStatus="<<system_->GetPropErrorNameFromEnum(b)<<'\n';
        if(!identity)throw std::runtime_error("HMD identity rejected: verify the selected profile, connected model and EDID properties");
        std::cout<<"identity="<<(identity->unspecified?"unspecified-no-edid":"actual-edid")
                 <<" vendor="<<identity->vendor<<" product="<<identity->product<<'\n';
        for(int i=0;i<2;++i) {
            float l,r,t,bottom;
            system_->GetProjectionRaw(static_cast<vr::EVREye>(i),&l,&r,&t,&bottom);
            if(!std::isfinite(l)||!std::isfinite(r)||!std::isfinite(t)||!std::isfinite(bottom)||
               !(l<0&&r>0&&t<0&&bottom>0))throw std::runtime_error("Invalid SteamVR HMD projection");
            eyes[i].projection={l,r,t,bottom};
            const auto pose=system_->GetEyeToHeadTransform(static_cast<vr::EVREye>(i));
            for(int row=0;row<3;++row)for(int col=0;col<3;++col)
                eyes[i].eyeToHead[row*3+col]=pose.m[row][col];
        }
        return *identity;
    }
    bool Read(const Gain& gain,std::array<float,4>& location) {
        vr::VREvent_t event{};
        while(system_->PollNextEvent(&event,sizeof(event)))
            if(event.eventType==vr::VREvent_Quit) {
                system_->AcknowledgeQuit_Exiting();
                throw std::runtime_error("SteamVR requested shutdown; restart the Frame connection after SteamVR reconnects");
            }
        vr::HmdVector2_t left{},right{};
        const bool valid=system_->IsTrackedDeviceConnected(0) &&
            system_->GetEyeTrackedFoveationCenter(&left,&right);
        return frame::MapNdc(valid,{left.v[0],left.v[1],right.v[0],right.v[1]},gain,location,steamFrame_);
    }
};
