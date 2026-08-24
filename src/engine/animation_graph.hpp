#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace noemancer {

inline constexpr std::string_view animation_graph_schema = "noemancer.animation-graph/0.1";
inline constexpr float animation_graph_editor_coordinate_limit = 1'000'000.0F;

struct AnimationGraphParameter final {
    std::string id;
    std::string type{"float"};
    float default_value{};
};

struct AnimationGraphBlendPoint final {
    std::string node_id;
    float threshold{};
};

struct AnimationGraphNode final {
    std::string id;
    std::string kind;
    std::string clip_asset;
    bool looping{true};
    std::string state_machine_asset;
    std::string parameter;
    std::vector<AnimationGraphBlendPoint> children;
};

struct AnimationGraphMaskJoint final {
    std::string name;
    float weight{1.0F};
};

struct AnimationGraphMask final {
    std::string id;
    bool include_descendants{true};
    std::vector<AnimationGraphMaskJoint> joints;
};

struct AnimationGraphSyncGroup final {
    std::string id;
    std::string mode{"normalized-time"};
};

struct AnimationGraphLayer final {
    std::string id;
    std::string root_node;
    std::string mode{"override"};
    float weight{1.0F};
    std::string weight_parameter;
    std::string mask_id;
    std::string sync_group;
};

struct AnimationGraphNodeLayout final {
    std::string node_id;
    float x{};
    float y{};
    bool collapsed{};
};

struct AnimationGraphEditorLayout final {
    std::vector<AnimationGraphNodeLayout> nodes;
    float zoom{1.0F};
    float pan_x{};
    float pan_y{};
};

struct AnimationGraphDocument final {
    std::string asset_id;
    std::vector<AnimationGraphParameter> parameters;
    std::vector<AnimationGraphNode> nodes;
    std::vector<AnimationGraphLayer> layers;
    std::vector<AnimationGraphMask> masks;
    std::vector<AnimationGraphSyncGroup> sync_groups;
    AnimationGraphEditorLayout editor;
};

struct AnimationGraphParseResult final {
    std::optional<AnimationGraphDocument> document;
    std::string code;
    std::string detail;
    explicit operator bool() const noexcept { return document.has_value(); }
};

struct AnimationGraphBlendSelection final {
    bool valid{};
    std::string code;
    std::string first_node;
    std::string second_node;
    float first_weight{1.0F};
    float second_weight{};
};

class AnimationGraphCodec final {
public:
    [[nodiscard]] static AnimationGraphParseResult parse_json(std::string_view source);
    [[nodiscard]] static std::string write_canonical_json(const AnimationGraphDocument& document);
    [[nodiscard]] static std::vector<std::string> asset_dependencies(const AnimationGraphDocument& document);
    [[nodiscard]] static AnimationGraphBlendSelection select_blend_1d(
        const AnimationGraphDocument& document, std::string_view node_id,
        const std::unordered_map<std::string, float>& parameters);
};

class AnimationGraphLibrary final {
public:
    [[nodiscard]] bool register_document(AnimationGraphDocument document);
    [[nodiscard]] const AnimationGraphDocument* find(std::string_view asset_id) const;
    [[nodiscard]] std::string inspect_json(std::string_view asset_id) const;

private:
    std::unordered_map<std::string, AnimationGraphDocument> documents_;
};

} // namespace noemancer
