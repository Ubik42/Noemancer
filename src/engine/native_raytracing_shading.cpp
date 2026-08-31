#include "engine/native_raytracing_shading.hpp"

#include <algorithm>
#include <cmath>
#include <cctype>
#include <cstring>

namespace noemancer {
namespace {

using Vec3 = NativeRayTracingShadingVec3;
using Color = NativeRayTracingShadingColor;

constexpr float kVectorEpsilon = 1.0e-6F;
constexpr std::uint64_t kFnvOffset = 1469598103934665603ULL;
constexpr std::uint64_t kFnvPrime = 1099511628211ULL;

float dot(const Vec3& left, const Vec3& right) noexcept {
    return left[0] * right[0] + left[1] * right[1] + left[2] * right[2];
}

float magnitude(const Vec3& value) noexcept {
    return std::sqrt(dot(value, value));
}

Vec3 scale(const Vec3& value, const float amount) noexcept {
    return {value[0] * amount, value[1] * amount, value[2] * amount};
}

bool finite(const Vec3& value) noexcept {
    return std::isfinite(value[0]) && std::isfinite(value[1]) &&
           std::isfinite(value[2]);
}

bool finite(const Color& value) noexcept {
    return std::isfinite(value[0]) && std::isfinite(value[1]) &&
           std::isfinite(value[2]) && std::isfinite(value[3]);
}

bool bounded_text(const std::string_view value) noexcept {
    if (value.empty() || value.size() > native_raytracing_shading_max_text_bytes) {
        return false;
    }
    return std::ranges::any_of(value, [](const char character) {
        return std::isspace(static_cast<unsigned char>(character)) == 0;
    });
}

std::string bounded_copy(const std::string_view value) {
    return std::string(value.substr(0, native_raytracing_shading_max_text_bytes));
}

bool valid_color(const Vec3& value) noexcept {
    return finite(value) && std::ranges::all_of(value, [](const float component) {
        return component >= 0.0F &&
               component <= native_raytracing_shading_max_color_component;
    });
}

bool valid_base_color(const Color& value) noexcept {
    return finite(value) && std::ranges::all_of(value, [](const float component) {
        return component >= 0.0F && component <= 1.0F;
    });
}

bool valid_material(const NativeRayTracingShadingMaterial& material) noexcept {
    return valid_base_color(material.base_color) &&
           std::isfinite(material.metallic) && material.metallic >= 0.0F &&
           material.metallic <= 1.0F && std::isfinite(material.roughness) &&
           material.roughness >= 0.0F && material.roughness <= 1.0F &&
           valid_color(material.emissive_color) &&
           std::isfinite(material.emissive_intensity) &&
           material.emissive_intensity >= 0.0F &&
           material.emissive_intensity <=
               native_raytracing_shading_max_emissive_intensity &&
           std::isfinite(material.normal_strength) &&
           material.normal_strength >= 0.0F &&
           material.normal_strength <= native_raytracing_shading_max_normal_strength;
}

bool same_material(const NativeRayTracingShadingMaterial& left,
                   const NativeRayTracingShadingMaterial& right) noexcept {
    return left.material_id == right.material_id &&
           left.base_color == right.base_color && left.metallic == right.metallic &&
           left.roughness == right.roughness &&
           left.emissive_color == right.emissive_color &&
           left.emissive_intensity == right.emissive_intensity &&
           left.has_base_color_texture == right.has_base_color_texture &&
           left.has_metallic_roughness_texture == right.has_metallic_roughness_texture &&
           left.has_emissive_texture == right.has_emissive_texture &&
           left.has_normal_texture == right.has_normal_texture &&
           left.normal_map_enabled == right.normal_map_enabled &&
           left.normal_strength == right.normal_strength &&
           left.double_sided == right.double_sided &&
           left.receives_shadows == right.receives_shadows;
}

void hash_byte(std::uint64_t& hash, const std::uint8_t value) noexcept {
    hash ^= value;
    hash *= kFnvPrime;
}

void hash_u32(std::uint64_t& hash, const std::uint32_t value) noexcept {
    hash_byte(hash, static_cast<std::uint8_t>(value & 0xffU));
    hash_byte(hash, static_cast<std::uint8_t>((value >> 8U) & 0xffU));
    hash_byte(hash, static_cast<std::uint8_t>((value >> 16U) & 0xffU));
    hash_byte(hash, static_cast<std::uint8_t>((value >> 24U) & 0xffU));
}

void hash_u64(std::uint64_t& hash, const std::uint64_t value) noexcept {
    hash_u32(hash, static_cast<std::uint32_t>(value & 0xffffffffULL));
    hash_u32(hash, static_cast<std::uint32_t>((value >> 32U) & 0xffffffffULL));
}

void hash_float(std::uint64_t& hash, const float value) noexcept {
    std::uint32_t bits{};
    static_assert(sizeof(bits) == sizeof(value));
    std::memcpy(&bits, &value, sizeof(bits));
    hash_u32(hash, bits);
}

void hash_text(std::uint64_t& hash, const std::string_view value) noexcept {
    hash_u64(hash, static_cast<std::uint64_t>(value.size()));
    for (const auto character : value) {
        hash_byte(hash, static_cast<std::uint8_t>(static_cast<unsigned char>(character)));
    }
}

void hash_vec3(std::uint64_t& hash, const Vec3& value) noexcept {
    hash_float(hash, value[0]);
    hash_float(hash, value[1]);
    hash_float(hash, value[2]);
}

void hash_color(std::uint64_t& hash, const Color& value) noexcept {
    hash_float(hash, value[0]);
    hash_float(hash, value[1]);
    hash_float(hash, value[2]);
    hash_float(hash, value[3]);
}

void hash_bool(std::uint64_t& hash, const bool value) noexcept {
    hash_byte(hash, value ? 1U : 0U);
}

void hash_material(std::uint64_t& hash,
                   const NativeRayTracingShadingMaterial& material) noexcept {
    hash_text(hash, material.material_id);
    hash_color(hash, material.base_color);
    hash_float(hash, material.metallic);
    hash_float(hash, material.roughness);
    hash_vec3(hash, material.emissive_color);
    hash_float(hash, material.emissive_intensity);
    hash_bool(hash, material.has_base_color_texture);
    hash_bool(hash, material.has_metallic_roughness_texture);
    hash_bool(hash, material.has_emissive_texture);
    hash_bool(hash, material.has_normal_texture);
    hash_bool(hash, material.normal_map_enabled);
    hash_float(hash, material.normal_strength);
    hash_bool(hash, material.double_sided);
    hash_bool(hash, material.receives_shadows);
}

std::uint64_t fingerprint(const NativeRayTracingShadingPlan& plan) noexcept {
    auto hash = kFnvOffset;
    hash_text(hash, plan.schema);
    hash_text(hash, plan.output_contract);
    hash_text(hash, plan.scene_id);
    hash_u64(hash, plan.scene_revision);
    hash_vec3(hash, plan.directional_light.direction);
    hash_vec3(hash, plan.directional_light.color);
    hash_float(hash, plan.directional_light.intensity);
    hash_bool(hash, plan.directional_light.enabled);
    hash_vec3(hash, plan.environment.color);
    hash_float(hash, plan.environment.intensity);
    hash_bool(hash, plan.environment.enabled);
    hash_u32(hash, plan.accepted_instance_count);
    hash_u32(hash, plan.accepted_primitive_count);
    hash_u32(hash, plan.accepted_material_count);
    for (const auto& instance : plan.instances) {
        hash_text(hash, instance.instance_id);
        hash_text(hash, instance.geometry_id);
        hash_bool(hash, instance.enabled);
        hash_u32(hash, static_cast<std::uint32_t>(instance.primitives.size()));
        for (const auto& primitive : instance.primitives) {
            hash_text(hash, primitive.primitive_id);
            hash_bool(hash, primitive.enabled);
            hash_material(hash, primitive.material);
        }
    }
    hash_u32(hash, plan.flattened_binding_count);
    for (const auto& binding : plan.flattened_bindings) {
        hash_text(hash, binding.instance_id);
        hash_text(hash, binding.primitive_id);
        hash_text(hash, binding.material_id);
        hash_u32(hash, binding.material_index);
    }
    return hash == 0U ? 1U : hash;
}

NativeRayTracingShadingPlan initial_plan(
    const NativeRayTracingShadingInput& input,
    const NativeRayTracingShadingOutputMode output_mode) {
    NativeRayTracingShadingPlan result;
    result.output_mode = output_mode;
    result.scene_id = bounded_copy(input.scene_id);
    result.scene_revision = input.scene_revision;
    result.directional_light = input.directional_light;
    result.environment = input.environment;
    if (output_mode == NativeRayTracingShadingOutputMode::future_linear_radiance) {
        result.output_contract =
            std::string(native_raytracing_shading_linear_radiance_contract);
        result.output_format =
            std::string(native_raytracing_shading_linear_radiance_format);
        result.output_semantic = "future-linear-radiance";
    }
    return result;
}

NativeRayTracingShadingPlan failure_plan(
    const NativeRayTracingShadingInput& input,
    const NativeRayTracingShadingOutputMode output_mode,
    const std::string_view code,
    const std::string_view detail) {
    auto result = initial_plan(input, output_mode);
    result.code = std::string(code);
    result.detail = std::string(detail);
    result.fallback_active = true;
    return result;
}

} // namespace

std::string_view native_raytracing_shading_output_mode_name(
    const NativeRayTracingShadingOutputMode mode) noexcept {
    switch (mode) {
    case NativeRayTracingShadingOutputMode::diagnostic_hit_mask:
        return "diagnostic-hit-mask";
    case NativeRayTracingShadingOutputMode::future_linear_radiance:
        return "future-linear-radiance";
    }
    return "unknown";
}

NativeRayTracingShadingPlan build_native_raytracing_shading_plan(
    const NativeRayTracingShadingInput& input,
    const NativeRayTracingShadingOutputMode output_mode) {
    if (output_mode != NativeRayTracingShadingOutputMode::diagnostic_hit_mask &&
        output_mode != NativeRayTracingShadingOutputMode::future_linear_radiance) {
        return failure_plan(input, output_mode, "shading-output-mode-invalid",
                            "The native ray-tracing shading output mode is outside the versioned contract vocabulary.");
    }
    if (!bounded_text(input.scene_id)) {
        return failure_plan(input, output_mode, "scene-id-invalid",
                            "The native ray-tracing shading input requires a non-empty bounded stable scene id.");
    }
    if (input.scene_revision == 0U) {
        return failure_plan(input, output_mode, "scene-revision-invalid",
                            "The native ray-tracing shading input requires a non-zero source scene revision.");
    }
    if (input.instances.size() > native_raytracing_shading_max_instances) {
        return failure_plan(input, output_mode, "instance-count-exceeded",
                            "The native ray-tracing shading plan exceeded its bounded instance count.");
    }

    const auto validate_light = [&]() -> std::string_view {
        if (!finite(input.directional_light.direction) ||
            magnitude(input.directional_light.direction) <= kVectorEpsilon) {
            return "directional-light-direction-invalid";
        }
        if (!valid_color(input.directional_light.color)) {
            return "directional-light-color-invalid";
        }
        if (!std::isfinite(input.directional_light.intensity) ||
            input.directional_light.intensity < 0.0F ||
            input.directional_light.intensity > native_raytracing_shading_max_light_intensity) {
            return "directional-light-intensity-invalid";
        }
        if (!valid_color(input.environment.color)) {
            return "environment-color-invalid";
        }
        if (!std::isfinite(input.environment.intensity) ||
            input.environment.intensity < 0.0F ||
            input.environment.intensity > native_raytracing_shading_max_light_intensity) {
            return "environment-intensity-invalid";
        }
        return {};
    };
    if (const auto code = validate_light(); !code.empty()) {
        return failure_plan(input, output_mode, code,
                            "The directional light or environment contains an invalid bounded PBR lighting value.");
    }

    auto result = initial_plan(input, output_mode);
    result.input_instance_count = static_cast<std::uint32_t>(input.instances.size());
    result.instances.reserve(input.instances.size());

    std::vector<std::string> instance_ids;
    instance_ids.reserve(input.instances.size());
    std::vector<NativeRayTracingShadingMaterial> materials;
    materials.reserve(std::min<std::size_t>(native_raytracing_shading_max_materials,
                                            input.instances.size()));
    std::size_t total_primitives{};

    for (const auto& input_instance : input.instances) {
        if (!bounded_text(input_instance.instance_id)) {
            return failure_plan(input, output_mode, "instance-id-invalid",
                                "Every RT shading instance requires a bounded stable instance id.");
        }
        if (!bounded_text(input_instance.geometry_id)) {
            return failure_plan(input, output_mode, "geometry-id-invalid",
                                "Every RT shading instance requires a bounded stable geometry id.");
        }
        if (std::ranges::find(instance_ids, input_instance.instance_id) != instance_ids.end()) {
            return failure_plan(input, output_mode, "instance-id-duplicate",
                                "RT shading instance ids must be unique within one scene input.");
        }
        instance_ids.push_back(input_instance.instance_id);
        if (input_instance.primitives.size() >
            native_raytracing_shading_max_primitives_per_instance) {
            return failure_plan(input, output_mode, "primitive-count-per-instance-exceeded",
                                "One RT shading instance exceeded its bounded primitive count.");
        }
        total_primitives += input_instance.primitives.size();
        if (total_primitives > native_raytracing_shading_max_primitives) {
            return failure_plan(input, output_mode, "primitive-count-exceeded",
                                "The native ray-tracing shading plan exceeded its bounded primitive count.");
        }

        std::vector<std::string> primitive_ids;
        primitive_ids.reserve(input_instance.primitives.size());
        for (const auto& input_primitive : input_instance.primitives) {
            if (!bounded_text(input_primitive.primitive_id)) {
                return failure_plan(input, output_mode, "primitive-id-invalid",
                                    "Every RT shading primitive requires a bounded stable primitive id.");
            }
            if (std::ranges::find(primitive_ids, input_primitive.primitive_id) !=
                primitive_ids.end()) {
                return failure_plan(input, output_mode, "primitive-id-duplicate",
                                    "RT shading primitive ids must be unique within their instance.");
            }
            primitive_ids.push_back(input_primitive.primitive_id);
            const auto& material = input_primitive.material;
            if (!bounded_text(material.material_id)) {
                return failure_plan(input, output_mode, "material-id-invalid",
                                    "Every RT shading material requires a bounded stable material id.");
            }
            if (!valid_material(material)) {
                return failure_plan(input, output_mode, "material-values-invalid",
                                    "RT shading PBR values must be finite and inside their bounded ranges.");
            }
            if (material.normal_map_enabled && !material.has_normal_texture) {
                return failure_plan(input, output_mode, "normal-texture-missing",
                                    "A normal-enabled RT material must declare a normal texture binding.");
            }
            const auto material_it = std::ranges::find_if(
                materials, [&material](const auto& existing) {
                    return existing.material_id == material.material_id;
                });
            if (material_it != materials.end()) {
                if (!same_material(*material_it, material)) {
                    return failure_plan(input, output_mode, "material-id-conflict",
                                        "One stable material id was supplied with conflicting PBR values.");
                }
            } else {
                if (materials.size() >= native_raytracing_shading_max_materials) {
                    return failure_plan(input, output_mode, "material-count-exceeded",
                                        "The native ray-tracing shading plan exceeded its bounded material count.");
                }
                materials.push_back(material);
            }
        }

        if (!input_instance.enabled) {
            ++result.excluded_disabled_instance_count;
            continue;
        }
        NativeRayTracingShadingInstance accepted_instance;
        accepted_instance.instance_id = input_instance.instance_id;
        accepted_instance.geometry_id = input_instance.geometry_id;
        accepted_instance.enabled = true;
        accepted_instance.primitives.reserve(input_instance.primitives.size());
        ++result.accepted_instance_count;
        for (const auto& input_primitive : input_instance.primitives) {
            if (!input_primitive.enabled) {
                ++result.excluded_disabled_primitive_count;
                continue;
            }
            accepted_instance.primitives.push_back(input_primitive);
            ++result.accepted_primitive_count;
        }
        result.instances.push_back(std::move(accepted_instance));
    }

    result.input_primitive_count = static_cast<std::uint32_t>(total_primitives);
    result.input_material_count = static_cast<std::uint32_t>(materials.size());
    std::vector<std::string> accepted_material_ids;
    for (const auto& instance : result.instances) {
        for (const auto& primitive : instance.primitives) {
            if (std::ranges::find(accepted_material_ids, primitive.material.material_id) ==
                accepted_material_ids.end()) {
                accepted_material_ids.push_back(primitive.material.material_id);
            }
        }
    }
    result.accepted_material_count =
        static_cast<std::uint32_t>(accepted_material_ids.size());
    std::ranges::sort(result.instances, {}, &NativeRayTracingShadingInstance::instance_id);
    for (auto& instance : result.instances) {
        std::ranges::sort(instance.primitives, {},
                          &NativeRayTracingShadingPrimitive::primitive_id);
    }
    result.flattened_bindings.reserve(result.accepted_primitive_count);
    for (const auto& instance : result.instances) {
        for (const auto& primitive : instance.primitives) {
            const auto material_index = static_cast<std::uint32_t>(
                result.flattened_bindings.size());
            result.flattened_bindings.push_back({
                instance.instance_id,
                primitive.primitive_id,
                primitive.material.material_id,
                material_index,
                primitive.material});
        }
    }
    result.flattened_binding_count =
        static_cast<std::uint32_t>(result.flattened_bindings.size());

    const auto direction_length = magnitude(result.directional_light.direction);
    result.directional_light.direction =
        scale(result.directional_light.direction, 1.0F / direction_length);
    result.pbr_inputs_valid = true;
    result.directional_light_described = true;
    result.environment_described = true;
    result.normal_flags_described = true;
    result.shading_fingerprint = fingerprint(result);

    if (output_mode == NativeRayTracingShadingOutputMode::future_linear_radiance) {
        result.code = "future-linear-radiance-not-implemented";
        result.detail =
            "The 0.1 contract prepares stable PBR inputs and direct-light/environment descriptions; a linear radiance producer is reserved for a later version and remains on fallback.";
        result.fallback_active = true;
        return result;
    }

    result.valid = true;
    result.supported = true;
    result.diagnostic_hit_mask = true;
    result.future_linear_radiance_planned = true;
    result.linear_radiance_implemented = false;
    result.claims_linear_radiance = false;
    result.fallback_active = false;
    result.code = "diagnostic-hit-mask-shading-input-ready";
    result.detail =
        "Stable PBR material, directional-light and environment inputs passed validation for the diagnostic hit-mask path; this contract does not evaluate or claim RTGI radiance.";
    return result;
}

} // namespace noemancer
