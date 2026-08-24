#include "engine/texture_streaming_demand.hpp"

#include <algorithm>
#include <limits>
#include <set>
#include <utility>

namespace noemancer {
namespace {

using Sample = TextureStreamingDemandSample;
using Asset = TextureStreamingDemandAssetPlan;

TextureStreamingDemandPlan invalid_plan(std::string code, std::string detail,
                                        const std::uint64_t budget_bytes) {
    TextureStreamingDemandPlan result;
    result.valid = false;
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

bool sum_from(const Sample& sample, const std::uint32_t start, std::uint64_t& result) {
    result = 0U;
    for (std::size_t index = start; index < sample.mip_bytes.size(); ++index) {
        if (!checked_add(result, sample.mip_bytes[index])) return false;
    }
    return true;
}

std::uint64_t sum_range(const Sample& sample, const std::uint32_t first,
                        const std::uint32_t last_exclusive) {
    std::uint64_t result = 0U;
    for (std::uint32_t index = first; index < last_exclusive; ++index) {
        if (!checked_add(result, sample.mip_bytes[index])) return
            std::numeric_limits<std::uint64_t>::max();
    }
    return result;
}

std::uint32_t ceil_div(const std::uint32_t numerator, const std::uint32_t denominator) {
    return numerator / denominator + (numerator % denominator != 0U ? 1U : 0U);
}

std::uint32_t floor_log2(const std::uint32_t value) {
    std::uint32_t result = 0U;
    for (auto remaining = value; remaining > 1U; remaining >>= 1U) ++result;
    return result;
}

std::uint64_t projected_pixels(const Sample& sample) {
    return static_cast<std::uint64_t>(sample.projected_width) *
        static_cast<std::uint64_t>(sample.projected_height);
}

std::uint32_t screen_mip_start(const Sample& sample) {
    if (!sample.visible || sample.projected_width == 0U || sample.projected_height == 0U)
        return sample.maximum_mip_start;

    // Choose the largest downscale ratio across the two dimensions.  This
    // avoids undersampling a tall or wide sprite and remains integer-only.
    const auto width_ratio = ceil_div(sample.base_width, sample.projected_width);
    const auto height_ratio = ceil_div(sample.base_height, sample.projected_height);
    return std::min(sample.maximum_mip_start,
        floor_log2(std::max(width_ratio, height_ratio)));
}

bool lower_rank(const Asset& left, const Asset& right) {
    if (left.importance != right.importance)
        return static_cast<std::uint8_t>(left.importance) <
            static_cast<std::uint8_t>(right.importance);
    if (left.visible != right.visible) return !left.visible;
    if (left.projected_pixels != right.projected_pixels)
        return left.projected_pixels < right.projected_pixels;
    if (left.authored_priority != right.authored_priority)
        return left.authored_priority < right.authored_priority;
    if (left.visibility_age_frames != right.visibility_age_frames)
        return left.visibility_age_frames > right.visibility_age_frames;
    return left.asset_id < right.asset_id;
}

bool higher_rank(const Asset& left, const Asset& right) {
    return lower_rank(right, left);
}

} // namespace

const char* texture_streaming_demand_decision_name(
    const TextureStreamingDemandDecision decision) noexcept {
    switch (decision) {
    case TextureStreamingDemandDecision::keep: return "keep";
    case TextureStreamingDemandDecision::upgrade: return "upgrade";
    case TextureStreamingDemandDecision::downgrade: return "downgrade";
    }
    return "unknown";
}

TextureStreamingDemandPlan plan_texture_streaming_demand(
    const std::span<const TextureStreamingDemandSample> samples,
    const std::uint64_t budget_bytes) {
    TextureStreamingDemandPlan result;
    result.budget_bytes = budget_bytes;

    std::vector<const Sample*> ordered;
    ordered.reserve(samples.size());
    for (const auto& sample : samples) {
        if (sample.asset_id.empty()) return invalid_plan(
            "texture-streaming-demand.asset-id-empty",
            "Every texture demand sample requires a non-empty asset ID.", budget_bytes);
        if (sample.asset_id.size() > kMaxTextureStreamingAssetIdLength) return invalid_plan(
            "texture-streaming-demand.asset-id-too-long",
            "Texture demand asset IDs must be at most 256 bytes.", budget_bytes);
        if (!valid_importance(sample.importance)) return invalid_plan(
            "texture-streaming-demand.importance-invalid",
            "Texture demand importance must be low, normal, high or critical.", budget_bytes);
        if (sample.authored_priority > kMaxTextureStreamingPriority) return invalid_plan(
            "texture-streaming-demand.priority-invalid",
            "Texture demand authored priority must be in the range 0..1000.", budget_bytes);
        if (sample.base_width == 0U || sample.base_height == 0U) return invalid_plan(
            "texture-streaming-demand.base-dimensions-invalid",
            "Texture demand base dimensions must be non-zero.", budget_bytes);
        if (sample.mip_bytes.empty() || sample.mip_bytes.size() > kMaxTextureStreamingMips)
            return invalid_plan("texture-streaming-demand.mip-count-invalid",
                "A texture demand sample must contain 1..32 mip levels.", budget_bytes);
        if (sample.current_mip_start > sample.maximum_mip_start ||
            sample.maximum_mip_start >= sample.mip_bytes.size()) return invalid_plan(
                "texture-streaming-demand.mip-index-invalid",
                "Current and maximum mip starts must be ordered and inside the mip chain.", budget_bytes);
        for (const auto bytes : sample.mip_bytes) if (bytes == 0U) return invalid_plan(
            "texture-streaming-demand.mip-bytes-zero",
            "Every texture demand mip level must have a non-zero byte size.", budget_bytes);
        ordered.push_back(&sample);
    }

    std::sort(ordered.begin(), ordered.end(), [](const Sample* left, const Sample* right) {
        return left->asset_id < right->asset_id;
    });
    for (std::size_t index = 1U; index < ordered.size(); ++index) {
        if (ordered[index - 1U]->asset_id == ordered[index]->asset_id) return invalid_plan(
            "texture-streaming-demand.asset-id-duplicate",
            "Texture demand asset IDs must be unique.", budget_bytes);
    }

    result.assets.reserve(ordered.size());
    for (const auto* sample : ordered) {
        std::uint64_t resident_bytes{};
        std::uint64_t minimum_bytes{};
        if (!sum_from(*sample, sample->current_mip_start, resident_bytes) ||
            !sum_from(*sample, sample->maximum_mip_start, minimum_bytes)) return invalid_plan(
            "texture-streaming-demand.byte-overflow",
            "Texture demand mip byte totals overflow a uint64 value.", budget_bytes);
        if (!checked_add(result.resident_bytes, resident_bytes) ||
            !checked_add(result.minimum_bytes, minimum_bytes)) return invalid_plan(
            "texture-streaming-demand.byte-overflow",
            "Aggregate texture demand byte totals overflow a uint64 value.", budget_bytes);

        const auto screen = screen_mip_start(*sample);
        auto hysteresis = screen;
        if (screen != sample->current_mip_start) {
            const auto hold_frames = screen < sample->current_mip_start
                ? sample->upgrade_hysteresis_frames : sample->downgrade_hysteresis_frames;
            const bool offscreen_grace = !sample->visible &&
                sample->visibility_age_frames < sample->downgrade_hysteresis_frames;
            if (sample->demand_age_frames < hold_frames || offscreen_grace)
                hysteresis = sample->current_mip_start;
        }
        std::uint64_t target_bytes{};
        if (!sum_from(*sample, hysteresis, target_bytes) ||
            !checked_add(result.demand_bytes, target_bytes)) return invalid_plan(
            "texture-streaming-demand.byte-overflow",
            "Texture demand target byte totals overflow a uint64 value.", budget_bytes);

        result.assets.push_back(Asset{
            .asset_id = sample->asset_id,
            .importance = sample->importance,
            .authored_priority = sample->authored_priority,
            .visible = sample->visible,
            .projected_pixels = projected_pixels(*sample),
            .current_mip_start = sample->current_mip_start,
            .screen_mip_start = screen,
            .hysteresis_mip_start = hysteresis,
            .target_mip_start = hysteresis,
            .maximum_mip_start = sample->maximum_mip_start,
            .decision = TextureStreamingDemandDecision::keep,
            .resident_bytes = resident_bytes,
            .target_bytes = target_bytes,
            .bytes_added = 0U,
            .bytes_released = 0U,
            .degraded_levels = 0U,
            .demand_age_frames = sample->demand_age_frames,
            .visibility_age_frames = sample->visibility_age_frames
        });
    }

    std::vector<std::size_t> rank(result.assets.size());
    for (std::size_t index = 0U; index < rank.size(); ++index) rank[index] = index;
    std::sort(rank.begin(), rank.end(), [&result](const std::size_t left, const std::size_t right) {
        return higher_rank(result.assets[left], result.assets[right]);
    });
    result.priority_order.reserve(rank.size());
    for (const auto index : rank) result.priority_order.push_back(result.assets[index].asset_id);

    result.planned_bytes = result.demand_bytes;
    const auto lower = [&result](const std::size_t left, const std::size_t right) {
        return lower_rank(result.assets[left], result.assets[right]);
    };
    std::set<std::size_t, decltype(lower)> candidates(lower);
    for (const auto index : rank) {
        if (result.assets[index].target_mip_start < result.assets[index].maximum_mip_start)
            candidates.insert(index);
    }
    while (result.planned_bytes > budget_bytes && !candidates.empty()) {
        const auto index = *candidates.begin();
        candidates.erase(candidates.begin());
        auto& asset = result.assets[index];
        const auto* sample = ordered[index];
        const auto dropped_level = asset.target_mip_start;
        result.planned_bytes -= sample->mip_bytes[dropped_level];
        asset.target_bytes -= sample->mip_bytes[dropped_level];
        ++asset.target_mip_start;
        ++asset.degraded_levels;
        ++result.degraded_levels;
        if (asset.target_mip_start < asset.maximum_mip_start) candidates.insert(index);
    }

    for (std::size_t index = 0U; index < result.assets.size(); ++index) {
        auto& asset = result.assets[index];
        const auto* sample = ordered[index];
        if (asset.target_mip_start < asset.current_mip_start) {
            asset.decision = TextureStreamingDemandDecision::upgrade;
            asset.bytes_added = sum_range(*sample, asset.target_mip_start, asset.current_mip_start);
        } else if (asset.target_mip_start > asset.current_mip_start) {
            asset.decision = TextureStreamingDemandDecision::downgrade;
            asset.bytes_released = sum_range(*sample, asset.current_mip_start, asset.target_mip_start);
        }
        if (!checked_add(result.bytes_added, asset.bytes_added) ||
            !checked_add(result.bytes_released, asset.bytes_released)) return invalid_plan(
            "texture-streaming-demand.byte-overflow",
            "Aggregate texture demand delta totals overflow a uint64 value.", budget_bytes);
    }
    result.over_budget = result.planned_bytes > budget_bytes;
    result.valid = true;
    if (result.over_budget) {
        result.code = "texture-streaming-demand.over-budget-minimum";
        result.detail = "The budget is below the sum of all authored texture tails.";
    } else {
        result.code = "ok";
        result.detail = result.degraded_levels == 0U
            ? "Texture screen demand fits the resident budget."
            : "Texture screen demand was deterministically degraded to fit the resident budget.";
    }
    return result;
}

} // namespace noemancer
