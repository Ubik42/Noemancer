#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace noemancer {

// Mip level zero is the highest-detail level.  A mip start selects that level
// and every coarser level through the tail.  The planner deliberately keeps
// this vocabulary independent of any GPU API or texture container.
enum class TextureStreamingImportance : std::uint8_t {
    low = 0,
    normal = 1,
    high = 2,
    critical = 3
};

inline constexpr std::uint32_t kMaxTextureStreamingMips = 32U;
inline constexpr std::uint32_t kMaxTextureStreamingPriority = 1000U;
inline constexpr std::size_t kMaxTextureStreamingAssetIdLength = 256U;

struct TextureStreamingAssetRequest final {
    std::string asset_id;
    TextureStreamingImportance importance{TextureStreamingImportance::normal};
    // Stable authored tie-break inside one importance class.  Lower values
    // are degraded first when the budget is under pressure.
    std::uint32_t authored_priority{};
    std::uint32_t requested_mip_start{};
    // Highest permitted mip start (the lowest permitted detail).  The
    // planner never advances target_mip_start beyond this tail boundary.
    std::uint32_t maximum_mip_start{};
    // Bytes for levels [0, mip_bytes.size()), from highest detail to tail.
    std::vector<std::uint64_t> mip_bytes;
};

struct TextureStreamingAssetPlan final {
    std::string asset_id;
    TextureStreamingImportance importance{TextureStreamingImportance::normal};
    std::uint32_t authored_priority{};
    std::uint32_t requested_mip_start{};
    std::uint32_t target_mip_start{};
    std::uint32_t maximum_mip_start{};
    std::uint64_t requested_bytes{};
    std::uint64_t planned_bytes{};
    std::uint64_t minimum_bytes{};
    std::uint32_t degraded_levels{};
};

struct TextureStreamingBudgetPlan final {
    // valid describes input validation.  A valid plan may still be
    // over_budget when even the authored tail cannot fit.
    bool valid{};
    bool over_budget{};
    std::string code;
    std::string detail;
    std::uint64_t budget_bytes{};
    std::uint64_t requested_bytes{};
    std::uint64_t planned_bytes{};
    std::uint64_t minimum_bytes{};
    // Demand pressure before degradation: requested_bytes / budget_bytes.
    // It is +infinity when a non-empty request is planned against a zero
    // budget, and zero for an empty request with a zero budget.
    double pressure{};
    std::uint32_t degraded_levels{};
    std::vector<TextureStreamingAssetPlan> assets;
};

[[nodiscard]] std::string texture_streaming_importance_name(
    TextureStreamingImportance importance);

[[nodiscard]] std::optional<TextureStreamingImportance>
texture_streaming_importance_from_name(std::string_view name);

// Plans a deterministic mip-tail budget.  The input order does not affect the
// result: output assets are canonicalized by asset_id and degradation chooses
// lowest importance, then lowest authored priority, then lexical asset ID.
// Each degradation step advances one mip level and never crosses the request's
// maximum_mip_start.  No SDL, KTX, Basis or backend type crosses this API.
[[nodiscard]] TextureStreamingBudgetPlan plan_texture_streaming_budget(
    std::span<const TextureStreamingAssetRequest> requests,
    std::uint64_t budget_bytes);

} // namespace noemancer
