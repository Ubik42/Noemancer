#include "engine/semantic_2d_character_bake.hpp"
#include "engine/semantic_2d_character_rig.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace {

std::string read_text(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    std::ostringstream stream;
    stream << input.rdbuf();
    return stream.str();
}

} // namespace

int main() {
    const auto fixture_root = std::filesystem::path(NOEMANCER_SOURCE_DIR) /
        "tests" / "fixtures" / "semantic-2d-rig";
    std::vector<std::filesystem::path> fixtures;
    std::error_code iteration_error;
    for (std::filesystem::directory_iterator iterator(fixture_root, iteration_error), end;
         !iteration_error && iterator != end; iterator.increment(iteration_error)) {
        if (iterator->is_regular_file() &&
            iterator->path().filename().string().ends_with(".rig.json")) {
            fixtures.push_back(iterator->path());
        }
    }
    std::ranges::sort(fixtures);
    if (iteration_error || fixtures.size() < 2U) {
        std::cerr << "semantic 2D Rig prototype requires at least two .rig.json fixtures\n";
        return 1;
    }

    std::set<std::string> character_ids;
    std::size_t action_count{};
    std::size_t direction_count{};
    std::size_t action_direction_pairs{};
    std::size_t canonical_bytes{};
    std::size_t baked_frames{};
    std::size_t sprite_clips{};
    std::size_t direct_authoring_bytes{};
    std::size_t package_metadata_bytes{};
    std::vector<noemancer::Semantic2DCharacterRigDocument> documents;
    for (const auto& fixture : fixtures) {
        const auto source = read_text(fixture);
        const auto parsed = noemancer::Semantic2DCharacterRigCodec::parse_json(source);
        if (!parsed) {
            std::cerr << fixture << " failed Rig parse";
            for (const auto& error : parsed.errors) {
                std::cerr << "\n" << error.code << " " << error.path << ": " << error.message;
            }
            std::cerr << '\n';
            return 2;
        }
        const auto& document = *parsed.document;
        if (!character_ids.insert(document.character_id).second ||
            document.parts.empty() || document.joints.empty() ||
            document.material_channels.empty() || document.poses.empty() ||
            document.actions.size() < 2U || document.directions.size() < 4U ||
            document.provenance.source_uri.empty() || document.provenance.generator.empty() ||
            document.provenance.license.empty()) {
            std::cerr << fixture << " does not cover the prototype identity contract\n";
            return 3;
        }
        const auto canonical = noemancer::Semantic2DCharacterRigCodec::write_canonical_json(document);
        const auto round_trip = noemancer::Semantic2DCharacterRigCodec::parse_json(canonical);
        if (!round_trip ||
            noemancer::Semantic2DCharacterRigCodec::write_canonical_json(*round_trip.document) != canonical) {
            std::cerr << fixture << " canonical round trip is not deterministic\n";
            return 4;
        }
        action_count += document.actions.size();
        direction_count += document.directions.size();
        action_direction_pairs += document.actions.size() * document.directions.size();
        canonical_bytes += canonical.size();
        const noemancer::Semantic2DBakeSettings settings{
            .sprite_asset_id = "sprite." + document.character_id + ".baked",
            .texture_asset_id = "texture." + document.character_id + ".atlas",
            .atlas_width = 512U,
            .atlas_height = 512U,
            .frame_width = 64U,
            .frame_height = 64U,
            .pixels_per_unit = 16.0F
        };
        const auto bake = noemancer::Semantic2DCharacterBakePrototype::plan(document, settings);
        const auto repeated = noemancer::Semantic2DCharacterBakePrototype::plan(document, settings);
        auto reordered = document;
        std::ranges::reverse(reordered.parts);
        std::ranges::reverse(reordered.joints);
        std::ranges::reverse(reordered.material_channels);
        std::ranges::reverse(reordered.poses);
        std::ranges::reverse(reordered.actions);
        std::ranges::reverse(reordered.directions);
        const auto reordered_bake = noemancer::Semantic2DCharacterBakePrototype::plan(reordered, settings);
        std::size_t expected_frames{};
        for (const auto& action : document.actions) {
            expected_frames += action.frames.size() * document.directions.size();
        }
        if (!bake.valid || !repeated.valid || !reordered_bake.valid ||
            bake.frames.size() != expected_frames ||
            bake.sprite.clips.size() != document.actions.size() * document.directions.size() ||
            bake.plan_fingerprint.empty() || bake.plan_fingerprint != repeated.plan_fingerprint ||
            bake.plan_fingerprint != reordered_bake.plan_fingerprint ||
            bake.registry_dependencies != noemancer::SpriteAssetCodec::asset_dependencies(bake.sprite)) {
            std::cerr << fixture << " did not deterministically adapt into Sprite/Registry/Cook plain data\n";
            return 6;
        }
        baked_frames += bake.frames.size();
        sprite_clips += bake.sprite.clips.size();
        direct_authoring_bytes += bake.metrics.direct_frame_authoring_bytes;
        package_metadata_bytes += bake.metrics.package_metadata_bytes;
        documents.push_back(document);
    }
    if (character_ids.size() < 2U || action_count < 4U || direction_count < 8U ||
        action_direction_pairs < 16U) {
        std::cerr << "semantic 2D Rig prototype did not reach 2 characters x 2 actions x 4 directions\n";
        return 5;
    }
    auto edited = documents.front();
    auto pose = std::ranges::find(edited.poses, std::string("pose.idle.south"),
        &noemancer::Semantic2DCharacterRigPose::id);
    if (pose == edited.poses.end() || pose->part_transforms.empty()) return 7;
    const noemancer::Semantic2DBakeSettings edit_settings{
        .sprite_asset_id = "sprite." + edited.character_id + ".baked",
        .texture_asset_id = "texture." + edited.character_id + ".atlas"
    };
    const auto before = noemancer::Semantic2DCharacterBakePrototype::plan(documents.front(), edit_settings);
    pose->part_transforms.front().x += 1.0F;
    const auto after = noemancer::Semantic2DCharacterBakePrototype::plan(edited, edit_settings);
    const auto comparison = noemancer::Semantic2DCharacterBakePrototype::compare(before, after);
    if (!comparison.comparable || comparison.affected_frames != 1U ||
        comparison.unchanged_frame_ids != before.frames.size()) {
        std::cerr << "isolated semantic pose edit did not remain localized and identity-stable\n";
        return 8;
    }
    auto unknown_field = nlohmann::json::parse(
        noemancer::Semantic2DCharacterRigCodec::write_canonical_json(documents.front()));
    unknown_field["privateRuntimeHandle"] = 42;
    if (noemancer::Semantic2DCharacterRigCodec::parse_json(unknown_field.dump())) {
        std::cerr << "prototype source accepted an unknown execution field\n";
        return 9;
    }
    auto broken_reference = documents.front();
    broken_reference.parts.front().joint_id = "joint.missing";
    if (noemancer::Semantic2DCharacterRigCodec::validate(broken_reference).empty()) {
        std::cerr << "prototype source accepted a broken semantic reference\n";
        return 10;
    }

    nlohmann::json receipt{
        {"schema", "noemancer.semantic-2d-character-rig-prototype-evidence/0.1"},
        {"success", true},
        {"characters", character_ids.size()},
        {"actions", action_count},
        {"directions", direction_count},
        {"actionDirectionPairs", action_direction_pairs},
        {"bakedFrames", baked_frames},
        {"spriteClips", sprite_clips},
        {"canonicalSourceBytes", canonical_bytes},
        {"directFrameAuthoringBytes", direct_authoring_bytes},
        {"packageMetadataBytes", package_metadata_bytes},
        {"canonicalRoundTripStable", true},
        {"strictUnknownFieldRejection", true},
        {"referenceIntegrity", true},
        {"arrayOrderIndependent", true},
        {"isolatedEdit", {{"affectedFrames", comparison.affected_frames},
            {"stableFrameIds", comparison.unchanged_frame_ids}}},
        {"productionRecommendation", direct_authoring_bytes > canonical_bytes
            ? "candidate-for-adr" : "close-unless-larger-workload-proves-authoring-benefit"}
    };
    std::cout << receipt.dump() << '\n';
    return 0;
}
