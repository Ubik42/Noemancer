#include "engine/animation_state_machine.hpp"
#include "engine/scene_document.hpp"
#include "engine/world.hpp"

#include <iostream>
#include <nlohmann/json.hpp>

int main() {
    constexpr auto source=R"({
      "schemaVersion":"noemancer.animation-state-machine/0.2",
      "assetId":"animation.machine.test-production",
      "initialState":"idle",
      "parameters":[{"id":"speed","type":"float","default":0},{"id":"grounded","type":"bool","default":1}],
      "states":[
        {"id":"idle","clipAsset":"asset.animation.test-bob","looping":true},
        {"id":"move","clipAsset":"asset.animation.test-run","looping":true},
        {"id":"air","clipAsset":"asset.animation.test-air","looping":false}
      ],
      "transitions":[
        {"id":"air.enter","from":"*","to":"air","priority":100,"durationSeconds":0.05,
         "conditions":[{"source":"parameter","parameter":"grounded","comparison":"less","threshold":0.5}]},
        {"id":"move.enter","from":"idle","to":"move","priority":50,"durationSeconds":0.2,
         "conditions":[{"source":"parameter","parameter":"grounded","comparison":"greater-or-equal","threshold":0.5},
                       {"source":"parameter","parameter":"speed","comparison":"greater","threshold":0.1}]}
      ]
    })";
    auto parsed=noemancer::AnimationStateMachineCodec::parse_json(source);
    if(!parsed||parsed.document->states.size()!=3U||parsed.document->transitions.size()!=2U||
       parsed.document->transitions[0].id!="air.enter"||
       noemancer::AnimationStateMachineCodec::write_canonical_json(*parsed.document).find("conditions")==std::string::npos) {
        std::cerr<<"Animation State Machine asset did not parse or normalize deterministically\n";return 1;
    }
    const auto invalid=noemancer::AnimationStateMachineCodec::parse_json(
        R"({"schemaVersion":"noemancer.animation-state-machine/0.2","assetId":"bad","initialState":"idle","parameters":[],"states":[{"id":"idle"}],"transitions":[{"id":"bad","from":"idle","to":"idle","conditions":[{"source":"parameter","parameter":"missing","comparison":"greater","threshold":0}]}]})");
    if(invalid||invalid.code!="animation.machine.condition-invalid") {
        std::cerr<<"Unknown transition parameter was not rejected\n";return 2;
    }
    auto malformed=nlohmann::json::parse(source);malformed["parameters"][0]["default"]="oops";
    const auto malformed_result=noemancer::AnimationStateMachineCodec::parse_json(malformed.dump());
    if(malformed_result||malformed_result.code!="animation.machine.type-invalid") {
        std::cerr<<"Malformed typed State Machine field did not return a stable parse error\n";return 7;
    }

    noemancer::World world;
    const auto scene=noemancer::SceneDocumentCodec::parse_json(
        R"({"schema":"noemancer.scene/0.1","sceneGuid":"scene.animation-machine","name":"Animation Machine","entities":[{"guid":"entity.actor","name":"Actor","parent":null,"components":{"Transform":{"position":[0,0,0]},"AnimationPlayer":{"clipAsset":"asset.animation.placeholder","looping":true,"playbackSpeed":1,"playing":true,"rootMotionMode":"ignore","stateMachineAsset":"animation.machine.test-production"}}}]})");
    if(!scene||!world.load_scene(*scene.document).success||!world.register_animation_state_machine(std::move(*parsed.document))) {
        std::cerr<<"Animation State Machine did not register into World\n";return 3;
    }
    auto view=world.entity_views().front();
    if(!view.animation_player||view.animation_player->active_state!="idle"||
       view.animation_player->clip_asset!="asset.animation.test-bob"||
       !view.animation_player->state_parameters.contains("grounded")) {
        std::cerr<<"State Machine initial state did not configure the player\n";return 4;
    }
    const auto parameter=nlohmann::json::parse(world.animation_state_parameter_set_json("entity.actor","speed",1.0F));
    world.tick(1.0F/60.0F);view=world.entity_views().front();
    if(!parameter.value("success",false)||!view.animation_player||view.animation_player->active_state!="move"||
       view.animation_player->next_clip_asset!="asset.animation.test-run"||
       world.animation_state_machine_json("entity.actor").find("move.enter")==std::string::npos) {
        std::cerr<<"Parameter-driven transition did not enter the authored Cross-fade\n";return 5;
    }
    const auto unknown=nlohmann::json::parse(world.animation_state_parameter_set_json("entity.actor","missing",1.0F));
    if(unknown.value("success",true)||unknown.value("code",std::string{})!="animation.invalid-parameter") {
        std::cerr<<"World accepted a parameter absent from the authored definition\n";return 6;
    }
    auto paused_source=nlohmann::json::parse(noemancer::SceneDocumentCodec::write_canonical_json(*scene.document));
    paused_source["entities"][0]["components"]["AnimationPlayer"]["playing"]=false;
    const auto paused_scene=noemancer::SceneDocumentCodec::parse_json(paused_source.dump());
    auto paused_machine=noemancer::AnimationStateMachineCodec::parse_json(source);noemancer::World paused_world;
    if(!paused_scene||!paused_machine||!paused_world.load_scene(*paused_scene.document).success||
       !paused_world.register_animation_state_machine(std::move(*paused_machine.document)))return 8;
    static_cast<void>(paused_world.animation_state_parameter_set_json("entity.actor","speed",1.0F));
    paused_world.tick(0.5F);const auto paused_view=paused_world.entity_views().front();
    if(!paused_view.animation_player||paused_view.animation_player->active_state!="idle"||
       paused_view.animation_player->state_elapsed_seconds!=0.0F||!paused_view.animation_player->next_clip_asset.empty()) {
        std::cerr<<"Paused AnimationPlayer advanced its state machine clock or transition\n";return 8;
    }
    std::cout<<"animation_state_machine_tests: ok\n";return 0;
}
