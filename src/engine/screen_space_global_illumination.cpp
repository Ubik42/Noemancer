#include "engine/screen_space_global_illumination.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <nlohmann/json.hpp>
#include <ranges>
#include <utility>

namespace noemancer {
namespace {

using Json = nlohmann::json;

std::string bounded_text(const std::string_view value) {
    return std::string(value.substr(0U, screen_space_global_illumination_max_text_bytes));
}

constexpr std::uint8_t quality_value(
    const ScreenSpaceGlobalIlluminationQuality value) noexcept {
    return static_cast<std::uint8_t>(value);
}

constexpr std::uint8_t policy_value(
    const ScreenSpaceGlobalIlluminationHybridPixelPolicy value) noexcept {
    return static_cast<std::uint8_t>(value);
}

void add_diagnostic(
    std::vector<ScreenSpaceGlobalIlluminationDiagnostic>& diagnostics,
    std::string code, std::string path, std::string message) {
    if (diagnostics.size() >= screen_space_global_illumination_max_diagnostics) return;
    if (code.size() > screen_space_global_illumination_max_text_bytes)
        code.resize(screen_space_global_illumination_max_text_bytes);
    if (path.size() > screen_space_global_illumination_max_text_bytes)
        path.resize(screen_space_global_illumination_max_text_bytes);
    if (message.size() > screen_space_global_illumination_max_text_bytes)
        message.resize(screen_space_global_illumination_max_text_bytes);
    diagnostics.push_back({std::move(code), std::move(path), std::move(message)});
}

bool text_non_empty(const std::string_view value) {
    return std::ranges::any_of(value, [](const unsigned char character) {
        return character > 0x20U;
    });
}

void validate_float(std::vector<ScreenSpaceGlobalIlluminationDiagnostic>& diagnostics,
                    const char* path, const float value, const float minimum,
                    const float maximum, const char* message) {
    if (!std::isfinite(value) || value < minimum || value > maximum)
        add_diagnostic(diagnostics, "ssgi.range", path, message);
}

Json sampling_json(const ScreenSpaceGlobalIlluminationSamplingSettings& value) {
    return {
        {"hierarchical", value.hierarchical},
        {"startMip", value.start_mip},
        {"maxMip", value.max_mip},
        {"sampleCount", value.sample_count},
        {"directions", value.directions},
        {"maxSteps", value.max_steps},
        {"rayStepPixels", value.ray_step_pixels},
        {"radius", value.radius},
        {"maxDistance", value.max_distance},
        {"thickness", value.thickness},
        {"intensity", value.intensity},
        {"falloff", value.falloff},
    };
}

Json material_json(const ScreenSpaceGlobalIlluminationMaterialEligibility& value) {
    return {
        {"contract", bounded_text(value.contract)},
        {"minimumRoughness", value.minimum_roughness},
        {"roughnessCutoff", value.roughness_cutoff},
        {"allowOpaque", value.allow_opaque},
        {"allowAlphaTested", value.allow_alpha_tested},
        {"allowTranslucent", value.allow_translucent},
        {"allowUnlit", value.allow_unlit},
        {"requireNormal", value.require_normal},
    };
}

Json bent_normal_json(const ScreenSpaceGlobalIlluminationBentNormal& value) {
    return {
        {"enabled", value.enabled},
        {"semantics", bounded_text(value.semantics)},
        {"strength", value.strength},
    };
}

Json visibility_json(const ScreenSpaceGlobalIlluminationVisibility& value) {
    return {
        {"enabled", value.enabled},
        {"semantics", bounded_text(value.semantics)},
        {"power", value.power},
    };
}

Json history_json(const ScreenSpaceGlobalIlluminationHistorySettings& value) {
    return {
        {"enabled", value.enabled},
        {"policy", bounded_text(value.policy)},
        {"fallback", bounded_text(value.fallback)},
        {"weight", value.weight},
        {"depthRejection", value.depth_rejection},
        {"normalRejectionCosine", value.normal_rejection_cosine},
    };
}

Json composition_json(const ScreenSpaceGlobalIlluminationComposition& value) {
    return {
        {"strategy", bounded_text(value.strategy)},
        {"fallback", bounded_text(value.fallback)},
        {"confidenceThreshold", value.confidence_threshold},
    };
}

Json config_json(const ScreenSpaceGlobalIlluminationConfig& value) {
    return {
        {"schema", bounded_text(value.schema)},
        {"profileId", bounded_text(value.profile_id)},
        {"quality", screen_space_global_illumination_quality_name(value.quality)},
        {"enabled", value.enabled},
        {"sampling", sampling_json(value.sampling)},
        {"material", material_json(value.material)},
        {"bentNormal", bent_normal_json(value.bent_normal)},
        {"visibility", visibility_json(value.visibility)},
        {"history", history_json(value.history)},
        {"composition", composition_json(value.composition)},
        {"hybridPixelPolicy",
         screen_space_global_illumination_hybrid_pixel_policy_name(
             value.hybrid_pixel_policy)},
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

std::string_view screen_space_global_illumination_quality_name(
    const ScreenSpaceGlobalIlluminationQuality quality) noexcept {
    switch (quality) {
    case ScreenSpaceGlobalIlluminationQuality::off: return "off";
    case ScreenSpaceGlobalIlluminationQuality::low: return "low";
    case ScreenSpaceGlobalIlluminationQuality::medium: return "medium";
    case ScreenSpaceGlobalIlluminationQuality::high: return "high";
    }
    return "unknown";
}

std::optional<ScreenSpaceGlobalIlluminationQuality>
screen_space_global_illumination_quality_from_string(
    const std::string_view value) noexcept {
    if (value == "off") return ScreenSpaceGlobalIlluminationQuality::off;
    if (value == "low") return ScreenSpaceGlobalIlluminationQuality::low;
    if (value == "medium") return ScreenSpaceGlobalIlluminationQuality::medium;
    if (value == "high") return ScreenSpaceGlobalIlluminationQuality::high;
    return std::nullopt;
}

bool screen_space_global_illumination_quality_valid(
    const ScreenSpaceGlobalIlluminationQuality quality) noexcept {
    return quality_value(quality) <= quality_value(
        ScreenSpaceGlobalIlluminationQuality::high);
}

std::string_view screen_space_global_illumination_hybrid_pixel_policy_name(
    const ScreenSpaceGlobalIlluminationHybridPixelPolicy policy) noexcept {
    switch (policy) {
    case ScreenSpaceGlobalIlluminationHybridPixelPolicy::disable: return "disabled";
    case ScreenSpaceGlobalIlluminationHybridPixelPolicy::spatial_only:
        return "spatial-only-no-history";
    case ScreenSpaceGlobalIlluminationHybridPixelPolicy::allow_temporal:
        return "allow-temporal";
    }
    return "unknown";
}

std::optional<ScreenSpaceGlobalIlluminationHybridPixelPolicy>
screen_space_global_illumination_hybrid_pixel_policy_from_string(
    const std::string_view value) noexcept {
    if (value == "disabled" || value == "disable")
        return ScreenSpaceGlobalIlluminationHybridPixelPolicy::disable;
    if (value == "spatial-only-no-history" || value == "spatial-only")
        return ScreenSpaceGlobalIlluminationHybridPixelPolicy::spatial_only;
    if (value == "allow-temporal")
        return ScreenSpaceGlobalIlluminationHybridPixelPolicy::allow_temporal;
    return std::nullopt;
}

bool screen_space_global_illumination_hybrid_pixel_policy_valid(
    const ScreenSpaceGlobalIlluminationHybridPixelPolicy policy) noexcept {
    return policy_value(policy) <= policy_value(
        ScreenSpaceGlobalIlluminationHybridPixelPolicy::allow_temporal);
}

ScreenSpaceGlobalIlluminationConfig
screen_space_global_illumination_quality_defaults(
    const ScreenSpaceGlobalIlluminationQuality quality) {
    ScreenSpaceGlobalIlluminationConfig result;
    result.quality = quality;
    result.enabled = quality != ScreenSpaceGlobalIlluminationQuality::off;
    result.hybrid_pixel_policy =
        ScreenSpaceGlobalIlluminationHybridPixelPolicy::disable;

    switch (quality) {
    case ScreenSpaceGlobalIlluminationQuality::off:
        result.sampling = {};
        result.sampling.hierarchical = false;
        result.material.roughness_cutoff = 0.0F;
        result.bent_normal.enabled = false;
        result.visibility.enabled = false;
        result.history.enabled = false;
        result.history.weight = 0.0F;
        result.composition.confidence_threshold = 1.0F;
        break;
    case ScreenSpaceGlobalIlluminationQuality::low:
        result.sampling.start_mip = 1U;
        result.sampling.max_mip = 5U;
        result.sampling.sample_count = 2U;
        result.sampling.directions = 2U;
        result.sampling.max_steps = 4U;
        result.sampling.ray_step_pixels = 1.0F;
        result.sampling.radius = 1.5F;
        result.sampling.max_distance = 25.0F;
        result.sampling.thickness = 0.16F;
        result.sampling.intensity = 0.80F;
        result.sampling.falloff = 1.0F;
        result.material.roughness_cutoff = 1.0F;
        result.bent_normal.strength = 0.80F;
        result.visibility.power = 1.0F;
        result.history.weight = 0.70F;
        result.composition.confidence_threshold = 0.30F;
        break;
    case ScreenSpaceGlobalIlluminationQuality::medium:
        result.sampling.start_mip = 1U;
        result.sampling.max_mip = 8U;
        result.sampling.sample_count = 4U;
        result.sampling.directions = 4U;
        result.sampling.max_steps = 6U;
        result.sampling.ray_step_pixels = 1.0F;
        result.sampling.radius = 2.5F;
        result.sampling.max_distance = 45.0F;
        result.sampling.thickness = 0.12F;
        result.sampling.intensity = 1.0F;
        result.sampling.falloff = 1.0F;
        result.material.roughness_cutoff = 1.0F;
        result.bent_normal.strength = 1.0F;
        result.visibility.power = 1.0F;
        result.history.weight = 0.85F;
        result.composition.confidence_threshold = 0.24F;
        break;
    case ScreenSpaceGlobalIlluminationQuality::high:
        result.sampling.start_mip = 0U;
        result.sampling.max_mip = 12U;
        result.sampling.sample_count = 8U;
        result.sampling.directions = 8U;
        result.sampling.max_steps = 8U;
        result.sampling.ray_step_pixels = 1.0F;
        result.sampling.radius = 4.0F;
        result.sampling.max_distance = 80.0F;
        result.sampling.thickness = 0.08F;
        result.sampling.intensity = 1.20F;
        result.sampling.falloff = 1.0F;
        result.material.roughness_cutoff = 1.0F;
        result.bent_normal.strength = 1.0F;
        result.visibility.power = 1.0F;
        result.history.weight = 0.90F;
        result.composition.confidence_threshold = 0.20F;
        break;
    }
    return result;
}

std::vector<ScreenSpaceGlobalIlluminationDiagnostic>
validate_screen_space_global_illumination(
    const ScreenSpaceGlobalIlluminationConfig& config) {
    std::vector<ScreenSpaceGlobalIlluminationDiagnostic> diagnostics;
    if (config.schema != screen_space_global_illumination_schema) {
        add_diagnostic(diagnostics, "ssgi.unsupported-schema", "/schema",
                       "Expected noemancer.screen-space-global-illumination/0.1.");
    }
    if (!text_non_empty(config.profile_id) || config.profile_id.size() > 128U) {
        add_diagnostic(diagnostics, "ssgi.profile-id-range", "/profileId",
                       "profileId must contain at most 128 UTF-8 bytes.");
    }
    if (!screen_space_global_illumination_quality_valid(config.quality)) {
        add_diagnostic(diagnostics, "ssgi.invalid-quality", "/quality",
                       "quality must be off, low, medium or high.");
    }
    if (!screen_space_global_illumination_hybrid_pixel_policy_valid(
            config.hybrid_pixel_policy)) {
        add_diagnostic(diagnostics, "ssgi.invalid-hybrid-pixel-policy",
                       "/hybridPixelPolicy",
                       "hybridPixelPolicy is outside the bounded vocabulary.");
    }
    if (config.quality == ScreenSpaceGlobalIlluminationQuality::off && config.enabled) {
        add_diagnostic(diagnostics, "ssgi.off-enabled-conflict", "/enabled",
                       "An Off quality profile must set enabled to false.");
    }

    if (!config.enabled) return diagnostics;

    const auto& sampling = config.sampling;
    if (!sampling.hierarchical) {
        add_diagnostic(diagnostics, "ssgi.hierarchy-required", "/sampling/hierarchical",
                       "The production SSGI path requires hierarchical depth sampling.");
    }
    if (sampling.sample_count == 0U ||
        sampling.sample_count > screen_space_global_illumination_max_samples) {
        add_diagnostic(diagnostics, "ssgi.sample-count-range", "/sampling/sampleCount",
                       "sampleCount must be in [1,16].");
    }
    if (sampling.directions == 0U ||
        sampling.directions > screen_space_global_illumination_max_directions) {
        add_diagnostic(diagnostics, "ssgi.direction-count-range", "/sampling/directions",
                       "directions must be in [1,16].");
    } else if (sampling.directions != sampling.sample_count) {
        add_diagnostic(diagnostics, "ssgi.direction-count-mismatch", "/sampling/directions",
                       "SSGI v0.1 requires directions to equal sampleCount.");
    }
    if (sampling.max_steps == 0U ||
        sampling.max_steps > screen_space_global_illumination_max_ray_steps) {
        add_diagnostic(diagnostics, "ssgi.max-steps-range", "/sampling/maxSteps",
                       "maxSteps must be in [1,32].");
    }
    if (sampling.start_mip > screen_space_global_illumination_max_hierarchy_mip ||
        sampling.max_mip > screen_space_global_illumination_max_hierarchy_mip ||
        sampling.start_mip > sampling.max_mip) {
        add_diagnostic(diagnostics, "ssgi.mip-range", "/sampling",
                       "startMip/maxMip must be ordered and in [0,16].");
    }
    validate_float(diagnostics, "/sampling/rayStepPixels", sampling.ray_step_pixels,
                   0.25F, 16.0F, "rayStepPixels must be finite in [0.25,16].");
    validate_float(diagnostics, "/sampling/radius", sampling.radius,
                   0.001F, 100.0F, "radius must be finite in [0.001,100].");
    validate_float(diagnostics, "/sampling/maxDistance", sampling.max_distance,
                   0.01F, 10'000.0F,
                   "maxDistance must be finite in [0.01,10000].");
    validate_float(diagnostics, "/sampling/thickness", sampling.thickness,
                   0.0001F, 10.0F,
                   "thickness must be finite in [0.0001,10].");
    validate_float(diagnostics, "/sampling/intensity", sampling.intensity,
                   0.0F, 16.0F, "intensity must be finite in [0,16].");
    validate_float(diagnostics, "/sampling/falloff", sampling.falloff,
                   0.01F, 8.0F, "falloff must be finite in [0.01,8].");

    const auto& material = config.material;
    if (material.contract != screen_space_global_illumination_material_contract) {
        add_diagnostic(diagnostics, "ssgi.material-contract-unsupported",
                       "/material/contract",
                       "The current SSGI material contract is normal.rgb+roughness.a+baseColor.rgb+metallic.a.");
    }
    validate_float(diagnostics, "/material/minimumRoughness",
                   material.minimum_roughness, 0.0F, 1.0F,
                   "minimumRoughness must be finite in [0,1].");
    validate_float(diagnostics, "/material/roughnessCutoff",
                   material.roughness_cutoff, 0.0F, 1.0F,
                   "roughnessCutoff must be finite in [0,1].");
    if (material.minimum_roughness > material.roughness_cutoff) {
        add_diagnostic(diagnostics, "ssgi.roughness-order", "/material",
                       "minimumRoughness must not exceed roughnessCutoff.");
    }
    if (!material.allow_opaque && !material.allow_alpha_tested &&
        !material.allow_translucent && !material.allow_unlit) {
        add_diagnostic(diagnostics, "ssgi.material-empty-eligibility", "/material",
                       "At least one material class must be eligible.");
    }
    if (material.require_normal && !config.bent_normal.enabled) {
        add_diagnostic(diagnostics, "ssgi.normal-output-required",
                       "/bentNormal/enabled",
                       "Material validity requires a normal-aware bent-normal path.");
    }

    const auto& bent_normal = config.bent_normal;
    if (bent_normal.semantics != screen_space_global_illumination_bent_normal_semantics) {
        add_diagnostic(diagnostics, "ssgi.bent-normal-semantics-unsupported",
                       "/bentNormal/semantics",
                       "The current bent-normal semantics are visibility-weighted-hemisphere-normal.");
    }
    validate_float(diagnostics, "/bentNormal/strength", bent_normal.strength,
                   0.0F, 2.0F, "bentNormal.strength must be finite in [0,2].");

    const auto& visibility = config.visibility;
    if (visibility.semantics != screen_space_global_illumination_visibility_semantics) {
        add_diagnostic(diagnostics, "ssgi.visibility-semantics-unsupported",
                       "/visibility/semantics",
                       "The current visibility semantics are confidence-weighted-ambient-visibility.");
    }
    validate_float(diagnostics, "/visibility/power", visibility.power,
                   0.01F, 8.0F, "visibility.power must be finite in [0.01,8].");

    const auto& history = config.history;
    if (history.policy != screen_space_global_illumination_history_policy) {
        add_diagnostic(diagnostics, "ssgi.history-policy-unsupported", "/history/policy",
                       "The current SSGI history policy uses shared temporal history.");
    }
    if (history.fallback != screen_space_global_illumination_history_fallback) {
        add_diagnostic(diagnostics, "ssgi.history-fallback-unsupported",
                       "/history/fallback",
                       "The current history fallback is a spatial current-frame estimate.");
    }
    validate_float(diagnostics, "/history/weight", history.weight,
                   0.0F, 1.0F, "history.weight must be finite in [0,1].");
    validate_float(diagnostics, "/history/depthRejection", history.depth_rejection,
                   0.0F, 1.0F,
                   "history.depthRejection must be finite in [0,1].");
    validate_float(diagnostics, "/history/normalRejectionCosine",
                   history.normal_rejection_cosine, -1.0F, 1.0F,
                   "history.normalRejectionCosine must be finite in [-1,1].");

    if (config.composition.strategy != screen_space_global_illumination_composition_strategy) {
        add_diagnostic(diagnostics, "ssgi.composition-unsupported",
                       "/composition/strategy",
                       "The current composition replaces IBL diffuse by confidence.");
    }
    if (config.composition.fallback != screen_space_global_illumination_fallback_strategy) {
        add_diagnostic(diagnostics, "ssgi.fallback-unsupported", "/composition/fallback",
                       "The current fallback retains IBL diffuse.");
    }
    validate_float(diagnostics, "/composition/confidenceThreshold",
                   config.composition.confidence_threshold, 0.0F, 1.0F,
                   "confidenceThreshold must be finite in [0,1].");
    return diagnostics;
}

ScreenSpaceGlobalIlluminationPlan build_screen_space_global_illumination_plan(
    const ScreenSpaceGlobalIlluminationConfig& config, const bool hybrid_pixel_active,
    const ScreenSpaceGlobalIlluminationInputAvailability& inputs,
    const ScreenSpaceGlobalIlluminationHistoryInput& history_inputs) {
    ScreenSpaceGlobalIlluminationPlan result;
    result.profile_id = config.profile_id;
    result.quality = config.quality;
    result.config = config;
    result.inputs = inputs;
    result.history_inputs = history_inputs;
    result.hybrid_pixel_active = hybrid_pixel_active;
    result.config_valid = validate_screen_space_global_illumination(config).empty();
    result.valid = result.config_valid;
    result.fallback_enabled = true;
    result.code = result.config_valid ? "ssgi.disabled" : "ssgi.invalid-config";
    result.detail = result.config_valid
                        ? "SSGI is intentionally disabled by the selected policy."
                        : "SSGI configuration failed the plain-data validity contract.";
    result.history.policy = config.history.policy;
    result.history.fallback = config.history.fallback;
    result.history.target_ready = inputs.history_target_ready;
    if (!result.config_valid) {
        result.diagnostics = validate_screen_space_global_illumination(config);
        return result;
    }

    result.enabled = config.enabled &&
        config.quality != ScreenSpaceGlobalIlluminationQuality::off;
    result.hierarchical_depth_required = result.enabled && config.sampling.hierarchical;
    result.bent_normal_output = result.enabled && config.bent_normal.enabled;
    result.visibility_output = result.enabled && config.visibility.enabled;
    result.history.required = result.enabled && config.history.enabled &&
        config.hybrid_pixel_policy !=
            ScreenSpaceGlobalIlluminationHybridPixelPolicy::spatial_only &&
        config.history.weight > 0.0F;

    if (!result.enabled) {
        result.fallback_only = true;
        result.history.fallback_to_current_frame = true;
        result.history.reset_reason = "disabled";
        result.code = "ssgi.off";
        result.detail = "SSGI is Off; the material's IBL diffuse is retained.";
        return result;
    }

    if (hybrid_pixel_active &&
        config.hybrid_pixel_policy ==
            ScreenSpaceGlobalIlluminationHybridPixelPolicy::disable) {
        result.enabled = false;
        result.hierarchical_depth_required = false;
        result.bent_normal_output = false;
        result.visibility_output = false;
        result.history.required = false;
        result.history.fallback_to_current_frame = true;
        result.history.reset_reason = "hybrid-pixel-disabled";
        result.disabled_by_hybrid_pixel = true;
        result.fallback_only = true;
        result.code = "ssgi.hybrid-pixel-disabled";
        result.detail =
            "Hybrid Pixel disables SSGI to preserve deterministic pixel stability; IBL remains.";
        return result;
    }

    if (!inputs.depth_pyramid_ready || !inputs.normal_buffer_ready ||
        !inputs.material_buffers_ready) {
        result.enabled = false;
        result.hierarchical_depth_required = false;
        result.bent_normal_output = false;
        result.visibility_output = false;
        result.history.required = false;
        result.history.fallback_to_current_frame = true;
        result.history.reset_reason = "inputs-unavailable";
        result.fallback_only = true;
        result.code = "ssgi.inputs-unavailable";
        result.detail = "SSGI inputs are unavailable; the declared IBL fallback is active.";
        if (!inputs.depth_pyramid_ready)
            add_diagnostic(result.diagnostics, "ssgi.depth-pyramid-unavailable",
                           "/inputs/depthPyramidReady",
                           "The hierarchical depth pyramid is not ready.");
        if (!inputs.normal_buffer_ready)
            add_diagnostic(result.diagnostics, "ssgi.normal-buffer-unavailable",
                           "/inputs/normalBufferReady",
                           "The normal buffer is not ready.");
        if (!inputs.material_buffers_ready)
            add_diagnostic(result.diagnostics, "ssgi.material-buffers-unavailable",
                           "/inputs/materialBuffersReady",
                           "The normal/roughness/albedo material source is not ready.");
        return result;
    }

    if (result.history.required) {
        if (!inputs.history_target_ready) {
            result.history.target_ready = false;
            result.history.use_previous = false;
            result.history.reset_required = true;
            result.history.fallback_to_current_frame = true;
            result.history.reset_reason = "history-target-unavailable";
            add_diagnostic(result.diagnostics, "ssgi.history-target-unavailable",
                           "/inputs/historyTargetReady",
                           "SSGI remains enabled with a spatial current-frame history fallback.");
        } else if (history_inputs.camera_cut) {
            result.history.reset_required = true;
            result.history.fallback_to_current_frame = true;
            result.history.reset_reason = "camera-cut";
        } else if (history_inputs.extent_changed) {
            result.history.reset_required = true;
            result.history.fallback_to_current_frame = true;
            result.history.reset_reason = "extent-changed";
        } else if (history_inputs.profile_changed) {
            result.history.reset_required = true;
            result.history.fallback_to_current_frame = true;
            result.history.reset_reason = "profile-changed";
        } else if (!history_inputs.previous_valid) {
            result.history.reset_required = true;
            result.history.fallback_to_current_frame = true;
            result.history.reset_reason = "first-frame";
        } else {
            result.history.use_previous = true;
            result.history.reset_reason = "none";
        }
    } else {
        result.history.reset_required = config.history.enabled;
        result.history.fallback_to_current_frame = true;
        result.history.reset_reason = config.history.enabled ? "policy-disabled" : "disabled";
    }

    result.code = hybrid_pixel_active &&
                          config.hybrid_pixel_policy ==
                              ScreenSpaceGlobalIlluminationHybridPixelPolicy::spatial_only
                      ? "ssgi.ready-spatial-only"
                      : result.history.fallback_to_current_frame
                          ? "ssgi.ready-with-history-fallback"
                          : "ssgi.ready";
    result.detail = result.history.use_previous
                        ? "Hierarchical SSGI is ready with shared temporal history."
                        : "Hierarchical SSGI is ready with a spatial current-frame fallback.";
    return result;
}

std::string screen_space_global_illumination_canonical_config(
    const ScreenSpaceGlobalIlluminationConfig& config) {
    return config_json(config).dump(2) + "\n";
}

std::string screen_space_global_illumination_canonical_evidence(
    const ScreenSpaceGlobalIlluminationPlan& plan) {
    Json diagnostics = Json::array();
    for (const auto& diagnostic : plan.diagnostics) {
        diagnostics.push_back({
            {"code", bounded_text(diagnostic.code)},
            {"path", bounded_text(diagnostic.path)},
            {"message", bounded_text(diagnostic.message)},
        });
    }
    const Json output = {
        {"schema", bounded_text(plan.schema)},
        {"profileId", bounded_text(plan.profile_id)},
        {"quality", screen_space_global_illumination_quality_name(plan.quality)},
        {"configValid", plan.config_valid},
        {"valid", plan.valid},
        {"enabled", plan.enabled},
        {"code", bounded_text(plan.code)},
        {"detail", bounded_text(plan.detail)},
        {"hybridPixelActive", plan.hybrid_pixel_active},
        {"hybridPixelPolicy",
         screen_space_global_illumination_hybrid_pixel_policy_name(
             plan.config.hybrid_pixel_policy)},
        {"disabledByHybridPixel", plan.disabled_by_hybrid_pixel},
        {"hierarchicalDepthRequired", plan.hierarchical_depth_required},
        {"sampleCount", plan.config.sampling.sample_count},
        {"directions", plan.config.sampling.directions},
        {"maxSteps", plan.config.sampling.max_steps},
        {"radius", plan.config.sampling.radius},
        {"thickness", plan.config.sampling.thickness},
        {"intensity", plan.config.sampling.intensity},
        {"material", material_json(plan.config.material)},
        {"bentNormal", bent_normal_json(plan.config.bent_normal)},
        {"visibility", visibility_json(plan.config.visibility)},
        {"history", {
            {"required", plan.history.required},
            {"targetReady", plan.history.target_ready},
            {"usePrevious", plan.history.use_previous},
            {"resetRequired", plan.history.reset_required},
            {"fallbackToCurrentFrame", plan.history.fallback_to_current_frame},
            {"policy", bounded_text(plan.history.policy)},
            {"fallback", bounded_text(plan.history.fallback)},
            {"resetReason", bounded_text(plan.history.reset_reason)},
            {"weight", plan.config.history.weight},
        }},
        {"confidenceThreshold", plan.config.composition.confidence_threshold},
        {"composition", bounded_text(plan.config.composition.strategy)},
        {"fallback", bounded_text(plan.config.composition.fallback)},
        {"fallbackEnabled", plan.fallback_enabled},
        {"fallbackOnly", plan.fallback_only},
        {"inputs", {
            {"depthPyramidReady", plan.inputs.depth_pyramid_ready},
            {"normalBufferReady", plan.inputs.normal_buffer_ready},
            {"materialBuffersReady", plan.inputs.material_buffers_ready},
            {"historyTargetReady", plan.inputs.history_target_ready},
        }},
        {"diagnostics", std::move(diagnostics)},
    };
    return output.dump(2) + "\n";
}

std::string screen_space_global_illumination_fingerprint(
    const ScreenSpaceGlobalIlluminationPlan& plan) {
    return "fnv1a64:" + hex_u64(fnv1a(
        screen_space_global_illumination_canonical_evidence(plan)));
}

} // namespace noemancer
