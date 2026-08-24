#include "runtime/texture_resource_table.hpp"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <iostream>

int main() {
    using namespace noemancer;
    auto* first = reinterpret_cast<SDL_GPUTexture*>(static_cast<std::uintptr_t>(0x1000U));
    auto* second = reinterpret_cast<SDL_GPUTexture*>(static_cast<std::uintptr_t>(0x2000U));
    TextureResourceTable table;
    const TextureResourceDescriptor descriptor{
        .stable_id="asset.texture.courier", .semantic="base-color-srgb", .owner="scene.sprite",
        .source="asset-registry", .residency="stream", .metadata={1024U,512U,11U,7U,128U}};
    const auto handle = table.acquire(descriptor, first);
    if (!handle.valid() || table.resolve(handle) != first || table.size() != 1U ||
        table.find(descriptor.stable_id, descriptor.semantic) != handle) return 1;
    const auto initial = table.view(handle);
    if (!initial || initial->resource_generation != 1U || initial->transition_pending) return 2;

    if (!table.stage_replacement(handle, second, {1024U,512U,11U,3U,8192U}) ||
        table.resolve(handle) != second) return 3;
    const auto pending = table.view(handle);
    if (!pending || !pending->transition_pending || pending->resource_generation != 1U ||
        pending->descriptor.metadata.resident_mip_start != 3U) return 4;
    if (table.rollback_replacement(handle) != second || table.resolve(handle) != first ||
        table.view(handle)->resource_generation != 1U) return 5;
    if (table.stage_replacement(handle, first, {1024U,512U,11U,7U,128U})) return 11;

    if (!table.stage_replacement(handle, second, {1024U,512U,11U,0U,2097152U}) ||
        table.commit_replacement(handle) != first || table.resolve(handle) != second ||
        table.view(handle)->resource_generation != 2U) return 6;
    const auto observation = nlohmann::json::parse(table.observe_json("scene.sprite", 8U));
    if (observation.at("schemaVersion") != "noemancer.texture-resource-table/0.1" ||
        observation.at("resourceCount") != 1U || observation.at("pendingTransitions") != 0U ||
        observation.at("resources").at(0).at("resourceGeneration") != 2U ||
        observation.at("resources").at(0).at("metadata").at("residentMipStart") != 0U) return 7;

    if (table.remove(handle) != second || table.resolve(handle) != nullptr || table.size() != 0U) return 8;
    const auto reused = table.acquire(TextureResourceDescriptor{
        .stable_id="ui.inspector/atlas/1", .semantic="ui-rgba", .owner="ui.inspector",
        .source="retained-ui", .metadata={16U,16U,1U,0U,1024U}}, first);
    if (!reused.valid() || reused.slot != handle.slot || reused.identity_generation == handle.identity_generation ||
        table.resolve(handle) != nullptr || table.resolve(reused) != first) return 9;

    const auto collision = table.acquire(TextureResourceDescriptor{
        .stable_id="ui.inspector/atlas/1", .semantic="ui-rgba", .owner="wrong-owner",
        .source="fixture", .metadata={16U,16U,1U,0U,1024U}}, second);
    if (collision.valid() || table.resolve(reused) != first) {
        std::cerr << "Resource identity collision changed another owner's texture\n";
        return 10;
    }
    return 0;
}
