#include "editor/hybrid_pixel_profile_panel.hpp"
#include "engine/project_hybrid_pixel_authoring.hpp"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {

using Json = nlohmann::json;
namespace fs = std::filesystem;

struct Options final {
    fs::path project_root;
    fs::path receipt;
};

bool parse_options(const int argc, char** argv, Options& options) {
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument = argv[index];
        if (index + 1 >= argc) return false;
        if (argument == "--project-root") options.project_root = argv[++index];
        else if (argument == "--receipt" || argument == "--output") options.receipt = argv[++index];
        else return false;
    }
    return !options.project_root.empty() && !options.receipt.empty();
}

Json read_json(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("manifest could not be opened: " + path.string());
    const Json value = Json::parse(std::string{std::istreambuf_iterator<char>(input), {}}, nullptr, false);
    if (value.is_discarded() || !value.is_object()) throw std::runtime_error("manifest is not an object");
    return value;
}

void write_json(const fs::path& path, const Json& value) {
    if (!path.parent_path().empty()) fs::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) throw std::runtime_error("receipt could not be opened: " + path.string());
    output << value.dump(2) << '\n';
}

noemancer::HybridPixelProfile acceptance_profile() {
    noemancer::HybridPixelProfile profile;
    profile.profile_id = "hybrid.authoring.acceptance";
    profile.enabled = true;
    profile.virtual_width = 320U;
    profile.virtual_height = 180U;
    profile.pixels_per_unit = 16.0F;
    profile.integer_scaling = true;
    profile.snap_camera = true;
    profile.snap_sprites = true;
    profile.presentation_filter = "nearest";
    return profile;
}

bool equal_profile_id(const std::optional<noemancer::HybridPixelProfile>& profile,
                      const std::string_view id) {
    return profile && profile->profile_id == id;
}

} // namespace

int main(const int argc, char** argv) {
    Options options;
    if (!parse_options(argc, argv, options)) {
        std::cerr << "Usage: hybrid_pixel_profile_authoring_acceptance --project-root PATH --receipt PATH\n";
        return 2;
    }

    Json receipt{{"schemaVersion", "noemancer.hybrid-pixel-profile-authoring-acceptance/0.1"},
                 {"success", false}, {"projectRoot", fs::absolute(options.project_root).lexically_normal().generic_string()},
                 {"checks", Json::object()}, {"errors", Json::array()}};
    const auto manifest = fs::absolute(options.project_root / "noemancer.project.json").lexically_normal();
    try {
        const auto before = read_json(manifest);
        const auto original_input = before.value("inputActions", Json::array());
        const auto original_unknown = before.value("futureProjectField", Json(nullptr));
        if (before.value("schema", std::string{}) != "noemancer.project/0.2" ||
            !before.contains("inputActions") || !before.at("inputActions").is_array() ||
            !before.contains("futureProjectField")) {
            throw std::runtime_error("staging manifest must be Project 0.2 with inputActions and futureProjectField");
        }

        const auto loaded = noemancer::ProjectHybridPixelAuthoring::load_manifest(manifest);
        if (!loaded.success) throw std::runtime_error("ProjectHybridPixelAuthoring load failed: " + loaded.detail);
        noemancer::ProjectHybridPixelAuthoring authoring(loaded.profile, manifest, 1U);
        const auto profile = acceptance_profile();
        const auto applied = authoring.apply(profile, {.expected_revision = 1U});
        const auto undone = authoring.undo({.expected_revision = 2U});
        const auto redone = authoring.redo({.expected_revision = 3U});
        const auto after = read_json(manifest);

        const bool transaction_ok = applied.success && applied.persisted && applied.revision == 2U &&
            undone.success && undone.persisted && undone.revision == 3U &&
            redone.success && redone.persisted && redone.revision == 4U &&
            equal_profile_id(redone.profile, profile.profile_id) && authoring.revision() == 4U;
        const bool preservation_ok = after.value("inputActions", Json(nullptr)) == original_input &&
            after.value("futureProjectField", Json(nullptr)) == original_unknown;
        const bool profile_ok = after.contains("hybridPixelProfile") &&
            after.at("hybridPixelProfile").value("profileId", std::string{}) == profile.profile_id;

        noemancer::HybridPixelProfilePanel panel(
            {.revision = authoring.revision(), .profile = authoring.profile()}, {1440U, 900U});
        panel.set_undo_redo_available(authoring.can_undo(), authoring.can_redo());
        const auto semantic_text = panel.semantic_state_json();
        const auto semantic = Json::parse(semantic_text, nullptr, false);
        const bool panel_ok = !semantic.is_discarded() && semantic.is_object() &&
            semantic.value("schema", std::string{}) == "noemancer.hybrid-pixel-profile-panel/0.1" &&
            semantic.value("nodeId", std::string{}) == "editor.project-settings.hybrid-pixel-profile" &&
            semantic.at("snapshot").value("revision", 0U) == 4U &&
            semantic.at("draft").at("profile").value("profileId", std::string{}) == profile.profile_id &&
            semantic.at("validation").value("valid", false) &&
            semantic.at("preview").value("integerScale", 0U) == 4U &&
            semantic.at("preview").value("presentedWidth", 0U) == 1280U &&
            semantic.at("preview").value("presentedHeight", 0U) == 720U &&
            semantic.at("history").value("canUndo", false) &&
            !semantic.at("history").value("canRedo", true) &&
            semantic.at("request").is_null() && semantic.value("lastError", "x").empty();

        receipt["checks"] = {
            {"projectSchema", before.value("schema", std::string{}) == "noemancer.project/0.2"},
            {"applyUndoRedo", transaction_ok},
            {"inputActionsPreserved", preservation_ok && after.at("inputActions") == original_input},
            {"unknownFieldsPreserved", preservation_ok},
            {"profilePersisted", profile_ok},
            {"panelSemanticState", panel_ok},
            {"noImGuiContext", true}
        };
        receipt["revisions"] = {{"apply", applied.revision}, {"undo", undone.revision}, {"redo", redone.revision}};
        receipt["operations"] = {Json::parse(applied.to_json()), Json::parse(undone.to_json()), Json::parse(redone.to_json())};
        receipt["panelSemantic"] = semantic.is_discarded() ? Json(nullptr) : semantic;
        receipt["success"] = transaction_ok && preservation_ok && profile_ok && panel_ok;
        if (!receipt.value("success", false)) receipt["errors"].push_back("Hybrid Profile authoring acceptance checks failed.");
    } catch (const std::exception& error) {
        receipt["errors"].push_back(error.what());
    }

    try {
        write_json(options.receipt, receipt);
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    std::cout << receipt.dump() << '\n';
    return receipt.value("success", false) ? 0 : 3;
}
