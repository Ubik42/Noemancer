#include "engine/sprite_atlas_artifact.hpp"

#include "engine/content_hash.hpp"
#include "engine/ktx2_cook_adapter.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iomanip>
#include <limits>
#include <ranges>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace noemancer {
namespace {

using Json = nlohmann::json;

constexpr std::size_t kMaxManifestBytes = 16U * 1024U * 1024U;

void add_error(std::vector<SpriteAssetError>& errors, std::string code,
               std::string path, std::string message) {
    errors.push_back(SpriteAssetError{
        .code = std::move(code),
        .path = std::move(path),
        .message = std::move(message)
    });
}

bool checked_multiply(const std::uint64_t left, const std::uint64_t right,
                      std::uint64_t& output) noexcept {
    if (left != 0U && right > std::numeric_limits<std::uint64_t>::max() / left) return false;
    output = left * right;
    return true;
}

bool checked_add(const std::uint64_t left, const std::uint64_t right,
                 std::uint64_t& output) noexcept {
    if (right > std::numeric_limits<std::uint64_t>::max() - left) return false;
    output = left + right;
    return true;
}

std::string hex_u64(const std::uint64_t value) {
    std::ostringstream stream;
    stream << std::hex << std::setfill('0') << std::setw(16) << value;
    return stream.str();
}

std::string page_content_fingerprint(const std::uint32_t width, const std::uint32_t height,
                                     const std::span<const std::byte> pixels) {
    std::string material = std::to_string(width) + "x" + std::to_string(height) + ":";
    if (!pixels.empty())
        material.append(reinterpret_cast<const char*>(pixels.data()), pixels.size());
    const auto hash = sha256_bytes(std::span<const std::byte>(
        reinterpret_cast<const std::byte*>(material.data()), material.size()));
    return hash.success ? hash.value : std::string{};
}

std::string payload_fingerprint(const std::span<const std::byte> payload) {
    const auto hash = sha256_bytes(payload);
    return hash.success ? hash.value : std::string{};
}

bool valid_sha256_identity(const std::string_view value) noexcept {
    constexpr std::string_view prefix = "sha256:";
    if (!value.starts_with(prefix) || value.size() != prefix.size() + 64U) return false;
    return std::ranges::all_of(value.substr(prefix.size()), [](const char character) {
        return std::isxdigit(static_cast<unsigned char>(character)) != 0;
    });
}

std::string page_asset_id(const std::string_view source_asset_id, const std::uint32_t page_index) {
    return std::string(source_asset_id) + ".atlas.page." + std::to_string(page_index);
}

std::string page_source_hash(const std::string_view content_fingerprint) {
    return std::string(content_fingerprint);
}

std::string cache_worker_identity(const SpriteAtlasArtifactExecutionOptions& options) {
    if (!options.worker_identity.empty()) return options.worker_identity;
    return "workers=" + std::to_string(resolve_texture_cook_worker_count(
        options.cook_execution.requested_worker_count));
}

void append_plan_errors(SpriteAtlasArtifact& result,
                        const SpriteAtlasPlanningReport& report) {
    result.diagnostics = report.diagnostics;
    result.code = report.code;
    if (!report.diagnostics.empty()) result.detail = report.diagnostics.front().message;
}

void append_cook_error(SpriteAtlasArtifact& result, const std::uint32_t page_index,
                       const TextureCookProduct& product) {
    add_error(result.diagnostics, product.code.empty() ? "sprite.atlas-page-cook-failed" : product.code,
              "/pages/" + std::to_string(page_index),
              product.detail.empty() ? "Atlas page Cook failed." : product.detail);
}

std::string bundle_fingerprint(const SpriteAtlasArtifact& artifact) {
    std::string material;
    const auto append = [&material](const std::string_view label, const std::string_view value) {
        material += label;
        material += std::to_string(value.size());
        material.push_back(':');
        material.append(value);
        material.push_back('\n');
    };
    append("schema=", artifact.schema);
    append("asset=", artifact.source_asset_id);
    // Source hashes intentionally do not participate in the bundle identity:
    // a cache hit/miss or a source alias must not change an identical cooked
    // page set.
    append("profile=", artifact.target_profile);
    append("page-width=", std::to_string(artifact.page_width));
    append("page-height=", std::to_string(artifact.page_height));
    append("padding=", std::to_string(artifact.padding));
    append("layout=", std::to_string(artifact.layout_fingerprint));
    for (const auto& page : artifact.pages) {
        append("page-index=", std::to_string(page.page_index));
        append("page-asset=", page.asset_id);
        append("page-width=", std::to_string(page.width));
        append("page-height=", std::to_string(page.height));
        append("page-frames=", std::to_string(page.frame_count));
        append("page-input-bytes=", std::to_string(page.input_bytes));
        append("page-content=", page.content_fingerprint);
        append("page-format=", page.payload_format);
        append("page-payload=", page.payload_fingerprint);
        append("page-payload-bytes=", std::to_string(page.payload_bytes));
    }
    for (const auto& binding : artifact.bindings) {
        append("frame=", binding.frame_id);
        append("frame-page=", std::to_string(binding.page_index));
        append("frame-x=", std::to_string(binding.x));
        append("frame-y=", std::to_string(binding.y));
        append("frame-width=", std::to_string(binding.width));
        append("frame-height=", std::to_string(binding.height));
    }
    for (const auto index : artifact.full_page_indices) append("full-page=", std::to_string(index));
    for (const auto index : artifact.incremental_page_indices)
        append("incremental-page=", std::to_string(index));
    const auto hash = sha256_bytes(std::span<const std::byte>(
        reinterpret_cast<const std::byte*>(material.data()), material.size()));
    return hash.success ? hash.value : std::string{};
}

Json diagnostic_json(const std::vector<SpriteAssetError>& diagnostics) {
    Json items = Json::array();
    for (const auto& diagnostic : diagnostics) {
        items.push_back(Json{
            {"code", diagnostic.code},
            {"path", diagnostic.path},
            {"message", diagnostic.message}
        });
    }
    return items;
}

Json page_timing_free_json(const SpriteAtlasPageArtifact& page) {
    return Json{
        {"valid", page.valid},
        {"cacheHit", page.cache_hit},
        {"rebuilt", page.rebuilt},
        {"pageIndex", page.page_index},
        {"assetId", page.asset_id},
        {"size", Json::array({page.width, page.height})},
        {"frameCount", page.frame_count},
        {"inputBytes", page.input_bytes},
        {"contentFingerprint", page.content_fingerprint},
        {"payload", {
            {"format", page.payload_format},
            {"bytes", page.payload_bytes},
            {"fingerprint", page.payload_fingerprint}}},
        {"cacheKey", page.cache_key},
        {"code", page.code},
        {"detail", page.detail}
    };
}

bool json_has_string(const Json& value, const char* key, std::string& output) {
    if (!value.contains(key) || !value.at(key).is_string()) return false;
    output = value.at(key).get<std::string>();
    return true;
}

bool json_has_bool(const Json& value, const char* key, bool& output) {
    if (!value.contains(key) || !value.at(key).is_boolean()) return false;
    output = value.at(key).get<bool>();
    return true;
}

bool json_has_u32(const Json& value, const char* key, std::uint32_t& output) {
    if (!value.contains(key) || !value.at(key).is_number_unsigned()) return false;
    const auto number = value.at(key).get<std::uint64_t>();
    if (number > std::numeric_limits<std::uint32_t>::max()) return false;
    output = static_cast<std::uint32_t>(number);
    return true;
}

bool json_has_u64(const Json& value, const char* key, std::uint64_t& output) {
    if (!value.contains(key) || !value.at(key).is_number_unsigned()) return false;
    output = value.at(key).get<std::uint64_t>();
    return true;
}

bool json_has_size(const Json& value, const char* key, std::size_t& output) {
    std::uint64_t number{};
    if (!json_has_u64(value, key, number) || number > std::numeric_limits<std::size_t>::max()) return false;
    output = static_cast<std::size_t>(number);
    return true;
}

bool parse_size_pair(const Json& value, std::uint32_t& width, std::uint32_t& height) {
    if (!value.is_array() || value.size() != 2U) return false;
    if (!value.at(0).is_number_unsigned() || !value.at(1).is_number_unsigned()) return false;
    const auto width_value = value.at(0).get<std::uint64_t>();
    const auto height_value = value.at(1).get<std::uint64_t>();
    if (width_value == 0U || height_value == 0U ||
        width_value > std::numeric_limits<std::uint32_t>::max() ||
        height_value > std::numeric_limits<std::uint32_t>::max()) return false;
    width = static_cast<std::uint32_t>(width_value);
    height = static_cast<std::uint32_t>(height_value);
    return true;
}

bool parse_page_indices(const Json& value, std::vector<std::uint32_t>& output,
                        const std::size_t max_items) {
    if (!value.is_array() || value.size() > max_items) return false;
    output.reserve(value.size());
    for (const auto& item : value) {
        if (!item.is_number_unsigned()) return false;
        const auto index = item.get<std::uint64_t>();
        if (index > std::numeric_limits<std::uint32_t>::max()) return false;
        output.push_back(static_cast<std::uint32_t>(index));
    }
    return true;
}

} // namespace

