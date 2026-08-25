#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace noemancer {

// Renderer-neutral SSR policy.  This contract intentionally contains no
// texture, command-buffer, SDL or RHI type.  The Runtime maps the plan to the
// depth pyramid, scene colour and material buffers owned by its backend.
inline constexpr std::string_view screen_space_reflections_schema =
    "noemancer.screen-space-reflections/0.1";
inline constexpr std::string_view screen_space_reflections_material_contract =
    "F0.rgb+roughness.a";
inline constexpr std::string_view screen_space_reflections_composition_strategy =
    "replace-ibl-specular-by-confidence";
inline constexpr std::string_view screen_space_reflections_fallback_strategy =
    "retain-ibl-specular";

inline constexpr std::uint32_t screen_space_reflections_max_ray_steps = 128U;
inline constexpr std::uint32_t screen_space_reflections_max_binary_search_steps = 16U;
inline constexpr std::uint32_t screen_space_reflections_max_hierarchy_mip = 16U;
inline constexpr std::size_t screen_space_reflections_max_diagnostics = 32U;
inline constexpr std::size_t screen_space_reflections_max_text_bytes = 512U;

enum class ScreenSpaceReflectionsQuality : std::uint8_t {
    off = 0U,
    low = 1U,
    medium = 2U,
    high = 3U,

    Off = off,
    Low = low,
    Medium = medium,
    High = high,
};

enum class ScreenSpaceReflectionsHybridPixelPolicy : std::uint8_t {
    // SSR is disabled so a Hybrid Pixel profile keeps deterministic pixel
    // stability.  IBL remains the specular source through the fallback.
    disable = 0U,
    // SSR may run per-frame, but it must not consume temporal history.
    spatial_only = 1U,
    // Explicit opt-in for projects willing to trade pixel stability for
    // temporal SSR.  This is never the default.
    allow_temporal = 2U,

    Disable = disable,
    SpatialOnly = spatial_only,
    AllowTemporal = allow_temporal,
};

[[nodiscard]] std::string_view screen_space_reflections_quality_name(
    ScreenSpaceReflectionsQuality quality) noexcept;
[[nodiscard]] std::optional<ScreenSpaceReflectionsQuality>
screen_space_reflections_quality_from_string(std::string_view value) noexcept;
[[nodiscard]] bool screen_space_reflections_quality_valid(
    ScreenSpaceReflectionsQuality quality) noexcept;

[[nodiscard]] std::string_view screen_space_reflections_hybrid_pixel_policy_name(
    ScreenSpaceReflectionsHybridPixelPolicy policy) noexcept;
[[nodiscard]] std::optional<ScreenSpaceReflectionsHybridPixelPolicy>
screen_space_reflections_hybrid_pixel_policy_from_string(
    std::string_view value) noexcept;
[[nodiscard]] bool screen_space_reflections_hybrid_pixel_policy_valid(
    ScreenSpaceReflectionsHybridPixelPolicy policy) noexcept;

struct ScreenSpaceReflectionsRayMarchSettings final {
    bool hierarchical{true};
    // The first depth-pyramid level sampled by a ray.  Keeping this explicit
    // makes the coarse-to-fine policy inspectable by an Agent.
    std::uint32_t start_mip{};
    std::uint32_t max_mip{12U};
    std::uint32_t max_steps{32U};
    std::uint32_t binary_search_steps{5U};
    float initial_step_pixels{1.0F};
    float max_distance{100.0F};
    float thickness{0.08F};
    float mip_bias{};
};

struct ScreenSpaceReflectionsMaterialEligibility final {
    std::string contract{std::string(screen_space_reflections_material_contract)};
    float minimum_roughness{};
    float roughness_cutoff{0.85F};
    bool allow_opaque{true};
    bool allow_alpha_tested{true};
    bool allow_translucent{};
    bool allow_unlit{};
};

struct ScreenSpaceReflectionsEdgeFade final {
    // Values are normalized screen-edge distances.  Reflection confidence is
    // multiplied by a smooth fade from start to end.
    float start{0.65F};
    float end{0.95F};
};

struct ScreenSpaceReflectionsComposition final {
    std::string strategy{std::string(screen_space_reflections_composition_strategy)};
    std::string fallback{std::string(screen_space_reflections_fallback_strategy)};
    float confidence_threshold{0.15F};
    float history_weight{0.90F};
};

struct ScreenSpaceReflectionsConfig final {
    std::string schema{std::string(screen_space_reflections_schema)};
    std::string profile_id{"default"};
    ScreenSpaceReflectionsQuality quality{ScreenSpaceReflectionsQuality::high};
    bool enabled{true};
    ScreenSpaceReflectionsRayMarchSettings ray_march;
    ScreenSpaceReflectionsMaterialEligibility material;
    ScreenSpaceReflectionsEdgeFade edge_fade;
    ScreenSpaceReflectionsComposition composition;
    ScreenSpaceReflectionsHybridPixelPolicy hybrid_pixel_policy{
        ScreenSpaceReflectionsHybridPixelPolicy::disable};
};

struct ScreenSpaceReflectionsDiagnostic final {
    std::string code;
    std::string path;
    std::string message;
};

// These are the only resource facts needed by the pure planner.  The
// Runtime fills them from its actual Render Graph state; no backend handle is
// allowed to cross into this structure.
struct ScreenSpaceReflectionsInputAvailability final {
    bool depth_pyramid_ready{true};
    bool scene_color_ready{true};
    bool material_buffers_ready{true};
};

struct ScreenSpaceReflectionsPlan final {
    std::string schema{std::string(screen_space_reflections_schema)};
    std::string profile_id;
    ScreenSpaceReflectionsQuality quality{ScreenSpaceReflectionsQuality::off};
    ScreenSpaceReflectionsConfig config;
    ScreenSpaceReflectionsInputAvailability inputs;
    bool config_valid{};
    bool valid{};
    bool enabled{};
    bool hierarchical_depth_required{};
    bool temporal_history_required{};
    bool hybrid_pixel_active{};
    bool disabled_by_hybrid_pixel{};
    bool fallback_enabled{true};
    bool fallback_only{};
    std::string code;
    std::string detail;
    std::vector<ScreenSpaceReflectionsDiagnostic> diagnostics;
};

[[nodiscard]] ScreenSpaceReflectionsConfig
screen_space_reflections_quality_defaults(ScreenSpaceReflectionsQuality quality);

[[nodiscard]] std::vector<ScreenSpaceReflectionsDiagnostic>
validate_screen_space_reflections(const ScreenSpaceReflectionsConfig& config);

// Derives an immutable, bounded execution policy.  `hybrid_pixel_active`
// applies the explicit Hybrid Pixel policy before checking resource inputs.
// A valid plan may be intentionally disabled (Off, Hybrid Pixel disable or a
// missing input); `code`, `fallback_only` and diagnostics explain why.
[[nodiscard]] ScreenSpaceReflectionsPlan build_screen_space_reflections_plan(
    const ScreenSpaceReflectionsConfig& config,
    bool hybrid_pixel_active = false,
    const ScreenSpaceReflectionsInputAvailability& inputs = {});

[[nodiscard]] std::string screen_space_reflections_canonical_config(
    const ScreenSpaceReflectionsConfig& config);
[[nodiscard]] std::string screen_space_reflections_canonical_evidence(
    const ScreenSpaceReflectionsPlan& plan);
[[nodiscard]] std::string screen_space_reflections_fingerprint(
    const ScreenSpaceReflectionsPlan& plan);

} // namespace noemancer
