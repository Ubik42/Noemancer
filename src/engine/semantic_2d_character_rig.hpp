#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace noemancer {

// This is deliberately a disposable, Engine-owned prototype contract.  It is
// plain data only: no renderer, ECS, image, or third-party types are exposed.
inline constexpr std::string_view semantic_2d_character_rig_schema =
    "noemancer.semantic-2d-character-rig-prototype/0.1";
inline constexpr std::size_t semantic_2d_character_rig_max_source_bytes = 262144U;
inline constexpr std::size_t semantic_2d_character_rig_max_string_bytes = 4096U;
inline constexpr std::size_t semantic_2d_character_rig_max_id_bytes = 256U;
inline constexpr std::size_t semantic_2d_character_rig_max_parts = 256U;
inline constexpr std::size_t semantic_2d_character_rig_max_joints = 256U;
inline constexpr std::size_t semantic_2d_character_rig_max_material_channels = 128U;
inline constexpr std::size_t semantic_2d_character_rig_max_poses = 4096U;
inline constexpr std::size_t semantic_2d_character_rig_max_actions = 256U;
inline constexpr std::size_t semantic_2d_character_rig_max_directions = 32U;
inline constexpr std::size_t semantic_2d_character_rig_max_pose_part_transforms = 256U;
inline constexpr std::size_t semantic_2d_character_rig_max_action_frames = 4096U;

struct Semantic2DCharacterRigError final {
    std::string code;
    std::string path;
    std::string message;
};

struct Semantic2DCharacterRigProvenance final {
    std::string source_uri;
    std::string source_sha256;
    std::string generator;
    std::string license;
};

struct Semantic2DCharacterRigPart final {
    std::string id;
    std::string display_name;
    std::string joint_id;
    std::string source_asset;
    std::string material_channel_id;
    std::int32_t z_order{};
    float pivot_x{};
    float pivot_y{};
    bool visible{true};
};

struct Semantic2DCharacterRigJoint final {
    std::string id;
    std::string parent_id;
    float rest_x{};
    float rest_y{};
    float rest_rotation_degrees{};
};

struct Semantic2DCharacterRigMaterialChannel final {
    std::string id;
    std::string semantic;
    std::string source_asset;
    std::string texture_asset;
    std::string color_space{"srgb"};
    float intensity{1.0F};
    bool optional{};
};

struct Semantic2DCharacterRigPosePartTransform final {
    std::string part_id;
    float x{};
    float y{};
    float rotation_degrees{};
    float scale_x{1.0F};
    float scale_y{1.0F};
    float opacity{1.0F};
    bool visible{true};
    std::string material_channel_id;
    std::string cel_key;
};

struct Semantic2DCharacterRigPose final {
    std::string id;
    std::string direction_id;
    std::string frame_key;
    std::vector<Semantic2DCharacterRigPosePartTransform> part_transforms;
};

struct Semantic2DCharacterRigActionFrame final {
    std::string key;
    std::string pose_id;
    std::uint32_t duration_ms{100U};
};

struct Semantic2DCharacterRigAction final {
    std::string id;
    std::string display_name;
    bool looping{true};
    float sample_rate{12.0F};
    std::vector<Semantic2DCharacterRigActionFrame> frames;
};

struct Semantic2DCharacterRigDirection final {
    std::string id;
    std::string display_name;
    float angle_degrees{};
    std::string mirror_of_direction_id;
    std::string override_of_direction_id;
    bool mirror_x{};
    // Maps a base pose ID to a direction-specific replacement pose ID.  The
    // map key/value are semantic IDs, never array positions.
    std::map<std::string, std::string> pose_overrides;
};

struct Semantic2DCharacterRigDocument final {
    std::string schema{std::string(semantic_2d_character_rig_schema)};
    std::string character_id;
    std::string display_name;
    std::vector<Semantic2DCharacterRigPart> parts;
    std::vector<Semantic2DCharacterRigJoint> joints;
    std::vector<Semantic2DCharacterRigMaterialChannel> material_channels;
    std::vector<Semantic2DCharacterRigPose> poses;
    std::vector<Semantic2DCharacterRigAction> actions;
    std::vector<Semantic2DCharacterRigDirection> directions;
    Semantic2DCharacterRigProvenance provenance;
};

struct Semantic2DCharacterRigParseResult final {
    std::optional<Semantic2DCharacterRigDocument> document;
    std::vector<Semantic2DCharacterRigError> errors;

    [[nodiscard]] explicit operator bool() const noexcept {
        return document.has_value();
    }
};

class Semantic2DCharacterRigCodec final {
public:
    [[nodiscard]] static Semantic2DCharacterRigParseResult parse_json(
        std::string_view json);
    [[nodiscard]] static std::vector<Semantic2DCharacterRigError> validate(
        const Semantic2DCharacterRigDocument& document);
    [[nodiscard]] static std::string write_canonical_json(
        const Semantic2DCharacterRigDocument& document);
    [[nodiscard]] static std::vector<std::string> source_dependencies(
        const Semantic2DCharacterRigDocument& document);
};

} // namespace noemancer