SpriteAtlasArtifact execute_sprite_atlas_artifact(
    const SpriteAssetDocument& document,
    const std::span<const std::byte> atlas_rgba8,
    const SpriteAtlasPlanningOptions& planning_options,
    const CookSource& source,
    const CookPlatformProfile& profile,
    const TextureCookSettings& texture_settings,
    const std::vector<std::string>& changed_frame_ids,
    const SpriteAtlasArtifactExecutionOptions& execution_options) {
    SpriteAtlasArtifact result;
    result.source_asset_id = source.asset_id.empty() ? document.asset_id : source.asset_id;
    result.source_hash = source.source_hash;
    result.target_profile = profile.id;
    result.page_width = planning_options.page_width;
    result.page_height = planning_options.page_height;
    result.padding = planning_options.padding;

    const auto plan = SpriteAssetCodec::plan_atlas_pages(document, planning_options, changed_frame_ids);
    if (!plan.valid) {
        append_plan_errors(result, plan);
        return result;
    }
    result.layout_fingerprint = plan.layout_fingerprint;

    std::uint64_t source_pixels{};
    std::uint64_t expected_source_bytes{};
    if (!checked_multiply(document.texture_width, document.texture_height, source_pixels) ||
        !checked_multiply(source_pixels, 4U, expected_source_bytes) ||
        expected_source_bytes > std::numeric_limits<std::size_t>::max() ||
        expected_source_bytes != atlas_rgba8.size()) {
        add_error(result.diagnostics, "sprite.atlas-source-size-invalid", "/atlasRgba8",
                  "Raw atlas bytes must equal textureWidth*textureHeight*4 without overflow.");
        result.code = result.diagnostics.back().code;
        result.detail = result.diagnostics.back().message;
        return result;
    }
    if (expected_source_bytes > planning_options.limits.max_estimated_cook_bytes) {
        add_error(result.diagnostics, "sprite.atlas-source-budget", "/atlasRgba8",
                  "Raw atlas bytes exceed the bounded atlas Cook budget.");
        result.code = result.diagnostics.back().code;
        result.detail = result.diagnostics.back().message;
        return result;
    }

    std::uint64_t page_pixels{};
    std::uint64_t page_bytes{};
    std::uint64_t all_page_bytes{};
    if (!checked_multiply(planning_options.page_width, planning_options.page_height, page_pixels) ||
        !checked_multiply(page_pixels, 4U, page_bytes) ||
        !checked_multiply(page_bytes, plan.page_count, all_page_bytes) ||
        all_page_bytes > planning_options.limits.max_estimated_cook_bytes ||
        page_bytes > std::numeric_limits<std::size_t>::max()) {
        add_error(result.diagnostics, "sprite.atlas-page-budget", "/planning",
                  "Cooked RGBA page storage exceeds the bounded atlas budget.");
        result.code = result.diagnostics.back().code;
        result.detail = result.diagnostics.back().message;
        return result;
    }

    result.full_page_indices.reserve(plan.page_count);
    for (std::size_t index = 0U; index < plan.page_count; ++index)
        result.full_page_indices.push_back(static_cast<std::uint32_t>(index));
    result.incremental_page_indices = plan.affected_page_indices;
    result.bindings.reserve(plan.placements.size());
    for (const auto& placement : plan.placements) {
        result.bindings.push_back(SpriteAtlasFrameBinding{
            .frame_id = placement.frame_id,
            .page_index = placement.page_index,
            .x = placement.x,
            .y = placement.y,
            .width = placement.width,
            .height = placement.height
        });
    }

    std::unordered_map<std::string, const SpriteFrame*> frames;
    frames.reserve(document.frames.size());
    for (const auto& frame : document.frames) frames.emplace(frame.id, &frame);

    std::vector<std::vector<const SpriteAtlasFramePlacement*>> placements_by_page(plan.page_count);
    for (const auto& placement : plan.placements) {
        if (placement.page_index >= placements_by_page.size()) {
            add_error(result.diagnostics, "sprite.atlas-placement-invalid", "/placements",
                      "Planner emitted a placement outside the page set.");
            result.code = result.diagnostics.back().code;
            result.detail = result.diagnostics.back().message;
            return result;
        }
        placements_by_page[placement.page_index].push_back(&placement);
    }

    result.pages.reserve(plan.page_count);
    for (std::size_t page_index = 0U; page_index < plan.page_count; ++page_index) {
        SpriteAtlasPageArtifact page;
        page.page_index = static_cast<std::uint32_t>(page_index);
        page.asset_id = page_asset_id(result.source_asset_id, page.page_index);
        page.width = planning_options.page_width;
        page.height = planning_options.page_height;
        page.frame_count = placements_by_page[page_index].size();
        page.input_bytes = page_bytes;
        page.code = "sprite.atlas-page-invalid";

        std::vector<std::byte> page_pixels_rgba(static_cast<std::size_t>(page_bytes), std::byte{0});
        bool copy_valid = true;
        for (const auto* placement : placements_by_page[page_index]) {
            const auto frame_iterator = frames.find(placement->frame_id);
            if (frame_iterator == frames.end()) {
                add_error(result.diagnostics, "sprite.atlas-unknown-frame", "/bindings",
                          "Planner emitted a binding for an unknown frame.");
                copy_valid = false;
                break;
            }
            const auto& frame = *frame_iterator->second;
            if (static_cast<std::uint64_t>(placement->x) + placement->width > page.width ||
                static_cast<std::uint64_t>(placement->y) + placement->height > page.height ||
                static_cast<std::uint64_t>(frame.x) + frame.width > document.texture_width ||
                static_cast<std::uint64_t>(frame.y) + frame.height > document.texture_height) {
                add_error(result.diagnostics, "sprite.atlas-copy-out-of-bounds", "/pages",
                          "Frame copy rectangle exceeds the source atlas or destination page.");
                copy_valid = false;
                break;
            }
            for (std::uint32_t row = 0U; row < frame.height; ++row) {
                std::uint64_t source_pixel{};
                std::uint64_t source_byte{};
                std::uint64_t destination_pixel{};
                std::uint64_t destination_byte{};
                std::uint64_t row_bytes{};
                if (!checked_multiply(static_cast<std::uint64_t>(frame.y) + row,
                                      document.texture_width, source_pixel) ||
                    !checked_add(source_pixel, frame.x, source_pixel) ||
                    !checked_multiply(source_pixel, 4U, source_byte) ||
                    !checked_multiply(static_cast<std::uint64_t>(placement->y) + row,
                                      page.width, destination_pixel) ||
                    !checked_add(destination_pixel, placement->x, destination_pixel) ||
                    !checked_multiply(destination_pixel, 4U, destination_byte) ||
                    !checked_multiply(frame.width, 4U, row_bytes) ||
                    source_byte > atlas_rgba8.size() || destination_byte > page_pixels_rgba.size() ||
                    row_bytes > atlas_rgba8.size() - source_byte ||
                    row_bytes > page_pixels_rgba.size() - destination_byte ||
                    row_bytes > std::numeric_limits<std::size_t>::max()) {
                    add_error(result.diagnostics, "sprite.atlas-copy-overflow", "/pages",
                              "Frame copy offset cannot be represented safely.");
                    copy_valid = false;
                    break;
                }
                std::copy_n(atlas_rgba8.data() + static_cast<std::size_t>(source_byte),
                            static_cast<std::size_t>(row_bytes),
                            page_pixels_rgba.data() + static_cast<std::size_t>(destination_byte));
            }
            if (!copy_valid) break;
        }
        if (!copy_valid) {
            result.code = result.diagnostics.back().code;
            result.detail = result.diagnostics.back().message;
            page.detail = result.detail;
            result.pages.push_back(std::move(page));
            return result;
        }

        page.content_fingerprint = page_content_fingerprint(
            page.width, page.height,
            std::span<const std::byte>(page_pixels_rgba.data(), page_pixels_rgba.size()));
        const CookSource page_source{
            .asset_id = page.asset_id,
            .source_uri = source.source_uri + "#page/" + std::to_string(page.page_index),
            .source_hash = page_source_hash(page.content_fingerprint),
            .source_bytes = page_pixels_rgba.size(),
            .importer = source.importer + "/sprite-atlas"
        };
        const TextureCookInput page_input{
            .width = page.width,
            .height = page.height,
            .rgba8 = std::move(page_pixels_rgba)
        };
        const auto produce_page = [&]() -> SpriteAtlasPageProduced {
            const auto cooked = execute_texture_cook(
                page_source, page_input, profile, texture_settings,
                execution_options.compression, execution_options.cook_execution);
            if (!cooked.valid) {
                return SpriteAtlasPageProduced{
                    .success = false,
                    .code = cooked.code,
                    .detail = cooked.detail,
                    .payload = {}};
            }
            return SpriteAtlasPageProduced{
                .success = true,
                .code = cooked.code,
                .detail = cooked.detail,
                .payload = cooked.payload};
        };

        SpriteAtlasPageCacheReceipt cache_receipt;
        if (!execution_options.page_cache_root.empty()) {
            const auto cook_plan = plan_texture_cook(page_source, profile, texture_settings);
            if (!cook_plan.valid) {
                add_error(result.diagnostics, cook_plan.code, "/pages/" + std::to_string(page.page_index),
                          cook_plan.detail);
                page.code = cook_plan.code;
                page.detail = cook_plan.detail;
                result.pages.push_back(std::move(page));
                result.code = result.diagnostics.back().code;
                result.detail = result.diagnostics.back().message;
                return result;
            }
            SpriteAtlasPageCacheRequest cache_request;
            cache_request.cache_root = execution_options.page_cache_root;
            cache_request.source_page_fingerprint = page.content_fingerprint;
            cache_request.page_layout_fingerprint =
                "fnv1a64:" + hex_u64(plan.pages[page_index].layout_fingerprint);
            cache_request.cook_recipe_fingerprint = cook_plan.settings_fingerprint;
            cache_request.profile_fingerprint = cook_platform_profile_fingerprint(profile);
            cache_request.compression = texture_cook_compression_name(execution_options.compression);
            cache_request.worker_identity = cache_worker_identity(execution_options);
            cache_request.limits = execution_options.cache_limits;
            cache_receipt = execute_sprite_atlas_page_cache(cache_request, produce_page);
            page.cache_hit = cache_receipt.cache_hit;
            page.rebuilt = cache_receipt.rebuilt;
            page.cache_key = cache_receipt.cache_key;
            page.code = cache_receipt.code;
            page.detail = cache_receipt.detail;
            if (!cache_receipt.success) {
                add_error(result.diagnostics, cache_receipt.code, "/pages/" + std::to_string(page.page_index),
                          cache_receipt.detail);
                result.pages.push_back(std::move(page));
                result.code = result.diagnostics.back().code;
                result.detail = result.diagnostics.back().message;
                return result;
            }
            page.payload = std::move(cache_receipt.payload);
            page.payload_fingerprint = payload_fingerprint(
                std::span<const std::byte>(page.payload.data(), page.payload.size()));
            if (page.payload_fingerprint.empty() ||
                page.payload_fingerprint != cache_receipt.payload_fingerprint) {
                add_error(result.diagnostics, "sprite.atlas-payload-hash-invalid",
                          "/pages/" + std::to_string(page.page_index),
                          "The page cache receipt payload did not preserve its SHA-256 identity.");
                result.pages.push_back(std::move(page));
                result.code = result.diagnostics.back().code;
                result.detail = result.diagnostics.back().message;
                return result;
            }
        } else {
            const auto produced = produce_page();
            page.code = produced.code;
            page.detail = produced.detail;
            if (!produced.success) {
                add_error(result.diagnostics, produced.code, "/pages/" + std::to_string(page.page_index),
                          produced.detail);
                result.pages.push_back(std::move(page));
                result.code = result.diagnostics.back().code;
                result.detail = result.diagnostics.back().message;
                return result;
            }
            page.payload = produced.payload;
            page.payload_fingerprint = payload_fingerprint(
                std::span<const std::byte>(page.payload.data(), page.payload.size()));
        }
        if (page.payload_fingerprint.empty()) {
            add_error(result.diagnostics, "sprite.atlas-payload-hash-failed",
                      "/pages/" + std::to_string(page.page_index),
                      "SHA-256 payload identity could not be computed.");
            result.pages.push_back(std::move(page));
            result.code = result.diagnostics.back().code;
            result.detail = result.diagnostics.back().message;
            return result;
        }
        page.payload_format = "ktx2";
        page.payload_bytes = page.payload.size();
        page.valid = true;
        result.pages.push_back(std::move(page));
    }

    for (const auto& page : result.pages) {
        if (!checked_add(result.full_page_bytes, page.payload_bytes, result.full_page_bytes)) {
            add_error(result.diagnostics, "sprite.atlas-payload-overflow", "/pages",
                      "Full atlas payload byte count overflowed.");
            result.code = result.diagnostics.back().code;
            result.detail = result.diagnostics.back().message;
            return result;
        }
    }
    if (result.full_page_bytes > planning_options.limits.max_estimated_cook_bytes) {
        add_error(result.diagnostics, "sprite.atlas-payload-budget", "/cookSets/full/bytes",
                  "Encoded atlas payload exceeds the bounded atlas Cook budget.");
        result.code = result.diagnostics.back().code;
        result.detail = result.diagnostics.back().message;
        return result;
    }
    for (const auto page_index : result.incremental_page_indices) {
        if (page_index >= result.pages.size() ||
            !checked_add(result.incremental_page_bytes, result.pages[page_index].payload_bytes,
                         result.incremental_page_bytes)) {
            add_error(result.diagnostics, "sprite.atlas-incremental-overflow", "/cookSets/incremental",
                      "Incremental atlas payload byte count overflowed.");
            result.code = result.diagnostics.back().code;
            result.detail = result.diagnostics.back().message;
            return result;
        }
    }
    result.bundle_fingerprint = bundle_fingerprint(result);
    result.valid = true;
    result.code = "ok";
    result.detail = "Sprite atlas pages copied into transparent RGBA surfaces and cooked as independent KTX2 artifacts.";
    return result;
}

