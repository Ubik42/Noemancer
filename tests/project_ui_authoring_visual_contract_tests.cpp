#include "editor/project_ui_authoring_panel.hpp"

#include <nlohmann/json.hpp>

#include <cstddef>
#include <cstdint>
#include <iostream>
#include <set>
#include <string>
#include <string_view>

namespace {

using Json = nlohmann::json;
using noemancer::ProjectUiAuthoringPanel;
using noemancer::ProjectUiAuthoringSnapshot;

std::string document_json() {
    return R"json({
        "schemaVersion":"noemancer.ui-document/0.1",
        "documentId":"visual-contract.project-ui",
        "revision":41,
        "designTokens":{"accent":"#78d7ff","surface":"#101827","spacing":{"sm":8}},
        "components":[
            {"id":"ui.control.base","rootNodeId":"ui.status","label":"Base control",
             "role":"property","presentation":{"control":"field","layout":{"gap":8}}},
            {"id":"ui.control.status","rootNodeId":"ui.status","label":"Status control",
             "extends":"ui.control.base","state":{"enabled":true},
             "presentation":{"control":"meter"}}
        ],
        "nodes":[
            {"id":"ui.root","parentId":null,"role":"container","label":"HUD root",
             "state":{"visible":true},"presentation":{"layout":{"direction":"column"}},"value":null},
            {"id":"ui.status","parentId":"ui.root","role":"property","label":"Energy",
             "componentRef":"ui.control.status",
             "binding":{"kind":"world-property","property":"player.energy"},
             "state":{"visible":true,"enabled":true,"focused":false},
             "presentation":{"control":"meter","layout":{"width":240,"height":24}},
             "value":{"current":72,"maximum":100}}
        ]
    })json";
}

ProjectUiAuthoringSnapshot snapshot(const std::uint64_t revision = 41U) {
    return {.document_json = document_json(),
            .revision = revision,
            .fingerprint = "fnv1a64:visual-contract",
            .can_undo = true,
            .can_redo = false};
}

const Json* find_node(const Json& semantic, const std::string_view id) {
    if (!semantic.contains("nodes") || !semantic.at("nodes").is_array()) return nullptr;
    for (const auto& node : semantic.at("nodes"))
        if (node.value("id", "") == id) return &node;
    return nullptr;
}

const Json* find_component(const Json& semantic, const std::string_view id) {
    if (!semantic.contains("components") || !semantic.at("components").is_array()) return nullptr;
    for (const auto& component : semantic.at("components"))
        if (component.value("id", "") == id) return &component;
    return nullptr;
}

const Json* find_draft(const Json& semantic, const std::string_view node_id) {
    if (!semantic.contains("drafts") || !semantic.at("drafts").is_array()) return nullptr;
    for (const auto& draft : semantic.at("drafts"))
        if (draft.value("nodeId", "") == node_id) return &draft;
    return nullptr;
}

const Json* find_field(const Json& node, const std::string_view field_name) {
    if (!node.contains("fields") || !node.at("fields").is_array()) return nullptr;
    for (const auto& field : node.at("fields"))
        if (field.value("field", "") == field_name) return &field;
    return nullptr;
}

const Json* find_diagnostic(const Json& semantic, const std::string_view code) {
    if (!semantic.contains("diagnostics") || !semantic.at("diagnostics").is_array()) return nullptr;
    for (const auto& diagnostic : semantic.at("diagnostics"))
        if (diagnostic.value("code", "") == code) return &diagnostic;
    return nullptr;
}

bool require(bool condition, const char* message, const int code) {
    if (condition) return true;
    std::cerr << message << '\n';
    return false;
}

} // namespace

