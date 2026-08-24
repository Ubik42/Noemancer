#include "engine/vfx_gpu_residency.hpp"

#include <algorithm>
#include <bit>
#include <limits>
#include <unordered_set>

namespace noemancer {

VfxGpuSortPlan plan_vfx_gpu_alpha_sort(const std::size_t count, const std::uint32_t capacity,
                                       const std::uint32_t thread_group_size) {
    VfxGpuSortPlan plan;
    plan.requested_count = static_cast<std::uint32_t>(std::min<std::size_t>(count, std::numeric_limits<std::uint32_t>::max()));
    if (capacity == 0 || thread_group_size == 0) return plan;
    plan.bounded_count = std::min(plan.requested_count, capacity);
    plan.truncated = count > capacity;
    const auto maximum_span = std::bit_floor(capacity);
    plan.span = std::min(maximum_span, std::bit_ceil(std::max(1U, plan.bounded_count)));
    for (std::uint32_t sequence = 2U; sequence <= plan.span; sequence <<= 1U) {
        for (std::uint32_t stride = sequence >> 1U; stride > 0U; stride >>= 1U)
            plan.stages.push_back({sequence, stride, (plan.span + thread_group_size - 1U) / thread_group_size});
    }
    return plan;
}

VfxGpuResidency::VfxGpuResidency(const std::uint32_t capacity)
    : capacity_(capacity), ids_by_slot_(capacity) {}

VfxGpuResidencyDelta VfxGpuResidency::synchronize(const std::span<const std::uint64_t> particle_ids) {
    struct DesiredParticle final {
        std::size_t source_index{};
        std::uint64_t id{};
        std::uint32_t slot{};
        bool resident{};
        bool newly_resident{};
    };

    std::vector<DesiredParticle> desired;
    desired.reserve(std::min<std::size_t>(particle_ids.size(), capacity_));
    std::unordered_set<std::uint64_t> desired_ids;
    desired_ids.reserve(particle_ids.size());
    for (std::size_t source_index = 0; source_index < particle_ids.size(); ++source_index) {
        const auto id = particle_ids[source_index];
        if (id == 0 || !desired_ids.insert(id).second) continue;
        const auto found = slots_by_id_.find(id);
        desired.push_back({source_index, id, found == slots_by_id_.end() ? 0U : found->second,
                           found != slots_by_id_.end(), false});
    }

    VfxGpuResidencyDelta delta;
    for (std::uint32_t slot = 0; slot < capacity_; ++slot) {
        const auto id = ids_by_slot_[slot];
        if (id != 0 && !desired_ids.contains(id)) {
            slots_by_id_.erase(id);
            ids_by_slot_[slot] = 0;
            ++delta.reclaimed;
        }
    }

    std::uint32_t next_free{};
    for (auto& particle : desired) {
        if (particle.resident) continue;
        while (next_free < capacity_ && ids_by_slot_[next_free] != 0) ++next_free;
        if (next_free >= capacity_) {
            ++delta.dropped;
            continue;
        }
        particle.slot = next_free;
        particle.resident = true;
        ids_by_slot_[next_free] = particle.id;
        slots_by_id_.emplace(particle.id, next_free);
        particle.newly_resident = true;
        delta.uploads.push_back({particle.source_index, next_free, particle.id});
        ++next_free;
    }

    delta.alive_slots.reserve(std::min<std::size_t>(desired.size(), capacity_));
    delta.bindings.reserve(std::min<std::size_t>(desired.size(), capacity_));
    for (const auto& particle : desired) {
        if (particle.resident) {
            delta.alive_slots.push_back(particle.slot);
            delta.bindings.push_back({particle.source_index,particle.slot,particle.id,particle.newly_resident});
        }
    }
    delta.resident = slots_by_id_.size();
    delta.revision = ++revision_;
    return delta;
}

void VfxGpuResidency::clear() {
    std::fill(ids_by_slot_.begin(), ids_by_slot_.end(), 0);
    slots_by_id_.clear();
    ++revision_;
}

} // namespace noemancer
