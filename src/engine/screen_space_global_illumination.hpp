#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace noemancer {

// Engine-owned SSGI policy.  It describes the ray budget and the semantic
// output of the pass without leaking SDL, GPU textures, command buffers or a
// particular RHI into the authoring/Agent contract.
inline constexpr std::string_view screen_space_global_illumination_schema =
    "noemancer.screen-space-global-illumination/0.1";
inline constexpr std::string_view screen_space_global_illumination_material_contract =
    "normal.rgb+roughness.a+baseColor.rgb+metallic.a";
inline constexpr std::string_view screen_space_global_illumination_bent_normal_semantics =
    "visibility-weighted-hemisphere-normal";
inline constexpr std::string_view screen_space_global_illumination_visibility_semantics =
    "confidence-weighted-ambient-visibility";
inline constexpr std::string_view screen_space_global_illumination_composition_strategy =
    "replace-IBL-diffuse-by-confidence";
inline constexpr std::string_view screen_space_global_illumination_fallback_strategy =
    "retain-ibl-diffuse";
inline constexpr std::string_view screen_space_global_illumination_history_policy =
    "shared-temporal-history";
inline constexpr std::string_view screen_space_global_illumination_history_fallback =
    "spatial-current-frame";

// These limits are part of the v0.1 execution contract and match the bounded
// loops compiled into ssgi_hiz_gather.frag.hlsl.  The public policy must never
// advertise work that the shader silently clamps away.
inline constexpr std::uint32_t screen_space_global_illumination_max_samples = 16U;
inline constexpr std::uint32_t screen_space_global_illumination_max_directions = 16U;
inline constexpr std::uint32_t screen_space_global_illumination_max_ray_steps = 32U;
inline constexpr std::uint32_t screen_space_global_illumination_max_hierarchy_mip = 16U;
inline constexpr std::size_t screen_space_global_illumination_max_diagnostics = 32U;
inline constexpr std::size_t screen_space_global_illumination_max_text_bytes = 512U;

enum class ScreenSpaceGlobalIlluminationQuality : std::uint8_t {
    off = 0U,
    low = 1U,
    medium = 2U,
    high = 3U,

    Off = off,
    Low = low,
    Medium = medium,
    High = high,
};

enum class ScreenSpaceGlobalIlluminationHybridPixelPolicy : std::uint8_t {
    // The default keeps Hybrid Pixel spatially deterministic.  The IBL
    // diffuse fallback remains the ambient source.
    disable = 0U,
    // Run a current-frame SSGI estimate but do not consume temporal history.
    spatial_only = 1U,
    // Explicit opt-in to shared temporal SSGI history.
    allow_temporal = 2U,

    Disable = disable,
    SpatialOnly = spatial_only,
    AllowTemporal = allow_temporal,
};

[[nodiscard]] std::string_view screen_space_global_illumination_quality_name(
    ScreenSpaceGlobalIlluminationQuality quality) noexcept;
[[nodiscard]] std::optional<ScreenSpaceGlobalIlluminationQuality>
screen_space_global_illumination_quality_from_string(std::string_view value) noexcept;
[[nodiscard]] bool screen_space_global_illumination_quality_valid(
    ScreenSpaceGlobalIlluminationQuality quality) noexcept;

[[nodiscard]] std::string_view screen_space_global_illumination_hybrid_pixel_policy_name(
    ScreenSpaceGlobalIlluminationHybridPixelPolicy policy) noexcept;
[[nodiscard]] std::optional<ScreenSpaceGlobalIlluminationHybridPixelPolicy>
screen_space_global_illumination_hybrid_pixel_policy_from_string(
    std::string_view value) noexcept;
[[nodiscard]] bool screen_space_global_illumination_hybrid_pixel_policy_valid(
    ScreenSpaceGlobalIlluminationHybridPixelPolicy policy) noexcept;

struct ScreenSpaceGlobalIlluminationSamplingSettings final {
    bool hierarchical{true};
    std::uint32_t start_mip{};
    std::uint32_t max_mip{12U};
    std::uint32_t sample_count{16U};
    std::uint32_t directions{8U};
    std::uint32_t max_steps{32U};
    float ray_step_pixels{1.0F};
    float radius{4.0F};
    float max_distance{80.0F};
    float thickness{0.08F};
    float intensity{1.20F};
    float falloff{1.0F};
};

struct ScreenSpaceGlobalIlluminationMaterialEligibility final {
    std::string contract{
        std::string(screen_space_global_illumination_material_contract)};
    float minimum_roughness{};
    float roughness_cutoff{1.0F};
    bool allow_opaque{true};
    bool allow_alpha_tested{true};
    bool allow_translucent{};
    bool allow_unlit{};
    bool require_normal{true};
};

struct ScreenSpaceGlobalIlluminationBentNormal final {
    bool enabled{true};
    std::string semantics{
        std::string(screen_space_global_illumination_bent_normal_semantics)};
    float strength{1.0F};
};

struct ScreenSpaceGlobalIlluminationVisibility final {
    bool enabled{true};
    std::string semantics{
        std::string(screen_space_global_illumination_visibility_semantics)};
    float power{1.0F};
};

