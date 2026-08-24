#include "engine/cascaded_shadow.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace noemancer {
namespace {

ShadowVec3 operator+(const ShadowVec3 a,const ShadowVec3 b){return {a.x+b.x,a.y+b.y,a.z+b.z};}
ShadowVec3 operator-(const ShadowVec3 a,const ShadowVec3 b){return {a.x-b.x,a.y-b.y,a.z-b.z};}
ShadowVec3 operator*(const ShadowVec3 value,const float scale){return {value.x*scale,value.y*scale,value.z*scale};}
float dot(const ShadowVec3 a,const ShadowVec3 b){return a.x*b.x+a.y*b.y+a.z*b.z;}
ShadowVec3 cross(const ShadowVec3 a,const ShadowVec3 b){return {a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x};}
float length(const ShadowVec3 value){return std::sqrt(dot(value,value));}
ShadowVec3 normalize(const ShadowVec3 value){const float magnitude=length(value);return magnitude>1.0e-6F?value*(1.0F/magnitude):ShadowVec3{};}

ShadowMat4 identity(){ShadowMat4 result{};result.value[0]=result.value[5]=result.value[10]=result.value[15]=1.0F;return result;}
ShadowMat4 multiply(const ShadowMat4& left,const ShadowMat4& right){
    ShadowMat4 result{};
    for(int column=0;column<4;++column) for(int row=0;row<4;++row) for(int k=0;k<4;++k)
        result.value[column*4+row]+=left.value[k*4+row]*right.value[column*4+k];
    return result;
}
ShadowMat4 look_at(const ShadowVec3 eye,const ShadowVec3 target,const ShadowVec3 up){
    const auto forward=normalize(target-eye),side=normalize(cross(forward,up)),corrected_up=cross(side,forward);
    auto result=identity();
    result.value[0]=side.x;result.value[1]=corrected_up.x;result.value[2]=-forward.x;
    result.value[4]=side.y;result.value[5]=corrected_up.y;result.value[6]=-forward.y;
    result.value[8]=side.z;result.value[9]=corrected_up.z;result.value[10]=-forward.z;
    result.value[12]=-dot(side,eye);result.value[13]=-dot(corrected_up,eye);result.value[14]=dot(forward,eye);
    return result;
}
ShadowMat4 orthographic(const float left,const float right,const float bottom,const float top,const float near_plane,const float far_plane){
    auto result=identity(); result.value[0]=2.0F/(right-left); result.value[5]=2.0F/(top-bottom);
    result.value[10]=1.0F/(near_plane-far_plane); result.value[12]=-(right+left)/(right-left);
    result.value[13]=-(top+bottom)/(top-bottom); result.value[14]=near_plane/(near_plane-far_plane); return result;
}

bool finite_vec(const ShadowVec3 value){return std::isfinite(value.x)&&std::isfinite(value.y)&&std::isfinite(value.z);}

} // namespace

