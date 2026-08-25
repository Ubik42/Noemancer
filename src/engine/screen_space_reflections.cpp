#include "engine/screen_space_reflections.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <nlohmann/json.hpp>
#include <ranges>
#include <utility>

namespace noemancer {
namespace {

using Json = nlohmann::json;

std::string bounded_text(const std::string_view value) {
    return std::string(value.substr(0U, screen_space_reflections_max_text_bytes));
}

constexpr std::uint8_t quality_value(const ScreenSpaceReflectionsQuality value) noexcept {
    return static_cast<std::uint8_t>(value);
}

constexpr std::uint8_t hybrid_policy_value(
    const ScreenSpaceReflectionsHybridPixelPolicy value) noexcept {
    return static_cast<std::uint8_t>(value);
}

void add_diagnostic(std::vector<ScreenSpaceReflectionsDiagnostic>& diagnostics,
                    std::string code, std::string path, std::string message) {
    if (diagnostics.size() >= screen_space_reflections_max_diagnostics) return;
    if (code.size() > screen_space_reflections_max_text_bytes)
        code.resize(screen_space_reflections_max_text_bytes);
    if (path.size() > screen_space_reflections_max_text_bytes)
        path.resize(screen_space_reflections_max_text_bytes);
    if (message.size() > screen_space_reflections_max_text_bytes)
        message.resize(screen_space_reflections_max_text_bytes);
    diagnostics.push_back(
        {std::move(code), std::move(path), std::move(message)});
}

bool text_non_empty(const std::string_view value) {
    return std::ranges::any_of(value, [](const unsigned char character) {
        return character > 0x20U;
    });
}

void validate_float(std::vector<ScreenSpaceReflectionsDiagnostic>& diagnostics,
                    const char* path, const float value, const float minimum,
                    const float maximum, const char* detail) {
    if (!std::isfinite(value) || value < minimum || value > maximum) {
        add_diagnostic(diagnostics, "ssr.range", path, detail);
    }
}

Json ray_march_json(const ScreenSpaceReflectionsRayMarchSettings& value) {
    return {
        {"hierarchical", value.hierarchical},
        {"startMip", value.start_mip},
        {"maxMip", value.max_mip},
        {"maxSteps", value.max_steps},
        {"binarySearchSteps", value.binary_search_steps},
        {"initialStepPixels", value.initial_step_pixels},
        {"maxDistance", value.max_distance},
        {"thickness", value.thickness},
        {"mipBias", value.mip_bias},
    };
}

Json material_json(const ScreenSpaceReflectionsMaterialEligibility& value) {
    return {
        {"contract", bounded_text(value.contract)},
        {"minimumRoughness", value.minimum_roughness},
        {"roughnessCutoff", value.roughness_cutoff},
        {"allowOpaque", value.allow_opaque},
        {"allowAlphaTested", value.allow_alpha_tested},
        {"allowTranslucent", value.allow_translucent},
        {"allowUnlit", value.allow_unlit},
    };
}

Json edge_fade_json(const ScreenSpaceReflectionsEdgeFade& value) {
    return {{"start", value.start}, {"end", value.end}};
}

Json composition_json(const ScreenSpaceReflectionsComposition& value) {
    return {
        {"strategy", bounded_text(value.strategy)},
        {"fallback", bounded_text(value.fallback)},
        {"confidenceThreshold", value.confidence_threshold},
        {"historyWeight", value.history_weight},
    };
}

Json config_json(const ScreenSpaceReflectionsConfig& value) {
    return {
        {"schema", bounded_text(value.schema)},
        {"profileId", bounded_text(value.profile_id)},
        {"quality", screen_space_reflections_quality_name(value.quality)},
        {"enabled", value.enabled},
        {"rayMarch", ray_march_json(value.ray_march)},
        {"material", material_json(value.material)},
        {"edgeFade", edge_fade_json(value.edge_fade)},
        {"composition", composition_json(value.composition)},
        {"hybridPixelPolicy",
         screen_space_reflections_hybrid_pixel_policy_name(value.hybrid_pixel_policy)},
    };
}

std::uint64_t fnv1a(const std::string_view value) noexcept {
    std::uint64_t hash = 14'695'981'039'346'656'037ULL;
    for (const auto character : value) {
        hash ^= static_cast<std::uint8_t>(character);
        hash *= 1'099'511'628'211ULL;
    }
    return hash;
}

std::string hex_u64(const std::uint64_t value) {
    constexpr char digits[] = "0123456789abcdef";
    std::string result(16U, '0');
    for (std::size_t index = 0U; index < result.size(); ++index) {
        const auto shift = static_cast<unsigned>((result.size() - index - 1U) * 4U);
        result[index] = digits[(value >> shift) & 0x0fU];
    }
    return result;
}

} // namespace

std::string_view screen_space_reflections_quality_name(
    const ScreenSpaceReflectionsQuality quality) noexcept {
    switch (quality) {
    case ScreenSpaceReflectionsQuality::off: return "off";
    case ScreenSpaceReflectionsQuality::low: return "low";
    case ScreenSpaceReflectionsQuality::medium: return "medium";
    case ScreenSpaceReflectionsQuality::high: return "high";
    }
    return "unknown";
}

std::optional<ScreenSpaceReflectionsQuality>
screen_space_reflections_quality_from_string(const std::string_view value) noexcept {
    if (value == "off") return ScreenSpaceReflectionsQuality::off;
    if (value == "low") return ScreenSpaceReflectionsQuality::low;
    if (value == "medium") return ScreenSpaceReflectionsQuality::medium;
    if (value == "high") return ScreenSpaceReflectionsQuality::high;
    return std::nullopt;
}

bool screen_space_reflections_quality_valid(
    const ScreenSpaceReflectionsQuality quality) noexcept {
    return quality_value(quality) <= quality_value(ScreenSpaceReflectionsQuality::high);
}

std::string_view screen_space_reflections_hybrid_pixel_policy_name(
    const ScreenSpaceReflectionsHybridPixelPolicy policy) noexcept {
    switch (policy) {
    case ScreenSpaceReflectionsHybridPixelPolicy::disable: return "disabled";
    case ScreenSpaceReflectionsHybridPixelPolicy::spatial_only:
        return "spatial-only-no-history";
    case ScreenSpaceReflectionsHybridPixelPolicy::allow_temporal:
        return "allow-temporal";
    }
    return "unknown";
}

std::optional<ScreenSpaceReflectionsHybridPixelPolicy>
screen_space_reflections_hybrid_pixel_policy_from_string(
    const std::string_view value) noexcept {
    if (value == "disabled" || value == "disable")
        return ScreenSpaceReflectionsHybridPixelPolicy::disable;
    if (value == "spatial-only-no-history" || value == "spatial-only")
        return ScreenSpaceReflectionsHybridPixelPolicy::spatial_only;
    if (value == "allow-temporal")
        return ScreenSpaceReflectionsHybridPixelPolicy::allow_temporal;
    return std::nullopt;
}

bool screen_space_reflections_hybrid_pixel_policy_valid(
    const ScreenSpaceReflectionsHybridPixelPolicy policy) noexcept {
    return hybrid_policy_value(policy) <= hybrid_policy_value(
        ScreenSpaceReflectionsHybridPixelPolicy::allow_temporal);
}

ScreenSpaceReflectionsConfig
screen_space_reflections_quality_defaults(const ScreenSpaceReflectionsQuality quality) {
    ScreenSpaceReflectionsConfig result;
    result.quality = quality;
    result.enabled = quality != ScreenSpaceReflectionsQuality::off;
    result.hybrid_pixel_policy = ScreenSpaceReflectionsHybridPixelPolicy::disable;

    switch (quality) {
    case ScreenSpaceReflectionsQuality::off:
        result.ray_march = {};
        result.ray_march.hierarchical = false;
        result.material.roughness_cutoff = 0.0F;
        result.composition.confidence_threshold = 1.0F;
        result.composition.history_weight = 0.0F;
        break;
    case ScreenSpaceReflectionsQuality::low:
        result.ray_march.start_mip = 1U;
        result.ray_march.max_mip = 5U;
        result.ray_march.max_steps = 8U;
        result.ray_march.binary_search_steps = 2U;
        result.ray_march.initial_step_pixels = 1.0F;
        result.ray_march.max_distance = 25.0F;
        result.ray_march.thickness = 0.16F;
        result.material.roughness_cutoff = 0.60F;
        result.edge_fade = {0.55F, 0.90F};
        result.composition.confidence_threshold = 0.25F;
        result.composition.history_weight = 0.75F;
        break;
    case ScreenSpaceReflectionsQuality::medium:
        result.ray_march.start_mip = 1U;
        result.ray_march.max_mip = 8U;
        result.ray_march.max_steps = 16U;
        result.ray_march.binary_search_steps = 4U;
        result.ray_march.initial_step_pixels = 1.0F;
        result.ray_march.max_distance = 50.0F;
        result.ray_march.thickness = 0.12F;
        result.material.roughness_cutoff = 0.75F;
        result.edge_fade = {0.60F, 0.93F};
        result.composition.confidence_threshold = 0.20F;
        result.composition.history_weight = 0.85F;
        break;
    case ScreenSpaceReflectionsQuality::high:
        result.ray_march.start_mip = 0U;
        result.ray_march.max_mip = 12U;
        result.ray_march.max_steps = 32U;
        result.ray_march.binary_search_steps = 5U;
        result.ray_march.initial_step_pixels = 1.0F;
        result.ray_march.max_distance = 100.0F;
        result.ray_march.thickness = 0.08F;
        result.material.roughness_cutoff = 0.85F;
        result.edge_fade = {0.65F, 0.95F};
        result.composition.confidence_threshold = 0.15F;
        result.composition.history_weight = 0.90F;
        break;
    }
    return result;
}

std::vector<ScreenSpaceReflectionsDiagnostic>
validate_screen_space_reflections(const ScreenSpaceReflectionsConfig& config) {
    std::vector<ScreenSpaceReflectionsDiagnostic> diagnostics;
    if (config.schema != screen_space_reflections_schema) {
        add_diagnostic(diagnostics, "ssr.unsupported-schema", "/schema",
                       "Expected noemancer.screen-space-reflections/0.1.");
    }
    if (!text_non_empty(config.profile_id) || config.profile_id.size() > 128U) {
        add_diagnostic(diagnostics, "ssr.profile-id-range", "/profileId",
                       "profileId must contain at most 128 UTF-8 bytes.");
    }
    if (!screen_space_reflections_quality_valid(config.quality)) {
        add_diagnostic(diagnostics, "ssr.invalid-quality", "/quality",
                       "quality must be off, low, medium or high.");
    }
    if (!screen_space_reflections_hybrid_pixel_policy_valid(config.hybrid_pixel_policy)) {
        add_diagnostic(diagnostics, "ssr.invalid-hybrid-pixel-policy",
                       "/hybridPixelPolicy",
                       "hybridPixelPolicy is outside the bounded vocabulary.");
    }
    if (config.quality == ScreenSpaceReflectionsQuality::off && config.enabled) {
        add_diagnostic(diagnostics, "ssr.off-enabled-conflict", "/enabled",
                       "An Off quality profile must set enabled to false.");
    }

    const auto& ray = config.ray_march;
    if (config.enabled) {
        if (!ray.hierarchical) {
            add_diagnostic(diagnostics, "ssr.hierarchy-required", "/rayMarch/hierarchical",
                           "The production SSR path requires hierarchical depth marching.");
        }
        if (ray.max_steps == 0U || ray.max_steps > screen_space_reflections_max_ray_steps) {
            add_diagnostic(diagnostics, "ssr.max-steps-range", "/rayMarch/maxSteps",
                           "maxSteps must be in [1,128].");
        }
        if (ray.binary_search_steps > screen_space_reflections_max_binary_search_steps ||
            ray.binary_search_steps > ray.max_steps) {
            add_diagnostic(diagnostics, "ssr.binary-search-range",
                           "/rayMarch/binarySearchSteps",
                           "binarySearchSteps must not exceed maxSteps or 16.");
        }
        if (ray.start_mip > screen_space_reflections_max_hierarchy_mip ||
            ray.max_mip > screen_space_reflections_max_hierarchy_mip ||
            ray.start_mip > ray.max_mip) {
            add_diagnostic(diagnostics, "ssr.mip-range", "/rayMarch",
                           "startMip/maxMip must be ordered and in [0,16].");
        }
        validate_float(diagnostics, "/rayMarch/initialStepPixels",
                       ray.initial_step_pixels, 0.25F, 16.0F,
                       "initialStepPixels must be finite in [0.25,16].");
        validate_float(diagnostics, "/rayMarch/maxDistance", ray.max_distance,
                       0.01F, 10'000.0F,
                       "maxDistance must be finite in [0.01,10000].");
        validate_float(diagnostics, "/rayMarch/thickness", ray.thickness,
                       0.0001F, 10.0F,
                       "thickness must be finite in [0.0001,10].");
        validate_float(diagnostics, "/rayMarch/mipBias", ray.mip_bias,
                       -8.0F, 8.0F,
                       "mipBias must be finite in [-8,8].");

        const auto& material = config.material;
        if (material.contract != screen_space_reflections_material_contract) {
            add_diagnostic(diagnostics, "ssr.material-contract-unsupported",
                           "/material/contract",
                           "The current SSR shader contract is F0.rgb+roughness.a.");
        }
        validate_float(diagnostics, "/material/minimumRoughness",
                       material.minimum_roughness, 0.0F, 1.0F,
                       "minimumRoughness must be finite in [0,1].");
        validate_float(diagnostics, "/material/roughnessCutoff",
                       material.roughness_cutoff, 0.0F, 1.0F,
                       "roughnessCutoff must be finite in [0,1].");
        if (material.minimum_roughness > material.roughness_cutoff) {
            add_diagnostic(diagnostics, "ssr.roughness-order",
                           "/material", "minimumRoughness must not exceed roughnessCutoff.");
        }
        if (!material.allow_opaque && !material.allow_alpha_tested &&
            !material.allow_translucent && !material.allow_unlit) {
            add_diagnostic(diagnostics, "ssr.material-empty-eligibility",
                           "/material", "At least one material class must be eligible.");
        }

        const auto& edge = config.edge_fade;
        validate_float(diagnostics, "/edgeFade/start", edge.start, 0.0F, 1.0F,
                       "edgeFade.start must be finite in [0,1].");
        validate_float(diagnostics, "/edgeFade/end", edge.end, 0.0F, 1.0F,
                       "edgeFade.end must be finite in [0,1].");
        if (edge.start >= edge.end) {
            add_diagnostic(diagnostics, "ssr.edge-fade-order", "/edgeFade",
                           "edgeFade.start must be less than edgeFade.end.");
        }

        if (config.composition.strategy != screen_space_reflections_composition_strategy) {
            add_diagnostic(diagnostics, "ssr.composition-unsupported",
                           "/composition/strategy",
                           "The current strategy replaces IBL specular by confidence.");
        }
        if (config.composition.fallback != screen_space_reflections_fallback_strategy) {
            add_diagnostic(diagnostics, "ssr.fallback-unsupported", "/composition/fallback",
                           "The current fallback retains IBL specular.");
        }
        validate_float(diagnostics, "/composition/confidenceThreshold",
                       config.composition.confidence_threshold, 0.0F, 1.0F,
                       "confidenceThreshold must be finite in [0,1].");
        validate_float(diagnostics, "/composition/historyWeight",
                       config.composition.history_weight, 0.0F, 1.0F,
                       "historyWeight must be finite in [0,1].");
    }
    return diagnostics;
}

ScreenSpaceReflectionsPlan build_screen_space_reflections_plan(
    const ScreenSpaceReflectionsConfig& config, const bool hybrid_pixel_active,
    const ScreenSpaceReflectionsInputAvailability& inputs) {
    ScreenSpaceReflectionsPlan result;
    result.profile_id = config.profile_id;
    result.quality = config.quality;
    result.config = config;
    result.inputs = inputs;
    result.hybrid_pixel_active = hybrid_pixel_active;
    result.config_valid = validate_screen_space_reflections(config).empty();
    result.valid = result.config_valid;
    result.fallback_enabled = true;
    result.code = result.config_valid ? "ssr.disabled" : "ssr.invalid-config";
    result.detail = result.config_valid
                        ? "SSR is intentionally disabled by the selected policy."
                        : "SSR configuration failed the plain-data validity contract.";
    if (!result.config_valid) {
        result.diagnostics = validate_screen_space_reflections(config);
        return result;
    }

    result.enabled = config.enabled && config.quality != ScreenSpaceReflectionsQuality::off;
    result.hierarchical_depth_required = result.enabled && config.ray_march.hierarchical;
    result.temporal_history_required = result.enabled &&
        config.hybrid_pixel_policy != ScreenSpaceReflectionsHybridPixelPolicy::spatial_only &&
        config.composition.history_weight > 0.0F;

    if (!result.enabled) {
        result.fallback_only = true;
        result.code = "ssr.off";
        result.detail = "SSR is Off; the material's IBL specular is retained.";
        return result;
    }

    if (hybrid_pixel_active &&
        config.hybrid_pixel_policy == ScreenSpaceReflectionsHybridPixelPolicy::disable) {
        result.enabled = false;
        result.temporal_history_required = false;
        result.disabled_by_hybrid_pixel = true;
        result.fallback_only = true;
        result.code = "ssr.hybrid-pixel-disabled";
        result.detail =
            "Hybrid Pixel disables SSR to preserve deterministic pixel stability; IBL remains.";
        return result;
    }

    if (!inputs.depth_pyramid_ready || !inputs.scene_color_ready ||
        !inputs.material_buffers_ready) {
        result.enabled = false;
        result.temporal_history_required = false;
        result.fallback_only = true;
        result.code = "ssr.inputs-unavailable";
        result.detail = "SSR inputs are unavailable; the declared IBL fallback is active.";
        if (!inputs.depth_pyramid_ready)
            add_diagnostic(result.diagnostics, "ssr.depth-pyramid-unavailable",
                           "/inputs/depthPyramidReady",
                           "The hierarchical depth pyramid is not ready.");
        if (!inputs.scene_color_ready)
            add_diagnostic(result.diagnostics, "ssr.scene-color-unavailable",
                           "/inputs/sceneColorReady",
                           "The scene color source is not ready.");
        if (!inputs.material_buffers_ready)
            add_diagnostic(result.diagnostics, "ssr.material-buffers-unavailable",
                           "/inputs/materialBuffersReady",
                           "The F0/roughness material source is not ready.");
        return result;
    }

    result.code = hybrid_pixel_active &&
                          config.hybrid_pixel_policy ==
                              ScreenSpaceReflectionsHybridPixelPolicy::spatial_only
                      ? "ssr.ready-spatial-only"
                      : "ssr.ready";
    result.detail = result.temporal_history_required
                        ? "Hierarchical SSR is ready with shared temporal history."
                        : "Hierarchical SSR is ready without temporal history.";
    return result;
}

std::string screen_space_reflections_canonical_config(
    const ScreenSpaceReflectionsConfig& config) {
    return config_json(config).dump(2) + "\n";
}

std::string screen_space_reflections_canonical_evidence(
    const ScreenSpaceReflectionsPlan& plan) {
    Json diagnostics = Json::array();
    for (const auto& diagnostic : plan.diagnostics) {
        diagnostics.push_back({
            {"code", diagnostic.code.substr(0U, screen_space_reflections_max_text_bytes)},
            {"path", diagnostic.path.substr(0U, screen_space_reflections_max_text_bytes)},
            {"message", diagnostic.message.substr(0U, screen_space_reflections_max_text_bytes)},
        });
    }
    const Json output = {
        {"schema", bounded_text(plan.schema)},
        {"profileId", bounded_text(plan.profile_id)},
        {"quality", screen_space_reflections_quality_name(plan.quality)},
        {"configValid", plan.config_valid},
        {"valid", plan.valid},
        {"enabled", plan.enabled},
        {"code", bounded_text(plan.code)},
        {"detail", bounded_text(plan.detail)},
        {"hybridPixelActive", plan.hybrid_pixel_active},
        {"hybridPixelPolicy", screen_space_reflections_hybrid_pixel_policy_name(
                                 plan.config.hybrid_pixel_policy)},
        {"disabledByHybridPixel", plan.disabled_by_hybrid_pixel},
        {"hierarchicalDepthRequired", plan.hierarchical_depth_required},
        {"temporalHistoryRequired", plan.temporal_history_required},
        {"fallbackEnabled", plan.fallback_enabled},
        {"fallbackOnly", plan.fallback_only},
        {"rayMarch", ray_march_json(plan.config.ray_march)},
        {"material", material_json(plan.config.material)},
        {"edgeFade", edge_fade_json(plan.config.edge_fade)},
        {"confidenceThreshold", plan.config.composition.confidence_threshold},
        {"historyWeight", plan.config.composition.history_weight},
        {"composition", bounded_text(plan.config.composition.strategy)},
        {"fallback", bounded_text(plan.config.composition.fallback)},
        {"inputs", {
            {"depthPyramidReady", plan.inputs.depth_pyramid_ready},
            {"sceneColorReady", plan.inputs.scene_color_ready},
            {"materialBuffersReady", plan.inputs.material_buffers_ready},
        }},
        {"diagnostics", std::move(diagnostics)},
    };
    return output.dump(2) + "\n";
}

std::string screen_space_reflections_fingerprint(
    const ScreenSpaceReflectionsPlan& plan) {
    return "fnv1a64:" + hex_u64(fnv1a(screen_space_reflections_canonical_evidence(plan)));
}

} // namespace noemancer
