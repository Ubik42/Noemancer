#include "engine/hybrid_pixel_post.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace noemancer {
namespace {

constexpr float raster_history_weight = 0.90F;
constexpr float raster_exposure = 1.0F;
constexpr float raster_minimum_exposure = 0.25F;
constexpr float raster_maximum_exposure = 4.0F;
constexpr float raster_key_value = 0.18F;
constexpr float raster_speed_up = 3.0F;
constexpr float raster_speed_down = 1.0F;
constexpr float raster_ao_scale = 0.5F;
constexpr float raster_ao_radius_pixels = 14.0F;
constexpr float raster_ao_intensity = 1.35F;
constexpr float raster_ao_bias = 0.02F;
constexpr float raster_ao_power = 1.25F;
constexpr float raster_ao_depth_sigma = 1.5F;
constexpr float raster_ao_normal_power = 16.0F;

std::uint32_t scaled_extent(const std::uint32_t extent, const float scale) {
    const auto result = static_cast<double>(extent) * static_cast<double>(scale);
    if (!std::isfinite(result) || result <= 0.0) return 0U;
    const auto rounded = std::ceil(result);
    if (rounded >= static_cast<double>(std::numeric_limits<std::uint32_t>::max())) {
        return std::numeric_limits<std::uint32_t>::max();
    }
    return static_cast<std::uint32_t>(rounded);
}

HybridPixelPostProcessResolution physical_resolution(const float scale) {
    HybridPixelPostProcessResolution result;
    result.space = "physical-render-target";
    result.scale = scale;
    // The planner has no physical surface dimensions by design.  A Runtime
    // caller can use the declared scale with its own target extent.
    return result;
}

HybridPixelPostProcessResolution virtual_resolution(
    const HybridPixelProfile& profile, const float scale) {
    HybridPixelPostProcessResolution result;
    result.space = "virtual-resolution";
    result.source_width = profile.virtual_width;
    result.source_height = profile.virtual_height;
    result.scale = scale;
    result.target_width = scaled_extent(profile.virtual_width, scale);
    result.target_height = scaled_extent(profile.virtual_height, scale);
    return result;
}

HybridPixelPostProcessStrategy raster_strategy(const HybridPixelProfile& profile) {
    HybridPixelPostProcessStrategy result;
    result.profile_id = profile.profile_id;
    result.valid = true;
    result.hybrid_pixel_active = false;
    result.code = "raster-defaults";
    result.detail = "The ordinary Raster post-processing strategy is active.";

    result.temporal.mode = "TAAU";
    result.temporal.history_enabled = true;
    result.temporal.jitter_enabled = true;
    result.temporal.reset_history_on_profile_change = true;
    result.temporal.history_weight = raster_history_weight;
    result.temporal.policy = "raster-temporal-history";

    result.auto_exposure.enabled = true;
    result.auto_exposure.locked = false;
    result.auto_exposure.mode = "adaptive";
    result.auto_exposure.exposure = raster_exposure;
    result.auto_exposure.minimum_exposure = raster_minimum_exposure;
    result.auto_exposure.maximum_exposure = raster_maximum_exposure;
    result.auto_exposure.key_value = raster_key_value;
    result.auto_exposure.speed_up = raster_speed_up;
    result.auto_exposure.speed_down = raster_speed_down;
    result.auto_exposure.policy = "raster-adaptive";

    result.bloom.enabled = true;
    result.bloom.resolution = physical_resolution(1.0F);
    result.bloom.levels = hybrid_pixel_post_bloom_max_levels;
    result.bloom.threshold = hybrid_pixel_post_bloom_max_threshold;
    result.bloom.soft_knee = hybrid_pixel_post_bloom_max_soft_knee;
    result.bloom.strength = hybrid_pixel_post_bloom_max_strength;
    result.bloom.scatter = hybrid_pixel_post_bloom_max_scatter;
    result.bloom.maximum_levels = hybrid_pixel_post_bloom_max_levels;
    result.bloom.maximum_threshold = hybrid_pixel_post_bloom_max_threshold;
    result.bloom.maximum_soft_knee = hybrid_pixel_post_bloom_max_soft_knee;
    result.bloom.maximum_strength = hybrid_pixel_post_bloom_max_strength;
    result.bloom.maximum_scatter = hybrid_pixel_post_bloom_max_scatter;
    result.bloom.parameter_policy = "raster-defaults";

    result.ambient_occlusion.enabled = true;
    result.ambient_occlusion.resolution = physical_resolution(raster_ao_scale);
    result.ambient_occlusion.technique =
        "eight-direction-horizon/separable-bilateral/indirect-only";
    result.ambient_occlusion.format = "R8_UNORM";
    result.ambient_occlusion.denoise_passes = 2U;
    result.ambient_occlusion.radius_pixels = raster_ao_radius_pixels;
    result.ambient_occlusion.intensity = raster_ao_intensity;
    result.ambient_occlusion.bias = raster_ao_bias;
    result.ambient_occlusion.power = raster_ao_power;
    result.ambient_occlusion.depth_sigma = raster_ao_depth_sigma;
    result.ambient_occlusion.normal_power = raster_ao_normal_power;
    result.ambient_occlusion.parameter_policy = "raster-defaults";
    return result;
}

} // namespace

