#include "engine/asset_thumbnail.hpp"
#include "engine/image_decoder.hpp"

#include <nlohmann/json.hpp>

#include <cstddef>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

namespace {

using noemancer::AssetRecord;
using noemancer::DecodedImage;
using noemancer::ThumbnailExecutionOptions;
using noemancer::ThumbnailSourceReadResult;

AssetRecord png_asset() {
    return AssetRecord{
        .id = "asset.texture.hero",
        .display_name = "Hero",
        .kind = "texture",
        .uri = "project://textures/hero.png",
        .relative_path = "textures/hero.png",
        .extension = ".png",
        .content_hash = "sha256:hero-content",
        .hash_provenance = "source",
        .import_state = "ready",
        .available = true
    };
}

std::vector<std::byte> as_bytes(const std::vector<std::uint8_t>& bytes) {
    std::vector<std::byte> result;
    result.reserve(bytes.size());
    for (const auto byte : bytes) result.push_back(static_cast<std::byte>(byte));
    return result;
}

bool check_png(const std::vector<std::uint8_t>& bytes, const std::uint32_t width,
               const std::uint32_t height) {
    const auto decoded = noemancer::decode_png_rgba8(std::span<const std::byte>(
        reinterpret_cast<const std::byte*>(bytes.data()), bytes.size()));
    return decoded.valid && decoded.width == width && decoded.height == height;
}

} // namespace

