#pragma once

#include "engine/asset_registry.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace noemancer {

// A thumbnail is a derived presentation artifact.  It is not part of the
// source AssetRecord and it never becomes a second asset authority.
struct ThumbnailRecipe final {
    std::string version{"noemancer.thumbnail-recipe/0.1"};
    std::uint32_t width{160U};
    std::uint32_t height{96U};
    std::string color_space{"srgb"};
};

struct ThumbnailSource final {
    AssetRecord asset;
    // Cook/import adapters may provide an already materialized preview.  The
    // thumbnail service only reuses safe, image-like artifacts; a meshbin or
    // KTX2 payload is not falsely advertised as a UI preview.
    std::vector<std::string> artifact_uris;
};

struct ThumbnailPlan final {
    bool valid{};
    std::string code;
    std::string detail;
    std::string schema{"noemancer.thumbnail-plan/0.1"};
    std::string asset_id;
    std::string source_kind;
    std::string strategy;
    std::string source_uri;
    std::string source_hash;
    std::string recipe_version;
    std::string recipe_fingerprint;
    std::string cache_key;
    std::string artifact_uri;
    std::string payload_format{"image/png"};
    std::uint32_t width{};
    std::uint32_t height{};
    bool reusable_artifact{};
    std::string reused_artifact_uri;
    std::vector<std::string> diagnostics;
};

struct ThumbnailSourceReadResult final {
    bool success{};
    std::string code;
    std::string detail;
    std::vector<std::byte> bytes;
};

using ThumbnailSourceReader = std::function<ThumbnailSourceReadResult(std::string_view source_uri)>;

using ThumbnailArtifactWriter = std::function<bool(
    std::string_view artifact_uri,
    std::string_view payload_format,
    std::span<const std::uint8_t> bytes,
    std::string& code,
    std::string& detail)>;

struct ThumbnailExecutionOptions final {
    ThumbnailSourceReader read_source;
    ThumbnailArtifactWriter write_artifact;
    std::size_t max_source_bytes{64U * 1024U * 1024U};
};

struct ThumbnailReceipt final {
    bool success{};
    bool cache_hit{};
    std::string code;
    std::string detail;
    std::string schema{"noemancer.thumbnail-receipt/0.1"};
    std::string asset_id;
    std::string strategy;
    std::string cache_key;
    std::string artifact_uri;
    std::string payload_format{"image/png"};
    std::string payload_fingerprint;
    std::uint32_t width{};
    std::uint32_t height{};
    std::size_t payload_bytes{};
    std::vector<std::uint8_t> payload;
    std::vector<std::string> diagnostics;
};

[[nodiscard]] ThumbnailPlan plan_thumbnail(
    const ThumbnailSource& source,
    const ThumbnailRecipe& recipe = {});

[[nodiscard]] ThumbnailPlan plan_thumbnail(
    const AssetRecord& asset,
    const ThumbnailRecipe& recipe = {});

// Executes only the plan's declared strategy.  PNG authoring sources use the
// existing image decoder/encoder adapter; all other supported source kinds get
// a deterministic PNG proxy until a domain-specific preview adapter exists.
// The optional writer owns persistence of the artifact URI.  Without a writer
// the receipt retains the encoded bytes, which is useful to a headless/editor
// adapter and keeps this service filesystem-neutral.
[[nodiscard]] ThumbnailReceipt execute_thumbnail(
    const ThumbnailPlan& plan,
    const ThumbnailExecutionOptions& options = {});

[[nodiscard]] std::string thumbnail_plan_json(
    const ThumbnailPlan& plan,
    std::size_t max_bytes = 16U * 1024U);

[[nodiscard]] std::string thumbnail_receipt_json(
    const ThumbnailReceipt& receipt,
    std::size_t max_bytes = 16U * 1024U);

} // namespace noemancer
