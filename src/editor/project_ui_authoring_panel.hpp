#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace noemancer {

// A deliberately small, editor-owned view of a project UI document.  The
// editor never hands nlohmann::json (or a World object) across this boundary:
// the owner of the project session consumes the plain request below and
// decides whether to commit it.
struct ProjectUiAuthoringSnapshot final {
    std::string document_json;
    std::uint64_t revision{};
    std::string fingerprint;
    bool can_undo{};
    bool can_redo{};
};

enum class ProjectUiAuthoringNodeKind : std::uint8_t {
    unknown,
    container,
    text,
    button,
    property
};

[[nodiscard]] const char* project_ui_authoring_node_kind_name(ProjectUiAuthoringNodeKind kind) noexcept;

// The editor exposes status per authored field so a human or Agent can tell
// whether a value is invalid, locally dirty, blocked by another request, or
// conflicted with a newer document revision.  These are editor observations;
// they are never persisted into the project UI source document.
enum class ProjectUiAuthoringFieldKind : std::uint8_t {
    label,
    role,
    parent,
    action_id,
    binding,
    state,
    presentation,
    value,
    component_ref,
    component_declaration,
    design_tokens
};

[[nodiscard]] const char* project_ui_authoring_field_kind_name(ProjectUiAuthoringFieldKind kind) noexcept;

enum class ProjectUiAuthoringJsonKind : std::uint8_t {
    absent,
    object,
    array,
    scalar,
    invalid
};

struct ProjectUiAuthoringFieldState final {
    ProjectUiAuthoringFieldKind field{ProjectUiAuthoringFieldKind::label};
    bool valid{true};
    bool dirty{};
    bool disabled{};
    bool pending{};
    bool conflict{};
    std::string error;
};

struct ProjectUiAuthoringJsonDraft final {
    std::string json;
    bool present{};
    ProjectUiAuthoringJsonKind kind{ProjectUiAuthoringJsonKind::absent};
    ProjectUiAuthoringFieldState status;
};

struct ProjectUiAuthoringDiagnostic final {
    std::string code;
    std::string path;
    std::string node_id;
    std::string detail;
};

struct ProjectUiAuthoringNode final {
    std::string id;
    std::string parent_id;
    std::string role;
    std::string label;
    std::string action_id;
    std::string binding_json{"{}"};
    std::string state_json{"{}"};
    std::string presentation_json{"{}"};
    std::string value_json{"null"};
    std::string component_ref;
    ProjectUiAuthoringNodeKind kind{ProjectUiAuthoringNodeKind::unknown};
    std::size_t depth{};
    std::size_t child_count{};
    bool binding_valid{true};
    bool state_valid{true};
    bool presentation_valid{true};
    bool value_valid{true};
    ProjectUiAuthoringJsonKind state_kind{ProjectUiAuthoringJsonKind::object};
    ProjectUiAuthoringJsonKind presentation_kind{ProjectUiAuthoringJsonKind::object};
    ProjectUiAuthoringJsonKind value_kind{ProjectUiAuthoringJsonKind::absent};
    std::vector<ProjectUiAuthoringFieldState> fields;
};

struct ProjectUiAuthoringComponentDeclaration final {
    std::string id;
    std::string root_node_id;
    std::string label;
    std::string component_json{"{}"};
    bool valid{true};
    ProjectUiAuthoringFieldState status;
};

struct ProjectUiAuthoringView final {
    std::string schema_version{"noemancer.ui-document/0.1"};
    std::string document_id;
    bool valid{};
    std::vector<std::string> root_ids;
    std::vector<ProjectUiAuthoringNode> nodes;
    std::string design_tokens_json{"{}"};
    bool design_tokens_valid{true};
    ProjectUiAuthoringJsonKind design_tokens_kind{ProjectUiAuthoringJsonKind::object};
    ProjectUiAuthoringFieldState design_tokens_status;
    std::vector<ProjectUiAuthoringComponentDeclaration> components;
    std::vector<ProjectUiAuthoringDiagnostic> diagnostics;
};

