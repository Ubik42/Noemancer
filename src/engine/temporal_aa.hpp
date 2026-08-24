#pragma once

#include <array>
#include <cstdint>

namespace noemancer {

struct TemporalJitter final {
    std::uint32_t sample_index{};
    std::array<float,2> pixel_offset{};
    std::array<float,2> ndc_offset{};
};

struct TemporalRenderExtent final {
    std::uint32_t output_width{};
    std::uint32_t output_height{};
    std::uint32_t render_width{};
    std::uint32_t render_height{};
    float requested_scale{1.0F};
};

[[nodiscard]] TemporalJitter temporal_jitter(
    std::uint64_t frame_index,std::uint32_t width,std::uint32_t height,std::uint32_t sequence_length=8);
[[nodiscard]] TemporalRenderExtent temporal_render_extent(
    std::uint32_t output_width,std::uint32_t output_height,float requested_scale,std::uint32_t minimum_dimension=64);
[[nodiscard]] std::array<float,16> apply_projection_jitter(
    const std::array<float,16>& column_major_projection,const TemporalJitter& jitter);
[[nodiscard]] float linearize_device_depth(float depth,float near_clip,float far_clip);
[[nodiscard]] bool temporal_depth_compatible(
    float current_depth,float history_depth,float near_clip,float far_clip,
    float relative_threshold=0.02F,float minimum_world_threshold=0.10F);

} // namespace noemancer
