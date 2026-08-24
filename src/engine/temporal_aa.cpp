#include "engine/temporal_aa.hpp"

#include <algorithm>
#include <cmath>

namespace noemancer {
namespace {

float halton(std::uint32_t index,const std::uint32_t base) {
    float result{};
    float fraction=1.0F;
    while (index>0U) {
        fraction/=static_cast<float>(base);
        result+=fraction*static_cast<float>(index%base);
        index/=base;
    }
    return result;
}

} // namespace

TemporalJitter temporal_jitter(
    const std::uint64_t frame_index,const std::uint32_t width,const std::uint32_t height,const std::uint32_t sequence_length) {
    TemporalJitter result;
    if (width==0U || height==0U || sequence_length==0U) return result;
    result.sample_index=static_cast<std::uint32_t>(frame_index%sequence_length)+1U;
    result.pixel_offset={halton(result.sample_index,2U)-0.5F,halton(result.sample_index,3U)-0.5F};
    result.ndc_offset={2.0F*result.pixel_offset[0]/static_cast<float>(width),
        2.0F*result.pixel_offset[1]/static_cast<float>(height)};
    return result;
}

TemporalRenderExtent temporal_render_extent(
    const std::uint32_t output_width,const std::uint32_t output_height,const float requested_scale,
    const std::uint32_t minimum_dimension) {
    TemporalRenderExtent result{output_width,output_height,output_width,output_height,std::clamp(requested_scale,0.5F,1.0F)};
    if (output_width==0U || output_height==0U) return result;
    const auto minimum_width=std::min(minimum_dimension,output_width);
    const auto minimum_height=std::min(minimum_dimension,output_height);
    result.render_width=std::clamp(static_cast<std::uint32_t>(std::lround(static_cast<float>(output_width)*result.requested_scale)),minimum_width,output_width);
    result.render_height=std::clamp(static_cast<std::uint32_t>(std::lround(static_cast<float>(output_height)*result.requested_scale)),minimum_height,output_height);
    return result;
}

std::array<float,16> apply_projection_jitter(
    const std::array<float,16>& column_major_projection,const TemporalJitter& jitter) {
    auto result=column_major_projection;
    // The perspective matrix uses clip.w=-view.z. Offsetting the z column by
    // the negative NDC jitter produces the requested post-divide displacement.
    result[8]-=jitter.ndc_offset[0];
    result[9]-=jitter.ndc_offset[1];
    return result;
}

float linearize_device_depth(const float depth,const float near_clip,const float far_clip) {
    if (!(near_clip>0.0F) || !(far_clip>near_clip)) return 0.0F;
    const float clamped=std::clamp(depth,0.0F,1.0F);
    return near_clip*far_clip/std::max(far_clip-clamped*(far_clip-near_clip),0.000001F);
}

bool temporal_depth_compatible(
    const float current_depth,const float history_depth,const float near_clip,const float far_clip,
    const float relative_threshold,const float minimum_world_threshold) {
    if (current_depth>=1.0F || history_depth>=1.0F) return false;
    const float current=linearize_device_depth(current_depth,near_clip,far_clip);
    const float history=linearize_device_depth(history_depth,near_clip,far_clip);
    const float tolerance=std::max(minimum_world_threshold,current*relative_threshold);
    return std::abs(current-history)<=tolerance;
}

} // namespace noemancer
