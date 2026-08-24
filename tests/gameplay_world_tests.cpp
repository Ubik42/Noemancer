#include "engine/scene_document.hpp"
#include "engine/world.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <filesystem>
#include <iostream>

int main() {
    noemancer::World world;
    if(!world.load_scene(noemancer::make_bootstrap_scene_document()).success) return 1;
    const auto initial_count=world.entity_count();
    const auto spawned=nlohmann::json::parse(world.spawn_prefab_json("entity.demo-cube","entity.spawned-cube","Spawned Cube",{4.0F,2.0F,1.0F}));
    if(!spawned.at("success").get<bool>()||world.entity_count()!=initial_count+1||world.snapshot_json().find("entity.spawned-cube")==std::string::npos) {
        std::cerr<<"Prefab spawn did not create a persisted ECS entity\n"; return 2;
    }
    const auto save=nlohmann::json::parse(world.save_capture_json());
    if(save.at("schemaVersion")!="noemancer.save-game/0.2"||!save.at("document").is_object()||
       save.at("scriptState").at("schemaVersion")!="noemancer.script-state/0.1") return 3;
    const auto despawned=nlohmann::json::parse(world.despawn_entity_json("entity.spawned-cube"));
    if(!despawned.at("success").get<bool>()||world.entity_count()!=initial_count) return 4;
    const auto restored=nlohmann::json::parse(world.save_restore_json(save.dump()));
    if(!restored.at("success").get<bool>()||world.entity_count()!=initial_count+1) return 5;

    static_cast<void>(world.replay_start_json());
    static_cast<void>(world.inject_input_json("keyboard.space",1.0F));
    static_cast<void>(world.inject_input_json("keyboard.space",0.0F));
    const auto replay=nlohmann::json::parse(world.replay_stop_json());
    if(replay.at("schemaVersion")!="noemancer.input-replay/0.2"||replay.at("sampleCount")!=2||
       !replay.at("initialSave").is_object()||!replay.at("samples").at(0).contains("tick")) return 6;
    const auto applied=nlohmann::json::parse(world.replay_apply_json(replay.dump()));
    if(!applied.at("success").get<bool>()||applied.at("appliedSamples")!=2||applied.at("simulatedTicks")!=1||
        world.gameplay_observation_json().find("replay.applied")==std::string::npos) return 7;
    const auto prefab=nlohmann::json::parse(world.export_prefab_json("entity.demo-sphere"));
    if(!prefab.at("valid").get<bool>()) return 8;
    const auto instantiated=nlohmann::json::parse(world.instantiate_prefab_json(prefab.dump(),"entity.prefab-sphere","Prefab Sphere",{-3.0F,2.0F,0.0F}));
    if(!instantiated.at("success").get<bool>()||world.snapshot_json().find("entity.prefab-sphere")==std::string::npos) return 9;
    const auto attached=nlohmann::json::parse(world.scripting_attach_json("script.player","entity.prefab-sphere","asset.script.gameplay","Game.PlayerController"));
    const auto invoked=nlohmann::json::parse(world.scripting_invoke_json("script.player","OnCreate",R"({"spawned":true})"));
    const auto scripting_abi=nlohmann::json::parse(world.scripting_abi_json());
    if(!attached.at("success").get<bool>()||!invoked.at("success").get<bool>()||invoked.at("executedManagedCode")!=true||
        invoked.at("managedResult").at("runtime").get<std::string>().find(".NET 10.")==std::string::npos||
        invoked.at("managedResult").at("callback")!="OnCreate"||scripting_abi.at("status")!="ready"||
        scripting_abi.at("hostfxr").at("requiredMajor")!=10||!scripting_abi.at("hostfxr").contains("diagnostic")||
        world.scripting_observation_json().find("Game.PlayerController")==std::string::npos) return 10;
    const auto script_project=std::filesystem::path(NOEMANCER_SOURCE_DIR)/"tests/fixtures/managed-project/ManagedFixture.csproj";
    const auto configured=nlohmann::json::parse(world.scripting_project_configure_json(script_project.parent_path(),script_project));
    const auto compiled=nlohmann::json::parse(world.scripting_project_compile_json("Debug"));
    const auto cached_compile=nlohmann::json::parse(world.scripting_project_compile_json("Debug"));
    const auto project_observation=nlohmann::json::parse(world.scripting_project_observation_json());
    if(!configured.at("success")||!compiled.at("success")||
       (compiled.at("cacheHit")&&compiled.at("cacheScope")!="disk")||compiled.at("assembly").get<std::string>().empty()||
       !compiled.at("diagnostics").empty()||!cached_compile.at("success")||!cached_compile.at("cacheHit")||
       project_observation.at("schemaVersion")!="noemancer.script-project-state/0.3"||
       project_observation.at("scriptProject").get<std::string>().find("ManagedFixture.csproj")==std::string::npos||
       project_observation.at("sourceState").at("needsCompile")||project_observation.at("sourceState").at("needsReload")||
       project_observation.at("sourceDocuments").at("count").get<std::size_t>()<1U||
       std::ranges::none_of(project_observation.at("sourceDocuments").at("items"),[](const auto& document){
           return document.at("path")=="PlayerController.cs"&&document.at("language")=="csharp";
       })) return 17;
    const auto type_catalog=nlohmann::json::parse(world.scripting_project_types_json());
    const auto player_type=std::ranges::find_if(type_catalog.at("types"),[](const auto& type) {
       return type.at("fullName")=="ManagedFixture.PlayerController";
    });
    if(!type_catalog.at("success")||type_catalog.at("typeCount").get<int>()<6||player_type==type_catalog.at("types").end()||
       !player_type->contains("callbacks")||
       std::ranges::none_of(player_type->at("callbacks"),[](const auto& callback){return callback=="OnCreate";})||
       std::ranges::any_of(player_type->at("callbacks"),[](const auto& callback){return callback=="OnUpdate";}))return 18;
    const auto persistence_attached=nlohmann::json::parse(world.scripting_attach_json(
        "script.persistence","entity.prefab-sphere","asset.script.fixture","ManagedFixture.PersistenceController"));
    const auto persistence_invoked=nlohmann::json::parse(world.scripting_invoke_json("script.persistence","OnCreate",R"({})"));
    const auto persistence_requests=world.consume_persistence_requests();
    if(!persistence_attached.at("success")||!persistence_invoked.at("success")||
       !persistence_invoked.at("commandApplication").at("success")||persistence_requests.size()!=5U||
       persistence_requests.at(0).action!="save"||persistence_requests.at(0).slot_id!="autosave"||
       persistence_requests.at(2).action!="replay-start"||!persistence_requests.at(2).slot_id.empty()||
       persistence_requests.at(4).action!="replay-play"||persistence_requests.at(4).slot_id!="last-run") {
        std::cerr<<persistence_invoked.dump(2)<<'\n';return 1810;
    }
    world.complete_persistence_request(persistence_requests.front(),true,"ok","fixture completion");
    if(world.gameplay_observation_json().find("gameplay.persistence.completed")==std::string::npos)return 1811;
    {
        noemancer::World sprite_world;auto sprite_scene=noemancer::make_bootstrap_scene_document();
        auto sprite_entity=std::ranges::find(sprite_scene.entities,std::string("entity.demo-sphere"),&noemancer::SceneEntityDocument::guid);
        if(sprite_entity==sprite_scene.entities.end())return 181;
        sprite_entity->sprite_renderer=noemancer::SceneSpriteRenderer{.sprite_asset="sprite.fixture",.clip="idle"};
        sprite_entity->velocity=noemancer::SceneVelocity{};sprite_entity->rigid_body=noemancer::SceneRigidBody{};
        sprite_entity->sphere_collider.reset();sprite_entity->capsule_collider=noemancer::SceneCapsuleCollider{};
        sprite_entity->character_motor_2d=noemancer::SceneCharacterMotor2D{};
        noemancer::SpriteAssetDocument sprite_asset{.asset_id="sprite.fixture",.texture_asset="texture.fixture",
            .texture_width=2,.texture_height=1,.pixels_per_unit=1.0F,
            .frames={{.id="idle.0",.width=1,.height=1,.source_width=1,.source_height=1},
                     {.id="run.0",.x=1,.width=1,.height=1,.source_width=1,.source_height=1}},
            .clips={{.id="idle",.frames={{.frame_id="idle.0",.duration_ms=100}}},
                    {.id="run",.frames={{.frame_id="run.0",.duration_ms=80}}}},
            .provenance={.source_uri="fixture",.source_sha256="fixture",.generator="test",.license="CC0-1.0"}};
        const auto sprite_errors=noemancer::SpriteAssetCodec::validate(sprite_asset);
        if(!sprite_errors.empty()) {for(const auto& error:sprite_errors)std::cerr<<error.code<<' '<<error.path<<'\n';return 182;}
        if(!sprite_world.register_sprite_asset(std::move(sprite_asset)))return 182;
        const auto audio_registered=nlohmann::json::parse(sprite_world.register_audio_asset_json(
            "asset.audio.script-fixture","sha256:script-fixture",noemancer::AudioAssetStorage::resident));
        if(!audio_registered.at("valid"))return 185;
        const auto sprite_load=sprite_world.load_scene(sprite_scene);
        if(!sprite_load.success){for(const auto& error:sprite_load.errors)std::cerr<<error.code<<' '<<error.path<<' '<<error.message<<'\n';return 184;}
        const auto sprite_configured=nlohmann::json::parse(
            sprite_world.scripting_project_configure_json(script_project.parent_path(),script_project));
        const auto sprite_compiled=nlohmann::json::parse(sprite_world.scripting_project_compile_json("Debug"));
        const auto sprite_attached=nlohmann::json::parse(sprite_world.scripting_attach_json(
            "script.sprite","entity.demo-sphere","asset.script.fixture","ManagedFixture.SpriteController"));
        const auto sprite_invoked=nlohmann::json::parse(sprite_world.scripting_invoke_json("script.sprite","OnCreate",R"({})"));
        const auto sprite_state=nlohmann::json::parse(sprite_world.sprite_observation_json("entity.demo-sphere"));
        if(!sprite_configured.at("success")||!sprite_compiled.at("success")||!sprite_attached.at("success")||
           !sprite_invoked.at("success")||sprite_invoked.at("commandApplication").at("applied")!=2||
           !sprite_invoked.at("managedResult").at("state").at("SawCharacterMotor")||
           sprite_invoked.at("managedResult").at("state").at("InitialClip")!="idle"||
           sprite_state.at("items").at(0).at("playback").at("clipId")!="run"||
           !sprite_state.at("items").at(0).at("flipX")||sprite_state.at("items").at(0).at("playbackSpeed")!=1.25F||
           sprite_world.canonical_scene_json().find(R"("clip": "idle")")==std::string::npos||
           sprite_world.audio_observation_json().find("asset.audio.script-fixture")==std::string::npos) {
            std::cerr<<sprite_invoked.dump(2)<<'\n'<<sprite_state.dump(2)<<'\n';return 183;
        }
    }
    const auto fixture_attached=nlohmann::json::parse(world.scripting_attach_json(
        "script.fixture","entity.prefab-sphere","asset.script.fixture","ManagedFixture.PlayerController"));
    const auto fixture_invoked=nlohmann::json::parse(world.scripting_invoke_json("script.fixture","OnCreate",R"({"source":"test"})"));
    if(!fixture_attached.at("success")||!fixture_invoked.at("success")||!fixture_invoked.at("executedManagedCode")||
       !fixture_invoked.at("managedResult").at("projectCodeExecuted")||
       fixture_invoked.at("managedResult").at("state").at("CreateCount")!=1||
       fixture_invoked.at("managedResult").at("state").at("TransformEntityCount").get<int>()<1||
       fixture_invoked.at("managedResult").at("loadGeneration")!=1) {
        std::cerr<<fixture_invoked.dump(2)<<'\n'; return 19;
    }
    const auto stateful_attached=nlohmann::json::parse(world.scripting_attach_json(
        "script.stateful","entity.prefab-sphere","asset.script.fixture","ManagedFixture.StatefulController"));
    const auto stateful_before_reload=nlohmann::json::parse(world.scripting_invoke_json("script.stateful","OnUpdate",R"({})"));
    const auto release_compile=nlohmann::json::parse(world.scripting_project_compile_json("Release"));
    const auto reloaded=nlohmann::json::parse(world.scripting_invoke_json("script.fixture","OnCreate",R"({"source":"reload-test"})"));
    const auto stateful_after_reload=nlohmann::json::parse(world.scripting_invoke_json("script.stateful","OnUpdate",R"({})"));
    if(!release_compile.at("success")||!reloaded.at("success")||!reloaded.at("managedResult").at("projectCodeExecuted")||
       reloaded.at("managedResult").at("loadGeneration")!=2||reloaded.at("managedResult").at("state").at("CreateCount")!=1||
       !stateful_attached.at("success")||stateful_before_reload.at("managedResult").at("state").at("Counter")!=1||
       !stateful_after_reload.at("success")||stateful_after_reload.at("managedResult").at("state").at("Counter")!=2||
       stateful_after_reload.at("managedResult").at("migration").at("restoredCount")!=1||
       stateful_after_reload.at("managedResult").at("migration").at("failedCount")!=0||
       stateful_after_reload.at("managedResult").at("migration").at("members").at(0).at("status")!="restored"||
       stateful_after_reload.at("managedResult").at("retiredLoadContexts").empty()||
       !stateful_after_reload.at("managedResult").at("retiredLoadContexts").at(0).contains("Collected")) {
        std::cerr<<reloaded.dump(2)<<'\n'<<stateful_after_reload.dump(2)<<'\n'; return 20;
    }
    const auto managed_save=nlohmann::json::parse(world.save_capture_json());
    const auto stateful_after_save=nlohmann::json::parse(world.scripting_invoke_json("script.stateful","OnUpdate",R"({})"));
    const auto managed_restored=nlohmann::json::parse(world.save_restore_json(managed_save.dump()));
    const auto stateful_after_restore=nlohmann::json::parse(world.scripting_invoke_json("script.stateful","OnUpdate",R"({})"));
    if(!managed_save.at("scriptState").at("success")||
       std::ranges::none_of(managed_save.at("scriptState").at("instances"),[](const auto& instance){
           return instance.at("instanceId")=="script.stateful"&&instance.at("state").at("property:Counter")==2;})||
       stateful_after_save.at("managedResult").at("state").at("Counter")!=3||!managed_restored.at("success")||
       managed_restored.at("scriptState").at("restoredCount").get<int>()<1||
       stateful_after_restore.at("managedResult").at("state").at("Counter")!=3) {
        std::cerr<<managed_save.dump(2)<<'\n'<<managed_restored.dump(2)<<'\n'<<stateful_after_restore.dump(2)<<'\n';return 201;
    }
    static_cast<void>(world.inject_input_json("keyboard.d",1.0F));
    const auto mover_attached=nlohmann::json::parse(world.scripting_attach_json(
        "script.mover","entity.prefab-sphere","asset.script.fixture","ManagedFixture.MoverController"));
    const auto mover_invoked=nlohmann::json::parse(world.scripting_invoke_json("script.mover","OnUpdate",R"({})"));
    const auto moved_views=world.entity_views();
    const auto moved=std::ranges::find(moved_views,std::string("entity.prefab-sphere"),&noemancer::WorldEntityView::id);
    if(!mover_attached.at("success")||!mover_invoked.at("success")||
       !mover_invoked.at("commandApplication").at("success")||mover_invoked.at("commandApplication").at("applied")!=2||
       mover_invoked.at("managedResult").at("state").at("ObservedInput")!=1.0F||moved==moved_views.end()||!moved->transform||
       moved->transform->x!=4.0F||moved->transform->y!=5.0F||moved->transform->z!=6.0F||
       world.gameplay_observation_json().find("script.fixture.moved")==std::string::npos) {
        std::cerr<<mover_invoked.dump(2)<<'\n'; return 22;
    }
    const auto throwing_attached=nlohmann::json::parse(world.scripting_attach_json(
        "script.throwing","entity.prefab-sphere","asset.script.fixture","ManagedFixture.ThrowingController"));
    const auto throwing_invoked=nlohmann::json::parse(world.scripting_invoke_json("script.throwing","OnUpdate",R"({})"));
    if(!throwing_attached.at("success")||throwing_invoked.at("success")||!throwing_invoked.at("executedManagedCode")||
       throwing_invoked.at("code")!="scripting.managed-callback-failed"||
       throwing_invoked.at("managedResult").at("error")!="InvalidOperationException"||
       throwing_invoked.at("managedResult").at("stackTrace").get<std::string>().find("ThrowingController.cs")==std::string::npos) return 21;
    auto automatic_scene=noemancer::make_bootstrap_scene_document();
    auto automatic_entity=std::ranges::find(automatic_scene.entities,std::string("entity.demo-sphere"),&noemancer::SceneEntityDocument::guid);
    automatic_entity->transform->position.y=1.0;
    automatic_entity->sphere_collider->is_trigger=true;
    automatic_entity->managed_script=noemancer::SceneManagedScript{"script.scene.mover","project.script","ManagedFixture.MoverController",true,R"({"speed":3.5})"};
    if(!world.load_scene(automatic_scene).success)return 23;
    static_cast<void>(world.inject_input_json("keyboard.d",1.0F));
    world.tick(1.0F/60.0F);
    const auto automatic_views=world.entity_views();
    const auto automatic_moved=std::ranges::find(automatic_views,std::string("entity.demo-sphere"),&noemancer::WorldEntityView::id);
    const auto automatic_scripts=nlohmann::json::parse(world.scripting_observation_json());
    const auto automatic_instance=std::ranges::find(automatic_scripts.at("instances"),std::string("script.scene.mover"),[](const auto& item){return item.at("id").template get<std::string>();});
    if(automatic_moved==automatic_views.end()||!automatic_moved->transform||automatic_moved->transform->x!=4.0F||
       automatic_instance==automatic_scripts.at("instances").end()||automatic_instance->at("callbackCount").get<int>()<3||
       automatic_instance->at("lastCallback")!="OnUpdate"||!automatic_instance->at("sceneOwned")||
       automatic_scripts.at("lastManagedResult").at("state").at("TriggerEnterCount").get<int>()<1||
       automatic_scripts.at("lastManagedResult").at("state").at("TriggerOtherId").get<std::string>().empty()||
       !automatic_scripts.at("lastManagedResult").at("state").at("TriggerWasSensor")){
        std::cerr<<automatic_scripts.dump(2)<<'\n'<<world.physics_observation_json()<<'\n';return 24;
    }
    {
        noemancer::World edit_world;
        noemancer::World play_world;
        if(!edit_world.load_scene(noemancer::make_bootstrap_scene_document()).success||
           !play_world.load_scene(noemancer::make_bootstrap_scene_document()).success)return 25;
        const auto configure_session=[&](noemancer::World& session_world) {
            const auto configured_session=nlohmann::json::parse(
                session_world.scripting_project_configure_json(script_project.parent_path(),script_project));
            const auto compiled_session=nlohmann::json::parse(session_world.scripting_project_compile_json("Debug"));
            const auto attached_session=nlohmann::json::parse(session_world.scripting_attach_json(
                "script.shared-id","entity.demo-sphere","asset.script.fixture","ManagedFixture.PlayerController"));
            const auto invoked_session=nlohmann::json::parse(
                session_world.scripting_invoke_json("script.shared-id","OnCreate",R"({"sessionTest":true})"));
            return nlohmann::json{{"configured",configured_session},{"compiled",compiled_session},
                {"attached",attached_session},{"invoked",invoked_session},
                {"observation",nlohmann::json::parse(session_world.scripting_observation_json())}};
        };
        const auto edit_session=configure_session(edit_world);
        const auto play_session=configure_session(play_world);
        if(!edit_session.at("configured").at("success")||!edit_session.at("compiled").at("success")||
           !edit_session.at("attached").at("success")||!edit_session.at("invoked").at("success")||
           !play_session.at("configured").at("success")||!play_session.at("compiled").at("success")||
           !edit_session.at("compiled").at("cacheHit")||edit_session.at("compiled").at("cacheScope")!="process"||
           !play_session.at("compiled").at("cacheHit")||play_session.at("compiled").at("cacheScope")!="process"||
           !play_session.at("attached").at("success")||!play_session.at("invoked").at("success")||
           edit_session.at("invoked").at("managedResult").at("state").at("CreateCount")!=1||
           play_session.at("invoked").at("managedResult").at("state").at("CreateCount")!=1||
           edit_session.at("observation").at("sessionId")==play_session.at("observation").at("sessionId"))return 25;
    }
    const auto authoring_compile=nlohmann::json::parse(world.scripting_project_compile_json("Debug"));
    const auto authoring_attached=nlohmann::json::parse(world.scripting_attach_json(
        "script.authoring","entity.demo-sphere","asset.script.fixture","ManagedFixture.AuthoringController"));
    const auto authoring_invoked=nlohmann::json::parse(world.scripting_invoke_json(
        "script.authoring","OnUpdate",R"({"deltaSeconds":0.02,"properties":{"rate":2.5}})"));
    const auto authoring_views=world.entity_views();
    const auto authored=std::ranges::find(authoring_views,std::string("entity.demo-sphere"),&noemancer::WorldEntityView::id);
    const auto script_spawned=std::ranges::find(authoring_views,std::string("entity.script-spawned"),&noemancer::WorldEntityView::id);
    const auto authored_velocity=std::ranges::find(authoring_views,std::string("entity.demo-cube"),&noemancer::WorldEntityView::id);
    const auto authored_scene=nlohmann::json::parse(world.canonical_scene_json());
    const auto authored_scene_entity=std::ranges::find_if(authored_scene.at("entities"),[](const auto& item){
        return item.at("guid")=="entity.script-spawned";
    });
    if(!authoring_compile.at("success")||!authoring_attached.at("success")||!authoring_invoked.at("success")||
       authoring_invoked.at("commandApplication").at("applied")!=3||
       !authoring_invoked.at("managedResult").at("state").at("TimerFired")||
       !authoring_invoked.at("managedResult").at("state").at("FoundCube")||
       authoring_invoked.at("managedResult").at("state").at("RenderableCount").get<int>()<2||
       authoring_invoked.at("managedResult").at("state").at("ConfiguredRate")!=2.5F||
       authored==authoring_views.end()||!authored->pbr_material||authored->pbr_material->roughness!=0.25F||
       authored_velocity==authoring_views.end()||!authored_velocity->velocity||
       authored_velocity->velocity->x!=2.0F||authored_velocity->velocity->y!=-1.0F||
       script_spawned==authoring_views.end()||!script_spawned->transform||script_spawned->transform->x!=8.0F||
       authored_scene_entity==authored_scene.at("entities").end()||
       authored_scene_entity->at("components").at("ManagedScript").at("instanceId")!="script.entity.script-spawned")return 26;
    const auto removable_attached=nlohmann::json::parse(world.scripting_attach_json(
        "script.reusable","entity.script-spawned","asset.script.fixture","ManagedFixture.PlayerController"));
    const auto removable_created=nlohmann::json::parse(world.scripting_invoke_json("script.reusable","OnCreate",R"({})"));
    const auto despawn_attached=nlohmann::json::parse(world.scripting_attach_json(
        "script.despawn","entity.demo-sphere","asset.script.fixture","ManagedFixture.DespawnController"));
    const auto despawn_invoked=nlohmann::json::parse(world.scripting_invoke_json(
        "script.despawn","OnUpdate",R"({"properties":{"target":"entity.script-spawned"}})"));
    const auto after_script_despawn=world.entity_views();
    const auto reused_attached=nlohmann::json::parse(world.scripting_attach_json(
        "script.reusable","entity.demo-cube","asset.script.fixture","ManagedFixture.PlayerController"));
    const auto reused_created=nlohmann::json::parse(world.scripting_invoke_json("script.reusable","OnCreate",R"({})"));
    const auto reused_destroyed=nlohmann::json::parse(world.scripting_invoke_json("script.reusable","OnDestroy",R"({})"));
    const auto attached_after_destroy=nlohmann::json::parse(world.scripting_attach_json(
        "script.reusable","entity.demo-cube","asset.script.fixture","ManagedFixture.PlayerController"));
    if(!removable_attached.at("success")||!removable_created.at("success")||
       !despawn_attached.at("success")||!despawn_invoked.at("success")||
       despawn_invoked.at("commandApplication").at("applied")!=1||
       std::ranges::find(after_script_despawn,std::string("entity.script-spawned"),&noemancer::WorldEntityView::id)!=after_script_despawn.end()||
       !reused_attached.at("success")||!reused_created.at("success")||
       reused_created.at("managedResult").at("state").at("CreateCount")!=1||
       !reused_destroyed.at("success")||!attached_after_destroy.at("success"))return 28;
    const auto tag_attached=nlohmann::json::parse(world.scripting_attach_json(
        "script.tags","entity.demo-sphere","asset.script.fixture","ManagedFixture.TagController"));
    const auto tag_created=nlohmann::json::parse(world.scripting_invoke_json("script.tags","OnCreate",R"({})"));
    const auto tag_updated=nlohmann::json::parse(world.scripting_invoke_json("script.tags","OnUpdate",R"({})"));
    const auto tag_state=nlohmann::json::parse(world.gameplay_ability_observation_json("entity.demo-sphere"));
    if(!tag_attached.at("success")||!tag_created.at("success")||tag_created.at("commandApplication").at("applied")!=1||
       !tag_updated.at("success")||!tag_updated.at("managedResult").at("state").at("ObservedTagged")||
       tag_updated.at("commandApplication").at("applied")!=1||tag_state.at("actors").empty()||
       std::ranges::find(tag_state.at("actors").at(0).at("tags"),"state.script-tested")!=tag_state.at("actors").at(0).at("tags").end())return 27;
    const auto before_atomic_views=world.entity_views();
    const auto before_atomic_entity=std::ranges::find(before_atomic_views,std::string("entity.demo-cube"),&noemancer::WorldEntityView::id);
    if(before_atomic_entity==before_atomic_views.end()||!before_atomic_entity->transform)return 29;
    const auto before_atomic_failure=before_atomic_entity->transform.value();
    const auto atomic_attached=nlohmann::json::parse(world.scripting_attach_json(
        "script.atomic-failure","entity.demo-cube","asset.script.fixture","ManagedFixture.AtomicFailureController"));
    const auto atomic_failed=nlohmann::json::parse(world.scripting_invoke_json("script.atomic-failure","OnUpdate",R"({})"));
    const auto after_atomic_views=world.entity_views();
    const auto after_atomic_failure=std::ranges::find(after_atomic_views,std::string("entity.demo-cube"),&noemancer::WorldEntityView::id);
    if(!atomic_attached.at("success")||atomic_failed.at("success")||
       atomic_failed.at("commandApplication").at("applied")!=0||
       atomic_failed.at("commandApplication").at("errors").at(0).at("code")!="gameplay.entity-has-dependents"||
       after_atomic_failure==after_atomic_views.end()||!after_atomic_failure->transform||
       after_atomic_failure->transform->x!=before_atomic_failure.x||after_atomic_failure->transform->y!=before_atomic_failure.y||
       after_atomic_failure->transform->z!=before_atomic_failure.z)return 29;
    const auto invalid_project=std::filesystem::path(NOEMANCER_SOURCE_DIR)/"tests/fixtures/managed-project-invalid/InvalidFixture.csproj";
    const auto invalid_configured=nlohmann::json::parse(world.scripting_project_configure_json(invalid_project.parent_path(),invalid_project));
    const auto rejected_compile=nlohmann::json::parse(world.scripting_project_compile_json("Debug"));
    const auto rejected_observation=nlohmann::json::parse(world.scripting_project_observation_json());
    if(!invalid_configured.at("success")||rejected_compile.at("success")||rejected_compile.at("code")!="scripting.compile-failed"||
       rejected_compile.at("diagnostics").empty()||rejected_compile.at("diagnostics").at(0).at("severity")!="error"||
       rejected_compile.at("diagnostics").at(0).at("file").get<std::string>().find("BrokenScript.cs")==std::string::npos||
       !rejected_observation.at("sourceState").at("needsCompile")||
       rejected_observation.at("sourceState").at("lastAttemptFingerprint").get<std::string>().empty()) return 18;
    const auto ability_grant=nlohmann::json::parse(world.gameplay_ability_grant_json("entity.demo-cube","ability.combat.impact"));
    const auto ability_activate=nlohmann::json::parse(world.gameplay_ability_activate_json("entity.demo-cube","ability.combat.impact","entity.demo-sphere"));
    world.tick(1.0F/60.0F);
    const auto ability_state=nlohmann::json::parse(world.gameplay_ability_observation_json("entity.demo-cube"));
    const auto target_ability_state=nlohmann::json::parse(world.gameplay_ability_observation_json("entity.demo-sphere"));
    const auto animation_state=nlohmann::json::parse(world.animation_observation_json());
    const auto machine_state=nlohmann::json::parse(world.animation_state_machine_json("entity.demo-skeletal-cube"));
    const auto vfx_state=nlohmann::json::parse(world.vfx_observation_json(1));
    if(!ability_grant.at("success")||!ability_activate.at("success")||ability_activate.at("eventType")!="combat.hit"||
       ability_activate.at("effects").size()!=2||ability_state.at("actors").at(0).at("attributes").at("stamina").get<float>()<=85.0F||
       target_ability_state.at("actors").at(0).at("attributes").at("health")!=80.0F||
       target_ability_state.at("activeEffects").size()!=1||vfx_state.at("aliveCount").get<int>()<48||
       animation_state.at("gameplayCues").empty()||animation_state.at("gameplayCues").at(0).at("cue")!="hit-react"||
       !machine_state.at("valid")||machine_state.at("definition").at("states").size()!=4||
       vfx_state.at("lastEventSequence")!=ability_activate.at("effects").back().at("eventSequence")) return 11;
    const auto parameter=nlohmann::json::parse(world.animation_state_parameter_set_json("entity.demo-skeletal-cube","speed",1.0F));
    world.tick(1.0F/60.0F);
    const auto locomotion=nlohmann::json::parse(world.animation_state_machine_json("entity.demo-skeletal-cube"));
    if(!parameter.at("success")||locomotion.at("instance").at("activeState")!="locomotion") return 13;
    world.tick(0.40F);
    const auto expired_state=nlohmann::json::parse(world.gameplay_ability_observation_json("entity.demo-sphere"));
    if(!expired_state.at("activeEffects").empty()||
       std::ranges::find(expired_state.at("actors").at(0).at("tags"),"state.hit-react")!=
           expired_state.at("actors").at(0).at("tags").end()) return 12;
    if(!nlohmann::json::parse(world.gameplay_ability_grant_json("entity.demo-sphere","ability.combat.ignite")).at("success")) return 14;
    const auto swept=nlohmann::json::parse(world.gameplay_ability_activate_sweep_json("entity.demo-sphere","ability.combat.ignite",
        {0.0F,3.0F,0.0F},{0.0F,-5.0F,0.0F},0.2F));
    if(!swept.at("success")||swept.at("hit").at("entityId")!="entity.demo-cube"||
       swept.at("activation").at("abilityId")!="ability.combat.ignite") return 15;
    world.tick(0.5F);
    const auto burned=nlohmann::json::parse(world.gameplay_ability_observation_json("entity.demo-cube"));
    if(burned.at("actors").at(0).at("attributes").at("health")!=97.0F||
       burned.at("activeEffects").at(0).at("stackCount")!=1) return 16;
    return 0;
}
