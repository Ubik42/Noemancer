#include "editor/project_ui_authoring_panel.hpp"

#include <nlohmann/json.hpp>

#include <iostream>
#include <string>

namespace {

using noemancer::ProjectUiAuthoringSnapshot;

ProjectUiAuthoringSnapshot snapshot(const std::uint64_t revision = 17U) {
    return {
        .document_json = R"json({
            "schemaVersion":"noemancer.ui-document/0.1",
            "documentId":"editor.project-ui",
            "revision":17,
            "designTokens":{"accent":"#62d7ff","surface":"#0b1018"},
            "components":[{"id":"component.button","rootNodeId":"ui.play","label":"Button","nodes":[]}],
            "nodes":[
                {"id":"ui.root","parentId":null,"role":"container","label":"Root"},
                {"id":"ui.title","parentId":"ui.root","role":"text","label":"Title"},
                {"id":"ui.play","parentId":"ui.root","role":"button","label":"Play",
                 "actionId":"game.start","binding":{"kind":"display-value"},
                 "actions":[{"id":"game.start","binding":{"kind":"command","target":"game.start"}}],
                 "state":{"visible":true,"enabled":true},"presentation":{"control":"primary"},
                 "value":"Ready","componentRef":"component.button"},
                {"id":"ui.health","parentId":"ui.root","role":"property","label":"Health",
                 "binding":{"kind":"world-property","property":"health"}}
            ]
        })json",
        .revision = revision,
        .fingerprint = "fnv1a64:project-ui",
        .can_undo = true,
        .can_redo = true};
}

bool contains(std::string_view source, std::string_view needle) {
    return source.find(needle) != std::string_view::npos;
}

} // namespace

