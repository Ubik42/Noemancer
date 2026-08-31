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
// SDL_GPU's exported texture is intentionally kept UINT for the current
// same-device copy contract.  The native full-frame DXR producer writes
// IEEE-754 float bits into that UINT footprint; the composite shader therefore
// performs an explicit asfloat decode rather than dividing those bits by 255.
inline constexpr std::string_view native_raytracing_composite_marker_contract =
    "noemancer.native-rt-marker-probe/0.1";
inline constexpr std::string_view native_raytracing_composite_radiance_contract =
    "noemancer.native-rt-full-frame/0.3";
inline constexpr std::string_view native_raytracing_composite_output_format =
    "RGBA8_UNORM";
inline constexpr std::string_view native_raytracing_composite_shader_contract =
    "noemancer.native-rt-composite/0.2";
inline constexpr std::string_view native_raytracing_composite_stage =
    "after-tone-map-before-present";
inline constexpr std::size_t native_raytracing_composite_max_text_bytes = 256U;
inline constexpr std::uint32_t native_raytracing_composite_max_extent = 16384U;

enum class NativeRayTracingCompositeMode : std::uint8_t {
    debug_marker = 0U,
    future_rtgi = 1U,
    linear_radiance = 2U,

    DebugMarker = debug_marker,
    FutureRtgi = future_rtgi,
    LinearRadiance = linear_radiance,
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
    // The physical SDL texture remains UINT, so the producer contract is the
    // versioned discriminator between the legacy marker bytes and the 0.3
    // scene-linear float bit pattern.  Omitting it preserves marker behavior.
    std::string producer_shader_contract{
        std::string(native_raytracing_composite_marker_contract)};
    std::uint32_t width{};
    std::uint32_t height{};
    std::uint64_t resource_generation{};
    bool producer_complete{};
    bool shader_readable{};
};

// Bounded renderer-private data consumed by the future SceneRenderer wiring.
// The marker and direct+ambient radiance composites are separate producer
// contracts.  The latter is still only a diagnostic presentation path: it
// does not mark RTGI implemented or claim that the native shader produced
// indirect lighting.
struct NativeRayTracingCompositePlan final {
    std::string schema{std::string(native_raytracing_composite_plan_schema)};
    NativeRayTracingCompositeMode mode{NativeRayTracingCompositeMode::debug_marker};
    bool valid{};
    bool supported{};
    bool debug_composite{};
    bool linear_radiance_composite{};
    bool future_rtgi_planned{true};
    bool rtgi_implemented{};
    bool claims_rtgi{};
    bool integer_texture_load{};
    bool float_bit_pattern_load{};
    bool tone_mapping_precedes_composite{true};
    bool fallback_active{true};

    std::string code;
    std::string detail;
    std::string visual_path{"raster-pbr-fallback"};
    std::string input_resource_id;
    std::string input_resource_kind;
    std::string producer_shader_contract;
    std::string input_format{std::string(native_raytracing_composite_input_format)};
    std::string output_format{std::string(native_raytracing_composite_output_format)};
    std::string color_space{"linear-rec709"};
    std::string decode{"uint8-marker-normalized-no-srgb-transfer"};
    std::string read_path{"Texture2D<uint4>.Load"};
    std::string stage{std::string(native_raytracing_composite_stage)};
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
