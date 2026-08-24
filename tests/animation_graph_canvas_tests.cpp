#include "editor/animation_graph_canvas.hpp"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>
#include <nlohmann/json.hpp>

namespace {

constexpr auto valid_graph = R"({
  "schemaVersion":"noemancer.animation-graph/0.1",
  "assetId":"animation.graph.canvas.test",
  "parameters":[{"id":"speed","type":"float","default":0}],
  "nodes":[
    {"id":"idle","kind":"clip","clipAsset":"asset.animation.idle","looping":true},
    {"id":"run","kind":"clip","clipAsset":"asset.animation.run","looping":true},
    {"id":"locomotion","kind":"blend-1d","parameter":"speed","children":[
      {"nodeId":"run","threshold":1},{"nodeId":"idle","threshold":0}]},
    {"id":"aim","kind":"state-machine","stateMachineAsset":"animation.machine.aim"}
  ],
  "layers":[
    {"id":"base","rootNode":"locomotion","mode":"override","weight":1},
    {"id":"upper","rootNode":"aim","mode":"additive","weight":0.75}
  ],
  "masks":[],"syncGroups":[],
  "editor":{"nodes":[{"id":"aim","position":[420,80],"collapsed":true},{"id":"locomotion","position":[80,160],"collapsed":false}],"zoom":1,"pan":[0,0]}
})";

const noemancer::AnimationGraphCanvasNodeBounds* find_node(
    const noemancer::AnimationGraphCanvasProjection& projection, const std::string_view node_id) {
    const auto found = std::ranges::find(projection.nodes, node_id,
        &noemancer::AnimationGraphCanvasNodeBounds::node_id);
    return found == projection.nodes.end() ? nullptr : &*found;
}

const noemancer::AnimationGraphCanvasPort* find_port(
    const noemancer::AnimationGraphCanvasProjection& projection,
    const std::string_view node_id, const std::string_view port_id) {
    const auto found = std::ranges::find_if(projection.ports, [&](const auto& port) {
        return port.node_id == node_id && port.port_id == port_id;
    });
    return found == projection.ports.end() ? nullptr : &*found;
}

const noemancer::AnimationGraphCanvasBlendEdge* find_edge(
    const noemancer::AnimationGraphCanvasProjection& projection,
    const std::string_view blend_node_id, const std::string_view child_node_id) {
    const auto found = std::ranges::find_if(projection.blend_edges, [&](const auto& edge) {
        return edge.blend_node_id == blend_node_id && edge.child_node_id == child_node_id;
    });
    return found == projection.blend_edges.end() ? nullptr : &*found;
}

} // namespace