int main() {
    using namespace noemancer;

    ProjectUiAuthoringPanel panel(snapshot());
    if (!panel.view().valid || panel.view().nodes.size() != 4U || panel.view().root_ids.size() != 1U ||
        panel.view().components.size() != 1U || panel.view().design_tokens_json.find("accent") == std::string::npos ||
        panel.state().selected_node_id != "ui.root") {
        std::cerr << "Project UI panel did not build a bounded initial hierarchy.\n";
        return 1;
    }
    if (!panel.select_node("ui.play") || panel.state().selected_draft.action_id != "game.start" ||
        panel.state().selected_draft.binding_json.find("command") == std::string::npos ||
        panel.state().selected_draft.state_json.find("visible") == std::string::npos ||
        panel.state().selected_draft.component_ref != "component.button") return 2;

    if (!panel.set_label("Launch") || !panel.set_action_id("game.launch") ||
        !panel.set_binding_json(R"({"kind":"command","target":"game.launch"})") ||
        !panel.set_state_json(R"({"visible":true,"enabled":false})") ||
        !panel.set_presentation_json(R"({"control":"secondary"})") ||
        !panel.set_value_json(R"("Paused")") || !panel.set_component_ref("component.button")) {
        std::cerr << "Project UI draft editing failed.\n";
        return 3;
    }
    if (panel.set_binding_json("{not-json") || !contains(panel.last_error(), "valid JSON")) {
        std::cerr << "Invalid binding JSON was not diagnosed.\n";
        return 4;
    }
    if (!panel.request_update_node()) return 5;
    const auto update = panel.consume_request();
    if (!update || update->kind != ProjectUiAuthoringPanelRequestKind::update_node ||
        update->base_revision != 17U || update->node_id != "ui.play" || update->label != "Launch" ||
        update->action_id != "game.launch" || update->state_json.find("enabled") == std::string::npos ||
        update->presentation_json.find("secondary") == std::string::npos || update->value_json != R"("Paused")" ||
        update->component_ref != "component.button" || update->request_id.empty()) {
        std::cerr << "Update request was not plain, stable, or revision-bound.\n";
        return 6;
    }
    if (!panel.request_update_node()) return 7;
    const auto repeated_update = panel.consume_request();
    if (!repeated_update || repeated_update->request_id != update->request_id) {
        std::cerr << "Equivalent UI requests did not retain stable identity.\n";
        return 8;
    }

    if (!panel.set_design_tokens_json(R"({"accent":"#ffcc66"})") || !panel.request_update_design_tokens()) return 9;
    const auto token_update = panel.consume_request();
    if (!token_update || token_update->kind != ProjectUiAuthoringPanelRequestKind::update_design_tokens ||
        token_update->design_tokens_json.find("#ffcc66") == std::string::npos) return 10;
    if (!panel.set_component_declaration_json("component.button", R"({"nodes":["ui.play"]})") ||
        !panel.request_update_component_declaration("component.button")) return 11;
    const auto component_update = panel.consume_request();
    if (!component_update || component_update->kind != ProjectUiAuthoringPanelRequestKind::update_component_declaration ||
        component_update->component_id != "component.button" || component_update->component_json.find("ui.play") == std::string::npos)
        return 12;
    if (!panel.request_add_component_declaration("component.card")) return 13;
    const auto component_add = panel.consume_request();
    if (!component_add || component_add->kind != ProjectUiAuthoringPanelRequestKind::add_component_declaration ||
        component_add->component_id != "component.card") return 14;
    if (!panel.request_remove_component_declaration("component.button")) return 15;
    const auto component_remove = panel.consume_request();
    if (!component_remove || component_remove->kind != ProjectUiAuthoringPanelRequestKind::remove_component_declaration ||
        component_remove->component_id != "component.button") return 16;

    panel.set_add_node_draft(ProjectUiAuthoringNodeKind::property, "ui.score", "property", "Score", "ui.root",
                             "game.score", R"({"kind":"world-property","property":"score"})");
    if (!panel.request_add_node()) return 17;
    const auto add = panel.consume_request();
    if (!add || add->kind != ProjectUiAuthoringPanelRequestKind::add_node ||
        add->node_kind != ProjectUiAuthoringNodeKind::property || add->parent_id != "ui.root" ||
        add->role != "property" || add->binding_json.find("score") == std::string::npos) {
        std::cerr << "Property node add request was incomplete.\n";
        return 18;
    }

    if (!panel.set_parent("ui.title") || !panel.request_reparent_node()) return 19;
    const auto reparent = panel.consume_request();
    if (!reparent || reparent->kind != ProjectUiAuthoringPanelRequestKind::reparent_node ||
        reparent->node_id != "ui.play" || reparent->parent_id != "ui.title") return 20;

    if (!panel.set_parent("ui.play") || panel.request_reparent_node() || panel.last_error().empty()) {
        std::cerr << "Reparent cycle was not rejected.\n";
        return 21;
    }
    if (!panel.set_parent("ui.root")) return 22;
    if (!panel.request_remove_node()) return 23;
    const auto remove = panel.consume_request();
    if (!remove || remove->kind != ProjectUiAuthoringPanelRequestKind::remove_node || remove->node_id != "ui.play")
        return 24;

    if (!panel.request_undo()) return 25;
    const auto undo = panel.consume_request();
    if (!undo || undo->kind != ProjectUiAuthoringPanelRequestKind::undo || undo->base_revision != 17U) return 26;
    if (!panel.request_redo()) return 27;
    const auto redo = panel.consume_request();
    if (!redo || redo->kind != ProjectUiAuthoringPanelRequestKind::redo) return 28;

    const auto semantic = nlohmann::json::parse(panel.semantic_snapshot_json(), nullptr, false);
    if (semantic.is_discarded() || semantic.value("schemaVersion", "") != "noemancer.project-ui-authoring/0.1" ||
        semantic.value("selectedNodeId", "") != "ui.play" || semantic.at("nodes").size() != 4U ||
        !semantic.at("nodes").at(2).contains("binding") || !semantic.contains("components") ||
        !semantic.contains("designTokensStatus") || !semantic.at("nodes").at(2).contains("fields")) {
        std::cerr << "Agent semantic project UI snapshot was incomplete.\n";
        return 29;
    }

    if (!panel.request_update_node()) return 30;
    panel.set_snapshot(snapshot(18U));
    if (panel.state().has_pending_request || !contains(panel.last_error(), "stale")) {
        std::cerr << "Revision change did not reject a stale pending project UI request.\n";
        return 31;
    }
    const auto conflicted = nlohmann::json::parse(panel.semantic_snapshot_json(), nullptr, false);
    bool has_conflicted_state{};
    if (!conflicted.is_discarded() && conflicted.contains("nodes")) {
        for (const auto& node : conflicted.at("nodes")) {
            if (node.value("id", "") != "ui.play") continue;
            for (const auto& field : node.value("fields", nlohmann::json::array()))
                if (field.value("field", "") == "state" && field.value("conflict", false)) has_conflicted_state = true;
        }
    }
    if (!has_conflicted_state) {
        std::cerr << "Stale revision did not project a field-level conflict.\n";
        return 32;
    }

    ProjectUiAuthoringPanel malformed({.document_json = "{not-json", .revision = 1U});
    if (malformed.view().valid || malformed.diagnostics().empty() ||
        malformed.diagnostics().front().code != "ui.document-json-parse-error" ||
        !contains(malformed.semantic_snapshot_json(), "ui.document-json-parse-error")) {
        std::cerr << "Malformed project UI JSON did not produce an explicit diagnostic.\n";
        return 33;
    }
    return 0;
}
