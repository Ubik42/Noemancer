#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace noemancer {

// Engine-owned, renderer-neutral policy for deciding whether a static opaque
// candidate may be rejected by the shared min/max depth pyramid.  The
// Runtime maps these facts to the actual Render Graph and GPU resources; no
// backend handle or third-party type crosses this contract.
inline constexpr std::string_view gpu_occlusion_policy_schema =
    "noemancer.gpu-occlusion-policy/0.1";
inline constexpr std::string_view gpu_occlusion_policy_hiz_resource_id =
    "render.resource.scene-depth-pyramid";
inline constexpr std::uint32_t gpu_occlusion_policy_max_hiz_mip = 16U;
inline constexpr std::uint32_t gpu_occlusion_policy_max_hysteresis_frames = 8U;
inline constexpr std::size_t gpu_occlusion_policy_max_diagnostics = 32U;
inline constexpr std::size_t gpu_occlusion_policy_max_text_bytes = 512U;

enum class GpuOcclusionQuality : std::uint8_t {
    off = 0U,
    low = 1U,
    medium = 2U,
    high = 3U,

    Off = off,
    Low = low,
    Medium = medium,
    High = high,
};

enum class GpuOcclusionDecision : std::uint8_t {
    visible = 0U,
    occluded = 1U,
    // Unknown is deliberately draw-visible.  It is a first-class decision so
    // diagnostics can distinguish a conservative fallback from a positive
    // visibility result without ever allowing invalid data to cull geometry.
    conservative_unknown = 2U,

    Visible = visible,
    Occluded = occluded,
    ConservativeUnknown = conservative_unknown,
};

[[nodiscard]] std::string_view gpu_occlusion_quality_name(
    GpuOcclusionQuality quality) noexcept;
[[nodiscard]] std::optional<GpuOcclusionQuality>
gpu_occlusion_quality_from_string(std::string_view value) noexcept;
[[nodiscard]] bool gpu_occlusion_quality_valid(GpuOcclusionQuality quality) noexcept;

[[nodiscard]] std::string_view gpu_occlusion_decision_name(
    GpuOcclusionDecision decision) noexcept;
[[nodiscard]] bool gpu_occlusion_decision_culls(
    GpuOcclusionDecision decision) noexcept;

struct GpuOcclusionConfig final {
    std::string schema{std::string(gpu_occlusion_policy_schema)};
    std::string profile_id{"default"};
    GpuOcclusionQuality quality{GpuOcclusionQuality::high};
    bool enabled{true};
    // The selected sample mip must be below both this profile limit and the
    // actual HiZ mip count.  The policy never silently clamps an invalid mip.
    std::uint32_t max_mip{12U};
    // Linear-view-depth tolerance in world units.  A candidate is only
    // positively occluded when its nearest depth is beyond HiZ max depth plus
    // this conservative bias.
    float depth_bias{0.05F};
    // Consecutive positive HiZ samples required before culling is allowed.
    std::uint32_t occluded_frames_to_cull{2U};
    // Consecutive visible samples required to leave a previously confirmed
    // occluded state.  Unknown samples always remain draw-visible.
    std::uint32_t visible_frames_to_release{1U};
};

struct GpuOcclusionProjectionBounds final {
    // Normalized viewport bounds.  Values outside [0,1] are not clamped: an
    // object crossing the viewport is conservative-unknown and remains drawn.
    bool valid{};
    float min_x{};
    float min_y{};
    float max_x{};
    float max_y{};
    // Linear-view depth interval shared with the RG32F min/max HiZ pyramid.
    float nearest_depth{};
    float farthest_depth{};
};

struct GpuOcclusionHiZSample final {
    bool available{};
    std::string resource_id{std::string(gpu_occlusion_policy_hiz_resource_id)};
    bool covers_bounds{};
    std::uint32_t mip_level{};
    std::uint32_t mip_count{};
    std::uint32_t sample_count{};
    float min_depth{};
    float max_depth{};
};

struct GpuOcclusionCameraState final {
    // Stable semantic camera identity, not a pointer or backend handle.
    std::string identity;
    std::uint64_t revision{};
    bool cut{};
    bool projection_changed{};
};

struct GpuOcclusionHistory final {
    bool valid{};
    std::string camera_identity;
    std::uint64_t camera_revision{};
    std::uint32_t occluded_streak{};
    std::uint32_t visible_streak{};
};

struct GpuOcclusionQuery final {
    GpuOcclusionProjectionBounds projection;
    GpuOcclusionHiZSample hiz;
    GpuOcclusionCameraState camera;
    GpuOcclusionHistory history;
};

struct GpuOcclusionDiagnostic final {
    std::string code;
    std::string path;
    std::string message;
};

struct GpuOcclusionPlan final {
    std::string schema{std::string(gpu_occlusion_policy_schema)};
    std::string profile_id;
    GpuOcclusionQuality quality{GpuOcclusionQuality::off};
    GpuOcclusionConfig config;
    GpuOcclusionQuery query;
    GpuOcclusionHistory next_history;
    GpuOcclusionDecision raw_decision{
        GpuOcclusionDecision::conservative_unknown};
    GpuOcclusionDecision decision{GpuOcclusionDecision::conservative_unknown};
    bool config_valid{};
    bool valid{};
    bool enabled{};
    bool camera_switched{};
    bool hysteresis_applied{};
    // This is the explicit safety boundary consumed by submission: false is
    // only allowed for a confirmed occluded result after hysteresis.
    bool draw_visible{true};
    std::string code;
    std::string detail;
    std::vector<GpuOcclusionDiagnostic> diagnostics;
};

[[nodiscard]] GpuOcclusionConfig gpu_occlusion_quality_defaults(
    GpuOcclusionQuality quality);

[[nodiscard]] std::vector<GpuOcclusionDiagnostic>
validate_gpu_occlusion_policy(const GpuOcclusionConfig& config);

// Evaluates one candidate using a pure plain-data input.  Invalid projection
// bounds, a missing/invalid HiZ sample, a camera switch and hysteresis warmup
// all return conservative-unknown with draw_visible=true.  Only a stable
// positive HiZ result may return occluded/draw_visible=false.
[[nodiscard]] GpuOcclusionPlan build_gpu_occlusion_plan(
    const GpuOcclusionConfig& config,
    const GpuOcclusionQuery& query);

using GpuOcclusionEvaluation = GpuOcclusionPlan;

[[nodiscard]] std::string gpu_occlusion_policy_canonical_config(
    const GpuOcclusionConfig& config);
[[nodiscard]] std::string gpu_occlusion_policy_canonical_evidence(
    const GpuOcclusionPlan& plan);
[[nodiscard]] std::string gpu_occlusion_policy_fingerprint(
    const GpuOcclusionPlan& plan);

} // namespace noemancer
