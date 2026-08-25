#include "runtime/gpu_batch_resource_identity.hpp"

#include <limits>
#include <set>
#include <string>
#include <utility>

namespace noemancer {
namespace {

[[nodiscard]] GpuBatchResourceIdentityResult failure(std::string code,
                                                     std::string detail) {
    GpuBatchResourceIdentityResult result;
    result.valid = false;
    result.code = std::move(code);
    result.detail = std::move(detail);
    return result;
}

[[nodiscard]] std::string texture_context(const std::size_t index) {
    return "texture binding at index " + std::to_string(index);
}

} // namespace

GpuBatchResourceIdentityResult build_gpu_batch_resource_identity(
    const TextureResourceTable& texture_resources,
    const GpuBatchResourceIdentityDescriptor& descriptor) {
    if (descriptor.geometry_id.empty()) {
        return failure("invalid-geometry-identity", "geometry_id must not be empty");
    }
    if (descriptor.material_id.empty()) {
        return failure("invalid-material-identity", "material_id must not be empty");
    }

    GpuBatchKey key;
    key.geometry_id = descriptor.geometry_id;
    key.geometry_generation = descriptor.geometry_generation;
    key.material_id = descriptor.material_id;
    key.material_generation = descriptor.material_generation;
    key.raster_generation = descriptor.raster_generation;
    key.textures.reserve(descriptor.textures.size());
    std::vector<TextureResourceBindingRequest> binding_requests;
    binding_requests.reserve(descriptor.textures.size());

    std::set<std::string> semantics;
    for (std::size_t index = 0; index < descriptor.textures.size(); ++index) {
        const auto& binding = descriptor.textures[index];
        if (binding.semantic.empty()) {
            return failure("invalid-texture-semantic",
                           texture_context(index) + " has an empty shader binding semantic");
        }
        GpuBatchTextureGeneration texture_identity;
        texture_identity.semantic = binding.semantic;
        const auto view = texture_resources.view(binding.handle);
        if (!view) {
            if (binding.fallback.stable_id.empty()) {
                return failure("missing-texture-fallback",
                               texture_context(index) +
                                   " has an invalid or stale handle and no fallback stable identity");
            }
            texture_identity.stable_id = binding.fallback.stable_id;
            // Fallback identities are semantic placeholders.  A fixed initial
            // generation matches the runtime's built-in fallback resources;
            // callers change the stable ID when the fallback identity changes.
            texture_identity.resource_generation = 1U;
        } else {
            if (view->descriptor.stable_id.empty()) {
                return failure("invalid-texture-identity",
                               texture_context(index) +
                                   " resolved to a resource with an empty stable_id");
            }
            texture_identity.stable_id = view->descriptor.stable_id;
            texture_identity.resource_generation = view->resource_generation;
            if (view->transition_pending) {
                if (view->resource_generation == std::numeric_limits<std::uint64_t>::max()) {
                    return failure("texture-generation-overflow",
                                   texture_context(index) +
                                       " has a pending replacement but its generation cannot be incremented");
                }
                ++texture_identity.resource_generation;
            }
        }

        if (!semantics.insert(binding.semantic).second) {
            return failure("duplicate-texture-semantic",
                           "shader binding semantic is repeated: " + binding.semantic);
        }
        binding_requests.push_back({binding.handle, binding.semantic, binding.fallback.stable_id});
        key.textures.push_back(std::move(texture_identity));
    }

    auto binding_snapshot = texture_resources.snapshot_bindings(binding_requests);
    if (!binding_snapshot.valid) {
        return failure(binding_snapshot.code, binding_snapshot.detail);
    }

    GpuBatchResourceIdentityResult result;
    result.valid = true;
    result.code = "ok";
    result.key = std::move(key);
    result.binding_snapshot = std::move(binding_snapshot);
    return result;
}

} // namespace noemancer
