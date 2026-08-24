#include "engine/texture_streaming_demand.hpp"

#include <iostream>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace noemancer;

bool check(const bool condition, const std::string& message) {
    if (!condition) std::cerr << message << '\n';
    return condition;
}

TextureStreamingDemandSample sample(std::string id, const TextureStreamingImportance importance,
    const std::uint32_t priority, const std::uint32_t current, const bool visible,
    const std::uint32_t projected_width, const std::uint32_t projected_height,
    const std::uint32_t demand_age, const std::uint32_t visibility_age = 0U) {
    return TextureStreamingDemandSample{
        .asset_id = std::move(id),
        .importance = importance,
        .authored_priority = priority,
        .base_width = 1024U,
        .base_height = 1024U,
        .projected_width = projected_width,
        .projected_height = projected_height,
        .visible = visible,
        .current_mip_start = current,
        .maximum_mip_start = 4U,
        .mip_bytes = {160U, 80U, 40U, 20U, 10U},
        .demand_age_frames = demand_age,
        .visibility_age_frames = visibility_age,
        .upgrade_hysteresis_frames = 2U,
        .downgrade_hysteresis_frames = 4U
    };
}

const TextureStreamingDemandAssetPlan* find_asset(const TextureStreamingDemandPlan& plan,
    const std::string_view id) {
    for (const auto& asset : plan.assets) if (asset.asset_id == id) return &asset;
    return nullptr;
}

bool test_screen_demand_and_hysteresis() {
    const auto upgrade_held = sample("asset.hero", TextureStreamingImportance::high, 10U,
        2U, true, 1024U, 1024U, 1U);
    const auto upgrade_ready = sample("asset.hero", TextureStreamingImportance::high, 10U,
        2U, true, 1024U, 1024U, 2U);
    const auto held_plan = plan_texture_streaming_demand(
        std::vector<TextureStreamingDemandSample>{upgrade_held}, 1000U);
    const auto ready_plan = plan_texture_streaming_demand(
        std::vector<TextureStreamingDemandSample>{upgrade_ready}, 1000U);
    const auto* held = find_asset(held_plan, "asset.hero");
    const auto* ready = find_asset(ready_plan, "asset.hero");
    if (!check(held != nullptr && ready != nullptr && held->screen_mip_start == 0U &&
               held->hysteresis_mip_start == 2U && held->decision == TextureStreamingDemandDecision::keep,
               "Upgrade hysteresis did not hold a one-frame demand spike.")) return false;
    if (!check(ready->hysteresis_mip_start == 0U && ready->decision == TextureStreamingDemandDecision::upgrade &&
               ready->bytes_added == 240U,
               "Stable screen demand did not schedule the expected texture upgrade.")) return false;

    const auto downgrade_held = sample("asset.hero", TextureStreamingImportance::high, 10U,
        0U, true, 64U, 64U, 1U);
    const auto downgrade_ready = sample("asset.hero", TextureStreamingImportance::high, 10U,
        0U, true, 64U, 64U, 4U);
    const auto held_downgrade = plan_texture_streaming_demand(
        std::vector<TextureStreamingDemandSample>{downgrade_held}, 1000U);
    const auto ready_downgrade = plan_texture_streaming_demand(
        std::vector<TextureStreamingDemandSample>{downgrade_ready}, 1000U);
    const auto* held_tail = find_asset(held_downgrade, "asset.hero");
    const auto* ready_tail = find_asset(ready_downgrade, "asset.hero");
    return check(held_tail != nullptr && held_tail->decision == TextureStreamingDemandDecision::keep &&
                 ready_tail != nullptr && ready_tail->screen_mip_start == 4U &&
                 ready_tail->decision == TextureStreamingDemandDecision::downgrade &&
                 ready_tail->bytes_released == 300U,
                 "Downgrade hysteresis did not release only after stable small-screen demand.");
}

