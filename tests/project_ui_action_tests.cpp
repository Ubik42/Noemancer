#include "engine/command_registry.hpp"
#include "engine/project_ui_authoring.hpp"
#include "engine/retained_ui_runtime.hpp"
#include "engine/world.hpp"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <iostream>

namespace {
using Json=nlohmann::json;

bool succeeded(const Json& value) {
    return value.is_object()&&value.value("success",false);
}
}

int main() {
    noemancer::World world;
    if(!world.load_scene(noemancer::make_bootstrap_scene_document()).success)return 1;
    const auto script_project=std::filesystem::path(NOEMANCER_SOURCE_DIR)/
        "tests/fixtures/managed-project/ManagedFixture.csproj";
    const auto configured=Json::parse(world.scripting_project_configure_json(script_project.parent_path(),script_project));
    const auto compiled=Json::parse(world.scripting_project_compile_json("Debug"));
    const auto attached=Json::parse(world.scripting_attach_json(
        "script.ui","entity.demo-cube","asset.script.fixture","ManagedFixture.UiController"));
    if(!succeeded(configured)||!succeeded(compiled)||!succeeded(attached)) {
        std::cerr<<configured.dump(2)<<'\n'<<compiled.dump(2)<<'\n'<<attached.dump(2)<<'\n';return 2;
    }

    const auto document=Json{{"schemaVersion","noemancer.ui-document/0.1"},{"documentId","project.hud"},
        {"revision",7},{"nodes",Json::array({
            Json{{"id","project.hud"},{"parentId",nullptr},{"role","hud"},{"label","HUD"},
                {"state",{{"visible",true},{"enabled",true},{"editable",false}}},{"actions",Json::array()}},
            Json{{"id","project.hud.resume"},{"parentId","project.hud"},{"role","button"},{"label","Resume"},
                {"state",{{"visible",true},{"enabled",true},{"editable",false}}},
                {"actions",Json::array({Json{{"id","game.pause.resume"},
                    {"binding",{{"kind","script-callback"},{"instanceId","script.ui"}}}}})}}
        })}};
    if(!world.configure_project_hud(document.dump())) {std::cerr<<document.dump(2)<<'\n';return 3;}
    const auto markup=noemancer::retained_ui_rml_from_semantic_document(document.dump());
    if(markup.find("<button type=\"button\" id=\"project.hud.resume\"")==std::string::npos||
       markup.find("data-action=\"game.pause.resume\"")==std::string::npos||
       markup.find("script-callback")==std::string::npos)return 31;
    const auto dry_run=Json::parse(world.project_ui_action_invoke_json(
        "project.hud.resume","game.pause.resume","invoke","null",7,true,"test.ui",1));
    if(!succeeded(dry_run)||!dry_run.at("dryRun")||dry_run.at("instanceId")!="script.ui") {
        std::cerr<<dry_run.dump(2)<<'\n';return 4;
    }
    const auto invoked=Json::parse(world.project_ui_action_invoke_json(
        "project.hud.resume","game.pause.resume","invoke",R"({"pointer":"primary"})",7,false,"test.ui",2));
    if(!succeeded(invoked)||invoked.at("scriptReceipt").at("managedResult").at("state").at("ActionCount")!=1||
       invoked.at("scriptReceipt").at("managedResult").at("state").at("LastActionId")!="game.pause.resume"||
       invoked.at("scriptReceipt").at("managedResult").at("state").at("LastNodeId")!="project.hud.resume") {
        std::cerr<<invoked.dump(2)<<'\n';return 5;
    }
    const auto stale=Json::parse(world.project_ui_action_invoke_json(
        "project.hud.resume","game.pause.resume","invoke","null",6,true,"test.ui",3));
    const auto undeclared=Json::parse(world.project_ui_action_invoke_json(
        "project.hud.resume","game.pause.quit","invoke","null",7,true,"test.ui",4));
    if(stale.value("code","")!="ui.document-revision-conflict"||
       undeclared.value("code","")!="ui.action-not-declared") {
        std::cerr<<stale.dump(2)<<'\n'<<undeclared.dump(2)<<'\n';return 6;
    }

    noemancer::CommandRegistry registry(world);
    const auto agent=registry.invoke("ui.project.action.invoke",
        R"({"nodeId":"project.hud.resume","actionId":"game.pause.resume","baseRevision":7,"dryRun":true})");
    const auto agent_receipt=Json::parse(agent.output_json);
    if(agent.exit_code!=0||!agent_receipt.value("ok",false)||
       !succeeded(agent_receipt.value("result",Json::object()))) {
        std::cerr<<agent.exit_code<<' '<<agent.output_json<<'\n';return 7;
    }
    const auto authoring_path=std::filesystem::temp_directory_path()/"noemancer-project-ui-action-test.ui.json";
    {std::ofstream output(authoring_path,std::ios::binary|std::ios::trunc);output<<document.dump();}
    auto authoring=noemancer::ProjectUiAuthoringSession::from_file(authoring_path);
    registry.attach_project_ui_authoring(authoring);
    const auto source_observe=registry.invoke("ui.project.source.observe","{}");
    const auto source_edit=registry.invoke("ui.project.source.edit",
        R"({"operation":"update","baseRevision":7,"dryRun":true,"nodeId":"project.hud.resume","label":"Continue"})");
    const auto source_commit=registry.invoke("ui.project.source.edit",
        R"({"operation":"update","baseRevision":7,"nodeId":"project.hud.resume","label":"Continue"})");
    const auto observe_envelope=Json::parse(source_observe.output_json);
    const auto edit_envelope=Json::parse(source_edit.output_json);
    const auto commit_envelope=Json::parse(source_commit.output_json);
    if(source_observe.exit_code!=0||source_edit.exit_code!=0||source_commit.exit_code!=0||
       observe_envelope.at("result").value("revision",0U)!=7U||
       !edit_envelope.at("result").value("success",false)||
       !edit_envelope.at("result").value("changed",false)||
       edit_envelope.at("result").value("runtimeHotApplyAttempted",true)||
       edit_envelope.at("result").value("runtimeHotApplied",true)||
       !commit_envelope.at("result").value("success",false)||
       !commit_envelope.at("result").value("runtimeHotApplyAttempted",false)||
       !commit_envelope.at("result").value("runtimeHotApplied",false)||authoring.revision()!=8U) {
        std::cerr<<source_observe.output_json<<'\n'<<source_edit.output_json<<'\n'
                 <<source_commit.output_json<<'\n';return 8;
    }
    std::error_code cleanup_error;
    std::filesystem::remove(authoring_path,cleanup_error);
    return 0;
}
