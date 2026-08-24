#include "engine/gpu_batch_cache.hpp"

#include <algorithm>
#include <functional>
#include <limits>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace noemancer {
namespace {

[[nodiscard]] bool texture_less(const GpuBatchTextureGeneration& lhs,
                                const GpuBatchTextureGeneration& rhs) noexcept {
    if (lhs.semantic != rhs.semantic) {
        return lhs.semantic < rhs.semantic;
    }
    if (lhs.stable_id != rhs.stable_id) {
        return lhs.stable_id < rhs.stable_id;
    }
    return lhs.resource_generation < rhs.resource_generation;
}

[[nodiscard]] bool key_less(const GpuBatchKey& lhs, const GpuBatchKey& rhs) noexcept {
    if (lhs.geometry_id != rhs.geometry_id) {
        return lhs.geometry_id < rhs.geometry_id;
    }
    if (lhs.geometry_generation != rhs.geometry_generation) {
        return lhs.geometry_generation < rhs.geometry_generation;
    }
    if (lhs.material_id != rhs.material_id) {
        return lhs.material_id < rhs.material_id;
    }
    if (lhs.material_generation != rhs.material_generation) {
        return lhs.material_generation < rhs.material_generation;
    }
    if (lhs.raster_generation != rhs.raster_generation) {
        return lhs.raster_generation < rhs.raster_generation;
    }
    if (lhs.textures.size() != rhs.textures.size()) {
        return lhs.textures.size() < rhs.textures.size();
    }
    for (std::size_t index = 0; index < lhs.textures.size(); ++index) {
        if (lhs.textures[index] == rhs.textures[index]) {
            continue;
        }
        return texture_less(lhs.textures[index], rhs.textures[index]);
    }
    return false;
}

struct StringViewHash final {
    [[nodiscard]] std::size_t operator()(const std::string_view value) const noexcept {
        return std::hash<std::string_view>{}(value);
    }
};

struct StringViewEqual final {
    [[nodiscard]] bool operator()(const std::string_view lhs,
                                  const std::string_view rhs) const noexcept {
        return lhs == rhs;
    }
};

[[nodiscard]] GpuBatchPlan failure_plan(const GpuBatchCacheConfig& config,
                                        const std::size_t input_count,
                                        std::string code,
                                        std::string detail) {
    GpuBatchPlan plan;
    plan.valid = false;
    plan.code = std::move(code);
    plan.detail = std::move(detail);
    plan.instance_stride_bytes = config.instance_stride_bytes;
    plan.full_rebuild = true;
    plan.full_rebuild_reason = plan.code;
    plan.statistics.input_draws = input_count;
    return plan;
}

[[nodiscard]] bool valid_config(const GpuBatchCacheConfig& config) noexcept {
    if (config.max_slots == 0U || config.minimum_batch_size == 0U ||
        config.max_slots > static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max()) ||
        config.instance_stride_bytes == 0U) {
        return false;
    }

    constexpr auto max_u64 = std::numeric_limits<std::uint64_t>::max();
    return static_cast<std::uint64_t>(config.max_slots) <=
               max_u64 / static_cast<std::uint64_t>(config.instance_stride_bytes);
}

[[nodiscard]] bool same_linear_item(
    const std::size_t index,
    const std::vector<const GpuBatchKey*>& old_keys,
    const std::vector<std::uint64_t>& old_revisions,
    const std::vector<const std::string*>& old_draw_ids,
    const std::vector<const GpuBatchKey*>& new_keys,
    const std::vector<std::uint64_t>& new_revisions,
    const std::vector<std::string_view>& new_draw_ids) {
    if (index >= old_keys.size() || index >= old_revisions.size() ||
        index >= old_draw_ids.size() || index >= new_keys.size() ||
        index >= new_revisions.size() || index >= new_draw_ids.size() ||
        old_keys[index] == nullptr || new_keys[index] == nullptr ||
        old_draw_ids[index] == nullptr) {
        return false;
    }
    return *old_keys[index] == *new_keys[index] &&
           old_revisions[index] == new_revisions[index] &&
           std::string_view(*old_draw_ids[index]) == new_draw_ids[index];
}

void append_dirty_ranges(const std::vector<unsigned char>& dirty,
                         const std::uint32_t instance_stride_bytes,
                         std::vector<GpuBatchDirtyRange>& output) {
    std::size_t range_begin = dirty.size();
    for (std::size_t index = 0; index <= dirty.size(); ++index) {
        const bool is_dirty = index < dirty.size() && dirty[index] != 0U;
        if (!is_dirty && range_begin == dirty.size()) {
            continue;
        }
        if (is_dirty && range_begin == dirty.size()) {
            range_begin = index;
            continue;
        }
        if (is_dirty) {
            continue;
        }

        const std::size_t range_count = index - range_begin;
        output.push_back(GpuBatchDirtyRange{
            static_cast<std::uint64_t>(range_begin) * instance_stride_bytes,
            static_cast<std::uint64_t>(range_count) * instance_stride_bytes,
        });
        range_begin = dirty.size();
    }
}

} // namespace

