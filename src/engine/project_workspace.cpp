#include "engine/project_workspace.hpp"

#include "engine/animation_graph.hpp"
#include "engine/animation_state_machine.hpp"
#include "engine/hybrid_pixel_profile.hpp"
#include "engine/scene_document.hpp"
#include "engine/gameplay_runtime.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <fstream>
#include <system_error>

namespace noemancer {
namespace {

using Json=nlohmann::json;

std::string slug(std::string_view value) {
    std::string result;bool separator{};
    for(const auto raw:value) {
        const auto character=static_cast<unsigned char>(raw);
        if(std::isalnum(character)) {result.push_back(static_cast<char>(std::tolower(character)));separator=false;}
        else if(!result.empty()&&!separator) {result.push_back('-');separator=true;}
    }
    while(!result.empty()&&result.back()=='-')result.pop_back();
    return result.empty()?"game":result;
}

std::string identifier(std::string_view value) {
    std::string result;bool upper=true;
    for(const auto raw:value) {
        const auto character=static_cast<unsigned char>(raw);
        if(std::isalnum(character)) {result.push_back(upper?static_cast<char>(std::toupper(character)):static_cast<char>(character));upper=false;}
        else upper=true;
    }
    if(result.empty()||std::isdigit(static_cast<unsigned char>(result.front())))result="Game"+result;
    return result;
}

bool write_text(const std::filesystem::path& path,const std::string_view text,std::error_code& error) {
    std::filesystem::create_directories(path.parent_path(),error);if(error)return false;
    std::ofstream output(path,std::ios::binary|std::ios::trunc);if(!output){error=std::make_error_code(std::errc::io_error);return false;}
    output.write(text.data(),static_cast<std::streamsize>(text.size()));output.flush();
    if(!output){error=std::make_error_code(std::errc::io_error);return false;}return true;
}

Json failure(const std::string_view code,const std::string_view detail,const std::filesystem::path& root={}) {
    return {{"schemaVersion","noemancer.project-workspace-action/0.1"},{"success",false},{"code",code},
        {"operation","project.create"},{"detail",detail},{"projectPath",root.generic_string()}};
}

Json starter_input_actions() {
    Json actions=Json::array();
    for(const auto& action:default_input_action_definitions()) {
        Json bindings=Json::array();
        for(const auto& binding:action.bindings)bindings.push_back({{"source",binding.source},{"scale",binding.scale},
            {"deadZone",binding.dead_zone}});
        actions.push_back({{"id",action.id},{"kind",action.kind==InputActionKind::button?"button":"axis1d"},
            {"bindings",std::move(bindings)}});
    }
    return actions;
}

Json starter_hud_document(const std::string_view name) {
    const std::string root_id="game.hud.main";
    return {{"schemaVersion","noemancer.ui-document/0.1"},
        {"documentId",root_id},{"surface","game"},{"kind","hud"},{"revision",1},
        {"locale","en-US"},{"roots",Json::array({root_id})},
        {"designTokens",{{"surfaceColor","#0b1018dc"},{"groupColor","#182334e8"},{"textColor","#e8edf5"},
            {"accentColor","#62d7ff"},{"surfaceWidthPx",320}}},
        {"capabilities",{{"semanticQuery",true},{"transactionActions",true},{"layoutEvidence",true}}},
        {"nodes",Json::array({
            Json{{"id",root_id},{"parentId",nullptr},{"role","hud"},{"label",std::string(name)},
                {"state",{{"visible",true},{"enabled",true},{"editable",false}}},{"actions",Json::array()}},
            Json{{"id",root_id+".status"},{"parentId",root_id},{"role","status"},{"label","Status"},{"value","Ready"},
                {"state",{{"visible",true},{"enabled",true},{"editable",false}}},{"actions",Json::array()}}
        })}};
}

HybridPixelProfile starter_hybrid_pixel_profile(const std::string_view project_slug) {
    HybridPixelProfile profile;
    // Keep the profile identity stable within the generated project while
    // retaining the codec's complete, validated starter defaults.
    profile.profile_id = "project." + std::string(project_slug) + ".hybrid-pixel";
    return profile;
}

AnimationStateMachineDocument starter_animation_state_machine(const std::string_view project_slug) {
    AnimationStateMachineDocument document;
    document.asset_id="animation.machine."+std::string(project_slug)+".starter";
    document.initial_state="idle";
    document.parameters={{"speed","float",0.0F},{"grounded","bool",1.0F}};
    document.states={{"idle","asset.animation.test-bob",true},{"moving","asset.animation.test-bob",true}};
    document.transitions={
        {"moving.enter","idle","moving",{{"parameter","speed","greater",0.1F,{}}},0.15F,10},
        {"moving.exit","moving","idle",{{"parameter","speed","less-or-equal",0.1F,{}}},0.15F,10}};
    return document;
}

AnimationGraphDocument starter_animation_graph(const std::string_view project_slug,
                                                const AnimationStateMachineDocument& machine) {
    AnimationGraphDocument document;
    document.asset_id="animation.graph."+std::string(project_slug)+".starter";
    document.parameters={{"speed","float",0.0F},{"grounded","bool",1.0F}};
    document.nodes={{.id="node.locomotion",.kind="state-machine",.state_machine_asset=machine.asset_id}};
    document.layers={{.id="layer.base",.root_node="node.locomotion",.mode="override",.weight=1.0F,
        .sync_group="locomotion"}};
    document.sync_groups={{"locomotion","normalized-time"}};
    document.editor.nodes={{"node.locomotion",120.0F,120.0F,false}};
    return document;
}

} // namespace

std::string create_project_workspace_json(const ProjectWorkspaceCreateRequest& request) {
    std::error_code error;const auto root=std::filesystem::absolute(request.root,error).lexically_normal();
    if(error||request.root.empty()||!request.root.is_absolute()||root.filename().empty())
        return failure("project.create-invalid-root","Project root must be an absolute path naming a new directory.",root).dump();
    if(request.name.empty()||request.name.size()>96U)
        return failure("project.create-invalid-name","Project name must contain between 1 and 96 characters.",root).dump();
    if(request.preset!=project_workspace_preset_starter&&
       request.preset!=project_workspace_preset_hybrid_pixel)
        return failure("project.create-invalid-preset",
            "Project workspace preset must be starter or hybrid-pixel.",root).dump();
    if(std::filesystem::exists(root,error)||error)
        return failure("project.create-target-exists","Project creation never overwrites an existing path.",root).dump();

    static std::atomic_uint64_t sequence{1U};const auto project_slug=slug(request.name);const auto code_name=identifier(request.name);
    const auto stage=root.parent_path()/("."+root.filename().string()+".creating-"+std::to_string(sequence.fetch_add(1U)));
    if(std::filesystem::exists(stage,error)||error)
        return failure("project.create-staging-exists","A sibling project staging directory already exists.",root).dump();

    auto scene=make_bootstrap_scene_document();scene.scene_guid="scene."+project_slug+".main";scene.name=request.name+" Main";
    scene.source_uri=(root/"scenes"/"main.scene.json").generic_string();
    std::erase_if(scene.entities,[](const SceneEntityDocument& entity){return entity.guid.starts_with("entity.test-");});
    if(const auto cube=std::ranges::find(scene.entities,"entity.demo-cube",&SceneEntityDocument::guid);cube!=scene.entities.end())
        cube->managed_script=SceneManagedScript{"script."+project_slug+".entry","project.script",code_name+".GameEntry",true,"{}"};
    const auto animation_machine=starter_animation_state_machine(project_slug);
    const auto animation_graph=starter_animation_graph(project_slug,animation_machine);
    if(const auto actor=std::ranges::find(scene.entities,"entity.demo-skeletal-cube",&SceneEntityDocument::guid);
       actor!=scene.entities.end()&&actor->animation_player)
        { actor->animation_player->state_machine_asset=animation_machine.asset_id;
          actor->animation_player->animation_graph_asset=animation_graph.asset_id; }

    const auto script_directory=std::filesystem::path("scripts")/(code_name+".Gameplay");
    const auto script_project=script_directory/(code_name+".Gameplay.csproj");
    Json manifest_document=Json{{"schema","noemancer.project/0.2"},{"projectId","game."+project_slug},{"name",request.name},
        {"startupScene","scenes/main.scene.json"},{"assetRoots",Json::array({"assets"})},
        {"scriptProject",script_project.generic_string()},{"hudDocument","ui/hud.ui.json"},
        {"inputActions",starter_input_actions()}};
    if(request.preset==project_workspace_preset_hybrid_pixel) {
        const auto profile=starter_hybrid_pixel_profile(project_slug);
        manifest_document["hybridPixelProfile"]=
            Json::parse(HybridPixelProfileCodec::write_canonical_json(profile));
    }
    const auto manifest=manifest_document.dump(2)+"\n";
    const auto csproj=std::string{R"(<Project Sdk="Microsoft.NET.Sdk">
  <PropertyGroup>
    <TargetFramework>net10.0</TargetFramework>
    <ImplicitUsings>enable</ImplicitUsings>
    <Nullable>enable</Nullable>
  </PropertyGroup>
  <ItemGroup>
    <ProjectReference Include="$(NoemancerSdkRoot)/managed/Noemancer.Managed/Noemancer.Managed.csproj" />
  </ItemGroup>
</Project>
)"};
    const auto source="using Noemancer;\n\nnamespace "+code_name+";\n\npublic sealed class GameEntry : ScriptBehaviour\n{\n"+
        "    public override void OnCreate(in ScriptContext context)\n    {\n        // Your game starts here.\n    }\n}\n";
    const auto asset_registry=Json{{"schema","noemancer.assets/0.1"},{"assets",Json::array({
        Json{{"id","asset.primitive.cube"},{"displayName","Cube"},{"kind","Geometry"},{"uri","builtin://geometry/cube"},
            {"license","LicenseRef-Noemancer-built-in"},{"redistribution","public"},{"tags",Json::array({"primitive","geometry"})}},
        Json{{"id","asset.primitive.plane"},{"displayName","Plane"},{"kind","Geometry"},{"uri","builtin://geometry/plane"},
            {"license","LicenseRef-Noemancer-built-in"},{"redistribution","public"},{"tags",Json::array({"primitive","geometry"})}},
        Json{{"id","asset.primitive.sphere"},{"displayName","Sphere"},{"kind","Geometry"},{"uri","builtin://geometry/sphere"},
            {"license","LicenseRef-Noemancer-built-in"},{"redistribution","public"},{"tags",Json::array({"primitive","geometry"})}},
        Json{{"id","asset.texture.checker"},{"displayName","Checker Texture"},{"kind","Texture"},{"uri","builtin://texture/checker"},
            {"license","LicenseRef-Noemancer-built-in"},{"redistribution","public"},{"tags",Json::array({"texture","procedural"})}},
        Json{{"id","asset.animation.test-bob"},{"displayName","Starter Bob Animation"},{"kind","Animation"},
            {"uri","builtin://animation/test-bob"},{"license","LicenseRef-Noemancer-built-in"},{"redistribution","public"},
            {"tags",Json::array({"animation","procedural"})}},
        Json{{"id",animation_machine.asset_id},{"displayName","Starter Animation State Machine"},
            {"kind","AnimationStateMachine"},{"uri","asset://animation/starter.animation-state-machine.json"},
            {"path","animation/starter.animation-state-machine.json"},{"license","Noemancer project"},
            {"redistribution","public"},{"tags",Json::array({"animation","state-machine","starter"})}},
        Json{{"id",animation_graph.asset_id},{"displayName","Starter Animation Graph"},
            {"kind","AnimationGraph"},{"uri","asset://animation/starter.animation-graph.json"},
            {"path","animation/starter.animation-graph.json"},{"license","Noemancer project"},
            {"redistribution","public"},{"tags",Json::array({"animation","graph","starter"})}}
    })}}.dump(2)+"\n";
    const auto gitignore=std::string{"generated/\n**/bin/\n**/obj/\ndist/\n"};

    bool written=write_text(stage/"noemancer.project.json",manifest,error)&&
        write_text(stage/"scenes"/"main.scene.json",SceneDocumentCodec::write_canonical_json(scene)+"\n",error)&&
        write_text(stage/script_project,csproj,error)&&write_text(stage/script_directory/"GameEntry.cs",source,error)&&
        write_text(stage/"ui"/"hud.ui.json",starter_hud_document(request.name).dump(2)+"\n",error)&&
        write_text(stage/"assets"/"animation"/"starter.animation-state-machine.json",
            AnimationStateMachineCodec::write_canonical_json(animation_machine)+"\n",error)&&
        write_text(stage/"assets"/"animation"/"starter.animation-graph.json",
            AnimationGraphCodec::write_canonical_json(animation_graph)+"\n",error)&&
        write_text(stage/"assets"/"registry.json",asset_registry,error)&&
        write_text(stage/".gitignore",gitignore,error);
    if(!written||error) {
        std::error_code cleanup_error;std::filesystem::remove_all(stage,cleanup_error);
        return failure("project.create-write-failed",error.message(),root).dump();
    }
    std::filesystem::rename(stage,root,error);
    if(error) {std::error_code cleanup_error;std::filesystem::remove_all(stage,cleanup_error);
        return failure("project.create-commit-failed",error.message(),root).dump();}
    return Json{{"schemaVersion","noemancer.project-workspace-action/0.1"},{"success",true},{"code","ok"},
        {"operation","project.create"},{"detail","Project workspace created atomically."},{"projectPath",root.generic_string()},
        {"preset",request.preset},
        {"manifest",(root/"noemancer.project.json").generic_string()},{"startupScene",(root/"scenes"/"main.scene.json").generic_string()},
        {"scriptProject",(root/script_project).generic_string()}}.dump();
}

} // namespace noemancer
