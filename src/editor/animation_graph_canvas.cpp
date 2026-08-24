#include "editor/animation_graph_canvas.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <nlohmann/json.hpp>
#include <unordered_set>
#include <utility>

namespace noemancer {
namespace {

using Json = nlohmann::json;

constexpr float default_node_width = 220.0F;
constexpr float default_node_height = 64.0F;
constexpr float blend_child_row_height = 22.0F;
constexpr float node_output_port_y = 32.0F;
constexpr float canvas_port_radius = 8.0F;
constexpr float fallback_column_width = 280.0F;
constexpr float fallback_row_height = 140.0F;
constexpr std::size_t fallback_columns = 8U;
constexpr std::size_t edge_curve_segments = 16U;

bool finite(const float value) noexcept {
    return std::isfinite(value);
}

bool bounded_coordinate(const float value) noexcept {
    return finite(value) && std::abs(value) <= animation_graph_editor_coordinate_limit;
}

bool valid_view(const float pan_x, const float pan_y, const float zoom) noexcept {
    return bounded_coordinate(pan_x) && bounded_coordinate(pan_y) && finite(zoom) &&
        zoom >= 0.1F && zoom <= 4.0F;
}

float squared_distance(const AnimationGraphCanvasPoint& left,
    const AnimationGraphCanvasPoint& right) noexcept {
    const auto dx = left.x - right.x;
    const auto dy = left.y - right.y;
    return dx * dx + dy * dy;
}

float point_segment_distance_squared(const AnimationGraphCanvasPoint& point,
    const AnimationGraphCanvasPoint& first, const AnimationGraphCanvasPoint& second) noexcept {
    const auto dx = second.x - first.x;
    const auto dy = second.y - first.y;
    const auto length_squared = dx * dx + dy * dy;
    if (length_squared <= 0.0F) return squared_distance(point, first);
    const auto projection = std::clamp(((point.x - first.x) * dx + (point.y - first.y) * dy) /
        length_squared, 0.0F, 1.0F);
    return squared_distance(point, {first.x + projection * dx, first.y + projection * dy});
}

AnimationGraphCanvasPoint cubic_point(const AnimationGraphCanvasPoint& first,
    const AnimationGraphCanvasPoint& control_first,
    const AnimationGraphCanvasPoint& control_second,
    const AnimationGraphCanvasPoint& last, const float t) noexcept {
    const auto inverse = 1.0F - t;
    const auto inverse_squared = inverse * inverse;
    const auto t_squared = t * t;
    return {
        inverse_squared * inverse * first.x + 3.0F * inverse_squared * t * control_first.x +
            3.0F * inverse * t_squared * control_second.x + t_squared * t * last.x,
        inverse_squared * inverse * first.y + 3.0F * inverse_squared * t * control_first.y +
            3.0F * inverse * t_squared * control_second.y + t_squared * t * last.y
    };
}

float edge_distance_squared(const AnimationGraphCanvasBlendEdge& edge,
    const AnimationGraphCanvasPoint& point) noexcept {
    const AnimationGraphCanvasPoint first{edge.from_x, edge.from_y};
    const AnimationGraphCanvasPoint last{edge.to_x, edge.to_y};
    const auto bend = std::max(42.0F, std::abs(last.x - first.x) * 0.42F);
    const AnimationGraphCanvasPoint control_first{first.x + bend, first.y};
    const AnimationGraphCanvasPoint control_second{last.x - bend, last.y};
    auto previous = first;
    auto best = std::numeric_limits<float>::infinity();
    for (std::size_t index = 1; index <= edge_curve_segments; ++index) {
        const auto t = static_cast<float>(index) / static_cast<float>(edge_curve_segments);
        const auto current = cubic_point(first, control_first, control_second, last, t);
        best = std::min(best, point_segment_distance_squared(point, previous, current));
        previous = current;
    }
    return best;
}

void set_failure(AnimationGraphCanvasProjection& projection,
    std::string code, std::string detail) {
    projection.valid = false;
    projection.code = std::move(code);
    projection.detail = std::move(detail);
    projection.nodes.clear();
    projection.blend_edges.clear();
    projection.layer_roots.clear();
    projection.ports.clear();
}

Json node_json(const AnimationGraphCanvasNodeBounds& node) {
    return {
        {"id", node.node_id},
        {"kind", node.kind},
        {"bounds", {
            {"x", node.x}, {"y", node.y},
            {"width", node.width}, {"height", node.height}
        }},
        {"selected", node.selected},
        {"collapsed", node.collapsed}
    };
}

Json port_json(const AnimationGraphCanvasPort& port) {
    return {
        {"nodeId", port.node_id},
        {"portId", port.port_id},
        {"direction", port.direction},
        {"kind", port.kind},
        {"index", port.index},
        {"position", {port.x, port.y}},
        {"radius", port.radius}
    };
}

Json edge_json(const AnimationGraphCanvasBlendEdge& edge) {
    return {
        {"kind", "blend-1d"},
        {"fromNodeId", edge.blend_node_id},
        {"toNodeId", edge.child_node_id},
        {"sourceNodeId", edge.child_node_id},
        {"targetNodeId", edge.blend_node_id},
        {"threshold", edge.threshold},
        {"edgeId", edge.edge_id},
        {"fromPortId", edge.from_port_id},
        {"toPortId", edge.to_port_id},
        {"from", {edge.from_x, edge.from_y}},
        {"to", {edge.to_x, edge.to_y}}
    };
}

Json layer_root_json(const AnimationGraphCanvasLayerRoot& root) {
    return {
        {"layerId", root.layer_id},
        {"rootNodeId", root.root_node_id},
        {"mode", root.mode},
        {"weight", root.weight}
    };
}

} // namespace

std::string AnimationGraphCanvasLayoutOperation::to_json() const {
    return Json{
        {"schemaVersion", animation_graph_canvas_operation_schema},
        {"operation", "setNodePosition"},
        {"assetId", asset_id},
        {"nodeId", node_id},
        {"position", {x, y}},
        {"x", x},
        {"y", y}
    }.dump();
}

std::string AnimationGraphCanvasProjection::to_json() const {
    Json nodes_json = Json::array();
    for (const auto& node : nodes) nodes_json.push_back(node_json(node));

    Json edges_json = Json::array();
    for (const auto& edge : blend_edges) edges_json.push_back(edge_json(edge));

    Json roots_json = Json::array();
    for (const auto& root : layer_roots) roots_json.push_back(layer_root_json(root));

    Json ports_json = Json::array();
    for (const auto& port : ports) ports_json.push_back(port_json(port));

    return Json{
        {"schemaVersion", animation_graph_canvas_schema},
        {"valid", valid},
        {"code", code},
        {"detail", detail},
        {"assetId", asset_id},
        {"coordinateSpace", "graph"},
        {"view", {
            {"pan", {pan_x, pan_y}},
            {"zoom", zoom}
        }},
        {"selection", {
            {"nodeIds", selected_node_ids},
            {"count", selected_node_ids.size()}
        }},
        {"nodes", std::move(nodes_json)},
        {"blendEdges", std::move(edges_json)},
        {"layerRoots", std::move(roots_json)},
        {"ports", std::move(ports_json)},
        {"bounds", {
            {"nodeCount", nodes.size()},
            {"blendEdgeCount", blend_edges.size()},
            {"layerRootCount", layer_roots.size()},
            {"selectionCount", selected_node_ids.size()},
            {"portCount", ports.size()}
        }}
    }.dump();
}

AnimationGraphCanvasModel::AnimationGraphCanvasModel(
    const AnimationGraphDocument* document) noexcept {
    bind(document);
}

void AnimationGraphCanvasModel::bind(const AnimationGraphDocument* document) noexcept {
    document_ = document;
    selection_.clear();
    reset_view_to_document();
}

const AnimationGraphDocument* AnimationGraphCanvasModel::document() const noexcept {
    return document_;
}

bool AnimationGraphCanvasModel::has_node(const std::string_view node_id) const noexcept {
    if (document_ == nullptr || node_id.empty()) return false;
    return std::ranges::any_of(document_->nodes,
        [node_id](const AnimationGraphNode& node) { return node.id == node_id; });
}

AnimationGraphCanvasProjection AnimationGraphCanvasModel::project() const {
    AnimationGraphCanvasProjection projection{
        false,
        "animation.graph.canvas-no-document",
        "No Animation Graph document is bound to the Canvas.",
        {},
        pan_x_,
        pan_y_,
        zoom_,
        {},
        {},
        {},
        {},
        {}
    };
    if (document_ == nullptr) return projection;

    projection.asset_id = document_->asset_id;
    if (document_->asset_id.empty()) {
        set_failure(projection, "animation.graph.canvas-identity-missing",
            "The bound Animation Graph has no asset ID.");
        return projection;
    }
    if (document_->nodes.empty()) {
        set_failure(projection, "animation.graph.canvas-nodes-empty",
            "The bound Animation Graph has no nodes to project.");
        return projection;
    }
    if (document_->nodes.size() > animation_graph_canvas_max_nodes) {
        set_failure(projection, "animation.graph.canvas-node-limit",
            "The Animation Graph exceeds the bounded Canvas node limit.");
        return projection;
    }
    if (document_->layers.size() > animation_graph_canvas_max_layer_roots) {
        set_failure(projection, "animation.graph.canvas-layer-limit",
            "The Animation Graph exceeds the bounded Canvas layer limit.");
        return projection;
    }
    if (!bounded_coordinate(pan_x_) || !bounded_coordinate(pan_y_) ||
        !finite(zoom_) || zoom_ < 0.1F || zoom_ > 4.0F) {
        set_failure(projection, "animation.graph.canvas-view-invalid",
            "Canvas pan and zoom must remain finite and bounded.");
        return projection;
    }

    std::unordered_set<std::string> node_ids;
    node_ids.reserve(document_->nodes.size());
    for (const auto& node : document_->nodes) {
        if (node.id.empty() || !node_ids.insert(node.id).second) {
            set_failure(projection, "animation.graph.canvas-node-invalid",
                "Canvas projection requires unique non-empty node IDs.");
            return projection;
        }
    }

    std::unordered_set<std::string> layout_ids;
    layout_ids.reserve(document_->editor.nodes.size());
    for (const auto& layout : document_->editor.nodes) {
        if (layout_ids.size() >= animation_graph_canvas_max_nodes || layout.node_id.empty() ||
            !layout_ids.insert(layout.node_id).second || !node_ids.contains(layout.node_id) ||
            !bounded_coordinate(layout.x) || !bounded_coordinate(layout.y)) {
            set_failure(projection, "animation.graph.canvas-layout-invalid",
                "Canvas layout entries must reference unique graph nodes with bounded positions.");
            return projection;
        }
    }

    std::size_t edge_count{};
    for (const auto& node : document_->nodes) {
        if (node.kind != "clip" && node.kind != "state-machine" && node.kind != "blend-1d") {
            set_failure(projection, "animation.graph.canvas-node-kind-invalid",
                "Canvas projection encountered an unsupported Graph node kind.");
            return projection;
        }
        if (node.kind != "blend-1d") continue;
        if (edge_count + node.children.size() > animation_graph_canvas_max_blend_edges) {
            set_failure(projection, "animation.graph.canvas-edge-limit",
                "The Animation Graph exceeds the bounded Canvas edge limit.");
            return projection;
        }
        for (const auto& child : node.children) {
            if (child.node_id.empty() || !node_ids.contains(child.node_id) || !finite(child.threshold)) {
                set_failure(projection, "animation.graph.canvas-edge-invalid",
                    "Blend edges must reference existing nodes and finite thresholds.");
                return projection;
            }
        }
        edge_count += node.children.size();
    }

    std::unordered_set<std::string> layer_ids;
    layer_ids.reserve(document_->layers.size());
    for (const auto& layer : document_->layers) {
        if (layer.id.empty() || !layer_ids.insert(layer.id).second ||
            layer.root_node.empty() || !node_ids.contains(layer.root_node) ||
            (layer.mode != "override" && layer.mode != "additive") ||
            !finite(layer.weight) || layer.weight < 0.0F || layer.weight > 1.0F) {
            set_failure(projection, "animation.graph.canvas-layer-invalid",
                "Canvas layer roots must reference existing nodes with bounded weights.");
            return projection;
        }
    }

    projection.nodes.reserve(document_->nodes.size());
    for (std::size_t index = 0; index < document_->nodes.size(); ++index) {
        const auto& node = document_->nodes[index];
        float x = static_cast<float>(index % fallback_columns) * fallback_column_width;
        float y = static_cast<float>(index / fallback_columns) * fallback_row_height;
        bool collapsed{};
        const auto layout = std::ranges::find(document_->editor.nodes, node.id,
            &AnimationGraphNodeLayout::node_id);
        if (layout != document_->editor.nodes.end()) {
            x = layout->x;
            y = layout->y;
            collapsed = layout->collapsed;
        }
        const auto height = default_node_height +
            (node.kind == "blend-1d" ? static_cast<float>(node.children.size()) * blend_child_row_height : 0.0F);
        const auto selected = std::ranges::find(selection_, node.id) != selection_.end();
        projection.nodes.push_back({node.id, node.kind, x, y, default_node_width, height, selected, collapsed});

        if (node.kind == "blend-1d") {
            for (const auto& child : node.children)
                projection.blend_edges.push_back({node.id, child.node_id, child.threshold});
        }
    }

    projection.ports.reserve(projection.nodes.size() + projection.blend_edges.size());
    for (const auto& node : projection.nodes) {
        projection.ports.push_back({node.node_id, "output", "output", "pose", 0U,
            node.x + node.width, node.y + node_output_port_y, canvas_port_radius});
    }
    for (const auto& node : projection.nodes) {
        if (node.kind != "blend-1d") continue;
        const auto source = std::ranges::find(document_->nodes, node.node_id,
            &AnimationGraphNode::id);
        if (source == document_->nodes.end()) continue;
        for (std::size_t child_index = 0; child_index < source->children.size(); ++child_index) {
            const auto& child = source->children[child_index];
            const auto port_id = "child:" + child.node_id;
            const auto child_y = node.y + default_node_height +
                (static_cast<float>(child_index) + 0.5F) * blend_child_row_height;
            projection.ports.push_back({node.node_id, port_id, "input", "blend-child", child_index,
                node.x, child_y, canvas_port_radius});

            const auto edge = std::ranges::find_if(projection.blend_edges,
                [&](const AnimationGraphCanvasBlendEdge& value) {
                    return value.blend_node_id == node.node_id && value.child_node_id == child.node_id;
                });
            const auto child_bounds = std::ranges::find(projection.nodes, child.node_id,
                &AnimationGraphCanvasNodeBounds::node_id);
            if (edge == projection.blend_edges.end() || child_bounds == projection.nodes.end()) continue;
            edge->from_x = child_bounds->x + child_bounds->width;
            edge->from_y = child_bounds->y + node_output_port_y;
            edge->to_x = node.x;
            edge->to_y = child_y;
            edge->from_port_id = "output";
            edge->to_port_id = port_id;
            edge->edge_id = node.node_id + "->" + child.node_id;
        }
    }
    projection.layer_roots.reserve(document_->layers.size());
    for (const auto& layer : document_->layers)
        projection.layer_roots.push_back({layer.id, layer.root_node, layer.mode, layer.weight});

    projection.selected_node_ids.reserve(std::min(selection_.size(), animation_graph_canvas_max_selection));
    for (const auto& node_id : selection_) {
        if (projection.selected_node_ids.size() >= animation_graph_canvas_max_selection) break;
        if (node_ids.contains(node_id)) projection.selected_node_ids.push_back(node_id);
    }
    projection.valid = true;
    projection.code = "ok";
    projection.detail = "Animation Graph Canvas projection is bounded and renderer-neutral.";
    return projection;
}

std::string AnimationGraphCanvasModel::projection_json() const {
    return project().to_json();
}

bool AnimationGraphCanvasModel::select_node(const std::string_view node_id,
    const bool additive) noexcept {
    if (!has_node(node_id)) return false;
    if (!additive) selection_.clear();
    if (std::ranges::find(selection_, node_id) == selection_.end()) {
        if (selection_.size() >= animation_graph_canvas_max_selection) return false;
        selection_.emplace_back(node_id);
    }
    return true;
}

bool AnimationGraphCanvasModel::deselect_node(const std::string_view node_id) noexcept {
    const auto found = std::ranges::find(selection_, node_id);
    if (found == selection_.end()) return false;
    selection_.erase(found);
    return true;
}

void AnimationGraphCanvasModel::clear_selection() noexcept {
    selection_.clear();
}

const std::vector<std::string>& AnimationGraphCanvasModel::selection() const noexcept {
    return selection_;
}

bool AnimationGraphCanvasModel::set_view(const float pan_x, const float pan_y,
    const float zoom) noexcept {
    if (!bounded_coordinate(pan_x) || !bounded_coordinate(pan_y) ||
        !finite(zoom) || zoom < 0.1F || zoom > 4.0F) return false;
    pan_x_ = pan_x;
    pan_y_ = pan_y;
    zoom_ = zoom;
    return true;
}

bool AnimationGraphCanvasModel::pan_by(const float delta_x, const float delta_y) noexcept {
    if (!bounded_coordinate(delta_x) || !bounded_coordinate(delta_y) ||
        !bounded_coordinate(pan_x_ + delta_x) || !bounded_coordinate(pan_y_ + delta_y)) return false;
    pan_x_ += delta_x;
    pan_y_ += delta_y;
    return true;
}

void AnimationGraphCanvasModel::reset_view_to_document() noexcept {
    pan_x_ = 0.0F;
    pan_y_ = 0.0F;
    zoom_ = 1.0F;
    if (document_ == nullptr) return;
    if (bounded_coordinate(document_->editor.pan_x)) pan_x_ = document_->editor.pan_x;
    if (bounded_coordinate(document_->editor.pan_y)) pan_y_ = document_->editor.pan_y;
    if (finite(document_->editor.zoom) && document_->editor.zoom >= 0.1F && document_->editor.zoom <= 4.0F)
        zoom_ = document_->editor.zoom;
}

std::optional<AnimationGraphCanvasLayoutOperation> AnimationGraphCanvasModel::drag_to_layout(
    const std::string_view node_id, const float x, const float y) const {
    if (!has_node(node_id) || !bounded_coordinate(x) || !bounded_coordinate(y)) return std::nullopt;
    return AnimationGraphCanvasLayoutOperation{document_->asset_id, std::string(node_id), x, y};
}

std::optional<AnimationGraphCanvasPoint> AnimationGraphCanvasModel::graph_to_canvas(
    const float x, const float y) const noexcept {
    if (!bounded_coordinate(x) || !bounded_coordinate(y) ||
        !valid_view(pan_x_, pan_y_, zoom_)) return std::nullopt;
    const AnimationGraphCanvasPoint result{pan_x_ + x * zoom_, pan_y_ + y * zoom_};
    if (!finite(result.x) || !finite(result.y)) return std::nullopt;
    return result;
}

std::optional<AnimationGraphCanvasPoint> AnimationGraphCanvasModel::canvas_to_graph(
    const float x, const float y) const noexcept {
    if (!finite(x) || !finite(y) || !valid_view(pan_x_, pan_y_, zoom_)) return std::nullopt;
    const AnimationGraphCanvasPoint result{(x - pan_x_) / zoom_, (y - pan_y_) / zoom_};
    if (!bounded_coordinate(result.x) || !bounded_coordinate(result.y)) return std::nullopt;
    return result;
}

AnimationGraphCanvasHit AnimationGraphCanvasModel::hit_test(
    const float canvas_x, const float canvas_y, const float tolerance) const {
    AnimationGraphCanvasHit result;
    if (!finite(canvas_x) || !finite(canvas_y) || !finite(tolerance) || tolerance < 0.0F ||
        !bounded_coordinate(tolerance) || !valid_view(pan_x_, pan_y_, zoom_)) return result;
    const auto projection = project();
    if (!projection.valid) return result;
    const auto graph_point = canvas_to_graph(canvas_x, canvas_y);
    if (!graph_point) return result;
    const auto graph_tolerance = tolerance / projection.zoom;
    const auto tolerance_squared = graph_tolerance * graph_tolerance;

    // A port is the most specific target.  Keeping this pass separate from
    // node/edge testing makes a port hit deterministic even when it lies on a
    // node boundary or at an edge endpoint.
    auto best_distance = std::numeric_limits<float>::infinity();
    const AnimationGraphCanvasPort* best_port{};
    for (const auto& port : projection.ports) {
        if (!finite(port.x) || !finite(port.y) || !finite(port.radius) || port.radius < 0.0F)
            continue;
        const auto radius = port.radius + graph_tolerance;
        const auto distance = squared_distance(*graph_point, {port.x, port.y});
        if (distance <= radius * radius && distance < best_distance) {
            best_distance = distance;
            best_port = &port;
        }
    }
    if (best_port != nullptr) {
        result.kind = "port";
        result.node_id = best_port->node_id;
        result.port_id = best_port->port_id;
        result.distance = std::sqrt(best_distance) * projection.zoom;
        return result;
    }

    for (const auto& node : projection.nodes) {
        if (graph_point->x >= node.x && graph_point->x <= node.x + node.width &&
            graph_point->y >= node.y && graph_point->y <= node.y + node.height) {
            result.kind = "node";
            result.node_id = node.node_id;
            return result;
        }
    }

    best_distance = std::numeric_limits<float>::infinity();
    const AnimationGraphCanvasBlendEdge* best_edge{};
    for (const auto& edge : projection.blend_edges) {
        if (!finite(edge.from_x) || !finite(edge.from_y) || !finite(edge.to_x) || !finite(edge.to_y))
            continue;
        const auto distance = edge_distance_squared(edge, *graph_point);
        if (distance <= tolerance_squared && distance < best_distance) {
            best_distance = distance;
            best_edge = &edge;
        }
    }
    if (best_edge != nullptr) {
        result.kind = "edge";
        result.edge_id = best_edge->edge_id;
        result.blend_node_id = best_edge->blend_node_id;
        result.child_node_id = best_edge->child_node_id;
        result.distance = std::sqrt(best_distance) * projection.zoom;
    }
    return result;
}

} // namespace noemancer
