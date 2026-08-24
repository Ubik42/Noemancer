#include "engine/sprite_asset.hpp"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>

namespace {

using noemancer::SpriteAssetDocument;
using noemancer::SpriteAssetError;

SpriteAssetDocument make_pressure_document() {
    SpriteAssetDocument document;
    document.schema = "noemancer.sprite-asset/0.2";
    document.asset_id = "sprite.pressure.hero";
    document.texture_asset = "texture.pressure.hero.atlas";
    document.texture_width = 512U;
    document.texture_height = 512U;
    document.pixels_per_unit = 16.0F;
    document.sampling = "nearest";
    document.alpha_mode = "cutout";
    document.provenance = {
        .source_uri = "generated/pressure/hero.atlas.png",
        .source_sha256 = "pressure-fixture-sha256",
        .generator = "sprite-asset-production-pressure-test",
        .license = "CC0-1.0"};

    // 32x32 sparse atlas slots: 1,024 frames with deterministic gaps.  The
    // source schema still contains one textureAsset; page/fragmentation is
    // measured from these existing frame rectangles.
    document.frames.reserve(1024U);
    for (std::uint32_t index = 0U; index < 1024U; ++index) {
        const auto column = index % 32U;
        const auto row = index / 32U;
        document.frames.push_back({
            .id = "hero.frame." + std::to_string(index),
            .x = column * 16U,
            .y = row * 16U,
            .width = 8U,
            .height = 8U,
            .trim_x = 0U,
            .trim_y = 0U,
            .source_width = 8U,
            .source_height = 8U,
            .pivot_x = 0.5F,
            .pivot_y = 0.9F,
            .collision_profile = "hero.body"});
    }

    // Eight clips overlap their windows by half a window. This exercises
    // multi-clip reference reuse while still covering every authored frame.
    document.clips.reserve(8U);
    for (std::uint32_t clip_index = 0U; clip_index < 8U; ++clip_index) {
        noemancer::SpriteClip clip{
            .id = "hero.clip." + std::to_string(clip_index),
            .looping = clip_index != 7U};
        clip.frames.reserve(256U);
        const auto start = (clip_index * 128U) % 1024U;
        for (std::uint32_t frame_index = 0U; frame_index < 256U; ++frame_index) {
            clip.frames.push_back({
                .frame_id = "hero.frame." + std::to_string((start + frame_index) % 1024U),
                .duration_ms = 80U + (frame_index % 5U),
                .event = frame_index % 32U == 0U ? "step" : ""});
        }
        document.clips.push_back(std::move(clip));
    }
    return document;
}

bool has_error(const std::vector<SpriteAssetError>& errors, const std::string_view code) {
    for (const auto& error : errors)
        if (error.code == code) return true;
    return false;
}

bool same_core_report(const noemancer::SpriteAssetProductionReport& left,
                      const noemancer::SpriteAssetProductionReport& right) {
    return left.valid == right.valid && left.code == right.code && left.frame_count == right.frame_count &&
        left.clip_count == right.clip_count &&
        left.total_clip_frame_references == right.total_clip_frame_references &&
        left.unique_referenced_frame_count == right.unique_referenced_frame_count &&
        left.unreferenced_frame_count == right.unreferenced_frame_count &&
        left.max_clip_frame_count == right.max_clip_frame_count &&
        left.atlas_page_count == right.atlas_page_count && left.atlas_area == right.atlas_area &&
        left.frame_area_sum == right.frame_area_sum && left.occupied_area == right.occupied_area &&
        left.free_area == right.free_area && left.overlap_area == right.overlap_area &&
        left.layout_fingerprint == right.layout_fingerprint;
}

} // namespace

