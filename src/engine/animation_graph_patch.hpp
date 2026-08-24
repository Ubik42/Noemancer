#pragma once

#include "engine/animation_graph.hpp"

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace noemancer {

// Animation Graph authoring is intentionally a separate boundary from the
// Codec and from the Editor.  The patch schema describes only the small set
// of fields that this transaction can author; schemaVersion and assetId are
// never patch targets.
inline constexpr std::string_view animation_graph_patch_schema =
    "noemancer.animation-graph-patch/0.2";
inline constexpr std::string_view animation_graph_patch_plan_schema =
    "noemancer.animation-graph-patch-plan/0.2";
inline constexpr std::string_view animation_graph_patch_receipt_schema =
    "noemancer.animation-graph-patch-receipt/0.2";
inline constexpr std::size_t animation_graph_patch_max_operations = 256;
inline constexpr std::size_t animation_graph_patch_max_children = 32;
inline constexpr std::size_t animation_graph_patch_max_string_bytes = 4096;
inline constexpr std::size_t animation_graph_patch_max_input_bytes = 1024U * 1024U;

struct AnimationGraphPatchOperation final {
    // Canonical values are setNodePosition, setLayerWeight,
    // setLayerWeightParameter, setMaskJointWeight, createNode, deleteNode,
    // connectBlend1DChild and disconnectBlend1DChild.  Keeping this as plain
    // data makes the operation safe to carry across an ABI or JSON adapter.
    std::string operation;

    std::string node_id;
    float x{};
    float y{};

    std::string layer_id;
    float weight{};
    std::string parameter;

    std::string mask_id;
    std::string joint_name;

    // Topology fields are appended so existing aggregate construction of the
    // original layout/layer/mask operations remains source-compatible.
    std::string node_kind;
    std::string clip_asset;
    bool looping{true};
    std::string state_machine_asset;
    std::string child_node_id;
    float threshold{};
    std::vector<AnimationGraphBlendPoint> children;

    [[nodiscard]] static AnimationGraphPatchOperation set_node_position(
        std::string node_id, float x, float y);
    [[nodiscard]] static AnimationGraphPatchOperation set_layer_weight(
        std::string layer_id, float weight);
    [[nodiscard]] static AnimationGraphPatchOperation set_layer_weight_parameter(
        std::string layer_id, std::string parameter);
    [[nodiscard]] static AnimationGraphPatchOperation set_mask_joint_weight(
        std::string mask_id, std::string joint_name, float weight);
    [[nodiscard]] static AnimationGraphPatchOperation create_node(
        std::string node_id, std::string kind, std::string clip_asset = {},
        bool looping = true, std::string state_machine_asset = {},
        std::string parameter = {});
    [[nodiscard]] static AnimationGraphPatchOperation create_node(AnimationGraphNode node);
    [[nodiscard]] static AnimationGraphPatchOperation create_clip_node(
        std::string node_id, std::string clip_asset, bool looping = true);
    [[nodiscard]] static AnimationGraphPatchOperation create_state_machine_node(
        std::string node_id, std::string state_machine_asset);
    [[nodiscard]] static AnimationGraphPatchOperation create_blend_1d_node(
        std::string node_id, std::string parameter,
        std::vector<AnimationGraphBlendPoint> children = {});
    [[nodiscard]] static AnimationGraphPatchOperation create_blend1d_node(
        std::string node_id, std::string parameter,
        std::vector<AnimationGraphBlendPoint> children = {}) {
        return create_blend_1d_node(std::move(node_id), std::move(parameter), std::move(children));
    }
    [[nodiscard]] static AnimationGraphPatchOperation delete_node(std::string node_id);
    [[nodiscard]] static AnimationGraphPatchOperation connect_blend_1d_child(
        std::string blend_node_id, std::string child_node_id, float threshold);
    [[nodiscard]] static AnimationGraphPatchOperation connect_blend1d_child(
        std::string blend_node_id, std::string child_node_id, float threshold) {
        return connect_blend_1d_child(std::move(blend_node_id), std::move(child_node_id), threshold);
    }
    [[nodiscard]] static AnimationGraphPatchOperation disconnect_blend_1d_child(
        std::string blend_node_id, std::string child_node_id);
    [[nodiscard]] static AnimationGraphPatchOperation disconnect_blend1d_child(
        std::string blend_node_id, std::string child_node_id) {
        return disconnect_blend_1d_child(std::move(blend_node_id), std::move(child_node_id));
    }

    // Camel-case aliases mirror the serialized operation names and are useful
    // to adapters which already expose the Graph authoring vocabulary.
    [[nodiscard]] static AnimationGraphPatchOperation setNodePosition(
        std::string node_id, float x, float y) {
        return set_node_position(std::move(node_id), x, y);
    }
    [[nodiscard]] static AnimationGraphPatchOperation setLayerWeight(
        std::string layer_id, float weight) {
        return set_layer_weight(std::move(layer_id), weight);
    }
    [[nodiscard]] static AnimationGraphPatchOperation setLayerWeightParameter(
        std::string layer_id, std::string parameter) {
        return set_layer_weight_parameter(std::move(layer_id), std::move(parameter));
    }
    [[nodiscard]] static AnimationGraphPatchOperation setMaskJointWeight(
        std::string mask_id, std::string joint_name, float weight) {
        return set_mask_joint_weight(std::move(mask_id), std::move(joint_name), weight);
    }
    [[nodiscard]] static AnimationGraphPatchOperation createNode(
        std::string node_id, std::string kind, std::string clip_asset = {},
        bool looping = true, std::string state_machine_asset = {},
        std::string parameter = {}) {
        return create_node(std::move(node_id), std::move(kind), std::move(clip_asset), looping,
            std::move(state_machine_asset), std::move(parameter));
    }
    [[nodiscard]] static AnimationGraphPatchOperation createNode(AnimationGraphNode node) {
        return create_node(std::move(node));
    }
    [[nodiscard]] static AnimationGraphPatchOperation createClipNode(
        std::string node_id, std::string clip_asset, bool looping = true) {
        return create_clip_node(std::move(node_id), std::move(clip_asset), looping);
    }
    [[nodiscard]] static AnimationGraphPatchOperation createStateMachineNode(
        std::string node_id, std::string state_machine_asset) {
        return create_state_machine_node(std::move(node_id), std::move(state_machine_asset));
    }
    [[nodiscard]] static AnimationGraphPatchOperation createBlend1DNode(
        std::string node_id, std::string parameter,
        std::vector<AnimationGraphBlendPoint> children = {}) {
        return create_blend_1d_node(std::move(node_id), std::move(parameter), std::move(children));
    }
    [[nodiscard]] static AnimationGraphPatchOperation createBlend1dNode(
        std::string node_id, std::string parameter,
        std::vector<AnimationGraphBlendPoint> children = {}) {
        return create_blend_1d_node(std::move(node_id), std::move(parameter), std::move(children));
    }
    [[nodiscard]] static AnimationGraphPatchOperation deleteNode(std::string node_id) {
        return delete_node(std::move(node_id));
    }
    [[nodiscard]] static AnimationGraphPatchOperation connectBlend1DChild(
        std::string blend_node_id, std::string child_node_id, float threshold) {
        return connect_blend_1d_child(std::move(blend_node_id), std::move(child_node_id), threshold);
    }
    [[nodiscard]] static AnimationGraphPatchOperation connectBlend1dChild(
        std::string blend_node_id, std::string child_node_id, float threshold) {
        return connect_blend_1d_child(std::move(blend_node_id), std::move(child_node_id), threshold);
    }
    [[nodiscard]] static AnimationGraphPatchOperation connectBlendChild(
        std::string blend_node_id, std::string child_node_id, float threshold) {
        return connect_blend_1d_child(std::move(blend_node_id), std::move(child_node_id), threshold);
    }
    [[nodiscard]] static AnimationGraphPatchOperation connect_blend_child(
        std::string blend_node_id, std::string child_node_id, float threshold) {
        return connect_blend_1d_child(std::move(blend_node_id), std::move(child_node_id), threshold);
    }
    [[nodiscard]] static AnimationGraphPatchOperation disconnectBlend1DChild(
        std::string blend_node_id, std::string child_node_id) {
        return disconnect_blend_1d_child(std::move(blend_node_id), std::move(child_node_id));
    }
    [[nodiscard]] static AnimationGraphPatchOperation disconnectBlend1dChild(
        std::string blend_node_id, std::string child_node_id) {
        return disconnect_blend_1d_child(std::move(blend_node_id), std::move(child_node_id));
    }
    [[nodiscard]] static AnimationGraphPatchOperation disconnectBlendChild(
        std::string blend_node_id, std::string child_node_id) {
        return disconnect_blend_1d_child(std::move(blend_node_id), std::move(child_node_id));
    }
    [[nodiscard]] static AnimationGraphPatchOperation disconnect_blend_child(
        std::string blend_node_id, std::string child_node_id) {
        return disconnect_blend_1d_child(std::move(blend_node_id), std::move(child_node_id));
    }
};

struct AnimationGraphPatchIssue final {
    std::string code;
    std::string path;
    std::string detail;
};

struct AnimationGraphPatchPlan final {
    bool valid{};
    std::string code;
    std::string detail;
    std::string plan_id;
    std::string asset_id;
    std::string base_fingerprint;
    std::string result_fingerprint;
    std::size_t requested_operation_count{};
    std::size_t changed_operation_count{};
    std::vector<AnimationGraphPatchOperation> operations;
    std::optional<AnimationGraphDocument> result;
    std::vector<AnimationGraphPatchIssue> issues;

    [[nodiscard]] std::string to_json() const;
};

struct AnimationGraphPatchReceipt final {
    bool success{};
    bool dry_run{};
    std::string code;
    std::string detail;
    std::string plan_id;
    std::string asset_id;
    std::string fingerprint_before;
    std::string fingerprint_after;
    std::size_t operation_count{};

    [[nodiscard]] std::string to_json() const;
};

class AnimationGraphPatch final {
public:
    [[nodiscard]] static std::string fingerprint(const AnimationGraphDocument& document);

    [[nodiscard]] static AnimationGraphPatchPlan plan(
        const AnimationGraphDocument& document,
        std::vector<AnimationGraphPatchOperation> operations,
        std::string_view expected_fingerprint = {});

    // Convenience entry point for source adapters.  It parses the source via
    // the frozen Codec and then delegates to the same transaction core.
    [[nodiscard]] static AnimationGraphPatchPlan plan_source(
        std::string_view source,
        std::vector<AnimationGraphPatchOperation> operations,
        std::string_view expected_fingerprint = {});

    // Apply verifies the immutable plan, the current base fingerprint and the
    // candidate fingerprint before changing the caller-owned document.  A
    // dry run returns a receipt without changing it.
    [[nodiscard]] static AnimationGraphPatchReceipt apply(
        AnimationGraphDocument& document,
        const AnimationGraphPatchPlan& plan,
        bool dry_run = false);

    [[nodiscard]] static std::string plan_json(const AnimationGraphPatchPlan& plan);
    [[nodiscard]] static std::string receipt_json(const AnimationGraphPatchReceipt& receipt);
};

// Free-function aliases keep the transaction usable from code that prefers a
// functional engine API while retaining one implementation and one contract.
[[nodiscard]] std::string animation_graph_fingerprint(const AnimationGraphDocument& document);
[[nodiscard]] AnimationGraphPatchPlan plan_animation_graph_patch(
    const AnimationGraphDocument& document,
    std::vector<AnimationGraphPatchOperation> operations,
    std::string_view expected_fingerprint = {});
[[nodiscard]] AnimationGraphPatchReceipt apply_animation_graph_patch(
    AnimationGraphDocument& document,
    const AnimationGraphPatchPlan& plan,
    bool dry_run = false);

} // namespace noemancer
