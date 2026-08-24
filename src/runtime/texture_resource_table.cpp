#include "runtime/texture_resource_table.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <limits>
#include <utility>

namespace noemancer {
namespace {

using Json = nlohmann::json;

bool valid_descriptor(const TextureResourceDescriptor& descriptor) {
    return !descriptor.stable_id.empty() && !descriptor.semantic.empty() &&
        !descriptor.owner.empty() && descriptor.metadata.width > 0U &&
        descriptor.metadata.height > 0U && descriptor.metadata.mip_count > 0U &&
        descriptor.metadata.resident_mip_start < descriptor.metadata.mip_count &&
        (descriptor.residency == "resident" || descriptor.residency == "stream");
}

Json metadata_json(const TextureResourceMetadata& metadata) {
    return {{"width", metadata.width}, {"height", metadata.height},
        {"mipCount", metadata.mip_count}, {"residentMipStart", metadata.resident_mip_start},
        {"allocationBytesEstimate", metadata.allocation_bytes_estimate}};
}

} // namespace

std::string TextureResourceTable::key(const std::string_view stable_id, const std::string_view semantic) {
    std::string result;
    result.reserve(stable_id.size() + semantic.size() + 1U);
    result.append(stable_id); result.push_back('\x1f'); result.append(semantic);
    return result;
}

TextureResourceTable::Entry* TextureResourceTable::entry(const TextureResourceHandle handle) noexcept {
    if (!handle.valid() || handle.slot >= entries_.size()) return nullptr;
    auto& candidate = entries_[handle.slot];
    return candidate.live && candidate.identity_generation == handle.identity_generation ? &candidate : nullptr;
}

const TextureResourceTable::Entry* TextureResourceTable::entry(const TextureResourceHandle handle) const noexcept {
    if (!handle.valid() || handle.slot >= entries_.size()) return nullptr;
    const auto& candidate = entries_[handle.slot];
    return candidate.live && candidate.identity_generation == handle.identity_generation ? &candidate : nullptr;
}

TextureResourceHandle TextureResourceTable::acquire(TextureResourceDescriptor descriptor, SDL_GPUTexture* texture) {
    if (!valid_descriptor(descriptor) || texture == nullptr) return {};
    if (const auto existing = find(descriptor.stable_id, descriptor.semantic)) {
        auto* current = entry(*existing);
        return current != nullptr && current->descriptor.owner == descriptor.owner && current->texture == texture
            ? *existing : TextureResourceHandle{};
    }

    std::uint32_t slot{};
    if (free_slots_.empty()) {
        if (entries_.size() >= TextureResourceHandle::invalid_slot) return {};
        slot = static_cast<std::uint32_t>(entries_.size());
        entries_.push_back({});
    } else {
        slot = free_slots_.back();
        free_slots_.pop_back();
    }
    auto& created = entries_[slot];
    created.live = true;
    created.resource_generation = 1U;
    created.descriptor = std::move(descriptor);
    created.texture = texture;
    created.transition_pending = false;
    created.pending_texture = nullptr;
    const TextureResourceHandle handle{slot, created.identity_generation};
    keys_.emplace_back(key(created.descriptor.stable_id, created.descriptor.semantic), handle);
    std::ranges::sort(keys_, {}, &std::pair<std::string, TextureResourceHandle>::first);
    ++live_count_;
    ++revision_;
    return handle;
}

std::optional<TextureResourceHandle> TextureResourceTable::find(
    const std::string_view stable_id, const std::string_view semantic) const {
    const auto wanted = key(stable_id, semantic);
    const auto found = std::ranges::lower_bound(keys_, wanted, {}, &std::pair<std::string, TextureResourceHandle>::first);
    if (found == keys_.end() || found->first != wanted || entry(found->second) == nullptr) return std::nullopt;
    return found->second;
}

SDL_GPUTexture* TextureResourceTable::resolve(const TextureResourceHandle handle) const noexcept {
    const auto* resource = entry(handle);
    if (resource == nullptr) return nullptr;
    return resource->transition_pending ? resource->pending_texture : resource->texture;
}

std::optional<TextureResourceView> TextureResourceTable::view(const TextureResourceHandle handle) const {
    const auto* resource = entry(handle);
    if (resource == nullptr) return std::nullopt;
    auto descriptor = resource->descriptor;
    if (resource->transition_pending) descriptor.metadata = resource->pending_metadata;
    return TextureResourceView{handle, std::move(descriptor), resolve(handle),
        resource->resource_generation, resource->transition_pending};
}

bool TextureResourceTable::stage_replacement(const TextureResourceHandle handle, SDL_GPUTexture* texture,
                                             const TextureResourceMetadata metadata) {
    auto* resource = entry(handle);
    if (resource == nullptr || texture == nullptr || texture == resource->texture || resource->transition_pending ||
        metadata.width == 0U || metadata.height == 0U || metadata.mip_count == 0U ||
        metadata.resident_mip_start >= metadata.mip_count) return false;
    resource->transition_pending = true;
    resource->pending_texture = texture;
    resource->pending_metadata = metadata;
    ++revision_;
    return true;
}

SDL_GPUTexture* TextureResourceTable::commit_replacement(const TextureResourceHandle handle) {
    auto* resource = entry(handle);
    if (resource == nullptr || !resource->transition_pending) return nullptr;
    auto* previous = resource->texture;
    resource->texture = resource->pending_texture;
    resource->descriptor.metadata = resource->pending_metadata;
    resource->pending_texture = nullptr;
    resource->transition_pending = false;
    if (resource->texture != previous) ++resource->resource_generation;
    ++revision_;
    return previous;
}

SDL_GPUTexture* TextureResourceTable::rollback_replacement(const TextureResourceHandle handle) {
    auto* resource = entry(handle);
    if (resource == nullptr || !resource->transition_pending) return nullptr;
    auto* rejected = resource->pending_texture;
    resource->pending_texture = nullptr;
    resource->transition_pending = false;
    ++revision_;
    return rejected;
}

SDL_GPUTexture* TextureResourceTable::remove(const TextureResourceHandle handle) {
    auto* resource = entry(handle);
    if (resource == nullptr || resource->transition_pending) return nullptr;
    auto* texture = resource->texture;
    const auto erased_key = key(resource->descriptor.stable_id, resource->descriptor.semantic);
    std::erase_if(keys_, [&](const auto& item) { return item.first == erased_key && item.second == handle; });
    resource->live = false;
    resource->texture = nullptr;
    resource->descriptor = {};
    resource->resource_generation = 0U;
    resource->identity_generation = resource->identity_generation == std::numeric_limits<std::uint32_t>::max()
        ? 1U : resource->identity_generation + 1U;
    free_slots_.push_back(handle.slot);
    std::ranges::sort(free_slots_, std::greater{});
    --live_count_;
    ++revision_;
    return texture;
}

std::string TextureResourceTable::observe_json(const std::string_view owner,
                                               const std::size_t maximum_resources) const {
    Json resources = Json::array();
    std::size_t matched{};
    std::uint64_t allocation{};
    std::size_t pending{};
    for (const auto& [unused_key, handle] : keys_) {
        static_cast<void>(unused_key);
        const auto state = view(handle);
        if (!state || (!owner.empty() && state->descriptor.owner != owner)) continue;
        ++matched;
        allocation += state->descriptor.metadata.allocation_bytes_estimate;
        pending += state->transition_pending ? 1U : 0U;
        if (resources.size() >= maximum_resources) continue;
        resources.push_back({{"stableId", state->descriptor.stable_id}, {"semantic", state->descriptor.semantic},
            {"owner", state->descriptor.owner}, {"source", state->descriptor.source},
            {"residency", state->descriptor.residency}, {"handle", {{"slot", state->handle.slot},
                {"identityGeneration", state->handle.identity_generation}}},
            {"resourceGeneration", state->resource_generation},
            {"available", state->texture != nullptr}, {"transitionPending", state->transition_pending},
            {"metadata", metadata_json(state->descriptor.metadata)}});
    }
    return Json{{"schemaVersion", "noemancer.texture-resource-table/0.1"},
        {"ownerFilter", owner.empty() ? Json(nullptr) : Json(owner)}, {"resourceCount", matched},
        {"returnedCount", resources.size()}, {"truncated", resources.size() < matched},
        {"pendingTransitions", pending}, {"allocationBytesEstimate", allocation},
        {"physicalTelemetry", {{"available", false}, {"reason", "SDL_GPU does not expose backend memory budget telemetry"}}},
        {"resources", std::move(resources)}}.dump();
}

} // namespace noemancer
