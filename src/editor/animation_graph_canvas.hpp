#pragma once

#include "engine/animation_graph.hpp"

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace noemancer {

// The Canvas is a renderer-neutral projection over a caller-owned Graph
// document.  It deliberately stores only ephemeral view state and selection;
// graph nodes, edges and persisted editor positions are always read from the
// AnimationGraphDocument authority.
inline constexpr std::string_view animation_graph_canvas_schema =
    "noemancer.animation-graph-canvas/0.2";
inline constexpr std::string_view animation_graph_canvas_operation_schema =
    "noemancer.animation-graph-canvas-operation/0.1";

inline constexpr std::size_t animation_graph_canvas_max_nodes = 256U;
inline constexpr std::size_t animation_graph_canvas_max_blend_edges = 8192U;
inline constexpr std::size_t animation_graph_canvas_max_layer_roots = 16U;
inline constexpr std::size_t animation_graph_canvas_max_selection = 256U;

struct AnimationGraphCanvasNodeBounds final {
    std::string node_id;
    std::string kind;
    float x{};
    float y{};
    float width{};
    float height{};
    bool selected{};
    // Persisted editor state is projected without making the Canvas the
    // document authority.  In schema 0.2 this is a semantic authoring flag;
    // geometry intentionally retains every child row and port so observations,
    // edges, and hit targets do not disappear for non-visual consumers.
    bool collapsed{};
};

struct AnimationGraphCanvasPort final {
    std::string node_id;
    std::string port_id;
    std::string direction; // "input" or "output".
    std::string kind;      // "pose" or "blend-child".
    std::size_t index{};
    float x{};
    float y{};
    float radius{8.0F};
};

struct AnimationGraphCanvasBlendEdge final {
    std::string blend_node_id;
    std::string child_node_id;
    float threshold{};
    // Geometry is in Graph-space and follows pose flow: the child output is
    // the source and the Blend input is the target.  blend_node_id and
    // child_node_id retain the structural relationship used by the 0.1 API.
    // A renderer applies graph_to_canvas() to place geometry in its viewport.
    float from_x{};
    float from_y{};
    float to_x{};
    float to_y{};
    std::string from_port_id;
    std::string to_port_id;
    std::string edge_id;
};

struct AnimationGraphCanvasPoint final {
    float x{};
    float y{};
};

struct AnimationGraphCanvasHit final {
    // kind is one of "node", "port", "edge" or "empty".  Empty is an
    // explicit result so callers do not have to infer it from an optional.
    std::string kind{"empty"};
    std::string node_id;
    std::string port_id;
    std::string edge_id;
    std::string blend_node_id;
    std::string child_node_id;
    // Distance from the queried Canvas-space point, measured in Canvas-space
    // pixels.  Node hits are inside the bounds and therefore report zero.
    float distance{};
};

struct AnimationGraphCanvasLayerRoot final {
    std::string layer_id;
    std::string root_node_id;
    std::string mode;
    float weight{};
};

// A drag produces plain data for the existing Graph authoring authority to
// validate and apply.  The Canvas never changes the source document itself.
struct AnimationGraphCanvasLayoutOperation final {
    std::string asset_id;
    std::string node_id;
    float x{};
    float y{};

    [[nodiscard]] std::string to_json() const;
};

struct AnimationGraphCanvasProjection final {
    bool valid{};
    std::string code;
    std::string detail;
    std::string asset_id;
    float pan_x{};
    float pan_y{};
    float zoom{1.0F};
    std::vector<AnimationGraphCanvasNodeBounds> nodes;
    std::vector<AnimationGraphCanvasBlendEdge> blend_edges;
    std::vector<AnimationGraphCanvasLayerRoot> layer_roots;
    std::vector<std::string> selected_node_ids;
    std::vector<AnimationGraphCanvasPort> ports;

    [[nodiscard]] std::string to_json() const;
};

class AnimationGraphCanvasModel final {
public:
    explicit AnimationGraphCanvasModel(const AnimationGraphDocument* document = nullptr) noexcept;
    explicit AnimationGraphCanvasModel(const AnimationGraphDocument& document) noexcept
        : AnimationGraphCanvasModel(&document) {}

    // The caller owns the document and must keep it alive while this model is
    // used.  Binding does not copy or mutate the document.
    void bind(const AnimationGraphDocument* document) noexcept;
    [[nodiscard]] const AnimationGraphDocument* document() const noexcept;

    [[nodiscard]] AnimationGraphCanvasProjection project() const;
    [[nodiscard]] std::string projection_json() const;

    // Selection and view state are ephemeral Canvas state.  They are bounded
    // and are never written into AnimationGraphDocument::editor.
    [[nodiscard]] bool select_node(std::string_view node_id, bool additive = false) noexcept;
    [[nodiscard]] bool deselect_node(std::string_view node_id) noexcept;
    void clear_selection() noexcept;
    [[nodiscard]] const std::vector<std::string>& selection() const noexcept;

    [[nodiscard]] bool set_view(float pan_x, float pan_y, float zoom) noexcept;
    [[nodiscard]] bool pan_by(float delta_x, float delta_y) noexcept;
    void reset_view_to_document() noexcept;

    // Coordinates are Graph-space positions.  The returned operation is
    // suitable for an adapter that routes it through the Graph patch/undo
    // authority; this method never updates the source document.
    [[nodiscard]] std::optional<AnimationGraphCanvasLayoutOperation> drag_to_layout(
        std::string_view node_id, float x, float y) const;

    // Coordinates are local Canvas-space values: pan is applied after the
    // Graph-space coordinate is scaled by zoom.  These helpers deliberately
    // contain no renderer or ImGui types and reject non-finite/out-of-bounds
    // values rather than producing an unbounded interaction target.
    [[nodiscard]] std::optional<AnimationGraphCanvasPoint> graph_to_canvas(
        float x, float y) const noexcept;
    [[nodiscard]] std::optional<AnimationGraphCanvasPoint> canvas_to_graph(
        float x, float y) const noexcept;

    // Returns a deterministic bounded target.  Ports win over nodes, nodes
    // win over edges, and ties preserve projection order.  The tolerance is
    // expressed in Canvas-space pixels and is converted through the current
    // zoom before testing Graph-space geometry.  Returned distances are in
    // the same Canvas-space units.
    [[nodiscard]] AnimationGraphCanvasHit hit_test(
        float canvas_x, float canvas_y, float tolerance = 6.0F) const;

private:
    [[nodiscard]] bool has_node(std::string_view node_id) const noexcept;

    const AnimationGraphDocument* document_{};
    std::vector<std::string> selection_;
    float pan_x_{};
    float pan_y_{};
    float zoom_{1.0F};
};

} // namespace noemancer
