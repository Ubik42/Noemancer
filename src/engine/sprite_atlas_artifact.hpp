#pragma once

#include "engine/asset_cook_pipeline.hpp"
#include "engine/ktx2_cook_adapter.hpp"
#include "engine/sprite_asset.hpp"
#include "engine/sprite_atlas_page_cache.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace noemancer {

// A cooked page is an engine-owned record.  The page payload is an independent
// KTX2 artifact; no libktx/Basis type crosses this boundary.
struct SpriteAtlasPageArtifact final {
    bool valid{};
    bool cache_hit{};
    bool rebuilt{};
    std::uint32_t page_index{};
    std::string asset_id;
    std::uint32_t width{};
    std::uint32_t height{};
    std::size_t frame_count{};
    std::uint64_t input_bytes{};
    std::string content_fingerprint;
    std::string payload_format{"ktx2"};
    std::string payload_fingerprint;
    std::uint64_t payload_bytes{};
    std::string cache_key;
    std::vector<std::byte> payload;
    std::string code;
    std::string detail;
};

struct SpriteAtlasFrameBinding final {
    std::string frame_id;
    std::uint32_t page_index{};
    std::uint32_t x{};
    std::uint32_t y{};
    std::uint32_t width{};
    std::uint32_t height{};
};

struct SpriteAtlasArtifact final {
    bool valid{};
    std::string schema{"noemancer.sprite-atlas-artifact/0.1"};
    std::string code{"sprite.atlas-artifact-invalid"};
    std::string detail;
    std::string source_asset_id;
    std::string source_hash;
    std::string target_profile;
    std::uint32_t page_width{};
    std::uint32_t page_height{};
    std::uint32_t padding{};
    std::uint64_t layout_fingerprint{};
    std::string bundle_fingerprint;
    std::uint64_t full_page_bytes{};
    std::uint64_t incremental_page_bytes{};
    std::vector<std::uint32_t> full_page_indices;
    std::vector<std::uint32_t> incremental_page_indices;
    std::vector<SpriteAtlasPageArtifact> pages;
    std::vector<SpriteAtlasFrameBinding> bindings;
    std::vector<SpriteAssetError> diagnostics;
};

struct SpriteAtlasArtifactExecutionOptions final {
    // Empty means no disk cache and preserves the original in-memory Cook
    // path. The cache owns only KTX2 bytes and is never required for
    // correctness.
    std::filesystem::path page_cache_root;
    SpriteAtlasPageCacheLimits cache_limits{};
    TextureCookExecutionOptions cook_execution{};
    TextureCookCompression compression{TextureCookCompression::basis_lz};
    // When empty, the request derives a stable identity from the requested
    // Cook worker count. Explicit identities are useful for CI/platform
    // lanes, but remain part of the cache key by construction.
    std::string worker_identity;
};

struct SpriteAtlasArtifactParseResult final {
    std::optional<SpriteAtlasArtifact> artifact;
    std::vector<SpriteAssetError> errors;
    [[nodiscard]] explicit operator bool() const noexcept { return artifact.has_value(); }
};

// Manifest projection limits keep Agent/editor reads bounded even when a
// source contains tens of thousands of frames. Payload bytes are deliberately
// omitted from JSON; they remain available through page.payload.
inline constexpr std::size_t sprite_atlas_artifact_max_manifest_pages{4096U};
inline constexpr std::size_t sprite_atlas_artifact_max_manifest_bindings{4096U};

[[nodiscard]] SpriteAtlasArtifact execute_sprite_atlas_artifact(
    const SpriteAssetDocument& document,
    std::span<const std::byte> atlas_rgba8,
    const SpriteAtlasPlanningOptions& planning_options,
    const CookSource& source,
    const CookPlatformProfile& profile,
    const TextureCookSettings& texture_settings = {},
    const std::vector<std::string>& changed_frame_ids = {},
    const SpriteAtlasArtifactExecutionOptions& execution_options = {});

// Validates the engine-owned page/binding contract.  A parsed manifest has no
// payload bytes, so payload hashes are checked when payload is present and
// metadata/identity invariants are always checked.
[[nodiscard]] std::vector<SpriteAssetError> validate_sprite_atlas_artifact(
    const SpriteAtlasArtifact& artifact);

[[nodiscard]] std::string sprite_atlas_artifact_json(
    const SpriteAtlasArtifact& artifact,
    std::size_t max_pages = sprite_atlas_artifact_max_manifest_pages,
    std::size_t max_bindings = sprite_atlas_artifact_max_manifest_bindings);

[[nodiscard]] SpriteAtlasArtifactParseResult parse_sprite_atlas_artifact_json(
    std::string_view json,
    std::size_t max_pages = sprite_atlas_artifact_max_manifest_pages,
    std::size_t max_bindings = sprite_atlas_artifact_max_manifest_bindings);

// Projects a validated Cook artifact into the runtime-only Sprite library
// overlay. Invalid artifacts or pages without a payload identity return an
// empty batch so callers cannot register ambiguous page identities. Parsed
// manifests may project bindings without loading the KTX2 bytes themselves.
[[nodiscard]] std::vector<SpriteRuntimePageBinding> sprite_runtime_page_bindings(
    const SpriteAtlasArtifact& artifact);

} // namespace noemancer
