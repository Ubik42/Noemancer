#include "engine/scene_document.hpp"
#include "engine/world.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <string>

namespace {
using Json=nlohmann::json;

Json invoke_contact(noemancer::World& world,const std::string_view other) {
    return Json::parse(world.scripting_invoke_json("script.e2.player","OnTriggerEnter",Json{
        {"properties",{{"spawnX",-6.0F},{"spawnY",2.0F},{"spawnZ",0.0F}}},
        {"contact",{{"selfId","entity.demo-cube"},{"otherId",other},
            {"normal",{{"x",0.0F},{"y",1.0F},{"z",0.0F}}},{"penetration",0.0F},{"isTrigger",true}}}
    }.dump()));
}
}

int main() {
    noemancer::World world;
    if(!world.load_scene(noemancer::make_bootstrap_scene_document()).success)return 1;
    const auto project=std::filesystem::path(NOEMANCER_SOURCE_DIR)/"tests/fixtures/e2-game-project/E2GameplayProof.csproj";
    const auto configured=Json::parse(world.scripting_project_configure_json(project.parent_path(),project));
    const auto compiled=Json::parse(world.scripting_project_compile_json("Debug"));
    if(!configured.at("success")||!compiled.at("success"))return 2;

    const auto tag=[&](const std::string_view instance,const std::string_view entity,const std::string_view value) {
        const auto attached=Json::parse(world.scripting_attach_json(instance,entity,"project.script","E2GameplayProof.TagAuthor"));
        const auto invoked=Json::parse(world.scripting_invoke_json(instance,"OnCreate",Json{{"properties",{{"tag",value}}}}.dump()));
        return attached.value("success",false)&&invoked.value("success",false);
    };
    if(!tag("script.e2.collectible-tag","entity.demo-sphere","gameplay.collectible")||
       !tag("script.e2.hazard-tag","entity.demo-cube-secondary","gameplay.hazard")||
       !tag("script.e2.goal-tag","entity.demo-skeletal-cube","gameplay.goal"))return 3;
    if(!Json::parse(world.scripting_attach_json("script.e2.player","entity.demo-cube","project.script","E2GameplayProof.PlayerGameplay")).at("success"))return 4;

    const auto collected=invoke_contact(world,"entity.demo-sphere");
    const auto after_collect=world.entity_views();
    if(!collected.at("success")||collected.at("commandApplication").at("applied")!=3||
       collected.at("managedResult").at("state").at("CollectedCount")!=1||
       std::ranges::find(after_collect,std::string("entity.demo-sphere"),&noemancer::WorldEntityView::id)!=after_collect.end()||
       world.gameplay_observation_json().find("gameplay.item.collected")==std::string::npos)return 5;

    const auto respawned=invoke_contact(world,"entity.demo-cube-secondary");
    const auto after_respawn=world.entity_views();
    const auto player=std::ranges::find(after_respawn,std::string("entity.demo-cube"),&noemancer::WorldEntityView::id);
    if(!respawned.at("success")||respawned.at("commandApplication").at("applied")!=1||
       respawned.at("managedResult").at("state").at("RespawnCount")!=1||player==after_respawn.end()||!player->transform||
       player->transform->x!=-6.0F||player->transform->y!=2.0F||player->transform->z!=0.0F)return 6;

    const auto completed=invoke_contact(world,"entity.demo-skeletal-cube");
    if(!completed.at("success")||completed.at("commandApplication").at("applied")!=1||
       !completed.at("managedResult").at("state").at("Won")||
       world.gameplay_observation_json().find("gameplay.level.completed")==std::string::npos)return 7;
    std::cout<<"Independent C# project completed collectible, respawn and goal gameplay without engine changes\n";
    return 0;
}