bool test_visibility_aging() {
    const auto recent = sample("asset.recent", TextureStreamingImportance::normal, 0U,
        0U, false, 0U, 0U, 20U, 2U);
    const auto aged = sample("asset.aged", TextureStreamingImportance::normal, 0U,
        0U, false, 0U, 0U, 20U, 4U);
    const auto plan = plan_texture_streaming_demand(
        std::vector<TextureStreamingDemandSample>{aged, recent}, 1000U);
    const auto* recent_plan = find_asset(plan, "asset.recent");
    const auto* aged_plan = find_asset(plan, "asset.aged");
    if (!check(recent_plan != nullptr && aged_plan != nullptr &&
               recent_plan->decision == TextureStreamingDemandDecision::keep &&
               aged_plan->decision == TextureStreamingDemandDecision::downgrade,
               "Visibility aging did not preserve recent offscreen content or evict aged content.")) return false;
    return check(plan.priority_order.size() == 2U && plan.priority_order[0] == "asset.recent" &&
                 plan.priority_order[1] == "asset.aged",
                 "Visibility recency did not produce a stable priority order.");
}

bool test_screen_priority_budget_and_stability() {
    auto critical_small = sample("asset.critical-small", TextureStreamingImportance::critical,
        0U, 4U, true, 16U, 16U, 4U);
    auto normal_large = sample("asset.normal-large", TextureStreamingImportance::normal,
        500U, 4U, true, 1024U, 1024U, 4U);
    auto normal_small = sample("asset.normal-small", TextureStreamingImportance::normal,
        500U, 4U, true, 256U, 256U, 4U);
    const std::vector<TextureStreamingDemandSample> ordered{normal_small, critical_small, normal_large};
    const std::vector<TextureStreamingDemandSample> reversed{normal_large, critical_small, normal_small};
    const auto a = plan_texture_streaming_demand(ordered, 330U);
    const auto b = plan_texture_streaming_demand(reversed, 330U);
    if (!check(a.valid && b.valid && !a.over_budget && a.assets.size() == 3U,
               "Screen-priority budget plan was not valid.")) return false;
    if (!check(a.priority_order == std::vector<std::string>{
            "asset.critical-small", "asset.normal-large", "asset.normal-small"},
            "Importance and projected screen area did not order demand deterministically.")) return false;
    const auto* large = find_asset(a, "asset.normal-large");
    const auto* small = find_asset(a, "asset.normal-small");
    const auto* reversed_large = find_asset(b, "asset.normal-large");
    return check(large != nullptr && small != nullptr && reversed_large != nullptr &&
                 large->target_mip_start == 0U && small->target_mip_start == 4U &&
                 reversed_large->target_mip_start == large->target_mip_start &&
                 a.planned_bytes == b.planned_bytes,
                 "Resident budget degradation was not stable or did not protect screen demand.");
}

bool test_invalid_and_tail_budget() {
    auto tail = sample("asset.tail", TextureStreamingImportance::low, 0U,
        0U, false, 0U, 0U, 20U, 4U);
    const auto over = plan_texture_streaming_demand(
        std::vector<TextureStreamingDemandSample>{tail}, 5U);
    if (!check(over.valid && over.over_budget && over.code == "texture-streaming-demand.over-budget-minimum" &&
               over.planned_bytes == 10U,
               "Budget below the authored tail was not reported explicitly.")) return false;
    tail.current_mip_start = 5U;
    const auto invalid = plan_texture_streaming_demand(
        std::vector<TextureStreamingDemandSample>{tail}, 100U);
    return check(!invalid.valid && invalid.code == "texture-streaming-demand.mip-index-invalid",
                 "Invalid current mip state was accepted.");
}

} // namespace

int main() {
    if (!test_screen_demand_and_hysteresis()) return 1;
    if (!test_visibility_aging()) return 2;
    if (!test_screen_priority_budget_and_stability()) return 3;
    if (!test_invalid_and_tail_budget()) return 4;
    std::cout << "Texture streaming demand planner tests passed\n";
    return 0;
}
