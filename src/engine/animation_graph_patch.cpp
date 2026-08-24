#include "engine/animation_graph_patch.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <exception>
#include <iomanip>
#include <nlohmann/json.hpp>
#include <ranges>
#include <sstream>
#include <utility>

namespace noemancer {
namespace {

using Json = nlohmann::json;

enum class OperationKind {
    unknown,
    set_node_position,
    set_layer_weight,
    set_layer_weight_parameter,
    set_mask_joint_weight,
    create_node,
    delete_node,
    connect_blend_1d_child,
    disconnect_blend_1d_child,
};

constexpr std::uint64_t fnv_offset_basis = 14695981039346656037ULL;
constexpr std::uint64_t fnv_prime = 1099511628211ULL;

bool finite(const float value) noexcept { return std::isfinite(value); }

std::uint64_t fnv1a64(const std::string_view source) noexcept {
    std::uint64_t hash = fnv_offset_basis;
    for (const auto byte : source) {
        hash ^= static_cast<unsigned char>(byte);
        hash *= fnv_prime;
    }
    return hash;
}

std::string hex_u64(const std::uint64_t value) {
    std::ostringstream output;
    output << std::hex << std::setfill('0') << std::setw(16) << value;
    return output.str();
}

std::string text_fingerprint(const std::string_view source) {
    return "fnv1a64:" + hex_u64(fnv1a64(source));
}

OperationKind operation_kind(const std::string_view operation) noexcept {
    if (operation == "setNodePosition" || operation == "set_node_position")
        return OperationKind::set_node_position;
    if (operation == "setLayerWeight" || operation == "set_layer_weight")
        return OperationKind::set_layer_weight;
    if (operation == "setLayerWeightParameter" || operation == "set_layer_weight_parameter")
        return OperationKind::set_layer_weight_parameter;
    if (operation == "setMaskJointWeight" || operation == "set_mask_joint_weight")
        return OperationKind::set_mask_joint_weight;
    if (operation == "createNode" || operation == "create_node")
        return OperationKind::create_node;
    if (operation == "deleteNode" || operation == "delete_node")
        return OperationKind::delete_node;
    if (operation == "connectBlend1DChild" || operation == "connectBlend1dChild" ||
        operation == "connectBlendChild" || operation == "connect_blend_1d_child" ||
        operation == "connect_blend_child")
        return OperationKind::connect_blend_1d_child;
    if (operation == "disconnectBlend1DChild" || operation == "disconnectBlend1dChild" ||
        operation == "disconnectBlendChild" || operation == "disconnect_blend_1d_child" ||
        operation == "disconnect_blend_child")
        return OperationKind::disconnect_blend_1d_child;
    return OperationKind::unknown;
}

std::string_view canonical_operation_name(const OperationKind kind) noexcept {
    switch (kind) {
    case OperationKind::set_node_position: return "setNodePosition";
    case OperationKind::set_layer_weight: return "setLayerWeight";
    case OperationKind::set_layer_weight_parameter: return "setLayerWeightParameter";
    case OperationKind::set_mask_joint_weight: return "setMaskJointWeight";
    case OperationKind::create_node: return "createNode";
    case OperationKind::delete_node: return "deleteNode";
    case OperationKind::connect_blend_1d_child: return "connectBlend1DChild";
    case OperationKind::disconnect_blend_1d_child: return "disconnectBlend1DChild";
    default: return {};
    }
}

Json operation_json(const AnimationGraphPatchOperation& operation) {
    const auto kind = operation_kind(operation.operation);
    switch (kind) {
    case OperationKind::set_node_position:
        return { {"operation", "setNodePosition"}, {"nodeId", operation.node_id},
            {"x", operation.x}, {"y", operation.y} };
    case OperationKind::set_layer_weight:
        return { {"operation", "setLayerWeight"}, {"layerId", operation.layer_id},
            {"weight", operation.weight} };
    case OperationKind::set_layer_weight_parameter:
        return { {"operation", "setLayerWeightParameter"}, {"layerId", operation.layer_id},
            {"parameter", operation.parameter} };
    case OperationKind::set_mask_joint_weight:
        return { {"operation", "setMaskJointWeight"}, {"maskId", operation.mask_id},
            {"jointName", operation.joint_name}, {"weight", operation.weight} };
    case OperationKind::create_node: {
        Json value{{"operation", "createNode"}, {"nodeId", operation.node_id},
            {"kind", operation.node_kind}};
        if (operation.node_kind == "clip") {
            value["clipAsset"] = operation.clip_asset;
            value["looping"] = operation.looping;
        } else if (operation.node_kind == "state-machine") {
            value["stateMachineAsset"] = operation.state_machine_asset;
        } else if (operation.node_kind == "blend-1d") {
            value["parameter"] = operation.parameter;
            value["children"] = Json::array();
            for (const auto& child : operation.children)
                value["children"].push_back({{"nodeId", child.node_id}, {"threshold", child.threshold}});
        }
        return value;
    }
    case OperationKind::delete_node:
        return {{"operation", "deleteNode"}, {"nodeId", operation.node_id}};
    case OperationKind::connect_blend_1d_child:
        return {{"operation", "connectBlend1DChild"}, {"blendNodeId", operation.node_id},
            {"childNodeId", operation.child_node_id}, {"threshold", operation.threshold}};
    case OperationKind::disconnect_blend_1d_child:
        return {{"operation", "disconnectBlend1DChild"}, {"blendNodeId", operation.node_id},
            {"childNodeId", operation.child_node_id}};
    default:
        return { {"operation", operation.operation}, {"nodeId", operation.node_id},
            {"x", operation.x}, {"y", operation.y}, {"layerId", operation.layer_id},
            {"weight", operation.weight}, {"parameter", operation.parameter},
            {"maskId", operation.mask_id}, {"jointName", operation.joint_name},
            {"kind", operation.node_kind}, {"clipAsset", operation.clip_asset},
            {"looping", operation.looping}, {"stateMachineAsset", operation.state_machine_asset},
            {"childNodeId", operation.child_node_id}, {"threshold", operation.threshold} };
    }
}

std::string operation_material(const std::vector<AnimationGraphPatchOperation>& operations) {
    Json encoded = Json::array();
    for (const auto& operation : operations) encoded.push_back(operation_json(operation));
    return encoded.dump();
}

std::string patch_plan_id(const AnimationGraphPatchPlan& plan) {
    const auto material = plan.asset_id + "\n" + plan.base_fingerprint + "\n" +
        plan.result_fingerprint + "\n" + operation_material(plan.operations);
    return "animation-graph-patch-" + hex_u64(fnv1a64(material));
}

void add_issue(AnimationGraphPatchPlan& plan, std::string code, std::string path, std::string detail) {
    if (plan.issues.empty()) {
        plan.code = code;
        plan.detail = detail;
    }
    plan.issues.push_back({std::move(code), std::move(path), std::move(detail)});
}

void add_codec_issue(AnimationGraphPatchPlan& plan, const std::string_view path,
    const AnimationGraphParseResult& parsed, const std::string_view fallback_code,
    const std::string_view fallback_detail) {
    const auto code = parsed.code.empty() ? std::string(fallback_code) : parsed.code;
    const auto detail = parsed.detail.empty() ? std::string(fallback_detail) : parsed.detail;
    add_issue(plan, code, std::string(path), detail);
}

struct StrictDocument final {
    std::optional<AnimationGraphDocument> document;
    std::string code;
    std::string detail;
};

StrictDocument strict_document(const AnimationGraphDocument& document) {
    try {
        const auto parsed = AnimationGraphCodec::parse_json(
            AnimationGraphCodec::write_canonical_json(document));
        if (!parsed) return {std::nullopt, parsed.code, parsed.detail};
        return {std::move(parsed.document), "ok", "Animation Graph passed Codec validation."};
    } catch (const std::exception& exception) {
        return {std::nullopt, "animation.graph.codec-exception", exception.what()};
    }
}

const AnimationGraphNode* find_node(const AnimationGraphDocument& document, const std::string_view id) {
    const auto found = std::ranges::find(document.nodes, id, &AnimationGraphNode::id);
    return found == document.nodes.end() ? nullptr : &*found;
}

AnimationGraphNode* find_node(AnimationGraphDocument& document, const std::string_view id) {
    const auto found = std::ranges::find(document.nodes, id, &AnimationGraphNode::id);
    return found == document.nodes.end() ? nullptr : &*found;
}

AnimationGraphLayer* find_layer(AnimationGraphDocument& document, const std::string_view id) {
    const auto found = std::ranges::find(document.layers, id, &AnimationGraphLayer::id);
    return found == document.layers.end() ? nullptr : &*found;
}

AnimationGraphMask* find_mask(AnimationGraphDocument& document, const std::string_view id) {
    const auto found = std::ranges::find(document.masks, id, &AnimationGraphMask::id);
    return found == document.masks.end() ? nullptr : &*found;
}

const AnimationGraphParameter* find_parameter(const AnimationGraphDocument& document,
    const std::string_view id) {
    const auto found = std::ranges::find(document.parameters, id, &AnimationGraphParameter::id);
    return found == document.parameters.end() ? nullptr : &*found;
}

AnimationGraphNodeLayout* find_layout(AnimationGraphDocument& document, const std::string_view id) {
    const auto found = std::ranges::find(document.editor.nodes, id, &AnimationGraphNodeLayout::node_id);
    return found == document.editor.nodes.end() ? nullptr : &*found;
}

bool same_float(const float left, const float right) noexcept {
    // Values are copied, not recomputed, so exact equality is the stable
    // no-op test and treats signed zero as an equivalent authoring value.
    return left == right;
}

bool has_node_reference(const AnimationGraphDocument& document, const std::string_view node_id) {
    for (const auto& layer : document.layers)
        if (layer.root_node == node_id) return true;
    for (const auto& node : document.nodes)
        for (const auto& child : node.children)
            if (child.node_id == node_id) return true;
    return false;
}

bool has_blend_child(const AnimationGraphNode& blend, const std::string_view child_id) {
    return std::ranges::find(blend.children, child_id, &AnimationGraphBlendPoint::node_id) !=
        blend.children.end();
}

bool has_blend_threshold(const AnimationGraphNode& blend, const float threshold) {
    return std::ranges::find(blend.children, threshold, &AnimationGraphBlendPoint::threshold) !=
        blend.children.end();
}

bool valid_blend_child_list(const std::vector<AnimationGraphBlendPoint>& children) {
    if (children.size() > animation_graph_patch_max_children) return false;
    std::vector<std::string> ids;
    ids.reserve(children.size());
    for (std::size_t index = 0; index < children.size(); ++index) {
        const auto& child = children[index];
        if (child.node_id.empty() || !finite(child.threshold)) return false;
        if (std::ranges::find(ids, child.node_id) != ids.end()) return false;
        ids.push_back(child.node_id);
        if (index > 0U && children[index - 1].threshold >= child.threshold) return false;
    }
    return true;
}

void append_operation_issue(AnimationGraphPatchPlan& plan, const std::size_t index,
    std::string code, std::string field, std::string detail) {
    add_issue(plan, std::move(code), "/operations/" + std::to_string(index) + "/" + std::move(field),
        std::move(detail));
}

bool apply_operation(AnimationGraphDocument& candidate,
    const AnimationGraphPatchOperation& operation, const std::size_t index,
    AnimationGraphPatchPlan& plan) {
    const auto kind = operation_kind(operation.operation);
    if (kind == OperationKind::unknown) {
        append_operation_issue(plan, index, "animation.graph.patch-operation-unsupported", "operation",
            "Operation must be one of the supported layout, layer, mask, or topology operations.");
        return false;
    }

    switch (kind) {
    case OperationKind::set_node_position: {
        if (operation.node_id.empty()) {
            append_operation_issue(plan, index, "animation.graph.patch-node-id-required", "nodeId",
                "setNodePosition requires a non-empty nodeId.");
            return false;
        }
        if (!finite(operation.x) || !finite(operation.y) ||
            std::abs(operation.x)>animation_graph_editor_coordinate_limit ||
            std::abs(operation.y)>animation_graph_editor_coordinate_limit) {
            append_operation_issue(plan, index, "animation.graph.patch-position-invalid", "position",
                "Node positions must be finite and bounded.");
            return false;
        }
        if (find_node(candidate, operation.node_id) == nullptr) {
            append_operation_issue(plan, index, "animation.graph.patch-node-not-found", "nodeId",
                "setNodePosition targets an unknown graph node.");
            return false;
        }
        auto* layout = find_layout(candidate, operation.node_id);
        if (layout == nullptr) {
            candidate.editor.nodes.push_back({operation.node_id, operation.x, operation.y, false});
            ++plan.changed_operation_count;
            return true;
        }
        if (!same_float(layout->x, operation.x) || !same_float(layout->y, operation.y)) {
            layout->x = operation.x;
            layout->y = operation.y;
            ++plan.changed_operation_count;
        }
        return true;
    }
    case OperationKind::set_layer_weight: {
        if (operation.layer_id.empty()) {
            append_operation_issue(plan, index, "animation.graph.patch-layer-id-required", "layerId",
                "setLayerWeight requires a non-empty layerId.");
            return false;
        }
        if (!finite(operation.weight) || operation.weight < 0.0F || operation.weight > 1.0F) {
            append_operation_issue(plan, index, "animation.graph.patch-layer-weight-invalid", "weight",
                "Layer weight must be finite and in [0,1].");
            return false;
        }
        auto* layer = find_layer(candidate, operation.layer_id);
        if (layer == nullptr) {
            append_operation_issue(plan, index, "animation.graph.patch-layer-not-found", "layerId",
                "setLayerWeight targets an unknown graph layer.");
            return false;
        }
        if (!same_float(layer->weight, operation.weight)) {
            layer->weight = operation.weight;
            ++plan.changed_operation_count;
        }
        return true;
    }
    case OperationKind::set_layer_weight_parameter: {
        if (operation.layer_id.empty()) {
            append_operation_issue(plan, index, "animation.graph.patch-layer-id-required", "layerId",
                "setLayerWeightParameter requires a non-empty layerId.");
            return false;
        }
        auto* layer = find_layer(candidate, operation.layer_id);
        if (layer == nullptr) {
            append_operation_issue(plan, index, "animation.graph.patch-layer-not-found", "layerId",
                "setLayerWeightParameter targets an unknown graph layer.");
            return false;
        }
        if (!operation.parameter.empty()) {
            const auto* parameter = find_parameter(candidate, operation.parameter);
            if (parameter == nullptr || parameter->type != "float") {
                append_operation_issue(plan, index, "animation.graph.patch-weight-parameter-invalid", "parameter",
                    "A non-empty layer weight parameter must reference a float graph parameter.");
                return false;
            }
        }
        if (layer->weight_parameter != operation.parameter) {
            layer->weight_parameter = operation.parameter;
            ++plan.changed_operation_count;
        }
        return true;
    }
    case OperationKind::set_mask_joint_weight: {
        if (operation.mask_id.empty()) {
            append_operation_issue(plan, index, "animation.graph.patch-mask-id-required", "maskId",
                "setMaskJointWeight requires a non-empty maskId.");
            return false;
        }
        if (operation.joint_name.empty()) {
            append_operation_issue(plan, index, "animation.graph.patch-joint-name-required", "jointName",
                "setMaskJointWeight requires a non-empty jointName.");
            return false;
        }
        if (!finite(operation.weight) || operation.weight < 0.0F || operation.weight > 1.0F) {
            append_operation_issue(plan, index, "animation.graph.patch-joint-weight-invalid", "weight",
                "Mask joint weight must be finite and in [0,1].");
            return false;
        }
        auto* mask = find_mask(candidate, operation.mask_id);
        if (mask == nullptr) {
            append_operation_issue(plan, index, "animation.graph.patch-mask-not-found", "maskId",
                "setMaskJointWeight targets an unknown graph mask.");
            return false;
        }
        const auto joint = std::ranges::find(mask->joints, operation.joint_name,
            &AnimationGraphMaskJoint::name);
        if (joint == mask->joints.end()) {
            append_operation_issue(plan, index, "animation.graph.patch-joint-not-found", "jointName",
                "setMaskJointWeight targets an unknown joint in the graph mask.");
            return false;
        }
        if (!same_float(joint->weight, operation.weight)) {
            joint->weight = operation.weight;
            ++plan.changed_operation_count;
        }
        return true;
    }
    case OperationKind::create_node: {
        if (operation.node_id.empty()) {
            append_operation_issue(plan, index, "animation.graph.patch-node-id-required", "nodeId",
                "createNode requires a non-empty nodeId.");
            return false;
        }
        if (operation.node_kind != "clip" && operation.node_kind != "state-machine" &&
            operation.node_kind != "blend-1d") {
            append_operation_issue(plan, index, "animation.graph.patch-node-kind-invalid", "kind",
                "createNode kind must be clip, state-machine, or blend-1d.");
            return false;
        }
        if (find_node(candidate, operation.node_id) != nullptr) {
            append_operation_issue(plan, index, "animation.graph.patch-node-already-exists", "nodeId",
                "createNode cannot reuse an existing stable node ID.");
            return false;
        }
        if (operation.node_kind == "clip") {
            if (operation.clip_asset.empty()) {
                append_operation_issue(plan, index, "animation.graph.patch-clip-asset-required", "clipAsset",
                    "Clip nodes require a non-empty clipAsset.");
                return false;
            }
            if (!operation.state_machine_asset.empty() || !operation.parameter.empty() ||
                !operation.children.empty()) {
                append_operation_issue(plan, index, "animation.graph.patch-node-type-fields-invalid", "kind",
                    "Clip nodes cannot carry state-machine, Blend 1D, or child fields.");
                return false;
            }
        } else if (operation.node_kind == "state-machine") {
            if (operation.state_machine_asset.empty()) {
                append_operation_issue(plan, index, "animation.graph.patch-state-machine-asset-required",
                    "stateMachineAsset", "State-machine nodes require a non-empty stateMachineAsset.");
                return false;
            }
            if (!operation.clip_asset.empty() || !operation.parameter.empty() ||
                !operation.children.empty()) {
                append_operation_issue(plan, index, "animation.graph.patch-node-type-fields-invalid", "kind",
                    "State-machine nodes cannot carry clip, Blend 1D, or child fields.");
                return false;
            }
        } else {
            const auto* parameter = find_parameter(candidate, operation.parameter);
            if (parameter == nullptr || parameter->type != "float") {
                append_operation_issue(plan, index, "animation.graph.patch-blend-parameter-invalid", "parameter",
                    "Blend 1D nodes require an existing float graph parameter.");
                return false;
            }
            if (!operation.clip_asset.empty() || !operation.state_machine_asset.empty()) {
                append_operation_issue(plan, index, "animation.graph.patch-node-type-fields-invalid", "kind",
                    "Blend 1D nodes cannot carry clip or state-machine asset fields.");
                return false;
            }
            if (!valid_blend_child_list(operation.children)) {
                append_operation_issue(plan, index, "animation.graph.patch-blend-children-invalid", "children",
                    "Initial Blend 1D children must have unique finite IDs and strictly increasing thresholds.");
                return false;
            }
        }

        AnimationGraphNode node;
        node.id = operation.node_id;
        node.kind = operation.node_kind;
        node.clip_asset = operation.clip_asset;
        node.looping = operation.looping;
        node.state_machine_asset = operation.state_machine_asset;
        node.parameter = operation.parameter;
        node.children = operation.children;
        candidate.nodes.push_back(std::move(node));
        ++plan.changed_operation_count;
        return true;
    }
    case OperationKind::delete_node: {
        if (operation.node_id.empty()) {
            append_operation_issue(plan, index, "animation.graph.patch-node-id-required", "nodeId",
                "deleteNode requires a non-empty nodeId.");
            return false;
        }
        if (find_node(candidate, operation.node_id) == nullptr) {
            append_operation_issue(plan, index, "animation.graph.patch-node-not-found", "nodeId",
                "deleteNode targets an unknown graph node.");
            return false;
        }
        if (has_node_reference(candidate, operation.node_id)) {
            append_operation_issue(plan, index, "animation.graph.patch-node-in-use", "nodeId",
                "deleteNode requires all layer roots and Blend 1D child references to be disconnected first.");
            return false;
        }
        candidate.nodes.erase(std::remove_if(candidate.nodes.begin(), candidate.nodes.end(),
            [&](const AnimationGraphNode& node) { return node.id == operation.node_id; }), candidate.nodes.end());
        candidate.editor.nodes.erase(std::remove_if(candidate.editor.nodes.begin(), candidate.editor.nodes.end(),
            [&](const AnimationGraphNodeLayout& layout) { return layout.node_id == operation.node_id; }),
            candidate.editor.nodes.end());
        ++plan.changed_operation_count;
        return true;
    }
    case OperationKind::connect_blend_1d_child: {
        if (operation.node_id.empty()) {
            append_operation_issue(plan, index, "animation.graph.patch-blend-node-id-required", "blendNodeId",
                "connectBlend1DChild requires a non-empty blendNodeId.");
            return false;
        }
        if (operation.child_node_id.empty()) {
            append_operation_issue(plan, index, "animation.graph.patch-child-node-id-required", "childNodeId",
                "connectBlend1DChild requires a non-empty childNodeId.");
            return false;
        }
        if (!finite(operation.threshold)) {
            append_operation_issue(plan, index, "animation.graph.patch-threshold-invalid", "threshold",
                "Blend 1D thresholds must be finite.");
            return false;
        }
        auto* blend = find_node(candidate, operation.node_id);
        if (blend == nullptr) {
            append_operation_issue(plan, index, "animation.graph.patch-node-not-found", "blendNodeId",
                "connectBlend1DChild targets an unknown Blend 1D node.");
            return false;
        }
        if (blend->kind != "blend-1d") {
            append_operation_issue(plan, index, "animation.graph.patch-blend-node-invalid", "blendNodeId",
                "connectBlend1DChild requires a Blend 1D source node.");
            return false;
        }
        const auto* child = find_node(candidate, operation.child_node_id);
        if (child == nullptr) {
            append_operation_issue(plan, index, "animation.graph.patch-node-not-found", "childNodeId",
                "connectBlend1DChild targets an unknown child node.");
            return false;
        }
        if (child->kind != "clip") {
            append_operation_issue(plan, index, "animation.graph.patch-blend-child-invalid", "childNodeId",
                "Animation Graph 0.1 Blend 1D children must be terminal clip nodes.");
            return false;
        }
        if (has_blend_child(*blend, operation.child_node_id)) {
            append_operation_issue(plan, index, "animation.graph.patch-blend-child-already-connected", "childNodeId",
                "The Blend 1D child connection already exists.");
            return false;
        }
        if (has_blend_threshold(*blend, operation.threshold)) {
            append_operation_issue(plan, index, "animation.graph.patch-blend-threshold-duplicate", "threshold",
                "Blend 1D thresholds must remain strictly increasing and unique.");
            return false;
        }
        if (blend->children.size() >= 32U) {
            append_operation_issue(plan, index, "animation.graph.patch-blend-child-limit", "blendNodeId",
                "A Blend 1D node cannot contain more than 32 children.");
            return false;
        }
        const auto insertion = std::ranges::find_if(blend->children,
            [&](const AnimationGraphBlendPoint& point) { return point.threshold > operation.threshold; });
        blend->children.insert(insertion, {operation.child_node_id, operation.threshold});
        ++plan.changed_operation_count;
        return true;
    }
    case OperationKind::disconnect_blend_1d_child: {
        if (operation.node_id.empty()) {
            append_operation_issue(plan, index, "animation.graph.patch-blend-node-id-required", "blendNodeId",
                "disconnectBlend1DChild requires a non-empty blendNodeId.");
            return false;
        }
        if (operation.child_node_id.empty()) {
            append_operation_issue(plan, index, "animation.graph.patch-child-node-id-required", "childNodeId",
                "disconnectBlend1DChild requires a non-empty childNodeId.");
            return false;
        }
        auto* blend = find_node(candidate, operation.node_id);
        if (blend == nullptr) {
            append_operation_issue(plan, index, "animation.graph.patch-node-not-found", "blendNodeId",
                "disconnectBlend1DChild targets an unknown Blend 1D node.");
            return false;
        }
        if (blend->kind != "blend-1d") {
            append_operation_issue(plan, index, "animation.graph.patch-blend-node-invalid", "blendNodeId",
                "disconnectBlend1DChild requires a Blend 1D source node.");
            return false;
        }
        const auto child = std::ranges::find(blend->children, operation.child_node_id,
            &AnimationGraphBlendPoint::node_id);
        if (child == blend->children.end()) {
            append_operation_issue(plan, index, "animation.graph.patch-blend-child-not-found", "childNodeId",
                "The Blend 1D child connection does not exist.");
            return false;
        }
        blend->children.erase(child);
        ++plan.changed_operation_count;
        return true;
    }
    default:
        return false;
    }
}

void canonicalize_operations(AnimationGraphPatchPlan& plan) {
    for (auto& operation : plan.operations) {
        const auto kind = operation_kind(operation.operation);
        if (kind != OperationKind::unknown)
            operation.operation = std::string(canonical_operation_name(kind));
    }
}

Json candidate_json(const std::optional<AnimationGraphDocument>& document) {
    if (!document) return nullptr;
    try {
        return Json::parse(AnimationGraphCodec::write_canonical_json(*document));
    } catch (...) {
        return nullptr;
    }
}

Json issue_json(const AnimationGraphPatchIssue& issue) {
    return {{"code", issue.code}, {"path", issue.path}, {"detail", issue.detail}};
}

} // namespace

AnimationGraphPatchOperation AnimationGraphPatchOperation::set_node_position(
    std::string node_id, const float x, const float y) {
    AnimationGraphPatchOperation operation;
    operation.operation = "setNodePosition";
    operation.node_id = std::move(node_id);
    operation.x = x;
    operation.y = y;
    return operation;
}

AnimationGraphPatchOperation AnimationGraphPatchOperation::set_layer_weight(
    std::string layer_id, const float weight) {
    AnimationGraphPatchOperation operation;
    operation.operation = "setLayerWeight";
    operation.layer_id = std::move(layer_id);
    operation.weight = weight;
    return operation;
}

AnimationGraphPatchOperation AnimationGraphPatchOperation::set_layer_weight_parameter(
    std::string layer_id, std::string parameter) {
    AnimationGraphPatchOperation operation;
    operation.operation = "setLayerWeightParameter";
    operation.layer_id = std::move(layer_id);
    operation.parameter = std::move(parameter);
    return operation;
}

AnimationGraphPatchOperation AnimationGraphPatchOperation::set_mask_joint_weight(
    std::string mask_id, std::string joint_name, const float weight) {
    AnimationGraphPatchOperation operation;
    operation.operation = "setMaskJointWeight";
    operation.mask_id = std::move(mask_id);
    operation.joint_name = std::move(joint_name);
    operation.weight = weight;
    return operation;
}

AnimationGraphPatchOperation AnimationGraphPatchOperation::create_node(
    std::string node_id, std::string kind, std::string clip_asset, const bool looping,
    std::string state_machine_asset, std::string parameter) {
    AnimationGraphPatchOperation operation;
    operation.operation = "createNode";
    operation.node_id = std::move(node_id);
    operation.node_kind = std::move(kind);
    operation.clip_asset = std::move(clip_asset);
    operation.looping = looping;
    operation.state_machine_asset = std::move(state_machine_asset);
    operation.parameter = std::move(parameter);
    return operation;
}

AnimationGraphPatchOperation AnimationGraphPatchOperation::create_node(AnimationGraphNode node) {
    AnimationGraphPatchOperation operation = create_node(std::move(node.id), std::move(node.kind),
        std::move(node.clip_asset), node.looping, std::move(node.state_machine_asset),
        std::move(node.parameter));
    operation.children = std::move(node.children);
    return operation;
}

AnimationGraphPatchOperation AnimationGraphPatchOperation::create_clip_node(
    std::string node_id, std::string clip_asset, const bool looping) {
    return create_node(std::move(node_id), "clip", std::move(clip_asset), looping);
}

AnimationGraphPatchOperation AnimationGraphPatchOperation::create_state_machine_node(
    std::string node_id, std::string state_machine_asset) {
    return create_node(std::move(node_id), "state-machine", {}, true,
        std::move(state_machine_asset));
}

AnimationGraphPatchOperation AnimationGraphPatchOperation::create_blend_1d_node(
    std::string node_id, std::string parameter, std::vector<AnimationGraphBlendPoint> children) {
    AnimationGraphPatchOperation operation = create_node(std::move(node_id), "blend-1d", {}, true,
        {}, std::move(parameter));
    operation.children = std::move(children);
    return operation;
}

AnimationGraphPatchOperation AnimationGraphPatchOperation::delete_node(std::string node_id) {
    AnimationGraphPatchOperation operation;
    operation.operation = "deleteNode";
    operation.node_id = std::move(node_id);
    return operation;
}

AnimationGraphPatchOperation AnimationGraphPatchOperation::connect_blend_1d_child(
    std::string blend_node_id, std::string child_node_id, const float threshold) {
    AnimationGraphPatchOperation operation;
    operation.operation = "connectBlend1DChild";
    operation.node_id = std::move(blend_node_id);
    operation.child_node_id = std::move(child_node_id);
    operation.threshold = threshold;
    return operation;
}

AnimationGraphPatchOperation AnimationGraphPatchOperation::disconnect_blend_1d_child(
    std::string blend_node_id, std::string child_node_id) {
    AnimationGraphPatchOperation operation;
    operation.operation = "disconnectBlend1DChild";
    operation.node_id = std::move(blend_node_id);
    operation.child_node_id = std::move(child_node_id);
    return operation;
}

std::string AnimationGraphPatch::fingerprint(const AnimationGraphDocument& document) {
    try {
        return text_fingerprint(AnimationGraphCodec::write_canonical_json(document));
    } catch (...) {
        // A fingerprint is also used on failure receipts.  Keep that path
        // deterministic even when a caller hands the transaction an invalid
        // document containing a value the JSON writer cannot encode.
        return text_fingerprint("animation-graph-invalid\n" + document.asset_id);
    }
}

AnimationGraphPatchPlan AnimationGraphPatch::plan(
    const AnimationGraphDocument& document,
    std::vector<AnimationGraphPatchOperation> operations,
    const std::string_view expected_fingerprint) {
    AnimationGraphPatchPlan plan_result;
    plan_result.code = "animation.graph.patch-invalid";
    plan_result.detail = "Animation Graph patch has not been validated.";
    plan_result.asset_id = document.asset_id;
    plan_result.requested_operation_count = operations.size();
    plan_result.operations = std::move(operations);
    canonicalize_operations(plan_result);

    if (plan_result.requested_operation_count == 0U) {
        add_issue(plan_result, "animation.graph.patch-empty", "/operations",
            "A Graph patch must contain at least one operation.");
        return plan_result;
    }
    if (plan_result.requested_operation_count > animation_graph_patch_max_operations) {
        add_issue(plan_result, "animation.graph.patch-size", "/operations",
            "A Graph patch contains more operations than the bounded transaction limit.");
        return plan_result;
    }

    std::size_t operation_bytes{};
    for (std::size_t index = 0; index < plan_result.operations.size(); ++index) {
        const auto& operation = plan_result.operations[index];
        if (operation.children.size() > animation_graph_patch_max_children) {
            append_operation_issue(plan_result, index, "animation.graph.patch-child-limit", "children",
                "An Animation Graph operation cannot contain more than 32 children.");
            return plan_result;
        }
        const auto consume_string = [&](const std::string_view field, const std::string_view value) {
            if (value.size() > animation_graph_patch_max_string_bytes) {
                append_operation_issue(plan_result, index, "animation.graph.patch-string-too-large",
                    std::string(field), "Animation Graph patch strings cannot exceed 4096 bytes.");
                return false;
            }
            if (operation_bytes > animation_graph_patch_max_input_bytes - value.size()) {
                add_issue(plan_result, "animation.graph.patch-input-too-large", "/operations",
                    "Serialized Animation Graph patch material exceeds the 1 MiB transaction limit.");
                return false;
            }
            operation_bytes += value.size();
            return true;
        };
        if (!consume_string("operation", operation.operation) ||
            !consume_string("nodeId", operation.node_id) ||
            !consume_string("layerId", operation.layer_id) ||
            !consume_string("parameter", operation.parameter) ||
            !consume_string("maskId", operation.mask_id) ||
            !consume_string("jointName", operation.joint_name) ||
            !consume_string("kind", operation.node_kind) ||
            !consume_string("clipAsset", operation.clip_asset) ||
            !consume_string("stateMachineAsset", operation.state_machine_asset) ||
            !consume_string("childNodeId", operation.child_node_id)) return plan_result;
        for (const auto& child : operation.children)
            if (!consume_string("children/nodeId", child.node_id)) return plan_result;
    }

    const auto strict_base = strict_document(document);
    if (!strict_base.document) {
        AnimationGraphParseResult parsed{std::nullopt, strict_base.code, strict_base.detail};
        add_codec_issue(plan_result, "/base", parsed, "animation.graph.patch-base-invalid",
            "The base Animation Graph failed Codec validation.");
        plan_result.code = "animation.graph.patch-base-invalid";
        plan_result.detail = "The base Animation Graph failed Codec validation.";
        return plan_result;
    }

    const auto& base = *strict_base.document;
    plan_result.asset_id = base.asset_id;
    plan_result.base_fingerprint = fingerprint(base);
    if (!expected_fingerprint.empty() && expected_fingerprint != plan_result.base_fingerprint) {
        add_issue(plan_result, "animation.graph.patch-conflict", "/expectedFingerprint",
            "The Animation Graph fingerprint changed since the caller observed it.");
        return plan_result;
    }

    AnimationGraphDocument candidate = base;
    for (std::size_t index = 0; index < plan_result.operations.size(); ++index)
        static_cast<void>(apply_operation(candidate, plan_result.operations[index], index, plan_result));
    if (!plan_result.issues.empty()) return plan_result;

    const auto strict_candidate = strict_document(candidate);
    if (!strict_candidate.document) {
        AnimationGraphParseResult parsed{std::nullopt, strict_candidate.code, strict_candidate.detail};
        add_codec_issue(plan_result, "/candidate", parsed, "animation.graph.patch-candidate-invalid",
            "The candidate Animation Graph failed Codec validation.");
        plan_result.code = "animation.graph.patch-candidate-invalid";
        plan_result.detail = "The candidate Animation Graph failed Codec validation.";
        return plan_result;
    }
    if (strict_candidate.document->asset_id != base.asset_id) {
        add_issue(plan_result, "animation.graph.patch-protected-identity", "/assetId",
            "Animation Graph assetId is immutable during a patch transaction.");
        return plan_result;
    }

    plan_result.result = *strict_candidate.document;
    plan_result.result_fingerprint = fingerprint(*plan_result.result);
    if (plan_result.result_fingerprint == plan_result.base_fingerprint) {
        plan_result.code = "animation.graph.patch-no-op";
        plan_result.detail = "The patch is valid but does not change the canonical Graph.";
    } else {
        plan_result.code = "ok";
        plan_result.detail = "Animation Graph patch candidate passed Codec validation.";
    }
    plan_result.plan_id = patch_plan_id(plan_result);
    plan_result.valid = true;
    return plan_result;
}

AnimationGraphPatchPlan AnimationGraphPatch::plan_source(
    const std::string_view source,
    std::vector<AnimationGraphPatchOperation> operations,
    const std::string_view expected_fingerprint) {
    AnimationGraphParseResult parsed;
    try {
        parsed = AnimationGraphCodec::parse_json(source);
    } catch (const std::exception& exception) {
        parsed = {std::nullopt, "animation.graph.codec-exception", exception.what()};
    }
    if (!parsed) {
        AnimationGraphPatchPlan result;
        result.code = "animation.graph.patch-source-invalid";
        result.detail = "The source Animation Graph failed Codec validation.";
        result.requested_operation_count = operations.size();
        result.operations = std::move(operations);
        canonicalize_operations(result);
        add_codec_issue(result, "/source", parsed, "animation.graph.patch-source-invalid",
            "The source Animation Graph failed Codec validation.");
        result.code = "animation.graph.patch-source-invalid";
        result.detail = "The source Animation Graph failed Codec validation.";
        return result;
    }
    return plan(*parsed.document, std::move(operations), expected_fingerprint);
}

AnimationGraphPatchReceipt AnimationGraphPatch::apply(
    AnimationGraphDocument& document,
    const AnimationGraphPatchPlan& plan_result,
    const bool dry_run) {
    AnimationGraphPatchReceipt receipt;
    receipt.dry_run = dry_run;
    receipt.plan_id = plan_result.plan_id;
    receipt.asset_id = document.asset_id;
    receipt.operation_count = plan_result.requested_operation_count;

    const auto strict_current = strict_document(document);
    if (strict_current.document) receipt.fingerprint_before = fingerprint(*strict_current.document);
    else receipt.fingerprint_before = fingerprint(document);

    auto fail = [&](std::string code, std::string detail) {
        receipt.success = false;
        receipt.code = std::move(code);
        receipt.detail = std::move(detail);
        return receipt;
    };

    if (!plan_result.valid || !plan_result.result)
        return fail(plan_result.code.empty() ? "animation.graph.patch-invalid" : plan_result.code,
            plan_result.detail.empty() ? "The Graph patch plan is invalid." : plan_result.detail);
    if (plan_result.requested_operation_count == 0U ||
        plan_result.requested_operation_count > animation_graph_patch_max_operations ||
        plan_result.operations.size() != plan_result.requested_operation_count)
        return fail("animation.graph.patch-integrity-error", "The Graph patch operation count is invalid.");
    if (!strict_current.document)
        return fail("animation.graph.patch-current-invalid", "The current Animation Graph failed Codec validation.");
    if (plan_result.asset_id != strict_current.document->asset_id)
        return fail("animation.graph.patch-conflict", "The current Animation Graph assetId does not match the plan.");
    if (receipt.fingerprint_before != plan_result.base_fingerprint)
        return fail("animation.graph.patch-conflict", "The current Animation Graph fingerprint does not match the plan.");

    const auto strict_result = strict_document(*plan_result.result);
    if (!strict_result.document || strict_result.document->asset_id != strict_current.document->asset_id)
        return fail("animation.graph.patch-integrity-error", "The patch candidate failed identity or Codec integrity checks.");
    const auto result_fingerprint = fingerprint(*strict_result.document);
    if (result_fingerprint != plan_result.result_fingerprint ||
        plan_result.plan_id != patch_plan_id(plan_result))
        return fail("animation.graph.patch-integrity-error", "The Graph patch plan fingerprint or ID is invalid.");

    receipt.success = true;
    receipt.code = dry_run ? "animation.graph.patch-dry-run" : "ok";
    receipt.detail = dry_run ? "Dry run passed; Animation Graph state was not changed."
                             : "Animation Graph patch applied atomically to the caller-owned document.";
    receipt.asset_id = strict_current.document->asset_id;
    receipt.fingerprint_after = result_fingerprint;
    if (!dry_run) document = *strict_result.document;
    return receipt;
}

std::string AnimationGraphPatchPlan::to_json() const {
    return AnimationGraphPatch::plan_json(*this);
}

std::string AnimationGraphPatchReceipt::to_json() const {
    return AnimationGraphPatch::receipt_json(*this);
}

std::string AnimationGraphPatch::plan_json(const AnimationGraphPatchPlan& plan_result) {
    Json operations = Json::array();
    for (const auto& operation : plan_result.operations) operations.push_back(operation_json(operation));
    Json issues = Json::array();
    for (const auto& issue : plan_result.issues) issues.push_back(issue_json(issue));
    return Json{
        {"schemaVersion", animation_graph_patch_plan_schema},
        {"valid", plan_result.valid},
        {"code", plan_result.code},
        {"detail", plan_result.detail},
        {"planId", plan_result.plan_id},
        {"assetId", plan_result.asset_id},
        {"baseFingerprint", plan_result.base_fingerprint},
        {"resultFingerprint", plan_result.result_fingerprint},
        {"requestedOperationCount", plan_result.requested_operation_count},
        {"changedOperationCount", plan_result.changed_operation_count},
        {"operations", std::move(operations)},
        {"candidate", candidate_json(plan_result.result)},
        {"issues", std::move(issues)},
    }.dump();
}

std::string AnimationGraphPatch::receipt_json(const AnimationGraphPatchReceipt& receipt) {
    return Json{
        {"schemaVersion", animation_graph_patch_receipt_schema},
        {"success", receipt.success},
        {"dryRun", receipt.dry_run},
        {"code", receipt.code},
        {"detail", receipt.detail},
        {"planId", receipt.plan_id},
        {"assetId", receipt.asset_id},
        {"fingerprintBefore", receipt.fingerprint_before},
        {"fingerprintAfter", receipt.fingerprint_after},
        {"operationCount", receipt.operation_count},
    }.dump();
}

std::string animation_graph_fingerprint(const AnimationGraphDocument& document) {
    return AnimationGraphPatch::fingerprint(document);
}

AnimationGraphPatchPlan plan_animation_graph_patch(
    const AnimationGraphDocument& document,
    std::vector<AnimationGraphPatchOperation> operations,
    const std::string_view expected_fingerprint) {
    return AnimationGraphPatch::plan(document, std::move(operations), expected_fingerprint);
}

AnimationGraphPatchReceipt apply_animation_graph_patch(
    AnimationGraphDocument& document,
    const AnimationGraphPatchPlan& plan_result,
    const bool dry_run) {
    return AnimationGraphPatch::apply(document, plan_result, dry_run);
}

} // namespace noemancer
