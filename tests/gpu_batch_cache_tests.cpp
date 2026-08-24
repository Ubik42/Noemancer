#include "engine/gpu_batch_cache.hpp"

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using noemancer::GpuBatchCache;
using noemancer::GpuBatchCacheConfig;
using noemancer::GpuBatchDrawInput;
using noemancer::GpuBatchKey;
using noemancer::GpuBatchPlan;
using noemancer::GpuBatchTextureGeneration;

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

GpuBatchKey key(std::string material_id,
                std::uint64_t material_generation = 1U,
                std::uint64_t raster_generation = 1U) {
    GpuBatchKey result;
    result.geometry_id = "mesh.character";
    result.geometry_generation = 3U;
    result.material_id = std::move(material_id);
    result.material_generation = material_generation;
    result.raster_generation = raster_generation;
    result.textures.push_back(GpuBatchTextureGeneration{
        "base-color", "texture.character", 4U});
    return result;
}

GpuBatchDrawInput draw(std::string draw_id,
                       GpuBatchKey batch_key,
                       std::uint64_t revision = 1U,
                       bool visible = true) {
    GpuBatchDrawInput result;
    result.draw_id = std::move(draw_id);
    result.key = std::move(batch_key);
    result.visibility_eligible = visible;
    result.instance_payload_revision = revision;
    return result;
}

GpuBatchCacheConfig config(std::size_t max_slots,
                           std::size_t minimum_batch_size = 2U,
                           std::size_t max_batches = 1024U,
                           std::size_t max_textures_per_draw = 8U,
                           std::uint32_t instance_stride_bytes = 16U) {
    GpuBatchCacheConfig result;
    result.max_slots = max_slots;
    result.minimum_batch_size = minimum_batch_size;
    result.max_batches = max_batches;
    result.max_textures_per_draw = max_textures_per_draw;
    result.instance_stride_bytes = instance_stride_bytes;
    return result;
}

const noemancer::GpuBatchSlot& slot_for(const GpuBatchPlan& plan,
                                        const std::string& draw_id) {
    const auto found = std::find_if(
        plan.linear_slots.begin(), plan.linear_slots.end(),
        [&draw_id](const noemancer::GpuBatchSlot& slot) {
            return slot.draw_id == draw_id;
        });
    require(found != plan.linear_slots.end(), "draw is missing from linear plan: " + draw_id);
    return *found;
}

void test_initial_order_and_incremental_update() {
    GpuBatchCache cache(config(8U, 1U));
    std::vector<GpuBatchDrawInput> initial{
        draw("draw.c", key("z")),
        draw("draw.b", key("a")),
        draw("draw.a", key("a")),
    };

    const GpuBatchPlan first = cache.update(initial);
    require(first.valid, "initial plan must be valid");
    require(first.full_rebuild && first.full_rebuild_reason == "initial",
            "initial plan must request a full rebuild");
    require(first.statistics.new_draws == 3U && first.statistics.reused_draws == 0U,
            "initial allocation statistics are incorrect");
    require(first.statistics.batch_count == 2U && first.batches[0].instance_count == 2U,
            "same-key draws must form one linear batch");
    require(first.linear_slots[0].draw_id == "draw.a" &&
                first.linear_slots[1].draw_id == "draw.b" &&
                first.linear_slots[2].draw_id == "draw.c",
            "input order must not affect canonical linear order");
    require(first.dirty_byte_ranges.size() == 1U &&
                first.dirty_byte_ranges[0].byte_offset == 0U &&
                first.dirty_byte_ranges[0].byte_count == 48U,
            "initial upload must dirty the complete linear payload");
    require(slot_for(first, "draw.a").stable_slot == 0U &&
                slot_for(first, "draw.b").stable_slot == 1U &&
                slot_for(first, "draw.c").stable_slot == 2U,
            "first publication must allocate the lowest stable slots deterministically");

    std::vector<GpuBatchDrawInput> reordered{
        draw("draw.a", key("a")),
        draw("draw.c", key("z")),
        draw("draw.b", key("a")),
    };
    const GpuBatchPlan second = cache.update(reordered);
    require(second.valid && !second.full_rebuild && second.full_rebuild_reason == "none" &&
                second.statistics.topology_reused && !second.statistics.topology_changed,
            "unchanged update must be incremental");
    require(second.statistics.reused_draws == 3U && second.statistics.moved_draws == 0U &&
                second.statistics.dirty_draws == 0U && second.dirty_byte_ranges.empty(),
            "input reorder alone must reuse all linear payloads");
}

