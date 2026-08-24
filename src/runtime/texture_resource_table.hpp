#pragma once

#include <SDL3/SDL_gpu.h>

#include <cstdint>
#include <optional>
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
