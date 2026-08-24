#include "engine/sprite_asset.hpp"
#include "engine/scene_document.hpp"
#include "engine/world.hpp"

#include <array>
#include <iostream>
#include <nlohmann/json.hpp>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>

int main() {
    constexpr auto source=R"({
      "schema":"noemancer.sprite-asset/0.1",
      "assetId":"sprite.courier",
      "textureAsset":"texture.courier.atlas",
      "textureSize":[64,32],
      "pixelsPerUnit":16,
      "sampling":"nearest",
      "alphaMode":"cutout",
      "frames":[
        {"id":"idle.0","rect":[0,0,16,24],"trimOffset":[2,4],"sourceSize":[20,28],"pivot":[0.5,0.1],"collisionProfile":"courier.body"},
        {"id":"idle.1","rect":[16,0,16,24],"trimOffset":[2,4],"sourceSize":[20,28],"pivot":[0.5,0.1],"collisionProfile":"courier.body"}
      ],
      "clips":[{"id":"idle","looping":true,"frames":[{"frame":"idle.0","durationMs":120,"event":"foot.left"},{"frame":"idle.1","durationMs":130,"event":"blink"}]},{"id":"run","looping":true,"frames":[{"frame":"idle.1","durationMs":80,"event":"step"}]}],
      "provenance":{"sourceUri":"assets/art/source/courier.png","sourceSha256":"abc123","generator":"artist+agent","license":"project-original"}
    })";
    const auto parsed=noemancer::SpriteAssetCodec::parse_json(source);
    if(!parsed) { std::cerr<<"valid sprite asset rejected\n";return 1; }
    if(parsed.document->frames.size()!=2||parsed.document->clips.size()!=2||parsed.document->clips[0].frames[0].duration_ms!=120||
       parsed.document->material) {
        std::cerr<<"sprite asset values lost\n";return 2;
    }
    const auto canonical=noemancer::SpriteAssetCodec::write_canonical_json(*parsed.document);
    const auto round_trip=noemancer::SpriteAssetCodec::parse_json(canonical);
    if(!round_trip||round_trip.document->material||noemancer::SpriteAssetCodec::write_canonical_json(*round_trip.document)!=canonical) {
        std::cerr<<"sprite canonical round trip drifted\n";return 3;
    }
    auto material_source=std::string(source);
    constexpr std::string_view legacy_schema="noemancer.sprite-asset/0.1";
    material_source.replace(material_source.find(legacy_schema),legacy_schema.size(),"noemancer.sprite-asset/0.2");
    material_source.replace(material_source.find("\"frames\""),8,
        R"("material":{"normalTextureAsset":"texture.courier.normal","emissiveMaskTextureAsset":"texture.courier.emissive","depthTextureAsset":"texture.courier.depth","normalStrength":1.5,"emissiveColor":[0.2,0.8,1.0],"emissiveIntensity":3.0,"depthBias":0.001},"frames")");
    const auto material_parsed=noemancer::SpriteAssetCodec::parse_json(material_source);
    if(!material_parsed||!material_parsed.document->material||material_parsed.document->material->normal_strength!=1.5F||
       material_parsed.document->material->shading_model!="lit"||material_parsed.document->material->metallic!=0.0F||
       material_parsed.document->material->roughness!=0.8F||!material_parsed.document->material->receives_shadows||
       !material_parsed.document->material->casts_shadows||
       noemancer::SpriteAssetCodec::write_canonical_json(*material_parsed.document).find("texture.courier.depth")==std::string::npos) {
        std::cerr<<"sprite material v0.2 contract did not round trip\n";return 10;
    }
    const auto material_canonical_json=nlohmann::json::parse(
        noemancer::SpriteAssetCodec::write_canonical_json(*material_parsed.document));
    if(material_canonical_json.at("material").at("shadingModel")!="lit"||
       material_canonical_json.at("material").at("metallic")!=0.0F||
       material_canonical_json.at("material").at("roughness")!=0.8F||
       material_canonical_json.at("material").at("receivesShadows")!=true||
       material_canonical_json.at("material").at("castsShadows")!=true) {
        std::cerr<<"sprite material defaults were not canonicalized\n";return 15;
    }
    auto explicit_material_source=material_source;
    constexpr std::string_view material_tail="\"depthBias\":0.001";
    constexpr std::string_view explicit_material_tail=
        "\"depthBias\":0.001,\"shadingModel\":\"unlit\",\"metallic\":0.35,\"roughness\":0.045,\"receivesShadows\":false,\"castsShadows\":false";
    explicit_material_source.replace(explicit_material_source.find(material_tail),material_tail.size(),explicit_material_tail);
    const auto explicit_material=noemancer::SpriteAssetCodec::parse_json(explicit_material_source);
    if(!explicit_material||!explicit_material.document->material||explicit_material.document->material->shading_model!="unlit"||
       explicit_material.document->material->metallic!=0.35F||explicit_material.document->material->roughness!=0.045F||
       explicit_material.document->material->receives_shadows||explicit_material.document->material->casts_shadows) {
        std::cerr<<"sprite material authored values were not parsed\n";return 16;
    }
    const auto explicit_material_json=nlohmann::json::parse(
        noemancer::SpriteAssetCodec::write_canonical_json(*explicit_material.document));
    if(explicit_material_json.at("material").at("shadingModel")!="unlit"||
       explicit_material_json.at("material").at("metallic")!=0.35F||
       explicit_material_json.at("material").at("roughness")!=0.045F||
       explicit_material_json.at("material").at("receivesShadows")!=false||
       explicit_material_json.at("material").at("castsShadows")!=false) {
        std::cerr<<"sprite material authored values were not canonicalized\n";return 17;
    }
    auto duplicate_material_dependencies=*explicit_material.document;
    duplicate_material_dependencies.material->normal_texture_asset=duplicate_material_dependencies.texture_asset;
    duplicate_material_dependencies.material->emissive_mask_texture_asset=duplicate_material_dependencies.texture_asset;
    duplicate_material_dependencies.material->depth_texture_asset=duplicate_material_dependencies.texture_asset;
    const auto duplicate_dependencies=noemancer::SpriteAssetCodec::asset_dependencies(duplicate_material_dependencies);
    const auto duplicate_dependencies_repeat=noemancer::SpriteAssetCodec::asset_dependencies(duplicate_material_dependencies);
    if(duplicate_dependencies!=std::vector<std::string>{"texture.courier.atlas"}||
       duplicate_dependencies_repeat!=duplicate_dependencies) {
        std::cerr<<"sprite material dependency projection was not stable and deduplicated\n";return 22;
    }
    const std::array<std::pair<std::string_view,std::string_view>,5> invalid_material_values{{
        {"\"shadingModel\":\"unlit\"","\"shadingModel\":\"toon\""},
        {"\"metallic\":0.35","\"metallic\":1.01"},
        {"\"roughness\":0.045","\"roughness\":0.044"},
        {"\"receivesShadows\":false","\"receivesShadows\":1"},
        {"\"castsShadows\":false","\"castsShadows\":\"yes\""}
    }};
    for(const auto [needle,replacement]:invalid_material_values) {
        auto invalid=explicit_material_source;
        invalid.replace(invalid.find(needle),needle.size(),replacement);
        if(noemancer::SpriteAssetCodec::parse_json(invalid)) {
            std::cerr<<"invalid sprite material value was accepted: "<<replacement<<"\n";return 18;
        }
    }
    auto unknown_material_field=material_source;
    unknown_material_field.replace(unknown_material_field.find("\"material\":{")+12,0,"\"surprise\":true,");
    if(noemancer::SpriteAssetCodec::parse_json(unknown_material_field)) {
        std::cerr<<"unknown sprite material field was accepted\n";return 19;
    }
    auto invalid_legacy_material=material_source;
    constexpr std::string_view material_schema="noemancer.sprite-asset/0.2";
    invalid_legacy_material.replace(invalid_legacy_material.find(material_schema),material_schema.size(),legacy_schema);
    if(noemancer::SpriteAssetCodec::parse_json(invalid_legacy_material)) {
        std::cerr<<"legacy sprite schema accepted v0.2 material channels\n";return 11;
    }
    auto out_of_bounds=std::string(source);out_of_bounds.replace(out_of_bounds.find("[16,0,16,24]"),13,"[60,0,16,24]");
    const auto invalid_rect=noemancer::SpriteAssetCodec::parse_json(out_of_bounds);
    if(invalid_rect||invalid_rect.errors.empty()) { std::cerr<<"out-of-bounds frame accepted\n";return 4; }
    auto unknown_frame=std::string(source);unknown_frame.replace(unknown_frame.find("\"idle.1\",\"durationMs\""),22,"\"missing\",\"durationMs\"");
    const auto invalid_clip=noemancer::SpriteAssetCodec::parse_json(unknown_frame);
    if(invalid_clip||invalid_clip.errors.empty()) { std::cerr<<"unknown clip frame accepted\n";return 5; }
    auto unknown_field=std::string(source);unknown_field.replace(unknown_field.find("\"sampling\""),10,"\"surprise\":1,\"sampling\"");
    const auto invalid_field=noemancer::SpriteAssetCodec::parse_json(unknown_field);
    if(invalid_field||invalid_field.errors.empty()) { std::cerr<<"unknown field accepted\n";return 6; }
    noemancer::SpriteAssetLibrary library;
    if(!library.register_asset(*parsed.document)) { std::cerr<<"valid sprite could not enter runtime library\n";return 7; }
    noemancer::SpritePlaybackState playback{.asset_id="sprite.courier",.clip_id="idle"};
    const auto before=library.advance(playback,0.10);
    const auto entered_second=library.advance(playback,0.03);
    const auto looped=library.advance(playback,0.13);
    const auto observation=library.observe_json(playback);
    const auto resolved=library.resolve(playback);
    if(!before.success||before.frame_changed||!entered_second.success||entered_second.frame_id!="idle.1"||
       entered_second.event!="blink"||!looped.success||!looped.looped||looped.frame_id!="idle.0"||
       looped.event!="foot.left"||playback.completed_loops!=1||observation.find("courier.body")==std::string::npos||
       !resolved||resolved->texture_asset!="texture.courier.atlas"||resolved->frame.id!="idle.0"||
       resolved->texture_width!=64||resolved->pixels_per_unit!=16.0F||resolved->material) {
        std::cerr<<"deterministic sprite playback or semantic observation failed\n";return 8;
    }
    noemancer::SpriteAssetLibrary material_library;
    if(!material_library.register_asset(*explicit_material.document))return 20;
    const noemancer::SpritePlaybackState material_playback{.asset_id="sprite.courier",.clip_id="idle"};
    const auto material_resolved=material_library.resolve(material_playback);
    const auto material_observation=nlohmann::json::parse(material_library.observe_json(material_playback));
    if(!material_resolved||!material_resolved->material||material_resolved->material->shading_model!="unlit"||
       material_resolved->material->metallic!=0.35F||material_resolved->material->roughness!=0.045F||
       material_resolved->material->receives_shadows||material_resolved->material->casts_shadows||
       material_observation.at("material").at("shadingModel")!="unlit"||
       material_observation.at("material").at("metallic")!=0.35F||
       material_observation.at("material").at("roughness")!=0.045F||
       material_observation.at("material").at("receivesShadows")!=false||
       material_observation.at("material").at("castsShadows")!=false) {
        std::cerr<<"resolved sprite playback did not carry material values\n";return 21;
    }
    if(library.advance(playback,-0.1).code!="sprite.invalid-delta"||
       library.advance(playback,10.1).code!="sprite.invalid-delta") {
        std::cerr<<"sprite playback accepted an unbounded delta\n";return 9;
    }
    noemancer::World world;auto scene=noemancer::make_bootstrap_scene_document();
    auto sprite_entity=std::ranges::find(scene.entities,std::string("entity.demo-sphere"),&noemancer::SceneEntityDocument::guid);
    if(sprite_entity==scene.entities.end())return 12;
    sprite_entity->sprite_renderer=noemancer::SceneSpriteRenderer{.sprite_asset="sprite.courier",.clip="idle"};
    if(!world.register_sprite_asset(*parsed.document)||!world.load_scene(scene).success)return 13;
    const auto persisted_before=world.canonical_scene_json();const auto revision_before=world.revision();
    const auto state_change=nlohmann::json::parse(world.set_sprite_playback_json(
        "entity.demo-sphere","run",true,1.5F,true,false));
    const auto sprite_world=nlohmann::json::parse(world.sprite_observation_json("entity.demo-sphere"));
    const auto sprite_delta=world.delta_json(revision_before);
    const auto no_op=nlohmann::json::parse(world.set_sprite_playback_json(
        "entity.demo-sphere","run",true,1.5F,true,false));
    const auto rejected=nlohmann::json::parse(world.set_sprite_playback_json(
        "entity.demo-sphere","missing",true,1.0F,false,false));
    if(!state_change.at("success")||!state_change.at("changed")||state_change.at("revisionAfter")!=revision_before+1||
       sprite_world.at("items").size()!=1U||sprite_world.at("items").at(0).at("flipX")!=true||
       sprite_world.at("items").at(0).at("playbackSpeed")!=1.5F||
       sprite_world.at("items").at(0).at("playback").at("clipId")!="run"||
       sprite_delta.find(R"("flipX":true)")==std::string::npos||sprite_delta.find(R"("playbackSpeed":1.5)")==std::string::npos||
       !no_op.at("success")||no_op.at("changed")||no_op.at("revisionAfter")!=state_change.at("revisionAfter")||
       rejected.at("success")||rejected.at("code")!="sprite.clip-not-found"||
       world.canonical_scene_json()!=persisted_before) {
        std::cerr<<"runtime Sprite playback state did not remain validated, idempotent and non-persistent\n";return 14;
    }
    std::cout<<"Sprite asset contract passed\n";return 0;
}
