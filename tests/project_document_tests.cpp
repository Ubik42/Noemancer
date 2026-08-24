#include "engine/project_document.hpp"
#include "engine/project_input_authoring.hpp"
#include "engine/process_diagnostics.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>
#include <string_view>

namespace {

bool has_error(const noemancer::ProjectLoadResult& result,
               const std::string_view code, const std::string_view path) {
    for (const auto& error : result.errors) {
        if (error.code == code && error.path == path) return true;
    }
    return false;
}

constexpr auto valid_hybrid_pixel_profile = R"({
  "schema":"noemancer.hybrid-pixel-profile/0.1",
  "profileId":"hd2d.main",
  "enabled":true,
  "virtualWidth":640,
  "virtualHeight":360,
  "pixelsPerUnit":16,
  "integerScaling":true,
  "snapCamera":true,
  "snapSprites":true,
  "presentationFilter":"nearest"
})";

} // namespace

int main() {
    noemancer::configure_process_diagnostics("test.project-document");
    try {
    const auto root = std::filesystem::temp_directory_path() / "noemancer-project-document-test";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "scenes");
    std::filesystem::create_directories(root / "assets");
    std::filesystem::create_directories(root / "ui");
    {
        std::ofstream scene(root / "scenes" / "start.scene.json");
        scene << R"({"schema":"noemancer.scene/0.1","sceneGuid":"scene.test","name":"Test","entities":[{"guid":"entity.root","name":"Root","parent":null,"components":{"Transform":{"position":[0,0,0]}}}]})";
    }
    {
        std::ofstream hud(root / "ui" / "hud.ui.json");
        hud << R"({"schemaVersion":"noemancer.ui-document/0.1","documentId":"test.hud","nodes":[{"id":"test.hud","parentId":null,"role":"hud","label":"Test"}]})";
    }
    {
        // Legacy compatibility fixture: project/0.1 remains readable and
        // rejects only fields that require the current project/0.2 schema.
        std::ofstream manifest(root / "noemancer.project.json");
        manifest << R"({"schema":"noemancer.project/0.1","projectId":"project.test","name":"Test Project","startupScene":"scenes/start.scene.json","assetRoots":["assets"],"packagedAssets":["audio.jump"],"scriptProject":"scripts/Game.csproj","hudDocument":"ui/hud.ui.json","inputActions":[{"id":"gameplay.move.x","kind":"axis1d","bindings":[{"source":"keyboard.left","scale":-1},{"source":"gamepad.left.x","deadZone":0.25}]}]})";
    }
    const auto loaded = noemancer::load_project(root);
    if (!loaded || loaded.project->project_id != "project.test" || loaded.project->schema != "noemancer.project/0.1" ||
        loaded.project->hybrid_pixel_profile || !loaded.project->script_project ||
        loaded.project->script_project->generic_string() != "scripts/Game.csproj" ||
        loaded.startup_scene->scene_guid != "scene.test" || !loaded.project->hud_document || loaded.project->hud_document_json.empty() ||
        loaded.project->input_actions.size() != 1U ||loaded.project->packaged_assets!=std::vector<std::string>{"audio.jump"}||
        loaded.project->input_actions.front().bindings.at(1).dead_zone != 0.25F) {
        std::cerr << noemancer::project_load_errors_json(loaded) << '\n';
        std::filesystem::remove_all(root);
        return 1;
    }

    // Version 0.2 owns the embedded Hybrid Pixel profile.  The profile is
    // parsed by its own strict codec and then retained as engine plain data.
    {
        std::ofstream manifest(root / "noemancer.project.json", std::ios::trunc);
        manifest << R"({"schema":"noemancer.project/0.2","projectId":"project.test","name":"Test Project","startupScene":"scenes/start.scene.json","assetRoots":["assets"],"hybridPixelProfile":)"
                 << valid_hybrid_pixel_profile << '}';
    }
    const auto loaded_v2 = noemancer::load_project(root);
    if (!loaded_v2 || loaded_v2.project->schema != "noemancer.project/0.2" ||
        !loaded_v2.project->hybrid_pixel_profile ||
        loaded_v2.project->hybrid_pixel_profile->profile_id != "hd2d.main" ||
        loaded_v2.project->hybrid_pixel_profile->virtual_width != 640U) {
        std::cerr << noemancer::project_load_errors_json(loaded_v2) << '\n';
        std::filesystem::remove_all(root);
        return 4;
    }

    {
        std::ofstream manifest(root / "noemancer.project.json", std::ios::trunc);
        manifest << R"({"schema":"noemancer.project/0.2","projectId":"project.test","name":"Test Project","startupScene":"scenes/start.scene.json","assetRoots":["assets"]})";
    }
    const auto loaded_v2_without_profile = noemancer::load_project(root);
    if (!loaded_v2_without_profile || loaded_v2_without_profile.project->hybrid_pixel_profile) {
        std::filesystem::remove_all(root);
        return 9;
    }

    // Input authoring is allowed to update actions in either project schema,
    // but it must preserve the profile, source schema and unrelated fields.
    {
        std::ofstream manifest(root / "noemancer.project.json", std::ios::trunc);
        manifest << R"({"schema":"noemancer.project/0.2","projectId":"project.test","name":"Test Project","startupScene":"scenes/start.scene.json","assetRoots":["assets"],"futureProjectField":{"preserve":true},"hybridPixelProfile":)"
                 << valid_hybrid_pixel_profile << '}';
    }
    noemancer::ProjectInputAuthoring authoring;
    const auto authoring_save = authoring.save_project_manifest(root / "noemancer.project.json");
    if (!authoring_save.success) {
        std::cerr << authoring_save.detail << '\n';
        std::filesystem::remove_all(root);
        return 5;
    }
    std::ifstream persisted_stream(root / "noemancer.project.json", std::ios::binary);
    const auto persisted_text = std::string(std::istreambuf_iterator<char>(persisted_stream),
        std::istreambuf_iterator<char>());
    persisted_stream.close();
    const auto persisted = nlohmann::json::parse(persisted_text, nullptr, false);
    if (persisted.is_discarded() || persisted.at("schema") != "noemancer.project/0.2" ||
        persisted.at("futureProjectField").at("preserve") != true ||
        persisted.at("hybridPixelProfile").at("profileId") != "hd2d.main") {
        std::filesystem::remove_all(root);
        return 6;
    }

    // A 0.1 manifest may still be read, but the new field cannot be silently
    // accepted under the legacy schema.
    {
        std::ofstream manifest(root / "noemancer.project.json", std::ios::trunc);
        manifest << R"({"schema":"noemancer.project/0.1","projectId":"project.test","name":"Test Project","startupScene":"scenes/start.scene.json","assetRoots":["assets"],"hybridPixelProfile":)"
                 << valid_hybrid_pixel_profile << '}';
    }
    const auto legacy_profile = noemancer::load_project(root);
    if (legacy_profile || !has_error(legacy_profile,
        "project.hybrid-pixel-profile-schema", "/hybridPixelProfile")) {
        std::filesystem::remove_all(root);
        return 7;
    }

    // 0.2 profile errors are promoted without losing the nested path.
    {
        std::ofstream manifest(root / "noemancer.project.json", std::ios::trunc);
        manifest << R"({"schema":"noemancer.project/0.2","projectId":"project.test","name":"Test Project","startupScene":"scenes/start.scene.json","assetRoots":["assets"],"hybridPixelProfile":{"schema":"noemancer.hybrid-pixel-profile/0.1","profileId":"hd2d.main","enabled":true,"virtualWidth":0,"virtualHeight":360,"pixelsPerUnit":16,"integerScaling":true,"snapCamera":true,"snapSprites":true,"presentationFilter":"linear","unknown":true}})";
    }
    const auto invalid_profile = noemancer::load_project(root);
    if (invalid_profile || !has_error(invalid_profile,
        "project.hybrid-pixel-profile.hybrid-pixel.unknown-field",
        "/hybridPixelProfile/unknown") || !has_error(invalid_profile,
        "project.hybrid-pixel-profile.hybrid-pixel.dimension-range",
        "/hybridPixelProfile/virtualWidth") || !has_error(invalid_profile,
        "project.hybrid-pixel-profile.hybrid-pixel.invalid-filter",
        "/hybridPixelProfile/presentationFilter")) {
        std::filesystem::remove_all(root);
        return 8;
    }
    {
        std::ofstream manifest(root / "noemancer.project.json", std::ios::trunc);
        manifest << R"({"schema":"noemancer.project/0.1","projectId":"project.test","name":"Test Project","startupScene":"scenes/start.scene.json","assetRoots":["assets"],"inputActions":[{"id":"gameplay.jump","kind":"button","bindings":[{"source":"keyboard.space","deadZone":1.0}]}]})";
    }
    const auto invalid_input = noemancer::load_project(root);
    if (invalid_input || invalid_input.errors.empty() || invalid_input.errors.front().code != "project.invalid-input-binding") {
        std::cerr << "Invalid input binding was not rejected: "
                  << noemancer::project_load_errors_json(invalid_input) << '\n';
        std::filesystem::remove_all(root);
        return 3;
    }
    {
        std::ofstream manifest(root / "noemancer.project.json", std::ios::trunc);
        manifest << R"({"schema":"noemancer.project/0.1","projectId":"project.test","name":"Test Project","startupScene":"../outside.scene.json","assetRoots":["assets"]})";
    }
    const auto rejected = noemancer::load_project(root);
    if (rejected || rejected.errors.empty() || rejected.errors.front().code != "project.unsafe-path") {
        std::cerr << "Unsafe project-relative path was not rejected\n";
        std::filesystem::remove_all(root);
        return 2;
    }
    std::filesystem::remove_all(root);
    return 0;
    } catch (const std::exception& exception) {
        std::cerr << "project_document_tests uncaught exception: "
                  << exception.what() << '\n';
        return 99;
    }
}
