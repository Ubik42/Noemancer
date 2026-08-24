#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
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
    ProjectUiAuthoringNodeKind kind{ProjectUiAuthoringNodeKind::unknown};
    std::size_t depth{};
    std::size_t child_count{};
    bool binding_valid{true};
};

struct ProjectUiAuthoringView final {
    std::string schema_version{"noemancer.ui-document/0.1"};
    std::string document_id;
    bool valid{};
    std::vector<std::string> root_ids;
    std::vector<ProjectUiAuthoringNode> nodes;
    std::vector<ProjectUiAuthoringDiagnostic> diagnostics;
};

struct ProjectUiAuthoringNodeDraft final {
    std::string id;
    std::string parent_id;
    std::string role;
    std::string label;
    std::string action_id;
    std::string binding_json{"{}"};
};

enum class ProjectUiAuthoringPanelRequestKind : std::uint8_t {
    add_node,
    remove_node,
    update_node,
    reparent_node,
    undo,
    redo
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
};

struct ProjectUiAuthoringPanelState final {
    std::uint64_t revision{};
    std::string fingerprint;
    std::string selected_node_id;
    ProjectUiAuthoringNodeDraft selected_draft;
    ProjectUiAuthoringNodeDraft add_draft;
    ProjectUiAuthoringNodeKind add_kind{ProjectUiAuthoringNodeKind::container};
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
    void set_add_node_draft(ProjectUiAuthoringNodeKind kind, std::string id,
                            std::string role, std::string label, std::string parent_id = {},
                            std::string action_id = {}, std::string binding_json = "{}");

    [[nodiscard]] bool request_add_node();
    [[nodiscard]] bool request_remove_node();
    [[nodiscard]] bool request_update_node();
    [[nodiscard]] bool request_reparent_node();
    [[nodiscard]] bool request_undo();
    [[nodiscard]] bool request_redo();

    // Agent-facing, bounded semantic projection.  The returned JSON contains
    // plain fields and diagnostics and is independent of the ImGui lifecycle.
    [[nodiscard]] std::string semantic_snapshot_json() const;

    // Optional visual frontend.  Headless callers and tests need not call it.
    void render();

private:
    [[nodiscard]] const ProjectUiAuthoringNode* selected_node() const noexcept;
    [[nodiscard]] bool queue_request(ProjectUiAuthoringPanelRequest request);
    [[nodiscard]] std::string make_request_id(const ProjectUiAuthoringPanelRequest& request) const;
    [[nodiscard]] bool validate_binding_json(std::string_view binding_json);
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
    std::string last_error_;
};

} // namespace noemancer