int main() {
    const auto recipe = noemancer::ThumbnailRecipe{
        .version = "test-recipe/3",
        .width = 32U,
        .height = 24U,
        .color_space = "srgb"
    };
    const auto source = png_asset();
    const auto plan = noemancer::plan_thumbnail(source, recipe);
    const auto repeat = noemancer::plan_thumbnail(source, recipe);
    if (!plan.valid || plan.strategy != "png-decode-scale" || plan.cache_key != repeat.cache_key ||
        plan.artifact_uri != repeat.artifact_uri || plan.artifact_uri.find("..") != std::string::npos) {
        std::cerr << "PNG thumbnail plan was not deterministic or path-safe\n";
        return 1;
    }
    auto jpeg_source=source;
    jpeg_source.id="asset.texture.hero-jpeg";
    jpeg_source.uri="project://textures/hero.jpg";
    jpeg_source.relative_path="textures/hero.jpg";
    jpeg_source.extension=".jpg";
    jpeg_source.content_hash="sha256:hero-jpeg-content";
    const auto jpeg_plan=noemancer::plan_thumbnail(jpeg_source,recipe);
    if(!jpeg_plan.valid||jpeg_plan.strategy!="jpeg-decode-scale"){
        std::cerr<<"JPEG thumbnail did not select the libjpeg-turbo image adapter\n";
        return 10;
    }

    const std::vector<std::uint8_t> source_rgba{
        255U, 0U, 0U, 255U, 0U, 255U, 0U, 255U,
        0U, 0U, 255U, 255U, 255U, 255U, 0U, 255U
    };
    const auto encoded_source = noemancer::encode_png_rgba8(2U, 2U, source_rgba);
    if (!encoded_source.valid) {
        std::cerr << "The existing PNG encoder could not produce the source fixture\n";
        return 2;
    }
    const auto source_bytes = as_bytes(encoded_source.bytes);
    std::string written_uri;
    std::vector<std::uint8_t> written_bytes;
    ThumbnailExecutionOptions execution;
    execution.read_source = [&](const std::string_view uri) {
        if (uri != source.uri) {
            return ThumbnailSourceReadResult{.code = "thumbnail.fixture-wrong-uri"};
        }
        return ThumbnailSourceReadResult{
            .success = true,
            .code = "thumbnail.fixture-source-ready",
            .detail = "fixture",
            .bytes = source_bytes
        };
    };
    execution.write_artifact = [&](const std::string_view uri, const std::string_view format,
                                   const std::span<const std::uint8_t> bytes,
                                   std::string& code, std::string& detail) {
        if (format != "image/png" || uri != plan.artifact_uri) {
            code = "thumbnail.fixture-writer-contract";
            detail = "writer received an unexpected artifact contract";
            return false;
        }
        written_uri = std::string(uri);
        written_bytes.assign(bytes.begin(), bytes.end());
        return true;
    };
    const auto receipt = noemancer::execute_thumbnail(plan, execution);
    if (!receipt.success || receipt.cache_hit || receipt.payload.empty() ||
        written_uri != plan.artifact_uri || written_bytes != receipt.payload ||
        !check_png(receipt.payload, recipe.width, recipe.height)) {
        std::cerr << "PNG thumbnail execution did not produce a loadable artifact\n";
        return 3;
    }
    const auto receipt_json = nlohmann::json::parse(noemancer::thumbnail_receipt_json(receipt));
    if (receipt_json.at("payloadBytes") != receipt.payload.size() ||
        receipt_json.at("payloadFingerprint").get<std::string>().empty()) {
        std::cerr << "Thumbnail receipt omitted bounded artifact evidence\n";
        return 4;
    }

    AssetRecord sprite = source;
    sprite.id = "asset.sprite.hero";
    sprite.kind = "sprite";
    sprite.relative_path = "characters/hero.sprite.json";
    sprite.extension = ".json";
    const auto sprite_plan = noemancer::plan_thumbnail(sprite, recipe);
    const auto sprite_receipt = noemancer::execute_thumbnail(sprite_plan);
    if (!sprite_plan.valid || sprite_plan.source_kind != "sprite" || !sprite_receipt.success ||
        !check_png(sprite_receipt.payload, recipe.width, recipe.height) ||
        sprite_receipt.diagnostics.empty()) {
        std::cerr << "Sprite placeholder thumbnail was not a usable deterministic PNG\n";
        return 5;
    }

    AssetRecord model = source;
    model.id = "asset.model.robot";
    model.kind = "model";
    model.relative_path = "characters/robot.glb";
    model.extension = ".glb";
    const auto unsafe_artifact = noemancer::plan_thumbnail(
        noemancer::ThumbnailSource{
            .asset = model,
            .artifact_uris = {"cache://cook/../payload.png", "cache://cook/meshbin"}
        }, recipe);
    if (!unsafe_artifact.valid || unsafe_artifact.reusable_artifact ||
        unsafe_artifact.strategy != "model-placeholder" || unsafe_artifact.diagnostics.empty() ||
        unsafe_artifact.artifact_uri.find("cache://thumbnails/") != 0U) {
        std::cerr << "Unsafe or non-preview Cook artifacts were not handled safely\n";
        return 6;
    }

    const auto reusable = noemancer::plan_thumbnail(
        noemancer::ThumbnailSource{
            .asset = source,
            .artifact_uris = {"cache://cook/hero/preview.png"}
        }, recipe);
    const auto cache_receipt = noemancer::execute_thumbnail(reusable);
    if (!reusable.valid || !reusable.reusable_artifact ||
        reusable.strategy != "cooked-preview-passthrough" || !cache_receipt.success ||
        !cache_receipt.cache_hit || !cache_receipt.payload.empty() ||
        cache_receipt.artifact_uri != "cache://cook/hero/preview.png") {
        std::cerr << "Existing preview artifact was not reused as a cache hit\n";
        return 7;
    }

    AssetRecord unsupported = source;
    unsupported.id = "asset.script.logic";
    unsupported.kind = "script";
    unsupported.relative_path = "logic/player.cs";
    unsupported.extension = ".cs";
    const auto unsupported_plan = noemancer::plan_thumbnail(unsupported, recipe);
    if (unsupported_plan.valid || unsupported_plan.code != "thumbnail.unsupported-asset-kind") {
        std::cerr << "Unsupported asset kind was not rejected with a stable code\n";
        return 8;
    }

    AssetRecord missing_hash = source;
    missing_hash.content_hash.clear();
    const auto missing_hash_plan = noemancer::plan_thumbnail(missing_hash, recipe);
    if (missing_hash_plan.valid || missing_hash_plan.code != "thumbnail.content-hash-required") {
        std::cerr << "Missing content hash was not rejected before cache planning\n";
        return 9;
    }

    auto failed_reader = execution;
    failed_reader.read_source = [](const std::string_view) {
        return ThumbnailSourceReadResult{
            .success = false,
            .code = "fixture.source-denied",
            .detail = "source intentionally unavailable"
        };
    };
    const auto failed_receipt = noemancer::execute_thumbnail(plan, failed_reader);
    if (failed_receipt.success || failed_receipt.code != "fixture.source-denied") {
        std::cerr << "Source reader failure did not remain observable and stable\n";
        return 10;
    }

    const auto bounded = noemancer::thumbnail_plan_json(plan, 512U);
    if (bounded.size() > 512U || bounded.find("project://") != std::string::npos) {
        std::cerr << "Thumbnail observation exceeded its budget or leaked a source URI\n";
        return 11;
    }
    return 0;
}
