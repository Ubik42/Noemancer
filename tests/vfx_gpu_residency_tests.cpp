#include "engine/vfx_gpu_residency.hpp"
#include "engine/process_diagnostics.hpp"

#include <array>
#include <cassert>
#include <vector>

int main() {
    noemancer::configure_process_diagnostics("test.vfx-gpu-residency");
    noemancer::VfxGpuResidency residency(3);

    const std::array<std::uint64_t, 2> first_ids{11, 22};
    const auto first = residency.synchronize(first_ids);
    assert(first.uploads.size() == 2);
    assert(first.bindings.size() == 2 && first.bindings[0].newly_resident);
    assert((first.alive_slots == std::vector<std::uint32_t>{0, 1}));
    assert(first.reclaimed == 0 && first.dropped == 0 && first.resident == 2);

    const std::array<std::uint64_t, 2> stable_ids{22, 11};
    const auto stable = residency.synchronize(stable_ids);
    assert(stable.uploads.empty());
    assert(stable.bindings.size() == 2 && !stable.bindings[0].newly_resident);
    assert((stable.alive_slots == std::vector<std::uint32_t>{1, 0}));
    assert(stable.resident == 2);

    const std::array<std::uint64_t, 3> recycled_ids{22, 33, 44};
    const auto recycled = residency.synchronize(recycled_ids);
    assert(recycled.reclaimed == 1);
    assert(recycled.uploads.size() == 2);
    assert(recycled.uploads[0].particle_id == 33 && recycled.uploads[0].slot == 0);
    assert(recycled.uploads[1].particle_id == 44 && recycled.uploads[1].slot == 2);
    assert((recycled.alive_slots == std::vector<std::uint32_t>{1, 0, 2}));

    const std::array<std::uint64_t, 5> overflow_ids{22, 33, 44, 55, 55};
    const auto overflow = residency.synchronize(overflow_ids);
    assert(overflow.uploads.empty());
    assert(overflow.dropped == 1);
    assert(overflow.alive_slots.size() == 3);
    assert(residency.resident_count() == 3);

    residency.clear();
    assert(residency.resident_count() == 0);

    const auto small_sort=noemancer::plan_vfx_gpu_alpha_sort(48,8192);
    assert(small_sort.requested_count==48&&small_sort.bounded_count==48&&small_sort.span==64&&
           !small_sort.truncated&&small_sort.stages.size()==21&&small_sort.stages.back().compare_stride==1);
    const auto large_sort=noemancer::plan_vfx_gpu_alpha_sort(2049,8192);
    assert(large_sort.span==4096&&large_sort.stages.size()==78&&large_sort.stages.back().dispatch_groups==16);
    const auto overflow_sort=noemancer::plan_vfx_gpu_alpha_sort(9000,8192);
    assert(overflow_sort.bounded_count==8192&&overflow_sort.span==8192&&overflow_sort.truncated&&
           overflow_sort.stages.size()==91);

    struct SortKey { float distance; std::uint64_t stable_id; };
    const std::array<SortKey,5> keys{{{4.0F,9},{9.0F,8},{4.0F,3},{1.0F,4},{9.0F,2}}};
    const auto reference_plan=noemancer::plan_vfx_gpu_alpha_sort(keys.size(),8);
    std::vector<std::uint32_t> indices{0,1,2,3,4,8,8,8};
    const auto compare=[&](const std::uint32_t left,const std::uint32_t right) {
        const bool left_valid=left<keys.size(),right_valid=right<keys.size();
        if(left_valid!=right_valid) return left_valid?-1:1;
        if(!left_valid) return 0;
        if(keys[left].distance!=keys[right].distance) return keys[left].distance>keys[right].distance?-1:1;
        if(keys[left].stable_id!=keys[right].stable_id) return keys[left].stable_id<keys[right].stable_id?-1:1;
        return 0;
    };
    for(const auto& stage:reference_plan.stages) for(std::uint32_t lane=0;lane<reference_plan.span;++lane) {
        const auto partner=lane^stage.compare_stride;
        if(partner<=lane||partner>=reference_plan.span) continue;
        const auto order=compare(indices[lane],indices[partner]);
        const bool final_direction=(lane&stage.sequence_length)==0;
        if((final_direction&&order>0)||(!final_direction&&order<0)) std::swap(indices[lane],indices[partner]);
    }
    assert((std::vector<std::uint32_t>(indices.begin(),indices.begin()+5)==std::vector<std::uint32_t>{4,1,2,0,3}));
    return 0;
}
