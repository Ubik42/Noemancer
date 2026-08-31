#include "engine/native_raytracing_shading.hpp"

#include <cmath>
#include <iostream>
#include <limits>
#include <string_view>

namespace {

using namespace noemancer;

bool check(const bool condition, const std::string_view message) {
    if (!condition) {
        std::cerr << "native_raytracing_shading_tests: " << message << '\n';
    }
    return condition;
}

bool near(const float left, const float right, const float tolerance = 1.0e-5F) {
    return std::abs(left - right) <= tolerance;
}

NativeRayTracingShadingMaterial material(const std::string_view id,
                                         const NativeRayTracingShadingColor color,
                                         const float metallic,
                                         const float roughness) {
    NativeRayTracingShadingMaterial result;
    result.material_id = std::string(id);
    result.base_color = color;
    result.metallic = metallic;
    result.roughness = roughness;
    result.emissive_color = {0.02F, 0.04F, 0.08F};
    result.emissive_intensity = 0.4F;
    result.has_base_color_texture = true;
    result.has_metallic_roughness_texture = metallic > 0.0F;
    result.has_emissive_texture = true;
    result.has_normal_texture = true;
    result.normal_map_enabled = true;
    result.normal_strength = 0.75F;
    result.double_sided = false;
    result.receives_shadows = true;
    return result;
}

NativeRayTracingShadingPrimitive primitive(
    const std::string_view id,
    const NativeRayTracingShadingMaterial& material_value) {
    NativeRayTracingShadingPrimitive result;
    result.primitive_id = std::string(id);
    result.material = material_value;
    return result;
}

NativeRayTracingShadingInput valid_input() {
    const auto stone = material("material.stone", {0.62F, 0.66F, 0.72F, 1.0F},
                               0.75F, 0.32F);
    const auto red = material("material.red", {0.78F, 0.08F, 0.04F, 1.0F},
                              0.12F, 0.46F);
    const auto unused = material("material.unused", {0.2F, 0.2F, 0.2F, 1.0F},
                                 0.0F, 0.8F);

    NativeRayTracingShadingInput result;
    result.scene_id = "scene.rt.material-lab";
    result.scene_revision = 31U;
    result.directional_light.direction = {0.0F, -2.0F, 0.0F};
    result.directional_light.color = {1.0F, 0.92F, 0.80F};
    result.directional_light.intensity = 3.0F;
    result.environment.color = {0.10F, 0.14F, 0.20F};
    result.environment.intensity = 0.28F;
    result.instances = {
        {"instance.zeta", "geometry.shared", {
             primitive("surface.b", stone), primitive("surface.a", stone)}},
        {"instance.alpha", "geometry.shared", {primitive("surface.main", red)}},
        {"instance.disabled", "geometry.hidden", {primitive("surface.hidden", unused)}, false},
    };
    return result;
}

} // namespace

