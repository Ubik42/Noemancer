#include "engine/gameplay_runtime.hpp"
#include "engine/gameplay_ability.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <iostream>
#include <vector>

int main() {
    noemancer::InputActionRuntime input;
    if (!input.set_source_value("keyboard.space", 1.0F)) return 1;
    input.evaluate();
    const auto input_json = nlohmann::json::parse(input.observe_json());
    const auto jump = input_json["actions"][1];
    if (jump["id"] != "gameplay.jump" || !jump["pressed"] || jump["value"] != 1.0F) {
        std::cerr << "Input binding did not produce a semantic action edge\n"; return 2;
    }
    const std::vector<noemancer::InputActionDefinition> remapped{{"gameplay.move.x",noemancer::InputActionKind::axis_1d,
        {{"keyboard.left",-1.0F,0.0F},{"keyboard.right",1.0F,0.0F},{"gamepad.left.x",1.0F,0.2F}}}};
    if(!input.configure(remapped)||!input.set_source_value("gamepad.left.x",0.1F))return 21;
    input.evaluate();
    if(nlohmann::json::parse(input.observe_json()).at("actions").at(0).at("value")!=0.0F)return 22;
    if(!input.set_source_value("gamepad.left.x",0.6F))return 23;input.evaluate();
    const auto remapped_json=nlohmann::json::parse(input.observe_json());
    if(remapped_json.at("schemaVersion")!="noemancer.input-actions/0.2"||remapped_json.at("actionCount")!=1U||
        std::abs(remapped_json.at("actions").at(0).at("value").get<float>()-0.5F)>0.0001F||
        remapped_json.at("actions").at(0).at("bindings").at(0).at("deadZone")!=0.2F)return 24;
    if(!input.reset_defaults())return 25;

    noemancer::GameplayRuntime gameplay;
    if(!input.set_source_value("keyboard.space",1.0F))return 26;input.evaluate();gameplay.update_from_input(input);
    const auto gameplay_json = nlohmann::json::parse(gameplay.observe_json());
    if (gameplay_json["events"].size() != 1 || gameplay_json["events"][0]["type"] != "input.action.pressed") return 3;

    noemancer::AudioMixerRuntime audio;
    if (!audio.register_asset({"asset.audio.test", "sha256:test", noemancer::AudioAssetStorage::resident}) ||
        !audio.register_asset({"asset.audio.music", "sha256:music", noemancer::AudioAssetStorage::stream})) return 31;
    if (!audio.set_bus("audio.sfx", 0.75F, false)) return 4;
    const auto voice = audio.play("asset.audio.test", "audio.sfx", 0.8F, 1.0F, false);
    if (voice == 0 || !audio.stop(voice)) return 5;
    const auto audio_json = nlohmann::json::parse(audio.observe_json());
    if (audio_json["buses"].size() != 4 || audio_json["voices"][0]["playing"] != false ||
        audio_json["assets"].size() != 2 || audio_json["assets"][1]["storage"] != "resident") return 6;
    const auto asset_snapshot=audio.render_snapshot();
    if(asset_snapshot.assets.size()!=2U||asset_snapshot.assets.front().asset_id!="asset.audio.music"||
       asset_snapshot.assets.front().storage!=noemancer::AudioAssetStorage::stream) return 61;
    if(audio.play("asset.audio.pcm-test","audio.sfx",1.0F,1.0F,false)==0) return 7;
    std::vector<float> pcm(480U*2U);
    audio.mix_stereo(pcm,48000);
    if(std::ranges::all_of(pcm,[](const float sample){return sample==0.0F;})||
        nlohmann::json::parse(audio.observe_json()).at("mixedFrames")!=480) return 8;
    noemancer::AudioMixerRuntime authored;
    const auto synchronized_voice=authored.play("asset.audio.sync","audio.sfx",1.0F,1.0F,false);
    noemancer::AudioMixerRuntime render_transport;
    render_transport.synchronize_from(authored.render_snapshot());
    render_transport.mix_stereo(pcm,48000);
    const auto cursor_before=nlohmann::json::parse(render_transport.observe_json()).at("voices").at(0).at("clipCursor").get<double>();
    if(cursor_before!=0.0) return 81; // procedural voices advance phase, not a clip cursor
    if(!authored.set_bus("audio.sfx",0.5F,false)) return 82;
    render_transport.synchronize_from(authored.render_snapshot());
    const auto synchronized=nlohmann::json::parse(render_transport.observe_json()).at("voices").at(0);
    if(synchronized.at("id")!=synchronized_voice||synchronized.at("renderedFrames")!=480U) return 83;
    noemancer::AudioMixerRuntime authored_clip;
    if(!authored_clip.register_clip({"asset.audio.shared",48000,1,std::vector<float>(2048U,0.25F)})) return 84;
    if(authored_clip.play("asset.audio.shared","audio.sfx",1.0F,1.0F,false)==0) return 85;
    noemancer::AudioMixerRuntime clip_transport;
    const auto shared_snapshot=authored_clip.render_snapshot();
    if(shared_snapshot.clips.size()!=1U||shared_snapshot.clips.at(0)->samples.size()!=2048U) return 851;
    clip_transport.synchronize_from(shared_snapshot);clip_transport.mix_stereo(pcm,48000);
    if(!authored_clip.set_bus("audio.sfx",0.4F,false)) return 86;
    clip_transport.synchronize_from(authored_clip.render_snapshot());
    const auto clip_voice=nlohmann::json::parse(clip_transport.observe_json()).at("voices").at(0);
    if(clip_voice.at("clipCursor")!=480.0||clip_voice.at("renderedFrames")!=480U) return 87;
    noemancer::GameplayAbilityRuntime abilities;
    if(!abilities.grant("entity.player","ability.combat.impact")) return 9;
    const auto activated=nlohmann::json::parse(abilities.activate_json("entity.player","ability.combat.impact","entity.target",gameplay));
    const auto blocked=nlohmann::json::parse(abilities.activate_json("entity.player","ability.combat.impact","entity.target",gameplay));
    const auto ability_state=nlohmann::json::parse(abilities.observe_json("entity.player"));
    const auto target_state=nlohmann::json::parse(abilities.observe_json("entity.target"));
    const auto gameplay_events=nlohmann::json::parse(gameplay.observe_json()).at("events");
    if(!activated.at("success")||activated.at("eventType")!="combat.hit"||blocked.at("code")!="gameplay.ability.on-cooldown"||
       ability_state.at("actors").at(0).at("attributes").at("stamina")!=85.0F||
       target_state.at("actors").at(0).at("attributes").at("health")!=80.0F||
       target_state.at("actors").at(0).at("resistances").at("physical")!=0.20F||
       target_state.at("activeEffects").size()!=1||
       std::ranges::none_of(gameplay_events,[](const auto& event){return event.at("type")=="combat.damage.applied";})) return 10;
    abilities.tick(0.35F,gameplay);
    if(!nlohmann::json::parse(abilities.activate_json("entity.player","ability.combat.impact","entity.target",gameplay)).at("success")) return 11;
    const auto fire1=nlohmann::json::parse(abilities.apply_effect_json("entity.player","entity.fire-target","effect.damage.fire-dot",gameplay));
    const auto fire2=nlohmann::json::parse(abilities.apply_effect_json("entity.player","entity.fire-target","effect.damage.fire-dot",gameplay));
    const auto fire3=nlohmann::json::parse(abilities.apply_effect_json("entity.player","entity.fire-target","effect.damage.fire-dot",gameplay));
    const auto fire4=nlohmann::json::parse(abilities.apply_effect_json("entity.player","entity.fire-target","effect.damage.fire-dot",gameplay));
    if(fire1.at("effectInstanceId")!=fire2.at("effectInstanceId")||fire2.at("effectInstanceId")!=fire3.at("effectInstanceId")||
       fire3.at("effectInstanceId")!=fire4.at("effectInstanceId")||fire3.at("stackCount")!=3||fire4.at("stackCount")!=3||
       fire3.at("maximumStacks")!=3||fire3.at("damageType")!="fire") return 12;
    abilities.tick(0.5F,gameplay);
    const auto fire_state=nlohmann::json::parse(abilities.observe_json("entity.fire-target"));
    if(fire_state.at("actors").at(0).at("attributes").at("health")!=91.0F||
       fire_state.at("activeEffects").at(0).at("stackCount")!=3||
       fire_state.at("activeEffects").at(0).at("applications")!=4||
       fire_state.at("activeEffects").at(0).at("ticksExecuted")!=1||
       fire_state.at("activeEffects").at(0).at("secondsUntilNextTick")!=0.5F) return 13;
    const auto periodic_events=nlohmann::json::parse(gameplay.observe_json()).at("events");
    if(std::ranges::none_of(periodic_events,[](const auto& event){return event.at("type")=="gameplay.effect.periodic";})) return 14;
    const auto ability_revision_before_forget=abilities.revision();
    abilities.forget_entity("entity.fire-target");
    const auto forgotten=nlohmann::json::parse(abilities.observe_json("entity.fire-target"));
    if(abilities.revision()!=ability_revision_before_forget+1||!forgotten.at("actors").empty()||
       !forgotten.at("activeEffects").empty())return 15;
    return 0;
}
