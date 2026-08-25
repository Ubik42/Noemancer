#include "engine/screen_space_reflections.hpp"

#include <iostream>
#include <nlohmann/json.hpp>
#include <string_view>

namespace {

int fail(const char* message) {
    std::cerr << "screen_space_reflections_tests: " << message << '\n';
    return 1;
}

bool has_code(const std::vector<noemancer::ScreenSpaceReflectionsDiagnostic>& diagnostics,
             const std::string_view code) {
    for (const auto& diagnostic : diagnostics) {
        if (diagnostic.code == code) return true;
    }
    return false;
}

} // namespace

int main() {
    using namespace noemancer;

    if (screen_space_reflections_schema !=
            "noemancer.screen-space-reflections/0.1" ||
        screen_space_reflections_material_contract != "F0.rgb+roughness.a" ||
        screen_space_reflections_quality_name(ScreenSpaceReflectionsQuality::high) !=
            "high" ||
        screen_space_reflections_quality_from_string("medium") !=
            ScreenSpaceReflectionsQuality::medium ||
        screen_space_reflections_quality_from_string("ultra").has_value() ||
        screen_space_reflections_hybrid_pixel_policy_from_string(
            "spatial-only-no-history") !=
            ScreenSpaceReflectionsHybridPixelPolicy::spatial_only) {
        return fail("stable vocabulary drifted");
    }

    const auto low = screen_space_reflections_quality_defaults(
        ScreenSpaceReflectionsQuality::low);
    const auto medium = screen_space_reflections_quality_defaults(
        ScreenSpaceReflectionsQuality::medium);
    const auto high = screen_space_reflections_quality_defaults(
        ScreenSpaceReflectionsQuality::high);
    if (!low.enabled || !medium.enabled || !high.enabled ||
        low.ray_march.max_steps >= medium.ray_march.max_steps ||
        medium.ray_march.max_steps >= high.ray_march.max_steps ||
        low.material.roughness_cutoff >= medium.material.roughness_cutoff ||
        medium.material.roughness_cutoff >= high.material.roughness_cutoff) {
        return fail("quality defaults are not ordered or enabled");
    }

    const auto high_plan = build_screen_space_reflections_plan(high);
    if (!high_plan.valid || !high_plan.config_valid || !high_plan.enabled ||
        !high_plan.hierarchical_depth_required ||
        !high_plan.temporal_history_required || high_plan.fallback_only ||
        high_plan.code != "ssr.ready") {
        return fail("high quality plan did not enable the production path");
    }

    const auto high_evidence = screen_space_reflections_canonical_evidence(high_plan);
    const auto high_json = nlohmann::json::parse(high_evidence);
    if (high_json.at("quality") != "high" || !high_json.at("enabled").get<bool>() ||
        high_json.at("rayMarch").at("maxSteps") != high.ray_march.max_steps ||
        high_json.at("rayMarch").at("startMip") != high.ray_march.start_mip ||
        high_json.at("material").at("contract") != "F0.rgb+roughness.a" ||
        high_json.at("material").at("roughnessCutoff") != high.material.roughness_cutoff ||
        high_json.at("composition") != "replace-ibl-specular-by-confidence" ||
        high_json.at("fallback") != "retain-ibl-specular" ||
        high_json.at("hybridPixelPolicy") != "disabled" ||
        high_json.at("historyWeight") != high.composition.history_weight) {
        return fail("canonical evidence omitted the SSR semantic contract");
    }
    if (screen_space_reflections_fingerprint(high_plan) !=
        screen_space_reflections_fingerprint(build_screen_space_reflections_plan(high))) {
        return fail("identical plans do not have a deterministic fingerprint");
    }

    auto off = screen_space_reflections_quality_defaults(
        ScreenSpaceReflectionsQuality::off);
    if (off.enabled || !validate_screen_space_reflections(off).empty()) {
        return fail("Off defaults are not an explicit disabled configuration");
    }
    const auto off_plan = build_screen_space_reflections_plan(off);
    if (!off_plan.valid || off_plan.enabled || !off_plan.fallback_only ||
        off_plan.code != "ssr.off") {
        return fail("Off plan did not preserve the IBL fallback");
    }

    const auto hybrid_disabled = build_screen_space_reflections_plan(high, true);
    if (!hybrid_disabled.valid || hybrid_disabled.enabled ||
        !hybrid_disabled.disabled_by_hybrid_pixel || !hybrid_disabled.fallback_only ||
        hybrid_disabled.temporal_history_required ||
        hybrid_disabled.code != "ssr.hybrid-pixel-disabled") {
        return fail("Hybrid Pixel disable policy did not take effect");
    }
    auto spatial_config = high;
    spatial_config.hybrid_pixel_policy =
        ScreenSpaceReflectionsHybridPixelPolicy::spatial_only;
    const auto spatial_plan = build_screen_space_reflections_plan(spatial_config, true);
    if (!spatial_plan.valid || !spatial_plan.enabled ||
        spatial_plan.temporal_history_required ||
        spatial_plan.code != "ssr.ready-spatial-only") {
        return fail("Hybrid Pixel spatial-only policy was not bounded");
    }

    ScreenSpaceReflectionsInputAvailability missing;
    missing.depth_pyramid_ready = false;
    const auto missing_plan = build_screen_space_reflections_plan(high, false, missing);
    if (!missing_plan.valid || missing_plan.enabled || !missing_plan.fallback_only ||
        missing_plan.code != "ssr.inputs-unavailable" ||
        !has_code(missing_plan.diagnostics, "ssr.depth-pyramid-unavailable")) {
        return fail("missing hierarchical depth did not select fallback");
    }

    auto invalid = high;
    invalid.ray_march.max_steps = screen_space_reflections_max_ray_steps + 1U;
    invalid.ray_march.thickness = -1.0F;
    invalid.material.roughness_cutoff = 0.1F;
    invalid.material.minimum_roughness = 0.2F;
    invalid.edge_fade.start = 0.9F;
    invalid.edge_fade.end = 0.2F;
    invalid.composition.strategy = "unknown";
    const auto invalid_diagnostics = validate_screen_space_reflections(invalid);
    if (invalid_diagnostics.empty() ||
        !has_code(invalid_diagnostics, "ssr.max-steps-range") ||
        !has_code(invalid_diagnostics, "ssr.roughness-order") ||
        !has_code(invalid_diagnostics, "ssr.edge-fade-order") ||
        !has_code(invalid_diagnostics, "ssr.composition-unsupported")) {
        return fail("invalid SSR values were not bounded by diagnostics");
    }
    const auto invalid_plan = build_screen_space_reflections_plan(invalid);
    if (invalid_plan.valid || invalid_plan.enabled || invalid_plan.code != "ssr.invalid-config") {
        return fail("invalid SSR config produced an executable plan");
    }

    // Agent evidence remains bounded even when a malformed authoring payload
    // contains oversized strings.  The diagnostic still reports the contract
    // failure, while the projection cannot become an unbounded log channel.
    invalid.profile_id.assign(2'000U, 'p');
    invalid.material.contract.assign(2'000U, 'm');
    invalid.composition.strategy.assign(2'000U, 's');
    invalid.composition.fallback.assign(2'000U, 'f');
    const auto bounded_plan = build_screen_space_reflections_plan(invalid);
    const auto bounded_evidence = screen_space_reflections_canonical_evidence(bounded_plan);
    const auto bounded_config = screen_space_reflections_canonical_config(invalid);
    if (bounded_evidence.size() > 64U * 1024U || bounded_config.size() > 64U * 1024U ||
        bounded_evidence.find(std::string(513U, 'm')) != std::string::npos ||
        bounded_config.find(std::string(513U, 'm')) != std::string::npos) {
        return fail("canonical SSR projections are not bounded");
    }

    auto changed = high;
    changed.ray_march.max_steps += 1U;
    if (screen_space_reflections_fingerprint(high_plan) ==
        screen_space_reflections_fingerprint(build_screen_space_reflections_plan(changed))) {
        return fail("fingerprint did not change with a semantic config change");
    }

    const auto canonical_config = screen_space_reflections_canonical_config(high);
    if (canonical_config.size() > 16U * 1024U ||
        nlohmann::json::parse(canonical_config).at("schema") !=
            "noemancer.screen-space-reflections/0.1") {
        return fail("canonical config is not bounded JSON");
    }

    std::cout << "screen_space_reflections_tests: ok\n";
    return 0;
}
