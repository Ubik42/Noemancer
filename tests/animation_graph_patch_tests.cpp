#include "engine/animation_graph_patch.hpp"

#include <algorithm>
#include <iostream>
#include <limits>
#include <nlohmann/json.hpp>
#include <ranges>

namespace {

constexpr auto valid_graph = R"({
  "schemaVersion":"noemancer.animation-graph/0.1",
  "assetId":"animation.graph.patch.test",
  "parameters":[{"id":"speed","type":"float","default":0},{"id":"aimWeight","type":"float","default":0}],
  "nodes":[
    {"id":"idle","kind":"clip","clipAsset":"asset.animation.idle","looping":true},
    {"id":"run","kind":"clip","clipAsset":"asset.animation.run","looping":true},
    {"id":"locomotion","kind":"blend-1d","parameter":"speed","children":[{"nodeId":"run","threshold":1},{"nodeId":"idle","threshold":0}]},
    {"id":"aim","kind":"state-machine","stateMachineAsset":"animation.machine.aim"}
  ],
  "layers":[
    {"id":"base","rootNode":"locomotion","mode":"override","weight":1,"syncGroup":"locomotion"},
    {"id":"upper","rootNode":"aim","mode":"additive","weight":1,"weightParameter":"aimWeight","maskId":"upper-body"}
  ],
  "masks":[{"id":"upper-body","includeDescendants":true,"joints":[{"name":"spine","weight":1}]}],
  "syncGroups":[{"id":"locomotion","mode":"normalized-time"}],
  "editor":{"nodes":[{"id":"locomotion","position":[80,160],"collapsed":false}],"zoom":1,"pan":[0,0]}
})";

bool failed(const bool condition, const char* message) {
    if (!condition) return false;
    std::cerr << message << '\n';
    return true;
}

} // namespace