GpuBatchCache::GpuBatchCache(GpuBatchCacheConfig config) : config_(std::move(config)) {}

GpuBatchPlan GpuBatchCache::update(std::span<const GpuBatchDrawInput> inputs) {
    if (!valid_config(config_)) {
        return failure_plan(config_, inputs.size(), "invalid-config",
                            "max_slots, minimum_batch_size, and instance_stride_bytes must be non-zero; the slot range and upload size must fit in uint32/uint64");
    }

    using CurrentInputMap = std::unordered_map<std::string_view, std::size_t,
                                               StringViewHash, StringViewEqual>;
    CurrentInputMap current_inputs;
    current_inputs.reserve(inputs.size());
    std::size_t eligible_count = 0U;
    for (std::size_t index = 0; index < inputs.size(); ++index) {
        const GpuBatchDrawInput& input = inputs[index];
        if (input.draw_id.empty()) {
            return failure_plan(config_, inputs.size(), "invalid-input",
                                "draw at input index " + std::to_string(index) + " has an empty draw_id");
        }
        if (input.key.textures.size() > config_.max_textures_per_draw) {
            return failure_plan(config_, inputs.size(), "invalid-input",
                                "draw " + std::string(input.draw_id) + " exceeds max_textures_per_draw");
        }
        if (!current_inputs.emplace(input.draw_id, index).second) {
            return failure_plan(config_, inputs.size(), "invalid-input",
                                "draw_id is duplicated: " + std::string(input.draw_id));
        }
        if (input.visibility_eligible) {
            ++eligible_count;
        }
    }

    bool topology_reused = has_plan_ &&
                           current_inputs.size() == previous_input_signatures_.size();
    if (topology_reused) {
        for (const GpuBatchDrawInput& input : inputs) {
            const auto previous = previous_input_signatures_.find(input.draw_id);
            if (previous == previous_input_signatures_.end() ||
                previous->second.visibility_eligible != input.visibility_eligible ||
                previous->second.key != input.key) {
                topology_reused = false;
                break;
            }
        }
    }

    std::vector<std::size_t> ordered_indices;
    if (topology_reused) {
        // The previous linear IDs already encode canonical key order.  Only
        // map current string views back to input indices; no key or identity
        // is copied and no full sort is performed.
        ordered_indices.reserve(previous_linear_draw_ids_.size());
        for (const std::string* draw_id : previous_linear_draw_ids_) {
            if (draw_id == nullptr) {
                topology_reused = false;
                ordered_indices.clear();
                break;
            }
            const auto current = current_inputs.find(*draw_id);
            if (current == current_inputs.end()) {
                topology_reused = false;
                ordered_indices.clear();
                break;
            }
            ordered_indices.push_back(current->second);
        }
    }

    if (!topology_reused) {
        std::vector<std::size_t> eligible_indices;
        eligible_indices.reserve(eligible_count);
        for (std::size_t index = 0; index < inputs.size(); ++index) {
            if (inputs[index].visibility_eligible) {
                eligible_indices.push_back(index);
            }
        }
        std::sort(eligible_indices.begin(), eligible_indices.end(),
                  [&](const std::size_t lhs_index, const std::size_t rhs_index) {
                      const auto& lhs = inputs[lhs_index];
                      const auto& rhs = inputs[rhs_index];
                      if (lhs.key == rhs.key) {
                          return lhs.draw_id < rhs.draw_id;
                      }
                      return key_less(lhs.key, rhs.key);
                  });

        std::size_t remaining_slots = config_.max_slots;
        std::size_t selected_batches = 0U;
        std::size_t index = 0U;
        while (index < eligible_indices.size()) {
            std::size_t group_end = index + 1U;
            while (group_end < eligible_indices.size() &&
                   inputs[eligible_indices[group_end]].key ==
                       inputs[eligible_indices[index]].key) {
                ++group_end;
            }
            const std::size_t group_size = group_end - index;
            if (group_size < config_.minimum_batch_size ||
                selected_batches >= config_.max_batches ||
                remaining_slots < config_.minimum_batch_size) {
                index = group_end;
                continue;
            }
            const std::size_t selected_count = std::min(group_size, remaining_slots);
            if (selected_count < config_.minimum_batch_size) {
                index = group_end;
                continue;
            }
            ordered_indices.insert(ordered_indices.end(),
                                   eligible_indices.begin() + index,
                                   eligible_indices.begin() + index + selected_count);
            remaining_slots -= selected_count;
            ++selected_batches;
            index = group_end;
        }
    }

    if (ordered_indices.size() > config_.max_slots) {
        return failure_plan(config_, inputs.size(), "capacity-exceeded",
                            "selected GPU batch draws exceed max_slots");
    }

    // Owning signatures and slot state are staged only when topology changes.
    // A steady frame validates against the cached map and mutates only the
    // selected entry revisions/linear positions.
    using SignatureMap = decltype(previous_input_signatures_);
    SignatureMap next_signatures;
    if (!topology_reused) {
        for (const GpuBatchDrawInput& input : inputs) {
            next_signatures.emplace(std::string(input.draw_id),
                                    InputSignature{input.key, input.visibility_eligible});
        }
    }

    using EntryMap = decltype(entries_);
    EntryMap next_entries;
    std::set<std::uint32_t> next_free_slots;
    std::size_t next_slot = next_slot_;
    if (!topology_reused) {
        next_entries = entries_;
        next_free_slots = free_slots_;
    }
    EntryMap& entries_for_update = topology_reused ? entries_ : next_entries;
    std::set<std::uint32_t>& free_slots_for_update = topology_reused ? free_slots_ : next_free_slots;

    std::unordered_set<std::string_view, StringViewHash, StringViewEqual> active_ids;
    if (!topology_reused) {
        active_ids.reserve(ordered_indices.size());
        for (const std::size_t index : ordered_indices) {
            active_ids.insert(inputs[index].draw_id);
        }
    }

    GpuBatchPlan plan;
    plan.valid = true;
    plan.code = "ok";
    plan.instance_stride_bytes = config_.instance_stride_bytes;
    plan.full_rebuild = !has_plan_;
    plan.full_rebuild_reason = has_plan_ ? "none" : "initial";
    plan.statistics.input_draws = inputs.size();
    plan.statistics.eligible_draws = eligible_count;
    plan.statistics.fallback_draws = eligible_count - ordered_indices.size();
    plan.statistics.topology_reused = topology_reused;
    plan.statistics.topology_changed = has_plan_ && !topology_reused;

    if (!topology_reused) {
        for (const auto& [draw_id, entry] : entries_) {
            if (active_ids.find(std::string_view(draw_id)) != active_ids.end()) {
                continue;
            }
            next_entries.erase(draw_id);
            next_free_slots.insert(entry.stable_slot);
            plan.released_stable_slots.push_back(entry.stable_slot);
            ++plan.statistics.removed_draws;
        }
        std::sort(plan.released_stable_slots.begin(), plan.released_stable_slots.end());
    }

    for (std::size_t ordered_index = 0; ordered_index < ordered_indices.size(); ++ordered_index) {
        const std::size_t input_index = ordered_indices[ordered_index];
        const GpuBatchDrawInput& input = inputs[input_index];
        const auto old_entry = entries_.find(input.draw_id);
        std::uint32_t stable_slot = 0U;
        if (old_entry != entries_.end()) {
            stable_slot = old_entry->second.stable_slot;
            free_slots_for_update.erase(stable_slot);
            ++plan.statistics.reused_draws;
            bool key_changed = false;
            if (!topology_reused) {
                const auto previous = previous_input_signatures_.find(input.draw_id);
                key_changed = previous != previous_input_signatures_.end() &&
                              previous->second.key != input.key;
            }
            if (key_changed || old_entry->second.linear_index != ordered_index) {
                ++plan.statistics.moved_draws;
            }
        } else {
            if (!free_slots_for_update.empty()) {
                const auto free_slot = free_slots_for_update.begin();
                stable_slot = *free_slot;
                free_slots_for_update.erase(free_slot);
            } else {
                if (next_slot >= config_.max_slots) {
                    return failure_plan(config_, inputs.size(), "capacity-exceeded",
                                        "no stable slot is available for an eligible draw");
                }
                stable_slot = static_cast<std::uint32_t>(next_slot++);
            }
            ++plan.statistics.new_draws;
        }

        if (!topology_reused) {
            next_entries.insert_or_assign(std::string(input.draw_id),
                                          Entry{stable_slot, input.instance_payload_revision, 0U});
        }
    }

    const SignatureMap* signature_source =
        topology_reused ? &previous_input_signatures_ : &next_signatures;
    plan.linear_slots.reserve(ordered_indices.size());
    plan.batches.reserve(ordered_indices.size());
    std::vector<const GpuBatchKey*> new_keys;
    std::vector<std::uint64_t> new_revisions;
    std::vector<std::string_view> new_draw_ids;
    new_keys.reserve(ordered_indices.size());
    new_revisions.reserve(ordered_indices.size());
    new_draw_ids.reserve(ordered_indices.size());

    for (std::size_t index = 0; index < ordered_indices.size(); ++index) {
        const std::size_t input_index = ordered_indices[index];
        const GpuBatchDrawInput& input = inputs[input_index];
        const auto entry_found = entries_for_update.find(input.draw_id);
        if (entry_found == entries_for_update.end()) {
            return failure_plan(config_, inputs.size(), "invalid-input",
                                "selected draw identity was not allocated a stable slot");
        }
        Entry& entry = entry_found->second;
        entry.instance_payload_revision = input.instance_payload_revision;
        entry.linear_index = static_cast<std::uint32_t>(index);

        const auto cached_key = signature_source->find(input.draw_id);
        if (cached_key == signature_source->end()) {
            return failure_plan(config_, inputs.size(), "invalid-input",
                                "selected draw identity has no cached batch signature");
        }
        if (plan.batches.empty() || *plan.batches.back().key != cached_key->second.key) {
            plan.batches.push_back(GpuBatchGroup{
                &cached_key->second.key, static_cast<std::uint32_t>(index), 1U});
        } else {
            plan.batches.back().instance_count += 1U;
        }

        const auto cached_id = signature_source->find(input.draw_id);
        const std::string* cached_id_pointer =
            cached_id == signature_source->end() ? nullptr : &cached_id->first;
        plan.linear_slots.push_back(GpuBatchSlot{
            entry.stable_slot,
            static_cast<std::uint32_t>(index),
            static_cast<std::uint32_t>(plan.batches.size() - 1U),
            static_cast<std::uint32_t>(input_index),
            GpuBatchDrawIdView{cached_id_pointer},
            input.instance_payload_revision,
        });
        new_keys.push_back(&cached_key->second.key);
        new_revisions.push_back(input.instance_payload_revision);
        new_draw_ids.push_back(input.draw_id);
    }

    plan.statistics.batch_count = plan.batches.size();
    const std::size_t comparison_count = plan.linear_slots.size();
    std::vector<unsigned char> dirty(comparison_count, 0U);
    for (std::size_t index = 0; index < comparison_count; ++index) {
        const bool same = index < previous_linear_slots_.size() &&
            previous_linear_slots_[index] == plan.linear_slots[index].stable_slot &&
            same_linear_item(index, previous_linear_keys_, previous_linear_revisions_,
                             previous_linear_draw_ids_, new_keys, new_revisions,
                             new_draw_ids);
        if (!same) {
            dirty[index] = 1U;
            ++plan.statistics.dirty_draws;
        }
    }
    append_dirty_ranges(dirty, config_.instance_stride_bytes, plan.dirty_byte_ranges);

    if (!topology_reused) {
        previous_input_signatures_.swap(next_signatures);
        entries_ = std::move(next_entries);
        free_slots_ = std::move(next_free_slots);
    }
    next_slot_ = next_slot;

    previous_linear_slots_.clear();
    previous_linear_keys_.clear();
    previous_linear_revisions_.clear();
    previous_linear_draw_ids_.clear();
    previous_linear_slots_.reserve(plan.linear_slots.size());
    previous_linear_keys_.reserve(plan.linear_slots.size());
    previous_linear_revisions_.reserve(plan.linear_slots.size());
    previous_linear_draw_ids_.reserve(plan.linear_slots.size());
    for (std::size_t index = 0; index < plan.linear_slots.size(); ++index) {
        const auto cached = previous_input_signatures_.find(new_draw_ids[index]);
        // The topology map was validated/built above, so this is an internal
        // invariant rather than a caller-visible failure path.
        if (cached == previous_input_signatures_.end()) {
            continue;
        }
        previous_linear_slots_.push_back(plan.linear_slots[index].stable_slot);
        previous_linear_keys_.push_back(&cached->second.key);
        previous_linear_revisions_.push_back(new_revisions[index]);
        previous_linear_draw_ids_.push_back(&cached->first);
        plan.linear_slots[index].draw_id = GpuBatchDrawIdView{&cached->first};
    }
    has_plan_ = true;
    return plan;
}

void GpuBatchCache::clear() noexcept {
    entries_.clear();
    free_slots_.clear();
    next_slot_ = 0U;
    previous_linear_slots_.clear();
    previous_linear_keys_.clear();
    previous_linear_revisions_.clear();
    previous_linear_draw_ids_.clear();
    previous_input_signatures_.clear();
    has_plan_ = false;
}

} // namespace noemancer
