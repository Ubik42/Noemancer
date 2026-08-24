#include "engine/texture_streaming_policy.hpp"

#include <cmath>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

namespace {

using namespace noemancer;

bool check(const bool condition, const std::string& message) {
    if (!condition) std::cerr << message << '\n';
    return condition;
}

TextureStreamingAssetRequest request(std::string id, const TextureStreamingImportance importance,
    const std::uint32_t priority, const std::uint32_t requested, const std::uint32_t maximum,
    std::vector<std::uint64_t> bytes) {
    return TextureStreamingAssetRequest{
        .asset_id = std::move(id),
        .importance = importance,
        .authored_priority = priority,
        .requested_mip_start = requested,
        .maximum_mip_start = maximum,
        .mip_bytes = std::move(bytes)
    };
}

const TextureStreamingAssetPlan* find_asset(const TextureStreamingBudgetPlan& plan,
    const std::string_view id) {
    for (const auto& asset : plan.assets) if (asset.asset_id == id) return &asset;
    return nullptr;
}

bool test_deterministic_order_and_degradation() {
    auto first = request("asset.z", TextureStreamingImportance::low, 10U, 0U, 2U, {60U, 30U, 10U});
    auto second = request("asset.a", TextureStreamingImportance::normal, 10U, 0U, 2U, {60U, 30U, 10U});
    const std::vector<TextureStreamingAssetRequest> ordered{first, second};
    const std::vector<TextureStreamingAssetRequest> reversed{second, first};
    const auto a = plan_texture_streaming_budget(ordered, 120U);
    const auto b = plan_texture_streaming_budget(reversed, 120U);
    if (!check(a.valid && b.valid && a.code == "ok" && b.code == "ok",
               "Deterministic plan was not valid.")) return false;
    if (!check(a.assets.size() == 2U && a.assets[0].asset_id == "asset.a" &&
               a.assets[1].asset_id == "asset.z" && a.assets.size() == b.assets.size(),
               "Plan output was not canonicalized by asset ID.")) return false;
    const auto* a_low = find_asset(a, "asset.z");
    const auto* a_normal = find_asset(a, "asset.a");
    const auto* b_low = find_asset(b, "asset.z");
    if (!check(a_low != nullptr && a_normal != nullptr && b_low != nullptr,
               "Deterministic plan lost an asset.")) return false;
    if (!check(a_low->target_mip_start == 2U && a_normal->target_mip_start == 0U &&
               a.planned_bytes == 110U && a.minimum_bytes == 20U &&
               a.requested_bytes == 200U && a.degraded_levels == 2U,
               "Lowest-importance asset was not degraded one level at a time.")) return false;
    if (!check(a.pressure > 1.66 && a.pressure < 1.67 &&
               a_low->target_mip_start == b_low->target_mip_start &&
               a.planned_bytes == b.planned_bytes,
               "Demand pressure or input-order determinism was incorrect.")) return false;
    return true;
}

bool test_priority_and_importance() {
    const auto low_priority = request("asset.low-priority", TextureStreamingImportance::normal,
        1U, 0U, 1U, {50U, 25U});
    const auto high_priority = request("asset.high-priority", TextureStreamingImportance::normal,
        900U, 0U, 1U, {50U, 25U});
    const auto priority_plan = plan_texture_streaming_budget(
        std::vector<TextureStreamingAssetRequest>{high_priority, low_priority}, 100U);
    const auto* low = find_asset(priority_plan, "asset.low-priority");
    const auto* high = find_asset(priority_plan, "asset.high-priority");
    if (!check(priority_plan.valid && !priority_plan.over_budget && low != nullptr && high != nullptr,
               "Priority plan was not valid.")) return false;
    if (!check(low->target_mip_start == 1U && high->target_mip_start == 0U,
               "Lowest authored priority was not degraded first.")) return false;

    const auto critical = request("asset.critical", TextureStreamingImportance::critical,
        0U, 0U, 1U, {50U, 25U});
    const auto low_importance = request("asset.low", TextureStreamingImportance::low,
        1000U, 0U, 1U, {50U, 25U});
    const auto importance_plan = plan_texture_streaming_budget(
        std::vector<TextureStreamingAssetRequest>{critical, low_importance}, 100U);
    const auto* critical_plan = find_asset(importance_plan, "asset.critical");
    const auto* low_plan = find_asset(importance_plan, "asset.low");
    if (!check(critical_plan != nullptr && low_plan != nullptr &&
               low_plan->target_mip_start == 1U && critical_plan->target_mip_start == 0U,
               "Importance class did not protect critical content.")) return false;
    return check(texture_streaming_importance_name(TextureStreamingImportance::high) == "high" &&
                 texture_streaming_importance_from_name("critical") == TextureStreamingImportance::critical &&
                 !texture_streaming_importance_from_name("unknown").has_value(),
                 "Importance name conversion was not stable.");
}

bool test_tail_lower_bound_and_budget_below_tail() {
    const auto tail = request("asset.tail", TextureStreamingImportance::low, 0U,
        0U, 1U, {100U, 50U, 10U});
    const auto plan = plan_texture_streaming_budget(
        std::vector<TextureStreamingAssetRequest>{tail}, 30U);
    const auto* asset = find_asset(plan, "asset.tail");
    if (!check(plan.valid && plan.over_budget && plan.code == "texture-streaming.over-budget-minimum" &&
               asset != nullptr && asset->target_mip_start == 1U && asset->planned_bytes == 60U &&
               plan.minimum_bytes == 60U && plan.planned_bytes == 60U,
               "Budget below authored tail was not reported explicitly.")) return false;
    return check(plan.pressure > 5.3 && plan.pressure < 5.34,
                 "Budget pressure did not use requested demand.");
}

bool test_invalid_inputs() {
    const auto duplicate = request("asset.same", TextureStreamingImportance::normal, 0U, 0U, 0U, {1U});
    const auto duplicate_plan = plan_texture_streaming_budget(
        std::vector<TextureStreamingAssetRequest>{duplicate, duplicate}, 1U);
    if (!check(!duplicate_plan.valid && duplicate_plan.code == "texture-streaming.asset-id-duplicate",
               "Duplicate asset IDs were accepted.")) return false;

    const auto bad_priority = request("asset.priority", TextureStreamingImportance::normal,
        1001U, 0U, 0U, {1U});
    if (!check(!plan_texture_streaming_budget(
                   std::vector<TextureStreamingAssetRequest>{bad_priority}, 1U).valid,
               "Priority above 1000 was accepted.")) return false;

    const auto bad_indices = request("asset.indices", TextureStreamingImportance::normal,
        0U, 2U, 1U, {4U, 2U, 1U});
    if (!check(!plan_texture_streaming_budget(
                   std::vector<TextureStreamingAssetRequest>{bad_indices}, 1U).valid,
               "Invalid mip start ordering was accepted.")) return false;

    const auto zero_mip = request("asset.zero", TextureStreamingImportance::normal,
        0U, 0U, 0U, {0U});
    if (!check(!plan_texture_streaming_budget(
                   std::vector<TextureStreamingAssetRequest>{zero_mip}, 1U).valid,
               "Zero-sized mip was accepted.")) return false;

    std::vector<std::uint64_t> too_many_mips(kMaxTextureStreamingMips + 1U, 1U);
    const auto too_many = request("asset.mips", TextureStreamingImportance::normal,
        0U, 0U, 0U, std::move(too_many_mips));
    return check(!plan_texture_streaming_budget(
                      std::vector<TextureStreamingAssetRequest>{too_many}, 1U).valid,
                  "More than 32 mip levels were accepted.");
}

} // namespace

int main() {
    if (!test_deterministic_order_and_degradation()) return 1;
    if (!test_priority_and_importance()) return 2;
    if (!test_tail_lower_bound_and_budget_below_tail()) return 3;
    if (!test_invalid_inputs()) return 4;
    std::cout << "Texture streaming policy planner tests passed\n";
    return 0;
}
