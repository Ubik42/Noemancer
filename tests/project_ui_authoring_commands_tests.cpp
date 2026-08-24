#include "engine/project_ui_authoring_commands.hpp"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace {
using Json = nlohmann::json;

void write_text(const std::filesystem::path& path, const std::string_view text) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << text;
}

constexpr auto source_document = R"({
  "schemaVersion":"noemancer.ui-document/0.1",
  "documentId":"ui.command.fixture",
  "surface":"game",
  "kind":"hud",
  "revision":1,
  "roots":["ui.root"],
  "designTokens":{"accentColor":"#66ddcc"},
  "nodes":[
    {"id":"ui.root","parentId":null,"role":"hud","label":"Root","state":{"visible":true,"enabled":true},"actions":[]},
    {"id":"ui.first","parentId":"ui.root","role":"property","label":"First","state":{"visible":true,"enabled":true},"actions":[]},
    {"id":"ui.second","parentId":"ui.root","role":"property","label":"Second","state":{"visible":true,"enabled":true},"actions":[]}
  ]
})";

bool code_is(const Json& receipt, const std::string_view code) {
    return receipt.value("code", std::string{}) == code;
}
}

int main() {
    const auto root = std::filesystem::temp_directory_path() / "noemancer-project-ui-command-test";
    std::error_code cleanup;
    std::filesystem::remove_all(root, cleanup);
    std::filesystem::create_directories(root);
    const auto path = root / "hud.ui.json";
    write_text(path, source_document);

    auto session = noemancer::ProjectUiAuthoringSession::from_file(path);
    if (!session.valid()) return 1;
    noemancer::ProjectUiAuthoringCommandService service(session);

    const auto observed = Json::parse(service.observe_json());
    if (observed.at("schemaVersion") != std::string(noemancer::project_ui_authoring_schema) ||
        observed.at("revision") != 1U || observed.at("document").at("documentId") != "ui.command.fixture") return 2;

    auto receipt = Json::parse(service.dispatch_json(
        R"({"operation":"update","nodeId":"ui.first","label":"Missing base"})"));
    if (receipt.at("success") || !code_is(receipt, "ui.base-revision-required") || session.revision() != 1U) return 3;

    receipt = Json::parse(service.dispatch_json(
        R"({"operation":"update","baseRevision":"1","nodeId":"ui.first","label":"Bad type"})"));
    if (receipt.at("success") || !code_is(receipt, "ui.invalid-base-revision")) return 4;

    receipt = Json::parse(service.dispatch_json(
        R"({"operation":"update-node","baseRevision":1,"dryRun":true,"nodeId":"ui.first","label":"Dry"})"));
    if (!receipt.at("success") || !receipt.at("changed") || receipt.at("persisted") || receipt.at("revision") != 1U ||
        session.revision() != 1U || Json::parse(receipt.at("document").dump()).at("revision") != 2U) return 5;

    receipt = Json::parse(service.execute_json(
        R"({"operation":"update","baseRevision":1,"nodeId":"ui.first","label":"Edited"})"));
    if (!receipt.at("success") || receipt.at("revision") != 2U ||
        Json::parse(session.source_json()).at("nodes").at(1).at("label") != "Edited") return 6;

    receipt = Json::parse(service.invoke_json(
        R"({"operation":"add-node","baseRevision":2,"id":"ui.third","parentId":"ui.root","role":"button","label":"Third","actions":[{"id":"ui.invoke"}]})"));
    if (!receipt.at("success") || receipt.at("revision") != 3U ||
        Json::parse(session.source_json()).at("nodes").size() != 4U) return 7;

    receipt = Json::parse(service.dispatch_json(
        R"({"operation":"reparent","baseRevision":3,"nodeId":"ui.second","parentId":"ui.first"})"));
    if (!receipt.at("success") || receipt.at("revision") != 4U) return 8;

    receipt = Json::parse(service.dispatch_json(
        R"({"operation":"reorder-node","baseRevision":4,"nodeId":"ui.third","siblingIndex":0})"));
    if (!receipt.at("success") || receipt.at("revision") != 5U) return 9;

    receipt = Json::parse(service.dispatch_json(
        R"({"operation":"design-tokens","baseRevision":5,"designTokens":{"accentColor":"#ffffff","density":2}})"));
    if (!receipt.at("success") || receipt.at("revision") != 6U ||
        Json::parse(session.source_json()).at("designTokens").at("density") != 2) return 10;

    receipt = Json::parse(service.dispatch_json(R"({"operation":"undo","baseRevision":6})"));
    if (!receipt.at("success") || receipt.at("revision") != 7U || !session.can_redo()) return 11;
    receipt = Json::parse(service.dispatch_json(R"({"operation":"redo","baseRevision":7})"));
    if (!receipt.at("success") || receipt.at("revision") != 8U || !session.can_undo()) return 12;

    receipt = Json::parse(service.dispatch_json(
        R"({"operation":"remove","baseRevision":8,"nodeId":"ui.third"})"));
    if (!receipt.at("success") || receipt.at("revision") != 9U ||
        Json::parse(session.source_json()).at("nodes").size() != 3U) return 13;

    receipt = Json::parse(service.dispatch_json(
        R"({"operation":"add-component-declaration","baseRevision":9,"componentId":"ui.button","componentJson":{"role":"button","label":"Reusable","presentation":{"width":120},"actions":[]}})"));
    if (!receipt.at("success") || receipt.at("revision") != 10U ||
        Json::parse(session.source_json()).at("components").size() != 1U) return 14;

    receipt = Json::parse(service.dispatch_json(
        R"({"operation":"add","baseRevision":10,"id":"ui.component","parentId":"ui.root","role":"button","label":"Component instance","componentRef":"ui.button"})"));
    if (!receipt.at("success") || receipt.at("revision") != 11U ||
        Json::parse(session.source_json()).at("nodes").back().at("componentRef") != "ui.button") return 15;

    receipt = Json::parse(service.dispatch_json(
        R"({"operation":"update","baseRevision":11,"nodeId":"ui.first","componentRef":"ui.button"})"));
    if (!receipt.at("success") || receipt.at("revision") != 12U ||
        Json::parse(session.source_json()).at("nodes").at(1).at("componentRef") != "ui.button") return 16;

    receipt = Json::parse(service.dispatch_json(
        R"({"operation":"update-component-declaration","baseRevision":12,"componentId":"ui.button","componentJson":{"role":"button","label":"Updated reusable","actions":[]}})"));
    if (!receipt.at("success") || receipt.at("revision") != 13U) return 17;

    receipt = Json::parse(service.dispatch_json(R"({"operation":"remove-declaration","baseRevision":13,"id":"ui.button"})"));
    if (receipt.at("success") || !code_is(receipt, "ui.component-in-use")) return 18;

    receipt = Json::parse(service.dispatch_json(
        R"({"operation":"update","baseRevision":13,"nodeId":"ui.first","componentRef":null})"));
    if (!receipt.at("success") || receipt.at("revision") != 14U) return 19;

    receipt = Json::parse(service.dispatch_json(
        R"({"operation":"update","baseRevision":14,"nodeId":"ui.component","componentRef":null})"));
    if (!receipt.at("success") || receipt.at("revision") != 15U) return 20;

    receipt = Json::parse(service.dispatch_json(R"({"operation":"remove-component-declaration","baseRevision":15,"componentId":"ui.button"})"));
    if (!receipt.at("success") || receipt.at("revision") != 16U ||
        Json::parse(session.source_json()).at("components").size() != 0U) return 21;

    receipt = Json::parse(service.dispatch_json(R"({"operation":"does-not-exist","baseRevision":16})"));
    if (receipt.at("success") || !code_is(receipt, "ui.unknown-operation")) return 22;

    const auto oversized = std::string("{\"operation\":\"observe\",\"padding\":\"") +
        std::string(noemancer::project_ui_authoring_command_max_bytes, 'x') + "\"}";
    receipt = Json::parse(service.dispatch_json(oversized));
    if (receipt.at("success") || !code_is(receipt, "ui.command-too-large")) return 23;

    std::filesystem::remove_all(root, cleanup);
    std::cout << "Project UI command adapter preserved live-session authority and bounded receipts\n";
    return 0;
}
