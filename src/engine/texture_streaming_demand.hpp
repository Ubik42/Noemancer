#pragma once

#include "engine/texture_streaming_policy.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace noemancer {

// This layer turns renderer-independent screen observations into a resident
// mip target.  It deliberately carries no SDL, KTX, Basis or GPU handle so it
// can run in Render World extraction, Headless and Agent evidence tests.
enum class TextureStreamingDemandDecision : std::uint8_t {
    keep,
    upgrade,
    downgrade
};

struct TextureStreamingDemandSample final {
    std::string asset_id;
    TextureStreamingImportance importance{TextureStreamingImportance::normal};
    std::uint32_t authored_priority{};

    // Authoring and projected dimensions are in pixels.  A zero projected
    // dimension means no visible footprint for demand purposes.
    std::uint32_t base_width{1U};
    std::uint32_t base_height{1U};
    std::uint32_t projected_width{};
    std::uint32_t projected_height{};
    bool visible{true};

    // Mip zero is the highest detail level.  The current start is the level
    // actually resident before this plan; maximum is the authored tail floor.
    std::uint32_t current_mip_start{};
    std::uint32_t maximum_mip_start{};
    std::vector<std::uint64_t> mip_bytes;

    // Callers carry these small, deterministic histories rather than asking
    // the planner to own mutable state.  demand_age is the number of
    // consecutive frames with the same screen demand.  visibility_age is the
    // number of frames since the asset was last visible.
    std::uint32_t demand_age_frames{};
    std::uint32_t visibility_age_frames{};
    std::uint32_t upgrade_hysteresis_frames{2U};
    std::uint32_t downgrade_hysteresis_frames{8U};
};

struct TextureStreamingDemandAssetPlan final {
    std::string asset_id;
    TextureStreamingImportance importance{TextureStreamingImportance::normal};
    std::uint32_t authored_priority{};
    bool visible{};
    std::uint64_t projected_pixels{};
    std::uint32_t current_mip_start{};
    std::uint32_t screen_mip_start{};
    std::uint32_t hysteresis_mip_start{};
    std::uint32_t target_mip_start{};
    std::uint32_t maximum_mip_start{};
    TextureStreamingDemandDecision decision{TextureStreamingDemandDecision::keep};
    std::uint64_t resident_bytes{};
    std::uint64_t target_bytes{};
    std::uint64_t bytes_added{};
    std::uint64_t bytes_released{};
    std::uint32_t degraded_levels{};
    std::uint32_t demand_age_frames{};
    std::uint32_t visibility_age_frames{};
};

struct TextureStreamingDemandPlan final {
    bool valid{};
    bool over_budget{};
    std::string code;
    std::string detail;
    std::uint64_t budget_bytes{};
    std::uint64_t resident_bytes{};
    std::uint64_t demand_bytes{};
    std::uint64_t planned_bytes{};
    std::uint64_t minimum_bytes{};
    std::uint64_t bytes_added{};
    std::uint64_t bytes_released{};
    std::uint32_t degraded_levels{};
    // Highest demand first.  Asset plans themselves are canonicalized by ID;
    // this order is the explicit scheduling/degradation tie-break evidence.
    std::vector<std::string> priority_order;
    std::vector<TextureStreamingDemandAssetPlan> assets;
};

[[nodiscard]] const char* texture_streaming_demand_decision_name(
    TextureStreamingDemandDecision decision) noexcept;

// Computes a deterministic target and resident-budget plan.  Screen demand
// uses a conservative integer footprint-to-mip estimate.  Importance,
// visibility, projected area, authored priority, recency and asset ID form a
// stable ordering; lower-ranked assets are degraded one authored mip at a
// time when the budget cannot hold all requested targets.
[[nodiscard]] TextureStreamingDemandPlan plan_texture_streaming_demand(
    std::span<const TextureStreamingDemandSample> samples,
    std::uint64_t budget_bytes);

} // namespace noemancer
