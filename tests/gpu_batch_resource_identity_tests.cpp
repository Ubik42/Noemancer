#include "runtime/gpu_batch_resource_identity.hpp"

#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

using noemancer::GpuBatchResourceIdentityDescriptor;
using noemancer::GpuBatchResourceIdentityResult;
using noemancer::GpuBatchTextureBindingDescriptor;
using noemancer::GpuBatchTextureFallbackIdentity;
using noemancer::TextureResourceDescriptor;
using noemancer::TextureResourceHandle;
using noemancer::TextureResourceMetadata;
using noemancer::TextureResourceTable;

void require(const bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

SDL_GPUTexture* fake_texture(const std::uintptr_t address) {
    return reinterpret_cast<SDL_GPUTexture*>(address);
}

TextureResourceDescriptor resource_descriptor(std::string stable_id,
                                              std::string semantic,
                                              std::string owner = "batch.identity.tests") {
    TextureResourceDescriptor descriptor;
    descriptor.stable_id = std::move(stable_id);
    descriptor.semantic = std::move(semantic);
    descriptor.owner = std::move(owner);
    descriptor.source = "test-fixture";
    descriptor.residency = "resident";
    descriptor.metadata = TextureResourceMetadata{64U, 64U, 1U, 0U, 4096U};
    return descriptor;
}

GpuBatchTextureBindingDescriptor binding(const TextureResourceHandle handle,
                                         std::string binding_semantic,
                                         std::string fallback_stable_id = {}) {
    GpuBatchTextureBindingDescriptor result;
    result.handle = handle;
    result.semantic = std::move(binding_semantic);
    result.fallback = GpuBatchTextureFallbackIdentity{std::move(fallback_stable_id)};
    return result;
}

GpuBatchResourceIdentityDescriptor descriptor(
    std::vector<GpuBatchTextureBindingDescriptor> textures = {}) {
    GpuBatchResourceIdentityDescriptor result;
    result.geometry_id = "mesh.character";
    result.geometry_generation = 7U;
    result.material_id = "material.character";
    result.material_generation = 11U;
    result.raster_generation = 13U;
    result.textures = std::move(textures);
    return result;
}

void require_valid_key(const GpuBatchResourceIdentityResult& result) {
    require(result.valid && result.code == "ok", "expected a valid GPU batch resource identity");
}

void test_committed_and_pending_generations() {
    TextureResourceTable table;
    const auto first = fake_texture(0x1000U);
    const auto second = fake_texture(0x2000U);
    const auto handle = table.acquire(
        resource_descriptor("texture.character", "base-color"), first);
    require(handle.valid(), "fixture texture handle must be valid");

    const auto committed = build_gpu_batch_resource_identity(
        table, descriptor({binding(handle, "base-color")}));
    require_valid_key(committed);
    require(committed.key.geometry_id == "mesh.character" &&
                committed.key.geometry_generation == 7U &&
                committed.key.material_id == "material.character" &&
                committed.key.material_generation == 11U &&
                committed.key.raster_generation == 13U,
            "geometry/material/raster generations must project into the key");
    require(committed.key.textures.size() == 1U &&
                committed.key.textures[0].stable_id == "texture.character" &&
                committed.key.textures[0].semantic == "base-color" &&
                committed.key.textures[0].resource_generation == 1U,
            "committed table state must use descriptor identity and generation");
    require(committed.binding_snapshot.valid && committed.binding_snapshot.returned_count == 1U &&
                committed.binding_snapshot.bindings[0].state == "committed" &&
                committed.binding_snapshot.bindings[0].committed_generation == 1U &&
                committed.binding_snapshot.bindings[0].effective_generation == 1U &&
                committed.binding_snapshot.canonical_json().find("texturePointer") == std::string::npos,
            "valid batch identities must carry pointer-free committed binding evidence");

    require(table.stage_replacement(handle, second,
                                    TextureResourceMetadata{64U, 64U, 1U, 0U, 8192U}),
            "fixture replacement must enter the pending state");
    const auto pending = build_gpu_batch_resource_identity(
        table, descriptor({binding(handle, "base-color")}));
    require_valid_key(pending);
    require(pending.key.textures[0].resource_generation == 2U,
            "pending replacement must use current generation plus one");
    require(pending.binding_snapshot.valid && pending.binding_snapshot.bindings[0].state == "pending" &&
                pending.binding_snapshot.bindings[0].committed_generation == 1U &&
                pending.binding_snapshot.bindings[0].effective_generation == 2U,
            "pending batch identities must distinguish committed and effective generations");
    require(table.view(handle)->transition_pending,
            "fixture must remain pending while the key is projected");

    require(table.commit_replacement(handle) == first,
            "fixture replacement must commit the previous texture");
    const auto committed_replacement = build_gpu_batch_resource_identity(
        table, descriptor({binding(handle, "base-color")}));
    require_valid_key(committed_replacement);
    require(committed_replacement.key.textures[0].resource_generation == 2U,
            "committed replacement must retain the effective pending generation");
    require(committed_replacement.binding_snapshot.valid &&
                committed_replacement.binding_snapshot.bindings[0].state == "committed" &&
                committed_replacement.binding_snapshot.bindings[0].committed_generation == 2U &&
                committed_replacement.binding_snapshot.bindings[0].effective_generation == 2U &&
                committed_replacement.binding_snapshot.fingerprint() != committed.binding_snapshot.fingerprint(),
            "committed replacement must publish a new binding fingerprint");
}

void test_binding_semantics_are_independent_of_storage_semantics() {
    TextureResourceTable table;
    const auto normal = table.acquire(
        resource_descriptor("texture.material.normal", "pbr-storage"), fake_texture(0x2500U));
    const auto metallic_roughness = table.acquire(
        resource_descriptor("texture.material.metallic-roughness", "pbr-storage"), fake_texture(0x2600U));
    const auto occlusion = table.acquire(
        resource_descriptor("texture.material.occlusion", "pbr-storage"), fake_texture(0x2700U));
    require(normal.valid() && metallic_roughness.valid() && occlusion.valid(),
            "PBR storage-semantic fixtures must be valid resources");
    require(table.view(normal)->descriptor.semantic == "pbr-storage" &&
                table.view(metallic_roughness)->descriptor.semantic == "pbr-storage" &&
                table.view(occlusion)->descriptor.semantic == "pbr-storage",
            "the fixture views must share one generic storage semantic");

    const auto result = build_gpu_batch_resource_identity(
        table, descriptor({
            binding(normal, "normal"),
            binding(metallic_roughness, "metallic-roughness"),
            binding(occlusion, "occlusion"),
        }));
    require_valid_key(result);
    require(result.key.textures.size() == 3U &&
                result.key.textures[0].stable_id == "texture.material.normal" &&
                result.key.textures[0].semantic == "normal" &&
                result.key.textures[1].stable_id == "texture.material.metallic-roughness" &&
                result.key.textures[1].semantic == "metallic-roughness" &&
                result.key.textures[2].stable_id == "texture.material.occlusion" &&
                result.key.textures[2].semantic == "occlusion",
            "binding semantics must identify PBR slots independently of storage semantics");
}

void test_fallback_and_stale_handle() {
    TextureResourceTable table;
    const auto handle = table.acquire(
        resource_descriptor("texture.stale", "pbr-storage"), fake_texture(0x3000U));
    require(handle.valid(), "stale-handle fixture must be valid");
    const auto stale = handle;
    require(table.remove(handle) == fake_texture(0x3000U),
            "stale-handle fixture must be removed");
    const auto replacement_identity = table.acquire(
        resource_descriptor("texture.reused-slot", "pbr-storage"), fake_texture(0x3100U));
    require(replacement_identity.valid() && replacement_identity.slot == stale.slot &&
                replacement_identity.identity_generation != stale.identity_generation,
            "stale-handle fixture must reuse the slot with a new identity generation");

    const auto fallback = build_gpu_batch_resource_identity(
        table, descriptor({binding(stale, "normal", "fallback.flat-normal")}));
    require_valid_key(fallback);
    require(fallback.key.textures.size() == 1U &&
                fallback.key.textures[0].stable_id == "fallback.flat-normal" &&
                fallback.key.textures[0].semantic == "normal" &&
                fallback.key.textures[0].resource_generation == 1U,
            "stale handles must use only the caller-provided stable fallback");
    require(fallback.binding_snapshot.valid && fallback.binding_snapshot.bindings.size() == 1U &&
                fallback.binding_snapshot.bindings[0].state == "stale" &&
                fallback.binding_snapshot.bindings[0].fallback &&
                fallback.binding_snapshot.bindings[0].stale_handle &&
                fallback.binding_snapshot.bindings[0].effective_generation == 1U,
            "stale handles must be explicit fallback entries in binding evidence");

    const auto missing_fallback = build_gpu_batch_resource_identity(
        table, descriptor({binding(stale, "normal")}));
    require(!missing_fallback.valid && missing_fallback.code == "missing-texture-fallback",
            "stale handles without a fallback must fail structurally");

    const auto invalid_handle = build_gpu_batch_resource_identity(
        table, descriptor({binding(TextureResourceHandle{}, "occlusion", "fallback.white")}));
    require_valid_key(invalid_handle);
    require(invalid_handle.key.textures.size() == 1U &&
                invalid_handle.key.textures[0].stable_id == "fallback.white" &&
                invalid_handle.key.textures[0].semantic == "occlusion",
            "an invalid handle must use the explicit fallback while retaining its binding semantic");
}

void test_order_is_explicit_and_deterministic() {
    TextureResourceTable table;
    const auto normal = table.acquire(
        resource_descriptor("texture.normal", "normal"), fake_texture(0x4000U));
    const auto base_color = table.acquire(
        resource_descriptor("texture.base", "base-color"), fake_texture(0x5000U));
    require(normal.valid() && base_color.valid(), "ordered texture fixtures must be valid");

    const auto ordered_descriptor = descriptor({
        binding(normal, "normal"), binding(base_color, "base-color"),
    });
    const auto first = build_gpu_batch_resource_identity(table, ordered_descriptor);
    const auto second = build_gpu_batch_resource_identity(table, ordered_descriptor);
    require_valid_key(first);
    require_valid_key(second);
    require(first.key == second.key, "the same ordered descriptor must project deterministically");
    require(first.key.textures.size() == 2U &&
                first.key.textures[0].semantic == "normal" &&
                first.key.textures[1].semantic == "base-color",
            "texture order must remain the caller's explicit shader binding order");

    const auto reversed_descriptor = descriptor({
        binding(base_color, "base-color"), binding(normal, "normal"),
    });
    const auto reversed = build_gpu_batch_resource_identity(table, reversed_descriptor);
    require_valid_key(reversed);
    require(reversed.key.textures[0].semantic == "base-color" &&
                reversed.key.textures[1].semantic == "normal",
            "repeated projection must not depend on unordered table traversal");
}

void test_duplicate_binding_semantics_rejected() {
    TextureResourceTable table;
    const auto first = table.acquire(
        resource_descriptor("texture.first", "pbr-storage"), fake_texture(0x6000U));
    const auto second = table.acquire(
        resource_descriptor("texture.second", "pbr-storage"), fake_texture(0x7000U));
    require(first.valid() && second.valid(), "duplicate binding fixtures must be valid resources");

    const auto duplicate = build_gpu_batch_resource_identity(
        table, descriptor({binding(first, "normal"), binding(second, "normal")}));
    require(!duplicate.valid && duplicate.code == "duplicate-texture-semantic",
            "duplicate binding semantics must be rejected even when storage semantics match");
}

void test_rejection_paths() {
    TextureResourceTable table;

    auto empty_geometry = descriptor();
    empty_geometry.geometry_id.clear();
    const auto empty_geometry_result = build_gpu_batch_resource_identity(table, empty_geometry);
    require(!empty_geometry_result.valid && empty_geometry_result.code == "invalid-geometry-identity",
            "empty geometry identity must be rejected");

    auto empty_material = descriptor();
    empty_material.material_id.clear();
    const auto empty_material_result = build_gpu_batch_resource_identity(table, empty_material);
    require(!empty_material_result.valid && empty_material_result.code == "invalid-material-identity",
            "empty material identity must be rejected");

    const auto missing_binding_semantic = build_gpu_batch_resource_identity(
        table, descriptor({binding(TextureResourceHandle{}, "")}));
    require(!missing_binding_semantic.valid && missing_binding_semantic.code == "invalid-texture-semantic",
            "an empty binding semantic must fail closed before fallback resolution");
}

} // namespace

int main() {
    try {
        test_committed_and_pending_generations();
        test_binding_semantics_are_independent_of_storage_semantics();
        test_fallback_and_stale_handle();
        test_order_is_explicit_and_deterministic();
        test_duplicate_binding_semantics_rejected();
        test_rejection_paths();
    } catch (const std::exception& error) {
        std::cerr << "gpu_batch_resource_identity_tests: " << error.what() << '\n';
        return 1;
    }
    std::cout << "gpu_batch_resource_identity_tests: ok\n";
    return 0;
}
