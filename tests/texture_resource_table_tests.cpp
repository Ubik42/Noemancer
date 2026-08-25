#include "runtime/texture_resource_table.hpp"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

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

    TextureResourceTable snapshot_table;
    const auto alpha = snapshot_table.acquire(TextureResourceDescriptor{
        .stable_id="asset.texture.alpha", .semantic="pbr-storage", .owner="snapshot.tests",
        .source="fixture", .metadata={32U,32U,1U,0U,4096U}}, first);
    const auto zeta = snapshot_table.acquire(TextureResourceDescriptor{
        .stable_id="asset.texture.zeta", .semantic="pbr-storage", .owner="snapshot.tests",
        .source="fixture", .metadata={64U,64U,2U,0U,20480U}}, second);
    const std::vector<TextureResourceBindingRequest> zeta_request{{zeta, "base-color", {}}};
    const auto zeta_before = snapshot_table.snapshot_bindings(zeta_request);
    if (!alpha.valid() || !zeta.valid() || !zeta_before.valid ||
        zeta_before.bindings[0].state != "committed" || zeta_before.bindings[0].committed_generation != 1U ||
        !snapshot_table.stage_replacement(zeta, first, {64U,64U,2U,1U,8192U})) return 12;
    const auto stale = alpha;
    if (snapshot_table.remove(alpha) != first) return 13;
    const std::vector<TextureResourceBindingRequest> requests{
        {zeta, "base-color", {}},
        {stale, "normal", "zz.fallback.flat-normal"},
        {{}, "occlusion", "zz.fallback.white"},
    };
    const auto snapshot = snapshot_table.snapshot_bindings(requests);
    if (!snapshot.valid || snapshot.code != "ok" || snapshot.returned_count != requests.size() ||
        snapshot.truncated || snapshot.bindings.size() != requests.size()) return 14;
    if (snapshot.bindings[0].stable_id != "asset.texture.zeta" ||
        snapshot.bindings[0].state != "pending" || snapshot.bindings[0].committed_generation != 1U ||
        snapshot.bindings[0].effective_generation != 2U || !snapshot.bindings[0].transition_pending ||
        snapshot.bindings[1].stable_id != "zz.fallback.flat-normal" ||
        snapshot.bindings[1].state != "stale" || !snapshot.bindings[1].fallback ||
        !snapshot.bindings[1].stale_handle || snapshot.bindings[1].effective_generation != 1U ||
        snapshot.bindings[2].stable_id != "zz.fallback.white" ||
        snapshot.bindings[2].state != "fallback" || !snapshot.bindings[2].fallback ||
        snapshot.bindings[2].stale_handle) return 15;
    const auto snapshot_json = nlohmann::json::parse(snapshot.canonical_json());
    if (snapshot_json.at("schemaVersion") != "noemancer.texture-binding-snapshot/0.1" ||
        snapshot_json.at("fingerprint") != snapshot.fingerprint() ||
        snapshot_json.at("bindings").at(0).at("state") != "pending" ||
        snapshot_json.dump().find("texturePointer") != std::string::npos ||
        snapshot_json.dump().find("0x") != std::string::npos) return 16;

    const std::vector<TextureResourceBindingRequest> reordered{
        requests[2], requests[0], requests[1]};
    const auto reordered_snapshot = snapshot_table.snapshot_bindings(reordered);
    if (!reordered_snapshot.valid || reordered_snapshot.fingerprint() != snapshot.fingerprint() ||
        reordered_snapshot.canonical_json() != snapshot.canonical_json()) return 17;

    if (snapshot_table.rollback_replacement(zeta) != first) return 18;
    const auto rolled_back = snapshot_table.snapshot_bindings(requests);
    if (!rolled_back.valid || rolled_back.bindings[0].state != "committed" ||
        rolled_back.bindings[0].committed_generation != 1U ||
        rolled_back.bindings[0].effective_generation != 1U) return 19;
    const auto zeta_after_rollback = snapshot_table.snapshot_bindings(zeta_request);
    if (!zeta_after_rollback.valid || zeta_after_rollback.fingerprint() != zeta_before.fingerprint()) return 24;
    if (!snapshot_table.stage_replacement(zeta, first, {64U,64U,2U,1U,8192U}) ||
        snapshot_table.commit_replacement(zeta) != second) return 20;
    const auto committed_snapshot = snapshot_table.snapshot_bindings(requests);
    if (!committed_snapshot.valid || committed_snapshot.bindings[0].state != "committed" ||
        committed_snapshot.bindings[0].committed_generation != 2U ||
        committed_snapshot.bindings[0].effective_generation != 2U ||
        committed_snapshot.fingerprint() == rolled_back.fingerprint()) return 21;

    const auto truncated = snapshot_table.snapshot_bindings(requests, 2U);
    if (!truncated.valid || !truncated.truncated || truncated.returned_count != 2U ||
        truncated.bindings.size() != 2U || truncated.bindings[0].stable_id != "asset.texture.zeta") return 22;
    const std::vector<TextureResourceBindingRequest> missing_request{{{}, "base-color", {}}};
    const auto missing_fallback = snapshot_table.snapshot_bindings(missing_request);
    if (missing_fallback.valid || missing_fallback.code != "missing-texture-fallback") return 23;
    return 0;
}
