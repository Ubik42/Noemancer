#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <unordered_map>
#include <vector>

namespace noemancer {

struct VfxGpuParticleUpload final {
    std::size_t source_index{};
    std::uint32_t slot{};
    std::uint64_t particle_id{};
};

struct VfxGpuParticleBinding final {
    std::size_t source_index{};
    std::uint32_t slot{};
    std::uint64_t particle_id{};
    bool newly_resident{};
};

struct VfxGpuResidencyDelta final {
    std::vector<VfxGpuParticleUpload> uploads;
    std::vector<VfxGpuParticleBinding> bindings;
    std::vector<std::uint32_t> alive_slots;
    std::size_t reclaimed{};
    std::size_t dropped{};
    std::size_t resident{};
    std::uint64_t revision{};
};

struct VfxGpuSortStage final {
    std::uint32_t sequence_length{};
    std::uint32_t compare_stride{};
    std::uint32_t dispatch_groups{};
};

struct VfxGpuSortPlan final {
    std::uint32_t requested_count{};
    std::uint32_t bounded_count{};
    std::uint32_t span{};
    bool truncated{};
    std::vector<VfxGpuSortStage> stages;
};

[[nodiscard]] VfxGpuSortPlan plan_vfx_gpu_alpha_sort(std::size_t count, std::uint32_t capacity,
                                                     std::uint32_t thread_group_size = 256);

// Maintains a bounded CPU mirror of engine particle IDs so the renderer can
// emit only new spawn payloads. The deterministic mirror slots are diagnostic;
// physical slots are allocated independently by the GPU dead-list kernel.
class VfxGpuResidency final {
public:
    explicit VfxGpuResidency(std::uint32_t capacity);

    [[nodiscard]] VfxGpuResidencyDelta synchronize(std::span<const std::uint64_t> particle_ids);
    void clear();

    [[nodiscard]] std::uint32_t capacity() const noexcept { return capacity_; }
    [[nodiscard]] std::size_t resident_count() const noexcept { return slots_by_id_.size(); }
    [[nodiscard]] std::uint64_t revision() const noexcept { return revision_; }

private:
    std::uint32_t capacity_{};
    std::vector<std::uint64_t> ids_by_slot_;
    std::unordered_map<std::uint64_t, std::uint32_t> slots_by_id_;
    std::uint64_t revision_{};
};

} // namespace noemancer
