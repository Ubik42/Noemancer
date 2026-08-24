#include "engine/clustered_lighting.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace noemancer {
namespace {

struct Vec3 final { float x{}; float y{}; float z{}; };
Vec3 subtract(const std::array<float,3>& a,const std::array<float,3>& b){return {a[0]-b[0],a[1]-b[1],a[2]-b[2]};}
float dot(const Vec3 a,const Vec3 b){return a.x*b.x+a.y*b.y+a.z*b.z;}
Vec3 cross(const Vec3 a,const Vec3 b){return {a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x};}
Vec3 normalize(const Vec3 value){const auto length=std::sqrt(dot(value,value));return length>1.0e-6F?Vec3{value.x/length,value.y/length,value.z/length}:Vec3{};}
std::uint32_t depth_slice(const float depth,const float near_clip,const float far_clip,const std::uint32_t slices) {
    const auto ratio=std::log(std::max(depth,near_clip)/near_clip)/std::log(far_clip/near_clip);
    return std::min(slices-1U,static_cast<std::uint32_t>(std::floor(std::clamp(ratio,0.0F,0.999999F)*static_cast<float>(slices))));
}

} // namespace

ClusteredLightingAssignment build_clustered_lighting(const ClusteredLightingConfig& config,
    const ClusteredLightingCamera& camera,std::span<const RenderLocalLightSnapshot> lights) {
    ClusteredLightingAssignment result;
    if(config.tiles_x==0||config.tiles_y==0||config.depth_slices==0||config.maximum_lights_per_cluster==0||
       !std::isfinite(camera.near_clip)||!std::isfinite(camera.far_clip)||camera.near_clip<=0||camera.far_clip<=camera.near_clip)
        return result;
    const auto cluster_count=static_cast<std::size_t>(config.tiles_x)*config.tiles_y*config.depth_slices;
    std::vector<std::vector<std::uint32_t>> lists(cluster_count);
    const auto forward=normalize(subtract(camera.target,camera.position));
    const auto right=normalize(cross(forward,{0,1,0}));
    const auto up=normalize(cross(right,forward));
    if(dot(forward,forward)<0.5F||dot(right,right)<0.5F)return result;
    const auto accepted=std::min<std::size_t>(lights.size(),config.maximum_lights);
    result.accepted_light_count=static_cast<std::uint32_t>(accepted);
    result.dropped_light_count=static_cast<std::uint32_t>(lights.size()-accepted);
    const auto half_height=std::max(camera.orthographic_height*0.5F,0.001F);
    const auto tan_y=std::tan(std::clamp(camera.vertical_fov_degrees,1.0F,179.0F)*0.00872664626F);
    const auto tan_x=tan_y*std::max(camera.aspect_ratio,0.001F);
    for(std::size_t light_index=0;light_index<accepted;++light_index) {
        const auto& light=lights[light_index];
        const auto relative=subtract(light.position,camera.position);
        const auto view_x=dot(relative,right),view_y=dot(relative,up),view_z=dot(relative,forward);
        const auto radius=std::max(light.range_meters,0.001F);
        const auto depth_min=std::max(camera.near_clip,view_z-radius);
        const auto depth_max=std::min(camera.far_clip,view_z+radius);
        if(depth_min>depth_max)continue;
        float ndc_x{},ndc_y{},radius_x{},radius_y{};
        if(camera.orthographic) {
            ndc_x=view_x/(half_height*std::max(camera.aspect_ratio,0.001F));ndc_y=view_y/half_height;
            radius_x=radius/(half_height*std::max(camera.aspect_ratio,0.001F));radius_y=radius/half_height;
        } else {
            const auto projection_depth=std::max(view_z,camera.near_clip);
            const auto conservative_depth=std::max(depth_min,camera.near_clip);
            ndc_x=view_x/(projection_depth*tan_x);ndc_y=view_y/(projection_depth*tan_y);
            radius_x=radius/(conservative_depth*tan_x);radius_y=radius/(conservative_depth*tan_y);
        }
        const auto tile_min_x=std::clamp(static_cast<int>(std::floor((ndc_x-radius_x)*0.5F*config.tiles_x+0.5F*config.tiles_x)),0,static_cast<int>(config.tiles_x)-1);
        const auto tile_max_x=std::clamp(static_cast<int>(std::floor((ndc_x+radius_x)*0.5F*config.tiles_x+0.5F*config.tiles_x)),0,static_cast<int>(config.tiles_x)-1);
        const auto tile_min_y=std::clamp(static_cast<int>(std::floor((-ndc_y-radius_y)*0.5F*config.tiles_y+0.5F*config.tiles_y)),0,static_cast<int>(config.tiles_y)-1);
        const auto tile_max_y=std::clamp(static_cast<int>(std::floor((-ndc_y+radius_y)*0.5F*config.tiles_y+0.5F*config.tiles_y)),0,static_cast<int>(config.tiles_y)-1);
        if(ndc_x+radius_x<-1||ndc_x-radius_x>1||ndc_y+radius_y<-1||ndc_y-radius_y>1)continue;
        const auto slice_min=depth_slice(depth_min,camera.near_clip,camera.far_clip,config.depth_slices);
        const auto slice_max=depth_slice(depth_max,camera.near_clip,camera.far_clip,config.depth_slices);
        for(auto z=slice_min;z<=slice_max;++z)for(auto y=tile_min_y;y<=tile_max_y;++y)for(auto x=tile_min_x;x<=tile_max_x;++x) {
            auto& list=lists[static_cast<std::size_t>(x)+static_cast<std::size_t>(y)*config.tiles_x+
                static_cast<std::size_t>(z)*config.tiles_x*config.tiles_y];
            if(list.size()<config.maximum_lights_per_cluster)list.push_back(static_cast<std::uint32_t>(light_index));
            else ++result.overflowed_assignments;
        }
    }
    result.clusters.reserve(cluster_count);
    for(const auto& list:lists) {
        result.clusters.push_back({static_cast<std::uint32_t>(result.light_indices.size()),static_cast<std::uint32_t>(list.size())});
        result.light_indices.insert(result.light_indices.end(),list.begin(),list.end());
    }
    return result;
}

} // namespace noemancer
