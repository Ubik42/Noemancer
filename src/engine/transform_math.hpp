#pragma once

#include "engine/scene_document.hpp"

#include <algorithm>
#include <cmath>

namespace noemancer {

struct TransformQuaternion final {
    float x{};
    float y{};
    float z{};
    float w{1.0F};
};

inline TransformQuaternion normalized_quaternion(TransformQuaternion value) {
    const auto length=std::sqrt(value.x*value.x+value.y*value.y+value.z*value.z+value.w*value.w);
    if(!std::isfinite(length)||length<0.000001F) return {};
    value.x/=length;value.y/=length;value.z/=length;value.w/=length;
    if(value.w<0.0F) {value.x=-value.x;value.y=-value.y;value.z=-value.z;value.w=-value.w;}
    return value;
}

inline TransformQuaternion quaternion_from_euler_degrees(const SceneVector3 degrees) {
    constexpr double radians_per_degree=0.017453292519943295769;
    const auto roll=degrees.x*radians_per_degree*0.5,pitch=degrees.y*radians_per_degree*0.5,yaw=degrees.z*radians_per_degree*0.5;
    const auto cr=std::cos(roll),sr=std::sin(roll),cp=std::cos(pitch),sp=std::sin(pitch),cy=std::cos(yaw),sy=std::sin(yaw);
    return normalized_quaternion({static_cast<float>(sr*cp*cy-cr*sp*sy),static_cast<float>(cr*sp*cy+sr*cp*sy),
        static_cast<float>(cr*cp*sy-sr*sp*cy),static_cast<float>(cr*cp*cy+sr*sp*sy)});
}

inline SceneVector3 euler_degrees_from_quaternion(const TransformQuaternion source) {
    const auto q=normalized_quaternion(source);
    constexpr double degrees_per_radian=57.295779513082320877;
    const auto roll=std::atan2(2.0*(q.w*q.x+q.y*q.z),1.0-2.0*(q.x*q.x+q.y*q.y));
    const auto pitch=std::asin(std::clamp(2.0*(q.w*q.y-q.z*q.x),-1.0,1.0));
    const auto yaw=std::atan2(2.0*(q.w*q.z+q.x*q.y),1.0-2.0*(q.y*q.y+q.z*q.z));
    return {roll*degrees_per_radian,pitch*degrees_per_radian,yaw*degrees_per_radian};
}

} // namespace noemancer