int main() {
    const auto input = valid_input();
    const auto plan = build_native_raytracing_shading_plan(input);
    if (!check(plan.valid && plan.supported && plan.diagnostic_hit_mask &&
                   !plan.fallback_active && plan.pbr_inputs_valid &&
                   plan.directional_light_described && plan.environment_described &&
                   plan.normal_flags_described && plan.future_linear_radiance_planned &&
                   !plan.linear_radiance_implemented && !plan.claims_linear_radiance &&
                   plan.output_contract == native_raytracing_shading_diagnostic_contract &&
                   plan.output_format == native_raytracing_shading_diagnostic_format &&
                   plan.output_semantic == "diagnostic-hit-mask" &&
                   plan.contract_role == "input-and-plan-only" &&
                   plan.input_instance_count == 3U && plan.input_primitive_count == 4U &&
                   plan.input_material_count == 3U && plan.accepted_instance_count == 2U &&
                   plan.accepted_primitive_count == 3U && plan.accepted_material_count == 2U &&
                   plan.excluded_disabled_instance_count == 1U &&
                   plan.excluded_disabled_primitive_count == 0U &&
                   plan.flattened_binding_count == 3U &&
                   plan.flattened_bindings.size() == 3U && plan.shading_fingerprint != 0U,
               "valid PBR scene did not produce the bounded diagnostic shading plan")) {
        return 1;
    }
    if (!check(plan.instances.size() == 2U &&
                   plan.instances[0].instance_id == "instance.alpha" &&
                   plan.instances[1].instance_id == "instance.zeta" &&
                   plan.instances[1].primitives[0].primitive_id == "surface.a" &&
                   plan.instances[1].primitives[1].primitive_id == "surface.b" &&
                   plan.flattened_bindings[0].instance_id == "instance.alpha" &&
                   plan.flattened_bindings[0].primitive_id == "surface.main" &&
                   plan.flattened_bindings[0].material_id == "material.red" &&
                   plan.flattened_bindings[0].material_index == 0U &&
                   plan.flattened_bindings[1].material_id == "material.stone" &&
                   plan.flattened_bindings[1].material_index == 1U &&
                   plan.flattened_bindings[2].material_index == 2U,
               "flattened material bindings were not sorted by stable instance/primitive identity")) {
        return 2;
    }
    if (!check(near(plan.directional_light.direction[0], 0.0F) &&
                   near(plan.directional_light.direction[1], -1.0F) &&
                   near(plan.directional_light.direction[2], 0.0F) &&
                   plan.directional_light.intensity == input.directional_light.intensity &&
                   plan.environment.color == input.environment.color &&
                   plan.environment.intensity == input.environment.intensity,
               "directional light normalization or environment values drifted")) {
        return 3;
    }

    auto reordered = input;
    std::swap(reordered.instances[0], reordered.instances[1]);
    std::swap(reordered.instances[1].primitives[0], reordered.instances[1].primitives[1]);
    const auto reordered_plan = build_native_raytracing_shading_plan(reordered);
    if (!check(reordered_plan.valid &&
                   reordered_plan.shading_fingerprint == plan.shading_fingerprint &&
                   reordered_plan.flattened_bindings[0].material_index == 0U &&
                   reordered_plan.flattened_bindings[1].material_index == 1U,
               "stable IDs did not make the shading plan independent of source array order")) {
        return 4;
    }
    auto changed = input;
    changed.instances[1].primitives[0].material.roughness = 0.52F;
    const auto changed_plan = build_native_raytracing_shading_plan(changed);
    if (!check(changed_plan.valid &&
                   changed_plan.shading_fingerprint != plan.shading_fingerprint,
               "a semantic PBR value change did not alter the shading fingerprint")) {
        return 5;
    }

    const auto future = build_native_raytracing_shading_plan(
        input, NativeRayTracingShadingOutputMode::future_linear_radiance);
    if (!check(!future.valid && !future.supported && future.fallback_active &&
                   !future.diagnostic_hit_mask && future.future_linear_radiance_planned &&
                   !future.linear_radiance_implemented && !future.claims_linear_radiance &&
                   future.output_contract == native_raytracing_shading_linear_radiance_contract &&
                   future.output_format == native_raytracing_shading_linear_radiance_format &&
                   future.code == "future-linear-radiance-not-implemented" &&
                   native_raytracing_shading_output_mode_name(
                       NativeRayTracingShadingOutputMode::future_linear_radiance) ==
                       "future-linear-radiance",
               "future linear radiance was presented as an implemented shader path")) {
        return 6;
    }

    auto duplicate_instance = input;
    duplicate_instance.instances[1].instance_id = duplicate_instance.instances[0].instance_id;
    if (!check(!build_native_raytracing_shading_plan(duplicate_instance).valid &&
                   build_native_raytracing_shading_plan(duplicate_instance).code ==
                       "instance-id-duplicate",
               "duplicate stable instance identity was accepted")) {
        return 7;
    }
    auto duplicate_primitive = input;
    duplicate_primitive.instances[0].primitives[1].primitive_id =
        duplicate_primitive.instances[0].primitives[0].primitive_id;
    if (!check(!build_native_raytracing_shading_plan(duplicate_primitive).valid &&
                   build_native_raytracing_shading_plan(duplicate_primitive).code ==
                       "primitive-id-duplicate",
               "duplicate primitive identity was accepted")) {
        return 8;
    }
    auto conflicting_material = input;
    conflicting_material.instances[1].primitives[0].material.material_id =
        "material.stone";
    if (!check(!build_native_raytracing_shading_plan(conflicting_material).valid &&
                   build_native_raytracing_shading_plan(conflicting_material).code ==
                       "material-id-conflict",
               "one material id with conflicting PBR values was accepted")) {
        return 9;
    }
    auto invalid_material = input;
    invalid_material.instances[1].primitives[0].material.metallic = 1.01F;
    if (!check(!build_native_raytracing_shading_plan(invalid_material).valid &&
                   build_native_raytracing_shading_plan(invalid_material).code ==
                       "material-values-invalid",
               "out-of-range metallic value was accepted")) {
        return 10;
    }
    auto missing_normal = input;
    missing_normal.instances[1].primitives[0].material.has_normal_texture = false;
    if (!check(!build_native_raytracing_shading_plan(missing_normal).valid &&
                   build_native_raytracing_shading_plan(missing_normal).code ==
                       "normal-texture-missing",
               "normal mapping without a texture binding was accepted")) {
        return 11;
    }
    auto invalid_light = input;
    invalid_light.directional_light.direction = {0.0F, 0.0F, 0.0F};
    if (!check(!build_native_raytracing_shading_plan(invalid_light).valid &&
                   build_native_raytracing_shading_plan(invalid_light).code ==
                       "directional-light-direction-invalid",
               "zero directional-light direction was accepted")) {
        return 12;
    }
    auto invalid_finite = input;
    invalid_finite.environment.intensity = std::numeric_limits<float>::quiet_NaN();
    if (!check(!build_native_raytracing_shading_plan(invalid_finite).valid &&
                   build_native_raytracing_shading_plan(invalid_finite).code ==
                       "environment-intensity-invalid",
               "non-finite environment intensity was accepted")) {
        return 13;
    }
    auto oversized = input;
    oversized.instances.resize(native_raytracing_shading_max_instances + 1U);
    if (!check(!build_native_raytracing_shading_plan(oversized).valid &&
                   build_native_raytracing_shading_plan(oversized).code ==
                       "instance-count-exceeded",
               "out-of-budget instance count was accepted")) {
        return 14;
    }

    std::cout << "native_raytracing_shading_tests: ok\n";
    return 0;
}
