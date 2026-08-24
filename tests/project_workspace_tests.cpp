#include "engine/asset_registry.hpp"
#include "engine/project_document.hpp"
#include "engine/project_workspace.hpp"

#include <nlohmann/json.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

int main() {
    const auto unique=std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    const auto root=(std::filesystem::temp_directory_path()/("noemancer-workspace-test-"+unique)).lexically_normal();
    const auto receipt=nlohmann::json::parse(noemancer::create_project_workspace_json({root,"Orbit Garden"}));
    if(!receipt.value("success",false)||receipt.value("code",std::string{})!="ok"||!std::filesystem::is_directory(root)) {
        std::cerr<<receipt.dump()<<'\n';return 1;
    }
    const auto loaded=noemancer::load_project(root);
    if(!loaded||loaded.project->project_id!="game.orbit-garden"||loaded.project->name!="Orbit Garden"||
        loaded.project->schema!="noemancer.project/0.2"||loaded.project->hybrid_pixel_profile||
        loaded.project->startup_scene.generic_string()!="scenes/main.scene.json"||!loaded.project->script_project||
        !std::filesystem::is_regular_file(root/ *loaded.project->script_project)||
        !std::filesystem::is_regular_file(root/"scripts"/"OrbitGarden.Gameplay"/"GameEntry.cs")||
        !std::filesystem::is_regular_file(root/"assets"/"animation"/"starter.animation-state-machine.json")||
        !std::filesystem::is_regular_file(root/"assets"/"animation"/"starter.animation-graph.json")||
        !loaded.project->hud_document||!std::filesystem::is_regular_file(root/ *loaded.project->hud_document)||
        loaded.project->hud_document_json.empty()||loaded.project->input_actions.size()!=3U) {
        std::cerr<<noemancer::project_load_errors_json(loaded)<<'\n';return 2;
    }
    const auto script=std::ranges::find(loaded.startup_scene->entities,"script.orbit-garden.entry",
        [](const noemancer::SceneEntityDocument& entity){return entity.managed_script?entity.managed_script->instance_id:std::string{};});
    if(script==loaded.startup_scene->entities.end()||script->managed_script->type_name!="OrbitGarden.GameEntry")return 3;
    const noemancer::AssetRegistry registry(root/"assets");
    if(registry.records().size()!=7U||registry.find("asset.primitive.cube")==nullptr||
        registry.find("asset.primitive.cube")->license!="LicenseRef-Noemancer-built-in"||
        registry.find("asset.texture.checker")==nullptr||registry.find("asset.animation.test-bob")==nullptr||
        registry.find("animation.machine.orbit-garden.starter")==nullptr||
        registry.find("animation.graph.orbit-garden.starter")==nullptr)return 4;
    const auto animation_inspection=nlohmann::json::parse(registry.inspect_json("animation.machine.orbit-garden.starter"));
    if(!animation_inspection.value("valid",false)||animation_inspection.value("code",std::string{})!="ok")return 4;
    const auto graph_inspection=nlohmann::json::parse(registry.inspect_json("animation.graph.orbit-garden.starter"));
    if(!graph_inspection.value("valid",false)||graph_inspection.value("code",std::string{})!="ok")return 4;
    const auto duplicate=nlohmann::json::parse(noemancer::create_project_workspace_json({root,"Duplicate"}));
    if(duplicate.value("success",true)||duplicate.value("code",std::string{})!="project.create-target-exists")return 5;
    std::error_code cleanup_error;std::filesystem::remove_all(root,cleanup_error);
    if(cleanup_error){std::cerr<<cleanup_error.message()<<'\n';return 6;}return 0;
}