int main() {
    using namespace noemancer;

    ProjectUiAuthoringPanel panel(snapshot());
    const auto initial = Json::parse(panel.semantic_snapshot_json(), nullptr, false);
    if (!require(!initial.is_discarded(), "Initial visual contract snapshot was not JSON.", 1) ||
        !require(initial.value("schemaVersion", "") == "noemancer.project-ui-authoring/0.1",
                 "Authoring snapshot schema identity changed.", 2) ||
        !require(initial.value("documentSchema", "") == "noemancer.ui-document/0.1" &&
                     initial.value("documentId", "") == "visual-contract.project-ui" &&
                     initial.value("sourceRevision", 0U) == 41U &&
                     initial.value("sourceFingerprint", "") == "fnv1a64:visual-contract",
                 "Source identity was not exposed as stable semantic fields.", 3) ||
        !require(initial.value("valid", false) && initial.value("code", "") == "ok",
                 "A valid authored document was projected as invalid.", 4) ||
        !require(initial.at("roots") == Json::array({"ui.root"}),
                 "Hierarchy roots were not projected deterministically.", 5) ||
        !require(initial.at("nodes").size() == 2U && initial.at("components").size() == 2U,
                 "The visual projection lost authored nodes or declarations.", 6))
        return 1;

    const auto* base_component = find_component(initial, "ui.control.base");
    const auto* derived_component = find_component(initial, "ui.control.status");
    if (!require(base_component != nullptr && derived_component != nullptr,
                 "Component declaration identities were not exposed.", 7) ||
        !require(base_component->at("rootNodeId") == "ui.status" &&
                     base_component->at("component").at("role") == "property" &&
                     base_component->at("status").at("field") == "componentDeclaration" &&
                     base_component->at("status").at("valid") == true,
                 "Base component declaration projection is incomplete.", 8) ||
        !require(derived_component->at("component").at("extends") == "ui.control.base" &&
                     derived_component->at("component").at("presentation").at("control") == "meter" &&
                     derived_component->at("status").at("dirty") == false &&
                     derived_component->at("status").at("pending") == false &&
                     derived_component->at("status").at("conflict") == false,
                 "Component inheritance was not retained in the visual semantic projection.", 9))
        return 2;

    const auto* status_node = find_node(initial, "ui.status");
    if (!require(status_node != nullptr, "The componentRef node was not projected.", 10) ||
        !require(status_node->at("parentId") == "ui.root" &&
                     status_node->at("componentRef") == "ui.control.status" &&
                     status_node->at("componentRef").is_string(),
                 "Node hierarchy or componentRef identity was not stable.", 11) ||
        !require(status_node->at("state") == Json({{"visible", true}, {"enabled", true}, {"focused", false}}) &&
                     status_node->at("presentation").at("control") == "meter" &&
                     status_node->at("presentation").at("layout").at("width") == 240 &&
                     status_node->at("value") == Json({{"current", 72}, {"maximum", 100}}) &&
                     status_node->at("stateKind") == "object" &&
                     status_node->at("presentationKind") == "object" &&
                     status_node->at("valueKind") == "object" &&
                     status_node->at("stateValid") == true &&
                     status_node->at("presentationValid") == true &&
                     status_node->at("valueValid") == true,
                 "State, presentation, value, or their typed validity were not projected.", 12))
        return 3;

    const Json expected_fields = Json::array({
        "label", "role", "parent", "actionId", "binding", "state", "presentation", "value",
        "componentRef", "componentDeclaration"});
    if (!require(status_node->at("fields").size() == expected_fields.size(),
                 "The node field contract changed cardinality.", 13))
        return 4;
    std::set<std::string> field_ids;
    for (std::size_t index = 0U; index < expected_fields.size(); ++index) {
        const auto& field = status_node->at("fields").at(index);
        if (!require(field.value("field", "") == expected_fields.at(index).get<std::string>() &&
                         field_ids.insert(field.value("field", "")).second &&
                         field.value("valid", false) && !field.value("dirty", true) &&
                         !field.value("pending", true) && !field.value("conflict", true) &&
                         field.value("error", "").empty(),
                     "A node field lacks a stable identity or clean status contract.", 14))
            return 5;
    }
    if (!require(initial.at("designTokens") ==
                     Json({{"accent", "#78d7ff"}, {"surface", "#101827"},
                           {"spacing", {{"sm", 8}}}}) &&
                     initial.at("designTokensStatus").at("field") == "designTokens" &&
                     initial.at("designTokensStatus").at("valid") == true &&
                     initial.at("designTokensStatus").at("dirty") == false,
                 "Design token data or its field status was not projected.", 15))
        return 6;

    if (!require(panel.select_node("ui.status"),
                 "The panel could not select the node needed to exercise diagnostics.", 16) ||
        !require(!panel.set_state_json("{not-json"),
                 "Malformed state JSON was accepted by the visual authoring contract.", 17))
        return 7;
    const auto invalid_edit = Json::parse(panel.semantic_snapshot_json(), nullptr, false);
    const auto* invalid_edit_node = find_node(invalid_edit, "ui.status");
    const auto* invalid_state_field = invalid_edit_node == nullptr ? nullptr : find_field(*invalid_edit_node, "state");
    if (!require(invalid_state_field != nullptr && invalid_state_field->at("valid") == false &&
                     invalid_state_field->at("error").get<std::string>().find("valid JSON") != std::string::npos,
                 "Field-level JSON errors were not surfaced with stable state identity.", 18))
        return 8;

    if (!require(panel.set_state_json(R"({"visible":true,"enabled":true,"focused":false})") &&
                     panel.set_label("Energy online"),
                 "The panel could not restore a valid draft after a field error.", 19))
        return 9;
    const auto dirty = Json::parse(panel.semantic_snapshot_json(), nullptr, false);
    const auto* dirty_node = find_node(dirty, "ui.status");
    const auto* dirty_label = dirty_node == nullptr ? nullptr : find_field(*dirty_node, "label");
    const auto* dirty_draft = find_draft(dirty, "ui.status");
    if (!require(dirty_label != nullptr && dirty_label->at("dirty") == true &&
                     dirty_label->at("valid") == true && dirty_draft != nullptr &&
                     dirty_draft->at("label") == "Energy online",
                 "Dirty editor state was not exposed as a structured draft.", 20))
        return 10;

    if (!require(panel.request_update_node(), "The panel did not queue the revision-bound update.", 21)) return 11;
    const auto pending = Json::parse(panel.semantic_snapshot_json(), nullptr, false);
    const auto* pending_node = find_node(pending, "ui.status");
    const auto* pending_label = pending_node == nullptr ? nullptr : find_field(*pending_node, "label");
    if (!require(pending.at("pendingRequest").is_object() &&
                     pending.at("pendingRequest").at("kind") == "update-node" &&
                     pending.at("pendingRequest").at("baseRevision") == 41U &&
                     pending.at("pendingRequest").at("nodeId") == "ui.status" &&
                     !pending.at("pendingRequest").at("requestId").get<std::string>().empty() &&
                     pending_label != nullptr && pending_label->at("dirty") == true &&
                     pending_label->at("pending") == true,
                 "Pending request identity or field status was not projected.", 22))
        return 12;

    panel.set_snapshot(snapshot(42U));
    const auto conflicted = Json::parse(panel.semantic_snapshot_json(), nullptr, false);
    const auto* conflicted_node = find_node(conflicted, "ui.status");
    const auto* conflicted_label = conflicted_node == nullptr ? nullptr : find_field(*conflicted_node, "label");
    if (!require(conflicted.at("pendingRequest").is_null() &&
                     conflicted_label != nullptr && conflicted_label->at("dirty") == true &&
                     conflicted_label->at("conflict") == true &&
                     conflicted.contains("lastError") &&
                     conflicted.at("lastError").get<std::string>().find("stale") != std::string::npos,
                 "Revision conflict did not clear pending work while retaining dirty conflict evidence.", 23))
        return 13;

    const auto invalid_document = R"json({
        "schemaVersion":"noemancer.ui-document/0.1",
        "documentId":"visual-contract.invalid",
        "designTokens":["not-an-object"],
        "nodes":[
            {"id":"ui.root","role":"container"},
            {"id":"ui.bad","parentId":"ui.root","role":"property","state":[],"componentRef":"missing.component"}
        ]
    })json";
    ProjectUiAuthoringPanel malformed({.document_json = invalid_document,
                                       .revision = 7U,
                                       .fingerprint = "fnv1a64:invalid"});
    const auto invalid_semantic = Json::parse(malformed.semantic_snapshot_json(), nullptr, false);
    const auto* invalid_document_node = find_node(invalid_semantic, "ui.bad");
    const auto* invalid_document_state = invalid_document_node == nullptr ? nullptr :
        find_field(*invalid_document_node, "state");
    if (!require(invalid_semantic.value("valid", true) == false &&
                     invalid_semantic.value("code", "") == "ui.authoring-invalid-document" &&
                     find_diagnostic(invalid_semantic, "ui.invalid-design-tokens") != nullptr &&
                     find_diagnostic(invalid_semantic, "ui.invalid-state-shape") != nullptr &&
                     invalid_document_node != nullptr && invalid_document_node->at("stateValid") == false &&
                     invalid_document_state != nullptr && invalid_document_state->at("valid") == false &&
                     invalid_document_state->at("error").get<std::string>().find("object") != std::string::npos,
                 "Document diagnostics were not available to the visual contract consumer.", 24))
        return 14;

    return 0;
}
