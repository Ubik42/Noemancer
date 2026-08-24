#include "engine/sprite_atlas_artifact.hpp"
#include "engine/ktx2_cook_adapter.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <iostream>
#include <ranges>
#include <span>
#include <string>
#include <thread>
#include <vector>

namespace {

void set_pixel(std::vector<std::byte>& pixels, const std::uint32_t width,
               const std::uint32_t x, const std::uint32_t y,
               const std::array<std::uint8_t, 4> value) {
    const auto offset = (static_cast<std::size_t>(y) * width + x) * 4U;
    for (std::size_t channel = 0U; channel < value.size(); ++channel)
        pixels[offset + channel] = static_cast<std::byte>(value[channel]);
}

bool has_error(const noemancer::SpriteAtlasArtifact& artifact, const std::string& code) {
    for (const auto& diagnostic : artifact.diagnostics)
        if (diagnostic.code == code) return true;
    return false;
}

int fail(const char* message, const int code) {
    std::cerr << message << '\n';
    return code;
}

std::filesystem::path unique_cache_root() {
    const auto ticks = static_cast<std::uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch().count());
    const auto thread = std::hash<std::thread::id>{}(std::this_thread::get_id());
    return std::filesystem::temp_directory_path() /
        ("noemancer-sprite-atlas-artifact-tests-" + std::to_string(ticks) + "-" +
         std::to_string(thread));
}

} // namespace

