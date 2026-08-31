#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace noemancer {

// Renderer-neutral description of the camera view consumed by a native
// ray-tracing adapter.  RenderWorld owns the camera; this contract only
// carries the bounded values needed to derive primary rays.  It deliberately
// contains no SDL, D3D12, Vulkan, descriptor or resource types.
inline constexpr std::string_view native_raytracing_view_schema =
    "noemancer.native-raytracing-view/0.1";
inline constexpr std::string_view native_raytracing_diagnostic_hit_mask_contract =
    "noemancer.native-raytracing-diagnostic-hit-mask/0.1";
inline constexpr std::string_view native_raytracing_future_linear_radiance_contract =
    "noemancer.native-raytracing-linear-radiance/0.1";
inline constexpr std::string_view native_raytracing_diagnostic_output_format =
    "R32G32B32A32_UINT";

inline constexpr std::size_t native_raytracing_view_max_text_bytes = 256U;
inline constexpr std::uint32_t native_raytracing_view_max_extent = 16384U;
inline constexpr float native_raytracing_view_max_world_coordinate = 1.0e9F;
inline constexpr float native_raytracing_view_max_clip_distance = 1.0e9F;
inline constexpr float native_raytracing_view_max_aspect = 1000.0F;

enum class NativeRayTracingViewOutputMode : std::uint8_t {
    diagnostic_hit_mask = 0U,
    future_linear_radiance = 1U,

    DiagnosticHitMask = diagnostic_hit_mask,
    FutureLinearRadiance = future_linear_radiance,
};

[[nodiscard]] std::string_view native_raytracing_view_output_mode_name(
    NativeRayTracingViewOutputMode mode) noexcept;

using NativeRayTracingViewVec3 = std::array<float, 3>;

struct NativeRayTracingViewInput final {
    // `camera_id` is the stable RenderWorld identity; `camera_revision` is
    // the source/world revision at which these values were observed.
    std::string camera_id;
    std::uint64_t camera_revision{};
    NativeRayTracingViewVec3 position{};
    NativeRayTracingViewVec3 forward{0.0F, 0.0F, 1.0F};
    NativeRayTracingViewVec3 up{0.0F, 1.0F, 0.0F};
    // RenderWorld currently stores this vocabulary as a lower-case string;
    // keeping it as plain text avoids a second camera authority and makes an
    // input aggregate directly constructible from the snapshot.
    std::string projection{"perspective"};
    float vertical_fov_degrees{45.0F};
    float orthographic_height{10.0F};
    float aspect{16.0F / 9.0F};
    float near_clip{0.1F};
    float far_clip{100.0F};
    std::uint32_t output_width{};
    std::uint32_t output_height{};
};

struct NativeRayTracingViewBasis final {
    NativeRayTracingViewVec3 position{};
    NativeRayTracingViewVec3 forward{};
    NativeRayTracingViewVec3 right{};
    NativeRayTracingViewVec3 up{};
};

// Projection constants shared by every pixel in a plan.  Perspective values
// are tangents of the half field-of-view; orthographic values are half sizes.
// Keeping these explicit lets a GPU adapter reproduce the CPU ray convention
// without copying a matrix or relying on backend-specific clip-space rules.
struct NativeRayTracingPrimaryRayParameters final {
    float tan_half_fov_y{};
    float tan_half_fov_x{};
    float orthographic_half_height{};
    float orthographic_half_width{};
};

struct NativeRayTracingViewPlan final {
    std::string schema{std::string(native_raytracing_view_schema)};
    NativeRayTracingViewOutputMode output_mode{
        NativeRayTracingViewOutputMode::diagnostic_hit_mask};
    bool valid{};
    bool supported{};
    bool diagnostic_hit_mask{};
    bool future_linear_radiance_planned{true};
    bool linear_radiance_implemented{};
    bool claims_linear_radiance{};
    bool basis_orthonormal{};
    bool fallback_active{true};

    std::string code;
    std::string detail;
    std::string camera_id;
    std::uint64_t camera_revision{};
    std::string projection;
    std::string output_contract{
        std::string(native_raytracing_diagnostic_hit_mask_contract)};
    std::string output_format{std::string(native_raytracing_diagnostic_output_format)};
    std::string output_semantic{"diagnostic-hit-mask"};
    std::string color_space{"linear-rec709"};
    std::string ray_convention{"pixel-center/top-left-y/orthonormal-basis"};

    NativeRayTracingViewBasis basis{};
    NativeRayTracingPrimaryRayParameters primary_ray_parameters{};
    float vertical_fov_degrees{};
    float orthographic_height{};
    float aspect{};
    float near_clip{};
    float far_clip{};
    std::uint32_t output_width{};
    std::uint32_t output_height{};

    // FNV-1a over the canonical camera, basis, extent and projection values.
    // It identifies the exact ray recipe, not a native resource or a pixel
    // buffer.  Per-pixel results derive their own fingerprint from this value.
    std::uint64_t primary_ray_fingerprint{};
};

struct NativeRayTracingPrimaryRay final {
    std::uint32_t pixel_x{};
    std::uint32_t pixel_y{};
    float sample_u{};
    float sample_v{};
    NativeRayTracingViewVec3 origin{};
    NativeRayTracingViewVec3 direction{};
    float minimum_distance{};
    float maximum_distance{};
};

struct NativeRayTracingPrimaryRayResult final {
    bool valid{};
    std::string code;
    std::string detail;
    NativeRayTracingPrimaryRay ray{};
    std::uint64_t fingerprint{};
};

[[nodiscard]] NativeRayTracingViewPlan build_native_raytracing_view_plan(
    const NativeRayTracingViewInput& input,
    NativeRayTracingViewOutputMode output_mode =
        NativeRayTracingViewOutputMode::diagnostic_hit_mask);

// The calculation is intentionally CPU-side and backend independent.  A
// native adapter may mirror the same formula in a shader, while tests and
// Agent evidence can inspect one bounded pixel without allocating a frame.
[[nodiscard]] NativeRayTracingPrimaryRayResult make_native_raytracing_primary_ray(
    const NativeRayTracingViewPlan& plan,
    std::uint32_t pixel_x,
    std::uint32_t pixel_y);

} // namespace noemancer