int main() {
    const auto source_document = make_pressure_document();
    const auto canonical = noemancer::SpriteAssetCodec::write_canonical_json(source_document);
    const auto parsed = noemancer::SpriteAssetCodec::parse_json(canonical);
    if (!parsed || parsed.document->frames.size() != 1024U || parsed.document->clips.size() != 8U) {
        std::cerr << "1k+ frame pressure fixture did not round-trip through the SpriteAsset 0.2 codec\n";
        return 1;
    }
    if (canonical.find("texturePages") != std::string::npos) {
        std::cerr << "Pressure fixture introduced an unsupported texture-page schema\n";
        return 2;
    }

    const auto report = noemancer::SpriteAssetCodec::production_report(*parsed.document);
    const auto repeated_report = noemancer::SpriteAssetCodec::production_report(*parsed.document);
    if (!report.valid || !same_core_report(report, repeated_report) || report.frame_count != 1024U ||
        report.clip_count != 8U || report.total_clip_frame_references != 2048U ||
        report.unique_referenced_frame_count != 1024U || report.unreferenced_frame_count != 0U ||
        report.max_clip_frame_count != 256U || report.atlas_page_count != 1U ||
        report.atlas_area != 512U * 512U || report.frame_area_sum != 1024U * 64U ||
        report.occupied_area != 1024U * 64U || report.free_area != (512U * 512U - 1024U * 64U) ||
        report.overlap_area != 0U || report.layout_fingerprint == 0U) {
        std::cerr << "Sprite production report was not deterministic or did not quantify atlas occupancy\n";
        return 3;
    }
    const auto parsed_report = noemancer::SpriteAssetCodec::production_report(*parsed.document);
    if (!same_core_report(report, parsed_report)) {
        std::cerr << "Canonical parse changed the production report\n";
        return 4;
    }

    auto fragmented_document = *parsed.document;
    fragmented_document.frames[1].x = fragmented_document.frames[0].x;
    fragmented_document.frames[1].y = fragmented_document.frames[0].y;
    const auto fragmented = noemancer::SpriteAssetCodec::production_report(fragmented_document);
    if (!fragmented.valid || fragmented.overlap_area != 64U ||
        fragmented.occupied_area != report.occupied_area - 64U) {
        std::cerr << "Atlas overlap/fragmentation evidence was not measured deterministically\n";
        return 5;
    }

    auto limits = noemancer::SpriteAssetValidationLimits{};
    limits.max_frames = 1023U;
    if (const auto result = noemancer::SpriteAssetCodec::parse_json(canonical, limits);
        result || !has_error(result.errors, "sprite.frame-count-limit")) {
        std::cerr << "Frame-count production boundary was not enforced\n";
        return 6;
    }
    limits = {};
    limits.max_clips = 7U;
    if (const auto result = noemancer::SpriteAssetCodec::parse_json(canonical, limits);
        result || !has_error(result.errors, "sprite.clip-count-limit")) {
        std::cerr << "Clip-count production boundary was not enforced\n";
        return 7;
    }
    limits = {};
    limits.max_frames_per_clip = 255U;
    if (const auto result = noemancer::SpriteAssetCodec::parse_json(canonical, limits);
        result || !has_error(result.errors, "sprite.clip-frame-count-limit")) {
        std::cerr << "Per-clip frame production boundary was not enforced\n";
        return 8;
    }
    limits = {};
    limits.max_total_clip_frame_references = 2047U;
    if (const auto result = noemancer::SpriteAssetCodec::parse_json(canonical, limits);
        result || !has_error(result.errors, "sprite.total-clip-frame-count-limit")) {
        std::cerr << "Total clip-reference production boundary was not enforced\n";
        return 9;
    }
    limits = {};
    limits.max_source_bytes = canonical.size() - 1U;
    if (const auto result = noemancer::SpriteAssetCodec::parse_json(canonical, limits);
        result || !has_error(result.errors, "sprite.source-too-large")) {
        std::cerr << "Source-size production boundary was not enforced\n";
        return 10;
    }

    limits = {};
    limits.max_frames = 1023U;
    const auto rejected_report = noemancer::SpriteAssetCodec::production_report(*parsed.document, limits);
    if (rejected_report.valid || rejected_report.code != "sprite.frame-count-limit" ||
        !has_error(rejected_report.diagnostics, "sprite.frame-count-limit")) {
        std::cerr << "Production report did not expose a stable bounded failure code\n";
        return 11;
    }

    std::cout << "Sprite asset production pressure passed: 1024 frames, 8 clips, 1 atlas page\n";
    return 0;
}
