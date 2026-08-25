#include "engine/shadow_scalability_policy.hpp"

#include <cmath>
#include <iostream>
#include <limits>
#include <string>
#include <string_view>

namespace {

using namespace noemancer;

bool check(const bool condition, const std::string_view message) {
    if (!condition) std::cerr << "shadow_scalability_policy_tests: " << message << '\n';
    return condition;
}

ShadowScalabilityInput complete_input() {
    ShadowScalabilityInput input;
    input.workload = ShadowScalabilityWorkload{
        .schema = std::string(shadow_scalability_policy_schema),
        .directional_enabled = true,
        .cascade_count = 4U,
        .cascade_resolution = 2048U,
        .local_enabled = true,
        .local_layer_count = 8U,
        .local_resolution = 1024U,
        .requested_local_lights = 2U,
        .selected_local_lights = 2U,
        .dropped_local_lights = 0U,
        .estimated_atlas_bytes = 100U * 1024U * 1024U,
    };
    input.cache = ShadowScalabilityCacheObservation{
        .available = true,
        .directional_cascades_available = 4U,
        .directional_cascades_cached = 4U,
        .directional_cache_hits = 90U,
        .directional_cache_misses = 10U,
        .local_faces_available = 8U,
        .local_faces_cached = 8U,
        .local_cache_hits = 90U,
        .local_cache_misses = 10U,
    };
    input.geometry = ShadowScalabilityGeometryObservation{
        .available = true,
        .caster_count = 256U,
        .primitive_count = 4096U,
        .draw_count = 32U,
        .instances_submitted = 256U,
        .draw_calls_saved = 224U,
    };
    input.timing = ShadowScalabilityTimingObservation{
        .available = true,
        .shadow_pass_milliseconds = 5.0,
        .directional_pass_milliseconds = 3.0,
        .local_pass_milliseconds = 2.0,
        .frame_budget_milliseconds = 16.6666667,
    };
    input.invalidation = ShadowScalabilityInvalidationObservation{
        .available = true,
        .camera_revision = 7U,
        .light_revision = 4U,
        .caster_revision = 11U,
        .camera_invalidated = false,
        .light_invalidated = false,
        .caster_invalidated = false,
        .invalidations_last_window = 5U,
        .observation_frames = 100U,
    };
    input.atlas_budget_bytes = 200U * 1024U * 1024U;
    return input;
}

bool test_vocabulary_and_healthy_keep() {
    if (!check(shadow_scalability_policy_schema ==
                   "noemancer.shadow-scalability-policy/0.1",
               "schema drifted")) return false;
    if (!check(shadow_scalability_recommendation_name(
                   ShadowScalabilityRecommendation::keep_atlas) == "keep-atlas" &&
                   shadow_scalability_recommendation_name(
                       ShadowScalabilityRecommendation::extend_atlas) == "extend-atlas" &&
                   shadow_scalability_recommendation_name(
                       ShadowScalabilityRecommendation::prototype_virtual_pages) ==
                       "prototype-virtual-pages" &&
                   shadow_scalability_recommendation_name(
                       ShadowScalabilityRecommendation::insufficient_evidence) ==
                       "insufficient-evidence",
               "recommendation vocabulary drifted")) return false;

    const auto input = complete_input();
    const auto plan = evaluate_shadow_scalability(input);
    if (!check(plan.valid && plan.evidence_complete &&
                   plan.recommendation == ShadowScalabilityRecommendation::keep_atlas &&
                   plan.code == "shadow-scalability.keep-atlas" &&
                   plan.conservative_atlas_bytes == 110U * 1024U * 1024U &&
                   plan.atlas_headroom_bytes == 90U * 1024U * 1024U,
               "healthy atlas workload did not remain on the atlas path")) return false;
    return check(plan.reasons.size() == 3U && plan.missing_evidence.empty(),
                 "healthy recommendation did not expose bounded reasons");
}

bool test_threshold_boundaries() {
    auto boundary = complete_input();
    boundary.workload.estimated_atlas_bytes = 127U * 1024U * 1024U;
    boundary.atlas_budget_bytes = 200U * 1024U * 1024U;
    boundary.cache.directional_cache_hits = 90U;
    boundary.cache.directional_cache_misses = 10U;
    boundary.cache.local_cache_hits = 90U;
    boundary.cache.local_cache_misses = 10U;
    boundary.timing.shadow_pass_milliseconds = boundary.timing.frame_budget_milliseconds * 0.70;
    boundary.invalidation.invalidations_last_window = 10U;
    const auto keep = evaluate_shadow_scalability(boundary);
    if (!check(keep.recommendation == ShadowScalabilityRecommendation::keep_atlas,
               "inclusive keep thresholds did not keep the atlas")) return false;

    auto extend = boundary;
    extend.workload.estimated_atlas_bytes = 128U * 1024U * 1024U;
    const auto extension = evaluate_shadow_scalability(extend);
    if (!check(extension.recommendation == ShadowScalabilityRecommendation::extend_atlas &&
                   extension.valid,
               "tight but fitting atlas did not request extension")) return false;

    auto dropped = complete_input();
    dropped.workload.dropped_local_lights = 1U;
    dropped.workload.selected_local_lights = 1U;
    const auto dropped_extension = evaluate_shadow_scalability(dropped);
    if (!check(dropped_extension.recommendation ==
                   ShadowScalabilityRecommendation::extend_atlas,
               "dropped lights with atlas headroom skipped the bounded extension decision")) return false;

    auto prototype = dropped;
    prototype.workload.estimated_atlas_bytes = 190U * 1024U * 1024U;
    prototype.atlas_budget_bytes = 200U * 1024U * 1024U;
    const auto virtual_pages = evaluate_shadow_scalability(prototype);
    return check(virtual_pages.recommendation ==
                     ShadowScalabilityRecommendation::prototype_virtual_pages &&
                     virtual_pages.code == "shadow-scalability.prototype-virtual-pages",
                 "over-budget local-shadow pressure did not trigger a virtual-page prototype");
}

bool test_missing_evidence_never_misdiagnoses() {
    auto missing = complete_input();
    missing.cache.available = false;
    missing.geometry.available = false;
    missing.timing.available = false;
    missing.invalidation.available = false;
    const auto plan = evaluate_shadow_scalability(missing);
    if (!check(!plan.valid && !plan.evidence_complete &&
                   plan.recommendation == ShadowScalabilityRecommendation::insufficient_evidence &&
                   plan.code == "shadow-scalability.invalid-input",
               "missing evidence was converted into a capacity recommendation")) return false;
    const auto evidence = shadow_scalability_policy_canonical_evidence(plan);
    return check(evidence.find("insufficient-evidence") != std::string::npos &&
                     evidence.find("cache-invalid") != std::string::npos &&
                     !shadow_scalability_policy_fingerprint(plan).empty(),
                 "missing evidence receipt omitted the conservative outcome");
}

bool test_invalid_inputs_and_deterministic_fingerprint() {
    auto invalid = complete_input();
    invalid.workload.selected_local_lights = 3U;
    invalid.workload.dropped_local_lights = 0U;
    invalid.timing.shadow_pass_milliseconds = std::numeric_limits<double>::quiet_NaN();
    const auto plan = evaluate_shadow_scalability(invalid);
    if (!check(!plan.valid &&
                   plan.recommendation == ShadowScalabilityRecommendation::insufficient_evidence &&
                   !plan.diagnostics.empty(),
               "invalid workload facts were not rejected")) return false;

    const auto same = evaluate_shadow_scalability(complete_input());
    const auto same_again = evaluate_shadow_scalability(complete_input());
    if (!check(shadow_scalability_policy_fingerprint(same) ==
                   shadow_scalability_policy_fingerprint(same_again) &&
                   shadow_scalability_policy_canonical_evidence(same) ==
                       shadow_scalability_policy_canonical_evidence(same_again),
               "equivalent observations were not deterministic")) return false;
    auto changed = complete_input();
    changed.timing.shadow_pass_milliseconds = 12.0;
    const auto changed_plan = evaluate_shadow_scalability(changed);
    if (!check(shadow_scalability_policy_fingerprint(same) !=
                   shadow_scalability_policy_fingerprint(changed_plan),
               "timing pressure did not alter the evidence fingerprint")) return false;

    auto oversized = complete_input();
    oversized.schema.assign(2'000U, 's');
    const auto oversized_plan = evaluate_shadow_scalability(oversized);
    const auto oversized_evidence = shadow_scalability_policy_canonical_evidence(oversized_plan);
    return check(oversized_evidence.size() < 64U * 1024U &&
                     oversized_evidence.find(std::string(513U, 's')) == std::string::npos,
                 "canonical shadow evidence exceeded the bounded text contract");
}

} // namespace

int main() {
    if (!test_vocabulary_and_healthy_keep()) return 1;
    if (!test_threshold_boundaries()) return 2;
    if (!test_missing_evidence_never_misdiagnoses()) return 3;
    if (!test_invalid_inputs_and_deterministic_fingerprint()) return 4;
    std::cout << "shadow_scalability_policy_tests: ok\n";
    return 0;
}
