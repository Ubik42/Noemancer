#pragma once

#include "editor/project_settings_input_map.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace noemancer {

enum class ProjectSettingsInputMapCaptureState : std::uint8_t {
    idle,
    armed,
    captured,
    cancelled
};

// Runtime adapters translate their device-specific observation into this
// small Editor-domain value before handing it to the panel.
struct ProjectSettingsInputMapCaptureObservation final {
    ProjectSettingsInputMapCaptureState state{ProjectSettingsInputMapCaptureState::idle};
    std::uint64_t request_id{};
    std::string source;
    std::string device;
    float value{};
};

enum class ProjectSettingsInputMapPanelRequestKind : std::uint8_t {
    add_action,
    remove_action,
    add_binding,
    remove_binding,
    rebind_binding,
    begin_capture,
    cancel_capture
};

struct ProjectSettingsInputMapPanelRequest final {
    ProjectSettingsInputMapPanelRequestKind kind{ProjectSettingsInputMapPanelRequestKind::add_action};
    // Stable request identity. Domain edit requests reuse InputMapEditIntent::intent_id.
    std::string request_id;
    std::uint64_t base_revision{};
    std::uint64_t capture_request_id{};
    std::string action_id;
    std::string binding_id;
    std::string source;
    float scale{1.0F};
    float dead_zone{};
    InputActionKind action_kind{InputActionKind::button};
    std::optional<InputMapEditIntent> intent;
};

struct ProjectSettingsInputMapBindingDraft final {
    std::string action_id;
    std::string binding_id;
    std::string source;
    float scale{1.0F};
    float dead_zone{};
};

struct ProjectSettingsInputMapPanelState final {
    std::string selected_action_id;
    std::string selected_binding_id;
    std::string action_draft_id{"gameplay.new-action"};
    InputActionKind action_draft_kind{InputActionKind::button};
    std::string add_binding_source;
    float add_binding_scale{1.0F};
    float add_binding_dead_zone{};
    std::vector<ProjectSettingsInputMapBindingDraft> binding_drafts;
    ProjectSettingsInputMapCaptureObservation capture;
    std::string capture_action_id;
    std::string capture_binding_id;
    std::string last_error;
    bool has_pending_request{};
};

// The controller owns only transient authoring state. It never mutates a
// ProjectDocument or World; callers consume requests and perform the domain
// transaction themselves.
class ProjectSettingsInputMapPanel final {
public:
    explicit ProjectSettingsInputMapPanel(ProjectSettingsInputMapSnapshot snapshot);

    [[nodiscard]] const ProjectSettingsInputMapViewModel& view_model() const noexcept;
    [[nodiscard]] ProjectSettingsInputMapPanelState state() const;
    [[nodiscard]] std::optional<ProjectSettingsInputMapPanelRequest> consume_request();

    void set_snapshot(ProjectSettingsInputMapSnapshot snapshot);
    void set_capture_observation(ProjectSettingsInputMapCaptureObservation observation);

    [[nodiscard]] bool select_action(std::string_view action_id);
    [[nodiscard]] bool select_binding(std::string_view action_id, std::string_view binding_id);
    void set_action_draft(std::string action_id, InputActionKind kind);
    void set_add_binding_draft(std::string source, float scale, float dead_zone);
    [[nodiscard]] bool set_binding_draft(std::string_view action_id, std::string_view binding_id,
                                         std::string source, float scale, float dead_zone);
    [[nodiscard]] bool set_binding_scale(std::string_view action_id, std::string_view binding_id,
                                         float scale);
    [[nodiscard]] bool set_binding_dead_zone(std::string_view action_id, std::string_view binding_id,
                                             float dead_zone);

    [[nodiscard]] bool request_add_action();
    [[nodiscard]] bool request_remove_action();
    [[nodiscard]] bool request_add_binding();
    [[nodiscard]] bool request_remove_binding();
    [[nodiscard]] bool request_rebind_binding();
    [[nodiscard]] bool begin_rebind_capture();
    [[nodiscard]] bool request_cancel_capture();

    // This is the only method that touches ImGui. It can be skipped entirely
    // by headless tests and by callers that render the semantic document in a
    // different frontend.
    void render();

private:
    [[nodiscard]] const InputMapActionView* selected_action() const noexcept;
    [[nodiscard]] const InputMapBindingView* selected_binding() const noexcept;
    [[nodiscard]] ProjectSettingsInputMapBindingDraft* mutable_binding_draft(
        std::string_view action_id, std::string_view binding_id);
    [[nodiscard]] const ProjectSettingsInputMapBindingDraft* binding_draft(
        std::string_view action_id, std::string_view binding_id) const noexcept;
    [[nodiscard]] bool queue_request(ProjectSettingsInputMapPanelRequest request);
    [[nodiscard]] bool queue_edit(InputMapIntentResult result, ProjectSettingsInputMapPanelRequestKind kind);
    [[nodiscard]] std::uint64_t capture_request_id(std::string_view action_id,
                                                   std::string_view binding_id) const noexcept;
    void retain_selection();
    void sync_edit_buffers();

    ProjectSettingsInputMapViewModel view_model_;
    std::string selected_action_id_;
    std::string selected_binding_id_;
    std::string action_draft_id_{"gameplay.new-action"};
    InputActionKind action_draft_kind_{InputActionKind::button};
    std::string add_binding_source_;
    float add_binding_scale_{1.0F};
    float add_binding_dead_zone_{};
    std::vector<ProjectSettingsInputMapBindingDraft> binding_drafts_;
    ProjectSettingsInputMapCaptureObservation capture_;
    std::string capture_action_id_;
    std::string capture_binding_id_;
    std::uint64_t capture_request_id_{};
    std::string last_error_;
    std::optional<ProjectSettingsInputMapPanelRequest> pending_request_;
};

} // namespace noemancer