void test_revision_key_change_and_stable_slots() {
    GpuBatchCache cache(config(8U, 1U));
    std::vector<GpuBatchDrawInput> inputs{
        draw("draw.a", key("a")),
        draw("draw.b", key("a")),
        draw("draw.c", key("z")),
    };
    const GpuBatchPlan first = cache.update(inputs);
    require(first.valid, "setup plan must be valid");

    inputs[0].instance_payload_revision = 2U;
    const GpuBatchPlan revision = cache.update(inputs);
    require(revision.statistics.moved_draws == 0U && revision.statistics.dirty_draws == 1U &&
                revision.statistics.topology_reused,
            "payload-only update must not count as a topology move");
    require(revision.dirty_byte_ranges.size() == 1U &&
                revision.dirty_byte_ranges[0].byte_offset == 0U &&
                revision.dirty_byte_ranges[0].byte_count == 16U,
            "payload revision must dirty only its linear instance range");

    inputs[2].key = key("0");
    const GpuBatchPlan moved = cache.update(inputs);
    require(moved.statistics.moved_draws == 3U && moved.statistics.dirty_draws == 3U &&
                moved.statistics.topology_changed && !moved.statistics.topology_reused,
            "a key reorder must move and dirty every affected linear position");
    require(slot_for(moved, "draw.a").stable_slot == 0U &&
                slot_for(moved, "draw.b").stable_slot == 1U &&
                slot_for(moved, "draw.c").stable_slot == 2U,
            "stable slots must survive key changes and reordering");
}

void test_remove_add_visibility_and_range_clearing() {
    GpuBatchCache cache(config(4U, 1U));
    std::vector<GpuBatchDrawInput> inputs{
        draw("draw.a", key("a")),
        draw("draw.b", key("b")),
        draw("draw.c", key("c")),
    };
    const GpuBatchPlan first = cache.update(inputs);
    require(first.valid, "setup plan must be valid");

    inputs = {
        draw("draw.a", key("a")),
        draw("draw.c", key("c")),
        draw("draw.d", key("b")),
    };
    const GpuBatchPlan changed = cache.update(inputs);
    require(changed.statistics.removed_draws == 1U && changed.statistics.new_draws == 1U &&
                changed.statistics.reused_draws == 2U,
            "remove/add statistics are incorrect");
    require(changed.released_stable_slots.size() == 1U &&
                changed.released_stable_slots[0] == 1U &&
                slot_for(changed, "draw.d").stable_slot == 1U,
            "released stable slots must be deterministic and reusable");
    require(changed.dirty_byte_ranges.size() == 1U &&
                changed.dirty_byte_ranges[0].byte_offset == 16U &&
                changed.dirty_byte_ranges[0].byte_count == 16U,
            "compaction must report the changed current range without an out-of-bounds tail range");

    inputs = {
        draw("draw.a", key("a")),
        draw("draw.c", key("c")),
        draw("draw.d", key("b"), 1U, false),
    };
    const GpuBatchPlan hidden = cache.update(inputs);
    require(hidden.statistics.eligible_draws == 2U && hidden.statistics.removed_draws == 1U &&
                hidden.linear_slots.size() == 2U && hidden.dirty_byte_ranges.size() == 1U &&
                hidden.dirty_byte_ranges[0].byte_offset == 16U &&
                hidden.dirty_byte_ranges[0].byte_count == 16U,
            "visibility must remove ineligible draws from the linear plan");
    require(std::find_if(hidden.linear_slots.begin(), hidden.linear_slots.end(),
                         [](const noemancer::GpuBatchSlot& slot) {
                             return slot.draw_id == "draw.d";
                         }) == hidden.linear_slots.end(),
            "ineligible draw must not be emitted");
}

