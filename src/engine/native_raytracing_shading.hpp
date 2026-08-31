#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace noemancer {

// Engine-owned input/plan contract for native RT shading.  The scene, mesh
// cache and material registry remain authoritative elsewhere; this contract
// only copies the bounded values a backend needs to prepare a shading view.
// No SDL, D3D12, Vulkan or native resource handles cross this boundary.
inline constexpr std::string_view native_raytracing_shading_schema =
    "noemancer.native-raytracing-shading/0.1";
inline constexpr std::string_view native_raytracing_shading_diagnostic_contract =
    "noemancer.native-raytracing-diagnostic-hit-mask/0.1";
inline constexpr std::string_view native_raytracing_shading_linear_radiance_contract =
    "noemancer.native-raytracing-linear-radiance/0.1";
inline constexpr std::string_view native_raytracing_shading_diagnostic_format =
    "R32G32B32A32_UINT";
inline constexpr std::string_view native_raytracing_shading_linear_radiance_format =
    "RGBA16_FLOAT";

inline constexpr std::size_t native_raytracing_shading_max_text_bytes = 256U;
inline constexpr std::size_t native_raytracing_shading_max_instances = 16384U;
inline constexpr std::size_t native_raytracing_shading_max_primitives = 65536U;
inline constexpr std::size_t native_raytracing_shading_max_materials = 65536U;
inline constexpr std::size_t native_raytracing_shading_max_primitives_per_instance = 4096U;
inline constexpr float native_raytracing_shading_max_color_component = 10000.0F;
inline constexpr float native_raytracing_shading_max_emissive_intensity = 1000000.0F;
inline constexpr float native_raytracing_shading_max_light_intensity = 1000000.0F;
inline constexpr float native_raytracing_shading_max_normal_strength = 8.0F;

enum class NativeRayTracingShadingOutputMode : std::uint8_t {
    diagnostic_hit_mask = 0U,
    future_linear_radiance = 1U,

    DiagnosticHitMask = diagnostic_hit_mask,
    FutureLinearRadiance = future_linear_radiance,
};

[[nodiscard]] std::string_view native_raytracing_shading_output_mode_name(
    NativeRayTracingShadingOutputMode mode) noexcept;

using NativeRayTracingShadingVec3 = std::array<float, 3>;
using NativeRayTracingShadingColor = std::array<float, 4>;

// The first RT shading path is opaque metal/rough PBR input preparation.  The
// texture booleans describe binding intent; they are not GPU descriptors and
// the actual texture authorities stay in the asset/runtime systems.
struct NativeRayTracingShadingMaterial final {
    std::string material_id;
    NativeRayTracingShadingColor base_color{1.0F, 1.0F, 1.0F, 1.0F};
    float metallic{};
    float roughness{0.5F};
    NativeRayTracingShadingVec3 emissive_color{};
    float emissive_intensity{};
    bool has_base_color_texture{};
    bool has_metallic_roughness_texture{};
    bool has_emissive_texture{};
    bool has_normal_texture{};
    bool normal_map_enabled{};
    float normal_strength{1.0F};
    bool double_sided{};
    bool receives_shadows{true};
};

struct NativeRayTracingShadingPrimitive final {
    std::string primitive_id;
    NativeRayTracingShadingMaterial material;
    bool enabled{true};
};

struct NativeRayTracingShadingInstance final {
    std::string instance_id;
    std::string geometry_id;
    std::vector<NativeRayTracingShadingPrimitive> primitives;
    bool enabled{true};
};

struct NativeRayTracingShadingDirectionalLight final {
    // Direction from a shaded point toward the light.  The plan normalizes it
    // so CPU evidence and a GPU adapter share one convention.
    NativeRayTracingShadingVec3 direction{-0.55F, -1.0F, -0.35F};
    NativeRayTracingShadingVec3 color{1.0F, 0.96F, 0.88F};
    float intensity{1.0F};
    bool enabled{true};
};

struct NativeRayTracingShadingEnvironment final {
    NativeRayTracingShadingVec3 color{0.18F, 0.20F, 0.24F};
    float intensity{0.18F};
    bool enabled{true};
};

// One deterministic record per accepted instance/primitive pair.  The
// flattened index is assigned after sorting by stable instance id and then
// primitive id, so a backend can use a ray-triangle PrimitiveIndex() without
// guessing how nested scene data maps to its material buffer.
struct NativeRayTracingShadingMaterialBinding final {
    std::string instance_id;
    std::string primitive_id;
    std::string material_id;
    std::uint32_t material_index{};
    NativeRayTracingShadingMaterial material;
};

struct NativeRayTracingShadingInput final {
    std::string scene_id;
    std::uint64_t scene_revision{};
    std::vector<NativeRayTracingShadingInstance> instances;
    NativeRayTracingShadingDirectionalLight directional_light;
    NativeRayTracingShadingEnvironment environment;
};

struct NativeRayTracingShadingPlan final {
    std::string schema{std::string(native_raytracing_shading_schema)};
    NativeRayTracingShadingOutputMode output_mode{
        NativeRayTracingShadingOutputMode::diagnostic_hit_mask};
    bool valid{};
    bool supported{};
    bool diagnostic_hit_mask{};
    bool future_linear_radiance_planned{true};
    bool linear_radiance_implemented{};
    bool claims_linear_radiance{};
    bool fallback_active{true};

    // These flags report that the plain-data inputs passed validation.  They
    // do not claim that a native shader has evaluated direct or indirect light.
    bool pbr_inputs_valid{};
    bool directional_light_described{};
    bool environment_described{};
    bool normal_flags_described{};

    std::string code;
    std::string detail;
    std::string scene_id;
    std::uint64_t scene_revision{};
    std::string output_contract{
        std::string(native_raytracing_shading_diagnostic_contract)};
    std::string output_format{std::string(native_raytracing_shading_diagnostic_format)};
    std::string output_semantic{"diagnostic-hit-mask"};
    std::string color_space{"linear-rec709"};
    std::string shading_model{"pbr-metal-rough-input-contract"};
    std::string contract_role{"input-and-plan-only"};

    NativeRayTracingShadingDirectionalLight directional_light;
    NativeRayTracingShadingEnvironment environment;
    std::vector<NativeRayTracingShadingInstance> instances;
    std::uint32_t input_instance_count{};
    std::uint32_t input_primitive_count{};
    std::uint32_t input_material_count{};
    std::uint32_t accepted_instance_count{};
    std::uint32_t accepted_primitive_count{};
    std::uint32_t accepted_material_count{};
    std::uint32_t excluded_disabled_instance_count{};
    std::uint32_t excluded_disabled_primitive_count{};
    std::vector<NativeRayTracingShadingMaterialBinding> flattened_bindings;
    std::uint32_t flattened_binding_count{};
    std::uint64_t shading_fingerprint{};
};

[[nodiscard]] NativeRayTracingShadingPlan build_native_raytracing_shading_plan(
    const NativeRayTracingShadingInput& input,
    NativeRayTracingShadingOutputMode output_mode =
        NativeRayTracingShadingOutputMode::diagnostic_hit_mask);

} // namespace noemancer
