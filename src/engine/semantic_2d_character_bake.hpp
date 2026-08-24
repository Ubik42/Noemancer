#pragma once

#include "engine/semantic_2d_character_rig.hpp"
#include "engine/sprite_asset.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace noemancer {

struct Semantic2DBakeSettings final {
    std::string sprite_asset_id;
    std::string texture_asset_id;
    std::uint32_t atlas_width{512U};
    std::uint32_t atlas_height{512U};
    std::uint32_t frame_width{64U};
    std::uint32_t frame_height{64U};
    float pixels_per_unit{16.0F};
};

struct Semantic2DBakePartDraw final {
    std::string part_id;
    std::string joint_id;
    std::string source_asset;
    std::string material_channel_id;
    std::string cel_key;
    std::int32_t z_order{};
    float x{};
    float y{};
    float rotation_degrees{};
    float scale_x{1.0F};
    float scale_y{1.0F};
    float opacity{1.0F};
    bool visible{true};
};

struct Semantic2DBakeFrame final {
    std::string id;
    std::string action_id;
    std::string direction_id;
    std::string frame_key;
    std::string pose_id;
    std::uint32_t duration_ms{};
    bool mirrored{};
    std::vector<Semantic2DBakePartDraw> draws;
    std::string content_fingerprint;
};

struct Semantic2DBakeMetrics final {
    std::size_t rig_source_bytes{};
    std::size_t direct_frame_authoring_bytes{};
    std::size_t sprite_document_bytes{};
    std::size_t bake_manifest_bytes{};
    std::size_t package_metadata_bytes{};
};

struct Semantic2DBakePlan final {
    bool valid{};
    std::string code;
    std::string detail;
    std::string schema{"noemancer.semantic-2d-character-bake-plan-prototype/0.1"};
    std::string character_id;
    std::string source_fingerprint;
    std::string plan_fingerprint;
    SpriteAssetDocument sprite;
    std::vector<Semantic2DBakeFrame> frames;
    std::vector<std::string> source_dependencies;
    std::vector<std::string> registry_dependencies;
    Semantic2DBakeMetrics metrics;
};

struct Semantic2DBakeEditComparison final {
    bool comparable{};
    std::string code;
    std::size_t before_frames{};
    std::size_t after_frames{};
    std::size_t affected_frames{};
    std::size_t unchanged_frame_ids{};
    std::vector<std::string> affected_frame_ids;
};

class Semantic2DCharacterBakePrototype final {
public:
    [[nodiscard]] static Semantic2DBakePlan plan(
        const Semantic2DCharacterRigDocument& rig,
        const Semantic2DBakeSettings& settings);
    [[nodiscard]] static Semantic2DBakeEditComparison compare(
        const Semantic2DBakePlan& before,
        const Semantic2DBakePlan& after);
    [[nodiscard]] static std::string write_observation_json(
        const Semantic2DBakePlan& plan);
};

} // namespace noemancer
