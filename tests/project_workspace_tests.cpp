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

    // The Hybrid Pixel preset only adds the one engine-owned profile; the
    // generated project must retain the complete starter contract and load
    // through the same ProjectDocument path as the default workspace.
    const auto hybrid_root=root.parent_path()/(root.filename().string()+"-hybrid");
    const auto hybrid_receipt=nlohmann::json::parse(
        noemancer::create_project_workspace_json({hybrid_root,"Pixel Garden",
            std::string(noemancer::project_workspace_preset_hybrid_pixel)}));
    const auto hybrid=noemancer::load_project(hybrid_root);
    if(!hybrid_receipt.value("success",false)||
       hybrid_receipt.value("code",std::string{})!="ok"||
       hybrid_receipt.value("preset",std::string{})!=noemancer::project_workspace_preset_hybrid_pixel||!hybrid||
       hybrid.project->schema!="noemancer.project/0.2"||
       hybrid.project->project_id!="game.pixel-garden"||
       !hybrid.project->hybrid_pixel_profile||
       hybrid.project->hybrid_pixel_profile->schema!=noemancer::hybrid_pixel_profile_schema||
       hybrid.project->hybrid_pixel_profile->profile_id!="project.pixel-garden.hybrid-pixel"||
       hybrid.project->hybrid_pixel_profile->virtual_width!=320U||
       hybrid.project->hybrid_pixel_profile->virtual_height!=180U||
       hybrid.project->hybrid_pixel_profile->pixels_per_unit!=16.0F||
       !hybrid.project->hybrid_pixel_profile->enabled||
       !hybrid.project->hybrid_pixel_profile->integer_scaling||
       !hybrid.project->hybrid_pixel_profile->snap_camera||
       !hybrid.project->hybrid_pixel_profile->snap_sprites||
       hybrid.project->hybrid_pixel_profile->presentation_filter!="nearest"||
       !std::filesystem::is_regular_file(hybrid_root/"noemancer.project.json")||
       !std::filesystem::is_regular_file(hybrid_root/"scenes"/"main.scene.json")||
       !std::filesystem::is_regular_file(hybrid_root/"scripts"/"PixelGarden.Gameplay"/"GameEntry.cs")||
       !std::filesystem::is_regular_file(hybrid_root/"scripts"/"PixelGarden.Gameplay"/"PixelGarden.Gameplay.csproj")||
       !std::filesystem::is_regular_file(hybrid_root/"ui"/"hud.ui.json")||
       !std::filesystem::is_regular_file(hybrid_root/"assets"/"registry.json")||
       hybrid.project->input_actions.size()!=3U) {
        std::cerr<<noemancer::project_load_errors_json(hybrid)<<'\n';return 7;
    }
    std::ifstream hybrid_manifest_stream(hybrid_root/"noemancer.project.json",std::ios::binary);
    const auto hybrid_manifest=nlohmann::json::parse(hybrid_manifest_stream,nullptr,false);
    hybrid_manifest_stream.close();
    const auto expected_profile=nlohmann::json::parse(
        noemancer::HybridPixelProfileCodec::write_canonical_json(*hybrid.project->hybrid_pixel_profile),
        nullptr,false);
    if(hybrid_manifest.is_discarded()||hybrid_manifest.value("schema",std::string{})!="noemancer.project/0.2"||
       hybrid_manifest.value("hybridPixelProfile",nlohmann::json(nullptr))!=expected_profile||
       hybrid_manifest.value("startupScene",std::string{})!="scenes/main.scene.json"||
       hybrid_manifest.value("assetRoots",nlohmann::json(nullptr))!=nlohmann::json::array({"assets"})||
       !hybrid_manifest.contains("scriptProject")||!hybrid_manifest.contains("hudDocument")||
       !hybrid_manifest.contains("inputActions"))return 8;

    // Preset validation happens before staging, so an unknown preset cannot
    // leave either a target directory or a partially written sibling behind.
    const auto invalid_root=root.parent_path()/(root.filename().string()+"-invalid");
    const auto invalid=nlohmann::json::parse(
        noemancer::create_project_workspace_json({invalid_root,"Invalid Preset","not-a-preset"}));
    if(invalid.value("success",true)||
       invalid.value("code",std::string{})!="project.create-invalid-preset"||
       std::filesystem::exists(invalid_root))return 9;

    const auto duplicate=nlohmann::json::parse(noemancer::create_project_workspace_json({root,"Duplicate"}));
    if(duplicate.value("success",true)||duplicate.value("code",std::string{})!="project.create-target-exists")return 10;
    std::error_code cleanup_error;std::filesystem::remove_all(root,cleanup_error);
    if(cleanup_error){std::cerr<<cleanup_error.message()<<'\n';return 11;}
    std::filesystem::remove_all(hybrid_root,cleanup_error);
    if(cleanup_error){std::cerr<<cleanup_error.message()<<'\n';return 12;}
    std::filesystem::remove_all(invalid_root,cleanup_error);
    if(cleanup_error){std::cerr<<cleanup_error.message()<<'\n';return 13;}return 0;
}
