#pragma once

#include "engine/asset_cook_pipeline.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace noemancer {

// KTX-Software and Basis Universal are deliberately hidden behind this
// adapter.  Authoring, Cook plans, persisted artifacts and Agent observations
// only see these engine-owned records.
enum class TextureCookCompression : std::uint8_t {
    basis_lz,
    uastc
};

struct TextureCookMip final {
    std::uint32_t width{};
    std::uint32_t height{};
    std::vector<std::byte> rgba8;
};

struct TextureCookInput final {
    // The first level is read from rgba8.  Additional levels are optional and
    // must be ordered from the base level toward the 1x1 tail.  When the
    // caller supplies only the base level and mip generation is enabled in
    // TextureCookSettings, the adapter creates the remaining levels with a
    // deterministic box filter.
    std::uint32_t width{};
    std::uint32_t height{};
    std::vector<std::byte> rgba8;
    std::vector<TextureCookMip> mip_levels;
};

struct TextureCookProduct final {
    bool valid{};
    bool ktx_available{};
    std::string code;
    std::string detail;
    std::string schema{"noemancer.texture-artifact/0.1"};
    std::string source_asset_id;
    std::string source_hash;
    std::string target_profile;
    std::string semantic;
    std::string alpha_mode;
    std::string color_space;
    std::string compression;
    std::string supercompression;
    std::string payload_format{"ktx2"};
    std::string payload_fingerprint;
    std::uint32_t width{};
    std::uint32_t height{};
    std::uint32_t level_count{};
    std::vector<std::byte> payload;
    std::vector<std::string> diagnostics;
};

struct DecodedKtx2Texture final {
    bool valid{};
    std::string code;
    std::string detail;
    std::uint32_t width{};
    std::uint32_t height{};
    std::uint32_t level_count{};
    bool srgb{};
    std::vector<std::byte> rgba8;
};

// Runtime execution formats remain engine-owned. Backends translate these
// values to their native texture format only after querying device support.
enum class RuntimeTextureFormat : std::uint8_t {
    rgba8,
    bc7_rgba
};

struct RuntimeTextureMip final {
    std::uint32_t level{};
    std::uint32_t width{};
    std::uint32_t height{};
    std::vector<std::byte> bytes;
};

struct DecodedKtx2MipChain final {
    bool valid{};
    std::string code;
    std::string detail;
    RuntimeTextureFormat format{RuntimeTextureFormat::rgba8};
    std::uint32_t width{};
    std::uint32_t height{};
    bool srgb{};
    std::uint64_t source_bytes{};
    std::uint64_t upload_bytes{};
    std::vector<RuntimeTextureMip> levels;
};

[[nodiscard]] bool ktx2_available() noexcept;

[[nodiscard]] TextureCookProduct execute_texture_cook(
    const CookSource& source,
    const TextureCookInput& input,
    const CookPlatformProfile& profile,
    const TextureCookSettings& settings = {},
    TextureCookCompression compression = TextureCookCompression::basis_lz);

[[nodiscard]] std::string texture_cook_compression_name(TextureCookCompression compression);
[[nodiscard]] std::string texture_cook_product_json(const TextureCookProduct& product);
[[nodiscard]] std::string runtime_texture_format_name(RuntimeTextureFormat format);
// Validates a single 2D KTX2 payload and extracts every authored mip. Basis
// payloads are transcoded through libktx to the requested portable execution
// format; no third-party type crosses this boundary.
[[nodiscard]] DecodedKtx2MipChain decode_ktx2_mip_chain(
    std::span<const std::byte> payload,
    RuntimeTextureFormat format);
// Portable Runtime fallback. It validates and transcodes the base level to
// RGBA8 without leaking libktx types. Backends may later select native block
// formats directly when their capability/quality profile proves worthwhile.
[[nodiscard]] DecodedKtx2Texture decode_ktx2_rgba8(std::span<const std::byte> payload);

} // namespace noemancer
