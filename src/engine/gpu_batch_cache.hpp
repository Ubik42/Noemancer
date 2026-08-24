#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace noemancer {

// A texture binding is identified by authored/runtime identity and its
// committed resource generation.  It intentionally carries no SDL or GPU
// handle; physical resources belong to the Runtime adapter.
struct GpuBatchTextureGeneration final {
    std::string semantic;
    std::string stable_id;
    std::uint64_t resource_generation{};

    friend bool operator==(const GpuBatchTextureGeneration&, const GpuBatchTextureGeneration&) = default;
};

// The batch key contains only stable engine identities and generations.  It
// is safe to compare, sort and cache across frames and across GPU backends.
struct GpuBatchKey final {
    std::string geometry_id;
    std::uint64_t geometry_generation{};
    std::string material_id;
    std::uint64_t material_generation{};
    std::uint64_t raster_generation{};
    std::vector<GpuBatchTextureGeneration> textures;

    friend bool operator==(const GpuBatchKey&, const GpuBatchKey&) = default;
};

// A view of a cache-owned draw identity.  The conversion to
// const std::string& keeps existing runtime lookup code allocation-free while
// the cache avoids copying the identity into every output slot.
struct GpuBatchDrawIdView final {
    const std::string* value{};

    [[nodiscard]] std::string_view view() const noexcept {
        return value != nullptr ? std::string_view(*value) : std::string_view{};
    }
    operator const std::string&() const noexcept {
        static const std::string empty;
        return value != nullptr ? *value : empty;
    }
    operator std::string_view() const noexcept { return view(); }

    friend bool operator==(const GpuBatchDrawIdView lhs,
                           const std::string_view rhs) noexcept {
        return lhs.view() == rhs;
    }
    friend bool operator==(const std::string_view lhs,
                           const GpuBatchDrawIdView rhs) noexcept {
        return lhs == rhs.view();
    }
};

struct GpuBatchDrawInput final {
    std::string draw_id;
    GpuBatchKey key;
    bool visibility_eligible{};
    // The caller increments this whenever the bytes for this instance change.
    // The planner does not inspect or own the payload bytes.
    std::uint64_t instance_payload_revision{};

};

struct GpuBatchCacheConfig final {
    std::size_t max_slots{16384U};
    std::size_t minimum_batch_size{32U};
    std::size_t max_batches{1024U};
    std::size_t max_textures_per_draw{8U};
    std::uint32_t instance_stride_bytes{224U};
};

struct GpuBatchSlot final {
    // Stable across input reorder and while the same draw identity remains
    // eligible.  This is a logical slot, not a GPU pointer or byte address.
    std::uint32_t stable_slot{};
    // Position in the deterministic, contiguous linear upload order.
    std::uint32_t linear_index{};
    std::uint32_t batch_index{};
    // Input index is valid for the update that produced this plan.  draw_id
    // remains a cache-owned view for compatibility with existing runtime
    // lookup paths and does not allocate per slot.
    std::uint32_t input_index{};
    GpuBatchDrawIdView draw_id;
    std::uint64_t instance_payload_revision{};
};

struct GpuBatchGroup final {
    // The key points into the cache-owned topology signature and remains
    // valid until the next topology-changing update or clear().
    const GpuBatchKey* key{};
    std::uint32_t first_linear_index{};
    std::uint32_t instance_count{};
};

struct GpuBatchDirtyRange final {
    // Ranges refer only to the current linear instance upload buffer.  A
    // removed tail is handled by the caller's active instance count rather
    // than by an out-of-bounds stale-tail upload.
    std::uint64_t byte_offset{};
    std::uint64_t byte_count{};
};

struct GpuBatchStatistics final {
    std::size_t input_draws{};
    std::size_t eligible_draws{};
    std::size_t batch_count{};
    std::size_t reused_draws{};
    std::size_t new_draws{};
    std::size_t removed_draws{};
    std::size_t moved_draws{};
    std::size_t dirty_draws{};
    std::size_t fallback_draws{};
    bool topology_reused{};
    bool topology_changed{};
};

struct GpuBatchPlan final {
    bool valid{};
    std::string code;
    std::string detail;
    std::uint32_t instance_stride_bytes{};

    // Initial publication and rejected bounded updates explain why a caller
    // should treat the upload as a full rebuild.  "none" means an incremental
    // plan is valid.
    bool full_rebuild{};
    std::string full_rebuild_reason;

    // Sorted by batch key and then draw identity.  Each slot retains its
    // stable_slot while linear_index may move as batches are re-ordered.
    std::vector<GpuBatchSlot> linear_slots;
    std::vector<GpuBatchGroup> batches;
    std::vector<std::uint32_t> released_stable_slots;
    std::vector<GpuBatchDirtyRange> dirty_byte_ranges;
    GpuBatchStatistics statistics;
};

// Engine-owned incremental planner for static/opaque GPU instance batches.
// It canonicalizes input order, preserves logical slots by stable draw ID,
// and reports exactly which linear payload ranges changed.  It does not own
// GPU memory, transient handles, or render-world state.
class GpuBatchCache final {
public:
    explicit GpuBatchCache(GpuBatchCacheConfig config = {});

    [[nodiscard]] GpuBatchPlan update(std::span<const GpuBatchDrawInput> inputs);
    void clear() noexcept;

    [[nodiscard]] const GpuBatchCacheConfig& config() const noexcept { return config_; }
    [[nodiscard]] bool has_plan() const noexcept { return has_plan_; }

private:
    struct Entry final {
        std::uint32_t stable_slot{};
        std::uint64_t instance_payload_revision{};
        std::uint32_t linear_index{};
    };

    struct InputSignature final {
        GpuBatchKey key;
        bool visibility_eligible{};

        friend bool operator==(const InputSignature&, const InputSignature&) = default;
    };

    GpuBatchCacheConfig config_;
    std::map<std::string, Entry, std::less<>> entries_;
    std::set<std::uint32_t> free_slots_;
    std::size_t next_slot_{};
    std::vector<std::uint32_t> previous_linear_slots_;
    std::vector<const GpuBatchKey*> previous_linear_keys_;
    std::vector<std::uint64_t> previous_linear_revisions_;
    std::vector<const std::string*> previous_linear_draw_ids_;
    std::map<std::string, InputSignature, std::less<>> previous_input_signatures_;
    bool has_plan_{};
};

} // namespace noemancer
