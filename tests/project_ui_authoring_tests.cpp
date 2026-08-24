#include "engine/project_ui_authoring.hpp"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace {

using Json = nlohmann::json;

void write_text(const std::filesystem::path& path, const std::string& text) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << text;
}

std::string read_text(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

bool has_code(const noemancer::ProjectUiEditReceipt& receipt, const std::string_view code) {
    if (receipt.code == code) return true;
    for (const auto& diagnostic : receipt.diagnostics) {
        if (diagnostic.code == code) return true;
    }
    return false;
}

constexpr auto source_document = R"({
  "schemaVersion":"noemancer.ui-document/0.1",
  "documentId":"ui.authoring.fixture",
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

} // namespace

int main() {
    const auto root = std::filesystem::temp_directory_path() / "noemancer-project-ui-authoring-test";
    std::error_code cleanup;
    std::filesystem::remove_all(root, cleanup);
    std::filesystem::create_directories(root);
    const auto path = root / "hud.ui.json";
    write_text(path, source_document);

    auto session = noemancer::ProjectUiAuthoringSession::from_file(path);
    if (!session.valid() || session.revision() != 1U || session.fingerprint().empty()) {
        std::cerr << "A valid project UI source did not load: " << session.observation_json() << '\n';
        return 1;
    }
    const auto source_before_dry_run = read_text(path);
    noemancer::ProjectUiUpdateNodeRequest dry_update;
    dry_update.node_id = "ui.first";
    dry_update.label = "Dry Run";
    auto receipt = session.update_node(std::move(dry_update), {.expected_revision = 1U, .dry_run = true});
    if (!receipt || !receipt.changed || receipt.persisted || session.revision() != 1U ||
        read_text(path) != source_before_dry_run ||
        Json::parse(receipt.document_json).at("revision") != 2U) {
        std::cerr << "Dry-run changed source or did not expose the candidate revision\n";
        return 2;
    }

    noemancer::ProjectUiUpdateNodeRequest update;
    update.node_id = "ui.first";
    update.label = "Edited";
    receipt = session.update_node(std::move(update), {.expected_revision = 1U});
    if (!receipt || !receipt.changed || !receipt.persisted || receipt.revision != 2U ||
        Json::parse(session.source_json()).at("revision") != 2U ||
        Json::parse(read_text(path)).at("revision") != 2U ||
        Json::parse(read_text(path)).at("nodes").at(1).at("label") != "Edited") {
        std::cerr << "Committed Project UI update did not persist revision and label\n";
        return 3;
    }
    const auto after_update = read_text(path);

    noemancer::ProjectUiUpdateNodeRequest stale;
    stale.node_id = "ui.first";
    stale.label = "Stale";
    receipt = session.update_node(std::move(stale), {.expected_revision = 1U});
    if (receipt || !has_code(receipt, "ui.revision-conflict") || read_text(path) != after_update) {
        std::cerr << "Revision CAS did not reject stale Project UI update\n";
        return 4;
    }

    noemancer::ProjectUiAddNodeRequest add;
    add.id = "ui.third";
    add.parent_id = "ui.root";
    add.role = "button";
    add.label = "Third";
    add.actions_json = R"([{"id":"ui.invoke","binding":{"kind":"command","command":"demo"}}])";
    receipt = session.add_node(std::move(add), {.expected_revision = 2U});
    if (!receipt || receipt.revision != 3U || Json::parse(session.source_json()).at("nodes").size() != 4U) {
        std::cerr << "add_node did not commit\n";
        return 5;
    }

    receipt = session.reparent("ui.second", "ui.first", {.expected_revision = 3U});
    if (!receipt || receipt.revision != 4U ||
        Json::parse(session.source_json()).at("nodes").at(2).at("parentId") != "ui.first") {
        std::cerr << "reparent did not preserve a valid tree\n";
        return 6;
    }
    receipt = session.reparent("ui.root", "ui.first", {.expected_revision = 4U});
    if (receipt || !has_code(receipt, "ui.parent-cycle")) {
        std::cerr << "reparent cycle was not rejected\n";
        return 7;
    }

    receipt = session.reorder("ui.third", 0U, {.expected_revision = 4U});
    if (!receipt || receipt.revision != 5U) {
        std::cerr << "reorder did not commit\n";
        return 8;
    }
    const auto after_reorder = Json::parse(session.source_json());
    if (after_reorder.at("nodes").at(1).at("id") != "ui.third") {
        std::cerr << "reorder produced an unexpected sibling order\n";
        return 9;
    }

    receipt = session.undo({.expected_revision = 5U});
    if (!receipt || receipt.revision != 6U || !session.can_redo() ||
        Json::parse(session.source_json()).at("revision") != 6U) {
        std::cerr << "undo did not atomically publish a new revision\n";
        return 10;
    }
    receipt = session.redo({.expected_revision = 6U});
    if (!receipt || receipt.revision != 7U || !session.can_undo() ||
        Json::parse(read_text(path)).at("revision") != 7U) {
        std::cerr << "redo did not atomically publish a new revision\n";
        return 11;
    }

    const auto observation_a = session.observation_json();
    const auto observation_b = session.observation_json();
    if (observation_a != observation_b ||
        !Json::parse(observation_a).at("valid").get<bool>() ||
        Json::parse(observation_a).at("document").at("revision") != 7U) {
        std::cerr << "Project UI observation is not deterministic\n";
        return 12;
    }

    const auto stable_source = read_text(path);
    noemancer::ProjectUiUpdateNodeRequest invalid_actions;
    invalid_actions.node_id = "ui.first";
    invalid_actions.actions_json = R"({"not":"an array"})";
    receipt = session.update_node(std::move(invalid_actions), {.expected_revision = 7U});
    if (receipt || !has_code(receipt, "ui.invalid-json-value") || read_text(path) != stable_source) {
        std::cerr << "Invalid actions edit was not rejected atomically\n";
        return 13;
    }

    auto cyclic = noemancer::ProjectUiAuthoringSession::from_json(
        R"({"schemaVersion":"noemancer.ui-document/0.1","documentId":"ui.cycle","nodes":[{"id":"ui.a","parentId":"ui.b","role":"group"},{"id":"ui.b","parentId":"ui.a","role":"group"}]})");
    if (cyclic.valid() || cyclic.validate().empty()) {
        std::cerr << "Cyclic source was accepted\n";
        return 14;
    }
    auto runtime_derived = noemancer::ProjectUiAuthoringSession::from_json(
        R"({"schemaVersion":"noemancer.ui-document/0.1","documentId":"ui.runtime","valid":true,"nodes":[{"id":"ui.root","parentId":null,"role":"hud","fingerprint":"runtime"}]})");
    if (runtime_derived.valid()) {
        std::cerr << "Runtime-derived fields were accepted as source data\n";
        return 15;
    }

    std::filesystem::remove_all(root, cleanup);
    return 0;
}
