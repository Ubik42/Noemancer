#include "engine/hybrid_pixel_post.hpp"

#include <cmath>
#include <iostream>
#include <string_view>

namespace {

int fail(const char* detail) {
    std::cerr << "hybrid_pixel_post_tests: " << detail << '\n';
    return 1;
}

bool near(const float left, const float right) {
    return std::abs(left - right) <= 1.0e-6F;
}

bool same_resolution(const noemancer::HybridPixelPostProcessResolution& left,
                     const noemancer::HybridPixelPostProcessResolution& right) {
    return left.space == right.space && left.source_width == right.source_width &&
           left.source_height == right.source_height && near(left.scale, right.scale) &&
           left.target_width == right.target_width && left.target_height == right.target_height;
}

bool same_strategy(const noemancer::HybridPixelPostProcessStrategy& left,
                   const noemancer::HybridPixelPostProcessStrategy& right) {
    return left.schema == right.schema && left.profile_id == right.profile_id &&
           left.valid == right.valid && left.hybrid_pixel_active == right.hybrid_pixel_active &&
           left.code == right.code && left.detail == right.detail &&
           left.temporal.mode == right.temporal.mode &&
           left.temporal.history_enabled == right.temporal.history_enabled &&
           left.temporal.jitter_enabled == right.temporal.jitter_enabled &&
           left.temporal.reset_history_on_profile_change ==
               right.temporal.reset_history_on_profile_change &&
           near(left.temporal.history_weight, right.temporal.history_weight) &&
           left.temporal.policy == right.temporal.policy &&
           left.auto_exposure.enabled == right.auto_exposure.enabled &&
           left.auto_exposure.locked == right.auto_exposure.locked &&
           left.auto_exposure.mode == right.auto_exposure.mode &&
           near(left.auto_exposure.exposure, right.auto_exposure.exposure) &&
           near(left.auto_exposure.minimum_exposure, right.auto_exposure.minimum_exposure) &&
           near(left.auto_exposure.maximum_exposure, right.auto_exposure.maximum_exposure) &&
           near(left.auto_exposure.key_value, right.auto_exposure.key_value) &&
           near(left.auto_exposure.speed_up, right.auto_exposure.speed_up) &&
           near(left.auto_exposure.speed_down, right.auto_exposure.speed_down) &&
           left.auto_exposure.policy == right.auto_exposure.policy &&
           left.bloom.enabled == right.bloom.enabled &&
           same_resolution(left.bloom.resolution, right.bloom.resolution) &&
           left.bloom.levels == right.bloom.levels && near(left.bloom.threshold, right.bloom.threshold) &&
           near(left.bloom.soft_knee, right.bloom.soft_knee) &&
           near(left.bloom.strength, right.bloom.strength) &&
           near(left.bloom.scatter, right.bloom.scatter) &&
           left.bloom.maximum_levels == right.bloom.maximum_levels &&
           near(left.bloom.maximum_threshold, right.bloom.maximum_threshold) &&
           near(left.bloom.maximum_soft_knee, right.bloom.maximum_soft_knee) &&
           near(left.bloom.maximum_strength, right.bloom.maximum_strength) &&
           near(left.bloom.maximum_scatter, right.bloom.maximum_scatter) &&
           left.bloom.parameter_policy == right.bloom.parameter_policy &&
           left.ambient_occlusion.enabled == right.ambient_occlusion.enabled &&
           same_resolution(left.ambient_occlusion.resolution, right.ambient_occlusion.resolution) &&
           left.ambient_occlusion.technique == right.ambient_occlusion.technique &&
           left.ambient_occlusion.format == right.ambient_occlusion.format &&
           left.ambient_occlusion.denoise_passes == right.ambient_occlusion.denoise_passes &&
           near(left.ambient_occlusion.radius_pixels, right.ambient_occlusion.radius_pixels) &&
           near(left.ambient_occlusion.intensity, right.ambient_occlusion.intensity) &&
           near(left.ambient_occlusion.bias, right.ambient_occlusion.bias) &&
           near(left.ambient_occlusion.power, right.ambient_occlusion.power) &&
           near(left.ambient_occlusion.depth_sigma, right.ambient_occlusion.depth_sigma) &&
           near(left.ambient_occlusion.normal_power, right.ambient_occlusion.normal_power) &&
           left.ambient_occlusion.parameter_policy == right.ambient_occlusion.parameter_policy &&
           left.diagnostics.size() == right.diagnostics.size();
}

bool has_diagnostic(const noemancer::HybridPixelPostProcessStrategy& strategy,
                    const std::string_view code, const std::string_view path) {
    for (const auto& diagnostic : strategy.diagnostics) {
        if (diagnostic.code == code && diagnostic.path == path && !diagnostic.message.empty()) {
            return true;
        }
    }
    return false;
}

} // namespace

