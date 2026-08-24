#include "editor/project_settings_input_map.hpp"

#include <nlohmann/json.hpp>

#include <iostream>
#include <string>
#include <vector>

int main() {
    using namespace noemancer;
    const std::vector<InputActionDefinition> actions{
        {"gameplay.jump", InputActionKind::button, {{"keyboard.space", 1.0F, 0.0F}}},
        {"gameplay.move.x", InputActionKind::axis_1d,
         {{"keyboard.a", -1.0F, 0.0F}, {"keyboard.d", 1.0F, 0.0F}}},
        {"gameplay.menu", InputActionKind::button, {{"keyboard.space", 1.0F, 0.0F}}},
    };
    ProjectSettingsInputMapViewModel view("project.editor-test", "Editor Test", 7U, actions);
    if (view.actions().size() != actions.size() || view.diagnostics().empty() || !view.valid()) {
        std::cerr << "Input Map conflict was not diagnosed.\n";
        return 1;
    }

    const auto document = nlohmann::json::parse(view.semantic_ui_document_json());
    if (document.at("schemaVersion") != std::string(editor_project_settings_input_map_ui_schema) ||
        document.at("documentId") != "editor.project-settings.project.editor-test" ||
        document.at("nodes").at(2).at("id") != editor_project_settings_action_node_id("gameplay.jump") ||
        document.at("nodes").at(2).at("actions").at(0).at("id") !=
            std::string(editor_project_settings_input_map_remove_action_id)) {
        std::cerr << "Input Map semantic node/action IDs are not stable.\n";
        return 2;
    }
    const auto reordered = ProjectSettingsInputMapViewModel(
        "project.editor-test", "Editor Test", 7U,
        std::vector<InputActionDefinition>{actions.at(2), actions.at(0), actions.at(1)});
    const auto reordered_document = nlohmann::json::parse(reordered.semantic_ui_document_json());
    if (reordered_document.at("nodes").at(4).at("id") != editor_project_settings_action_node_id("gameplay.jump")) {
        std::cerr << "Action node identity changed when the authored array was reordered.\n";
        return 3;
    }

    const auto add_action = view.add_action_intent("gameplay.pause", InputActionKind::button);
    if (!add_action || !add_action.intent || add_action.intent->base_revision != 7U ||
        add_action.intent->intent_id != "editor.project-settings.input-map.intent.add-action.gameplay.pause") {
        std::cerr << "Add action intent contract is invalid.\n";
        return 4;
    }
    const auto add_binding = view.add_binding_intent("gameplay.jump", "keyboard.escape", 1.0F, 0.1F);
    if (!add_binding || !add_binding.intent || add_binding.intent->binding_id !=
        editor_project_settings_binding_id("gameplay.jump", "keyboard.escape")) {
        std::cerr << "Add binding intent contract is invalid.\n";
        return 5;
    }
    const auto binding_id = view.actions().at(0).bindings.at(0).id;
    const auto remove_binding = view.remove_binding_intent("gameplay.jump", binding_id);
    const auto rebind = view.rebind_binding_intent("gameplay.jump", binding_id, "keyboard.enter", 1.0F, 0.0F);
    if (!remove_binding || !rebind || rebind.intent->binding_id != binding_id ||
        rebind.intent->previous_source != "keyboard.space" ||
        nlohmann::json::parse(input_map_intent_json(*rebind.intent)).at("baseRevision") != 7U) {
        std::cerr << "Remove/rebind intent contract is invalid.\n";
        return 6;
    }
    if (view.add_action_intent("gameplay/invalid", InputActionKind::button).code !=
        "editor.input-map.invalid-action-id") {
        std::cerr << "Invalid intent did not return a stable error code.\n";
        return 7;
    }
    const auto authoring = nlohmann::json::parse(view.authoring_json());
    if (authoring.at("schemaVersion") != std::string(editor_project_settings_input_map_schema) ||
        authoring.at("documentSchema") != std::string(editor_project_settings_input_map_ui_schema)) {
        std::cerr << "Authoring schema contract is invalid.\n";
        return 8;
    }
    return 0;
}
