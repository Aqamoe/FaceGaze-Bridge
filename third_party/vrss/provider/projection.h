#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <stdexcept>
struct EyeGeometry {
    std::array<double,4> projection{}; // OpenVR left, right, top, bottom.
    std::array<double,9> eyeToHead{};
};
struct Gain { double x=.5,y=.55,leftDegrees=0,rightDegrees=0; };
inline std::array<float,2> ProjectHeadTangent(double tx,double ty,
        const EyeGeometry& eye,const Gain& gain,int index) {
    const auto& p=eye.projection;
    if(!(p[0]<0 && p[1]>0 && p[2]<0 && p[3]>0)) throw std::runtime_error("invalid projection");
    const double angle=(index==0?gain.leftDegrees:gain.rightDegrees)*3.141592653589793/180;
    // BrokenEye parser restores source X. Convert its forward-positive ray to
    // OpenVR's right/up/back coordinate system before the inverse eye rotation.
    const double h[3]={std::tan(std::atan(tx*gain.x)+angle),-ty*gain.y,-1};
    double v[3]{};
    for(int i=0;i<3;++i) for(int j=0;j<3;++j) v[i]+=eye.eyeToHead[j*3+i]*h[j];
    if(!std::isfinite(v[0]) || !std::isfinite(v[1]) || !std::isfinite(v[2]) || v[2]>=-1e-6)
        throw std::runtime_error("invalid gaze ray");
    const double x=2*(v[0]/-v[2]-p[0])/(p[1]-p[0])-1;
    const double y=2*(v[1]/-v[2])/(p[3]-p[2])+(p[2]+p[3])/(p[3]-p[2]);
    return {float(std::clamp(x,-1.,1.)),float(std::clamp(y,-1.,1.))};
}