struct ScreenSpaceGlobalIlluminationHistorySettings final {
    bool enabled{true};
    std::string policy{std::string(screen_space_global_illumination_history_policy)};
    std::string fallback{std::string(screen_space_global_illumination_history_fallback)};
    float weight{0.90F};
    float depth_rejection{0.02F};
    float normal_rejection_cosine{0.85F};
};

struct ScreenSpaceGlobalIlluminationComposition final {
    std::string strategy{
        std::string(screen_space_global_illumination_composition_strategy)};
    std::string fallback{std::string(screen_space_global_illumination_fallback_strategy)};
    float confidence_threshold{0.20F};
};

struct ScreenSpaceGlobalIlluminationConfig final {
    std::string schema{std::string(screen_space_global_illumination_schema)};
    std::string profile_id{"default"};
    ScreenSpaceGlobalIlluminationQuality quality{
        ScreenSpaceGlobalIlluminationQuality::high};
    bool enabled{true};
    ScreenSpaceGlobalIlluminationSamplingSettings sampling;
    ScreenSpaceGlobalIlluminationMaterialEligibility material;
    ScreenSpaceGlobalIlluminationBentNormal bent_normal;
    ScreenSpaceGlobalIlluminationVisibility visibility;
    ScreenSpaceGlobalIlluminationHistorySettings history;
    ScreenSpaceGlobalIlluminationComposition composition;
    ScreenSpaceGlobalIlluminationHybridPixelPolicy hybrid_pixel_policy{
        ScreenSpaceGlobalIlluminationHybridPixelPolicy::disable};
};

struct ScreenSpaceGlobalIlluminationDiagnostic final {
    std::string code;
    std::string path;
    std::string message;
};

// Resource readiness is deliberately expressed as facts rather than handles.
// The Runtime maps the current Render Graph state into this value.
struct ScreenSpaceGlobalIlluminationInputAvailability final {
    bool depth_pyramid_ready{true};
    bool normal_buffer_ready{true};
    bool material_buffers_ready{true};
    bool history_target_ready{true};
};

struct ScreenSpaceGlobalIlluminationHistoryInput final {
    bool previous_valid{};
    bool camera_cut{};
    bool extent_changed{};
    bool profile_changed{};
};

struct ScreenSpaceGlobalIlluminationHistoryPlan final {
    bool required{};
    bool target_ready{true};
    bool use_previous{};
    bool reset_required{};
    bool fallback_to_current_frame{};
    std::string policy{std::string(screen_space_global_illumination_history_policy)};
    std::string fallback{std::string(screen_space_global_illumination_history_fallback)};
    std::string reset_reason{"none"};
};

struct ScreenSpaceGlobalIlluminationPlan final {
    std::string schema{std::string(screen_space_global_illumination_schema)};
    std::string profile_id;
    ScreenSpaceGlobalIlluminationQuality quality{
        ScreenSpaceGlobalIlluminationQuality::off};
    ScreenSpaceGlobalIlluminationConfig config;
    ScreenSpaceGlobalIlluminationInputAvailability inputs;
    ScreenSpaceGlobalIlluminationHistoryInput history_inputs;
    ScreenSpaceGlobalIlluminationHistoryPlan history;
    bool config_valid{};
    bool valid{};
    bool enabled{};
    bool hierarchical_depth_required{};
    bool hybrid_pixel_active{};
    bool disabled_by_hybrid_pixel{};
    bool bent_normal_output{};
    bool visibility_output{};
    bool fallback_enabled{true};
    bool fallback_only{};
    std::string code;
    std::string detail;
    std::vector<ScreenSpaceGlobalIlluminationDiagnostic> diagnostics;
};

[[nodiscard]] ScreenSpaceGlobalIlluminationConfig
screen_space_global_illumination_quality_defaults(
    ScreenSpaceGlobalIlluminationQuality quality);

[[nodiscard]] std::vector<ScreenSpaceGlobalIlluminationDiagnostic>
validate_screen_space_global_illumination(
    const ScreenSpaceGlobalIlluminationConfig& config);

// Derives a bounded, renderer-neutral policy.  A valid plan can intentionally
// be disabled (Off, Hybrid Pixel policy or missing mandatory inputs); the
// fallback and diagnostics explain how the renderer should continue.
[[nodiscard]] ScreenSpaceGlobalIlluminationPlan
build_screen_space_global_illumination_plan(
    const ScreenSpaceGlobalIlluminationConfig& config,
    bool hybrid_pixel_active = false,
    const ScreenSpaceGlobalIlluminationInputAvailability& inputs = {},
    const ScreenSpaceGlobalIlluminationHistoryInput& history_inputs = {});

[[nodiscard]] std::string screen_space_global_illumination_canonical_config(
    const ScreenSpaceGlobalIlluminationConfig& config);
[[nodiscard]] std::string screen_space_global_illumination_canonical_evidence(
    const ScreenSpaceGlobalIlluminationPlan& plan);
[[nodiscard]] std::string screen_space_global_illumination_fingerprint(
    const ScreenSpaceGlobalIlluminationPlan& plan);

} // namespace noemancer