std::vector<SpriteAssetError> validate_sprite_atlas_artifact(
    const SpriteAtlasArtifact& artifact) {
    std::vector<SpriteAssetError> errors;
    if (artifact.schema != "noemancer.sprite-atlas-artifact/0.1") {
        add_error(errors, "sprite.atlas-artifact-schema-invalid", "/schema",
                  "Expected noemancer.sprite-atlas-artifact/0.1.");
    }
    if (artifact.page_width == 0U || artifact.page_height == 0U) {
        add_error(errors, "sprite.atlas-artifact-size-invalid", "/layout",
                  "Atlas page dimensions must be positive.");
    }
    if (artifact.pages.size() > sprite_atlas_plan_max_projected_pages) {
        add_error(errors, "sprite.atlas-artifact-page-limit", "/pages",
                  "Artifact page count exceeds the bounded manifest limit.");
    }
    for (std::size_t index = 0U; index < artifact.pages.size(); ++index) {
        const auto& page = artifact.pages[index];
        if (page.page_index != index || page.asset_id.empty() || page.width != artifact.page_width ||
            page.height != artifact.page_height || page.input_bytes == 0U) {
            add_error(errors, "sprite.atlas-artifact-page-invalid", "/pages/" + std::to_string(index),
                      "Page identity, dimensions or input byte count is invalid.");
        }
        if (!valid_sha256_identity(page.content_fingerprint)) {
            add_error(errors, "sprite.atlas-artifact-content-hash-invalid",
                      "/pages/" + std::to_string(index) + "/contentFingerprint",
                      "Page content identity must use SHA-256.");
        }
        if (page.payload_bytes != 0U && !valid_sha256_identity(page.payload_fingerprint)) {
            add_error(errors, "sprite.atlas-artifact-payload-hash-invalid",
                      "/pages/" + std::to_string(index) + "/payload/fingerprint",
                      "Page payload identity must use SHA-256.");
        }
        if (!page.payload.empty()) {
            if (page.payload_bytes != page.payload.size() ||
                page.payload_fingerprint != payload_fingerprint(
                    std::span<const std::byte>(page.payload.data(), page.payload.size()))) {
                add_error(errors, "sprite.atlas-artifact-payload-invalid",
                          "/pages/" + std::to_string(index) + "/payload",
                          "Page payload bytes do not match its stable fingerprint.");
            }
        }
    }
    const auto check_indices = [&](const std::vector<std::uint32_t>& indices,
                                   const std::string_view path) {
        std::uint32_t previous = 0U;
        bool first = true;
        for (const auto index : indices) {
            if (index >= artifact.pages.size() || (!first && index <= previous)) {
                add_error(errors, "sprite.atlas-artifact-page-set-invalid", std::string(path),
                          "Page sets must be strictly increasing and reference existing pages.");
                break;
            }
            previous = index;
            first = false;
        }
    };
    check_indices(artifact.full_page_indices, "/cookSets/full/pageIndices");
    check_indices(artifact.incremental_page_indices, "/cookSets/incremental/pageIndices");
    for (const auto index : artifact.incremental_page_indices) {
        if (!std::ranges::binary_search(artifact.full_page_indices, index)) {
            add_error(errors, "sprite.atlas-artifact-incremental-page-invalid",
                      "/cookSets/incremental/pageIndices",
                      "Incremental pages must be a subset of the full page set.");
            break;
        }
    }
    for (const auto& binding : artifact.bindings) {
        if (binding.frame_id.empty() || binding.page_index >= artifact.pages.size() ||
            static_cast<std::uint64_t>(binding.x) + binding.width > artifact.page_width ||
            static_cast<std::uint64_t>(binding.y) + binding.height > artifact.page_height ||
            binding.width == 0U || binding.height == 0U) {
            add_error(errors, "sprite.atlas-artifact-binding-invalid", "/bindings",
                      "Frame binding must reference a bounded non-empty page rectangle.");
            break;
        }
    }
    std::uint64_t full_bytes{};
    for (const auto& page : artifact.pages) {
        if (!checked_add(full_bytes, page.payload_bytes, full_bytes)) {
            add_error(errors, "sprite.atlas-artifact-byte-overflow", "/cookSets/full",
                      "Full payload byte count overflowed.");
            break;
        }
    }
    if (full_bytes != artifact.full_page_bytes) {
        add_error(errors, "sprite.atlas-artifact-byte-mismatch", "/cookSets/full/bytes",
                  "Full page byte count does not match the page records.");
    }
    std::uint64_t incremental_bytes{};
    for (const auto index : artifact.incremental_page_indices) {
        if (index < artifact.pages.size() &&
            !checked_add(incremental_bytes, artifact.pages[index].payload_bytes, incremental_bytes)) break;
    }
    if (incremental_bytes != artifact.incremental_page_bytes) {
        add_error(errors, "sprite.atlas-artifact-byte-mismatch", "/cookSets/incremental/bytes",
                  "Incremental page byte count does not match the page records.");
    }
    if (!artifact.bundle_fingerprint.empty() && !valid_sha256_identity(artifact.bundle_fingerprint)) {
        add_error(errors, "sprite.atlas-artifact-bundle-hash-invalid", "/bundleFingerprint",
                  "Bundle identity must use SHA-256.");
    } else if (!artifact.bundle_fingerprint.empty() && artifact.bundle_fingerprint != bundle_fingerprint(artifact)) {
        add_error(errors, "sprite.atlas-artifact-bundle-fingerprint-invalid", "/bundleFingerprint",
                  "Bundle fingerprint does not match the artifact metadata.");
    }
    return errors;
}

