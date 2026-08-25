#pragma once

#include <SDL3/SDL_gpu.h>

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace noemancer {

struct TextureResourceHandle final {
    static constexpr std::uint32_t invalid_slot = 0xffffffffU;
    std::uint32_t slot{invalid_slot};
    std::uint32_t identity_generation{};

    [[nodiscard]] bool valid() const noexcept {
        return slot != invalid_slot && identity_generation != 0U;
    }
    friend bool operator==(const TextureResourceHandle&, const TextureResourceHandle&) = default;
};

struct TextureResourceMetadata final {
    std::uint32_t width{};
    std::uint32_t height{};
    std::uint32_t mip_count{1U};
    std::uint32_t resident_mip_start{};
    std::uint64_t allocation_bytes_estimate{};
};

struct TextureResourceDescriptor final {
    std::string stable_id;
    std::string semantic;
    std::string owner;
    std::string source;
    std::string residency{"resident"};
    TextureResourceMetadata metadata;
};

struct TextureResourceView final {
    TextureResourceHandle handle;
    TextureResourceDescriptor descriptor;
    SDL_GPUTexture* texture{};
    std::uint64_t resource_generation{};
    bool transition_pending{};
};

// A binding request is an authored shader-slot identity plus an optional
// stable fallback.  It deliberately carries no SDL pointer: the table can
// project the request into a descriptor-array/bindless-ready identity while
// keeping physical resources private to Runtime.
struct TextureResourceBindingRequest final {
    TextureResourceHandle handle{};
    std::string semantic;
    std::string fallback_stable_id;
};

struct TextureResourceBindingSnapshotEntry final {
    std::string stable_id;
    std::string semantic;
    TextureResourceHandle handle{};
    std::string state; // committed, pending, fallback, or stale
    std::string owner;
    std::uint64_t committed_generation{};
    std::uint64_t effective_generation{};
    bool available{};
    bool transition_pending{};
    bool fallback{};
    bool stale_handle{};
    std::string fallback_stable_id;
};

// Canonical, bounded identity-only evidence for a sampled-resource binding
// set.  The canonical JSON and fingerprint are computed from stable strings,
// generations and state only; SDL handles are never serialized.
struct TextureResourceBindingSnapshot final {
    static constexpr std::size_t maximum_entries = 256U;

    std::string schema_version{"noemancer.texture-binding-snapshot/0.1"};
    bool valid{};
    std::string code;
    std::string detail;
    std::size_t requested_count{};
    std::size_t returned_count{};
    bool truncated{};
    std::vector<TextureResourceBindingSnapshotEntry> bindings;

    [[nodiscard]] std::string fingerprint() const;
    [[nodiscard]] std::string canonical_json() const;
};

// Runtime-private indirection for sampled GPU textures. Persisted documents and
// public commands retain stable Asset IDs; SDL handles never cross this table.
// Replacements are staged while a command buffer is recorded and become the
// committed generation only after the owner reports a successful submission.
class TextureResourceTable final {
public:
    [[nodiscard]] TextureResourceHandle acquire(
        TextureResourceDescriptor descriptor, SDL_GPUTexture* texture);
    [[nodiscard]] std::optional<TextureResourceHandle> find(
        std::string_view stable_id, std::string_view semantic) const;
    [[nodiscard]] SDL_GPUTexture* resolve(TextureResourceHandle handle) const noexcept;
    [[nodiscard]] std::optional<TextureResourceView> view(TextureResourceHandle handle) const;

    [[nodiscard]] bool stage_replacement(TextureResourceHandle handle, SDL_GPUTexture* texture,
                                         TextureResourceMetadata metadata);
    [[nodiscard]] SDL_GPUTexture* commit_replacement(TextureResourceHandle handle);
    [[nodiscard]] SDL_GPUTexture* rollback_replacement(TextureResourceHandle handle);
    [[nodiscard]] SDL_GPUTexture* remove(TextureResourceHandle handle);

    [[nodiscard]] std::string observe_json(std::string_view owner = {},
                                           std::size_t maximum_resources = 256U) const;
    [[nodiscard]] TextureResourceBindingSnapshot snapshot_bindings(
        std::span<const TextureResourceBindingRequest> requests,
        std::size_t maximum_bindings = TextureResourceBindingSnapshot::maximum_entries) const;
    [[nodiscard]] std::size_t size() const noexcept { return live_count_; }
    [[nodiscard]] std::uint64_t revision() const noexcept { return revision_; }

private:
    struct Entry final {
        bool live{};
        std::uint32_t identity_generation{1U};
        std::uint64_t resource_generation{};
        TextureResourceDescriptor descriptor;
        SDL_GPUTexture* texture{};
        bool transition_pending{};
        SDL_GPUTexture* pending_texture{};
        TextureResourceMetadata pending_metadata;
    };

    [[nodiscard]] Entry* entry(TextureResourceHandle handle) noexcept;
    [[nodiscard]] const Entry* entry(TextureResourceHandle handle) const noexcept;
    [[nodiscard]] static std::string key(std::string_view stable_id, std::string_view semantic);

    std::vector<Entry> entries_;
    std::vector<std::uint32_t> free_slots_;
    std::vector<std::pair<std::string, TextureResourceHandle>> keys_;
    std::size_t live_count_{};
    std::uint64_t revision_{};
};

} // namespace noemancer
