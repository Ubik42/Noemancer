#include "engine/asset_thumbnail.hpp"

#include "engine/image_decoder.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <iomanip>
#include <limits>
#include <sstream>

namespace noemancer {
namespace {

using Json = nlohmann::json;

constexpr std::size_t kMaxDiagnosticCount = 16U;
constexpr std::size_t kMaxDiagnosticBytes = 512U;
constexpr std::uint32_t kMaxThumbnailDimension = 4096U;
constexpr std::uint64_t kMaxThumbnailPixels = 16ULL * 1024ULL * 1024ULL;

std::string lower_ascii(std::string value) {
    std::ranges::transform(value, value.begin(), [](const unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return value;
}

bool ends_with(const std::string_view value, const std::string_view suffix) {
    return value.size() >= suffix.size() &&
        value.substr(value.size() - suffix.size()) == suffix;
}

bool contains(const std::string_view value, const std::string_view token) {
    return value.find(token) != std::string_view::npos;
}

std::string bounded_text(const std::string_view value) {
    if (value.size() <= kMaxDiagnosticBytes) return std::string(value);
    return std::string(value.substr(0U, kMaxDiagnosticBytes));
}

std::string hex_u64(const std::uint64_t value) {
    std::ostringstream output;
    output << std::hex << std::setfill('0') << std::setw(16) << value;
    return output.str();
}

std::uint64_t fnv1a(const std::string_view value) {
    std::uint64_t hash = 1469598103934665603ULL;
    for (const unsigned char character : value) {
        hash ^= character;
        hash *= 1099511628211ULL;
    }
    return hash;
}

std::uint64_t fnv1a_bytes(const std::span<const std::uint8_t> bytes) {
    std::uint64_t hash = 1469598103934665603ULL;
    for (const auto byte : bytes) {
        hash ^= byte;
        hash *= 1099511628211ULL;
    }
    return hash;
}

std::string safe_token(const std::string_view value) {
    std::string token;
    token.reserve(std::min<std::size_t>(value.size(), 80U));
    for (const unsigned char character : value) {
        if ((character >= 'a' && character <= 'z') ||
            (character >= 'A' && character <= 'Z') ||
            (character >= '0' && character <= '9') || character == '_' ||
            character == '-' || character == '.') {
            token.push_back(static_cast<char>(character));
        } else {
            token.push_back('_');
        }
        if (token.size() == 80U) break;
    }
    if (token.empty() || token == "." || token == "..") return "asset";
    if (token.front() == '.') token.insert(token.begin(), '_');
    return token;
}

bool safe_artifact_uri(const std::string_view uri) {
    const auto scheme_end = uri.find("://");
    if (scheme_end == std::string_view::npos || scheme_end == 0U) return false;
    for (std::size_t index = 0U; index < scheme_end; ++index) {
        const auto character = static_cast<unsigned char>(uri[index]);
        if (!((character >= 'a' && character <= 'z') ||
              (character >= 'A' && character <= 'Z') ||
              (character >= '0' && character <= '9') || character == '+' ||
              character == '-' || character == '.')) return false;
    }
    if (uri.find('\\') != std::string_view::npos ||
        uri.find("..") != std::string_view::npos) return false;
    for (const unsigned char character : uri) {
        if (character < 0x20U || std::isspace(character) != 0U) return false;
    }
    const auto path = uri.substr(scheme_end + 3U);
    if (path.empty()) return false;
    std::size_t cursor = 0U;
    while (cursor <= path.size()) {
        const auto separator = path.find('/', cursor);
        const auto end = separator == std::string_view::npos ? path.size() : separator;
        const auto segment = path.substr(cursor, end - cursor);
        if (segment == "." || segment == "..") return false;
        if (separator == std::string_view::npos) break;
        cursor = separator + 1U;
    }
    return true;
}

bool is_image_preview_uri(const std::string_view uri) {
    const auto lower = lower_ascii(std::string(uri));
    return ends_with(lower, ".png") || ends_with(lower, ".ppm") ||
        ends_with(lower, ".jpg") || ends_with(lower, ".jpeg") ||
        ends_with(lower, ".webp") || contains(lower, "/preview/") ||
        contains(lower, "/preview.");
}

std::string preview_format(const std::string_view uri) {
    const auto lower = lower_ascii(std::string(uri));
    if (ends_with(lower, ".ppm")) return "image/x-portable-pixmap";
    if (ends_with(lower, ".jpg") || ends_with(lower, ".jpeg")) return "image/jpeg";
    if (ends_with(lower, ".webp")) return "image/webp";
    return "image/png";
}

struct Classification final {
    std::string kind;
    std::string strategy;
};

Classification classify_asset(const AssetRecord& asset) {
    const auto path = lower_ascii(asset.relative_path + " " + asset.extension);
    const auto kind = lower_ascii(asset.kind);
    if (ends_with(path, ".sprite.json") || contains(kind, "sprite")) {
        return {"sprite", "sprite-placeholder"};
    }
    if (ends_with(path, ".tilemap.json") || contains(kind, "tilemap")) {
        return {"tilemap", "tilemap-placeholder"};
    }
    if (ends_with(path, ".scene.json") || contains(kind, "scene")) {
        return {"scene", "scene-placeholder"};
    }
    if (ends_with(path, ".png")) return {"texture", "png-decode-scale"};
    if (ends_with(path, ".jpg") || ends_with(path, ".jpeg") ||
        ends_with(path, ".webp") || ends_with(path, ".tga") ||
        ends_with(path, ".exr") || ends_with(path, ".hdr") ||
        ends_with(path, ".ktx2") || ends_with(path, ".basis") ||
        contains(kind, "texture") || contains(kind, "image")) {
        return {"texture", "texture-placeholder"};
    }
    if (ends_with(path, ".glb") || ends_with(path, ".gltf") ||
        ends_with(path, ".fbx") || contains(kind, "model")) {
        return {"model", "model-placeholder"};
    }
    if (ends_with(path, ".meshbin") || contains(kind, "geometry") ||
        contains(kind, "mesh")) {
        return {"geometry", "geometry-placeholder"};
    }
    return {};
}

void add_diagnostic(std::vector<std::string>& diagnostics, const std::string_view value) {
    if (diagnostics.size() >= kMaxDiagnosticCount) return;
    diagnostics.emplace_back(bounded_text(value));
}

std::string recipe_fingerprint(const ThumbnailRecipe& recipe) {
    std::ostringstream material;
    material << recipe.version << '|' << recipe.width << '|' << recipe.height << '|'
             << recipe.color_space;
    return "fnv1a64:" + hex_u64(fnv1a(material.str()));
}

std::string failure_code(const std::string_view code, const std::string_view fallback) {
    return code.empty() ? std::string(fallback) : std::string(code);
}

Json bounded_diagnostics(const std::vector<std::string>& diagnostics) {
    Json result = Json::array();
    for (std::size_t index = 0U; index < diagnostics.size() && index < kMaxDiagnosticCount; ++index) {
        result.push_back(bounded_text(diagnostics[index]));
    }
    return result;
}

std::string bounded_json(const Json& value, const std::size_t max_bytes) {
    const auto budget = max_bytes == 0U ? 16U * 1024U : max_bytes;
    const auto encoded = value.dump();
    if (encoded.size() <= budget) return encoded;
    const Json minimal = {
        {"valid", false},
        {"code", "thumbnail.observation.byte-budget"},
        {"minimumRequiredBytes", encoded.size()}
    };
    const auto compact = minimal.dump();
    if (compact.size() <= budget) return compact;
    return R"({"valid":false,"code":"thumbnail.observation.byte-budget"})";
}

void set_pixel(std::vector<std::uint8_t>& pixels, const std::uint32_t width,
               const std::uint32_t x, const std::uint32_t y,
               const std::uint8_t red, const std::uint8_t green,
               const std::uint8_t blue, const std::uint8_t alpha = 255U) {
    const auto offset = (static_cast<std::size_t>(y) * width + x) * 4U;
    pixels[offset] = red;
    pixels[offset + 1U] = green;
    pixels[offset + 2U] = blue;
    pixels[offset + 3U] = alpha;
}

std::vector<std::uint8_t> make_placeholder(const ThumbnailPlan& plan) {
    const auto pixel_count = static_cast<std::size_t>(plan.width) * plan.height;
    std::vector<std::uint8_t> pixels(pixel_count * 4U, 255U);
    const auto seed = fnv1a(plan.cache_key);
    const auto base_red = static_cast<std::uint8_t>(48U + ((seed >> 0U) & 63U));
    const auto base_green = static_cast<std::uint8_t>(64U + ((seed >> 8U) & 63U));
    const auto base_blue = static_cast<std::uint8_t>(92U + ((seed >> 16U) & 63U));
    for (std::uint32_t y = 0U; y < plan.height; ++y) {
        for (std::uint32_t x = 0U; x < plan.width; ++x) {
            const auto checker = ((x / 16U) + (y / 16U)) % 2U;
            const auto shade = checker == 0U ? 0U : 18U;
            set_pixel(pixels, plan.width, x, y,
                static_cast<std::uint8_t>(std::min(255U, static_cast<unsigned>(base_red) + shade)),
                static_cast<std::uint8_t>(std::min(255U, static_cast<unsigned>(base_green) + shade)),
                static_cast<std::uint8_t>(std::min(255U, static_cast<unsigned>(base_blue) + shade)));
        }
    }

    if (plan.source_kind == "tilemap") {
        for (std::uint32_t y = 0U; y < plan.height; y += 16U) {
            for (std::uint32_t x = 0U; x < plan.width; x += 16U) {
                const auto tile = static_cast<std::uint8_t>(40U + ((seed + x * 13U + y * 7U) & 31U));
                for (std::uint32_t tile_y = y; tile_y < std::min(plan.height, y + 15U); ++tile_y) {
                    for (std::uint32_t tile_x = x; tile_x < std::min(plan.width, x + 15U); ++tile_x) {
                        set_pixel(pixels, plan.width, tile_x, tile_y, 48U, tile, 46U);
                    }
                }
            }
        }
    } else if (plan.source_kind == "sprite") {
        const auto left = plan.width / 3U;
        const auto right = plan.width - left;
        const auto top = plan.height / 5U;
        const auto bottom = plan.height - top;
        for (std::uint32_t y = top; y < bottom; ++y) {
            for (std::uint32_t x = left; x < right; ++x) {
                const auto edge = x == left || x + 1U == right || y == top || y + 1U == bottom;
                set_pixel(pixels, plan.width, x, y, edge ? 255U : 244U, edge ? 196U : 92U,
                    edge ? 74U : 68U);
            }
        }
    } else {
        const auto left = plan.width / 5U;
        const auto right = plan.width - left;
        const auto horizon = plan.height * 2U / 3U;
        for (std::uint32_t y = horizon; y < plan.height; ++y) {
            for (std::uint32_t x = 0U; x < plan.width; ++x) {
                set_pixel(pixels, plan.width, x, y, 34U, 38U, 44U);
            }
        }
        for (std::uint32_t y = plan.height / 5U; y < horizon; ++y) {
            for (std::uint32_t x = left; x < right; ++x) {
                const auto edge = x == left || x + 1U == right || y == plan.height / 5U;
                set_pixel(pixels, plan.width, x, y, edge ? 228U : 111U, edge ? 232U : 148U,
                    edge ? 242U : 188U);
            }
        }
    }
    return pixels;
}

std::vector<std::uint8_t> resize_nearest(const DecodedImage& source,
                                         const std::uint32_t width,
                                         const std::uint32_t height) {
    const auto pixel_count = static_cast<std::size_t>(width) * height;
    std::vector<std::uint8_t> result(pixel_count * 4U, 0U);
    for (std::uint32_t y = 0U; y < height; ++y) {
        const auto source_y = std::min(source.height - 1U,
            static_cast<std::uint32_t>((static_cast<std::uint64_t>(y) * source.height) / height));
        for (std::uint32_t x = 0U; x < width; ++x) {
            const auto source_x = std::min(source.width - 1U,
                static_cast<std::uint32_t>((static_cast<std::uint64_t>(x) * source.width) / width));
            const auto source_offset = (static_cast<std::size_t>(source_y) * source.width + source_x) * 4U;
            const auto destination_offset = (static_cast<std::size_t>(y) * width + x) * 4U;
            std::copy_n(source.rgba8.data() + source_offset, 4U, result.data() + destination_offset);
        }
    }
    return result;
}

ThumbnailReceipt base_receipt(const ThumbnailPlan& plan) {
    ThumbnailReceipt receipt;
    receipt.asset_id = plan.asset_id;
    receipt.strategy = plan.strategy;
    receipt.cache_key = plan.cache_key;
    receipt.artifact_uri = plan.artifact_uri;
    receipt.payload_format = plan.payload_format;
    receipt.width = plan.width;
    receipt.height = plan.height;
    receipt.diagnostics = plan.diagnostics;
    return receipt;
}

} // namespace

ThumbnailPlan plan_thumbnail(const ThumbnailSource& source, const ThumbnailRecipe& recipe) {
    ThumbnailPlan plan;
    plan.asset_id = source.asset.id;
    plan.source_uri = source.asset.uri;
    plan.source_hash = source.asset.content_hash;
    plan.recipe_version = recipe.version;
    plan.recipe_fingerprint = recipe_fingerprint(recipe);
    plan.width = recipe.width;
    plan.height = recipe.height;

    if (source.asset.id.empty()) {
        plan.code = "thumbnail.asset-id-required";
        plan.detail = "Thumbnail planning requires a stable asset ID.";
        return plan;
    }
    if (source.asset.content_hash.empty()) {
        plan.code = "thumbnail.content-hash-required";
        plan.detail = "Thumbnail cache identity requires the source content hash.";
        return plan;
    }
    if (recipe.version.empty()) {
        plan.code = "thumbnail.recipe-version-required";
        plan.detail = "Thumbnail cache identity requires a recipe version.";
        return plan;
    }
    if (recipe.width == 0U || recipe.height == 0U || recipe.width > kMaxThumbnailDimension ||
        recipe.height > kMaxThumbnailDimension ||
        static_cast<std::uint64_t>(recipe.width) * recipe.height > kMaxThumbnailPixels) {
        plan.code = "thumbnail.recipe-dimensions-invalid";
        plan.detail = "Thumbnail dimensions exceed the bounded preview budget.";
        return plan;
    }

    const auto classification = classify_asset(source.asset);
    plan.source_kind = classification.kind;
    plan.strategy = classification.strategy;
    std::ostringstream material;
    material << "thumbnail|" << source.asset.content_hash << '|' << recipe.version << '|'
             << recipe.width << 'x' << recipe.height << '|' << recipe.color_space << '|'
             << classification.kind;
    plan.cache_key = "thumb-" + hex_u64(fnv1a(material.str()));
    plan.artifact_uri = "cache://thumbnails/" + safe_token(plan.cache_key) + ".png";

    for (const auto& candidate : source.artifact_uris) {
        if (!safe_artifact_uri(candidate)) {
            add_diagnostic(plan.diagnostics, "thumbnail.artifact-uri-unsafe");
            continue;
        }
        if (is_image_preview_uri(candidate)) {
            plan.reusable_artifact = true;
            plan.reused_artifact_uri = candidate;
            plan.artifact_uri = candidate;
            plan.payload_format = preview_format(candidate);
            plan.strategy = "cooked-preview-passthrough";
            break;
        }
    }

    if (plan.source_kind.empty() && !plan.reusable_artifact) {
        plan.code = "thumbnail.unsupported-asset-kind";
        plan.detail = "No supported thumbnail preview strategy exists for this asset.";
        return plan;
    }
    if (!source.artifact_uris.empty() && !plan.reusable_artifact) {
        add_diagnostic(plan.diagnostics, "thumbnail.cooked-artifact-not-previewable");
    }
    if (plan.strategy == "texture-placeholder" || plan.strategy == "sprite-placeholder" ||
        plan.strategy == "tilemap-placeholder" || plan.strategy == "scene-placeholder" ||
        plan.strategy == "model-placeholder" || plan.strategy == "geometry-placeholder") {
        add_diagnostic(plan.diagnostics, "thumbnail.source-preview-proxy");
    }
    plan.valid = true;
    plan.code = "thumbnail.plan-ready";
    plan.detail = plan.reusable_artifact
        ? "An existing image artifact will be reused as the thumbnail."
        : "The thumbnail plan is deterministic and ready for execution.";
    return plan;
}

ThumbnailPlan plan_thumbnail(const AssetRecord& asset, const ThumbnailRecipe& recipe) {
    return plan_thumbnail(ThumbnailSource{.asset = asset}, recipe);
}

ThumbnailReceipt execute_thumbnail(const ThumbnailPlan& plan, const ThumbnailExecutionOptions& options) {
    auto receipt = base_receipt(plan);
    if (!plan.valid) {
        receipt.code = failure_code(plan.code, "thumbnail.plan-invalid");
        receipt.detail = plan.detail.empty() ? "Thumbnail plan is invalid." : plan.detail;
        return receipt;
    }
    if (plan.reusable_artifact) {
        if (!safe_artifact_uri(plan.artifact_uri)) {
            receipt.code = "thumbnail.artifact-uri-unsafe";
            receipt.detail = "The reusable thumbnail artifact URI failed path safety validation.";
            return receipt;
        }
        receipt.success = true;
        receipt.cache_hit = true;
        receipt.code = "thumbnail.cache-hit";
        receipt.detail = "An existing preview artifact was reused without decoding or rewriting it.";
        return receipt;
    }

    std::vector<std::uint8_t> rgba8;
    if (plan.strategy == "png-decode-scale") {
        if (!options.read_source) {
            receipt.code = "thumbnail.source-reader-required";
            receipt.detail = "A PNG source thumbnail requires the existing source reader and image decoder.";
            return receipt;
        }
        const auto source = options.read_source(plan.source_uri);
        if (!source.success) {
            receipt.code = failure_code(source.code, "thumbnail.source-read-failed");
            receipt.detail = source.detail.empty() ? "The thumbnail source could not be read." : source.detail;
            return receipt;
        }
        if (source.bytes.empty() || source.bytes.size() > options.max_source_bytes) {
            receipt.code = "thumbnail.source-too-large";
            receipt.detail = "The thumbnail source is empty or exceeds the bounded source budget.";
            return receipt;
        }
        const auto decoded = decode_png_rgba8(std::span<const std::byte>(source.bytes.data(), source.bytes.size()));
        if (!decoded.valid || decoded.width == 0U || decoded.height == 0U) {
            receipt.code = "thumbnail.png-decode-failed";
            receipt.detail = "The existing PNG decoder rejected the source thumbnail.";
            add_diagnostic(receipt.diagnostics, decoded.code);
            return receipt;
        }
        rgba8 = resize_nearest(decoded, plan.width, plan.height);
        add_diagnostic(receipt.diagnostics, "thumbnail.source-decoded-with-image-adapter");
    } else {
        rgba8 = make_placeholder(plan);
    }

    const auto encoded = encode_png_rgba8(plan.width, plan.height, rgba8);
    if (!encoded.valid || encoded.bytes.empty()) {
        receipt.code = "thumbnail.png-encode-failed";
        receipt.detail = "The existing image encoder could not produce a PNG artifact.";
        add_diagnostic(receipt.diagnostics, encoded.code);
        return receipt;
    }
    if (options.write_artifact) {
        std::string writer_code;
        std::string writer_detail;
        if (!options.write_artifact(plan.artifact_uri, "image/png", encoded.bytes,
                                    writer_code, writer_detail)) {
            receipt.code = failure_code(writer_code, "thumbnail.artifact-write-failed");
            receipt.detail = writer_detail.empty() ? "The thumbnail artifact writer rejected the output." : writer_detail;
            return receipt;
        }
    }
    receipt.success = true;
    receipt.code = "thumbnail.generated";
    receipt.detail = plan.strategy == "png-decode-scale"
        ? "PNG thumbnail generated through the existing image adapter."
        : "Deterministic PNG proxy thumbnail generated; source-specific decoding remains deferred.";
    receipt.payload = encoded.bytes;
    receipt.payload_bytes = receipt.payload.size();
    receipt.payload_fingerprint = "fnv1a64:" + hex_u64(fnv1a_bytes(receipt.payload));
    return receipt;
}

std::string thumbnail_plan_json(const ThumbnailPlan& plan, const std::size_t max_bytes) {
    const Json value = {
        {"valid", plan.valid},
        {"code", plan.code},
        {"detail", plan.detail},
        {"schema", plan.schema},
        {"assetId", plan.asset_id},
        {"sourceKind", plan.source_kind},
        {"strategy", plan.strategy},
        {"sourceHash", plan.source_hash},
        {"recipeVersion", plan.recipe_version},
        {"recipeFingerprint", plan.recipe_fingerprint},
        {"cacheKey", plan.cache_key},
        {"artifactUri", plan.artifact_uri},
        {"payloadFormat", plan.payload_format},
        {"width", plan.width},
        {"height", plan.height},
        {"reusableArtifact", plan.reusable_artifact},
        {"reusedArtifactUri", plan.reused_artifact_uri},
        {"diagnostics", bounded_diagnostics(plan.diagnostics)}
    };
    return bounded_json(value, max_bytes);
}

std::string thumbnail_receipt_json(const ThumbnailReceipt& receipt, const std::size_t max_bytes) {
    const Json value = {
        {"success", receipt.success},
        {"cacheHit", receipt.cache_hit},
        {"code", receipt.code},
        {"detail", receipt.detail},
        {"schema", receipt.schema},
        {"assetId", receipt.asset_id},
        {"strategy", receipt.strategy},
        {"cacheKey", receipt.cache_key},
        {"artifactUri", receipt.artifact_uri},
        {"payloadFormat", receipt.payload_format},
        {"payloadFingerprint", receipt.payload_fingerprint},
        {"width", receipt.width},
        {"height", receipt.height},
        {"payloadBytes", receipt.payload_bytes},
        {"diagnostics", bounded_diagnostics(receipt.diagnostics)}
    };
    return bounded_json(value, max_bytes);
}

} // namespace noemancer
