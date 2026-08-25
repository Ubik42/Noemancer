#include "engine/gpu_occlusion_policy.hpp"

#include <algorithm>
#include <cmath>
#include <nlohmann/json.hpp>
#include <utility>

namespace noemancer {
namespace {

using Json = nlohmann::json;

std::string bounded_text(const std::string_view value) {
    return std::string(value.substr(0U, gpu_occlusion_policy_max_text_bytes));
}

bool text_non_empty(const std::string_view value) {
    return !value.empty() && value.size() <= gpu_occlusion_policy_max_text_bytes;
}

void add_diagnostic(std::vector<GpuOcclusionDiagnostic>& diagnostics,
                    std::string code, std::string path, std::string message) {
    if (diagnostics.size() >= gpu_occlusion_policy_max_diagnostics) return;
    diagnostics.push_back(GpuOcclusionDiagnostic{
        .code = bounded_text(code),
        .path = bounded_text(path),
        .message = bounded_text(message),
    });
}

void validate_float(std::vector<GpuOcclusionDiagnostic>& diagnostics,
                    const std::string_view path, const float value,
                    const float minimum, const float maximum,
                    const std::string_view message) {
    if (!std::isfinite(value) || value < minimum || value > maximum)
        add_diagnostic(diagnostics, "gpu-occlusion.float-range", std::string(path),
                       std::string(message));
}

bool finite_depth_interval(const float nearest, const float farthest) {
    return std::isfinite(nearest) && std::isfinite(farthest) &&
        nearest >= 0.0F && farthest >= nearest;
}

Json finite_json(const float value) {
    return std::isfinite(value) ? Json(value) : Json(nullptr);
}

Json projection_json(const GpuOcclusionProjectionBounds& projection) {
    return Json{
        {"valid", projection.valid},
        {"minX", finite_json(projection.min_x)},
        {"minY", finite_json(projection.min_y)},
        {"maxX", finite_json(projection.max_x)},
        {"maxY", finite_json(projection.max_y)},
        {"nearestDepth", finite_json(projection.nearest_depth)},
        {"farthestDepth", finite_json(projection.farthest_depth)},
    };
}

Json hiz_json(const GpuOcclusionHiZSample& hiz) {
    return Json{
        {"available", hiz.available},
        {"resourceId", bounded_text(hiz.resource_id)},
        {"coversBounds", hiz.covers_bounds},
        {"mipLevel", hiz.mip_level},
        {"mipCount", hiz.mip_count},
        {"sampleCount", hiz.sample_count},
        {"minDepth", finite_json(hiz.min_depth)},
        {"maxDepth", finite_json(hiz.max_depth)},
    };
}

Json camera_json(const GpuOcclusionCameraState& camera) {
    return Json{
        {"identity", bounded_text(camera.identity)},
        {"revision", camera.revision},
        {"cut", camera.cut},
        {"projectionChanged", camera.projection_changed},
    };
}

Json history_json(const GpuOcclusionHistory& history) {
    return Json{
        {"valid", history.valid},
        {"cameraIdentity", bounded_text(history.camera_identity)},
        {"cameraRevision", history.camera_revision},
        {"occludedStreak", history.occluded_streak},
        {"visibleStreak", history.visible_streak},
    };
}

Json config_json(const GpuOcclusionConfig& config) {
    return Json{
        {"schema", bounded_text(config.schema)},
        {"profileId", bounded_text(config.profile_id)},
        {"quality", gpu_occlusion_quality_name(config.quality)},
        {"enabled", config.enabled},
        {"maxMip", config.max_mip},
        {"depthBias", finite_json(config.depth_bias)},
        {"occludedFramesToCull", config.occluded_frames_to_cull},
        {"visibleFramesToRelease", config.visible_frames_to_release},
    };
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

GpuOcclusionHistory reset_history_for_camera(const GpuOcclusionCameraState& camera) {
    GpuOcclusionHistory result;
    if (!camera.identity.empty() &&
        camera.identity.size() <= gpu_occlusion_policy_max_text_bytes) {
        result.valid = true;
        result.camera_identity = camera.identity;
        result.camera_revision = camera.revision;
    }
    return result;
}

bool same_camera(const GpuOcclusionHistory& history,
                 const GpuOcclusionCameraState& camera) {
    return history.valid && history.camera_identity == camera.identity &&
        history.camera_revision == camera.revision;
}

void add_input_diagnostic(GpuOcclusionPlan& result, std::string code,
                          std::string path, std::string message) {
    add_diagnostic(result.diagnostics, std::move(code), std::move(path),
                   std::move(message));
}

bool valid_projection(const GpuOcclusionProjectionBounds& projection,
                      GpuOcclusionPlan& result) {
    if (!projection.valid) {
        add_input_diagnostic(result, "gpu-occlusion.projection-invalid", "/projection",
                             "The projected bounds are not marked valid.");
        return false;
    }
    if (!std::isfinite(projection.min_x) || !std::isfinite(projection.min_y) ||
        !std::isfinite(projection.max_x) || !std::isfinite(projection.max_y) ||
        !std::isfinite(projection.nearest_depth) ||
        !std::isfinite(projection.farthest_depth)) {
        add_input_diagnostic(result, "gpu-occlusion.projection-non-finite", "/projection",
                             "Projection bounds and depths must be finite.");
        return false;
    }
    if (projection.min_x < 0.0F || projection.min_y < 0.0F ||
        projection.max_x > 1.0F || projection.max_y > 1.0F) {
        add_input_diagnostic(result, "gpu-occlusion.projection-out-of-bounds",
                             "/projection", "Out-of-viewport bounds are conservative-unknown.");
        return false;
    }
    if (!(projection.min_x < projection.max_x && projection.min_y < projection.max_y)) {
        add_input_diagnostic(result, "gpu-occlusion.projection-degenerate", "/projection",
                             "Projected bounds must have positive area.");
        return false;
    }
    if (!finite_depth_interval(projection.nearest_depth, projection.farthest_depth)) {
        add_input_diagnostic(result, "gpu-occlusion.projection-depth-invalid",
                             "/projection/depth", "Projection depth interval is invalid.");
        return false;
    }
    return true;
}

bool valid_hiz_sample(const GpuOcclusionHiZSample& hiz,
                      const GpuOcclusionConfig& config,
                      GpuOcclusionPlan& result) {
    if (!hiz.available) {
        add_input_diagnostic(result, "gpu-occlusion.hiz-unavailable", "/hiz/available",
                             "The shared HiZ pyramid is unavailable; draw conservatively.");
        return false;
    }
    if (hiz.resource_id != gpu_occlusion_policy_hiz_resource_id) {
        add_input_diagnostic(result, "gpu-occlusion.hiz-resource-mismatch", "/hiz/resourceId",
                             "Occlusion requires the shared scene-depth-pyramid resource.");
        return false;
    }
    if (!hiz.covers_bounds) {
        add_input_diagnostic(result, "gpu-occlusion.hiz-coverage-unknown", "/hiz/coversBounds",
                             "The HiZ sample does not conservatively cover the projected bounds.");
        return false;
    }
    if (hiz.mip_count == 0U || hiz.mip_level >= hiz.mip_count ||
        hiz.mip_level > config.max_mip ||
        hiz.mip_level > gpu_occlusion_policy_max_hiz_mip) {
        add_input_diagnostic(result, "gpu-occlusion.hiz-mip-invalid", "/hiz/mipLevel",
                             "The selected HiZ mip is outside the active bounded chain.");
        return false;
    }
    if (hiz.sample_count == 0U) {
        add_input_diagnostic(result, "gpu-occlusion.hiz-sample-empty", "/hiz/sampleCount",
                             "At least one conservative HiZ sample is required.");
        return false;
    }
    if (!finite_depth_interval(hiz.min_depth, hiz.max_depth)) {
        add_input_diagnostic(result, "gpu-occlusion.hiz-depth-invalid", "/hiz/depth",
                             "HiZ min/max depths must be finite and ordered.");
        return false;
    }
    return true;
}

void finalize_unknown(GpuOcclusionPlan& result, const std::string_view detail) {
    result.raw_decision = GpuOcclusionDecision::conservative_unknown;
    result.decision = GpuOcclusionDecision::conservative_unknown;
    result.draw_visible = true;
    result.code = "gpu-occlusion.conservative-unknown";
    result.detail = std::string(detail);
    result.next_history = reset_history_for_camera(result.query.camera);
}

} // namespace

std::string_view gpu_occlusion_quality_name(const GpuOcclusionQuality quality) noexcept {
    switch (quality) {
    case GpuOcclusionQuality::off: return "off";
    case GpuOcclusionQuality::low: return "low";
    case GpuOcclusionQuality::medium: return "medium";
    case GpuOcclusionQuality::high: return "high";
    }
    return "unknown";
}

std::optional<GpuOcclusionQuality>
gpu_occlusion_quality_from_string(const std::string_view value) noexcept {
    if (value == "off") return GpuOcclusionQuality::off;
    if (value == "low") return GpuOcclusionQuality::low;
    if (value == "medium") return GpuOcclusionQuality::medium;
    if (value == "high") return GpuOcclusionQuality::high;
    return std::nullopt;
}

bool gpu_occlusion_quality_valid(const GpuOcclusionQuality quality) noexcept {
    return quality == GpuOcclusionQuality::off || quality == GpuOcclusionQuality::low ||
        quality == GpuOcclusionQuality::medium || quality == GpuOcclusionQuality::high;
}

std::string_view gpu_occlusion_decision_name(const GpuOcclusionDecision decision) noexcept {
    switch (decision) {
    case GpuOcclusionDecision::visible: return "visible";
    case GpuOcclusionDecision::occluded: return "occluded";
    case GpuOcclusionDecision::conservative_unknown: return "conservative-unknown";
    }
    return "conservative-unknown";
}

bool gpu_occlusion_decision_culls(const GpuOcclusionDecision decision) noexcept {
    return decision == GpuOcclusionDecision::occluded;
}

GpuOcclusionConfig gpu_occlusion_quality_defaults(const GpuOcclusionQuality quality) {
    GpuOcclusionConfig result;
    result.quality = quality;
    switch (quality) {
    case GpuOcclusionQuality::off:
        result.enabled = false;
        result.max_mip = 0U;
        result.depth_bias = 0.0F;
        result.occluded_frames_to_cull = 0U;
        result.visible_frames_to_release = 0U;
        break;
    case GpuOcclusionQuality::low:
        result.max_mip = 5U;
        result.depth_bias = 0.15F;
        result.occluded_frames_to_cull = 2U;
        result.visible_frames_to_release = 1U;
        break;
    case GpuOcclusionQuality::medium:
        result.max_mip = 8U;
        result.depth_bias = 0.08F;
        result.occluded_frames_to_cull = 2U;
        result.visible_frames_to_release = 1U;
        break;
    case GpuOcclusionQuality::high:
        result.max_mip = 12U;
        result.depth_bias = 0.05F;
        result.occluded_frames_to_cull = 3U;
        result.visible_frames_to_release = 2U;
        break;
    default:
        result.quality = GpuOcclusionQuality::off;
        result.enabled = false;
        result.max_mip = 0U;
        result.depth_bias = 0.0F;
        result.occluded_frames_to_cull = 0U;
        result.visible_frames_to_release = 0U;
        break;
    }
    return result;
}

std::vector<GpuOcclusionDiagnostic>
validate_gpu_occlusion_policy(const GpuOcclusionConfig& config) {
    std::vector<GpuOcclusionDiagnostic> diagnostics;
    if (config.schema != gpu_occlusion_policy_schema) {
        add_diagnostic(diagnostics, "gpu-occlusion.unsupported-schema", "/schema",
                       "Expected noemancer.gpu-occlusion-policy/0.1.");
    }
    if (!text_non_empty(config.profile_id)) {
        add_diagnostic(diagnostics, "gpu-occlusion.profile-id-range", "/profileId",
                       "profileId must contain 1..512 UTF-8 bytes.");
    }
    if (!gpu_occlusion_quality_valid(config.quality)) {
        add_diagnostic(diagnostics, "gpu-occlusion.invalid-quality", "/quality",
                       "quality must be off, low, medium or high.");
    }
    if (config.quality == GpuOcclusionQuality::off && config.enabled) {
        add_diagnostic(diagnostics, "gpu-occlusion.off-enabled-conflict", "/enabled",
                       "An Off quality profile must set enabled to false.");
    }
    if (!config.enabled) return diagnostics;

    if (config.max_mip == 0U || config.max_mip > gpu_occlusion_policy_max_hiz_mip) {
        add_diagnostic(diagnostics, "gpu-occlusion.max-mip-range", "/maxMip",
                       "maxMip must be in [1,16] for an enabled policy.");
    }
    validate_float(diagnostics, "/depthBias", config.depth_bias, 0.0F, 10.0F,
                   "depthBias must be finite in [0,10].");
    if (config.occluded_frames_to_cull == 0U ||
        config.occluded_frames_to_cull > gpu_occlusion_policy_max_hysteresis_frames) {
        add_diagnostic(diagnostics, "gpu-occlusion.occluded-hysteresis-range",
                       "/occludedFramesToCull", "occludedFramesToCull must be in [1,8].");
    }
    if (config.visible_frames_to_release == 0U ||
        config.visible_frames_to_release > gpu_occlusion_policy_max_hysteresis_frames) {
        add_diagnostic(diagnostics, "gpu-occlusion.visible-hysteresis-range",
                       "/visibleFramesToRelease", "visibleFramesToRelease must be in [1,8].");
    }
    return diagnostics;
}

GpuOcclusionPlan build_gpu_occlusion_plan(const GpuOcclusionConfig& config,
                                          const GpuOcclusionQuery& query) {
    GpuOcclusionPlan result;
    result.profile_id = config.profile_id;
    result.quality = config.quality;
    result.config = config;
    result.query = query;
    result.config_valid = validate_gpu_occlusion_policy(config).empty();
    result.valid = result.config_valid;
    result.enabled = config.enabled && config.quality != GpuOcclusionQuality::off;
    result.draw_visible = true;
    result.code = result.config_valid ? "gpu-occlusion.off" : "gpu-occlusion.invalid-config";
    result.detail = result.config_valid
        ? "GPU occlusion is intentionally disabled by the selected policy."
        : "GPU occlusion configuration failed the plain-data validity contract.";
    if (!result.config_valid) {
        result.diagnostics = validate_gpu_occlusion_policy(config);
        result.raw_decision = GpuOcclusionDecision::conservative_unknown;
        result.decision = GpuOcclusionDecision::conservative_unknown;
        result.draw_visible = true;
        result.code = "gpu-occlusion.invalid-config";
        result.detail = "Invalid policy configuration forces conservative visibility.";
        result.next_history = reset_history_for_camera(query.camera);
        return result;
    }
    if (!result.enabled) {
        result.raw_decision = GpuOcclusionDecision::visible;
        result.decision = GpuOcclusionDecision::visible;
        result.draw_visible = true;
        result.next_history = reset_history_for_camera(query.camera);
        return result;
    }

    const auto camera_identity_valid = !query.camera.identity.empty() &&
        query.camera.identity.size() <= gpu_occlusion_policy_max_text_bytes;
    if (!camera_identity_valid) {
        add_input_diagnostic(result, "gpu-occlusion.camera-identity-invalid", "/camera/identity",
                             "A bounded stable camera identity is required.");
        finalize_unknown(result, "Camera identity is unavailable; draw conservatively.");
        return result;
    }
    if (query.history.valid &&
        (query.history.camera_identity.empty() ||
         query.history.camera_identity.size() > gpu_occlusion_policy_max_text_bytes)) {
        add_input_diagnostic(result, "gpu-occlusion.history-camera-invalid",
                             "/history/cameraIdentity",
                             "History camera identity is invalid; reset occlusion hysteresis.");
        finalize_unknown(result, "Invalid history identity forces conservative visibility.");
        return result;
    }
    if (query.history.occluded_streak > gpu_occlusion_policy_max_hysteresis_frames ||
        query.history.visible_streak > gpu_occlusion_policy_max_hysteresis_frames) {
        add_input_diagnostic(result, "gpu-occlusion.history-range", "/history",
                             "History streaks exceed the bounded policy range.");
        finalize_unknown(result, "Out-of-range history forces conservative visibility.");
        return result;
    }

    result.camera_switched = query.camera.cut || query.camera.projection_changed ||
        (query.history.valid && !same_camera(query.history, query.camera));
    if (result.camera_switched) {
        add_input_diagnostic(result, "gpu-occlusion.camera-switch-reset", "/camera",
                             "Camera switch or projection cut resets occlusion hysteresis.");
        finalize_unknown(result, "Camera changed; one conservative frame is required.");
        return result;
    }
    if (!valid_projection(query.projection, result) ||
        !valid_hiz_sample(query.hiz, config, result)) {
        finalize_unknown(result, "Invalid projection or HiZ facts force conservative visibility.");
        return result;
    }

    const auto threshold = static_cast<double>(query.hiz.max_depth) +
        static_cast<double>(config.depth_bias);
    if (!std::isfinite(threshold)) {
        add_input_diagnostic(result, "gpu-occlusion.depth-threshold-invalid", "/depth",
                             "The HiZ comparison threshold is non-finite.");
        finalize_unknown(result, "Non-finite depth threshold forces conservative visibility.");
        return result;
    }
    result.raw_decision = static_cast<double>(query.projection.nearest_depth) > threshold
        ? GpuOcclusionDecision::occluded
        : GpuOcclusionDecision::visible;

    auto next = reset_history_for_camera(query.camera);
    const auto previous_occluded = query.history.valid ? query.history.occluded_streak : 0U;
    const auto previous_visible = query.history.valid ? query.history.visible_streak : 0U;
    if (result.raw_decision == GpuOcclusionDecision::occluded) {
        next.occluded_streak = std::min(
            gpu_occlusion_policy_max_hysteresis_frames, previous_occluded + 1U);
        next.visible_streak = 0U;
        if (next.occluded_streak < config.occluded_frames_to_cull) {
            result.decision = GpuOcclusionDecision::conservative_unknown;
            result.draw_visible = true;
            result.hysteresis_applied = true;
            add_input_diagnostic(result, "gpu-occlusion.occluded-warmup", "/history",
                                 "Positive HiZ samples have not reached the cull hysteresis threshold.");
            result.code = "gpu-occlusion.conservative-unknown";
            result.detail = "Occlusion warmup remains draw-visible.";
        } else {
            result.decision = GpuOcclusionDecision::occluded;
            result.draw_visible = false;
            result.hysteresis_applied = next.occluded_streak > 1U;
            result.code = "gpu-occlusion.occluded";
            result.detail = "A stable HiZ positive passed the occlusion hysteresis threshold.";
        }
    } else {
        const bool releasing = previous_occluded >= config.occluded_frames_to_cull;
        // Preserve the confirmed occluded state while release hysteresis is
        // warming up.  Otherwise a second visible frame would forget that the
        // first one was a release candidate and bypass a threshold > 2.
        next.occluded_streak = releasing ? previous_occluded : 0U;
        next.visible_streak = std::min(
            gpu_occlusion_policy_max_hysteresis_frames, previous_visible + 1U);
        if (releasing && next.visible_streak < config.visible_frames_to_release) {
            result.decision = GpuOcclusionDecision::conservative_unknown;
            result.draw_visible = true;
            result.hysteresis_applied = true;
            add_input_diagnostic(result, "gpu-occlusion.visible-release-warmup", "/history",
                                 "A visible sample is held draw-visible until release hysteresis completes.");
            result.code = "gpu-occlusion.conservative-unknown";
            result.detail = "Visibility release warmup remains draw-visible.";
        } else {
            next.occluded_streak = 0U;
            result.decision = GpuOcclusionDecision::visible;
            result.draw_visible = true;
            result.hysteresis_applied = releasing && config.visible_frames_to_release > 1U;
            result.code = "gpu-occlusion.visible";
            result.detail = "The candidate is not conservatively behind the HiZ max depth.";
        }
    }
    result.next_history = std::move(next);
    return result;
}

std::string gpu_occlusion_policy_canonical_config(const GpuOcclusionConfig& config) {
    return config_json(config).dump(2) + "\n";
}

std::string gpu_occlusion_policy_canonical_evidence(const GpuOcclusionPlan& plan) {
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
        {"quality", gpu_occlusion_quality_name(plan.quality)},
        {"config", config_json(plan.config)},
        {"configValid", plan.config_valid},
        {"valid", plan.valid},
        {"enabled", plan.enabled},
        {"rawDecision", gpu_occlusion_decision_name(plan.raw_decision)},
        {"decision", gpu_occlusion_decision_name(plan.decision)},
        {"drawVisible", plan.draw_visible},
        {"cameraSwitched", plan.camera_switched},
        {"hysteresisApplied", plan.hysteresis_applied},
        {"code", bounded_text(plan.code)},
        {"detail", bounded_text(plan.detail)},
        {"projection", projection_json(plan.query.projection)},
        {"hiz", hiz_json(plan.query.hiz)},
        {"camera", camera_json(plan.query.camera)},
        {"history", history_json(plan.query.history)},
        {"nextHistory", history_json(plan.next_history)},
        {"diagnostics", std::move(diagnostics)},
    };
    return output.dump(2) + "\n";
}

std::string gpu_occlusion_policy_fingerprint(const GpuOcclusionPlan& plan) {
    return "fnv1a64:" + hex_u64(fnv1a(gpu_occlusion_policy_canonical_evidence(plan)));
}

} // namespace noemancer
