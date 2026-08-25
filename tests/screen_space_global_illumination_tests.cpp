#include "engine/screen_space_global_illumination.hpp"

#include <iostream>
#include <nlohmann/json.hpp>
#include <string_view>

namespace {

int fail(const char* message) {
    std::cerr << "screen_space_global_illumination_tests: " << message << '\n';
    return 1;
}

bool has_code(
    const std::vector<noemancer::ScreenSpaceGlobalIlluminationDiagnostic>& diagnostics,
    const std::string_view code) {
    for (const auto& diagnostic : diagnostics) {
        if (diagnostic.code == code) return true;
    }
    return false;
}

} // namespace

int main() {
    using namespace noemancer;

    if (screen_space_global_illumination_schema !=
            "noemancer.screen-space-global-illumination/0.1" ||
        screen_space_global_illumination_quality_name(
            ScreenSpaceGlobalIlluminationQuality::high) != "high" ||
        screen_space_global_illumination_quality_from_string("medium") !=
            ScreenSpaceGlobalIlluminationQuality::medium ||
        screen_space_global_illumination_quality_from_string("ultra").has_value() ||
        screen_space_global_illumination_hybrid_pixel_policy_from_string(
            "spatial-only-no-history") !=
            ScreenSpaceGlobalIlluminationHybridPixelPolicy::spatial_only) {
        return fail("stable SSGI vocabulary drifted");
    }

    const auto low = screen_space_global_illumination_quality_defaults(
        ScreenSpaceGlobalIlluminationQuality::low);
    const auto medium = screen_space_global_illumination_quality_defaults(
        ScreenSpaceGlobalIlluminationQuality::medium);
    const auto high = screen_space_global_illumination_quality_defaults(
        ScreenSpaceGlobalIlluminationQuality::high);
    if (!low.enabled || !medium.enabled || !high.enabled ||
        low.sampling.sample_count >= medium.sampling.sample_count ||
        medium.sampling.sample_count >= high.sampling.sample_count ||
        low.sampling.max_steps >= medium.sampling.max_steps ||
        medium.sampling.max_steps >= high.sampling.max_steps) {
        return fail("SSGI quality defaults are not ordered or enabled");
    }

    const auto high_plan = build_screen_space_global_illumination_plan(high);
    if (!high_plan.valid || !high_plan.config_valid || !high_plan.enabled ||
        !high_plan.hierarchical_depth_required || !high_plan.bent_normal_output ||
        !high_plan.visibility_output || !high_plan.history.required ||
        high_plan.history.use_previous || !high_plan.history.reset_required ||
        high_plan.history.reset_reason != "first-frame" ||
        high_plan.code != "ssgi.ready-with-history-fallback") {
        return fail("high SSGI plan did not expose first-frame history fallback");
    }

    const auto high_evidence = screen_space_global_illumination_canonical_evidence(high_plan);
    const auto high_json = nlohmann::json::parse(high_evidence);
    if (high_json.at("quality") != "high" || !high_json.at("enabled").get<bool>() ||
        high_json.at("sampleCount") != high.sampling.sample_count ||
        high_json.at("radius") != high.sampling.radius ||
        high_json.at("thickness") != high.sampling.thickness ||
        high_json.at("intensity") != high.sampling.intensity ||
        high_json.at("material").at("contract") !=
            "normal.rgb+roughness.a+baseColor.rgb+metallic.a" ||
        high_json.at("bentNormal").at("semantics") !=
            "visibility-weighted-hemisphere-normal" ||
        high_json.at("visibility").at("semantics") !=
            "confidence-weighted-ambient-visibility" ||
        high_json.at("history").at("fallback") != "spatial-current-frame" ||
        high_json.at("composition") != "replace-IBL-diffuse-by-confidence" ||
        high_json.at("fallback") != "retain-ibl-diffuse") {
        return fail("canonical SSGI evidence omitted semantic fields");
    }

    auto consecutive_history = high_plan;
    ScreenSpaceGlobalIlluminationHistoryInput history;
    history.previous_valid = true;
    consecutive_history = build_screen_space_global_illumination_plan(
        high, false, {}, history);
    if (!consecutive_history.valid || !consecutive_history.enabled ||
        !consecutive_history.history.use_previous ||
        consecutive_history.history.reset_required ||
        consecutive_history.history.reset_reason != "none" ||
        consecutive_history.code != "ssgi.ready") {
        return fail("compatible SSGI history was not reused");
    }

    history.camera_cut = true;
    const auto camera_cut = build_screen_space_global_illumination_plan(
        high, false, {}, history);
    if (!camera_cut.valid || camera_cut.history.use_previous ||
        !camera_cut.history.reset_required ||
        camera_cut.history.reset_reason != "camera-cut" ||
        camera_cut.code != "ssgi.ready-with-history-fallback") {
        return fail("camera cut did not reset SSGI history");
    }

    const auto hybrid_disabled = build_screen_space_global_illumination_plan(high, true);
    if (!hybrid_disabled.valid || hybrid_disabled.enabled ||
        !hybrid_disabled.disabled_by_hybrid_pixel || !hybrid_disabled.fallback_only ||
        hybrid_disabled.history.required || hybrid_disabled.code != "ssgi.hybrid-pixel-disabled") {
        return fail("Hybrid Pixel SSGI disable policy did not take effect");
    }

    auto spatial = high;
    spatial.hybrid_pixel_policy =
        ScreenSpaceGlobalIlluminationHybridPixelPolicy::spatial_only;
    const auto spatial_plan = build_screen_space_global_illumination_plan(spatial, true);
    if (!spatial_plan.valid || !spatial_plan.enabled || spatial_plan.history.required ||
        !spatial_plan.history.fallback_to_current_frame ||
        spatial_plan.code != "ssgi.ready-spatial-only") {
        return fail("Hybrid Pixel spatial-only SSGI policy was not bounded");
    }

    ScreenSpaceGlobalIlluminationInputAvailability missing;
    missing.normal_buffer_ready = false;
    const auto missing_plan = build_screen_space_global_illumination_plan(high, false, missing);
    if (!missing_plan.valid || missing_plan.enabled || !missing_plan.fallback_only ||
        missing_plan.code != "ssgi.inputs-unavailable" ||
        !has_code(missing_plan.diagnostics, "ssgi.normal-buffer-unavailable")) {
        return fail("missing SSGI normal input did not select fallback");
    }

    ScreenSpaceGlobalIlluminationInputAvailability missing_history;
    missing_history.history_target_ready = false;
    const auto history_fallback = build_screen_space_global_illumination_plan(
        high, false, missing_history, history);
    if (!history_fallback.valid || !history_fallback.enabled ||
        history_fallback.history.use_previous ||
        !history_fallback.history.fallback_to_current_frame ||
        history_fallback.history.reset_reason != "history-target-unavailable" ||
        !has_code(history_fallback.diagnostics, "ssgi.history-target-unavailable")) {
        return fail("missing SSGI history target did not retain spatial fallback");
    }

    auto invalid = high;
    invalid.sampling.sample_count = screen_space_global_illumination_max_samples + 1U;
    invalid.sampling.thickness = -1.0F;
    invalid.material.roughness_cutoff = 0.1F;
    invalid.material.minimum_roughness = 0.2F;
    invalid.bent_normal.semantics = "unknown";
    invalid.visibility.semantics = "unknown";
    invalid.history.policy = "unknown";
    invalid.composition.strategy = "unknown";
    const auto invalid_diagnostics = validate_screen_space_global_illumination(invalid);
    if (invalid_diagnostics.empty() ||
        !has_code(invalid_diagnostics, "ssgi.sample-count-range") ||
        !has_code(invalid_diagnostics, "ssgi.roughness-order") ||
        !has_code(invalid_diagnostics, "ssgi.bent-normal-semantics-unsupported") ||
        !has_code(invalid_diagnostics, "ssgi.visibility-semantics-unsupported") ||
        !has_code(invalid_diagnostics, "ssgi.history-policy-unsupported") ||
        !has_code(invalid_diagnostics, "ssgi.composition-unsupported")) {
        return fail("invalid SSGI values were not bounded by diagnostics");
    }
    const auto invalid_plan = build_screen_space_global_illumination_plan(invalid);
    if (invalid_plan.valid || invalid_plan.enabled || invalid_plan.code != "ssgi.invalid-config") {
        return fail("invalid SSGI config produced an executable plan");
    }

    auto mismatched_directions = high;
    mismatched_directions.sampling.directions =
        mismatched_directions.sampling.sample_count - 1U;
    const auto direction_diagnostics =
        validate_screen_space_global_illumination(mismatched_directions);
    if (!has_code(direction_diagnostics, "ssgi.direction-count-mismatch") ||
        build_screen_space_global_illumination_plan(mismatched_directions).valid) {
        return fail("SSGI directions can diverge from the executed sample budget");
    }

    invalid.profile_id.assign(2'000U, 'p');
    invalid.material.contract.assign(2'000U, 'm');
    invalid.bent_normal.semantics.assign(2'000U, 'b');
    invalid.history.policy.assign(2'000U, 'h');
    const auto bounded_plan = build_screen_space_global_illumination_plan(invalid);
    const auto bounded_evidence =
        screen_space_global_illumination_canonical_evidence(bounded_plan);
    const auto bounded_config = screen_space_global_illumination_canonical_config(invalid);
    if (bounded_evidence.size() > 64U * 1024U || bounded_config.size() > 64U * 1024U ||
        bounded_evidence.find(std::string(513U, 'm')) != std::string::npos ||
        bounded_config.find(std::string(513U, 'm')) != std::string::npos) {
        return fail("canonical SSGI projections are not bounded");
    }

    auto changed = high;
    changed.sampling.sample_count += 1U;
    if (screen_space_global_illumination_fingerprint(high_plan) ==
        screen_space_global_illumination_fingerprint(
            build_screen_space_global_illumination_plan(changed))) {
        return fail("SSGI fingerprint did not change with a semantic config change");
    }

    const auto off = screen_space_global_illumination_quality_defaults(
        ScreenSpaceGlobalIlluminationQuality::off);
    if (off.enabled || !validate_screen_space_global_illumination(off).empty()) {
        return fail("SSGI Off defaults are not an explicit disabled configuration");
    }
    const auto off_plan = build_screen_space_global_illumination_plan(off);
    if (!off_plan.valid || off_plan.enabled || !off_plan.fallback_only ||
        off_plan.code != "ssgi.off") {
        return fail("SSGI Off plan did not preserve IBL diffuse fallback");
    }

    const auto canonical_config = screen_space_global_illumination_canonical_config(high);
    if (canonical_config.size() > 16U * 1024U ||
        nlohmann::json::parse(canonical_config).at("schema") !=
            "noemancer.screen-space-global-illumination/0.1") {
        return fail("canonical SSGI config is not bounded JSON");
    }

    std::cout << "screen_space_global_illumination_tests: ok\n";
    return 0;
}
