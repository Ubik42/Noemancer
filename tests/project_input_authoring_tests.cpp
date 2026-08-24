#include "engine/project_input_authoring.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

namespace {

bool has_code(const std::vector<noemancer::ProjectInputDiagnostic>& diagnostics,
              const std::string_view code) {
    for (const auto& diagnostic : diagnostics) {
        if (diagnostic.code == code) return true;
    }
    return false;
}

} // namespace

int main() {
    using namespace noemancer;
    const std::vector<InputActionDefinition> definitions{
        {"gameplay.jump", InputActionKind::button, {{"keyboard.space", 1.0F, 0.0F}}},
        {"gameplay.move.x", InputActionKind::axis_1d,
            {{"keyboard.a", -1.0F, 0.0F}, {"keyboard.d", 1.0F, 0.0F}}}
    };
    ProjectInputAuthoring authoring(definitions);
    const auto canonical = authoring.serialize_input_actions_json();
    ProjectInputAuthoring reordered({definitions.at(1), definitions.at(0)});
    if (canonical != reordered.serialize_input_actions_json()) {
        std::cerr << "Input action serialization was not deterministic.\n";
        return 1;
    }
    const auto observation = nlohmann::json::parse(authoring.serialize_json());
    if (observation.at("schemaVersion").get<std::string>() != std::string(project_input_authoring_schema) ||
        observation.at("revision") != 1U || observation.at("actions").size() != 2U) {
        std::cerr << "Input authoring observation contract is invalid.\n";
        return 2;
    }

    const auto initial_revision = authoring.revision();
    ProjectInputEditOptions dry_run_options;
    dry_run_options.expected_revision = initial_revision;
    dry_run_options.dry_run = true;
    const auto dry_run = authoring.add_action("gameplay.pause", InputActionKind::button,
        {{"keyboard.escape", 1.0F, 0.0F}}, dry_run_options);
    if (!dry_run.success || !dry_run.changed || authoring.revision() != initial_revision ||
        authoring.actions().size() != 2U) {
        std::cerr << "Dry-run input edit mutated or rejected a valid candidate (success="
            << dry_run.success << ", changed=" << dry_run.changed << ", code=" << dry_run.code
            << ", detail=" << dry_run.detail << ").\n";
        return 3;
    }
    ProjectInputEditOptions stale_options;
    stale_options.expected_revision = initial_revision - 1U;
    const auto stale = authoring.add_action("gameplay.stale", InputActionKind::button,
        {{"keyboard.f1", 1.0F, 0.0F}}, stale_options);
    if (stale.success || stale.code != "input.revision-conflict") return 4;

    const auto added = authoring.add_action("gameplay.interact", InputActionKind::button,
        {{"keyboard.e", 1.0F, 0.0F}});
    if (!added.success || authoring.actions().size() != 3U) return 5;
    const auto conflicting = authoring.add_binding("gameplay.interact", {"keyboard.space", 1.0F, 0.0F});
    if (conflicting.success || conflicting.code != "input.invalid-candidate" ||
        !has_code(conflicting.diagnostics, "input.binding-conflict")) {
        std::cerr << "Cross-action binding conflict was not diagnosed.\n";
        return 6;
    }
    const auto duplicate = authoring.add_binding("gameplay.jump", {"keyboard.space", 1.0F, 0.0F});
    if (duplicate.success || !has_code(duplicate.diagnostics, "input.duplicate-binding-source")) return 7;
    const auto invalid = authoring.add_action("gameplay/invalid", InputActionKind::button,
        {{"keyboard.f2", 0.0F, 1.0F}});
    if (invalid.success || !has_code(invalid.diagnostics, "input.invalid-action-id") ||
        !has_code(invalid.diagnostics, "input.invalid-binding-scale") ||
        !has_code(invalid.diagnostics, "input.invalid-binding-dead-zone")) return 8;

    const auto remapped = authoring.remap_binding("gameplay.interact", "keyboard.e",
        {"gamepad.north", 1.0F, 0.2F});
    if (!remapped.success || authoring.actions().back().id != "gameplay.move.x") return 9;
    const auto interact = std::find_if(authoring.actions().begin(), authoring.actions().end(),
        [](const auto& action) { return action.id == "gameplay.interact"; });
    if (interact == authoring.actions().end() || interact->bindings.front().source != "gamepad.north") return 10;
    if (authoring.remove_binding("gameplay.interact", "gamepad.north").code != "input.last-binding") return 11;
    if (!authoring.add_binding("gameplay.interact", {"keyboard.e", 1.0F, 0.0F}).success) return 12;
    if (!authoring.remove_binding("gameplay.interact", "gamepad.north").success) return 13;
    if (!authoring.remove_action("gameplay.interact").success || authoring.actions().size() != 2U) return 14;

    const auto root = (std::filesystem::temp_directory_path() /
        ("noemancer-project-input-authoring-" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()))).lexically_normal();
    std::filesystem::create_directories(root);
    const auto manifest = root / "noemancer.project.json";
    {
        std::ofstream output(manifest, std::ios::binary);
        output << R"({"schema":"noemancer.project/0.1","projectId":"project.test","name":"Input Test","startupScene":"scenes/main.scene.json","assetRoots":["assets"],"inputActions":[]})";
    }
    ProjectInputEditOptions save_options;
    save_options.expected_revision = authoring.revision();
    const auto saved = authoring.save_project_manifest(manifest, save_options);
    if (!saved.success) {
        std::cerr << saved.detail << '\n';
        std::filesystem::remove_all(root);
        return 15;
    }
    std::ifstream persisted_stream(manifest, std::ios::binary);
    const auto persisted_text = std::string(std::istreambuf_iterator<char>(persisted_stream),
        std::istreambuf_iterator<char>());
    persisted_stream.close();
    const auto persisted = nlohmann::json::parse(persisted_text);
    if (persisted.at("schema") != "noemancer.project/0.1" ||
        persisted.at("inputActions").size() != 2U ||
        persisted.at("inputActions").at(0).at("id") != "gameplay.jump" ||
        persisted.at("inputActions").at(1).at("id") != "gameplay.move.x") {
        std::cerr << "Atomic project input save did not persist canonical actions.\n";
        std::filesystem::remove_all(root);
        return 16;
    }

    // A 0.2 manifest may carry a Hybrid Pixel profile and unrelated future
    // fields. Input authoring changes only inputActions and must preserve all
    // other project authority verbatim at the JSON value level.
    const nlohmann::json hybrid_profile = {
        {"schema", "noemancer.hybrid-pixel-profile/0.1"},
        {"profileId", "hd2d.input-authoring"},
        {"enabled", true},
        {"virtualWidth", 640},
        {"virtualHeight", 360},
        {"pixelsPerUnit", 16},
        {"integerScaling", true},
        {"snapCamera", true},
        {"snapSprites", true},
        {"presentationFilter", "nearest"}
    };
    const nlohmann::json project_v2 = {
        {"schema", "noemancer.project/0.2"},
        {"projectId", "project.test"},
        {"name", "Input Test"},
        {"startupScene", "scenes/main.scene.json"},
        {"assetRoots", nlohmann::json::array({"assets"})},
        {"futureProjectField", {{"preserve", true}, {"owner", "test"}}},
        {"hybridPixelProfile", hybrid_profile},
        {"inputActions", nlohmann::json::array()}
    };
    {
        std::ofstream output(manifest, std::ios::binary | std::ios::trunc);
        output << project_v2.dump(2);
    }
    const auto saved_v2 = authoring.save_project_manifest(manifest, save_options);
    if (!saved_v2.success) {
        std::cerr << saved_v2.detail << '\n';
        std::filesystem::remove_all(root);
        return 17;
    }
    std::ifstream persisted_v2_stream(manifest, std::ios::binary);
    const auto persisted_v2_text = std::string(std::istreambuf_iterator<char>(persisted_v2_stream),
        std::istreambuf_iterator<char>());
    persisted_v2_stream.close();
    const auto persisted_v2 = nlohmann::json::parse(persisted_v2_text);
    if (persisted_v2.at("schema") != "noemancer.project/0.2" ||
        persisted_v2.at("futureProjectField") != project_v2.at("futureProjectField") ||
        persisted_v2.at("hybridPixelProfile") != hybrid_profile ||
        persisted_v2.at("inputActions").size() != 2U) {
        std::cerr << "Project 0.2 input save dropped profile or unrelated fields.\n";
        std::filesystem::remove_all(root);
        return 18;
    }
    {
        std::ofstream output(manifest, std::ios::binary | std::ios::trunc);
        output << R"({"schema":"not-a-project"})";
    }
    const auto invalid_save = authoring.save_project_manifest(manifest);
    if (invalid_save.success || invalid_save.code != "input.manifest-invalid") return 19;
    std::error_code cleanup_error;
    std::filesystem::remove_all(root, cleanup_error);
    if (cleanup_error || std::filesystem::exists(root)) return 20;

    const auto session_root = (std::filesystem::temp_directory_path() /
        ("noemancer-project-input-session-" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()))).lexically_normal();
    std::filesystem::create_directories(session_root);
    struct TemporaryDirectoryCleanup final {
        std::filesystem::path path;
        ~TemporaryDirectoryCleanup() {
            std::error_code error;
            std::filesystem::remove_all(path, error);
        }
    } session_cleanup{session_root};
    const auto session_manifest = session_root / "noemancer.project.json";
    {
        std::ofstream output(session_manifest, std::ios::binary);
        output << R"({"schema":"noemancer.project/0.1","projectId":"session.test","name":"Session Test","inputActions":[]})";
    }
    ProjectInputEditSession session = ProjectInputEditSession::load(definitions, session_manifest);
    if (session.revision() != 1U || session.manifest_path() != session_manifest) return 19;

    ProjectInputEditOptions session_options;
    session_options.expected_revision = 1U;
    const auto add_action_receipt = session.apply(ProjectInputEditRequest::add_action(
        "gameplay.pause", InputActionKind::button, {{"keyboard.escape", 1.0F, 0.0F}}, session_options));
    if (!add_action_receipt.success || !add_action_receipt.changed || !add_action_receipt.persisted ||
        add_action_receipt.code != "input.edit.committed" || add_action_receipt.revision != 2U ||
        session.actions().size() != 3U) return 20;

    session_options.expected_revision = 2U;
    const auto add_binding_receipt = session.apply(ProjectInputEditRequest::add_binding(
        "gameplay.jump", {"keyboard.enter", 1.0F, 0.0F}, session_options));
    if (!add_binding_receipt.success || !add_binding_receipt.persisted || add_binding_receipt.revision != 3U) return 21;

    std::ifstream before_stale_stream(session_manifest, std::ios::binary);
    const auto before_stale = std::string(std::istreambuf_iterator<char>(before_stale_stream),
        std::istreambuf_iterator<char>());
    before_stale_stream.close();
    session_options.expected_revision = 2U;
    const auto stale_receipt = session.apply(ProjectInputEditRequest::remove_binding(
        "gameplay.jump", "keyboard.enter", session_options));
    if (stale_receipt.success || stale_receipt.code != "input.revision-conflict" ||
        session.revision() != 3U || session.actions().size() != 3U) return 22;
    std::ifstream after_stale_stream(session_manifest, std::ios::binary);
    const auto after_stale = std::string(std::istreambuf_iterator<char>(after_stale_stream),
        std::istreambuf_iterator<char>());
    after_stale_stream.close();
    if (before_stale != after_stale) return 23;

    session_options.expected_revision = 3U;
    session_options.dry_run = true;
    const auto dry_session_receipt = session.apply(ProjectInputEditRequest::remove_binding(
        "gameplay.jump", "keyboard.enter", session_options));
    if (!dry_session_receipt.success || !dry_session_receipt.changed || dry_session_receipt.persisted ||
        dry_session_receipt.code != "input.edit.dry-run" || dry_session_receipt.revision != 3U ||
        session.actions().size() != 3U) return 24;
    std::ifstream after_dry_stream(session_manifest, std::ios::binary);
    const auto after_dry = std::string(std::istreambuf_iterator<char>(after_dry_stream),
        std::istreambuf_iterator<char>());
    after_dry_stream.close();
    if (after_dry != before_stale) return 25;

    session_options.dry_run = false;
    const auto remove_binding_receipt = session.apply(ProjectInputEditRequest::remove_binding(
        "gameplay.jump", "keyboard.enter", session_options));
    if (!remove_binding_receipt.success || !remove_binding_receipt.persisted ||
        remove_binding_receipt.revision != 4U || session.actions().size() != 3U) return 26;

    session_options.expected_revision = 4U;
    const auto remap_receipt = session.apply(ProjectInputEditRequest::remap_binding(
        "gameplay.jump", "keyboard.space", {"keyboard.return", 1.0F, 0.0F}, session_options));
    if (!remap_receipt.success || !remap_receipt.persisted || remap_receipt.revision != 5U) return 27;

    session_options.expected_revision = 5U;
    const auto remove_action_receipt = session.apply(ProjectInputEditRequest::remove_action(
        "gameplay.pause", session_options));
    if (!remove_action_receipt.success || !remove_action_receipt.persisted ||
        remove_action_receipt.revision != 6U || session.actions().size() != 2U) return 28;

    std::ifstream before_invalid_replace_stream(session_manifest, std::ios::binary);
    const auto before_invalid_replace = std::string(
        std::istreambuf_iterator<char>(before_invalid_replace_stream), std::istreambuf_iterator<char>());
    before_invalid_replace_stream.close();
    const auto invalid_replace = session.replace(
        {InputActionDefinition{"gameplay.invalid", InputActionKind::button, {{"keyboard.f2", 0.0F, 0.0F}}}},
        ProjectInputEditOptions{.expected_revision = 6U, .dry_run = false});
    if (invalid_replace.success || session.revision() != 6U || session.actions().size() != 2U) return 29;
    std::ifstream after_invalid_replace_stream(session_manifest, std::ios::binary);
    const auto after_invalid_replace = std::string(
        std::istreambuf_iterator<char>(after_invalid_replace_stream), std::istreambuf_iterator<char>());
    after_invalid_replace_stream.close();
    if (before_invalid_replace != after_invalid_replace) return 30;

    const auto old_session_path = session.manifest_path();
    const auto invalid_reload = session.reload(
        {InputActionDefinition{"gameplay.invalid", InputActionKind::button, {{"keyboard.f3", 0.0F, 0.0F}}}},
        session_root / "replacement.project.json", 7U);
    if (invalid_reload.success || session.revision() != 6U || session.manifest_path() != old_session_path ||
        session.actions().size() != 2U) return 31;
    const auto reloaded = session.reload(definitions, session_manifest, 9U);
    if (!reloaded.success || reloaded.code != "input.reload.committed" || reloaded.revision != 9U ||
        session.manifest_path() != session_manifest || session.actions().size() != 2U) return 32;

    {
        std::ofstream output(session_manifest, std::ios::binary | std::ios::trunc);
        output << R"({"schema":"not-a-project"})";
    }
    std::ifstream before_invalid_session_stream(session_manifest, std::ios::binary);
    const auto before_invalid_session = std::string(
        std::istreambuf_iterator<char>(before_invalid_session_stream), std::istreambuf_iterator<char>());
    before_invalid_session_stream.close();
    const auto invalid_session_save = session.apply(ProjectInputEditRequest::add_action(
        "gameplay.pause", InputActionKind::button, {{"keyboard.escape", 1.0F, 0.0F}},
        ProjectInputEditOptions{.expected_revision = 9U, .dry_run = false}));
    if (invalid_session_save.success || invalid_session_save.code != "input.manifest-invalid" ||
        session.revision() != 9U || session.actions().size() != 2U) return 33;
    std::ifstream after_invalid_session_stream(session_manifest, std::ios::binary);
    const auto after_invalid_session = std::string(
        std::istreambuf_iterator<char>(after_invalid_session_stream), std::istreambuf_iterator<char>());
    after_invalid_session_stream.close();
    if (before_invalid_session != after_invalid_session) return 34;

    ProjectInputEditSession missing_session(definitions, session_root / "missing.project.json");
    const auto missing_commit = missing_session.apply(ProjectInputEditRequest::add_action(
        "gameplay.pause", InputActionKind::button, {{"keyboard.escape", 1.0F, 0.0F}}));
    if (missing_commit.success || missing_commit.code != "input.manifest-not-found" ||
        missing_session.revision() != 1U || missing_session.actions().size() != 2U ||
        std::filesystem::exists(session_root / "missing.project.json")) return 35;

    cleanup_error.clear();
    std::filesystem::remove_all(session_root, cleanup_error);
    if (cleanup_error || std::filesystem::exists(session_root)) return 36;
    return 0;
}
