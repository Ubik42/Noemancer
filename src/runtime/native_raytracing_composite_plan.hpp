#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace noemancer {

// This plan describes the small bridge between a native RT marker output and
// the existing SDL_GPU post-process chain.  It is deliberately a plan only:
// resource creation, barriers and native handles remain owned by the Runtime.
inline constexpr std::string_view native_raytracing_composite_plan_schema =
    "noemancer.native-raytracing-composite-plan/0.1";
inline constexpr std::string_view native_raytracing_composite_input_format =
    "R32G32B32A32_UINT";
inline constexpr std::string_view native_raytracing_composite_output_format =
    "RGBA8_UNORM";
inline constexpr std::string_view native_raytracing_composite_shader_contract =
    "noemancer.native-rt-composite/0.1";
inline constexpr std::size_t native_raytracing_composite_max_text_bytes = 256U;
inline constexpr std::uint32_t native_raytracing_composite_max_extent = 16384U;

enum class NativeRayTracingCompositeMode : std::uint8_t {
    debug_marker = 0U,
    future_rtgi = 1U,

    DebugMarker = debug_marker,
    FutureRtgi = future_rtgi,
};

[[nodiscard]] std::string_view native_raytracing_composite_mode_name(
    NativeRayTracingCompositeMode mode) noexcept;

// The native D3D12 probe currently publishes a linear UAV buffer.  The
// composite plan intentionally accepts only the same-device Texture2D view
// that the Runtime interop step must create/import from that buffer.  This
// prevents a shader plan from pretending that a structured buffer is directly
// sampleable through the SDL_GPU graphics ABI.
struct NativeRayTracingCompositeInput final {
    std::string resource_id{"rt.native.output.texture"};
    std::string resource_kind{"texture2d"};
    std::string format{std::string(native_raytracing_composite_input_format)};
    std::uint32_t width{};
    std::uint32_t height{};
    std::uint64_t resource_generation{};
    bool producer_complete{};
    bool shader_readable{};
};

// Bounded renderer-private data consumed by the future SceneRenderer wiring.
// `debug_composite` is the only implemented visual path in 0.1.  The plan
// advertises RTGI as a later mode, but never marks it implemented or claims
// that the current marker shader produces indirect lighting.
struct NativeRayTracingCompositePlan final {
    std::string schema{std::string(native_raytracing_composite_plan_schema)};
    NativeRayTracingCompositeMode mode{NativeRayTracingCompositeMode::debug_marker};
    bool valid{};
    bool supported{};
    bool debug_composite{};
    bool future_rtgi_planned{true};
    bool rtgi_implemented{};
    bool claims_rtgi{};
    bool integer_texture_load{};
    bool fallback_active{true};

    std::string code;
    std::string detail;
    std::string visual_path{"raster-pbr-fallback"};
    std::string input_resource_id;
    std::string input_resource_kind;
    std::string input_format{std::string(native_raytracing_composite_input_format)};
    std::string output_format{std::string(native_raytracing_composite_output_format)};
    std::string color_space{"linear-rec709"};
    std::string decode{"uint8-marker-normalized-no-srgb-transfer"};
    std::string read_path{"Texture2D<uint4>.Load"};
    std::string vertex_shader_stem{"native_rt_composite.vert"};
    std::string fragment_shader_stem{"native_rt_composite.frag"};
    std::string shader_contract{std::string(native_raytracing_composite_shader_contract)};
    std::uint32_t width{};
    std::uint32_t height{};
    std::uint64_t resource_generation{};
    std::uint32_t debug_mode{};
};

[[nodiscard]] NativeRayTracingCompositePlan build_native_raytracing_composite_plan(
    const NativeRayTracingCompositeInput& input,
    NativeRayTracingCompositeMode mode = NativeRayTracingCompositeMode::debug_marker);

} // namespace noemancer
