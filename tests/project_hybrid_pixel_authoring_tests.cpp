#include "engine/project_hybrid_pixel_authoring.hpp"

#include <nlohmann/json.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>

namespace {

using Json = nlohmann::json;

noemancer::HybridPixelProfile profile(const std::string& id, const bool enabled = true) {
    noemancer::HybridPixelProfile value;
    value.profile_id = id;
    value.enabled = enabled;
    value.virtual_width = 640U;
    value.virtual_height = 360U;
    value.pixels_per_unit = 24.0F;
    return value;
}

std::string read_text(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

bool has_diagnostic(const std::vector<noemancer::ProjectHybridPixelDiagnostic>& diagnostics,
                   const std::string_view code) {
    for (const auto& diagnostic : diagnostics) {
        if (diagnostic.code == code) return true;
    }
    return false;
}

} // namespace

int main() {
    using namespace noemancer;
    const auto root = (std::filesystem::temp_directory_path() /
        ("noemancer-project-hybrid-pixel-authoring-" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()))).lexically_normal();
    std::filesystem::create_directories(root);
    const auto manifest = root / "noemancer.project.json";
    const Json original = {
        {"schema", "noemancer.project/0.2"},
        {"projectId", "project.hybrid-test"},
        {"name", "Hybrid Test"},
        {"startupScene", "scenes/main.scene.json"},
        {"assetRoots", Json::array({"assets"})},
        {"futureProjectField", {{"preserve", true}, {"owner", "test"}}},
        {"inputActions", Json::array({{{"id", "gameplay.jump"}, {"kind", "button"}}})}
    };
    {
        std::ofstream output(manifest, std::ios::binary);
        output << original.dump(2) << '\n';
    }
    const auto loaded = ProjectHybridPixelAuthoring::load_manifest(manifest);
    if (!loaded.success || loaded.profile || loaded.manifest_path != std::filesystem::absolute(manifest).lexically_normal()) {
        std::cerr << "Manifest load did not preserve an absent optional profile: " << loaded.code << " "
            << loaded.detail << " path=" << loaded.manifest_path.generic_string() << "\n";
        for (const auto& diagnostic : loaded.diagnostics) std::cerr << diagnostic.code << " " << diagnostic.path << "\n";
        return 1;
    }

    ProjectHybridPixelAuthoring authoring(manifest);
    if (authoring.revision() != 1U || authoring.profile() || authoring.can_undo() || authoring.can_redo()) return 2;
    const auto initial_text = read_text(manifest);
    const auto authored_profile = profile("hybrid.test", true);

    const auto dry_run = authoring.apply(authored_profile,
        ProjectHybridPixelEditOptions{.expected_revision = 1U, .dry_run = true});
    if (!dry_run.success || !dry_run.changed || dry_run.persisted || dry_run.revision != 1U ||
        !dry_run.profile || dry_run.profile->profile_id != "hybrid.test" || dry_run.can_undo ||
        authoring.profile() || authoring.revision() != 1U || read_text(manifest) != initial_text) {
        std::cerr << dry_run.to_json() << '\n';
        return 3;
    }

    const auto applied = authoring.apply(authored_profile,
        ProjectHybridPixelEditOptions{.expected_revision = 1U});
    if (!applied.success || !applied.changed || !applied.persisted || applied.revision != 2U ||
        !applied.profile || !applied.can_undo || applied.can_redo || !authoring.profile() ||
        authoring.profile()->enabled != true) {
        std::cerr << applied.to_json() << '\n';
        return 4;
    }
    const auto persisted_after_apply = Json::parse(read_text(manifest));
    if (persisted_after_apply.at("hybridPixelProfile").at("profileId") != "hybrid.test" ||
        persisted_after_apply.at("hybridPixelProfile").at("enabled") != true ||
        persisted_after_apply.at("futureProjectField") != original.at("futureProjectField") ||
        persisted_after_apply.at("inputActions") != original.at("inputActions")) return 5;

    const auto stale = authoring.replace(profile("stale", true),
        ProjectHybridPixelEditOptions{.expected_revision = 1U});
    if (stale.success || stale.code != "hybrid-pixel.revision-conflict" || stale.revision != 2U ||
        !stale.profile || stale.profile->profile_id != "hybrid.test" ||
        Json::parse(read_text(manifest)).at("hybridPixelProfile").at("profileId") != "hybrid.test") return 6;