struct ProjectUiAuthoringNodeDraft final {
    std::string id;
    std::string parent_id;
    std::string role;
    std::string label;
    std::string action_id;
    std::string binding_json{"{}"};
    std::string state_json{"{}"};
    std::string presentation_json{"{}"};
    std::string value_json{"null"};
    std::string component_ref;
};

enum class ProjectUiAuthoringPanelRequestKind : std::uint8_t {
    add_node,
    remove_node,
    update_node,
    reparent_node,
    undo,
    redo,
    update_design_tokens,
    add_component_declaration,
    update_component_declaration,
    remove_component_declaration
};

[[nodiscard]] const char* project_ui_authoring_request_kind_name(
    ProjectUiAuthoringPanelRequestKind kind) noexcept;

struct ProjectUiAuthoringPanelRequest final {
    ProjectUiAuthoringPanelRequestKind kind{ProjectUiAuthoringPanelRequestKind::update_node};
    // The identity is deterministic for the request payload and source
    // revision.  A session can safely de-duplicate retries by this value.
    std::string request_id;
    std::uint64_t base_revision{};
    ProjectUiAuthoringNodeKind node_kind{ProjectUiAuthoringNodeKind::unknown};
    std::string node_id;
    std::string parent_id;
    std::string role;
    std::string label;
    std::string action_id;
    std::string binding_json{"{}"};
    std::string state_json{"{}"};
    std::string presentation_json{"{}"};
    std::string value_json{"null"};
    std::string design_tokens_json{"{}"};
    std::string component_id;
    std::string component_ref;
    std::string component_json{"{}"};
};

struct ProjectUiAuthoringPanelState final {
    std::uint64_t revision{};
    std::string fingerprint;
    std::string selected_node_id;
    ProjectUiAuthoringNodeDraft selected_draft;
    ProjectUiAuthoringNodeDraft add_draft;
    ProjectUiAuthoringNodeKind add_kind{ProjectUiAuthoringNodeKind::container};
    std::string design_tokens_json{"{}"};
    ProjectUiAuthoringFieldState design_tokens_status;
    std::vector<ProjectUiAuthoringFieldState> selected_fields;
    bool can_undo{};
    bool can_redo{};
    bool has_pending_request{};
    std::string pending_request_id;
    std::string last_error;
};

// Headless controller for the project UI authoring surface.  It owns only a
// bounded projection of the source document and transient editor drafts.
// Mutations are emitted as revision-bound requests; this class never writes a
// document and never mutates a World.
class ProjectUiAuthoringPanel final {
public:
    explicit ProjectUiAuthoringPanel(ProjectUiAuthoringSnapshot snapshot);

    [[nodiscard]] const ProjectUiAuthoringView& view() const noexcept;
    [[nodiscard]] ProjectUiAuthoringPanelState state() const;
    [[nodiscard]] const std::vector<ProjectUiAuthoringDiagnostic>& diagnostics() const noexcept;
    [[nodiscard]] std::string_view last_error() const noexcept;
    [[nodiscard]] std::optional<ProjectUiAuthoringPanelRequest> consume_request();

    void set_snapshot(ProjectUiAuthoringSnapshot snapshot);

    [[nodiscard]] bool select_node(std::string_view node_id);
    [[nodiscard]] bool set_selected_draft(ProjectUiAuthoringNodeDraft draft);
    [[nodiscard]] bool set_label(std::string label);
    [[nodiscard]] bool set_role(std::string role);
    [[nodiscard]] bool set_parent(std::string parent_id);
    [[nodiscard]] bool set_action_id(std::string action_id);
    [[nodiscard]] bool set_binding_json(std::string binding_json);
    [[nodiscard]] bool set_state_json(std::string state_json);
    [[nodiscard]] bool set_presentation_json(std::string presentation_json);
    [[nodiscard]] bool set_value_json(std::string value_json);
    [[nodiscard]] bool set_component_ref(std::string component_ref);
    [[nodiscard]] bool set_component_declaration_json(std::string component_id,
                                                      std::string component_json);
    [[nodiscard]] bool set_design_tokens_json(std::string design_tokens_json);
    void set_add_node_draft(ProjectUiAuthoringNodeKind kind, std::string id,
                            std::string role, std::string label, std::string parent_id = {},
                            std::string action_id = {}, std::string binding_json = "{}");
    void set_add_node_component_ref(std::string component_ref);

