#include "engine/gpu_occlusion_policy.hpp"

#include <cmath>
#include <iostream>
#include <limits>
#include <string>
#include <string_view>

namespace {

using namespace noemancer;

bool check(const bool condition, const std::string_view message) {
    if (!condition) std::cerr << "gpu_occlusion_policy_tests: " << message << '\n';
    return condition;
}

bool has_code(const std::vector<GpuOcclusionDiagnostic>& diagnostics,
              const std::string_view code) {
    for (const auto& diagnostic : diagnostics)
        if (diagnostic.code == code) return true;
    return false;
}

GpuOcclusionQuery query(const float nearest_depth, const float hiz_max_depth) {
    GpuOcclusionQuery result;
    result.projection = GpuOcclusionProjectionBounds{
        .valid = true,
        .min_x = 0.25F,
        .min_y = 0.25F,
        .max_x = 0.75F,
        .max_y = 0.75F,
        .nearest_depth = nearest_depth,
        .farthest_depth = nearest_depth + 1.0F,
    };
    result.hiz = GpuOcclusionHiZSample{
        .available = true,
        .resource_id = std::string(gpu_occlusion_policy_hiz_resource_id),
        .covers_bounds = true,
        .mip_level = 4U,
        .mip_count = 12U,
        .sample_count = 4U,
        .min_depth = hiz_max_depth,
        .max_depth = hiz_max_depth,
    };
    result.camera = GpuOcclusionCameraState{
        .identity = "camera.main",
        .revision = 1U,
    };
    return result;
}

bool test_vocabulary_and_quality_defaults() {
    if (!check(gpu_occlusion_policy_schema ==
                   "noemancer.gpu-occlusion-policy/0.1",
               "schema drifted")) return false;
    if (!check(gpu_occlusion_quality_name(GpuOcclusionQuality::high) == "high" &&
                   gpu_occlusion_quality_from_string("medium") ==
                       GpuOcclusionQuality::medium &&
                   !gpu_occlusion_quality_from_string("ultra").has_value(),
               "quality vocabulary is not stable")) return false;
    if (!check(gpu_occlusion_decision_name(GpuOcclusionDecision::conservative_unknown) ==
                   "conservative-unknown" &&
                   !gpu_occlusion_decision_culls(
                       GpuOcclusionDecision::conservative_unknown),
               "decision safety boundary drifted")) return false;

    const auto off = gpu_occlusion_quality_defaults(GpuOcclusionQuality::off);
    const auto low = gpu_occlusion_quality_defaults(GpuOcclusionQuality::low);
    const auto medium = gpu_occlusion_quality_defaults(GpuOcclusionQuality::medium);
    const auto high = gpu_occlusion_quality_defaults(GpuOcclusionQuality::high);
    if (!check(!off.enabled && low.enabled && medium.enabled && high.enabled &&
                   low.max_mip < medium.max_mip && medium.max_mip < high.max_mip &&
                   low.occluded_frames_to_cull <= high.occluded_frames_to_cull,
               "quality defaults are not ordered and bounded")) return false;
    return check(validate_gpu_occlusion_policy(off).empty() &&
                     validate_gpu_occlusion_policy(low).empty() &&
                     validate_gpu_occlusion_policy(medium).empty() &&
                     validate_gpu_occlusion_policy(high).empty(),
                 "quality defaults failed validation");
}

bool test_visible_and_occluded_hysteresis() {
    auto config = gpu_occlusion_quality_defaults(GpuOcclusionQuality::high);
    auto occluded = query(10.0F, 2.0F);
    auto first = build_gpu_occlusion_plan(config, occluded);
    if (!check(first.valid && first.enabled &&
                   first.raw_decision == GpuOcclusionDecision::occluded &&
                   first.decision == GpuOcclusionDecision::conservative_unknown &&
                   first.draw_visible && first.hysteresis_applied,
               "first positive HiZ sample was not held draw-visible")) return false;

    occluded.history = first.next_history;
    const auto second = build_gpu_occlusion_plan(config, occluded);
    if (!check(second.decision == GpuOcclusionDecision::conservative_unknown &&
                   second.draw_visible && second.next_history.occluded_streak == 2U,
               "second positive HiZ sample did not advance hysteresis")) return false;

    occluded.history = second.next_history;
    const auto third = build_gpu_occlusion_plan(config, occluded);
    if (!check(third.decision == GpuOcclusionDecision::occluded &&
                   !third.draw_visible && gpu_occlusion_decision_culls(third.decision),
               "stable positive HiZ samples did not produce a cull")) return false;

    config.visible_frames_to_release = 3U;
    auto visible = query(2.0F, 10.0F);
    visible.history = third.next_history;
    const auto release_first = build_gpu_occlusion_plan(config, visible);
    if (!check(release_first.decision == GpuOcclusionDecision::conservative_unknown &&
                   release_first.draw_visible && release_first.hysteresis_applied &&
                   release_first.next_history.occluded_streak ==
                       third.next_history.occluded_streak,
               "visible release did not honor the configured hysteresis")) return false;
    visible.history = release_first.next_history;
    const auto release_second = build_gpu_occlusion_plan(config, visible);
    if (!check(release_second.decision == GpuOcclusionDecision::conservative_unknown &&
                   release_second.draw_visible,
               "visible release bypassed a multi-frame threshold")) return false;
    visible.history = release_second.next_history;
    const auto release_third = build_gpu_occlusion_plan(config, visible);
    return check(release_third.decision == GpuOcclusionDecision::visible &&
                     release_third.draw_visible &&
                     release_third.next_history.occluded_streak == 0U,
                 "visible release did not complete deterministically");
}

bool test_conservative_inputs_and_camera_switch() {
    const auto config = gpu_occlusion_quality_defaults(GpuOcclusionQuality::high);

    auto missing_hiz = query(10.0F, 2.0F);
    missing_hiz.hiz.available = false;
    const auto missing = build_gpu_occlusion_plan(config, missing_hiz);
    if (!check(missing.decision == GpuOcclusionDecision::conservative_unknown &&
                   missing.draw_visible &&
                   has_code(missing.diagnostics, "gpu-occlusion.hiz-unavailable"),
               "missing HiZ was not conservative-visible")) return false;

    auto nan_projection = query(10.0F, 2.0F);
    nan_projection.projection.nearest_depth =
        std::numeric_limits<float>::quiet_NaN();
    const auto nan = build_gpu_occlusion_plan(config, nan_projection);
    if (!check(nan.decision == GpuOcclusionDecision::conservative_unknown &&
                   nan.draw_visible &&
                   has_code(nan.diagnostics, "gpu-occlusion.projection-non-finite"),
               "NaN projection was not conservative-visible")) return false;

    auto out_of_bounds = query(10.0F, 2.0F);
    out_of_bounds.projection.min_x = -0.01F;
    const auto outside = build_gpu_occlusion_plan(config, out_of_bounds);
    if (!check(outside.decision == GpuOcclusionDecision::conservative_unknown &&
                   outside.draw_visible &&
                   has_code(outside.diagnostics, "gpu-occlusion.projection-out-of-bounds"),
               "out-of-bounds projection was not conservative-visible")) return false;

    auto switched = query(10.0F, 2.0F);
    switched.history.valid = true;
    switched.history.camera_identity = "camera.main";
    switched.history.camera_revision = 1U;
    switched.history.occluded_streak = 7U;
    switched.camera.revision = 2U;
    const auto camera_switch = build_gpu_occlusion_plan(config, switched);
    if (!check(camera_switch.camera_switched &&
                   camera_switch.decision == GpuOcclusionDecision::conservative_unknown &&
                   camera_switch.draw_visible && camera_switch.next_history.occluded_streak == 0U &&
                   has_code(camera_switch.diagnostics, "gpu-occlusion.camera-switch-reset"),
               "camera switch did not reset occlusion hysteresis")) return false;

    switched.camera.revision = 1U;
    switched.camera.cut = true;
    const auto camera_cut = build_gpu_occlusion_plan(config, switched);
    return check(camera_cut.camera_switched &&
                     camera_cut.decision == GpuOcclusionDecision::conservative_unknown &&
                     camera_cut.draw_visible,
                 "camera cut was not conservative-visible");
}

bool test_invalid_policy_and_hiz_contract() {
    auto invalid = gpu_occlusion_quality_defaults(GpuOcclusionQuality::high);
    invalid.schema = "wrong";
    invalid.max_mip = 17U;
    invalid.depth_bias = std::numeric_limits<float>::quiet_NaN();
    invalid.occluded_frames_to_cull = 0U;
    invalid.visible_frames_to_release = 9U;
    const auto diagnostics = validate_gpu_occlusion_policy(invalid);
    if (!check(diagnostics.size() >= 5U &&
                   has_code(diagnostics, "gpu-occlusion.unsupported-schema") &&
                   has_code(diagnostics, "gpu-occlusion.max-mip-range") &&
                   has_code(diagnostics, "gpu-occlusion.float-range") &&
                   has_code(diagnostics, "gpu-occlusion.occluded-hysteresis-range") &&
                   has_code(diagnostics, "gpu-occlusion.visible-hysteresis-range"),
               "invalid policy values were not bounded by diagnostics")) return false;
    const auto invalid_plan = build_gpu_occlusion_plan(invalid, query(10.0F, 2.0F));
    if (!check(!invalid_plan.valid && invalid_plan.draw_visible &&
                   invalid_plan.decision == GpuOcclusionDecision::conservative_unknown,
               "invalid policy was allowed to make a cull decision")) return false;

    auto wrong_resource = query(10.0F, 2.0F);
    wrong_resource.hiz.resource_id = "other-depth-pyramid";
    const auto wrong = build_gpu_occlusion_plan(
        gpu_occlusion_quality_defaults(GpuOcclusionQuality::high), wrong_resource);
    if (!check(wrong.decision == GpuOcclusionDecision::conservative_unknown &&
                   wrong.draw_visible &&
                   has_code(wrong.diagnostics, "gpu-occlusion.hiz-resource-mismatch"),
               "wrong HiZ resource was not rejected conservatively")) return false;

    auto bad_mip = query(10.0F, 2.0F);
    bad_mip.hiz.mip_level = 12U;
    bad_mip.hiz.mip_count = 4U;
    const auto mip = build_gpu_occlusion_plan(
        gpu_occlusion_quality_defaults(GpuOcclusionQuality::high), bad_mip);
    return check(mip.decision == GpuOcclusionDecision::conservative_unknown &&
                     mip.draw_visible &&
                     has_code(mip.diagnostics, "gpu-occlusion.hiz-mip-invalid"),
                 "invalid HiZ mip was not rejected conservatively");
}

bool test_canonical_evidence_and_fingerprint() {
    auto config = gpu_occlusion_quality_defaults(GpuOcclusionQuality::high);
    config.profile_id.assign(2'000U, 'p');
    auto candidate = query(10.0F, 2.0F);
    candidate.camera.identity.assign(2'000U, 'c');
    const auto plan = build_gpu_occlusion_plan(config, candidate);
    const auto evidence = gpu_occlusion_policy_canonical_evidence(plan);
    const auto canonical_config = gpu_occlusion_policy_canonical_config(config);
    if (!check(evidence.size() < 64U * 1024U && canonical_config.size() < 8U * 1024U &&
                   evidence.find(std::string(513U, 'c')) == std::string::npos &&
                   canonical_config.find(std::string(513U, 'p')) == std::string::npos,
               "canonical occlusion evidence was not bounded")) return false;
    const auto same = build_gpu_occlusion_plan(config, candidate);
    if (!check(gpu_occlusion_policy_fingerprint(plan) ==
                   gpu_occlusion_policy_fingerprint(same),
               "equivalent occlusion plans did not fingerprint identically")) return false;
    auto changed = config;
    changed.depth_bias += 0.01F;
    const auto changed_plan = build_gpu_occlusion_plan(changed, candidate);
    if (!check(gpu_occlusion_policy_fingerprint(plan) !=
                   gpu_occlusion_policy_fingerprint(changed_plan),
               "semantic occlusion change did not alter fingerprint")) return false;
    return check(evidence.find("conservative-unknown") != std::string::npos &&
                     evidence.find("drawVisible") != std::string::npos,
                 "canonical evidence omitted the safety decision");
}

} // namespace

int main() {
    if (!test_vocabulary_and_quality_defaults()) return 1;
    if (!test_visible_and_occluded_hysteresis()) return 2;
    if (!test_conservative_inputs_and_camera_switch()) return 3;
    if (!test_invalid_policy_and_hiz_contract()) return 4;
    if (!test_canonical_evidence_and_fingerprint()) return 5;
    std::cout << "gpu_occlusion_policy_tests: ok\n";
    return 0;
}