int main() {
    using namespace noemancer;
    if (!ktx2_available()) {
        std::cout << "sprite_atlas_artifact_tests: KTX unavailable; cook assertions skipped\n";
        return 0;
    }

    SpriteAssetDocument document;
    document.asset_id = "sprite.hero";
    document.texture_asset = "texture.hero.source";
    document.texture_width = 8U;
    document.texture_height = 4U;
    document.frames = {
        SpriteFrame{
            .id = "frame.a", .x = 0U, .y = 0U, .width = 2U, .height = 2U,
            .source_width = 2U, .source_height = 2U
        },
        SpriteFrame{
            .id = "frame.b", .x = 4U, .y = 0U, .width = 2U, .height = 2U,
            .source_width = 2U, .source_height = 2U
        }
    };
    std::vector<std::byte> source_pixels(
        static_cast<std::size_t>(document.texture_width) * document.texture_height * 4U,
        std::byte{0});
    for (std::uint32_t y = 0U; y < 2U; ++y) {
        for (std::uint32_t x = 0U; x < 2U; ++x) {
            set_pixel(source_pixels, document.texture_width, x, y, {0xE0U, 0x20U, 0x30U, 0xFFU});
            set_pixel(source_pixels, document.texture_width, x + 4U, y,
                      {0x20U, 0xC0U, 0x50U, 0xFFU});
        }
    }

    SpriteAtlasPlanningOptions planning;
    planning.page_width = 4U;
    planning.page_height = 4U;
    planning.padding = 1U;
    const auto profile = cook_platform_profile("windows-x64-debug");
    TextureCookSettings texture_settings;
    texture_settings.semantic = TextureSemantic::ui;
    texture_settings.alpha_mode = TextureAlphaMode::blend;
    texture_settings.srgb = true;
    texture_settings.generate_mipmaps = false;
    const CookSource source{
        .asset_id = document.asset_id,
        .source_uri = "asset://sprites/hero.source.rgba8",
        .source_hash = "sha256:sprite-hero-source-v1",
        .source_bytes = source_pixels.size(),
        .importer = "image/rgba8"
    };

    const auto artifact = execute_sprite_atlas_artifact(
        document, std::span<const std::byte>(source_pixels.data(), source_pixels.size()),
        planning, source, profile, texture_settings);
    if (!artifact.valid || artifact.code != "ok" || artifact.pages.size() != 2U ||
        artifact.bindings.size() != 2U || artifact.full_page_indices != std::vector<std::uint32_t>{0U, 1U} ||
        !artifact.incremental_page_indices.empty() || artifact.bundle_fingerprint.empty()) {
        return fail("sprite atlas artifact did not produce deterministic full page output", 1);
    }
    if (!validate_sprite_atlas_artifact(artifact).empty())
        return fail("fresh sprite atlas artifact failed structural validation", 2);
    for (const auto& page : artifact.pages) {
        if (!page.valid || page.asset_id.empty() || page.content_fingerprint.empty() ||
            page.payload_fingerprint.empty() || !page.payload_fingerprint.starts_with("sha256:") ||
            page.payload.empty() ||
            page.payload_bytes != page.payload.size() || page.input_bytes != 4U * 4U * 4U) {
            return fail("sprite atlas page omitted independent KTX2 identity or payload", 3);
        }
        const auto decoded = decode_ktx2_rgba8(page.payload);
        if (!decoded.valid || decoded.width != 4U || decoded.height != 4U || decoded.rgba8.size() != 64U ||
            std::to_integer<std::uint8_t>(decoded.rgba8[0U * 4U + 3U]) != 0U) {
            return fail("sprite atlas page did not preserve transparent page background", 4);
        }
    }

    const auto manifest = sprite_atlas_artifact_json(artifact);
    const auto manifest_json = nlohmann::json::parse(manifest);
    if (manifest_json.at("schema") != "noemancer.sprite-atlas-artifact/0.1" ||
        manifest_json.at("pages").at("total") != 2U ||
        manifest_json.at("pages").at("emitted") != 2U ||
        manifest_json.at("bindings").at("total") != 2U ||
        manifest_json.at("cookSets").at("full").at("bytes") != artifact.full_page_bytes ||
        manifest_json.at("bundleFingerprint") != artifact.bundle_fingerprint) {
        return fail("sprite atlas manifest omitted stable bounded page metadata", 5);
    }
    const auto parsed = parse_sprite_atlas_artifact_json(manifest);
    if (!parsed || !parsed.artifact || parsed.artifact->bundle_fingerprint != artifact.bundle_fingerprint ||
        parsed.artifact->pages.size() != artifact.pages.size() ||
        parsed.artifact->bindings.size() != artifact.bindings.size() ||
        !validate_sprite_atlas_artifact(*parsed.artifact).empty()) {
        // The parsed manifest intentionally has no payload bytes, but its
        // metadata and bundle fingerprint must still validate.
        return fail("sprite atlas manifest did not round-trip through bounded parser", 6);
    }
    const auto runtime_bindings = sprite_runtime_page_bindings(artifact);
    if (runtime_bindings.size() != artifact.bindings.size() || runtime_bindings.empty() ||
        runtime_bindings.front().sprite_asset_id != artifact.source_asset_id ||
        runtime_bindings.front().derived_texture_asset_id != artifact.pages.front().asset_id ||
        runtime_bindings.front().page_fingerprint != artifact.pages.front().payload_fingerprint ||
        runtime_bindings.front().layout_fingerprint != artifact.layout_fingerprint) {
        return fail("sprite atlas runtime binding projection lost artifact identity", 7);
    }

    const auto cache_root = unique_cache_root();
    SpriteAtlasArtifactExecutionOptions cached_execution;
    cached_execution.page_cache_root = cache_root;
    cached_execution.cache_limits.max_entries = 8U;
    cached_execution.cache_limits.max_entry_bytes = 64U * 1024U;
    cached_execution.cache_limits.max_total_bytes = 256U * 1024U;
    cached_execution.worker_identity = "workers=1";
    const auto cached_first = execute_sprite_atlas_artifact(
        document, std::span<const std::byte>(source_pixels.data(), source_pixels.size()),
        planning, source, profile, texture_settings, {}, cached_execution);
    if (!cached_first.valid || cached_first.bundle_fingerprint.empty())
        return fail("first disk-backed sprite atlas Cook failed", 11);
    for (const auto& page : cached_first.pages) {
        if (page.cache_hit || page.rebuilt || page.cache_key.empty() ||
            !page.payload_fingerprint.starts_with("sha256:")) {
            return fail("first disk-backed sprite atlas Cook did not report a cache miss", 12);
        }
    }
    const auto cached_second = execute_sprite_atlas_artifact(
        document, std::span<const std::byte>(source_pixels.data(), source_pixels.size()),
        planning, source, profile, texture_settings, {}, cached_execution);
    if (!cached_second.valid || cached_second.bundle_fingerprint != cached_first.bundle_fingerprint ||
        cached_second.pages.size() != cached_first.pages.size())
        return fail("second disk-backed sprite atlas Cook changed the bundle identity", 13);
    for (std::size_t index = 0U; index < cached_second.pages.size(); ++index) {
        const auto& first_page = cached_first.pages[index];
        const auto& second_page = cached_second.pages[index];
        if (!second_page.cache_hit || second_page.rebuilt || second_page.cache_key != first_page.cache_key ||
            second_page.payload != first_page.payload ||
            second_page.payload_fingerprint != first_page.payload_fingerprint) {
            return fail("page cache hit did not short-circuit producer with stable identity", 14);
        }
    }
    auto source_alias = source;
    source_alias.source_hash = "sha256:sprite-hero-source-v2-alias";
    const auto alias_hit = execute_sprite_atlas_artifact(
        document, std::span<const std::byte>(source_pixels.data(), source_pixels.size()),
        planning, source_alias, profile, texture_settings, {}, cached_execution);
    if (!alias_hit.valid || alias_hit.bundle_fingerprint != cached_first.bundle_fingerprint ||
        std::ranges::any_of(alias_hit.pages, [](const auto& page) { return !page.cache_hit; })) {
        return fail("page/bundle identity changed when only the source alias changed", 15);
    }
    const auto cached_manifest = nlohmann::json::parse(sprite_atlas_artifact_json(cached_second));
    if (!cached_manifest.at("pages").at("items").at(0U).at("cacheHit").get<bool>() ||
        cached_manifest.at("pages").at("items").at(0U).at("cacheKey").get<std::string>().empty()) {
        return fail("cached atlas manifest omitted cache receipt state", 16);
    }
    std::error_code cleanup_error;
    std::filesystem::remove_all(cache_root, cleanup_error);

    auto changed_pixels = source_pixels;
    set_pixel(changed_pixels, document.texture_width, 0U, 0U, {0x80U, 0x80U, 0xFFU, 0xFFU});
    const auto changed = execute_sprite_atlas_artifact(
        document, std::span<const std::byte>(changed_pixels.data(), changed_pixels.size()),
        planning, source, profile, texture_settings, {"frame.a"});
    if (!changed.valid || changed.incremental_page_indices != std::vector<std::uint32_t>{0U} ||
        changed.pages.size() != artifact.pages.size() ||
        changed.pages[0].content_fingerprint == artifact.pages[0].content_fingerprint ||
        changed.pages[0].payload_fingerprint == artifact.pages[0].payload_fingerprint ||
        changed.pages[1].content_fingerprint != artifact.pages[1].content_fingerprint ||
        changed.pages[1].payload_fingerprint != artifact.pages[1].payload_fingerprint) {
        return fail("changing one frame invalidated an unaffected atlas page", 17);
    }

    auto wrong_size = source_pixels;
    wrong_size.pop_back();
    const auto wrong_size_artifact = execute_sprite_atlas_artifact(
        document, std::span<const std::byte>(wrong_size.data(), wrong_size.size()),
        planning, source, profile, texture_settings);
    if (wrong_size_artifact.valid || !has_error(wrong_size_artifact, "sprite.atlas-source-size-invalid"))
        return fail("atlas source byte bounds were not enforced", 18);

    auto oversized_page = planning;
    oversized_page.page_width = 1U;
    oversized_page.page_height = 1U;
    oversized_page.padding = 0U;
    const auto oversized = execute_sprite_atlas_artifact(
        document, std::span<const std::byte>(source_pixels.data(), source_pixels.size()),
        oversized_page, source, profile, texture_settings);
    if (oversized.valid || !has_error(oversized, "sprite.atlas-frame-too-large"))
        return fail("atlas frame/page bounds were not enforced", 19);

    auto budget = planning;
    budget.limits.max_estimated_cook_bytes = 1U;
    const auto over_budget = execute_sprite_atlas_artifact(
        document, std::span<const std::byte>(source_pixels.data(), source_pixels.size()),
        budget, source, profile, texture_settings);
    if (over_budget.valid || over_budget.diagnostics.empty())
        return fail("atlas page budget was not enforced", 20);

    std::cout << "sprite_atlas_artifact_tests: ok\n";
    return 0;
}