std::string sprite_atlas_artifact_json(
    const SpriteAtlasArtifact& artifact, const std::size_t max_pages,
    const std::size_t max_bindings) {
    Json pages = Json::array();
    const auto page_count = std::min(artifact.pages.size(), max_pages);
    for (std::size_t index = 0U; index < page_count; ++index)
        pages.push_back(page_timing_free_json(artifact.pages[index]));
    Json bindings = Json::array();
    const auto binding_count = std::min(artifact.bindings.size(), max_bindings);
    for (std::size_t index = 0U; index < binding_count; ++index) {
        const auto& binding = artifact.bindings[index];
        bindings.push_back(Json{
            {"frameId", binding.frame_id},
            {"pageIndex", binding.page_index},
            {"rect", Json::array({binding.x, binding.y, binding.width, binding.height})}
        });
    }
    return Json{
        {"schema", artifact.schema},
        {"valid", artifact.valid},
        {"code", artifact.code},
        {"detail", artifact.detail},
        {"source", {
            {"assetId", artifact.source_asset_id},
            {"hash", artifact.source_hash},
            {"targetProfile", artifact.target_profile}}},
        {"layout", {
            {"pageSize", Json::array({artifact.page_width, artifact.page_height})},
            {"padding", artifact.padding},
            {"fingerprint", artifact.layout_fingerprint}}},
        {"bundleFingerprint", artifact.bundle_fingerprint},
        {"cookSets", {
            {"full", {
                {"pageIndices", artifact.full_page_indices},
                {"bytes", artifact.full_page_bytes}}},
            {"incremental", {
                {"pageIndices", artifact.incremental_page_indices},
                {"bytes", artifact.incremental_page_bytes}}}}},
        {"pages", {
            {"total", artifact.pages.size()},
            {"emitted", page_count},
            {"truncated", page_count != artifact.pages.size()},
            {"items", std::move(pages)}}},
        {"bindings", {
            {"total", artifact.bindings.size()},
            {"emitted", binding_count},
            {"truncated", binding_count != artifact.bindings.size()},
            {"items", std::move(bindings)}}},
        {"diagnostics", diagnostic_json(artifact.diagnostics)}
    }.dump();
}