void test_rejected_updates_do_not_mutate_cache() {
    GpuBatchCache cache(config(2U, 1U));
    std::vector<GpuBatchDrawInput> valid_input{draw("draw.a", key("a"))};
    const GpuBatchPlan initial = cache.update(valid_input);
    require(initial.valid && cache.has_plan(), "setup plan must be valid");

    std::vector<GpuBatchDrawInput> duplicate{
        draw("draw.a", key("a")),
        draw("draw.a", key("b")),
    };
    const GpuBatchPlan invalid = cache.update(duplicate);
    require(!invalid.valid && invalid.code == "invalid-input" && cache.has_plan(),
            "duplicate draw IDs must be rejected without clearing the cache");

    std::vector<GpuBatchDrawInput> unchanged{draw("draw.a", key("a"))};
    const GpuBatchPlan after_failure = cache.update(unchanged);
    require(after_failure.valid && after_failure.statistics.dirty_draws == 0U,
            "rejected updates must not perturb the last published state");
}

void test_group_threshold_capacity_and_batch_cap() {
    GpuBatchCache cache(config(4U, 2U, 2U));
    std::vector<GpuBatchDrawInput> inputs{
        draw("small.a", key("a")),
        draw("large.b1", key("b")),
        draw("large.b2", key("b")),
        draw("large.c1", key("c")),
        draw("large.c2", key("c")),
        draw("large.c3", key("c")),
    };
    const GpuBatchPlan capped = cache.update(inputs);
    require(capped.valid && capped.statistics.eligible_draws == 6U &&
                capped.statistics.fallback_draws == 2U &&
                capped.statistics.batch_count == 2U && capped.linear_slots.size() == 4U,
            "small groups and capacity tail must be counted as fallback");
    require(capped.batches[0].instance_count == 2U &&
                capped.batches[1].instance_count == 2U,
            "selected batches must meet the threshold and stop at capacity");
    require(capped.statistics.new_draws == 4U,
            "fallback draws must not consume stable slots");

    std::vector<GpuBatchDrawInput> over_batch_cap{
        draw("large.b1", key("b")),
        draw("large.b2", key("b")),
        draw("large.c1", key("c")),
        draw("large.c2", key("c")),
        draw("large.d1", key("d")),
        draw("large.d2", key("d")),
    };
    const GpuBatchPlan batch_capped = cache.update(over_batch_cap);
    require(batch_capped.valid && batch_capped.statistics.batch_count == 2U &&
                batch_capped.statistics.fallback_draws == 2U,
            "max_batches must route later qualifying groups to fallback");
}

void test_invalid_configuration() {
    GpuBatchCache cache(config(0U));
    const GpuBatchPlan invalid = cache.update({});
    require(!invalid.valid && invalid.code == "invalid-config" && invalid.full_rebuild,
            "invalid configuration must produce a structured failure");
}

} // namespace

int main() {
    try {
        test_initial_order_and_incremental_update();
        test_revision_key_change_and_stable_slots();
        test_remove_add_visibility_and_range_clearing();
        test_rejected_updates_do_not_mutate_cache();
        test_group_threshold_capacity_and_batch_cap();
        test_invalid_configuration();
    } catch (const std::exception& error) {
        std::cerr << "gpu_batch_cache_tests: " << error.what() << '\n';
        return 1;
    }
    std::cout << "gpu_batch_cache_tests: ok\n";
    return 0;
}