    [[nodiscard]] bool request_add_node();
    [[nodiscard]] bool request_remove_node();
    [[nodiscard]] bool request_update_node();
    [[nodiscard]] bool request_reparent_node();
    [[nodiscard]] bool request_undo();
    [[nodiscard]] bool request_redo();
    [[nodiscard]] bool request_update_design_tokens();
    [[nodiscard]] bool request_add_component_declaration(std::string component_id = {});
    [[nodiscard]] bool request_update_component_declaration(std::string component_id = {});
    [[nodiscard]] bool request_remove_component_declaration(std::string component_id = {});

    // Agent-facing, bounded semantic projection.  The returned JSON contains
    // plain fields and diagnostics and is independent of the ImGui lifecycle.
    [[nodiscard]] std::string semantic_snapshot_json() const;

    // Optional visual frontend.  Headless callers and tests need not call it.
    void render();

private:
    [[nodiscard]] const ProjectUiAuthoringNode* selected_node() const noexcept;
    [[nodiscard]] bool queue_request(ProjectUiAuthoringPanelRequest request);
    [[nodiscard]] std::string make_request_id(const ProjectUiAuthoringPanelRequest& request) const;
    [[nodiscard]] bool validate_binding_json(std::string_view binding_json, std::string_view field = "binding");
    [[nodiscard]] bool validate_object_json(std::string_view json, std::string_view field);
    [[nodiscard]] bool validate_any_json(std::string_view json, std::string_view field);
    [[nodiscard]] ProjectUiAuthoringFieldState field_state(
        const ProjectUiAuthoringNode& node, ProjectUiAuthoringFieldKind field) const;
    [[nodiscard]] bool field_is_dirty(const ProjectUiAuthoringNode& node,
                                      ProjectUiAuthoringFieldKind field) const;
    [[nodiscard]] bool field_is_pending(const ProjectUiAuthoringNode& node,
                                        ProjectUiAuthoringFieldKind field) const;
    [[nodiscard]] bool field_is_conflicted(std::string_view node_id,
                                           ProjectUiAuthoringFieldKind field) const;
    [[nodiscard]] std::string field_key(std::string_view node_id,
                                        ProjectUiAuthoringFieldKind field) const;
    void mark_conflict_for_request(const ProjectUiAuthoringPanelRequest& request);
    void mark_dirty_conflicts(std::uint64_t next_revision);
    void set_field_error(std::string_view node_id, ProjectUiAuthoringFieldKind field,
                         std::string error);
    void clear_field_error(std::string_view node_id, ProjectUiAuthoringFieldKind field);
    void set_design_tokens_error(std::string error);
    void clear_design_tokens_error();
    void parse_document();
    void retain_selection();
    void sync_selected_draft();
    void discard_stale_drafts();

    ProjectUiAuthoringSnapshot snapshot_;
    ProjectUiAuthoringView view_;
    std::string selected_node_id_;
    ProjectUiAuthoringNodeKind add_kind_{ProjectUiAuthoringNodeKind::container};
    ProjectUiAuthoringNodeDraft add_draft_;
    std::map<std::string, ProjectUiAuthoringNodeDraft> drafts_;
    std::optional<ProjectUiAuthoringPanelRequest> pending_request_;
    std::map<std::string, std::string> field_errors_;
    std::set<std::string> conflicted_fields_;
    std::map<std::string, std::string> component_drafts_;
    std::string selected_component_id_;
    std::string new_component_id_{"component.new"};
    std::string design_tokens_draft_{"{}"};
    std::string design_tokens_error_;
    std::string last_error_;
};

} // namespace noemancer