HybridPixelPostProcessStrategy
derive_hybrid_pixel_post_process_strategy(const HybridPixelProfile& profile) {
    auto result = raster_strategy(profile);
    if (!profile.enabled) return result;

    const auto errors = HybridPixelProfileCodec::validate(profile);
    if (!errors.empty()) {
        result.hybrid_pixel_active = true;
        result.valid = false;
        result.code = "hybrid-pixel-post.profile-invalid";
        result.detail =
            "The active Hybrid Pixel profile cannot produce a post-processing strategy.";
        result.diagnostics.reserve(errors.size());
        for (const auto& error : errors) {
            result.diagnostics.push_back({error.code, error.path, error.message});
        }
        return result;
    }

    result.hybrid_pixel_active = true;
    result.valid = true;
    result.code = "hybrid-pixel-post.hybrid-active";
    result.detail =
        "Hybrid Pixel post-processing uses stable virtual-resolution policies.";

    result.temporal.mode = "spatial-pixel-stable";
    result.temporal.history_enabled = false;
    result.temporal.jitter_enabled = false;
    result.temporal.reset_history_on_profile_change = true;
    result.temporal.history_weight = 0.0F;
    result.temporal.policy = "hybrid-pixel-history-disabled";

    // Keep the existing exposure pass in the graph, but make its target and
    // adaptation speeds identical so it is observably locked/stable rather
    // than silently removing a post-process stage.
    result.auto_exposure.enabled = true;
    result.auto_exposure.locked = true;
    result.auto_exposure.mode = "locked-stable";
    result.auto_exposure.exposure = raster_exposure;
    result.auto_exposure.minimum_exposure = raster_exposure;
    result.auto_exposure.maximum_exposure = raster_exposure;
    result.auto_exposure.key_value = raster_key_value;
    result.auto_exposure.speed_up = 0.0F;
    result.auto_exposure.speed_down = 0.0F;
    result.auto_exposure.policy = "hybrid-pixel-locked-exposure";

    result.bloom.enabled = true;
    result.bloom.resolution = virtual_resolution(profile, 1.0F);
    result.bloom.levels = hybrid_pixel_post_bloom_max_levels;
    result.bloom.threshold = std::min(1.0F, hybrid_pixel_post_bloom_max_threshold);
    result.bloom.soft_knee = std::min(0.5F, hybrid_pixel_post_bloom_max_soft_knee);
    result.bloom.strength = std::min(0.35F, hybrid_pixel_post_bloom_max_strength);
    result.bloom.scatter = std::min(0.7F, hybrid_pixel_post_bloom_max_scatter);
    result.bloom.maximum_levels = hybrid_pixel_post_bloom_max_levels;
    result.bloom.maximum_threshold = hybrid_pixel_post_bloom_max_threshold;
    result.bloom.maximum_soft_knee = hybrid_pixel_post_bloom_max_soft_knee;
    result.bloom.maximum_strength = hybrid_pixel_post_bloom_max_strength;
    result.bloom.maximum_scatter = hybrid_pixel_post_bloom_max_scatter;
    result.bloom.parameter_policy = "hybrid-pixel-fixed-upper-bounds";

    result.ambient_occlusion.enabled = true;
    result.ambient_occlusion.resolution = virtual_resolution(profile, raster_ao_scale);
    result.ambient_occlusion.parameter_policy = "hybrid-pixel-virtual-resolution";
    return result;
}

} // namespace noemancer