SpriteAtlasArtifactParseResult parse_sprite_atlas_artifact_json(
    const std::string_view json, const std::size_t max_pages, const std::size_t max_bindings) {
    SpriteAtlasArtifactParseResult result;
    if (json.size() > kMaxManifestBytes) {
        add_error(result.errors, "sprite.atlas-artifact-json-limit", "/",
                  "Atlas artifact manifest exceeds the bounded JSON input size.");
        return result;
    }
    try {
        const auto root = Json::parse(json);
        if (!root.is_object()) {
            add_error(result.errors, "sprite.atlas-artifact-json-invalid", "/",
                      "Atlas artifact manifest must be a JSON object.");
            return result;
        }
        SpriteAtlasArtifact artifact;
        if (!json_has_string(root, "schema", artifact.schema) ||
            artifact.schema != "noemancer.sprite-atlas-artifact/0.1") {
            add_error(result.errors, "sprite.atlas-artifact-schema-invalid", "/schema",
                      "Expected noemancer.sprite-atlas-artifact/0.1.");
            return result;
        }
        if (!json_has_string(root, "code", artifact.code) ||
            !json_has_string(root, "detail", artifact.detail) ||
            !root.contains("valid") || !root.at("valid").is_boolean() ||
            !json_has_string(root.at("source"), "assetId", artifact.source_asset_id) ||
            !json_has_string(root.at("source"), "hash", artifact.source_hash) ||
            !json_has_string(root.at("source"), "targetProfile", artifact.target_profile) ||
            !json_has_string(root, "bundleFingerprint", artifact.bundle_fingerprint)) {
            add_error(result.errors, "sprite.atlas-artifact-json-fields-invalid", "/",
                      "Manifest required fields have the wrong type or are missing.");
            return result;
        }
        artifact.valid = root.at("valid").get<bool>();
        const auto& layout = root.at("layout");
        if (!layout.is_object() || !layout.contains("pageSize") ||
            !parse_size_pair(layout.at("pageSize"), artifact.page_width, artifact.page_height) ||
            !json_has_u32(layout, "padding", artifact.padding) ||
            !json_has_u64(layout, "fingerprint", artifact.layout_fingerprint)) {
            add_error(result.errors, "sprite.atlas-artifact-layout-invalid", "/layout",
                      "Manifest layout fields are invalid.");
            return result;
        }
        const auto& cook_sets = root.at("cookSets");
        if (!cook_sets.is_object() || !cook_sets.contains("full") ||
            !cook_sets.contains("incremental")) {
            add_error(result.errors, "sprite.atlas-artifact-cook-sets-invalid", "/cookSets",
                      "Manifest must contain full and incremental page sets.");
            return result;
        }
        const auto parse_set = [&](const Json& value, std::vector<std::uint32_t>& indices,
                                   std::uint64_t& bytes, const std::string_view path) {
            if (!value.is_object() || !value.contains("pageIndices") || !value.contains("bytes") ||
                !parse_page_indices(value.at("pageIndices"), indices, max_pages) ||
                !json_has_u64(value, "bytes", bytes)) {
                add_error(result.errors, "sprite.atlas-artifact-cook-set-invalid", std::string(path),
                          "Manifest page set fields are invalid or exceed the parser bound.");
                return false;
            }
            return true;
        };
        if (!parse_set(cook_sets.at("full"), artifact.full_page_indices, artifact.full_page_bytes,
                       "/cookSets/full") ||
            !parse_set(cook_sets.at("incremental"), artifact.incremental_page_indices,
                       artifact.incremental_page_bytes, "/cookSets/incremental")) return result;

        const auto& pages = root.at("pages");
        std::size_t page_total{};
        if (!pages.is_object() || !json_has_size(pages, "total", page_total) ||
            page_total > max_pages || !pages.contains("items") ||
            !pages.at("items").is_array() || pages.at("items").size() > max_pages) {
            add_error(result.errors, "sprite.atlas-artifact-pages-invalid", "/pages",
                      "Manifest page list is invalid or exceeds the parser bound.");
            return result;
        }
        artifact.pages.reserve(page_total);
        if (pages.at("items").size() != page_total) {
            add_error(result.errors, "sprite.atlas-artifact-pages-truncated", "/pages",
                      "Parser requires a complete page list; truncated manifests are observation-only.");
            return result;
        }
        for (const auto& item : pages.at("items")) {
            SpriteAtlasPageArtifact page;
            if (!item.is_object() || !json_has_bool(item, "valid", page.valid) ||
                !json_has_bool(item, "cacheHit", page.cache_hit) ||
                !json_has_bool(item, "rebuilt", page.rebuilt) ||
                !json_has_u32(item, "pageIndex", page.page_index) ||
                !json_has_string(item, "assetId", page.asset_id) ||
                !item.contains("size") || !parse_size_pair(item.at("size"), page.width, page.height) ||
                !json_has_size(item, "frameCount", page.frame_count) ||
                !json_has_u64(item, "inputBytes", page.input_bytes) ||
                !json_has_string(item, "contentFingerprint", page.content_fingerprint) ||
                !json_has_string(item, "code", page.code) ||
                !json_has_string(item, "detail", page.detail) ||
                !json_has_string(item, "cacheKey", page.cache_key) || !item.contains("payload")) {
                add_error(result.errors, "sprite.atlas-artifact-page-json-invalid", "/pages/items",
                          "Manifest page item is invalid.");
                return result;
            }
            const auto& payload = item.at("payload");
            if (!payload.is_object() || !json_has_string(payload, "format", page.payload_format) ||
                !json_has_u64(payload, "bytes", page.payload_bytes) ||
                !json_has_string(payload, "fingerprint", page.payload_fingerprint)) {
                add_error(result.errors, "sprite.atlas-artifact-page-payload-invalid", "/pages/items/payload",
                          "Manifest page payload metadata is invalid.");
                return result;
            }
            artifact.pages.push_back(std::move(page));
        }
        const auto& bindings = root.at("bindings");
        std::size_t binding_total{};
        if (!bindings.is_object() || !json_has_size(bindings, "total", binding_total) ||
            binding_total > max_bindings || !bindings.contains("items") ||
            !bindings.at("items").is_array() || bindings.at("items").size() > max_bindings) {
            add_error(result.errors, "sprite.atlas-artifact-bindings-invalid", "/bindings",
                      "Manifest binding list is invalid or exceeds the parser bound.");
            return result;
        }
        artifact.bindings.reserve(binding_total);
        if (bindings.at("items").size() != binding_total) {
            add_error(result.errors, "sprite.atlas-artifact-bindings-truncated", "/bindings",
                      "Parser requires a complete binding list; truncated manifests are observation-only.");
            return result;
        }
        for (const auto& item : bindings.at("items")) {
            SpriteAtlasFrameBinding binding;
            if (!item.is_object() || !json_has_string(item, "frameId", binding.frame_id) ||
                !json_has_u32(item, "pageIndex", binding.page_index) || !item.contains("rect") ||
                !item.at("rect").is_array() || item.at("rect").size() != 4U) {
                add_error(result.errors, "sprite.atlas-artifact-binding-json-invalid", "/bindings/items",
                          "Manifest binding item is invalid.");
                return result;
            }
            const auto& rect = item.at("rect");
            std::uint64_t values[4]{};
            for (std::size_t index = 0U; index < 4U; ++index) {
                if (!rect.at(index).is_number_unsigned()) {
                    add_error(result.errors, "sprite.atlas-artifact-binding-json-invalid", "/bindings/items/rect",
                              "Binding rectangle values must be unsigned integers.");
                    return result;
                }
                values[index] = rect.at(index).get<std::uint64_t>();
                if (values[index] > std::numeric_limits<std::uint32_t>::max()) {
                    add_error(result.errors, "sprite.atlas-artifact-binding-json-invalid", "/bindings/items/rect",
                              "Binding rectangle values exceed 32-bit range.");
                    return result;
                }
            }
            binding.x = static_cast<std::uint32_t>(values[0]);
            binding.y = static_cast<std::uint32_t>(values[1]);
            binding.width = static_cast<std::uint32_t>(values[2]);
            binding.height = static_cast<std::uint32_t>(values[3]);
            artifact.bindings.push_back(std::move(binding));
        }
        if (const auto errors = validate_sprite_atlas_artifact(artifact); !errors.empty()) {
            result.errors = errors;
            return result;
        }
        result.artifact = std::move(artifact);
        return result;
    } catch (const std::exception& exception) {
        add_error(result.errors, "sprite.atlas-artifact-json-invalid", "/", exception.what());
        return result;
    }
}

std::vector<SpriteRuntimePageBinding> sprite_runtime_page_bindings(
    const SpriteAtlasArtifact& artifact) {
    if (!artifact.valid || !validate_sprite_atlas_artifact(artifact).empty()) return {};

    std::vector<SpriteRuntimePageBinding> bindings;
    bindings.reserve(artifact.bindings.size());
    for (const auto& binding : artifact.bindings) {
        if (binding.page_index >= artifact.pages.size()) return {};
        const auto& page = artifact.pages[binding.page_index];
        if (page.payload_fingerprint.empty()) return {};
        bindings.push_back(SpriteRuntimePageBinding{
            .sprite_asset_id = artifact.source_asset_id,
            .frame_id = binding.frame_id,
            .derived_texture_asset_id = page.asset_id,
            .page_index = binding.page_index,
            .page_width = page.width,
            .page_height = page.height,
            .x = binding.x,
            .y = binding.y,
            .width = binding.width,
            .height = binding.height,
            .layout_fingerprint = artifact.layout_fingerprint,
            .page_fingerprint = page.payload_fingerprint
        });
    }
    return bindings;
}

} // namespace noemancer