CascadedShadowPlan build_cascaded_shadow_plan(const ShadowVec3 camera_position,const ShadowVec3 camera_target,
    const ShadowVec3 camera_up,const float vertical_fov_radians,const float aspect_ratio,const float near_clip,
    const float far_clip,const ShadowVec3 light_direction,const CascadedShadowConfig& config) {
    CascadedShadowPlan result; result.config=config;
    const auto camera_forward=normalize(camera_target-camera_position),camera_right=normalize(cross(camera_forward,camera_up));
    const auto camera_vertical=normalize(cross(camera_right,camera_forward)),light_forward=normalize(light_direction);
    if (config.cascade_count==0U||config.cascade_count>CascadedShadowConfig::maximum_cascades||config.resolution<64U||config.resolution>16384U||
        config.maximum_distance<=near_clip||config.split_lambda<0.0F||config.split_lambda>1.0F||config.depth_padding<=0.0F||
        near_clip<=0.0F||far_clip<=near_clip||vertical_fov_radians<=0.0F||vertical_fov_radians>=3.13F||aspect_ratio<=0.0F||
        !finite_vec(camera_position)||!finite_vec(camera_target)||!finite_vec(camera_up)||!finite_vec(light_direction)||
        !std::isfinite(vertical_fov_radians)||!std::isfinite(aspect_ratio)||!std::isfinite(near_clip)||!std::isfinite(far_clip)||
        !std::isfinite(config.maximum_distance)||!std::isfinite(config.split_lambda)||!std::isfinite(config.depth_padding)||
        length(camera_forward)<0.99F||length(camera_right)<0.99F||length(light_forward)<0.99F) {
        result.code="shadow.csm-invalid-input"; result.detail="CSM camera, light or profile parameters are invalid."; return result;
    }
    const float shadow_far=std::min(far_clip,config.maximum_distance),tangent=std::tan(vertical_fov_radians*0.5F);
    float previous=near_clip;
    for(std::uint32_t index=0;index<config.cascade_count;++index) {
        const float fraction=static_cast<float>(index+1U)/static_cast<float>(config.cascade_count);
        const float logarithmic=near_clip*std::pow(shadow_far/near_clip,fraction);
        const float linear=near_clip+(shadow_far-near_clip)*fraction;
        const float split=config.split_lambda*logarithmic+(1.0F-config.split_lambda)*linear;
        std::array<ShadowVec3,8> corners{}; std::size_t corner{};
        for(const float distance:{previous,split}) {
            const float half_height=tangent*distance,half_width=half_height*aspect_ratio;
            const auto center=camera_position+camera_forward*distance;
            corners[corner++]=center-camera_right*half_width-camera_vertical*half_height;
            corners[corner++]=center+camera_right*half_width-camera_vertical*half_height;
            corners[corner++]=center-camera_right*half_width+camera_vertical*half_height;
            corners[corner++]=center+camera_right*half_width+camera_vertical*half_height;
        }
        ShadowVec3 center{}; for(const auto value:corners) center=center+value; center=center*(1.0F/8.0F);
        float radius{}; for(const auto value:corners) radius=std::max(radius,length(value-center));
        radius=std::ceil(radius*16.0F)/16.0F;
        const auto light_up=std::abs(light_forward.y)<0.95F?ShadowVec3{0,1,0}:ShadowVec3{1,0,0};
        const auto light_right=normalize(cross(light_forward,light_up)),light_vertical=normalize(cross(light_right,light_forward));
        const float texel_size=2.0F*radius/static_cast<float>(config.resolution);
        const float center_right=std::round(dot(center,light_right)/texel_size)*texel_size;
        const float center_vertical=std::round(dot(center,light_vertical)/texel_size)*texel_size;
        const float center_forward=dot(center,light_forward);
        const auto snapped=light_right*center_right+light_vertical*center_vertical+light_forward*center_forward;
        const float eye_distance=radius+config.depth_padding;
        const auto view=look_at(snapped-light_forward*eye_distance,snapped,light_up);
        const auto projection=orthographic(-radius,radius,-radius,radius,0.1F,2.0F*eye_distance);
        auto& cascade=result.cascades[index]; cascade.near_distance=previous; cascade.far_distance=split;
        cascade.radius=radius; cascade.world_units_per_texel=texel_size; cascade.snapped_center=snapped;
        cascade.view_projection=multiply(projection,view); previous=split;
        if (!finite_vec(snapped)||!std::isfinite(radius)||!std::isfinite(texel_size)) {
            result.code="shadow.csm-non-finite"; result.detail="CSM fitting generated a non-finite cascade."; return result;
        }
    }
    result.texture_bytes=static_cast<std::uint64_t>(config.resolution)*config.resolution*4ULL*config.cascade_count;
    result.valid=true; result.code="ok"; result.detail="Practical-split cascades fitted and stabilized to shadow texels."; return result;
}

} // namespace noemancer
