#pragma once

#include "engine/physics_constraint_types.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace noemancer {

// The panel is a headless projection over the engine-owned constraint
// contract.  It does not own a World, a scene document, or a native physics
// handle.  The surrounding editor/World transaction authority consumes the
// revision-bound requests emitted below.
inline constexpr std::string_view physics_constraint_panel_schema =
    "noemancer.physics-constraint-panel/0.1";
inline constexpr std::string_view physics_constraint_panel_node_id =
    "editor.physics.constraint-relationship-bench";
inline constexpr std::string_view physics_constraint_panel_create_id =
    "editor.physics.constraint-relationship-bench.create-draft";
inline constexpr std::string_view physics_constraint_panel_upsert_id =
    "editor.physics.constraint-relationship-bench.upsert";
inline constexpr std::string_view physics_constraint_panel_upsert_dry_run_id =
    "editor.physics.constraint-relationship-bench.upsert-dry-run";
inline constexpr std::string_view physics_constraint_panel_remove_id =
    "editor.physics.constraint-relationship-bench.remove";

inline constexpr std::size_t physics_constraint_panel_max_constraints = 1024U;
inline constexpr std::size_t physics_constraint_panel_max_entity_choices = 4096U;

// Entity names are intentionally a separate, plain-data list.  A World can
// use a stable GUID as entity_id while still presenting a readable scene name
// to a human author.
struct PhysicsConstraintEntityOption final {
    std::string entity_id;
    std::string display_name;
};

struct PhysicsConstraintPanelSnapshot final {
    std::uint64_t world_revision{1U};
    std::string manager_id{"world.physics"};
    std::vector<PhysicsConstraintSpec> constraints;
    std::vector<PhysicsConstraintEntityOption> rigid_bodies;
};

struct PhysicsConstraintPanelDiagnostic final {
    std::string code;
    std::string path;
    std::string message;
};

struct PhysicsConstraintPanelValidation final {
    bool valid{};
    std::vector<PhysicsConstraintPanelDiagnostic> diagnostics;
};

enum class PhysicsConstraintPanelRequestKind : std::uint8_t {
    upsert,
    remove,
};

[[nodiscard]] std::string_view physics_constraint_panel_request_kind_name(
    PhysicsConstraintPanelRequestKind kind) noexcept;

// This record is the only mutation seam exposed by the panel.  The manager
// must compare base_revision before applying it; dry_run keeps the same shape
// for validation previews and actual commits.
struct PhysicsConstraintPanelRequest final {
    PhysicsConstraintPanelRequestKind kind{PhysicsConstraintPanelRequestKind::upsert};
    std::string request_id;
    std::string manager_id{"world.physics"};
    std::uint64_t base_revision{};
    bool dry_run{};
    std::string constraint_id;
    std::optional<PhysicsConstraintSpec> constraint;
};

struct PhysicsConstraintPanelState final {
    PhysicsConstraintPanelSnapshot snapshot;
    std::string selected_constraint_id;
    std::optional<PhysicsConstraintSpec> draft;
    PhysicsConstraintPanelValidation validation;
    bool can_upsert{};
    bool can_remove{};
    bool has_pending_request{};
    std::optional<PhysicsConstraintPanelRequest> pending_request;
    std::string last_error;
};

// Headless-first relationship editor model.  All authoring methods mutate
// only an in-progress draft; persistence and native physics remain outside.
// The semantic JSON projection is intentionally useful to both a retained UI
// and a tool caller without requiring an ImGui context.
class PhysicsConstraintPanel final {
public:
    explicit PhysicsConstraintPanel(PhysicsConstraintPanelSnapshot snapshot = {});

    [[nodiscard]] const PhysicsConstraintPanelSnapshot& snapshot() const noexcept;
    [[nodiscard]] const std::optional<PhysicsConstraintSpec>& draft() const noexcept;
    [[nodiscard]] const PhysicsConstraintPanelValidation& validation() const noexcept;
    [[nodiscard]] std::string_view selected_constraint_id() const noexcept;
    [[nodiscard]] std::string_view last_error() const noexcept;
    [[nodiscard]] PhysicsConstraintPanelState state() const;

    // A new World snapshot invalidates pending requests from an older
    // revision.  Selection is retained by stable constraint ID when possible.
    void set_snapshot(PhysicsConstraintPanelSnapshot snapshot);

    [[nodiscard]] bool select_constraint(std::string_view constraint_id);
    void clear_selection();
    [[nodiscard]] bool set_draft(std::optional<PhysicsConstraintSpec> draft);
    [[nodiscard]] bool create_draft(PhysicsConstraintType type,
                                    std::string constraint_id = {});
    void clear_draft();

    [[nodiscard]] bool set_constraint_id(std::string constraint_id);
    [[nodiscard]] bool set_constraint_type(PhysicsConstraintType type);
    [[nodiscard]] bool set_body_a(std::string entity_id);
    [[nodiscard]] bool set_body_b(std::string entity_id);
    [[nodiscard]] bool set_enabled(bool enabled);
    [[nodiscard]] bool set_anchor_a(PhysicsConstraintVec3 value);
    [[nodiscard]] bool set_anchor_b(PhysicsConstraintVec3 value);
    [[nodiscard]] bool set_primary_axis_a(PhysicsConstraintVec3 value);
    [[nodiscard]] bool set_secondary_axis_a(PhysicsConstraintVec3 value);
    [[nodiscard]] bool set_primary_axis_b(PhysicsConstraintVec3 value);
    [[nodiscard]] bool set_secondary_axis_b(PhysicsConstraintVec3 value);
    [[nodiscard]] bool set_lower_limit(float value);
    [[nodiscard]] bool set_upper_limit(float value);
    [[nodiscard]] bool set_rest_length(float value);
    [[nodiscard]] bool set_spring_frequency_hz(float value);
    [[nodiscard]] bool set_spring_damping_ratio(float value);

    [[nodiscard]] bool request_upsert(bool dry_run = false);
    [[nodiscard]] bool request_remove(bool dry_run = false);
    [[nodiscard]] std::optional<PhysicsConstraintPanelRequest> consume_request();

    // Deterministic, bounded projection for the declarative editor UI and
    // context/observation consumers.  It contains no native backend types.
    [[nodiscard]] std::string semantic_state_json() const;
    [[nodiscard]] std::string state_json() const { return semantic_state_json(); }

private:
    void normalize_snapshot();
    void rebuild_projection();
    void set_error(std::string message);
    [[nodiscard]] bool ensure_draft();
    [[nodiscard]] bool queue_request(PhysicsConstraintPanelRequest request);
    [[nodiscard]] std::string make_request_id(
        PhysicsConstraintPanelRequestKind kind,
        const std::string& constraint_id,
        const std::optional<PhysicsConstraintSpec>& constraint,
        bool dry_run) const;
    [[nodiscard]] bool has_entity(std::string_view entity_id) const noexcept;
    [[nodiscard]] bool has_constraint(std::string_view constraint_id) const noexcept;
    [[nodiscard]] bool is_existing_draft() const noexcept;

    PhysicsConstraintPanelSnapshot snapshot_;
    std::string selected_constraint_id_;
    std::optional<PhysicsConstraintSpec> draft_;
    PhysicsConstraintPanelValidation validation_;
    std::optional<PhysicsConstraintPanelRequest> pending_request_;
    std::string last_error_;
    std::size_t source_constraint_count_{};
    std::size_t source_entity_choice_count_{};
};

} // namespace noemancer
