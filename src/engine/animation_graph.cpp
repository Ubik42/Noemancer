#include "animation_graph.hpp"

#include <algorithm>
#include <cmath>
#include <functional>
#include <nlohmann/json.hpp>
#include <unordered_set>

namespace noemancer {
namespace {

using Json = nlohmann::json;

constexpr std::size_t max_parameters = 64;
constexpr std::size_t max_nodes = 256;
constexpr std::size_t max_layers = 16;
constexpr std::size_t max_masks = 32;
constexpr std::size_t max_mask_joints = 256;
constexpr std::size_t max_sync_groups = 32;
constexpr std::size_t max_graph_depth = 32;

AnimationGraphParseResult failure(std::string code, std::string detail) {
    return {std::nullopt, std::move(code), std::move(detail)};
}

bool finite(const float value) { return std::isfinite(value); }

Json document_json(const AnimationGraphDocument& document) {
    Json parameters = Json::array();
    for (const auto& parameter : document.parameters) parameters.push_back({
        {"id", parameter.id}, {"type", parameter.type}, {"default", parameter.default_value}});
    Json nodes = Json::array();
    for (const auto& node : document.nodes) {
        Json value{{"id", node.id}, {"kind", node.kind}};
        if (node.kind == "clip") {
            value["clipAsset"] = node.clip_asset;
            value["looping"] = node.looping;
        } else if (node.kind == "state-machine") value["stateMachineAsset"] = node.state_machine_asset;
        else if (node.kind == "blend-1d") {
            value["parameter"] = node.parameter;
            value["children"] = Json::array();
            for (const auto& child : node.children)
                value["children"].push_back({{"nodeId", child.node_id}, {"threshold", child.threshold}});
        }
        nodes.push_back(std::move(value));
    }
    Json layers = Json::array();
    for (const auto& layer : document.layers) {
        Json value{{"id", layer.id}, {"rootNode", layer.root_node}, {"mode", layer.mode}, {"weight", layer.weight}};
        if (!layer.weight_parameter.empty()) value["weightParameter"] = layer.weight_parameter;
        if (!layer.mask_id.empty()) value["maskId"] = layer.mask_id;
        if (!layer.sync_group.empty()) value["syncGroup"] = layer.sync_group;
        layers.push_back(std::move(value));
    }
    Json masks = Json::array();
    for (const auto& mask : document.masks) {
        Json joints = Json::array();
        for (const auto& joint : mask.joints) joints.push_back({{"name", joint.name}, {"weight", joint.weight}});
        masks.push_back({{"id", mask.id}, {"includeDescendants", mask.include_descendants}, {"joints", std::move(joints)}});
    }
    Json sync_groups = Json::array();
    for (const auto& group : document.sync_groups) sync_groups.push_back({{"id", group.id}, {"mode", group.mode}});
    Json layout_nodes = Json::array();
    for (const auto& node : document.editor.nodes) layout_nodes.push_back(
        {{"id", node.node_id}, {"position", {node.x, node.y}}, {"collapsed", node.collapsed}});
    return {{"schemaVersion", animation_graph_schema}, {"assetId", document.asset_id},
        {"parameters", std::move(parameters)}, {"nodes", std::move(nodes)}, {"layers", std::move(layers)},
        {"masks", std::move(masks)}, {"syncGroups", std::move(sync_groups)},
        {"editor", {{"nodes", std::move(layout_nodes)}, {"zoom", document.editor.zoom},
                    {"pan", {document.editor.pan_x, document.editor.pan_y}}}}};
}

} // namespace

AnimationGraphParseResult AnimationGraphCodec::parse_json(const std::string_view source) {
    try {
    const auto root = Json::parse(source, nullptr, false);
    if (root.is_discarded() || !root.is_object())
        return failure("animation.graph.invalid-json", "Animation Graph must be a JSON object.");
    if (root.value("schemaVersion", std::string{}) != animation_graph_schema)
        return failure("animation.graph.unsupported-schema", "Expected noemancer.animation-graph/0.1.");
    AnimationGraphDocument document;
    document.asset_id = root.value("assetId", std::string{});
    if (document.asset_id.empty()) return failure("animation.graph.identity-missing", "assetId is required.");
    const auto parameters = root.value("parameters", Json::array());
    const auto nodes = root.value("nodes", Json::array());
    const auto layers = root.value("layers", Json::array());
    const auto masks = root.value("masks", Json::array());
    const auto sync_groups = root.value("syncGroups", Json::array());
    const auto editor = root.value("editor", Json::object());
    if (!parameters.is_array() || !nodes.is_array() || !layers.is_array() || !masks.is_array() ||
        !sync_groups.is_array() || !editor.is_object() || nodes.empty() || layers.empty() ||
        parameters.size() > max_parameters || nodes.size() > max_nodes || layers.size() > max_layers ||
        masks.size() > max_masks || sync_groups.size() > max_sync_groups)
        return failure("animation.graph.structure-invalid", "Graph arrays are missing, empty, or exceed bounded limits.");

    std::unordered_set<std::string> parameter_ids, node_ids, layer_ids, mask_ids, sync_group_ids;
    std::unordered_map<std::string, std::string> parameter_types;
    for (const auto& value : parameters) {
        if (!value.is_object()) return failure("animation.graph.parameter-invalid", "Every parameter must be an object.");
        AnimationGraphParameter parameter{value.value("id", std::string{}), value.value("type", std::string{"float"}),
            value.value("default", 0.0F)};
        if (parameter.id.empty() || (parameter.type != "float" && parameter.type != "bool") ||
            !finite(parameter.default_value) || !parameter_ids.insert(parameter.id).second)
            return failure("animation.graph.parameter-invalid", "Parameter IDs and finite float/bool defaults must be valid and unique.");
        if (parameter.type == "bool") parameter.default_value = parameter.default_value >= 0.5F ? 1.0F : 0.0F;
        parameter_types.emplace(parameter.id, parameter.type);
        document.parameters.push_back(std::move(parameter));
    }
    std::size_t state_machine_node_count{};
    for (const auto& value : nodes) {
        if (!value.is_object()) return failure("animation.graph.node-invalid", "Every node must be an object.");
        AnimationGraphNode node;
        node.id = value.value("id", std::string{}); node.kind = value.value("kind", std::string{});
        if (node.id.empty() || !node_ids.insert(node.id).second ||
            (node.kind != "clip" && node.kind != "state-machine" && node.kind != "blend-1d"))
            return failure("animation.graph.node-invalid", "Node IDs must be unique and kind must be clip, state-machine, or blend-1d.");
        if (node.kind == "clip") {
            node.clip_asset = value.value("clipAsset", std::string{}); node.looping = value.value("looping", true);
            if (node.clip_asset.empty()) return failure("animation.graph.clip-invalid", "Clip nodes require clipAsset.");
        } else if (node.kind == "state-machine") {
            ++state_machine_node_count;
            node.state_machine_asset = value.value("stateMachineAsset", std::string{});
            if (node.state_machine_asset.empty()) return failure("animation.graph.state-machine-invalid", "State-machine nodes require stateMachineAsset.");
        } else {
            node.parameter = value.value("parameter", std::string{});
            const auto children = value.value("children", Json::array());
            if (!children.is_array() || children.size() < 2 || children.size() > 32 ||
                !parameter_types.contains(node.parameter) || parameter_types.at(node.parameter) != "float")
                return failure("animation.graph.blend-invalid", "Blend 1D nodes require a float parameter and 2-32 children.");
            std::unordered_set<std::string> child_ids;
            for (const auto& child_value : children) {
                if (!child_value.is_object())
                    return failure("animation.graph.blend-invalid", "Every Blend 1D child must be an object.");
                AnimationGraphBlendPoint child{child_value.value("nodeId", std::string{}), child_value.value("threshold", 0.0F)};
                if (child.node_id.empty() || !finite(child.threshold) || !child_ids.insert(child.node_id).second)
                    return failure("animation.graph.blend-invalid", "Blend child IDs and thresholds must be valid and unique.");
                node.children.push_back(std::move(child));
            }
            std::ranges::sort(node.children, {}, &AnimationGraphBlendPoint::threshold);
            for (std::size_t index = 1; index < node.children.size(); ++index)
                if (node.children[index - 1].threshold >= node.children[index].threshold)
                    return failure("animation.graph.blend-invalid", "Blend thresholds must be strictly increasing.");
        }
        document.nodes.push_back(std::move(node));
    }
    if(state_machine_node_count>1U)
        return failure("animation.graph.state-machine-count-unsupported",
            "Animation Graph 0.1 supports at most one state-machine node so its instance state remains unambiguous.");
    for (const auto& value : masks) {
        if (!value.is_object()) return failure("animation.graph.mask-invalid", "Every mask must be an object.");
        AnimationGraphMask mask; mask.id = value.value("id", std::string{});
        mask.include_descendants = value.value("includeDescendants", true);
        const auto joints = value.value("joints", Json::array());
        if (mask.id.empty() || !mask_ids.insert(mask.id).second || !joints.is_array() || joints.empty() || joints.size() > max_mask_joints)
            return failure("animation.graph.mask-invalid", "Masks require a unique ID and 1-256 joints.");
        std::unordered_set<std::string> joint_names;
        for (const auto& joint_value : joints) {
            AnimationGraphMaskJoint joint{joint_value.value("name", std::string{}), joint_value.value("weight", 1.0F)};
            if (!joint_value.is_object() || joint.name.empty() || !joint_names.insert(joint.name).second ||
                !finite(joint.weight) || joint.weight < 0.0F || joint.weight > 1.0F)
                return failure("animation.graph.mask-invalid", "Mask joint names must be unique and weights must be in [0,1].");
            mask.joints.push_back(std::move(joint));
        }
        document.masks.push_back(std::move(mask));
    }
    for (const auto& value : sync_groups) {
        AnimationGraphSyncGroup group{value.value("id", std::string{}), value.value("mode", std::string{"normalized-time"})};
        if (!value.is_object() || group.id.empty() || group.mode != "normalized-time" || !sync_group_ids.insert(group.id).second)
            return failure("animation.graph.sync-group-invalid", "Sync groups require unique IDs and normalized-time mode.");
        document.sync_groups.push_back(std::move(group));
    }
    for (const auto& value : layers) {
        if (!value.is_object()) return failure("animation.graph.layer-invalid", "Every layer must be an object.");
        AnimationGraphLayer layer;
        layer.id = value.value("id", std::string{}); layer.root_node = value.value("rootNode", std::string{});
        layer.mode = value.value("mode", std::string{"override"}); layer.weight = value.value("weight", 1.0F);
        layer.weight_parameter = value.value("weightParameter", std::string{});
        layer.mask_id = value.value("maskId", std::string{}); layer.sync_group = value.value("syncGroup", std::string{});
        if (layer.id.empty() || !layer_ids.insert(layer.id).second || !node_ids.contains(layer.root_node) ||
            (layer.mode != "override" && layer.mode != "additive") || !finite(layer.weight) || layer.weight < 0.0F || layer.weight > 1.0F ||
            (!layer.weight_parameter.empty() && (!parameter_types.contains(layer.weight_parameter) || parameter_types.at(layer.weight_parameter) != "float")) ||
            (!layer.mask_id.empty() && !mask_ids.contains(layer.mask_id)) ||
            (!layer.sync_group.empty() && !sync_group_ids.contains(layer.sync_group)))
            return failure("animation.graph.layer-invalid", "Layer references, mode, and weight are invalid.");
        document.layers.push_back(std::move(layer));
    }
    if(document.layers.front().mode!="override"||document.layers.front().weight!=1.0F||
       !document.layers.front().weight_parameter.empty())
        return failure("animation.graph.base-layer-invalid",
            "The first layer must be an override base layer with constant weight 1.");
    bool additive_layer_seen=false;
    for(const auto& layer:document.layers) {
        additive_layer_seen=additive_layer_seen||layer.mode=="additive";
        if(additive_layer_seen&&layer.mode=="override")
            return failure("animation.graph.layer-order-unsupported",
                "Animation Graph 0.1 requires all override layers before additive layers.");
    }
    std::unordered_map<std::string, const AnimationGraphNode*> node_map;
    for (const auto& node : document.nodes) node_map.emplace(node.id, &node);
    for (const auto& node : document.nodes) for (const auto& child : node.children) {
        if (!node_map.contains(child.node_id)) return failure("animation.graph.node-reference-invalid", "Blend child references an unknown node.");
        if(node_map.at(child.node_id)->kind!="clip")
            return failure("animation.graph.nested-blend-unsupported",
                "Animation Graph 0.1 Blend 1D children must be terminal clip nodes.");
    }
    std::unordered_map<std::string, int> colors;
    std::function<bool(const AnimationGraphNode&, std::size_t)> visit = [&](const AnimationGraphNode& node, const std::size_t depth) {
        if (depth > max_graph_depth || colors[node.id] == 1) return false;
        if (colors[node.id] == 2) return true;
        colors[node.id] = 1;
        for (const auto& child : node.children) if (!visit(*node_map.at(child.node_id), depth + 1)) return false;
        colors[node.id] = 2; return true;
    };
    for (const auto& layer : document.layers) if (!visit(*node_map.at(layer.root_node), 1))
        return failure("animation.graph.cycle-or-depth", "Graph contains a cycle or exceeds depth 32.");

    const auto layout_nodes = editor.value("nodes", Json::array());
    const auto pan = editor.value("pan", Json::array({0.0F, 0.0F}));
    document.editor.zoom = editor.value("zoom", 1.0F);
    if (!layout_nodes.is_array() || layout_nodes.size() > max_nodes || !pan.is_array() || pan.size() != 2 ||
        !finite(document.editor.zoom) || document.editor.zoom < 0.1F || document.editor.zoom > 4.0F)
        return failure("animation.graph.editor-invalid", "Editor layout is invalid or exceeds limits.");
    document.editor.pan_x = pan[0].get<float>(); document.editor.pan_y = pan[1].get<float>();
    if (!finite(document.editor.pan_x) || !finite(document.editor.pan_y))
        return failure("animation.graph.editor-invalid", "Editor pan must be finite.");
    std::unordered_set<std::string> layout_ids;
    for (const auto& value : layout_nodes) {
        const auto position = value.value("position", Json::array());
        AnimationGraphNodeLayout layout; layout.node_id = value.value("id", std::string{});
        if (!value.is_object() || !node_ids.contains(layout.node_id) || !layout_ids.insert(layout.node_id).second ||
            !position.is_array() || position.size() != 2)
            return failure("animation.graph.editor-invalid", "Editor node layout must reference a unique graph node.");
        layout.x = position[0].get<float>(); layout.y = position[1].get<float>(); layout.collapsed = value.value("collapsed", false);
        if (!finite(layout.x) || !finite(layout.y) || std::abs(layout.x)>animation_graph_editor_coordinate_limit ||
            std::abs(layout.y)>animation_graph_editor_coordinate_limit)
            return failure("animation.graph.editor-invalid", "Editor positions must be finite and bounded.");
        document.editor.nodes.push_back(std::move(layout));
    }
    std::ranges::sort(document.parameters, {}, &AnimationGraphParameter::id);
    std::ranges::sort(document.nodes, {}, &AnimationGraphNode::id);
    std::ranges::sort(document.masks, {}, &AnimationGraphMask::id);
    std::ranges::sort(document.sync_groups, {}, &AnimationGraphSyncGroup::id);
    std::ranges::sort(document.editor.nodes, {}, &AnimationGraphNodeLayout::node_id);
    return {std::move(document), "ok", "Animation Graph parsed and normalized."};
    } catch(const Json::exception&) {
        return failure("animation.graph.type-invalid",
            "Animation Graph fields must use the types required by noemancer.animation-graph/0.1.");
    }
}

std::string AnimationGraphCodec::write_canonical_json(const AnimationGraphDocument& document) {
    return document_json(document).dump(2);
}

std::vector<std::string> AnimationGraphCodec::asset_dependencies(const AnimationGraphDocument& document) {
    std::vector<std::string> result;
    for (const auto& node : document.nodes) {
        if (!node.clip_asset.empty()) result.push_back(node.clip_asset);
        if (!node.state_machine_asset.empty()) result.push_back(node.state_machine_asset);
    }
    std::ranges::sort(result); result.erase(std::unique(result.begin(), result.end()), result.end());
    return result;
}

AnimationGraphBlendSelection AnimationGraphCodec::select_blend_1d(const AnimationGraphDocument& document,
    const std::string_view node_id, const std::unordered_map<std::string, float>& parameters) {
    const auto node = std::ranges::find(document.nodes, node_id, &AnimationGraphNode::id);
    if (node == document.nodes.end() || node->kind != "blend-1d") return {false, "animation.graph.blend-node-not-found"};
    const auto parameter = parameters.find(node->parameter);
    if (parameter == parameters.end() || !finite(parameter->second)) return {false, "animation.graph.parameter-not-found"};
    if (parameter->second <= node->children.front().threshold)
        return {true, "ok", node->children.front().node_id, node->children.front().node_id, 1.0F, 0.0F};
    if (parameter->second >= node->children.back().threshold)
        return {true, "ok", node->children.back().node_id, node->children.back().node_id, 1.0F, 0.0F};
    const auto upper = std::ranges::upper_bound(node->children, parameter->second, {}, &AnimationGraphBlendPoint::threshold);
    const auto lower = std::prev(upper);
    const auto alpha = (parameter->second - lower->threshold) / (upper->threshold - lower->threshold);
    return {true, "ok", lower->node_id, upper->node_id, 1.0F - alpha, alpha};
}

bool AnimationGraphLibrary::register_document(AnimationGraphDocument document) {
    const auto parsed=AnimationGraphCodec::parse_json(AnimationGraphCodec::write_canonical_json(document));
    if(!parsed)return false;
    documents_.insert_or_assign(parsed.document->asset_id, std::move(*parsed.document));
    return true;
}

const AnimationGraphDocument* AnimationGraphLibrary::find(const std::string_view asset_id) const {
    const auto found = documents_.find(std::string(asset_id));
    return found == documents_.end() ? nullptr : &found->second;
}

std::string AnimationGraphLibrary::inspect_json(const std::string_view asset_id) const {
    const auto* document = find(asset_id);
    if (document == nullptr) return Json{{"schemaVersion", "noemancer.animation-graph-inspection/0.1"},
        {"valid", false}, {"code", "animation.graph-not-found"}, {"assetId", asset_id}, {"definition", nullptr}}.dump();
    return Json{{"schemaVersion", "noemancer.animation-graph-inspection/0.1"}, {"valid", true}, {"code", "ok"},
        {"assetId", document->asset_id}, {"definition", document_json(*document)},
        {"dependencies", AnimationGraphCodec::asset_dependencies(*document)}}.dump();
}

} // namespace noemancer
