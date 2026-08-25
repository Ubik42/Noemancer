#include "engine/shadow_scalability_policy.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <nlohmann/json.hpp>
#include <utility>

namespace noemancer {
namespace {

using Json = nlohmann::json;

constexpr std::uint64_t max_observed_count = 1'000'000'000ULL;
constexpr double keep_memory_ratio = 0.70;
constexpr double keep_cache_hit_ratio = 0.90;
constexpr double keep_time_ratio = 0.70;
constexpr double keep_invalidation_ratio = 0.10;
constexpr double prototype_invalidation_ratio = 0.25;

std::string bounded_text(const std::string_view value) {
    return std::string(value.substr(0U, shadow_scalability_policy_max_text_bytes));
}

bool text_valid(const std::string_view value) {
    return !value.empty() && value.size() <= shadow_scalability_policy_max_text_bytes;
}

void add_diagnostic(std::vector<ShadowScalabilityDiagnostic>& diagnostics,
                    std::string code, std::string path, std::string message) {
    if (diagnostics.size() >= shadow_scalability_policy_max_diagnostics) return;
    diagnostics.push_back(ShadowScalabilityDiagnostic{
        .code = bounded_text(code),
        .path = bounded_text(path),
        .message = bounded_text(message),
    });
}

void add_range_diagnostic(std::vector<ShadowScalabilityDiagnostic>& diagnostics,
                         const std::string_view path, const std::string_view message) {
    add_diagnostic(diagnostics, "shadow-scalability.range", std::string(path),
                   std::string(message));
}

void add_missing(std::vector<ShadowScalabilityDiagnostic>& diagnostics,
                 const std::string_view code, const std::string_view path,
                 const std::string_view message) {
    add_diagnostic(diagnostics, std::string(code), std::string(path),
                   std::string(message));
}

Json finite_json(const double value) {
    return std::isfinite(value) ? Json(value) : Json(nullptr);
}

Json workload_json(const ShadowScalabilityWorkload& workload) {
    return Json{
        {"schema", bounded_text(workload.schema)},
        {"directionalEnabled", workload.directional_enabled},
        {"cascadeCount", workload.cascade_count},
        {"cascadeResolution", workload.cascade_resolution},
        {"localEnabled", workload.local_enabled},
        {"localLayerCount", workload.local_layer_count},
        {"localResolution", workload.local_resolution},
        {"requestedLocalLights", workload.requested_local_lights},
        {"selectedLocalLights", workload.selected_local_lights},
        {"droppedLocalLights", workload.dropped_local_lights},
        {"estimatedAtlasBytes", workload.estimated_atlas_bytes},
    };
}

Json cache_json(const ShadowScalabilityCacheObservation& cache) {
    return Json{
        {"available", cache.available},
        {"directionalCascadesAvailable", cache.directional_cascades_available},
        {"directionalCascadesCached", cache.directional_cascades_cached},
        {"directionalCacheHits", cache.directional_cache_hits},
        {"directionalCacheMisses", cache.directional_cache_misses},
        {"localFacesAvailable", cache.local_faces_available},
        {"localFacesCached", cache.local_faces_cached},
        {"localCacheHits", cache.local_cache_hits},
        {"localCacheMisses", cache.local_cache_misses},
    };
}

Json geometry_json(const ShadowScalabilityGeometryObservation& geometry) {
    return Json{
        {"available", geometry.available},
        {"casterCount", geometry.caster_count},
        {"primitiveCount", geometry.primitive_count},
        {"drawCount", geometry.draw_count},
        {"instancesSubmitted", geometry.instances_submitted},
        {"drawCallsSaved", geometry.draw_calls_saved},
    };
}

Json timing_json(const ShadowScalabilityTimingObservation& timing) {
    return Json{
        {"available", timing.available},
        {"shadowPassMilliseconds", finite_json(timing.shadow_pass_milliseconds)},
        {"directionalPassMilliseconds", finite_json(timing.directional_pass_milliseconds)},
        {"localPassMilliseconds", finite_json(timing.local_pass_milliseconds)},
        {"frameBudgetMilliseconds", finite_json(timing.frame_budget_milliseconds)},
    };
}

Json invalidation_json(const ShadowScalabilityInvalidationObservation& invalidation) {
    return Json{
        {"available", invalidation.available},
        {"cameraRevision", invalidation.camera_revision},
        {"lightRevision", invalidation.light_revision},
        {"casterRevision", invalidation.caster_revision},
        {"cameraInvalidated", invalidation.camera_invalidated},
        {"lightInvalidated", invalidation.light_invalidated},
        {"casterInvalidated", invalidation.caster_invalidated},
        {"invalidationsLastWindow", invalidation.invalidations_last_window},
        {"observationFrames", invalidation.observation_frames},
    };
}

Json input_json(const ShadowScalabilityInput& input) {
    return Json{
        {"schema", bounded_text(input.schema)},
        {"workload", workload_json(input.workload)},
        {"cache", cache_json(input.cache)},
        {"geometry", geometry_json(input.geometry)},
        {"timing", timing_json(input.timing)},
        {"invalidation", invalidation_json(input.invalidation)},
        {"atlasBudgetBytes", input.atlas_budget_bytes},
    };
}

Json diagnostic_json(const ShadowScalabilityDiagnostic& diagnostic) {
    return Json{
        {"code", bounded_text(diagnostic.code)},
        {"path", bounded_text(diagnostic.path)},
        {"message", bounded_text(diagnostic.message)},
    };
}

Json diagnostics_json(const std::vector<ShadowScalabilityDiagnostic>& diagnostics) {
    Json output = Json::array();
    for (const auto& diagnostic : diagnostics) output.push_back(diagnostic_json(diagnostic));
    return output;
}

std::uint64_t conservative_bytes(const std::uint64_t bytes) {
    if (bytes > shadow_scalability_policy_max_bytes) return shadow_scalability_policy_max_bytes;
    const auto margin = bytes / 10U + (bytes % 10U != 0U ? 1U : 0U);
    if (bytes > shadow_scalability_policy_max_bytes - margin)
        return shadow_scalability_policy_max_bytes;
    return bytes + margin;
}

std::uint64_t saturating_add(const std::uint64_t left, const std::uint64_t right) {
    if (left > std::numeric_limits<std::uint64_t>::max() - right)
        return std::numeric_limits<std::uint64_t>::max();
    return left + right;
}

std::uint64_t cache_operations(const ShadowScalabilityCacheObservation& cache) {
    return saturating_add(
        saturating_add(cache.directional_cache_hits, cache.directional_cache_misses),
        saturating_add(cache.local_cache_hits, cache.local_cache_misses));
}

std::uint64_t cache_hits(const ShadowScalabilityCacheObservation& cache) {
    return saturating_add(cache.directional_cache_hits, cache.local_cache_hits);
}

void add_reason(ShadowScalabilityPlan& plan, const std::string_view code,
                const std::string_view path, const std::string_view message) {
    add_diagnostic(plan.reasons, std::string(code), std::string(path),
                   std::string(message));
}

void set_insufficient_evidence(ShadowScalabilityPlan& plan,
                               const std::string_view code,
                               const std::string_view detail) {
    plan.recommendation = ShadowScalabilityRecommendation::insufficient_evidence;
    plan.valid = false;
    plan.evidence_complete = false;
    plan.code = std::string(code);
    plan.detail = std::string(detail);
    plan.diagnostics = plan.missing_evidence;
}

std::uint64_t count_sum(const std::uint64_t left, const std::uint64_t right) {
    return saturating_add(left, right);
}

std::uint64_t max_count(const std::uint64_t value) {
    return std::min(value, max_observed_count);
}

std::uint64_t fnv1a(const std::string_view value) noexcept {
    std::uint64_t hash = 1469598103934665603ULL;
    for (const auto character : value) {
        hash ^= static_cast<std::uint8_t>(character);
        hash *= 1099511628211ULL;
    }
    return hash;
}

std::string hex_u64(const std::uint64_t value) {
    constexpr char digits[] = "0123456789abcdef";
    std::string result(16U, '0');
    for (std::size_t index = 0U; index < result.size(); ++index) {
        const auto shift = static_cast<unsigned int>((result.size() - 1U - index) * 4U);
        result[index] = digits[(value >> shift) & 0x0fU];
    }
    return result;
}

} // namespace

std::string_view shadow_scalability_recommendation_name(
    const ShadowScalabilityRecommendation recommendation) noexcept {
    switch (recommendation) {
    case ShadowScalabilityRecommendation::keep_atlas: return "keep-atlas";
    case ShadowScalabilityRecommendation::extend_atlas: return "extend-atlas";
    case ShadowScalabilityRecommendation::prototype_virtual_pages:
        return "prototype-virtual-pages";
    case ShadowScalabilityRecommendation::insufficient_evidence:
        return "insufficient-evidence";
    }
    return "insufficient-evidence";
}

std::vector<ShadowScalabilityDiagnostic>
validate_shadow_scalability_input(const ShadowScalabilityInput& input) {
    std::vector<ShadowScalabilityDiagnostic> diagnostics;
    if (input.schema != shadow_scalability_policy_schema || !text_valid(input.schema))
        add_diagnostic(diagnostics, "shadow-scalability.unsupported-schema", "/schema",
                       "Expected noemancer.shadow-scalability-policy/0.1.");
    if (!text_valid(input.workload.schema))
        add_diagnostic(diagnostics, "shadow-scalability.workload-schema", "/workload/schema",
                       "The workload schema must be bounded and non-empty.");
    if (!input.workload.directional_enabled && !input.workload.local_enabled)
        add_diagnostic(diagnostics, "shadow-scalability.no-shadow-workload", "/workload",
                       "At least one directional or local shadow workload must be enabled.");
    if (input.workload.directional_enabled &&
        (input.workload.cascade_count == 0U ||
         input.workload.cascade_count > shadow_scalability_policy_max_cascades))
        add_range_diagnostic(diagnostics, "/workload/cascadeCount",
                             "Directional cascade count is outside the bounded range.");
    if (input.workload.directional_enabled &&
        (input.workload.cascade_resolution < shadow_scalability_policy_min_resolution ||
         input.workload.cascade_resolution > shadow_scalability_policy_max_resolution))
        add_range_diagnostic(diagnostics, "/workload/cascadeResolution",
                             "Directional shadow resolution is outside the bounded range.");
    if (input.workload.local_enabled &&
        (input.workload.local_layer_count == 0U ||
         input.workload.local_layer_count > shadow_scalability_policy_max_layers))
        add_range_diagnostic(diagnostics, "/workload/localLayerCount",
                             "Local shadow layer count is outside the bounded range.");
    if (input.workload.local_enabled &&
        (input.workload.local_resolution < shadow_scalability_policy_min_resolution ||
         input.workload.local_resolution > shadow_scalability_policy_max_resolution))
        add_range_diagnostic(diagnostics, "/workload/localResolution",
                             "Local shadow resolution is outside the bounded range.");
    if (input.workload.selected_local_lights > input.workload.requested_local_lights ||
        input.workload.dropped_local_lights > input.workload.requested_local_lights ||
        count_sum(input.workload.selected_local_lights, input.workload.dropped_local_lights) !=
            input.workload.requested_local_lights)
        add_diagnostic(diagnostics, "shadow-scalability.local-light-counts",
                       "/workload/localLights",
                       "Selected and dropped local lights must partition requested lights.");
    if (input.workload.estimated_atlas_bytes == 0U ||
        input.workload.estimated_atlas_bytes > shadow_scalability_policy_max_bytes)
        add_range_diagnostic(diagnostics, "/workload/estimatedAtlasBytes",
                             "Estimated atlas bytes must be finite and non-zero.");
    if (input.atlas_budget_bytes == 0U ||
        input.atlas_budget_bytes > shadow_scalability_policy_max_bytes)
        add_range_diagnostic(diagnostics, "/atlasBudgetBytes",
                             "Atlas budget bytes must be finite and non-zero.");

    const auto& cache = input.cache;
    if (cache.directional_cascades_cached > cache.directional_cascades_available ||
        (input.workload.directional_enabled &&
         cache.directional_cascades_available > input.workload.cascade_count) ||
        cache.local_faces_cached > cache.local_faces_available ||
        (input.workload.local_enabled &&
         cache.local_faces_available > input.workload.local_layer_count))
        add_diagnostic(diagnostics, "shadow-scalability.cache-counts", "/cache",
                       "Cached shadow units cannot exceed available workload units.");
    if (!cache.available && diagnostics.size() < shadow_scalability_policy_max_diagnostics)
        add_missing(diagnostics, "shadow-scalability.cache-invalid", "/cache/available",
                    "Cache observations are marked unavailable.");
    if (!input.geometry.available && diagnostics.size() < shadow_scalability_policy_max_diagnostics)
        add_missing(diagnostics, "shadow-scalability.geometry-invalid", "/geometry/available",
                    "Caster, primitive and draw observations are unavailable.");

    const auto& timing = input.timing;
    const bool timing_finite = std::isfinite(timing.shadow_pass_milliseconds) &&
        std::isfinite(timing.directional_pass_milliseconds) &&
        std::isfinite(timing.local_pass_milliseconds) &&
        std::isfinite(timing.frame_budget_milliseconds);
    if (timing.available &&
        (!timing_finite || timing.shadow_pass_milliseconds < 0.0 ||
         timing.directional_pass_milliseconds < 0.0 || timing.local_pass_milliseconds < 0.0 ||
         timing.frame_budget_milliseconds <= 0.0 ||
         timing.shadow_pass_milliseconds <
             std::max(timing.directional_pass_milliseconds, timing.local_pass_milliseconds)))
        add_diagnostic(diagnostics, "shadow-scalability.timing-range", "/timing",
                       "GPU pass timings must be finite, ordered and within a positive frame budget.");
    if (!timing.available && diagnostics.size() < shadow_scalability_policy_max_diagnostics)
        add_missing(diagnostics, "shadow-scalability.timing-invalid", "/timing/available",
                    "GPU pass timing evidence is unavailable.");

    const auto& invalidation = input.invalidation;
    if (invalidation.available && invalidation.observation_frames == 0U)
        add_diagnostic(diagnostics, "shadow-scalability.invalidation-range",
                       "/invalidation/observationFrames",
                       "Invalidation observations require at least one frame.");
    if (invalidation.available && invalidation.invalidations_last_window >
            max_count(invalidation.observation_frames) * 64ULL)
        add_diagnostic(diagnostics, "shadow-scalability.invalidation-range",
                       "/invalidation/invalidationsLastWindow",
                       "Invalidation count exceeds the bounded observation window.");
    if (!invalidation.available && diagnostics.size() < shadow_scalability_policy_max_diagnostics)
        add_missing(diagnostics, "shadow-scalability.invalidation-invalid", "/invalidation/available",
                    "Camera/light/caster invalidation evidence is unavailable.");

    if (input.geometry.available &&
        (input.geometry.caster_count > max_observed_count ||
         input.geometry.primitive_count > max_observed_count ||
         input.geometry.draw_count > max_observed_count ||
         input.geometry.instances_submitted > max_observed_count ||
         input.geometry.draw_calls_saved > max_observed_count))
        add_range_diagnostic(diagnostics, "/geometry",
                             "Geometry workload counts exceed the bounded range.");
    return diagnostics;
}

ShadowScalabilityPlan evaluate_shadow_scalability(const ShadowScalabilityInput& input) {
    ShadowScalabilityPlan plan;
    plan.input = input;
    plan.schema = std::string(shadow_scalability_policy_schema);
    plan.atlas_budget_bytes = input.atlas_budget_bytes;
    plan.conservative_atlas_bytes = conservative_bytes(input.workload.estimated_atlas_bytes);

    const auto validation = validate_shadow_scalability_input(input);
    plan.diagnostics = validation;
    if (!validation.empty()) {
        plan.missing_evidence = validation;
        set_insufficient_evidence(plan, "shadow-scalability.invalid-input",
                                  "Invalid shadow workload facts cannot produce a scalability recommendation.");
        plan.diagnostics = validation;
        return plan;
    }

    const auto cache_total = cache_operations(input.cache);
    if (cache_total == 0U)
        add_missing(plan.missing_evidence, "shadow-scalability.cache-samples-missing",
                    "/cache/hitsAndMisses",
                    "At least one cache hit or miss is required to judge atlas reuse.");
    if (input.geometry.caster_count == 0U || input.geometry.primitive_count == 0U ||
        input.geometry.draw_count == 0U)
        add_missing(plan.missing_evidence, "shadow-scalability.geometry-samples-missing",
                    "/geometry", "Caster, primitive and draw counts must be non-zero observations.");
    if (input.invalidation.observation_frames == 0U)
        add_missing(plan.missing_evidence, "shadow-scalability.invalidation-samples-missing",
                    "/invalidation/observationFrames", "Invalidation evidence needs an observation window.");
    if (!plan.missing_evidence.empty()) {
        set_insufficient_evidence(plan, "shadow-scalability.evidence-missing",
                                  "Required cache, geometry or invalidation evidence is missing.");
        plan.diagnostics.insert(plan.diagnostics.end(), plan.missing_evidence.begin(),
                                plan.missing_evidence.end());
        return plan;
    }

    plan.valid = true;
    plan.evidence_complete = true;
    plan.cache_hit_ratio = static_cast<double>(cache_hits(input.cache)) /
        static_cast<double>(cache_total);
    plan.shadow_time_ratio = input.timing.shadow_pass_milliseconds /
        input.timing.frame_budget_milliseconds;
    plan.invalidation_ratio = static_cast<double>(input.invalidation.invalidations_last_window) /
        static_cast<double>(input.invalidation.observation_frames);
    plan.atlas_headroom_bytes = plan.atlas_budget_bytes > plan.conservative_atlas_bytes
        ? plan.atlas_budget_bytes - plan.conservative_atlas_bytes : 0U;

    const bool over_budget = plan.conservative_atlas_bytes > plan.atlas_budget_bytes;
    const bool local_light_pressure = input.workload.dropped_local_lights > 0U;
    const bool timing_pressure = plan.shadow_time_ratio > 1.0;
    const bool invalidation_pressure = plan.invalidation_ratio > prototype_invalidation_ratio;
    if (over_budget || timing_pressure || invalidation_pressure) {
        plan.recommendation = ShadowScalabilityRecommendation::prototype_virtual_pages;
        plan.code = "shadow-scalability.prototype-virtual-pages";
        plan.detail = "Atlas capacity or update pressure is beyond a conservative extension decision.";
        if (over_budget)
            add_reason(plan, "shadow-scalability.atlas-budget-exceeded", "/atlasBudgetBytes",
                       "The conservative atlas estimate exceeds the available atlas budget.");
        if (timing_pressure)
            add_reason(plan, "shadow-scalability.pass-budget-exceeded", "/timing/shadowPassMilliseconds",
                       "Shadow recording exceeds the supplied frame budget.");
        if (invalidation_pressure)
            add_reason(plan, "shadow-scalability.invalidation-churn", "/invalidation/invalidationsLastWindow",
                       "Camera/light/caster invalidation churn makes atlas redraw pressure high.");
    } else if (!local_light_pressure &&
               plan.conservative_atlas_bytes * 10ULL <= plan.atlas_budget_bytes * 7ULL &&
               plan.cache_hit_ratio >= keep_cache_hit_ratio &&
               plan.shadow_time_ratio <= keep_time_ratio &&
               plan.invalidation_ratio <= keep_invalidation_ratio) {
        plan.recommendation = ShadowScalabilityRecommendation::keep_atlas;
        plan.code = "shadow-scalability.keep-atlas";
        plan.detail = "Atlas headroom, cache reuse and shadow pass timing are conservatively healthy.";
        add_reason(plan, "shadow-scalability.atlas-headroom-healthy", "/atlasBudgetBytes",
                   "Conservative atlas usage is at or below 70 percent of budget.");
        add_reason(plan, "shadow-scalability.cache-stable", "/cache/hitsAndMisses",
                   "Observed cache reuse is at least 90 percent.");
        add_reason(plan, "shadow-scalability.pass-within-budget", "/timing/shadowPassMilliseconds",
                   "Shadow pass timing is at or below 70 percent of the frame budget.");
    } else {
        plan.recommendation = ShadowScalabilityRecommendation::extend_atlas;
        plan.code = "shadow-scalability.extend-atlas";
        plan.detail = "The atlas remains within budget but one or more bounded pressure signals need capacity or cache tuning.";
        if (plan.conservative_atlas_bytes * 10ULL > plan.atlas_budget_bytes * 7ULL)
            add_reason(plan, "shadow-scalability.atlas-headroom-tight", "/atlasBudgetBytes",
                       "Conservative atlas usage is above the 70 percent headroom target.");
        if (local_light_pressure)
            add_reason(plan, "shadow-scalability.local-lights-dropped", "/workload/droppedLocalLights",
                       "The current local-shadow atlas cannot retain every requested light; measure a bounded atlas extension before virtual pages.");
        if (plan.cache_hit_ratio < keep_cache_hit_ratio)
            add_reason(plan, "shadow-scalability.cache-reuse-degraded", "/cache/hitsAndMisses",
                       "Cache reuse is below the 90 percent stability target.");
        if (plan.shadow_time_ratio > keep_time_ratio)
            add_reason(plan, "shadow-scalability.pass-budget-pressure", "/timing/shadowPassMilliseconds",
                       "Shadow timing is above the 70 percent headroom target.");
        if (plan.invalidation_ratio > keep_invalidation_ratio)
            add_reason(plan, "shadow-scalability.invalidation-pressure", "/invalidation/invalidationsLastWindow",
                       "Invalidation churn is above the 10 percent stability target.");
    }
    plan.diagnostics.insert(plan.diagnostics.end(), plan.reasons.begin(), plan.reasons.end());
    return plan;
}

std::string shadow_scalability_policy_canonical_evidence(
    const ShadowScalabilityPlan& plan) {
    const Json output = {
        {"schema", bounded_text(plan.schema)},
        {"recommendation", shadow_scalability_recommendation_name(plan.recommendation)},
        {"valid", plan.valid},
        {"evidenceComplete", plan.evidence_complete},
        {"code", bounded_text(plan.code)},
        {"detail", bounded_text(plan.detail)},
        {"conservativeAtlasBytes", plan.conservative_atlas_bytes},
        {"atlasBudgetBytes", plan.atlas_budget_bytes},
        {"atlasHeadroomBytes", plan.atlas_headroom_bytes},
        {"ratios", {
            {"cacheHit", finite_json(plan.cache_hit_ratio)},
            {"shadowTime", finite_json(plan.shadow_time_ratio)},
            {"invalidation", finite_json(plan.invalidation_ratio)},
        }},
        {"input", input_json(plan.input)},
        {"reasons", diagnostics_json(plan.reasons)},
        {"missingEvidence", diagnostics_json(plan.missing_evidence)},
        {"diagnostics", diagnostics_json(plan.diagnostics)},
    };
    return output.dump(2) + "\n";
}

std::string shadow_scalability_policy_fingerprint(
    const ShadowScalabilityPlan& plan) {
    return "fnv1a64:" + hex_u64(
        fnv1a(shadow_scalability_policy_canonical_evidence(plan)));
}

} // namespace noemancer
