#include "engine/scene_document.hpp"
#include "engine/render_reference_scene.hpp"
#include "engine/process_diagnostics.hpp"
#include "engine/world.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>

int main() {
    noemancer::configure_process_diagnostics("test.scene-document");
    const std::string scene_path =
        std::string(NOEMANCER_SOURCE_DIR) + "/assets/scenes/bootstrap.scene.json";
    std::ifstream stream(scene_path, std::ios::binary);
    const std::string source{
        std::istreambuf_iterator<char>(stream),
        std::istreambuf_iterator<char>()};
    if (source.empty()) {
        std::cerr << "Bootstrap scene fixture could not be read\n";
        return 1;
    }

    const auto parsed = noemancer::SceneDocumentCodec::parse_json(
        source,
        "asset://scenes/bootstrap.scene.json");
    if (!parsed || parsed.document->entities.size() != 10 ||
        !parsed.document->entities[1].camera || !parsed.document->entities[2].mesh_renderer) {
        std::cerr << "Canonical bootstrap scene did not parse\n";
        return 2;
    }
    // Git may materialize this fixture with CRLF on Windows while the codec
    // deliberately emits platform-neutral LF.  Line endings are transport
    // details, not part of the canonical scene contract.
    auto normalized_source = source;
    normalized_source.erase(
        std::remove(normalized_source.begin(), normalized_source.end(), '\r'),
        normalized_source.end());
    if (noemancer::SceneDocumentCodec::write_canonical_json(*parsed.document) != normalized_source) {
        std::cerr << "Bootstrap scene is not in canonical form\n";
        return 3;
    }

    noemancer::World world;
    const auto first_load = world.load_scene(*parsed.document);
    if (!first_load.success || first_load.entity_count != 10 || world.revision() != 1) {
        std::cerr << "Scene document did not instantiate into the ECS world\n";
        return 4;
    }
    const auto first_snapshot = world.snapshot_json();
    if (first_snapshot.find(R"("sceneGuid":"scene.bootstrap")") == std::string::npos ||
        first_snapshot.find(R"("pointer":"/entities/1")") == std::string::npos ||
        first_snapshot.find(R"("parentGuid":"entity.bootstrap-root")") == std::string::npos) {
        std::cerr << "World snapshot lost scene identity, hierarchy, or source anchors\n";
        return 5;
    }

    auto reordered = *parsed.document;
    std::ranges::reverse(reordered.entities);
    const auto second_load = world.load_scene(reordered);
    if (!second_load.success || world.entity_count() != 10 || world.revision() != 2 ||
        world.snapshot_json().find("entity.demo-cube") == std::string::npos) {
        std::cerr << "Reload did not preserve stable semantic identities\n";
        return 6;
    }

    auto invalid = reordered;
    invalid.entities[0].parent_guid = "entity.missing";
    const auto revision_before_invalid_load = world.revision();
    const auto rejected = world.load_scene(invalid);
    if (rejected.success || rejected.errors.empty() ||
        rejected.errors.front().code != "scene.missing-parent" ||
        world.revision() != revision_before_invalid_load || world.entity_count() != 10) {
        std::cerr << "Invalid scene was not rejected atomically\n";
        return 7;
    }

    const auto unknown_component = noemancer::SceneDocumentCodec::parse_json(
        R"({"schema":"noemancer.scene/0.1","sceneGuid":"scene.bad","name":"Bad","entities":[{"guid":"entity.bad","name":"Bad","parent":null,"components":{"Mystery":{}}}]})");
    if (unknown_component || unknown_component.errors.empty() ||
        unknown_component.errors.front().code != "scene.unknown-component") {
        std::cerr << "Unknown persisted component did not produce a structured error\n";
        return 8;
    }

    const auto scaled_contract = noemancer::SceneDocumentCodec::parse_json(
        R"({"schema":"noemancer.scene/0.1","sceneGuid":"scene.scaled","name":"Scaled","entities":[{"guid":"entity.scaled","name":"Scaled","parent":null,"components":{"MeshRenderer":{"castsShadows":true,"meshAsset":"asset.primitive.cube","receivesShadows":true,"visible":true},"Transform":{"position":[1.0,2.0,3.0],"rotationEulerDegrees":[15.0,30.0,45.0],"scale":[4.0,0.5,2.0]}}}]})");
    if (!scaled_contract || scaled_contract.document->entities[0].transform->scale.x != 4.0 ||
        scaled_contract.document->entities[0].transform->rotation_euler_degrees.y != 30.0 ||
        noemancer::SceneDocumentCodec::write_canonical_json(*scaled_contract.document).find("rotationEulerDegrees") == std::string::npos) {
        std::cerr << "Transform scale/rotation scene contract did not round-trip\n";
        return 19;
    }
    noemancer::World scaled_world;
    if (!scaled_world.load_scene(*scaled_contract.document).success ||
        scaled_world.snapshot_json().find(R"("scale":{"unit":"ratio","x":4.0)") == std::string::npos ||
        scaled_world.snapshot_json().find("rotationQuaternion") == std::string::npos ||
        scaled_world.inspector_document_json("entity.scaled").find("engine.entity.transform.rotationEulerDegrees") == std::string::npos) {
        std::cerr << "Transform scale/rotation did not reach World observation and Inspector\n";
        return 20;
    }
    const auto orthographic_contract=noemancer::SceneDocumentCodec::parse_json(
        R"({"schema":"noemancer.scene/0.1","sceneGuid":"scene.orthographic","name":"Orthographic","entities":[{"guid":"entity.camera","name":"Camera","parent":null,"components":{"Camera":{"farClip":100.0,"nearClip":0.1,"orthographicHeight":12.0,"primary":true,"projection":"orthographic","target":[0.0,0.0,0.0],"verticalFovDegrees":45.0},"Transform":{"position":[0.0,0.0,10.0]}}}]})");
    if(!orthographic_contract||orthographic_contract.document->entities[0].camera->projection!="orthographic"||
        orthographic_contract.document->entities[0].camera->orthographic_height!=12.0||
        noemancer::SceneDocumentCodec::write_canonical_json(*orthographic_contract.document).find("orthographicHeight")==std::string::npos) {
        std::cerr<<"Orthographic camera scene contract did not round-trip\n"; return 21;
    }
    noemancer::World orthographic_world;
    if(!orthographic_world.load_scene(*orthographic_contract.document).success||
        orthographic_world.render_observation_json().find(R"("projection":"orthographic")")==std::string::npos) {
        std::cerr<<"Orthographic camera did not reach World render observation\n"; return 22;
    }
    const auto local_light_contract=noemancer::SceneDocumentCodec::parse_json(
        R"({"schema":"noemancer.scene/0.1","sceneGuid":"scene.local-light","name":"Local Light","entities":[{"guid":"entity.light","name":"Spot","parent":null,"components":{"LocalLight":{"castsShadows":false,"color":[1.0,0.5,0.2],"direction":[0.0,-1.0,0.0],"innerConeDegrees":20.0,"kind":"spot","luminousPowerLumens":900.0,"outerConeDegrees":35.0,"rangeMeters":12.0,"sourceRadiusMeters":0.1},"Transform":{"position":[1.0,3.0,2.0]}}}]})");
    if(!local_light_contract||!local_light_contract.document->entities[0].local_light||
       local_light_contract.document->entities[0].local_light->kind!="spot"||
       noemancer::SceneDocumentCodec::write_canonical_json(*local_light_contract.document).find("luminousPowerLumens")==std::string::npos) {
        std::cerr<<"LocalLight scene contract did not round-trip\n";return 36;
    }
    noemancer::World local_light_world;
    if(!local_light_world.load_scene(*local_light_contract.document).success||
       local_light_world.snapshot_json().find("schema://noemancer/component/local-light/0.1")==std::string::npos||
       local_light_world.inspector_document_json("entity.light").find("engine.entity.localLight.luminousPowerLumens")==std::string::npos) {
        std::cerr<<"LocalLight did not reach the stable World observation\n";return 37;
    }
    const auto light_plan=local_light_world.plan_property_update("entity.light","engine.entity.localLight.luminousPowerLumens","1200.0",
        local_light_world.revision(),"test.local-light");
    const auto light_receipt=local_light_world.apply_property_plan(light_plan,false);
    if(!light_receipt.success||local_light_world.canonical_scene_json().find(R"("luminousPowerLumens": 1200.0)")==std::string::npos) {
        std::cerr<<"LocalLight declarative property edit did not reach the canonical Scene\n";return 39;
    }
    auto invalid_local_light=*local_light_contract.document;
    invalid_local_light.entities[0].local_light->inner_cone_degrees=50.0;
    if(std::ranges::none_of(noemancer::SceneDocumentCodec::validate(invalid_local_light),[](const noemancer::SceneDocumentError& error){
        return error.code=="scene.invalid-local-light";})) {
        std::cerr<<"Invalid LocalLight cone was not rejected\n";return 38;
    }
    const auto managed_contract=noemancer::SceneDocumentCodec::parse_json(
        R"({"schema":"noemancer.scene/0.1","sceneGuid":"scene.managed","name":"Managed","entities":[{"guid":"entity.scripted","name":"Scripted","parent":null,"components":{"ManagedScript":{"assemblyAsset":"project.script","enabled":true,"instanceId":"script.scene.mover","properties":{"speed":3.5},"typeName":"Game.Mover"},"Transform":{"position":[0.0,0.0,0.0]}}}]})");
    if(!managed_contract||!managed_contract.document->entities[0].managed_script||
       managed_contract.document->entities[0].managed_script->type_name!="Game.Mover"||
       noemancer::SceneDocumentCodec::write_canonical_json(*managed_contract.document).find("script.scene.mover")==std::string::npos) {
        std::cerr<<"ManagedScript scene contract did not round-trip\n";return 23;
    }
    const auto trigger_contract=noemancer::SceneDocumentCodec::parse_json(
        R"({"schema":"noemancer.scene/0.1","sceneGuid":"scene.trigger","name":"Trigger","entities":[{"guid":"entity.trigger","name":"Trigger","parent":null,"components":{"BoxCollider":{"friction":0.5,"halfExtents":[1.0,1.0,1.0],"isTrigger":true,"restitution":0.0},"RigidBody":{"gravityFactor":0.0,"linearDamping":0.0,"mass":1.0,"motionType":"static"},"Transform":{"position":[0.0,0.0,0.0]}}}]})");
    if(!trigger_contract||!trigger_contract.document->entities[0].box_collider||
       !trigger_contract.document->entities[0].box_collider->is_trigger||
       noemancer::SceneDocumentCodec::write_canonical_json(*trigger_contract.document).find("isTrigger")==std::string::npos){
        std::cerr<<"Trigger collider scene contract did not round-trip\n";return 26;
    }
    noemancer::World trigger_world;
    if(!trigger_world.load_scene(*trigger_contract.document).success||
       trigger_world.inspector_document_json("entity.trigger").find("engine.entity.collider.isTrigger")==std::string::npos||
       trigger_world.physics_observation_json().find(R"("isTrigger":true)")==std::string::npos){
        std::cerr<<"Trigger collider did not reach Inspector and physics observation\n";return 27;
    }
    noemancer::World managed_world;
    if(!managed_world.load_scene(*managed_contract.document).success||
       managed_world.inspector_document_json("entity.scripted").find("C# Script")==std::string::npos||
       managed_world.scripting_observation_json().find(R"("sceneOwned":true)")==std::string::npos) {
        std::cerr<<"ManagedScript did not reach declarative Inspector and runtime observation\n";return 25;
    }
    auto duplicate_script=*managed_contract.document;
    duplicate_script.entities.push_back(duplicate_script.entities.front());
    duplicate_script.entities.back().guid="entity.scripted-copy";
    if(std::ranges::none_of(noemancer::SceneDocumentCodec::validate(duplicate_script),[](const noemancer::SceneDocumentError& error){
        return error.code=="scene.duplicate-script-instance";
    })) {std::cerr<<"Duplicate managed instance ID was not rejected\n";return 24;}

    const auto animation_contract = noemancer::SceneDocumentCodec::parse_json(
        R"({"schema":"noemancer.scene/0.1","sceneGuid":"scene.animation","name":"Animation","entities":[{"guid":"entity.animated","name":"Animated","parent":null,"components":{"AnimationPlayer":{"clipAsset":"asset.clip.walk","looping":true,"nextClipAsset":"asset.clip.run","playbackSpeed":1.0,"playing":true,"rootMotionMode":"apply","transitionDurationSeconds":0.25}}}]})");
    if (!animation_contract || !animation_contract.document->entities[0].animation_player ||
        animation_contract.document->entities[0].animation_player->next_clip_asset != "asset.clip.run" ||
        animation_contract.document->entities[0].animation_player->root_motion_mode != "apply" ||
        noemancer::SceneDocumentCodec::write_canonical_json(*animation_contract.document).find("transitionDurationSeconds") == std::string::npos) {
        std::cerr << "Animation transition and root-motion scene contract did not round-trip\n";
        return 11;
    }
    auto invalid_animation = *animation_contract.document;
    invalid_animation.entities[0].animation_player->transition_duration_seconds = 0.0;
    if (std::ranges::none_of(noemancer::SceneDocumentCodec::validate(invalid_animation), [](const noemancer::SceneDocumentError& error) {
            return error.code == "scene.invalid-animation-player";
        })) {
        std::cerr << "Invalid animation transition was not rejected\n";
        return 12;
    }
    const auto sprite_contract=noemancer::SceneDocumentCodec::parse_json(
        R"({"schema":"noemancer.scene/0.1","sceneGuid":"scene.sprite","name":"Sprite","entities":[{"guid":"entity.sprite","name":"Courier","parent":null,"components":{"SpriteRenderer":{"clip":"idle","flipX":true,"flipY":false,"playbackSpeed":1.25,"playing":true,"sortingLayer":"characters","sortingOrder":7,"spriteAsset":"sprite.courier","visible":true},"Transform":{"position":[0.0,0.0,0.0]}}}]})");
    if(!sprite_contract||!sprite_contract.document->entities[0].sprite_renderer||
       sprite_contract.document->entities[0].sprite_renderer->sorting_order!=7||
       !sprite_contract.document->entities[0].sprite_renderer->flip_x||
       noemancer::SceneDocumentCodec::write_canonical_json(*sprite_contract.document).find("sprite.courier")==std::string::npos) {
        std::cerr<<"SpriteRenderer scene contract did not round-trip\n";return 28;
    }
    auto invalid_sprite=*sprite_contract.document;invalid_sprite.entities[0].sprite_renderer->playback_speed=-1.0;
    if(std::ranges::none_of(noemancer::SceneDocumentCodec::validate(invalid_sprite),[](const noemancer::SceneDocumentError& error){
        return error.code=="scene.invalid-sprite-renderer";})) {
        std::cerr<<"Invalid SpriteRenderer was not rejected\n";return 29;
    }
    const auto tilemap_contract=noemancer::SceneDocumentCodec::parse_json(
        R"({"schema":"noemancer.scene/0.1","sceneGuid":"scene.tilemap","name":"Tilemap","entities":[{"guid":"entity.tilemap","name":"Terrain","parent":null,"components":{"Transform":{"position":[0.0,0.0,0.0]},"TilemapRenderer":{"tilemapAsset":"tilemap.level","visible":true,"collisionEnabled":true}}}]})");
    if(!tilemap_contract||!tilemap_contract.document->entities[0].tilemap_renderer||
       !tilemap_contract.document->entities[0].tilemap_renderer->collision_enabled||
       noemancer::SceneDocumentCodec::write_canonical_json(*tilemap_contract.document).find("tilemap.level")==std::string::npos) {
        std::cerr<<"TilemapRenderer scene contract did not round-trip\n";return 30;
    }
    auto invalid_tilemap=*tilemap_contract.document;invalid_tilemap.entities[0].transform.reset();
    if(std::ranges::none_of(noemancer::SceneDocumentCodec::validate(invalid_tilemap),[](const noemancer::SceneDocumentError& error){
        return error.code=="scene.invalid-tilemap-renderer";})) return 31;
    noemancer::World tilemap_world;
    const auto tile_sprite=noemancer::SpriteAssetCodec::parse_json(R"({"schema":"noemancer.sprite-asset/0.1",
      "assetId":"sprite.tiles","textureAsset":"texture.tiles","textureSize":[16,16],"pixelsPerUnit":16,
      "sampling":"nearest","alphaMode":"cutout","frames":[{"id":"ground.0","rect":[0,0,16,16],
      "trimOffset":[0,0],"sourceSize":[16,16],"pivot":[0.5,0.0],"collisionProfile":"solid"}],
      "clips":[{"id":"idle","looping":true,"frames":[{"frame":"ground.0","durationMs":100,"event":""}]}],
      "provenance":{"sourceUri":"tiles.png","sourceSha256":"fixture","generator":"test","license":"CC0"}})");
    const auto tile_palette=noemancer::TilemapAssetCodec::parse_palette_json(R"({"schema":"noemancer.tile-palette/0.1",
      "assetId":"palette.level","spriteAsset":"sprite.tiles","tiles":[{"id":"ground","frame":"ground.0","collision":"solid","tags":[]}]})");
    const auto tile_asset=noemancer::TilemapAssetCodec::parse_tilemap_json(R"({"schema":"noemancer.tilemap/0.1",
      "assetId":"tilemap.level","paletteAsset":"palette.level","cellSize":[0.5,0.75],"chunkSize":[8,8],
      "layers":[{"id":"ground","sortingLayer":"terrain","sortingOrder":-2,"collisionEnabled":true,
      "chunks":[{"position":[1,-1],"cells":[[1,7,"ground",true,false]]}]}]})");
    if(!tile_sprite||!tile_palette||!tile_asset||!tilemap_world.register_sprite_asset(*tile_sprite.document)||
       !tilemap_world.register_tile_palette(*tile_palette.document)||!tilemap_world.register_tilemap_asset(*tile_asset.document)||
       !tilemap_world.load_scene(*tilemap_contract.document).success) return 32;
    const auto tilemap_views=tilemap_world.entity_views();
    if(tilemap_views.size()!=1||!tilemap_views[0].tilemap_renderer||tilemap_views[0].tilemap_renderer->tilemap_asset!="tilemap.level"||
       tilemap_views[0].tilemap_cells.size()!=1||tilemap_views[0].tilemap_cells[0].cell_x!=9||
       tilemap_views[0].tilemap_cells[0].cell_y!=-1||tilemap_views[0].tilemap_cells[0].sprite_frame.frame.id!="ground.0"||
       tilemap_world.inspector_document_json("entity.tilemap").find("engine.entity.tilemap.tilemapAsset")==std::string::npos||
       tilemap_world.observe_json({.entity_ids={"entity.tilemap"}}).find("tilemapRenderer")==std::string::npos) {
        std::cerr<<tilemap_world.inspector_document_json("entity.tilemap")<<'\n'<<tilemap_world.observe_json({.entity_ids={"entity.tilemap"}})<<'\n';return 33;
    }
    tilemap_world.tick(0.0F);
    const auto tile_physics=tilemap_world.physics_observation_json();
    const auto tile_hit=tilemap_world.physics_ray_cast_json({4.75F,2.0F,0.0F},{0.0F,-3.0F,0.0F});
    if(tile_physics.find("TilemapColliderBake")==std::string::npos||tile_physics.find("entity.tilemap/tile-collider/ground/")==std::string::npos||
       tile_hit.find(R"("hit":true)")==std::string::npos||tile_hit.find("entity.tilemap/tile-collider/ground/")==std::string::npos) {
        std::cerr<<tile_physics<<'\n'<<tile_hit<<'\n';return 35;
    }
    const auto tilemap_plan=tilemap_world.plan_property_update("entity.tilemap","engine.entity.tilemap.visible","false",
        tilemap_world.revision(),"test.tilemap");
    const auto tilemap_receipt=tilemap_world.apply_property_plan(tilemap_plan,false);
    if(!tilemap_receipt.success||tilemap_world.canonical_scene_json().find(R"("visible": false)")==std::string::npos) {
        std::cerr<<tilemap_plan.code<<' '<<tilemap_plan.detail<<' '<<tilemap_receipt.code<<' '<<tilemap_receipt.detail<<'\n'
                 <<tilemap_world.canonical_scene_json()<<'\n';return 34;
    }
    const auto sprite_asset=noemancer::SpriteAssetCodec::parse_json(R"({"schema":"noemancer.sprite-asset/0.1",
      "assetId":"sprite.courier","textureAsset":"texture.courier","textureSize":[32,16],"pixelsPerUnit":16,
      "sampling":"nearest","alphaMode":"cutout","frames":[
      {"id":"idle.0","rect":[0,0,16,16],"trimOffset":[0,0],"sourceSize":[16,16],"pivot":[0.5,0.0],"collisionProfile":"body"},
      {"id":"idle.1","rect":[16,0,16,16],"trimOffset":[0,0],"sourceSize":[16,16],"pivot":[0.5,0.0],"collisionProfile":"body"}],
      "clips":[{"id":"idle","looping":true,"frames":[{"frame":"idle.0","durationMs":120,"event":""},{"frame":"idle.1","durationMs":120,"event":"blink"}]}],
      "provenance":{"sourceUri":"courier.png","sourceSha256":"fixture","generator":"test","license":"CC0"}})");
    noemancer::World sprite_world;
    if(!sprite_asset||!sprite_world.register_sprite_asset(*sprite_asset.document)||
       !sprite_world.load_scene(*sprite_contract.document).success) {
        std::cerr<<"Sprite asset did not register with Scene World\n";return 30;
    }
    sprite_world.tick(0.13F);
    if(sprite_world.sprite_observation_json("entity.sprite").find(R"("id":"idle.1")")==std::string::npos||
       sprite_world.snapshot_json().find("schema://noemancer/component/sprite-renderer/0.1")==std::string::npos||
       sprite_world.gameplay_observation_json().find("sprite.frame-event")==std::string::npos||
       sprite_world.gameplay_observation_json().find("blink")==std::string::npos) {
        std::cerr<<"Sprite ECS playback, event, or semantic observation failed\n";return 31;
    }

    const auto capsule_contract = noemancer::SceneDocumentCodec::parse_json(
        R"({"schema":"noemancer.scene/0.1","sceneGuid":"scene.capsule","name":"Capsule","entities":[{"guid":"entity.capsule","name":"Capsule","parent":null,"components":{"CapsuleCollider":{"friction":0.6,"halfHeight":0.75,"radius":0.35,"restitution":0.1},"RigidBody":{"gravityFactor":1.0,"linearDamping":0.05,"mass":1.0,"motionType":"dynamic"},"Transform":{"position":[0.0,2.0,0.0]}}}]})");
    if (!capsule_contract || !capsule_contract.document->entities[0].capsule_collider ||
        capsule_contract.document->entities[0].capsule_collider->half_height != 0.75 ||
        noemancer::SceneDocumentCodec::write_canonical_json(*capsule_contract.document).find("CapsuleCollider") == std::string::npos) {
        std::cerr << "Capsule collider scene contract did not round-trip\n";
        return 13;
    }
    noemancer::World capsule_world;
    const auto capsule_load = capsule_world.load_scene(*capsule_contract.document);
    capsule_world.tick(1.0F / 60.0F);
    if (!capsule_load.success || capsule_world.physics_observation_json().find("capsuleCollider") == std::string::npos ||
        capsule_world.inspector_document_json("entity.capsule").find("engine.entity.collider.halfHeight") == std::string::npos) {
        std::cerr << "Capsule collider did not reach ECS, Jolt, observation, and Inspector\n";
        return 15;
    }
    auto invalid_capsule = *capsule_contract.document;
    invalid_capsule.entities[0].capsule_collider->radius = 0.0;
    if (std::ranges::none_of(noemancer::SceneDocumentCodec::validate(invalid_capsule), [](const noemancer::SceneDocumentError& error) {
            return error.code == "scene.invalid-capsule-collider";
        })) {
        std::cerr << "Invalid capsule collider was not rejected\n";
        return 14;
    }

    const auto convex_contract=noemancer::SceneDocumentCodec::parse_json(
        R"({"schema":"noemancer.scene/0.1","sceneGuid":"scene.convex","name":"Convex","entities":[{"guid":"entity.convex","name":"Convex","parent":null,"components":{"ConvexHullCollider":{"friction":0.5,"points":[[-0.5,-0.5,-0.5],[0.5,-0.5,-0.5],[-0.5,0.5,-0.5],[0.5,0.5,-0.5],[-0.5,-0.5,0.5],[0.5,-0.5,0.5],[-0.5,0.5,0.5],[0.5,0.5,0.5]],"restitution":0.1},"RigidBody":{"gravityFactor":1.0,"linearDamping":0.05,"mass":1.0,"motionType":"dynamic"},"Transform":{"position":[0.0,2.0,0.0]}}}]})");
    if(!convex_contract||!convex_contract.document->entities[0].convex_hull_collider||
        convex_contract.document->entities[0].convex_hull_collider->points.size()!=8||
        noemancer::SceneDocumentCodec::write_canonical_json(*convex_contract.document).find("ConvexHullCollider")==std::string::npos) {
        std::cerr<<"Convex hull scene contract did not round-trip\n"; return 16;
    }
    noemancer::World convex_world;
    if(!convex_world.load_scene(*convex_contract.document).success||convex_world.physics_observation_json().find("convexHullCollider")==std::string::npos||
        convex_world.inspector_document_json("entity.convex").find("Hull Points")==std::string::npos) return 17;
    auto coplanar=*convex_contract.document;
    for(auto& point:coplanar.entities[0].convex_hull_collider->points) point.y=0.0;
    if(std::ranges::none_of(noemancer::SceneDocumentCodec::validate(coplanar),[](const noemancer::SceneDocumentError& error){
        return error.code=="scene.invalid-convex-hull-collider";
    })) { std::cerr<<"Coplanar convex hull was not rejected before Jolt creation\n"; return 18; }

    auto cyclic = *parsed.document;
    cyclic.entities[0].parent_guid = cyclic.entities[1].guid;
    cyclic.entities[1].parent_guid = cyclic.entities[0].guid;
    const auto cycle_errors = noemancer::SceneDocumentCodec::validate(cyclic);
    if (std::ranges::none_of(cycle_errors, [](const noemancer::SceneDocumentError& error) {
            return error.code == "scene.parent-cycle";
        })) {
        std::cerr << "Scene hierarchy cycle was not rejected\n";
        return 9;
    }

    const auto stress = noemancer::make_render_stress_scene_document(64);
    if (stress.scene_guid != "scene.render-stress" || stress.entities.size() != 73 ||
        stress.entities[9].guid != "entity.render-stress-cube-0" ||
        stress.entities.back().guid != "entity.render-stress-cube-63" ||
        stress.entities[1].transform->position.y<=stress.entities[1].transform->position.x||
        stress.entities[1].camera->far_clip<200.0||
        stress.entities[9].pbr_material->base_color.x==stress.entities[10].pbr_material->base_color.x||
        !stress.entities.back().mesh_renderer ||
        !noemancer::SceneDocumentCodec::validate(stress).empty()) {
        std::cerr << "Render stress scene is not deterministic or valid\n";
        return 10;
    }
    const auto visibility_stress=noemancer::make_render_stress_scene_document(100,37);
    const auto offscreen_count=std::ranges::count_if(
        visibility_stress.entities.begin()+9,visibility_stress.entities.end(),[](const auto& entity) {
            return entity.transform&&entity.transform->position.x>100.0;
        });
    if(visibility_stress.scene_guid!="scene.gpu-visibility-stress"||
       visibility_stress.source_uri!="generated://scenes/gpu-visibility-stress.scene.json"||
       visibility_stress.entities.size()!=109U||offscreen_count!=37||
       !noemancer::SceneDocumentCodec::validate(visibility_stress).empty()) {
        std::cerr<<"Partial GPU visibility stress scene is not deterministic or valid\n";
        return 60;
    }
    const auto simulation_stress=noemancer::make_animation_physics_stress_scene_document();
    const auto simulation_errors=noemancer::SceneDocumentCodec::validate(simulation_stress);
    const auto animated_count=std::ranges::count_if(simulation_stress.entities,[](const auto& entity){return entity.animation_player.has_value();});
    const auto dynamic_count=std::ranges::count_if(simulation_stress.entities,[](const auto& entity){
        return entity.rigid_body&&entity.rigid_body->motion_type=="dynamic";
    });
    if(simulation_stress.scene_guid!="scene.animation-physics-stress"||simulation_stress.entities.size()!=328U||
       animated_count!=64||dynamic_count!=256||!simulation_errors.empty()) {
        std::cerr<<"Animation/physics stress scene is not deterministic or valid: entities="<<simulation_stress.entities.size()
                 <<", animated="<<animated_count<<", dynamic="<<dynamic_count<<", errors="<<simulation_errors.size()<<'\n';
        return 33;
    }
    const auto reference=noemancer::make_commercial_raster_reference_scene_document();
    const auto reference_contract=noemancer::commercial_raster_reference_contract_json();
    const auto reference_errors=noemancer::SceneDocumentCodec::validate(reference);
    using Json = nlohmann::json;
    const auto parsed_reference_contract = Json::parse(reference_contract, nullptr, false);
    const auto bloom_fixture = parsed_reference_contract.is_object() &&
            parsed_reference_contract.contains("bloomFixture") &&
            parsed_reference_contract.at("bloomFixture").is_object()
        ? parsed_reference_contract.at("bloomFixture") : Json::object();
    const auto source_search_roi = bloom_fixture.contains("sourceSearchRoi") &&
            bloom_fixture.at("sourceSearchRoi").is_object()
        ? bloom_fixture.at("sourceSearchRoi") : Json::object();
    const auto core_roi = bloom_fixture.contains("coreRoi") && bloom_fixture.at("coreRoi").is_object()
        ? bloom_fixture.at("coreRoi") : Json::object();
    const auto core_radius = core_roi.contains("radius") && core_roi.at("radius").is_object()
        ? core_roi.at("radius") : Json::object();
    const auto near_halo_roi = bloom_fixture.contains("nearHaloRoi") && bloom_fixture.at("nearHaloRoi").is_object()
        ? bloom_fixture.at("nearHaloRoi") : Json::object();
    const auto far_halo_roi = bloom_fixture.contains("farHaloRoi") && bloom_fixture.at("farHaloRoi").is_object()
        ? bloom_fixture.at("farHaloRoi") : Json::object();
    const auto baseline_roi = bloom_fixture.contains("baselineRoi") && bloom_fixture.at("baselineRoi").is_object()
        ? bloom_fixture.at("baselineRoi") : Json::object();
    const auto large_area_roi = bloom_fixture.contains("largeWeakAreaRoi") &&
            bloom_fixture.at("largeWeakAreaRoi").is_object()
        ? bloom_fixture.at("largeWeakAreaRoi") : Json::object();
    const auto large_area_baseline_roi = bloom_fixture.contains("largeWeakAreaBaselineRoi") &&
            bloom_fixture.at("largeWeakAreaBaselineRoi").is_object()
        ? bloom_fixture.at("largeWeakAreaBaselineRoi") : Json::object();
    const auto material_ao_fixture = parsed_reference_contract.is_object() &&
            parsed_reference_contract.contains("materialAoFixture") &&
            parsed_reference_contract.at("materialAoFixture").is_object()
        ? parsed_reference_contract.at("materialAoFixture") : Json::object();
    const auto material_ao_rois = material_ao_fixture.contains("rois") &&
            material_ao_fixture.at("rois").is_object()
        ? material_ao_fixture.at("rois") : Json::object();
    const auto material_ao_thresholds = material_ao_fixture.contains("thresholds") &&
            material_ao_fixture.at("thresholds").is_object()
        ? material_ao_fixture.at("thresholds") : Json::object();
    const auto color_fixture = parsed_reference_contract.is_object() &&
            parsed_reference_contract.contains("colorResponseFixture") &&
            parsed_reference_contract.at("colorResponseFixture").is_object()
        ? parsed_reference_contract.at("colorResponseFixture") : Json::object();
    const auto color_authoring = color_fixture.contains("authoring") &&
            color_fixture.at("authoring").is_object()
        ? color_fixture.at("authoring") : Json::object();
    const auto color_input = color_fixture.contains("input") && color_fixture.at("input").is_object()
        ? color_fixture.at("input") : Json::object();
    const auto color_world_layout = color_fixture.contains("worldLayout") &&
            color_fixture.at("worldLayout").is_object()
        ? color_fixture.at("worldLayout") : Json::object();
    const auto color_source_ids = color_fixture.contains("sourceIds") &&
            color_fixture.at("sourceIds").is_object()
        ? color_fixture.at("sourceIds") : Json::object();
    const auto color_expected_order = color_fixture.contains("expectedOrder") &&
            color_fixture.at("expectedOrder").is_object()
        ? color_fixture.at("expectedOrder") : Json::object();
    const auto color_rois = color_fixture.contains("rois") && color_fixture.at("rois").is_object()
        ? color_fixture.at("rois") : Json::object();
    const auto color_thresholds = color_fixture.contains("thresholds") &&
            color_fixture.at("thresholds").is_object()
        ? color_fixture.at("thresholds") : Json::object();
    const auto neutral_ramp_roi = color_rois.contains("neutralRamp") &&
            color_rois.at("neutralRamp").is_object()
        ? color_rois.at("neutralRamp") : Json::object();
    const auto chromatic_patches_roi = color_rois.contains("chromaticPatches") &&
            color_rois.at("chromaticPatches").is_object()
        ? color_rois.at("chromaticPatches") : Json::object();
    const auto hdr_rolloff_roi = color_rois.contains("hdrRollOff") &&
            color_rois.at("hdrRollOff").is_object()
        ? color_rois.at("hdrRollOff") : Json::object();
    const auto material_gradient_roi = material_ao_rois.contains("materialGradient") &&
            material_ao_rois.at("materialGradient").is_object()
        ? material_ao_rois.at("materialGradient") : Json::object();
    const auto ao_roi = material_ao_rois.contains("ao") && material_ao_rois.at("ao").is_object()
        ? material_ao_rois.at("ao") : Json::object();
    const auto contact_roi = material_ao_rois.contains("contact") && material_ao_rois.at("contact").is_object()
        ? material_ao_rois.at("contact") : Json::object();
    const auto concave_roi = material_ao_rois.contains("concave") && material_ao_rois.at("concave").is_object()
        ? material_ao_rois.at("concave") : Json::object();
    const auto control_roi = material_ao_rois.contains("control") && material_ao_rois.at("control").is_object()
        ? material_ao_rois.at("control") : Json::object();
    const auto is_rectangle_roi = [](const Json& roi, const double x0, const double x1,
                                     const double y0, const double y1) {
        return roi.value("shape", std::string{}) == "rectangle" &&
               roi.value("x0", -1.0) == x0 && roi.value("x1", -1.0) == x1 &&
               roi.value("y0", -1.0) == y0 && roi.value("y1", -1.0) == y1;
    };
    const auto is_annulus_roi = [](const Json& roi, const double inner, const double outer) {
        return roi.value("shape", std::string{}) == "annulus" &&
               roi.value("center", std::string{}) == "coreRoi" &&
               roi.value("innerRadiusCoreMultiplier", -1.0) == inner &&
               roi.value("outerRadiusCoreMultiplier", -1.0) == outer;
    };
    const auto is_color_row_roi = [](const Json& roi, const double y0, const double y1) {
        return roi.value("shape", std::string{}) == "rectangle" &&
               roi.value("x0", -1.0) == 0.83 && roi.value("x1", -1.0) == 0.93 &&
               roi.value("y0", -1.0) == y0 && roi.value("y1", -1.0) == y1 &&
               roi.value("columnCount", -1) == 6 &&
               roi.value("columnHalfWidth", -1.0) == 0.004 &&
               roi.value("columnCenters", Json{}) == Json::array({0.837, 0.855, 0.872, 0.889, 0.907, 0.924});
    };
    const auto exercises = parsed_reference_contract.is_object() &&
            parsed_reference_contract.contains("exercises") &&
            parsed_reference_contract.at("exercises").is_array()
        ? parsed_reference_contract.at("exercises") : Json::array();
    const auto exercises_bloom = std::ranges::any_of(exercises, [](const Json& exercise) {
        return exercise.is_string() && exercise.get<std::string>() == "bloom.multi-scale-dual-filter";
    });
    const auto small_source = std::ranges::find_if(reference.entities, [](const auto& entity) {
        return entity.guid == "entity.reference.emissive-cool";
    });
    const auto large_source = std::ranges::find_if(reference.entities, [](const auto& entity) {
        return entity.guid == "entity.reference.emissive-warm";
    });
    const auto material_last = std::ranges::find_if(reference.entities, [](const auto& entity) {
        return entity.guid == "entity.reference.material-4-4";
    });
    const auto ao_contact_source = std::ranges::find_if(reference.entities, [](const auto& entity) {
        return entity.guid == "entity.reference.ao-contact-prop";
    });
    const auto ao_wall_source = std::ranges::find_if(reference.entities, [](const auto& entity) {
        return entity.guid == "entity.reference.ao-corner-wall";
    });
    const auto ao_backdrop_source = std::ranges::find_if(reference.entities, [](const auto& entity) {
        return entity.guid == "entity.reference.ao-dark-backdrop";
    });
    const auto color_neutral_first = std::ranges::find_if(reference.entities, [](const auto& entity) {
        return entity.guid == "entity.reference.color-neutral-0";
    });
    const auto color_chromatic_first = std::ranges::find_if(reference.entities, [](const auto& entity) {
        return entity.guid == "entity.reference.color-chromatic-0";
    });
    const auto color_hdr_last = std::ranges::find_if(reference.entities, [](const auto& entity) {
        return entity.guid == "entity.reference.color-hdr-5";
    });
    const auto entity_id_exists = [&](const Json& id) {
        return id.is_string() && std::ranges::any_of(reference.entities, [&](const auto& entity) {
            return entity.guid == id.get<std::string>();
        });
    };
    const auto source_id_array_is_stable = [&](const Json& ids) {
        return ids.is_array() && ids.size() == 6U && std::ranges::all_of(ids, entity_id_exists);
    };
    const bool identity_is_v18 =
        reference.scene_guid == noemancer::commercial_raster_reference_scene_guid &&
        reference.name == noemancer::commercial_raster_reference_name &&
        reference.source_uri == noemancer::commercial_raster_reference_source_uri &&
        parsed_reference_contract.is_object() &&
        parsed_reference_contract.value("id", std::string{}) ==
            noemancer::commercial_raster_reference_scene_id &&
        parsed_reference_contract.value("name", std::string{}) ==
            noemancer::commercial_raster_reference_name &&
        parsed_reference_contract.value("sceneGuid", std::string{}) ==
            noemancer::commercial_raster_reference_scene_guid &&
        parsed_reference_contract.value("sourceUri", std::string{}) ==
            noemancer::commercial_raster_reference_source_uri &&
        reference_contract.find("v1-5") == std::string::npos &&
        reference_contract.find("v1-6") == std::string::npos &&
        reference_contract.find("v1-7") == std::string::npos;
    const bool bloom_fixture_is_stable =
        bloom_fixture.value("schemaVersion", std::string{}) == "noemancer.bloom-quality-fixture/0.1" &&
        bloom_fixture.value("smallHighIntensitySourceId", std::string{}) ==
            "entity.reference.emissive-cool" &&
        bloom_fixture.value("largeWeakAreaSourceId", std::string{}) ==
            "entity.reference.emissive-warm" &&
        bloom_fixture.value("analysisCoordinateSpace", std::string{}) == "output-normalized" &&
        bloom_fixture.value("analysisIntent", std::string{}) ==
            "small-core-near-halo-far-halo-energy-cap" &&
        bloom_fixture.value("smallSourceWorldPosition", Json{}) == Json::array({-4.15, 5.75, 1.1}) &&
        bloom_fixture.value("largeAreaWorldPosition", Json{}) == Json::array({0.4, 7.55, -0.8}) &&
        is_rectangle_roi(source_search_roi, 0.24, 0.42, 0.24, 0.53) &&
        core_roi.value("shape", std::string{}) == "disk" &&
        core_roi.value("center", std::string{}) == "brightest-pixel-in-source-search-roi" &&
        core_radius.value("minimumPixels", -1.0) == 8.0 &&
        core_radius.value("minOutputDimensionFraction", -1.0) == 0.012 &&
        is_annulus_roi(near_halo_roi, 1.5, 3.5) &&
        is_annulus_roi(far_halo_roi, 3.5, 6.5) &&
        is_annulus_roi(baseline_roi, 7.5, 11.0) &&
        is_rectangle_roi(large_area_roi, 0.36, 0.64, 0.13, 0.31) &&
        is_rectangle_roi(large_area_baseline_roi, 0.36, 0.64, 0.06, 0.12) &&
        exercises_bloom && small_source != reference.entities.end() && large_source != reference.entities.end() &&
        small_source->name == "Bloom High-Intensity Pin" && large_source->name == "Bloom Soft Emissive Area" &&
        small_source->transform && small_source->transform->position.x == -4.15 &&
        small_source->transform->position.y == 5.75 && small_source->transform->position.z == 1.1 &&
        large_source->transform && large_source->transform->position.x == 0.4 &&
        large_source->transform->position.y == 7.55 && large_source->transform->position.z == -0.8;
    const bool material_ao_fixture_is_stable =
        material_ao_fixture.value("schemaVersion", std::string{}) == "noemancer.material-ao-fixture/0.1" &&
        material_ao_fixture.value("analysisCoordinateSpace", std::string{}) == "output-normalized" &&
        material_ao_fixture.value("analysisIntent", std::string{}) ==
            "metallic-roughness-gradient-contact-concave-ao-ab" &&
        material_ao_fixture.value("comparison", std::string{}) ==
            "ao-enabled-minus-disabled-linear-luma" &&
        material_ao_fixture.contains("abIntent") &&
        material_ao_fixture.at("abIntent").value("enabledImage", std::string{}) == "ao-enabled" &&
        material_ao_fixture.at("abIntent").value("disabledImage", std::string{}) == "ao-disabled" &&
        material_ao_fixture.at("abIntent").value("delta", std::string{}) ==
            "enabled-minus-disabled-linear-luma" &&
        material_ao_fixture.at("abIntent").value("aoExpectedSign", std::string{}) == "negative" &&
        material_ao_fixture.at("abIntent").value("controlExpectedSign", std::string{}) == "near-zero" &&
        material_ao_fixture.value("materialGradientSourceIds", Json{}) == Json::array({
            "entity.reference.material-0-0", "entity.reference.material-0-4",
            "entity.reference.material-4-0", "entity.reference.material-4-4"}) &&
        material_ao_fixture.value("contactSourceId", std::string{}) ==
            "entity.reference.ao-contact-prop" &&
        material_ao_fixture.value("concaveSourceIds", Json{}) == Json::array({
            "entity.reference.ground", "entity.reference.ao-corner-wall"}) &&
        material_ao_fixture.value("darkBackgroundSourceId", std::string{}) ==
            "entity.reference.ao-dark-backdrop" &&
        material_ao_fixture.contains("materialGradient") &&
        material_ao_fixture.at("materialGradient").value("metallic", Json{}).value("axis", std::string{}) == "column" &&
        material_ao_fixture.at("materialGradient").value("metallic", Json{}).value("start", -1.0) == 0.0 &&
        material_ao_fixture.at("materialGradient").value("metallic", Json{}).value("end", -1.0) == 1.0 &&
        material_ao_fixture.at("materialGradient").value("roughness", Json{}).value("axis", std::string{}) == "row" &&
        material_ao_fixture.at("materialGradient").value("roughness", Json{}).value("start", -1.0) == 0.08 &&
        material_ao_fixture.at("materialGradient").value("roughness", Json{}).value("end", -1.0) == 0.96 &&
        is_rectangle_roi(material_gradient_roi, 0.25, 0.62, 0.53, 0.86) &&
        is_rectangle_roi(ao_roi, 0.63, 0.82, 0.52, 0.86) &&
        is_rectangle_roi(contact_roi, 0.66, 0.76, 0.70, 0.85) &&
        is_rectangle_roi(concave_roi, 0.65, 0.82, 0.53, 0.71) &&
        is_rectangle_roi(control_roi, 0.73, 0.94, 0.16, 0.34) &&
        material_ao_thresholds.value("globalMeanLinearMin", -1.0) == 0.02 &&
        material_ao_thresholds.value("globalMeanLinearMax", -1.0) == 0.90 &&
        material_ao_thresholds.value("globalMeanDeltaAbsMax", -1.0) == 0.025 &&
        material_ao_thresholds.value("aoMeanDeltaMin", 1.0) == -0.20 &&
        material_ao_thresholds.value("aoMeanDeltaMax", 1.0) == -0.001 &&
        material_ao_thresholds.value("controlMeanDeltaAbsMax", -1.0) == 0.01 &&
        material_ao_thresholds.value("controlP95AbsDeltaMax", -1.0) == 0.025 &&
        material_last != reference.entities.end() &&
        ao_contact_source != reference.entities.end() && ao_wall_source != reference.entities.end() &&
        ao_backdrop_source != reference.entities.end() &&
        ao_contact_source->transform && ao_contact_source->transform->position.x == 4.05 &&
        ao_contact_source->transform->position.y == 0.25 && ao_contact_source->transform->position.z == -1.0 &&
        ao_contact_source->pbr_material && ao_contact_source->pbr_material->metallic == 0.82 &&
        ao_contact_source->pbr_material->roughness == 0.18 &&
        ao_wall_source->transform && ao_wall_source->transform->position.z == -1.4 &&
        ao_wall_source->pbr_material && ao_wall_source->pbr_material->roughness == 0.88 &&
        ao_backdrop_source->pbr_material && ao_backdrop_source->pbr_material->base_color.x == 0.012 &&
        ao_backdrop_source->pbr_material->roughness == 0.96;
    const bool color_response_fixture_is_stable =
        color_fixture.value("schemaVersion", std::string{}) == "noemancer.color-response-fixture/0.1" &&
        color_fixture.value("analysisCoordinateSpace", std::string{}) == "output-normalized" &&
        color_fixture.value("analysisIntent", std::string{}) ==
            "neutral-exposure-ramp-rgb-cmy-separation-hdr-shoulder-compression" &&
        color_authoring.value("geometry", std::string{}) == "asset.primitive.cube" &&
        color_authoring.value("material", std::string{}) == "pbr-emissive-isolated" &&
        !color_authoring.value("castsShadows", true) && !color_authoring.value("receivesShadows", true) &&
        color_authoring.value("baseColor", Json{}) == Json::array({0.0, 0.0, 0.0}) &&
        color_authoring.value("metallic", -1.0) == 1.0 &&
        color_authoring.value("roughness", -1.0) == 1.0 &&
        color_authoring.value("neutralEmissiveIntensity", -1.0) == 1.0 &&
        color_authoring.value("chromaticEmissiveIntensity", -1.0) == 0.25 &&
        color_authoring.value("hdrEmissiveIntensities", Json{}) ==
            Json::array({0.125, 0.25, 0.50, 1.0, 2.0, 4.0}) &&
        color_input.value("neutralRampLinearValues", Json{}) ==
            Json::array({0.03, 0.08, 0.18, 0.36, 0.60, 0.90}) &&
        color_input.value("chromaticPatchLabels", Json{}) ==
            Json::array({"red", "green", "blue", "cyan", "magenta", "yellow"}) &&
        color_input.value("chromaticPatchLinearColors", Json{}) == Json::array({
            Json::array({0.85, 0.03, 0.02}), Json::array({0.03, 0.75, 0.05}),
            Json::array({0.02, 0.08, 0.85}), Json::array({0.02, 0.75, 0.78}),
            Json::array({0.78, 0.03, 0.72}), Json::array({0.84, 0.76, 0.03})}) &&
        color_input.value("hdrNeutralBaseColor", Json{}) == Json::array({0.45, 0.45, 0.45}) &&
        color_world_layout.value("xStart", -1.0) == 8.2 &&
        color_world_layout.value("xStep", -1.0) == 0.42 &&
        color_world_layout.value("columnCount", -1) == 6 &&
        color_world_layout.value("neutralY", -1.0) == 0.75 &&
        color_world_layout.value("chromaticY", -1.0) == 1.35 &&
        color_world_layout.value("hdrY", -1.0) == 1.95 &&
        color_world_layout.value("z", 0.0) == -0.7 &&
        color_world_layout.value("scale", Json{}) == Json::array({0.30, 0.18, 0.10}) &&
        color_source_ids.value("neutralRamp", Json{}) == Json::array({
            "entity.reference.color-neutral-0", "entity.reference.color-neutral-1",
            "entity.reference.color-neutral-2", "entity.reference.color-neutral-3",
            "entity.reference.color-neutral-4", "entity.reference.color-neutral-5"}) &&
        color_source_ids.value("chromaticPatches", Json{}) == Json::array({
            "entity.reference.color-chromatic-0", "entity.reference.color-chromatic-1",
            "entity.reference.color-chromatic-2", "entity.reference.color-chromatic-3",
            "entity.reference.color-chromatic-4", "entity.reference.color-chromatic-5"}) &&
        color_source_ids.value("hdrRollOff", Json{}) == Json::array({
            "entity.reference.color-hdr-0", "entity.reference.color-hdr-1",
            "entity.reference.color-hdr-2", "entity.reference.color-hdr-3",
            "entity.reference.color-hdr-4", "entity.reference.color-hdr-5"}) &&
        source_id_array_is_stable(color_source_ids.value("neutralRamp", Json{})) &&
        source_id_array_is_stable(color_source_ids.value("chromaticPatches", Json{})) &&
        source_id_array_is_stable(color_source_ids.value("hdrRollOff", Json{})) &&
        color_expected_order.value("neutralRamp", std::string{}) ==
            "increasing-linear-luma-left-to-right" &&
        color_expected_order.value("chromaticPatches", Json{}) ==
            Json::array({"red", "green", "blue", "cyan", "magenta", "yellow"}) &&
        color_expected_order.value("hdrRollOff", std::string{}) ==
            "increasing-emissive-input-left-to-right-with-late-step-compression" &&
        is_color_row_roi(neutral_ramp_roi, 0.712, 0.727) &&
        is_color_row_roi(chromatic_patches_roi, 0.669, 0.684) &&
        is_color_row_roi(hdr_rolloff_roi, 0.626, 0.641) &&
        color_thresholds.value("neutralAdjacentLumaMin", -1.0) == 0.002 &&
        color_thresholds.value("neutralChannelSpreadMax", -1.0) == 0.05 &&
        color_thresholds.value("neutralRelativeBiasMax", -1.0) == 0.30 &&
        color_thresholds.value("chromaticDominantChannelMin", -1.0) == 0.20 &&
        color_thresholds.value("chromaticDominantAdvantageMin", -1.0) == 0.035 &&
        color_thresholds.value("hdrAdjacentLumaMin", -1.0) == 0.001 &&
        color_thresholds.value("hdrLateStepRatioMax", -1.0) == 0.95 &&
        color_thresholds.value("hdrClippedFractionMax", -1.0) == 0.05 &&
        color_neutral_first != reference.entities.end() && color_chromatic_first != reference.entities.end() &&
        color_hdr_last != reference.entities.end() &&
        color_neutral_first->transform && color_neutral_first->transform->position.x == 8.2 &&
        color_neutral_first->transform->position.y == 0.75 && color_neutral_first->transform->position.z == -0.7 &&
        color_neutral_first->pbr_material && color_neutral_first->pbr_material->base_color.x == 0.0 &&
        color_neutral_first->pbr_material->metallic == 1.0 &&
        color_neutral_first->pbr_material->emissive_color.x == 0.03 &&
        color_neutral_first->pbr_material->emissive_intensity == 1.0 &&
        color_chromatic_first->pbr_material && color_chromatic_first->pbr_material->base_color.x == 0.0 &&
        color_chromatic_first->pbr_material->emissive_color.x == 0.85 &&
        color_chromatic_first->pbr_material->emissive_color.y == 0.03 &&
        color_chromatic_first->pbr_material->emissive_intensity == 0.25 &&
        color_hdr_last->transform &&
        std::abs(color_hdr_last->transform->position.x - 10.3) < 1.0e-9 &&
        color_hdr_last->transform->position.y == 1.95 && color_hdr_last->transform->position.z == -0.7 &&
        color_hdr_last->pbr_material && color_hdr_last->pbr_material->emissive_intensity == 4.0;
    if(!identity_is_v18 || !bloom_fixture_is_stable || !material_ao_fixture_is_stable ||
       !color_response_fixture_is_stable || reference.entities.size()!=59||
       reference.entities[13].guid!="entity.reference.material-0-0"||
       reference.entities[37].guid!="entity.reference.material-4-4"||
       reference.entities[38].guid!="entity.reference.ao-contact-prop"||
       reference.entities[39].guid!="entity.reference.ao-corner-wall"||
       reference.entities[40].guid!="entity.reference.ao-dark-backdrop"||
       reference.entities[41].guid!="entity.reference.color-neutral-0"||
       reference.entities[42].guid!="entity.reference.color-chromatic-0"||
       reference.entities[43].guid!="entity.reference.color-hdr-0"||
       reference.entities.back().guid!="entity.reference.color-hdr-5"||
       material_last == reference.entities.end()||
       !reference_errors.empty()||
       reference_contract.find("noemancer.commercial-raster-reference/1.8")==std::string::npos||
       reference_contract.find(R"("width":1920)")==std::string::npos||
       reference_contract.find("lighting.local-clustered")==std::string::npos||
       reference_contract.find("material.normal-texture")==std::string::npos||
       reference_contract.find("material.alpha-blend")==std::string::npos||
       reference_contract.find("lighting.local-shadow-point")==std::string::npos||
       reference_contract.find("shadow.directional-cache")==std::string::npos||
       reference_contract.find("noemancer.bloom-quality-fixture/0.1")==std::string::npos||
       reference_contract.find("bloom.multi-scale-dual-filter")==std::string::npos||
       reference_contract.find("noemancer.color-response-fixture/0.1")==std::string::npos) {
        std::cerr<<"Commercial Raster reference scene or contract is not deterministic: entities="<<reference.entities.size()
                 <<", firstGrid="<<(reference.entities.size()>13?reference.entities[13].guid:"missing")
                 <<", errors="<<reference_errors.size()<<", contractBytes="<<reference_contract.size()
                 <<", identity="<<identity_is_v18<<", bloom="<<bloom_fixture_is_stable
                 <<", materialAo="<<material_ao_fixture_is_stable
                 <<", colorResponse="<<color_response_fixture_is_stable<<'\n';
        for(const auto& error:reference_errors)std::cerr<<error.code<<' '<<error.path<<' '<<error.message<<'\n';
        return 32;
    }
    return 0;
}
