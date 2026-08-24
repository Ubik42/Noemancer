#include "engine/sprite_asset.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

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

SpriteAssetDocument make_multipage_document() {
    SpriteAssetDocument document;
    document.schema = "noemancer.sprite-asset/0.2";
    document.asset_id = "sprite.multipage.pressure";
    document.texture_asset = "texture.multipage.pressure.atlas";
    document.texture_width = 4096U;
    document.texture_height = 4096U;
    document.pixels_per_unit = 16.0F;
    document.sampling = "nearest";
    document.alpha_mode = "cutout";
    document.provenance = {
        .source_uri = "generated/pressure/multipage.atlas.png",
        .source_sha256 = "multipage-pressure-fixture-sha256",
        .generator = "sprite-asset-production-pressure-test",
        .license = "CC0-1.0"};

    // The persisted document remains a single texture.  The planner will
    // deterministically split these 2,048 stable IDs into in-memory pages.
    document.frames.reserve(2048U);
    for (std::uint32_t index = 0U; index < 2048U; ++index) {
        const auto column = index % 64U;
        const auto row = index / 64U;
        document.frames.push_back({
            .id = "multipage.frame." + std::to_string(index),
            .x = column * 32U,
            .y = row * 32U,
            .width = 24U,
            .height = 24U,
            .trim_x = 0U,
            .trim_y = 0U,
            .source_width = 24U,
            .source_height = 24U,
            .pivot_x = 0.5F,
            .pivot_y = 0.9F,
            .collision_profile = "pressure.body"});
    }
    return document;
}

