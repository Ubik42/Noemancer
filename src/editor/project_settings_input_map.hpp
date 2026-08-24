#pragma once

#include "engine/gameplay_runtime.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace noemancer {

inline constexpr std::string_view editor_project_settings_input_map_schema =
    "noemancer.editor-input-map-authoring/0.1";
inline constexpr std::string_view editor_project_settings_input_map_ui_schema =
    "noemancer.ui-document/0.1";
inline constexpr std::string_view editor_project_settings_node_id =
    "editor.project-settings";
inline constexpr std::string_view editor_project_settings_input_map_node_id =
    "editor.project-settings.input-map";
inline constexpr std::string_view editor_project_settings_input_map_add_action_id =
    "editor.project-settings.input-map.add-action";
inline constexpr std::string_view editor_project_settings_input_map_remove_action_id =
    "editor.project-settings.input-map.remove-action";
inline constexpr std::string_view editor_project_settings_input_map_add_binding_id =
    "editor.project-settings.input-map.add-binding";
inline constexpr std::string_view editor_project_settings_input_map_remove_binding_id =
    "editor.project-settings.input-map.remove-binding";
inline constexpr std::string_view editor_project_settings_input_map_rebind_id =
    "editor.project-settings.input-map.rebind";

enum class InputMapIntentKind : std::uint8_t {
    add_action,
    remove_action,
    add_binding,
    remove_binding,
    rebind_binding
};

struct ProjectSettingsInputMapSnapshot final {
    std::string project_id{"project.unknown"};
    std::string project_name{"Project Settings"};
    std::uint64_t revision{1};
    std::vector<InputActionDefinition> actions;
};

struct InputMapBindingView final {
    // The id is derived from the owning action and the authored source. It is
    // deliberately independent of the binding's position in the array.
    std::string id;
    std::string node_id;
    std::string source;
    float scale{1.0F};
    float dead_zone{};
    bool has_conflict{};
    std::vector<std::string> diagnostic_ids;
};

struct InputMapActionView final {
    std::string id;
    std::string node_id;
    InputActionKind kind{InputActionKind::button};
    std::vector<InputMapBindingView> bindings;
    std::vector<std::string> diagnostic_ids;
};

struct InputMapConflictDiagnostic final {
    std::string id;
    std::string code;
    std::string severity;
    std::string detail;
    std::string source;
    std::string action_id;
    std::string binding_id;
    std::string related_action_id;
    std::string related_binding_id;
};

struct InputMapEditIntent final {
    InputMapIntentKind kind{InputMapIntentKind::add_action};
    // Stable across repeated rendering of the same request. This is an
    // intent identity, not a transaction receipt or a generated array index.
    std::string intent_id;
    std::uint64_t base_revision{};
    std::string action_id;
    std::string binding_id;
    InputActionKind action_kind{InputActionKind::button};
    // The currently persisted source. Rebind intents keep this separate from
    // source, which is the requested replacement.
    std::string previous_source;
    std::string source;
    float scale{1.0F};
    float dead_zone{};
};

struct InputMapIntentResult final {
    bool success{};
    std::string code;
    std::string detail;
    std::optional<InputMapEditIntent> intent;

    [[nodiscard]] explicit operator bool() const noexcept { return success; }
};

// These helpers are also used by callers that need to retain selection while
// the declarative document is regenerated.
[[nodiscard]] std::string editor_project_settings_document_id(std::string_view project_id);
[[nodiscard]] std::string editor_project_settings_action_node_id(std::string_view action_id);
[[nodiscard]] std::string editor_project_settings_binding_id(std::string_view action_id,
                                                              std::string_view source);
[[nodiscard]] std::string editor_project_settings_binding_node_id(std::string_view action_id,
                                                                   std::string_view source);

[[nodiscard]] std::string input_map_intent_kind_name(InputMapIntentKind kind);
[[nodiscard]] std::string input_map_action_kind_name(InputActionKind kind);
[[nodiscard]] std::string input_map_intent_json(const InputMapEditIntent& intent);

class ProjectSettingsInputMapViewModel final {
public:
    explicit ProjectSettingsInputMapViewModel(ProjectSettingsInputMapSnapshot snapshot);
    ProjectSettingsInputMapViewModel(std::string project_id, std::string project_name,
                                     std::uint64_t revision,
                                     std::span<const InputActionDefinition> actions);

    [[nodiscard]] const ProjectSettingsInputMapSnapshot& snapshot() const noexcept;
    [[nodiscard]] const std::vector<InputMapActionView>& actions() const noexcept;
    [[nodiscard]] const std::vector<InputMapConflictDiagnostic>& diagnostics() const noexcept;
    [[nodiscard]] bool valid() const noexcept;

    // Returns a semantic, retained-UI-compatible document. It only describes
    // the authoring surface; applying an intent remains a domain transaction.
    [[nodiscard]] std::string semantic_ui_document_json(std::string_view locale = "en-US") const;
    [[nodiscard]] std::string authoring_json(std::string_view locale = "en-US") const;

    [[nodiscard]] InputMapIntentResult add_action_intent(std::string_view action_id,
                                                          InputActionKind kind,
                                                          std::string_view initial_source = {},
                                                          float scale = 1.0F,
                                                          float dead_zone = 0.0F) const;
    [[nodiscard]] InputMapIntentResult remove_action_intent(std::string_view action_id) const;
    [[nodiscard]] InputMapIntentResult add_binding_intent(std::string_view action_id,
                                                           std::string_view source,
                                                           float scale = 1.0F,
                                                           float dead_zone = 0.0F) const;
    [[nodiscard]] InputMapIntentResult remove_binding_intent(std::string_view action_id,
                                                              std::string_view binding_id) const;
    [[nodiscard]] InputMapIntentResult rebind_binding_intent(std::string_view action_id,
                                                              std::string_view binding_id,
                                                              std::string_view source,
                                                              float scale = 1.0F,
                                                              float dead_zone = 0.0F) const;

private:
    void rebuild();

    ProjectSettingsInputMapSnapshot snapshot_;
    std::vector<InputMapActionView> actions_;
    std::vector<InputMapConflictDiagnostic> diagnostics_;
};

} // namespace noemancer
