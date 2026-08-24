#pragma once

#include "engine/hybrid_pixel_profile.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace noemancer {

// Renderer-neutral policy projection for the post-processing stages.  The
// strategy contains no SDL/RHI handles, frame counters or process state, so
// Editor, Runtime and Agent callers can inspect the same deterministic value.
inline constexpr std::string_view hybrid_pixel_post_process_schema =
    "noemancer.hybrid-pixel-post-process/0.1";

inline constexpr std::uint32_t hybrid_pixel_post_bloom_max_levels = 4U;
inline constexpr float hybrid_pixel_post_bloom_max_threshold = 1.0F;
inline constexpr float hybrid_pixel_post_bloom_max_soft_knee = 0.5F;
inline constexpr float hybrid_pixel_post_bloom_max_strength = 0.35F;
inline constexpr float hybrid_pixel_post_bloom_max_scatter = 0.7F;

struct HybridPixelPostProcessDiagnostic final {
    std::string code;
    std::string path;
    std::string message;
};

struct HybridPixelPostProcessResolution final {
    // "virtual-resolution" is the Hybrid Pixel render target.  Raster uses
    // "physical-render-target" and leaves dimensions to the caller because
    // this pure planner intentionally has no surface or device dependency.
    std::string space{"physical-render-target"};
    std::uint32_t source_width{};
    std::uint32_t source_height{};
    float scale{1.0F};
    std::uint32_t target_width{};
    std::uint32_t target_height{};
};

struct HybridPixelPostProcessTemporal final {
    std::string mode{"TAAU"};
    bool history_enabled{true};
    bool jitter_enabled{true};
    bool reset_history_on_profile_change{true};
    float history_weight{0.90F};
    std::string policy{"raster-temporal-history"};
};

struct HybridPixelPostProcessAutoExposure final {
    bool enabled{true};
    bool locked{};
    std::string mode{"adaptive"};
    float exposure{1.0F};
    float minimum_exposure{0.25F};
    float maximum_exposure{4.0F};
    float key_value{0.18F};
    float speed_up{3.0F};
    float speed_down{1.0F};
    std::string policy{"raster-adaptive"};
};

struct HybridPixelPostProcessBloom final {
    bool enabled{true};
    HybridPixelPostProcessResolution resolution;
    std::uint32_t levels{hybrid_pixel_post_bloom_max_levels};
    float threshold{hybrid_pixel_post_bloom_max_threshold};
    float soft_knee{hybrid_pixel_post_bloom_max_soft_knee};
    float strength{hybrid_pixel_post_bloom_max_strength};
    float scatter{hybrid_pixel_post_bloom_max_scatter};
    std::uint32_t maximum_levels{hybrid_pixel_post_bloom_max_levels};
    float maximum_threshold{hybrid_pixel_post_bloom_max_threshold};
    float maximum_soft_knee{hybrid_pixel_post_bloom_max_soft_knee};
    float maximum_strength{hybrid_pixel_post_bloom_max_strength};
    float maximum_scatter{hybrid_pixel_post_bloom_max_scatter};
    std::string parameter_policy{"raster-defaults"};
};

struct HybridPixelPostProcessAmbientOcclusion final {
    bool enabled{true};
    HybridPixelPostProcessResolution resolution;
    std::string technique{
        "eight-direction-horizon/separable-bilateral/indirect-only"};
    std::string format{"R8_UNORM"};
    std::uint32_t denoise_passes{2U};
    float radius_pixels{14.0F};
    float intensity{1.35F};
    float bias{0.02F};
    float power{1.25F};
    float depth_sigma{1.5F};
    float normal_power{16.0F};
    std::string parameter_policy{"raster-defaults"};
};

struct HybridPixelPostProcessStrategy final {
    std::string schema{std::string(hybrid_pixel_post_process_schema)};
    std::string profile_id;
    bool valid{};
    bool hybrid_pixel_active{};
    std::string code{"raster-defaults"};
    std::string detail;
    HybridPixelPostProcessTemporal temporal;
    HybridPixelPostProcessAutoExposure auto_exposure;
    HybridPixelPostProcessBloom bloom;
    HybridPixelPostProcessAmbientOcclusion ambient_occlusion;
    std::vector<HybridPixelPostProcessDiagnostic> diagnostics;
};

// Purely derives a bounded, plain-data policy.  It does not mutate the
// profile, Render World, renderer state or any temporal/cache resource.
[[nodiscard]] HybridPixelPostProcessStrategy
derive_hybrid_pixel_post_process_strategy(const HybridPixelProfile& profile);

} // namespace noemancer
