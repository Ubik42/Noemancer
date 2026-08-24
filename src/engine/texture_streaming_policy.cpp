#include "engine/texture_streaming_policy.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <set>
#include <utility>

namespace noemancer {
namespace {

using Request = TextureStreamingAssetRequest;
using AssetPlan = TextureStreamingAssetPlan;

TextureStreamingBudgetPlan invalid_plan(std::string code, std::string detail,
                                        const std::uint64_t budget_bytes) {
    TextureStreamingBudgetPlan result;
    result.valid = false;
    result.over_budget = false;
    result.code = std::move(code);
    result.detail = std::move(detail);
    result.budget_bytes = budget_bytes;
    return result;
}

bool valid_importance(const TextureStreamingImportance importance) {
    switch (importance) {
    case TextureStreamingImportance::low:
    case TextureStreamingImportance::normal:
    case TextureStreamingImportance::high:
    case TextureStreamingImportance::critical:
        return true;
    }
    return false;
}

bool checked_add(std::uint64_t& total, const std::uint64_t value) {
    if (value > std::numeric_limits<std::uint64_t>::max() - total) return false;
    total += value;
    return true;
}

bool sum_from(const Request& request, const std::uint32_t start, std::uint64_t& result) {
    result = 0U;
    for (std::size_t index = start; index < request.mip_bytes.size(); ++index) {
        if (!checked_add(result, request.mip_bytes[index])) return false;
    }
    return true;
}

bool add_to_total(std::uint64_t& total, const std::uint64_t value) {
    return checked_add(total, value);
}

} // namespace

std::string texture_streaming_importance_name(const TextureStreamingImportance importance) {
    switch (importance) {
    case TextureStreamingImportance::low: return "low";
    case TextureStreamingImportance::normal: return "normal";
    case TextureStreamingImportance::high: return "high";
    case TextureStreamingImportance::critical: return "critical";
    }
    return "unknown";
}

std::optional<TextureStreamingImportance>
texture_streaming_importance_from_name(const std::string_view name) {
    if (name == "low") return TextureStreamingImportance::low;
    if (name == "normal") return TextureStreamingImportance::normal;
    if (name == "high") return TextureStreamingImportance::high;
    if (name == "critical") return TextureStreamingImportance::critical;
    return std::nullopt;
}

TextureStreamingBudgetPlan plan_texture_streaming_budget(
    const std::span<const TextureStreamingAssetRequest> requests,
    const std::uint64_t budget_bytes) {
    TextureStreamingBudgetPlan result;
    result.budget_bytes = budget_bytes;

    std::vector<const Request*> ordered;
    ordered.reserve(requests.size());
    for (const auto& request : requests) {
        if (request.asset_id.empty()) {
            return invalid_plan("texture-streaming.asset-id-empty",
                "Every texture streaming request requires a non-empty asset ID.", budget_bytes);
        }
        if (request.asset_id.size() > kMaxTextureStreamingAssetIdLength) {
            return invalid_plan("texture-streaming.asset-id-too-long",
                "Texture streaming asset IDs must be at most 256 bytes.", budget_bytes);
        }
        if (!valid_importance(request.importance)) {
            return invalid_plan("texture-streaming.importance-invalid",
                "Texture streaming importance must be low, normal, high or critical.", budget_bytes);
        }
        if (request.authored_priority > kMaxTextureStreamingPriority) {
            return invalid_plan("texture-streaming.priority-invalid",
                "Authored texture streaming priority must be in the range 0..1000.", budget_bytes);
        }
        if (request.mip_bytes.empty() || request.mip_bytes.size() > kMaxTextureStreamingMips) {
            return invalid_plan("texture-streaming.mip-count-invalid",
                "A texture streaming request must contain 1..32 mip levels.", budget_bytes);
        }
        if (request.requested_mip_start > request.maximum_mip_start ||
            request.maximum_mip_start >= request.mip_bytes.size()) {
            return invalid_plan("texture-streaming.mip-index-invalid",
                "Requested and maximum mip starts must be ordered and inside the mip chain.", budget_bytes);
        }
        for (const auto bytes : request.mip_bytes) {
            if (bytes == 0U) {
                return invalid_plan("texture-streaming.mip-bytes-zero",
                    "Every mip level must have a non-zero byte size.", budget_bytes);
            }
        }
        ordered.push_back(&request);
    }

    std::sort(ordered.begin(), ordered.end(), [](const Request* left, const Request* right) {
        return left->asset_id < right->asset_id;
    });
    for (std::size_t index = 1; index < ordered.size(); ++index) {
        if (ordered[index - 1U]->asset_id == ordered[index]->asset_id) {
            return invalid_plan("texture-streaming.asset-id-duplicate",
                "Texture streaming asset IDs must be unique.", budget_bytes);
        }
    }

    result.assets.reserve(ordered.size());
    for (const auto* request : ordered) {
        std::uint64_t requested_bytes{};
        std::uint64_t minimum_bytes{};
        if (!sum_from(*request, request->requested_mip_start, requested_bytes) ||
            !sum_from(*request, request->maximum_mip_start, minimum_bytes)) {
            return invalid_plan("texture-streaming.byte-overflow",
                "The mip byte totals overflow a uint64 value.", budget_bytes);
        }
        if (!add_to_total(result.requested_bytes, requested_bytes) ||
            !add_to_total(result.minimum_bytes, minimum_bytes)) {
            return invalid_plan("texture-streaming.byte-overflow",
                "The aggregate mip byte totals overflow a uint64 value.", budget_bytes);
        }
        result.assets.push_back(AssetPlan{
            .asset_id = request->asset_id,
            .importance = request->importance,
            .authored_priority = request->authored_priority,
            .requested_mip_start = request->requested_mip_start,
            .target_mip_start = request->requested_mip_start,
            .maximum_mip_start = request->maximum_mip_start,
            .requested_bytes = requested_bytes,
            .planned_bytes = requested_bytes,
            .minimum_bytes = minimum_bytes,
            .degraded_levels = 0U
        });
    }

    result.planned_bytes = result.requested_bytes;
    if (budget_bytes == 0U) {
        result.pressure = result.requested_bytes == 0U
            ? 0.0
            : std::numeric_limits<double>::infinity();
    } else {
        result.pressure = static_cast<double>(result.requested_bytes) /
            static_cast<double>(budget_bytes);
    }

    // A set keeps the degradation order stable while allowing the same asset
    // to consume multiple one-level steps before the next lower-priority
    // asset is touched.  The lexical ID is the final tie-break.
    const auto less = [&result](const std::size_t left, const std::size_t right) {
        const auto& a = result.assets[left];
        const auto& b = result.assets[right];
        if (a.importance != b.importance)
            return static_cast<std::uint8_t>(a.importance) < static_cast<std::uint8_t>(b.importance);
        if (a.authored_priority != b.authored_priority)
            return a.authored_priority < b.authored_priority;
        return a.asset_id < b.asset_id;
    };
    std::set<std::size_t, decltype(less)> candidates(less);
    for (std::size_t index = 0; index < result.assets.size(); ++index) {
        if (result.assets[index].target_mip_start < result.assets[index].maximum_mip_start)
            candidates.insert(index);
    }

    while (result.planned_bytes > budget_bytes && !candidates.empty()) {
        const auto candidate = *candidates.begin();
        candidates.erase(candidates.begin());
        auto& asset = result.assets[candidate];
        const auto dropped_level = asset.target_mip_start;
        // requested_mip_start <= target < maximum_mip_start guarantees that
        // the dropped level exists and remains part of the planned range.
        result.planned_bytes -= ordered[candidate]->mip_bytes[dropped_level];
        asset.planned_bytes -= ordered[candidate]->mip_bytes[dropped_level];
        ++asset.target_mip_start;
        ++asset.degraded_levels;
        ++result.degraded_levels;
        if (asset.target_mip_start < asset.maximum_mip_start) candidates.insert(candidate);
    }

    result.over_budget = result.planned_bytes > budget_bytes;
    result.valid = true;
    if (result.over_budget) {
        result.code = "texture-streaming.over-budget-minimum";
        result.detail = "The budget is below the sum of all authored mip tails; minimum residency was planned but the budget cannot be met.";
    } else {
        result.code = "ok";
        result.detail = result.degraded_levels == 0U
            ? "Requested texture mip starts fit within the budget."
            : "Texture mip starts were degraded deterministically to fit the budget.";
    }
    return result;
}

} // namespace noemancer
