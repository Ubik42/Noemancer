#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace noemancer {

// This cache deliberately stores only engine-owned bytes.  KTX-Software,
// Basis Universal and image-library types stay behind the producer callback;
// the cache boundary is safe to use from the Registry, CLI and Agent planes.
struct SpriteAtlasPageCacheLimits final {
    // The limits apply to the dedicated cache namespace, not to the caller's
    // parent directory or to files outside the cache extension.
    std::size_t max_entries{256U};
    std::size_t max_entry_bytes{64U * 1024U * 1024U};
    std::size_t max_total_bytes{512U * 1024U * 1024U};
    std::size_t max_identity_component_bytes{1024U};
};

struct SpriteAtlasPageCacheRequest final {
    std::filesystem::path cache_root;
    std::string source_page_fingerprint;
    std::string page_layout_fingerprint;
    std::string cook_recipe_fingerprint;
    std::string profile_fingerprint;
    std::string compression;
    std::string worker_identity;
    SpriteAtlasPageCacheLimits limits;
};

struct SpriteAtlasPageProduced final {
    bool success{};
    std::string code;
    std::string detail;
    std::vector<std::byte> payload;
};

using SpriteAtlasPageProducer = std::function<SpriteAtlasPageProduced()>;

struct SpriteAtlasPageCacheReceipt final {
    bool success{};
    bool cache_hit{};
    bool cache_miss{};
    bool rebuilt{};
    std::string code;
    std::string detail;
    std::string cache_key;
    std::filesystem::path artifact_path;
    std::string payload_fingerprint;
    std::size_t payload_bytes{};
    std::size_t cache_entries{};
    std::size_t cache_bytes{};
    std::vector<std::byte> payload;
    std::vector<std::string> diagnostics;
};

// The returned key is empty when any identity component is invalid.  Every
// component is included in the SHA-256 material, so changing a page source,
// layout, recipe/profile, compression mode or worker identity cannot alias an
// existing encoded page.
[[nodiscard]] std::string sprite_atlas_page_cache_key(
    const SpriteAtlasPageCacheRequest& request);

[[nodiscard]] SpriteAtlasPageCacheReceipt execute_sprite_atlas_page_cache(
    const SpriteAtlasPageCacheRequest& request,
    const SpriteAtlasPageProducer& producer);

[[nodiscard]] std::string sprite_atlas_page_cache_receipt_json(
    const SpriteAtlasPageCacheReceipt& receipt,
    std::size_t max_bytes = 128U * 1024U);

} // namespace noemancer