int main() {
    using noemancer::HybridPixelProfile;
    using noemancer::HybridPixelPostProcessStrategy;
    using noemancer::derive_hybrid_pixel_post_process_strategy;

    HybridPixelProfile hybrid;
    hybrid.profile_id = "post.acceptance";
    hybrid.virtual_width = 320U;
    hybrid.virtual_height = 180U;
    hybrid.pixels_per_unit = 16.0F;
    const auto first = derive_hybrid_pixel_post_process_strategy(hybrid);
    const auto second = derive_hybrid_pixel_post_process_strategy(hybrid);
    if (!first.valid || !first.hybrid_pixel_active || first.code != "hybrid-pixel-post.hybrid-active" ||
        !same_strategy(first, second) || !first.diagnostics.empty()) {
        return fail("valid Hybrid Pixel strategy was not deterministic");
    }
    if (first.temporal.history_enabled || first.temporal.jitter_enabled ||
        !near(first.temporal.history_weight, 0.0F) ||
        first.temporal.mode != "spatial-pixel-stable" ||
        !first.auto_exposure.enabled || !first.auto_exposure.locked ||
        first.auto_exposure.mode != "locked-stable" ||
        !near(first.auto_exposure.minimum_exposure, 1.0F) ||
        !near(first.auto_exposure.maximum_exposure, 1.0F) ||
        !near(first.auto_exposure.speed_up, 0.0F) ||
        !near(first.auto_exposure.speed_down, 0.0F)) {
        return fail("Hybrid Pixel temporal or exposure policy is not locked");
    }
    if (first.bloom.resolution.space != "virtual-resolution" ||
        first.bloom.resolution.source_width != 320U ||
        first.bloom.resolution.source_height != 180U ||
        first.bloom.resolution.target_width != 320U ||
        first.bloom.resolution.target_height != 180U ||
        first.bloom.levels != noemancer::hybrid_pixel_post_bloom_max_levels ||
        first.bloom.threshold > first.bloom.maximum_threshold ||
        first.bloom.soft_knee > first.bloom.maximum_soft_knee ||
        first.bloom.strength > first.bloom.maximum_strength ||
        first.bloom.scatter > first.bloom.maximum_scatter) {
        return fail("Hybrid Pixel Bloom did not use bounded virtual-resolution policy");
    }
    if (first.ambient_occlusion.resolution.space != "virtual-resolution" ||
        first.ambient_occlusion.resolution.source_width != 320U ||
        first.ambient_occlusion.resolution.source_height != 180U ||
        first.ambient_occlusion.resolution.target_width != 160U ||
        first.ambient_occlusion.resolution.target_height != 90U ||
        first.ambient_occlusion.format != "R8_UNORM" ||
        first.ambient_occlusion.denoise_passes != 2U) {
        return fail("Hybrid Pixel AO did not use virtual-resolution policy");
    }

    auto raster = hybrid;
    raster.enabled = false;
    const auto raster_strategy = derive_hybrid_pixel_post_process_strategy(raster);
    if (!raster_strategy.valid || raster_strategy.hybrid_pixel_active ||
        raster_strategy.code != "raster-defaults" ||
        !raster_strategy.temporal.history_enabled ||
        !raster_strategy.temporal.jitter_enabled ||
        !near(raster_strategy.temporal.history_weight, 0.90F) ||
        raster_strategy.auto_exposure.locked || !raster_strategy.auto_exposure.enabled ||
        raster_strategy.bloom.resolution.space != "physical-render-target" ||
        raster_strategy.ambient_occlusion.resolution.space != "physical-render-target") {
        return fail("disabled profile did not return ordinary Raster strategy");
    }

    auto invalid = hybrid;
    invalid.virtual_width = 0U;
    invalid.presentation_filter = "linear";
    const auto rejected = derive_hybrid_pixel_post_process_strategy(invalid);
    if (rejected.valid || !rejected.hybrid_pixel_active ||
        rejected.code != "hybrid-pixel-post.profile-invalid" ||
        !has_diagnostic(rejected, "hybrid-pixel.dimension-range", "/virtualWidth") ||
        !has_diagnostic(rejected, "hybrid-pixel.invalid-filter", "/presentationFilter")) {
        return fail("invalid profile did not return bounded plain-data diagnostics");
    }

    auto large = hybrid;
    large.virtual_width = 8192U;
    large.virtual_height = 8192U;
    const auto large_strategy = derive_hybrid_pixel_post_process_strategy(large);
    if (!large_strategy.valid || large_strategy.bloom.resolution.target_width != 8192U ||
        large_strategy.ambient_occlusion.resolution.target_width != 4096U ||
        large_strategy.ambient_occlusion.resolution.target_height != 4096U) {
        return fail("large valid profile produced unstable bounded extents");
    }

    std::cout << "hybrid_pixel_post_tests: ok\n";
    return 0;
}
