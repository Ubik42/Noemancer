#include "engine/cascaded_shadow.hpp"

#include <cmath>
#include <iostream>

int main() {
    using namespace noemancer;
    const CascadedShadowConfig config{};
    const auto plan=build_cascaded_shadow_plan({7,5.5F,8.5F},{0,1,0},{0,1,0},0.785398F,16.0F/9.0F,0.1F,100.0F,{-0.55F,-1,-0.35F},config);
    if (!plan.valid || plan.texture_bytes!=67108864ULL) {
        std::cerr<<"Four-cascade shadow plan or D32 memory budget is invalid\n"; return 1;
    }
    float previous=0.1F;
    for(std::uint32_t index=0;index<config.cascade_count;++index) {
        const auto& cascade=plan.cascades[index];
        if (std::abs(cascade.near_distance-previous)>0.001F || cascade.far_distance<=cascade.near_distance ||
            cascade.radius<=0.0F || cascade.world_units_per_texel<=0.0F) {
            std::cerr<<"Cascade split, radius or texel scale is not monotonic\n"; return 2;
        }
        previous=cascade.far_distance;
    }
    if (std::abs(previous-config.maximum_distance)>0.001F) {
        std::cerr<<"Last cascade does not terminate at the configured shadow distance\n"; return 3;
    }
    const auto jittered=build_cascaded_shadow_plan({7.00001F,5.5F,8.5F},{0.00001F,1,0},{0,1,0},
        0.785398F,16.0F/9.0F,0.1F,100.0F,{-0.55F,-1,-0.35F},config);
    for(std::uint32_t index=0;index<config.cascade_count;++index) {
        if (!jittered.valid || std::abs(plan.cascades[index].view_projection.value[12]-jittered.cascades[index].view_projection.value[12])>1.0e-6F ||
            std::abs(plan.cascades[index].view_projection.value[13]-jittered.cascades[index].view_projection.value[13])>1.0e-6F) {
            std::cerr<<"Sub-texel camera jitter changed a stabilized shadow projection\n"; return 5;
        }
    }
    const auto invalid=build_cascaded_shadow_plan({0,0,0},{0,0,0},{0,1,0},0.7F,1.0F,0.1F,100.0F,{0,-1,0},config);
    if (invalid.valid || invalid.code!="shadow.csm-invalid-input") {
        std::cerr<<"Degenerate CSM camera was not rejected\n"; return 4;
    }
    return 0;
}
