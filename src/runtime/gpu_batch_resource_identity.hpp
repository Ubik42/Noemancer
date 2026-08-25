#pragma once

#include "engine/gpu_batch_cache.hpp"
#include "runtime/texture_resource_table.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace noemancer {

// A fallback is an authored/runtime resource identity, not a physical
// resource. It is required when the supplied table handle is invalid or
// stale; the adapter never derives one from an SDL pointer. The shader-slot
// semantic belongs to the binding descriptor below and is therefore not
// duplicated here.
struct GpuBatchTextureFallbackIdentity final {
    std::string stable_id;

    friend bool operator==(const GpuBatchTextureFallbackIdentity&,
                           const GpuBatchTextureFallbackIdentity&) = default;
};

// Texture slots are intentionally an ordered vector: its order is the
// material/shader binding order and is preserved in the resulting key. The
// semantic names that shader slot (base-color, normal, occlusion, ...); it is
// deliberately independent from a resource table's broader storage semantic
// such as `pbr-linear-data`.
struct GpuBatchTextureBindingDescriptor final {
    TextureResourceHandle handle{};
    std::string semantic;
    GpuBatchTextureFallbackIdentity fallback;
};

// Runtime-private input for projecting resource-table state into the
// engine-owned, GPU-handle-free batch key.
struct GpuBatchResourceIdentityDescriptor final {
    std::string geometry_id;
    std::uint64_t geometry_generation{};
    std::string material_id;
    std::uint64_t material_generation{};
    std::uint64_t raster_generation{};
    std::vector<GpuBatchTextureBindingDescriptor> textures;
};

struct GpuBatchResourceIdentityResult final {
    bool valid{};
    std::string code;
    std::string detail;
    GpuBatchKey key;
    // A sorted, identity-only view of the same bindings for future
    // descriptor-array/bindless consumers.  The batch key itself preserves
    // shader binding order; this snapshot is the canonical evidence view.
    TextureResourceBindingSnapshot binding_snapshot;
};

// Builds a stable engine key from runtime texture indirection.  A pending
// replacement is already the texture returned by TextureResourceTable::resolve
// and therefore receives current resource_generation + 1 as its effective
// generation.  No SDL resource pointer is copied into the result.
[[nodiscard]] GpuBatchResourceIdentityResult build_gpu_batch_resource_identity(
    const TextureResourceTable& texture_resources,
    const GpuBatchResourceIdentityDescriptor& descriptor);

} // namespace noemancer
