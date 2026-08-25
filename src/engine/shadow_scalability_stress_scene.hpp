#pragma once

#include "engine/scene_document.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace noemancer {

// This is a renderer-neutral, deterministic workload contract.  It describes
// the pressure scene and the expected capacity arithmetic, but does not expose
// a GPU handle or claim that a particular backend selected the same lights.
inline constexpr std::string_view shadow_scalability_stress_scene_schema =
    "noemancer.shadow-scalability-stress/0.1";
inline constexpr std::uint32_t shadow_scalability_stress_scene_min_casters = 32U;
inline constexpr std::uint32_t shadow_scalability_stress_scene_max_casters = 4096U;
inline constexpr std::uint32_t shadow_scalability_stress_scene_max_local_lights = 128U;
inline constexpr std::uint32_t shadow_scalability_stress_scene_max_controls = 128U;
inline constexpr std::uint32_t shadow_scalability_stress_scene_max_capacity = 64U;
inline constexpr std::size_t shadow_scalability_stress_scene_max_text_bytes = 256U;
inline constexpr std::size_t shadow_scalability_stress_scene_max_contract_bytes = 1U << 20U;

struct ShadowScalabilityStressSceneConfig final {
    // The default deliberately exceeds the current high-quality local shadow
    // policy (one point light, two spot lights, eight atlas layers).
    std::uint32_t caster_count{128U};
    std::uint32_t point_light_count{2U};
    std::uint32_t spot_light_count{4U};
    std::uint32_t control_object_count{4U};
    std::uint32_t point_light_capacity{1U};
    std::uint32_t spot_light_capacity{2U};
    std::uint32_t local_atlas_layer_capacity{8U};
};

struct ShadowScalabilityStressSceneContract final {
    std::string schema{std::string(shadow_scalability_stress_scene_schema)};
    std::string workload_identity;
    std::string scene_guid;
    std::string camera_id;
    std::string directional_light_id;
    std::vector<std::string> caster_ids;
    std::vector<std::string> local_light_ids;
    std::vector<std::string> point_light_ids;
    std::vector<std::string> spot_light_ids;
    std::vector<std::string> control_ids;
    std::uint32_t expected_caster_count{};
    std::uint32_t expected_control_count{};
    std::uint32_t expected_requested_shadow_count{};
    std::uint32_t expected_selected_shadow_count{};
    std::uint32_t expected_dropped_shadow_count{};
    std::uint32_t expected_requested_local_shadow_layers{};
    std::uint32_t expected_selected_local_shadow_layers{};
    std::uint32_t expected_dropped_local_shadow_layers{};
    std::uint32_t point_light_capacity{};
    std::uint32_t spot_light_capacity{};
    std::uint32_t local_atlas_layer_capacity{};
    bool pressure_over_capacity{};
};

struct ShadowScalabilityStressSceneBuildResult final {
    bool valid{};
    std::string code;
    std::string detail;
    std::optional<SceneDocument> document;
    std::optional<ShadowScalabilityStressSceneContract> contract;

    [[nodiscard]] explicit operator bool() const noexcept { return valid; }
};

struct ShadowScalabilityStressSceneContractError final {
    std::string code;
    std::string path;
    std::string message;
};

struct ShadowScalabilityStressSceneContractParseResult final {
    std::optional<ShadowScalabilityStressSceneContract> contract;
    std::vector<ShadowScalabilityStressSceneContractError> errors;

    [[nodiscard]] explicit operator bool() const noexcept { return contract.has_value(); }
};

[[nodiscard]] ShadowScalabilityStressSceneBuildResult build_shadow_scalability_stress_scene(
    const ShadowScalabilityStressSceneConfig& config = {});

// Alias kept beside the builder so callers can use the same make_* vocabulary
// as the existing SceneDocument fixture builders.
[[nodiscard]] ShadowScalabilityStressSceneBuildResult make_shadow_scalability_stress_scene(
    const ShadowScalabilityStressSceneConfig& config = {});

[[nodiscard]] std::string write_shadow_scalability_stress_scene_contract_json(
    const ShadowScalabilityStressSceneContract& contract);

[[nodiscard]] ShadowScalabilityStressSceneContractParseResult
parse_shadow_scalability_stress_scene_contract_json(std::string_view json);

} // namespace noemancer