    const auto disabled = authoring.disable(ProjectHybridPixelEditOptions{.expected_revision = 2U});
    if (!disabled.success || !disabled.changed || !disabled.persisted || disabled.revision != 3U ||
        !disabled.profile || disabled.profile->profile_id != "hybrid.test" || disabled.profile->enabled ||
        !authoring.profile() || authoring.profile()->enabled ||
        Json::parse(read_text(manifest)).at("hybridPixelProfile").at("enabled") != false) return 7;

    const auto undone = authoring.undo(ProjectHybridPixelEditOptions{.expected_revision = 3U});
    if (!undone.success || !undone.persisted || undone.revision != 4U || !undone.profile ||
        !undone.profile->enabled || !undone.can_redo || !Json::parse(read_text(manifest)).at("hybridPixelProfile").at("enabled")) return 8;

    const auto redone = authoring.redo(ProjectHybridPixelEditOptions{.expected_revision = 4U});
    if (!redone.success || !redone.persisted || redone.revision != 5U || !redone.profile ||
        redone.profile->enabled || redone.can_redo ||
        Json::parse(read_text(manifest)).at("hybridPixelProfile").at("enabled") != false) return 9;

    const auto removed = authoring.remove(ProjectHybridPixelEditOptions{.expected_revision = 5U});
    const auto removed_manifest = Json::parse(read_text(manifest));
    if (!removed.success || !removed.persisted || removed.revision != 6U || removed.profile ||
        removed_manifest.contains("hybridPixelProfile") || !removed.can_undo) return 10;

    const auto restored = authoring.undo(ProjectHybridPixelEditOptions{.expected_revision = 6U});
    if (!restored.success || !restored.persisted || restored.revision != 7U || !restored.profile ||
        restored.profile->enabled || !Json::parse(read_text(manifest)).contains("hybridPixelProfile")) return 11;

    auto replacement = *restored.profile;
    replacement.profile_id = "hybrid.replaced";
    replacement.virtual_width = 320U;
    const auto replaced = authoring.replace(replacement,
        ProjectHybridPixelEditOptions{.expected_revision = 7U});
    if (!replaced.success || replaced.revision != 8U || replaced.can_redo || !replaced.profile ||
        replaced.profile->profile_id != "hybrid.replaced") return 12;

    const auto dry_undo = authoring.undo(ProjectHybridPixelEditOptions{.expected_revision = 8U, .dry_run = true});
    if (!dry_undo.success || !dry_undo.changed || dry_undo.persisted || dry_undo.revision != 8U ||
        !dry_undo.profile || dry_undo.profile->profile_id != "hybrid.test" || authoring.revision() != 8U ||
        authoring.profile()->profile_id != "hybrid.replaced") return 13;

    auto invalid = replacement;
    invalid.virtual_width = 0U;
    const auto invalid_receipt = authoring.replace(invalid,
        ProjectHybridPixelEditOptions{.expected_revision = 8U});
    if (invalid_receipt.success || invalid_receipt.code != "hybrid-pixel.invalid-profile" ||
        !has_diagnostic(invalid_receipt.diagnostics, "hybrid-pixel.dimension-range") ||
        authoring.revision() != 8U || authoring.profile()->profile_id != "hybrid.replaced") return 14;

    const auto observation = Json::parse(authoring.observe_json());
    if (observation.at("schemaVersion").get<std::string>() != project_hybrid_pixel_authoring_schema ||
        observation.at("revision") != 8U || observation.at("profilePresent") != true ||
        observation.at("enabled") != false || observation.at("profile").at("profileId") != "hybrid.replaced" ||
        observation.at("canUndo") != true || observation.at("canRedo") != false) return 15;
    const auto receipt_json = Json::parse(replaced.to_json());
    if (receipt_json.at("profile").at("profileId") != "hybrid.replaced" ||
        receipt_json.at("canUndo") != true || receipt_json.at("canRedo") != false) return 16;

    const auto bad_root = root / "bad.project.json";
    {
        std::ofstream output(bad_root, std::ios::binary);
        output << R"({"schema":"noemancer.project/0.2","hybridPixelProfile":{"schema":"wrong"}})";
    }
    const auto bad_load = ProjectHybridPixelAuthoring::load_manifest(bad_root);
    if (bad_load.success || bad_load.code != "hybrid-pixel.manifest-invalid" ||
        bad_load.diagnostics.empty()) return 17;

    std::error_code cleanup_error;
    std::filesystem::remove_all(root, cleanup_error);
    if (cleanup_error || std::filesystem::exists(root)) return 18;
    return 0;
}
