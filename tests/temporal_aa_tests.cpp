#include "engine/temporal_aa.hpp"

#include <array>
#include <cmath>
#include <iostream>

namespace {
bool near(const float left,const float right,const float tolerance=0.0001F) {
    return std::abs(left-right)<=tolerance;
}
}

int main() {
    using namespace noemancer;
    const auto first=temporal_jitter(0,100,50);
    const auto second=temporal_jitter(1,100,50);
    if (first.sample_index!=1U || !near(first.pixel_offset[0],0.0F) || !near(first.pixel_offset[1],-1.0F/6.0F) ||
        !near(second.pixel_offset[0],-0.25F) || !near(second.pixel_offset[1],1.0F/6.0F)) {
        std::cerr<<"Halton(2,3) temporal sequence is not deterministic\n"; return 1;
    }
    const auto extent=temporal_render_extent(960,540,0.67F);
    const auto clamped_extent=temporal_render_extent(100,50,0.1F);
    if (extent.render_width!=643U || extent.render_height!=362U ||
        clamped_extent.render_width!=64U || clamped_extent.render_height!=50U || !near(clamped_extent.requested_scale,0.5F)) {
        std::cerr<<"Temporal render extent scaling or clamping failed\n"; return 5;
    }
    std::array<float,16> projection{};
    projection[0]=projection[5]=projection[10]=projection[15]=1.0F;
    const auto jittered=apply_projection_jitter(projection,second);
    if (!near(jittered[8],-second.ndc_offset[0]) || !near(jittered[9],-second.ndc_offset[1]) || jittered[0]!=1.0F) {
        std::cerr<<"Projection jitter modified the wrong matrix terms\n"; return 2;
    }
    if (!near(linearize_device_depth(0.0F,0.1F,100.0F),0.1F) ||
        !near(linearize_device_depth(1.0F,0.1F,100.0F),100.0F,0.01F)) {
        std::cerr<<"D3D device depth linearization failed\n"; return 3;
    }
    if (!temporal_depth_compatible(0.50F,0.5001F,0.1F,100.0F) ||
        temporal_depth_compatible(0.50F,0.90F,0.1F,100.0F) ||
        temporal_depth_compatible(1.0F,1.0F,0.1F,100.0F)) {
        std::cerr<<"Temporal disocclusion threshold rejected the wrong history\n"; return 4;
    }
    return 0;
}
