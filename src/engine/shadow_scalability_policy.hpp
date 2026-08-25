#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace noemancer {

// Engine-owned, renderer-neutral decision contract for deciding whether the
// current atlas-backed shadow path is healthy, needs more atlas capacity, or
// has enough pressure to justify a virtual-page prototype.  The Runtime may
// project these facts to CSM/local-shadow resources, but no GPU/RHI handle is
// part of this API.
inline constexpr std::string_view shadow_scalability_policy_schema =
    "noemancer.shadow-scalability-policy/0.1";
inline constexpr std::size_t shadow_scalability_policy_max_diagnostics = 32U;
inline constexpr std::size_t shadow_scalability_policy_max_text_bytes = 512U;
inline constexpr std::uint32_t shadow_scalability_policy_max_cascades = 16U;
inline constexpr std::uint32_t shadow_scalability_policy_max_layers = 4096U;
inline constexpr std::uint32_t shadow_scalability_policy_min_resolution = 128U;
inline constexpr std::uint32_t shadow_scalability_policy_max_resolution = 16384U;
inline constexpr std::uint64_t shadow_scalability_policy_max_bytes = 1ULL << 40U;

enum class ShadowScalabilityRecommendation : std::uint8_t {
    keep_atlas = 0U,
    extend_atlas = 1U,
    prototype_virtual_pages = 2U,
    insufficient_evidence = 3U,

    KeepAtlas = keep_atlas,
    ExtendAtlas = extend_atlas,
    PrototypeVirtualPages = prototype_virtual_pages,
    InsufficientEvidence = insufficient_evidence,
};

[[nodiscard]] std::string_view shadow_scalability_recommendation_name(
    ShadowScalabilityRecommendation recommendation) noexcept;

struct ShadowScalabilityWorkload final {
    std::string schema{std::string(shadow_scalability_policy_schema)};
    bool directional_enabled{true};
    std::uint32_t cascade_count{4U};
    std::uint32_t cascade_resolution{2048U};
    bool local_enabled{true};
    std::uint32_t local_layer_count{8U};
    std::uint32_t local_resolution{1024U};
    std::uint32_t requested_local_lights{};
    std::uint32_t selected_local_lights{};
    std::uint32_t dropped_local_lights{};
    std::uint64_t estimated_atlas_bytes{};
};

struct ShadowScalabilityCacheObservation final {
    bool available{};
    std::uint32_t directional_cascades_available{};
    std::uint32_t directional_cascades_cached{};
    std::uint64_t directional_cache_hits{};
    std::uint64_t directional_cache_misses{};
    std::uint32_t local_faces_available{};
    std::uint32_t local_faces_cached{};
    std::uint64_t local_cache_hits{};
    std::uint64_t local_cache_misses{};
};

struct ShadowScalabilityGeometryObservation final {
    bool available{};
    std::uint64_t caster_count{};
    std::uint64_t primitive_count{};
    std::uint64_t draw_count{};
    std::uint64_t instances_submitted{};
    std::uint64_t draw_calls_saved{};
};

struct ShadowScalabilityTimingObservation final {
    bool available{};
    double shadow_pass_milliseconds{};
    double directional_pass_milliseconds{};
    double local_pass_milliseconds{};
    double frame_budget_milliseconds{};
};

struct ShadowScalabilityInvalidationObservation final {
    bool available{};
    std::uint64_t camera_revision{};
    std::uint64_t light_revision{};
    std::uint64_t caster_revision{};
    bool camera_invalidated{};
    bool light_invalidated{};
    bool caster_invalidated{};
    std::uint64_t invalidations_last_window{};
    std::uint64_t observation_frames{};
};

struct ShadowScalabilityInput final {
    std::string schema{std::string(shadow_scalability_policy_schema)};
    ShadowScalabilityWorkload workload;
    ShadowScalabilityCacheObservation cache;
    ShadowScalabilityGeometryObservation geometry;
    ShadowScalabilityTimingObservation timing;
    ShadowScalabilityInvalidationObservation invalidation;
    std::uint64_t atlas_budget_bytes{};
};

struct ShadowScalabilityDiagnostic final {
    std::string code;
    std::string path;
    std::string message;
};

struct ShadowScalabilityPlan final {
    std::string schema{std::string(shadow_scalability_policy_schema)};
    ShadowScalabilityInput input;
    ShadowScalabilityRecommendation recommendation{
        ShadowScalabilityRecommendation::insufficient_evidence};
    bool valid{};
    bool evidence_complete{};
    std::string code;
    std::string detail;
    std::uint64_t conservative_atlas_bytes{};
    std::uint64_t atlas_budget_bytes{};
    std::uint64_t atlas_headroom_bytes{};
    double cache_hit_ratio{};
    double shadow_time_ratio{};
    double invalidation_ratio{};
    std::vector<ShadowScalabilityDiagnostic> reasons;
    std::vector<ShadowScalabilityDiagnostic> missing_evidence;
    std::vector<ShadowScalabilityDiagnostic> diagnostics;
};

[[nodiscard]] std::vector<ShadowScalabilityDiagnostic>
validate_shadow_scalability_input(const ShadowScalabilityInput& input);

// Produces a deterministic recommendation.  Missing or malformed evidence
// never turns into a capacity claim: the result remains
// insufficient_evidence and carries bounded diagnostics.
[[nodiscard]] ShadowScalabilityPlan evaluate_shadow_scalability(
    const ShadowScalabilityInput& input);

[[nodiscard]] std::string shadow_scalability_policy_canonical_evidence(
    const ShadowScalabilityPlan& plan);
[[nodiscard]] std::string shadow_scalability_policy_fingerprint(
    const ShadowScalabilityPlan& plan);

} // namespace noemancer
