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
            "nodes":[
                {"id":"ui.root","parentId":null,"role":"container","label":"Root"},
                {"id":"ui.title","parentId":"ui.root","role":"text","label":"Title"},
                {"id":"ui.play","parentId":"ui.root","role":"button","label":"Play",
                 "actionId":"game.start","binding":{"kind":"display-value"},
                 "actions":[{"id":"game.start","binding":{"kind":"command","target":"game.start"}}]},
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
        panel.state().selected_node_id != "ui.root") {
        std::cerr << "Project UI panel did not build a bounded initial hierarchy.\n";
        return 1;
    }
    if (!panel.select_node("ui.play") || panel.state().selected_draft.action_id != "game.start" ||
        panel.state().selected_draft.binding_json.find("command") == std::string::npos) return 2;

    if (!panel.set_label("Launch") || !panel.set_action_id("game.launch") ||
        !panel.set_binding_json(R"({"kind":"command","target":"game.launch"})")) {
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
        update->action_id != "game.launch" || update->request_id.empty()) {
        std::cerr << "Update request was not plain, stable, or revision-bound.\n";
        return 6;
    }
    if (!panel.request_update_node()) return 7;
    const auto repeated_update = panel.consume_request();
    if (!repeated_update || repeated_update->request_id != update->request_id) {
        std::cerr << "Equivalent UI requests did not retain stable identity.\n";
        return 8;
    }

    panel.set_add_node_draft(ProjectUiAuthoringNodeKind::property, "ui.score", "property", "Score", "ui.root",
                             "game.score", R"({"kind":"world-property","property":"score"})");
    if (!panel.request_add_node()) return 9;
    const auto add = panel.consume_request();
    if (!add || add->kind != ProjectUiAuthoringPanelRequestKind::add_node ||
        add->node_kind != ProjectUiAuthoringNodeKind::property || add->parent_id != "ui.root" ||
        add->role != "property" || add->binding_json.find("score") == std::string::npos) {
        std::cerr << "Property node add request was incomplete.\n";
        return 10;
    }

    if (!panel.set_parent("ui.title") || !panel.request_reparent_node()) return 11;
    const auto reparent = panel.consume_request();
    if (!reparent || reparent->kind != ProjectUiAuthoringPanelRequestKind::reparent_node ||
        reparent->node_id != "ui.play" || reparent->parent_id != "ui.title") return 12;

    if (!panel.set_parent("ui.play") || panel.request_reparent_node() || panel.last_error().empty()) {
        std::cerr << "Reparent cycle was not rejected.\n";
        return 13;
    }
    if (!panel.set_parent("ui.root")) return 14;
    if (!panel.request_remove_node()) return 15;
    const auto remove = panel.consume_request();
    if (!remove || remove->kind != ProjectUiAuthoringPanelRequestKind::remove_node || remove->node_id != "ui.play")
        return 16;

    if (!panel.request_undo()) return 17;
    const auto undo = panel.consume_request();
    if (!undo || undo->kind != ProjectUiAuthoringPanelRequestKind::undo || undo->base_revision != 17U) return 18;
    if (!panel.request_redo()) return 19;
    const auto redo = panel.consume_request();
    if (!redo || redo->kind != ProjectUiAuthoringPanelRequestKind::redo) return 20;

    const auto semantic = nlohmann::json::parse(panel.semantic_snapshot_json(), nullptr, false);
    if (semantic.is_discarded() || semantic.value("schemaVersion", "") != "noemancer.project-ui-authoring/0.1" ||
        semantic.value("selectedNodeId", "") != "ui.play" || semantic.at("nodes").size() != 4U ||
        !semantic.at("nodes").at(2).contains("binding")) {
        std::cerr << "Agent semantic project UI snapshot was incomplete.\n";
        return 21;
    }

    if (!panel.request_update_node()) return 22;
    panel.set_snapshot(snapshot(18U));
    if (panel.state().has_pending_request || !contains(panel.last_error(), "stale")) {
        std::cerr << "Revision change did not reject a stale pending project UI request.\n";
        return 23;
    }

    ProjectUiAuthoringPanel malformed({.document_json = "{not-json", .revision = 1U});
    if (malformed.view().valid || malformed.diagnostics().empty() ||
        malformed.diagnostics().front().code != "ui.document-json-parse-error" ||
        !contains(malformed.semantic_snapshot_json(), "ui.document-json-parse-error")) {
        std::cerr << "Malformed project UI JSON did not produce an explicit diagnostic.\n";
        return 24;
    }
    return 0;
}