int main() {
    using Json = nlohmann::json;
    const auto parsed = noemancer::AnimationGraphCodec::parse_json(valid_graph);
    if (!parsed) {
        std::cerr << "Canvas fixture failed to parse: " << parsed.code << '\n';
        return 1;
    }
    auto document = *parsed.document;
    const auto source_before = noemancer::AnimationGraphCodec::write_canonical_json(document);

    noemancer::AnimationGraphCanvasModel model(document);
    const auto projection = model.project();
    if (!projection.valid || projection.code != "ok" || projection.asset_id != document.asset_id ||
        projection.nodes.size() != 4U || projection.blend_edges.size() != 2U ||
        projection.layer_roots.size() != 2U || !projection.selected_node_ids.empty()) {
        std::cerr << "Canvas projection did not expose the bounded graph structure.\n";
        return 2;
    }
    const auto* placed = find_node(projection, "locomotion");
    if (placed == nullptr || placed->x != 80.0F || placed->y != 160.0F ||
        placed->width <= 0.0F || placed->height <= 0.0F) {
        std::cerr << "Canvas projection did not preserve the authoritative node layout.\n";
        return 3;
    }
    if (!std::ranges::any_of(projection.blend_edges, [](const auto& edge) {
            return edge.blend_node_id == "locomotion" && edge.child_node_id == "idle" && edge.threshold == 0.0F;
        }) || !std::ranges::any_of(projection.layer_roots, [](const auto& root) {
            return root.layer_id == "upper" && root.root_node_id == "aim" && root.mode == "additive";
        })) {
        std::cerr << "Canvas projection did not expose stable blend edges and layer roots.\n";
        return 4;
    }

    const auto* collapsed = find_node(projection, "aim");
    const auto* blend_output = find_port(projection, "locomotion", "output");
    const auto* idle_input = find_port(projection, "locomotion", "child:idle");
    const auto* idle_output = find_port(projection, "idle", "output");
    const auto* idle_edge = find_edge(projection, "locomotion", "idle");
    if (collapsed == nullptr || !collapsed->collapsed || projection.ports.size() != 6U ||
        blend_output == nullptr || idle_input == nullptr || idle_output == nullptr || idle_edge == nullptr ||
        blend_output->direction != "output" || idle_input->direction != "input" ||
        idle_input->kind != "blend-child" || idle_edge->edge_id != "locomotion->idle" ||
        idle_edge->from_port_id != "output" || idle_edge->to_port_id != "child:idle" ||
        idle_edge->from_x != idle_output->x || idle_edge->from_y != idle_output->y ||
        idle_edge->to_x != idle_input->x || idle_edge->to_y != idle_input->y) {
        std::cerr << "Canvas topology projection did not expose collapsed state, ports, and edge geometry.\n";
        return 14;
    }

    const auto projection_json = Json::parse(model.projection_json());
    if (projection_json.at("schemaVersion").get<std::string>() !=
            std::string(noemancer::animation_graph_canvas_schema) ||
        projection_json.at("nodes").size() != 4U || projection_json.at("blendEdges").size() != 2U ||
        projection_json.at("layerRoots").size() != 2U || projection_json.at("ports").size() != 6U ||
        projection_json.at("coordinateSpace") != "graph" ||
        !projection_json.at("nodes").at(0).contains("collapsed") ||
        projection_json.at("blendEdges").at(0).at("sourceNodeId") != "idle" ||
        projection_json.at("blendEdges").at(0).at("targetNodeId") != "locomotion" ||
        projection_json.at("bounds").at("portCount") != 6U || projection_json.dump().size() > 64U * 1024U) {
        std::cerr << "Canvas semantic JSON was not stable or bounded.\n";
        return 5;
    }

    if (!model.select_node("locomotion") || !model.select_node("aim", true) ||
        model.selection() != std::vector<std::string>{"locomotion", "aim"} ||
        model.select_node("missing", true)) {
        std::cerr << "Canvas selection did not use stable node IDs.\n";
        return 6;
    }
    const auto selected_projection = model.project();
    if (selected_projection.selected_node_ids != std::vector<std::string>{"locomotion", "aim"} ||
        !find_node(selected_projection, "locomotion")->selected ||
        !find_node(selected_projection, "aim")->selected) {
        std::cerr << "Canvas selection was not reflected in the projection.\n";
        return 7;
    }
    if (!model.deselect_node("locomotion") || model.deselect_node("missing")) {
        std::cerr << "Canvas deselection accepted an invalid node.\n";
        return 8;
    }
    model.clear_selection();

    if (!model.set_view(10.0F, -5.0F, 2.0F) || !model.pan_by(5.0F, 3.0F) ||
        document.editor.pan_x != 0.0F || document.editor.pan_y != 0.0F || document.editor.zoom != 1.0F) {
        std::cerr << "Canvas view state mutated the Graph authority or rejected valid input.\n";
        return 9;
    }
    const auto view_projection = model.project();
    if (view_projection.pan_x != 15.0F || view_projection.pan_y != -2.0F || view_projection.zoom != 2.0F ||
        model.set_view(0.0F, 0.0F, std::numeric_limits<float>::quiet_NaN()) ||
        model.pan_by(std::numeric_limits<float>::quiet_NaN(), 0.0F)) {
        std::cerr << "Canvas pan/zoom bounds were not enforced.\n";
        return 10;
    }
    const auto canvas_point = model.graph_to_canvas(100.0F, 50.0F);
    const auto graph_point = canvas_point ? model.canvas_to_graph(canvas_point->x, canvas_point->y) : std::nullopt;
    if (!canvas_point || canvas_point->x != 215.0F || canvas_point->y != 98.0F || !graph_point ||
        std::abs(graph_point->x - 100.0F) > 0.0001F || std::abs(graph_point->y - 50.0F) > 0.0001F ||
        model.graph_to_canvas(std::numeric_limits<float>::quiet_NaN(), 0.0F) ||
        model.canvas_to_graph(std::numeric_limits<float>::infinity(), 0.0F)) {
        std::cerr << "Canvas graph/canvas coordinate transforms were not bounded or reversible.\n";
        return 15;
    }
    model.reset_view_to_document();

    const auto interaction_projection = model.project();
    const auto* interaction_node = find_node(interaction_projection, "locomotion");
    const auto* interaction_port = find_port(interaction_projection, "locomotion", "output");
    const auto* interaction_edge = find_edge(interaction_projection, "locomotion", "idle");
    if (interaction_node == nullptr || interaction_port == nullptr || interaction_edge == nullptr) return 16;
    const auto node_hit = model.hit_test(interaction_node->x + 100.0F, interaction_node->y + 20.0F);
    const auto port_hit = model.hit_test(interaction_port->x, interaction_port->y);
    const auto edge_first = noemancer::AnimationGraphCanvasPoint{interaction_edge->from_x, interaction_edge->from_y};
    const auto edge_last = noemancer::AnimationGraphCanvasPoint{interaction_edge->to_x, interaction_edge->to_y};
    const auto edge_bend = std::max(42.0F, std::abs(edge_last.x - edge_first.x) * 0.42F);
    const auto edge_control_first = noemancer::AnimationGraphCanvasPoint{edge_first.x + edge_bend, edge_first.y};
    const auto edge_control_last = noemancer::AnimationGraphCanvasPoint{edge_last.x - edge_bend, edge_last.y};
    const auto t = 0.5F;
    const auto inverse = 1.0F - t;
    const auto edge_midpoint = noemancer::AnimationGraphCanvasPoint{
        inverse * inverse * inverse * edge_first.x + 3.0F * inverse * inverse * t * edge_control_first.x +
            3.0F * inverse * t * t * edge_control_last.x + t * t * t * edge_last.x,
        inverse * inverse * inverse * edge_first.y + 3.0F * inverse * inverse * t * edge_control_first.y +
            3.0F * inverse * t * t * edge_control_last.y + t * t * t * edge_last.y};
    const auto edge_hit = model.hit_test(edge_midpoint.x, edge_midpoint.y, 2.0F);
    const auto empty_hit = model.hit_test(-1000.0F, -1000.0F);
    if (node_hit.kind != "node" || node_hit.node_id != "locomotion" ||
        port_hit.kind != "port" || port_hit.node_id != "locomotion" || port_hit.port_id != "output" ||
        edge_hit.kind != "edge" || edge_hit.blend_node_id != "locomotion" || edge_hit.child_node_id != "idle" ||
        edge_hit.distance > 2.0F ||
        empty_hit.kind != "empty") {
        std::cerr << "Canvas hit testing did not deterministically classify node, port, edge, and empty targets.\n";
        return 17;
    }

    const auto operation = model.drag_to_layout("locomotion", 120.0F, 220.0F);
    if (!operation || operation->asset_id != document.asset_id || operation->node_id != "locomotion" ||
        operation->x != 120.0F || operation->y != 220.0F ||
        noemancer::AnimationGraphCodec::write_canonical_json(document) != source_before ||
        Json::parse(operation->to_json()).at("operation") != "setNodePosition" ||
        Json::parse(operation->to_json()).at("position") != Json::array({120.0F, 220.0F}) ||
        model.drag_to_layout("missing", 0.0F, 0.0F) ||
        model.drag_to_layout("aim", std::numeric_limits<float>::quiet_NaN(), 0.0F)) {
        std::cerr << "Canvas drag did not produce a plain, non-mutating layout operation.\n";
        return 11;
    }

    auto duplicate = document;
    duplicate.nodes.at(1).id = duplicate.nodes.at(0).id;
    noemancer::AnimationGraphCanvasModel invalid_model(duplicate);
    const auto invalid_projection = invalid_model.project();
    if (invalid_projection.valid || invalid_projection.code != "animation.graph.canvas-node-invalid") {
        std::cerr << "Canvas did not reject duplicate node IDs with a bounded diagnostic.\n";
        return 12;
    }
    noemancer::AnimationGraphCanvasModel empty_model;
    if (empty_model.project().code != "animation.graph.canvas-no-document" ||
        empty_model.hit_test(0.0F, 0.0F).kind != "empty" ||
        model.hit_test(0.0F, 0.0F, -1.0F).kind != "empty" ||
        model.hit_test(std::numeric_limits<float>::quiet_NaN(), 0.0F).kind != "empty") return 13;
    return 0;
}