SpriteAssetDocument make_projection_bound_document() {
    auto document = make_multipage_document();
    document.asset_id = "sprite.multipage.projection-bound";
    document.frames.reserve(6144U);
    for (std::uint32_t index = 2048U; index < 6144U; ++index) {
        const auto column = index % 128U;
        const auto row = index / 128U;
        document.frames.push_back({
            .id = "multipage.frame." + std::to_string(index),
            .x = column * 32U,
            .y = row * 32U,
            .width = 24U,
            .height = 24U,
            .trim_x = 0U,
            .trim_y = 0U,
            .source_width = 24U,
            .source_height = 24U,
            .pivot_x = 0.5F,
            .pivot_y = 0.9F,
            .collision_profile = "pressure.body"});
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

    const auto multipage_document = make_multipage_document();
    noemancer::SpriteAtlasPlanningOptions single_page_options;
    single_page_options.page_width = 4096U;
    single_page_options.page_height = 4096U;
    single_page_options.padding = 1U;
    const auto single_page = noemancer::SpriteAssetCodec::plan_atlas_pages(
        multipage_document, single_page_options);
    if (!single_page.valid || single_page.frame_count != 2048U || single_page.page_count != 1U ||
        single_page.placements.size() != 2048U || single_page.pages.size() != 1U ||
        single_page.pages.front().occupied_area != 2048U * 24U * 24U ||
        single_page.pages.front().overlap_area != 0U || single_page.layout_fingerprint == 0U) {
        std::cerr << "2k-frame single-page atlas planning did not produce bounded deterministic evidence\n";
        return 12;
    }

    noemancer::SpriteAtlasPlanningOptions multipage_options;
    multipage_options.page_width = 256U;
    multipage_options.page_height = 256U;
    multipage_options.padding = 2U;
    const auto multipage = noemancer::SpriteAssetCodec::plan_atlas_pages(
        multipage_document, multipage_options);
    if (!multipage.valid || multipage.page_count <= 1U || multipage.page_count > 4096U ||
        multipage.planned_cook_pixels != multipage.page_count * 256U * 256U ||
        multipage.planned_cook_bytes != multipage.planned_cook_pixels * 4U ||
        multipage.single_atlas_cook_pixels != 4096U * 4096U ||
        multipage.single_atlas_cook_bytes != multipage.single_atlas_cook_pixels * 4U ||
        multipage.planned_cook_bytes >= multipage.single_atlas_cook_bytes ||
        multipage.layout_fingerprint == 0U) {
        std::cerr << "2k-frame multi-page atlas planning did not quantify a valid page comparison\n";
        return 13;
    }
    for (const auto& page : multipage.pages) {
        if (page.frame_count == 0U || page.overlap_area != 0U ||
            page.occupied_area + page.free_area != 256U * 256U ||
            page.estimated_cook_pixels != 256U * 256U ||
            page.estimated_cook_bytes != 256U * 256U * 4U || page.layout_fingerprint == 0U) {
            std::cerr << "Multi-page report omitted a page occupancy/free/overlap invariant\n";
            return 14;
        }
    }

    auto reordered_document = multipage_document;
    std::reverse(reordered_document.frames.begin(), reordered_document.frames.end());
    const auto reordered = noemancer::SpriteAssetCodec::plan_atlas_pages(
        reordered_document, multipage_options);
    if (!reordered.valid || reordered.layout_fingerprint != multipage.layout_fingerprint ||
        reordered.page_count != multipage.page_count || reordered.placements.size() != multipage.placements.size()) {
        std::cerr << "Frame input order changed the stable atlas plan fingerprint\n";
        return 15;
    }
    for (std::size_t index = 0U; index < multipage.placements.size(); ++index) {
        const auto& left = multipage.placements[index];
        const auto& right = reordered.placements[index];
        if (left.frame_id != right.frame_id || left.page_index != right.page_index || left.x != right.x ||
            left.y != right.y || left.width != right.width || left.height != right.height) {
            std::cerr << "Frame input order changed a stable placement\n";
            return 16;
        }
    }

    const auto local_change = noemancer::SpriteAssetCodec::plan_atlas_pages(
        multipage_document, multipage_options, {"multipage.frame.1234"});
    if (!local_change.valid || local_change.changed_frame_count != 1U ||
        local_change.affected_page_indices.size() != 1U || local_change.incremental_cook_pixels != 256U * 256U ||
        local_change.incremental_cook_bytes != 256U * 256U * 4U ||
        local_change.incremental_cook_bytes >= local_change.single_atlas_cook_bytes) {
        std::cerr << "Single-frame change did not produce a local page Cook estimate\n";
        return 17;
    }
    const auto unknown_change = noemancer::SpriteAssetCodec::plan_atlas_pages(
        multipage_document, multipage_options, {"multipage.frame.missing"});
    if (unknown_change.valid || unknown_change.code != "sprite.atlas-unknown-frame" ||
        !has_error(unknown_change.diagnostics, "sprite.atlas-unknown-frame")) {
        std::cerr << "Unknown changed frame ID did not fail with a stable bounded code\n";
        return 18;
    }

    auto too_large_document = multipage_document;
    too_large_document.frames.front().width = 253U;
    too_large_document.frames.front().source_width = 253U;
    const auto too_large = noemancer::SpriteAssetCodec::plan_atlas_pages(
        too_large_document, multipage_options);
    if (too_large.valid || too_large.code != "sprite.atlas-frame-too-large" ||
        !has_error(too_large.diagnostics, "sprite.atlas-frame-too-large")) {
        std::cerr << "Over-sized frame was not rejected by the atlas page boundary\n";
        return 19;
    }
    auto page_limited_options = multipage_options;
    page_limited_options.limits.max_pages = 1U;
    const auto page_limited = noemancer::SpriteAssetCodec::plan_atlas_pages(
        multipage_document, page_limited_options);
    if (page_limited.valid || page_limited.code != "sprite.atlas-page-limit" ||
        !has_error(page_limited.diagnostics, "sprite.atlas-page-limit")) {
        std::cerr << "Atlas page hard limit was not enforced\n";
        return 20;
    }

    const auto plan_json = noemancer::sprite_atlas_plan_json(
        multipage_document, multipage_options, {"multipage.frame.1234"});
    const auto repeated_plan_json = noemancer::sprite_atlas_plan_json(
        multipage_document, multipage_options, {"multipage.frame.1234"});
    const auto plan_projection = nlohmann::json::parse(plan_json);
    if (plan_json != repeated_plan_json ||
        plan_projection.at("schemaVersion") != "noemancer.sprite-atlas-plan/0.1" ||
        !plan_projection.at("valid").get<bool>() || plan_projection.at("code") != "ok" ||
        plan_projection.at("frameCount") != 2048U || plan_projection.at("changedFrameCount") != 1U ||
        plan_projection.at("pages").at("total") != multipage.page_count ||
        plan_projection.at("pages").at("maxItems") != noemancer::sprite_atlas_plan_max_projected_pages ||
        plan_projection.at("pages").at("emitted") != multipage.page_count ||
        plan_projection.at("pages").at("truncated").get<bool>() ||
        plan_projection.at("placements").at("total") != 2048U ||
        plan_projection.at("placements").at("planned") != 2048U ||
        plan_projection.at("placements").at("maxItems") != noemancer::sprite_atlas_plan_max_projected_placements ||
        plan_projection.at("placements").at("emitted") != 2048U ||
        plan_projection.at("placements").at("omitted") != 0U ||
        plan_projection.at("placements").at("truncated").get<bool>() ||
        plan_projection.at("placements").at("items").size() != 2048U ||
        plan_projection.at("affectedPages").size() != 1U ||
        plan_projection.at("cookEstimate").at("fullPlan").at("pixels") != multipage.planned_cook_pixels ||
        plan_projection.at("cookEstimate").at("incremental").at("bytes") != local_change.incremental_cook_bytes ||
        plan_projection.at("cookEstimate").at("singleAtlasBaseline").at("bytes") != local_change.single_atlas_cook_bytes ||
        plan_projection.at("scope").get<std::string>().find("not encoded file size") == std::string::npos ||
        plan_projection.at("diagnostics").size() != 0U) {
        std::cerr << "Stable atlas planning JSON omitted required bounded observation fields\n";
        return 21;
    }

    const auto projection_bound_document = make_projection_bound_document();
    const auto bounded_plan_json = noemancer::sprite_atlas_plan_json(
        projection_bound_document, multipage_options, {});
    const auto bounded_projection = nlohmann::json::parse(bounded_plan_json);
    if (!bounded_projection.at("valid").get<bool>() || bounded_projection.at("frameCount") != 6144U ||
        bounded_projection.at("placements").at("total") != 6144U ||
        bounded_projection.at("placements").at("planned") != 6144U ||
        bounded_projection.at("placements").at("maxItems") != noemancer::sprite_atlas_plan_max_projected_placements ||
        bounded_projection.at("placements").at("emitted") != noemancer::sprite_atlas_plan_max_projected_placements ||
        bounded_projection.at("placements").at("omitted") != 6144U - noemancer::sprite_atlas_plan_max_projected_placements ||
        !bounded_projection.at("placements").at("truncated").get<bool>() ||
        bounded_projection.at("placements").at("items").size() != noemancer::sprite_atlas_plan_max_projected_placements) {
        std::cerr << "Large atlas planning JSON did not enforce the placement projection bound\n";
        return 22;
    }
    const auto invalid_projection = nlohmann::json::parse(noemancer::sprite_atlas_plan_json(
        multipage_document, multipage_options, {"multipage.frame.missing"}));
    if (invalid_projection.at("valid").get<bool>() ||
        invalid_projection.at("code") != "sprite.atlas-unknown-frame" ||
        invalid_projection.at("diagnostics").empty() ||
        invalid_projection.at("scope").get<std::string>().find("GPU upload cost") == std::string::npos) {
        std::cerr << "Invalid atlas planning JSON did not preserve bounded diagnostics/scope\n";
        return 23;
    }

    std::cout << "Sprite asset production pressure passed: 1024 frames, 8 clips, 2048-frame deterministic multi-page planning\n";
    return 0;
}