int main() {
    using noemancer::AnimationGraphPatch;
    using noemancer::AnimationGraphPatchOperation;
    using Json = nlohmann::json;

    const auto parsed = noemancer::AnimationGraphCodec::parse_json(valid_graph);
    if (!parsed) {
        std::cerr << "Patch fixture failed to parse: " << parsed.code << '\n';
        return 1;
    }
    auto document = *parsed.document;
    const auto before = AnimationGraphPatch::fingerprint(document);
    if (before.rfind("fnv1a64:", 0) != 0U) return 2;

    const auto reordered = Json::parse(valid_graph);
    auto reordered_document = noemancer::AnimationGraphCodec::parse_json(reordered.dump());
    if (!reordered_document || AnimationGraphPatch::fingerprint(*reordered_document.document) != before) return 3;

    const auto plan = AnimationGraphPatch::plan(document,
        {AnimationGraphPatchOperation::setNodePosition("locomotion", 100.0F, 200.0F),
         AnimationGraphPatchOperation::setLayerWeight("upper", 0.5F),
         AnimationGraphPatchOperation::setLayerWeightParameter("upper", ""),
         AnimationGraphPatchOperation::setMaskJointWeight("upper-body", "spine", 0.25F)}, before);
    if (failed(!plan.valid || plan.code != "ok" || plan.changed_operation_count != 4U ||
        plan.base_fingerprint != before || !plan.result, "Valid Graph patch did not produce a candidate.")) return 4;
    if (plan.result->asset_id != document.asset_id || plan.result->layers.at(1).weight != 0.5F ||
        !plan.result->layers.at(1).weight_parameter.empty() || plan.result->masks.at(0).joints.at(0).weight != 0.25F)
        return 5;
    const auto plan_json = Json::parse(plan.to_json());
    if (plan_json.at("schemaVersion").get<std::string>() != noemancer::animation_graph_patch_plan_schema ||
        plan_json.at("candidate").at("assetId") != document.asset_id ||
        plan_json.at("operations").size() != 4U) return 6;

    auto baseline = document;
    const auto unchanged = noemancer::AnimationGraphCodec::write_canonical_json(document);
    const auto dry_receipt = AnimationGraphPatch::apply(document, plan, true);
    if (!dry_receipt.success || !dry_receipt.dry_run ||
        noemancer::AnimationGraphCodec::write_canonical_json(document) != unchanged ||
        dry_receipt.fingerprint_after != plan.result_fingerprint) return 7;
    const auto committed = AnimationGraphPatch::apply(document, plan, false);
    if (!committed.success || committed.dry_run || document.layers.at(1).weight != 0.5F ||
        document.editor.nodes.at(0).x != 100.0F) return 8;
    if (Json::parse(committed.to_json()).at("schemaVersion").get<std::string>() != noemancer::animation_graph_patch_receipt_schema)
        return 9;

    const auto conflict = AnimationGraphPatch::plan(document,
        {AnimationGraphPatchOperation::setLayerWeight("upper", 0.75F)}, before);
    if (conflict.valid || conflict.code != "animation.graph.patch-conflict" || conflict.issues.empty()) return 10;

    const auto invalid_candidate = AnimationGraphPatch::plan(document,
        {AnimationGraphPatchOperation::setLayerWeightParameter("upper", "missing")});
    if (invalid_candidate.valid || invalid_candidate.code != "animation.graph.patch-weight-parameter-invalid") return 11;

    std::vector<AnimationGraphPatchOperation> too_many;
    too_many.reserve(noemancer::animation_graph_patch_max_operations + 1U);
    for (std::size_t index = 0; index <= noemancer::animation_graph_patch_max_operations; ++index)
        too_many.push_back(AnimationGraphPatchOperation::setLayerWeight("upper", 0.5F));
    const auto bounded = AnimationGraphPatch::plan(document, std::move(too_many));
    if (bounded.valid || bounded.code != "animation.graph.patch-size") return 12;
    const auto huge_position=AnimationGraphPatch::plan(document,
        {AnimationGraphPatchOperation::setNodePosition("locomotion",1.0e20F,0.0F)});
    if(huge_position.valid||huge_position.code!="animation.graph.patch-position-invalid")return 14;

    auto tampered = plan;
    tampered.result->layers.at(0).weight = 0.25F;
    const auto tamper_receipt = AnimationGraphPatch::apply(baseline, tampered, false);
    if (tamper_receipt.success || tamper_receipt.code != "animation.graph.patch-integrity-error") return 13;

    const auto topology_plan = AnimationGraphPatch::plan(document,
        {AnimationGraphPatchOperation::createClipNode("walk", "asset.animation.walk"),
         AnimationGraphPatchOperation::connectBlend1DChild("locomotion", "walk", 0.5F)});
    if (failed(!topology_plan.valid || topology_plan.code != "ok" || !topology_plan.result,
        "Node creation and Blend 1D connection did not produce a valid candidate.")) return 15;
    const auto topology_json = Json::parse(topology_plan.to_json());
    if (topology_json.at("operations").at(0).at("operation") != "createNode" ||
        topology_json.at("operations").at(1).at("operation") != "connectBlend1DChild" ||
        topology_plan.base_fingerprint == topology_plan.result_fingerprint || topology_plan.plan_id.empty())
        return 29;
    const auto& topology_blend = *std::ranges::find(topology_plan.result->nodes, "locomotion",
        &noemancer::AnimationGraphNode::id);
    if (topology_blend.children.size() != 3U || topology_blend.children.at(1).node_id != "walk" ||
        topology_blend.children.at(1).threshold != 0.5F)
        return 16;
    const auto topology_before = AnimationGraphPatch::fingerprint(document);
    const auto topology_dry = AnimationGraphPatch::apply(document, topology_plan, true);
    if (!topology_dry.success || !topology_dry.dry_run ||
        AnimationGraphPatch::fingerprint(document) != topology_before) return 17;
    const auto topology_commit = AnimationGraphPatch::apply(document, topology_plan, false);
    if (!topology_commit.success ||
        std::ranges::find(document.nodes, "walk", &noemancer::AnimationGraphNode::id) == document.nodes.end())
        return 18;

    const auto duplicate_id = AnimationGraphPatch::plan(document,
        {AnimationGraphPatchOperation::createClipNode("idle", "asset.animation.other")});
    if (duplicate_id.valid || duplicate_id.code != "animation.graph.patch-node-already-exists") return 19;
    const auto invalid_kind = AnimationGraphPatch::plan(document,
        {AnimationGraphPatchOperation::createNode("invalid-kind", "not-a-node")});
    if (invalid_kind.valid || invalid_kind.code != "animation.graph.patch-node-kind-invalid") return 30;
    const auto missing_clip_asset = AnimationGraphPatch::plan(document,
        {AnimationGraphPatchOperation::createNode("missing-clip", "clip")});
    if (missing_clip_asset.valid || missing_clip_asset.code != "animation.graph.patch-clip-asset-required") return 31;
    const auto missing_blend_parameter = AnimationGraphPatch::plan(document,
        {AnimationGraphPatchOperation::createBlend1DNode("missing-parameter", "missing")});
    if (missing_blend_parameter.valid ||
        missing_blend_parameter.code != "animation.graph.patch-blend-parameter-invalid") return 32;
    const auto wrong_source = AnimationGraphPatch::plan(document,
        {AnimationGraphPatchOperation::connectBlend1DChild("idle", "walk", 2.0F)});
    if (wrong_source.valid || wrong_source.code != "animation.graph.patch-blend-node-invalid") return 20;
    const auto wrong_child = AnimationGraphPatch::plan(document,
        {AnimationGraphPatchOperation::connectBlend1DChild("locomotion", "aim", 2.0F)});
    if (wrong_child.valid || wrong_child.code != "animation.graph.patch-blend-child-invalid") return 21;
    const auto duplicate_threshold = AnimationGraphPatch::plan(document,
        {AnimationGraphPatchOperation::connectBlend1DChild("locomotion", "walk", 0.5F)});
    if (duplicate_threshold.valid ||
        duplicate_threshold.code != "animation.graph.patch-blend-child-already-connected") return 22;
    const auto nan_threshold = AnimationGraphPatch::plan(document,
        {AnimationGraphPatchOperation::connectBlend1DChild("locomotion", "walk",
            std::numeric_limits<float>::quiet_NaN())});
    if (nan_threshold.valid || nan_threshold.code != "animation.graph.patch-threshold-invalid") return 33;
    const auto invalid_initial_order = AnimationGraphPatch::plan(document,
        {AnimationGraphPatchOperation::createBlend1DNode("bad-order", "speed",
            {{"idle", 1.0F}, {"run", 0.0F}})});
    if (invalid_initial_order.valid || invalid_initial_order.code != "animation.graph.patch-blend-children-invalid") return 23;
    const auto second_state_machine = AnimationGraphPatch::plan(document,
        {AnimationGraphPatchOperation::createStateMachineNode("another-machine", "animation.machine.other")});
    if (second_state_machine.valid || second_state_machine.code != "animation.graph.patch-candidate-invalid") return 24;

    const auto delete_in_use = AnimationGraphPatch::plan(document,
        {AnimationGraphPatchOperation::deleteNode("walk")});
    if (delete_in_use.valid || delete_in_use.code != "animation.graph.patch-node-in-use") return 25;
    const auto delete_root = AnimationGraphPatch::plan(document,
        {AnimationGraphPatchOperation::deleteNode("locomotion")});
    if (delete_root.valid || delete_root.code != "animation.graph.patch-node-in-use") return 34;
    const auto disconnect_and_delete = AnimationGraphPatch::plan(document,
        {AnimationGraphPatchOperation::disconnectBlend1DChild("locomotion", "walk"),
         AnimationGraphPatchOperation::deleteNode("walk")});
    if (failed(!disconnect_and_delete.valid || !disconnect_and_delete.result,
        "Explicit disconnect followed by node deletion should preserve references.")) return 26;
    const auto delete_commit = AnimationGraphPatch::apply(document, disconnect_and_delete, false);
    if (!delete_commit.success ||
        std::ranges::find(document.nodes, "walk", &noemancer::AnimationGraphNode::id) != document.nodes.end())
        return 27;
    const auto missing_disconnect = AnimationGraphPatch::plan(document,
        {AnimationGraphPatchOperation::disconnectBlend1DChild("locomotion", "walk")});
    if (missing_disconnect.valid || missing_disconnect.code != "animation.graph.patch-blend-child-not-found") return 28;

    return 0;
}
