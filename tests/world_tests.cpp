#include "engine/command_registry.hpp"
#include "engine/asset_registry.hpp"
#include "engine/process_diagnostics.hpp"
#include "engine/semantic_state.hpp"
#include "engine/world.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>

int main() {
    noemancer::configure_process_diagnostics("test.world");
    try {
    noemancer::World world;
    const auto load = world.load_scene(noemancer::make_bootstrap_scene_document());
    if (!load.success || world.entity_count() != 10) {
        std::cerr << "Expected the bootstrap render scene to instantiate ten entities\n";
        return 1;
    }

    world.tick(1.0F / 60.0F);
    if (world.revision() != 2) {
        std::cerr << "World revision did not advance with semantic changes\n";
        return 2;
    }
    const auto snapshot = world.snapshot_json();
    if (snapshot.find("entity.demo-cube") == std::string::npos ||
        snapshot.find("entity.bootstrap-root") == std::string::npos ||
        snapshot.find("asset://scenes/bootstrap.scene.json") == std::string::npos ||
        snapshot.find(R"("parentGuid":"entity.bootstrap-root")") == std::string::npos ||
        snapshot.find(R"("unit":"m/s")") == std::string::npos ||
        snapshot.find("world.right-handed.y-up") == std::string::npos ||
        snapshot.find(R"("revision":2)") == std::string::npos) {
        std::cerr << "Snapshot is missing stable semantic data\n";
        return 3;
    }

    const auto views = world.entity_views();
    const auto demo_view = std::ranges::find(views, "entity.demo-cube", &noemancer::WorldEntityView::id);
    const auto camera_view = std::ranges::find(views, "entity.camera.editor", &noemancer::WorldEntityView::id);
    if (views.size() != 10 || views[0].id != "entity.bootstrap-root" ||
        demo_view == views.end() || !demo_view->transform || !demo_view->mesh_renderer ||
        camera_view == views.end() || !camera_view->camera) {
        std::cerr << "World entity view is not stable or complete\n";
        return 4;
    }
    const auto stale_update = world.update_transform(
        "entity.demo-cube",
        {1.0F, 2.0F, 3.0F},
        world.revision() - 1);
    if (stale_update.success || stale_update.code != "world.revision-conflict") {
        std::cerr << "Stale World mutation was not rejected\n";
        return 5;
    }
    const auto update = world.update_transform(
        "entity.demo-cube",
        {1.0F, 2.0F, 3.0F},
        world.revision());
    if (!update.success || update.revision != 3) {
        std::cerr << "Revision-bound World mutation did not commit\n";
        return 6;
    }

    const auto plan = world.plan_transform_update(
        "entity.demo-cube", {4.0F, 5.0F, 6.0F}, world.revision(), "test.agent");
    if (!plan.valid || plan.content_hash.empty() ||
        noemancer::World::change_plan_json(plan).find("predictedDelta") == std::string::npos) {
        std::cerr << "Transform plan was not deterministic and inspectable\n";
        return 14;
    }
    const auto dry_run = world.apply_transform_plan(plan, true);
    if (!dry_run.success || !dry_run.dry_run || world.revision() != 3) {
        std::cerr << "Dry-run mutated World state\n";
        return 15;
    }
    const auto applied = world.apply_transform_plan(plan, false);
    if (!applied.success || applied.revision_after != 4 || !world.can_undo()) {
        std::cerr << "Change plan did not commit atomically\n";
        return 16;
    }
    noemancer::ObservationQuery query{
        .entity_ids = {"entity.bootstrap-root"},
        .fields = {"identity", "hierarchy", "transform"},
        .depth = 1,
        .byte_budget = 4096
    };
    const auto observation = world.observe_json(query);
    if (observation.find("entity.demo-cube") == std::string::npos ||
        observation.find("asset://scenes") != std::string::npos) {
        std::cerr << "Focused observation did not honor scope or field mask\n";
        return 17;
    }
    const auto delta = world.delta_json(3);
    if (delta.find("engine.entity.transform.position") == std::string::npos ||
        delta.find(R"("resyncRequired":false)") == std::string::npos) {
        std::cerr << "Semantic delta did not describe the committed field change\n";
        return 18;
    }
    const auto undone = world.undo(world.revision(), "test.undo");
    const auto redone = world.redo(world.revision(), "test.redo");
    if (!undone.success || !redone.success || world.revision() != 6 || !world.can_undo()) {
        std::cerr << "Transaction undo/redo history is inconsistent\n";
        return 19;
    }
    const auto property_plan=world.plan_property_update("entity.demo-cube","engine.entity.material.roughness","0.25",
        world.revision(),"test.inspector");
    const auto property_dry=world.apply_property_plan(property_plan,true);
    const auto property_applied=world.apply_property_plan(property_plan,false);
    const auto property_undone=world.undo(world.revision(),"test.property-undo");
    const auto property_redone=world.redo(world.revision(),"test.property-redo");
    const auto inspector=world.inspector_document_json("entity.demo-cube");
    const auto property_scene=nlohmann::json::parse(world.canonical_scene_json());
    const auto persisted_property=std::ranges::find_if(property_scene.at("entities"),[](const auto& entity){return entity.at("guid")=="entity.demo-cube";});
    if(!property_plan.valid||!property_dry.success||!property_applied.success||!property_undone.success||!property_redone.success||
        inspector.find("engine.entity.material.roughness")==std::string::npos||inspector.find("world.property.plan")==std::string::npos||
        persisted_property==property_scene.at("entities").end()||persisted_property->at("components").at("PbrMaterial").at("roughness")!=0.25) {
        std::cerr<<"Declarative Inspector property transaction did not plan, apply, undo/redo, and persist\n";
        return 26;
    }
    const auto exported_scene = nlohmann::json::parse(world.canonical_scene_json());
    const auto& exported_position = exported_scene.at("entities").at(2).at("components").at("Transform").at("position");
    if (exported_position.at(0) != 4.0 || exported_position.at(1) != 5.0 || exported_position.at(2) != 6.0) {
        std::cerr << "Committed Transform was not reflected into the canonical scene document\n";
        return 20;
    }

    const noemancer::SemanticConventionRegistry conventions;
    if (conventions.conventions().size() < 8 ||
        conventions.schema_json().find("engine.entity.transform.position.x") == std::string::npos ||
        conventions.schema_json().find("engine.entity.transform.rotationQuaternion") == std::string::npos) {
        std::cerr << "Core semantic conventions are incomplete\n";
        return 7;
    }

    const auto binding_path=std::filesystem::path(NOEMANCER_SOURCE_DIR)/"managed/Noemancer.Managed/EngineSchema.g.cs";
    std::ifstream binding_stream(binding_path,std::ios::binary);
    if(!binding_stream) {
        std::cerr<<"Checked-in C# Schema Binding is missing\n";
        return 27;
    }
    std::string checked_in_binding{std::istreambuf_iterator<char>(binding_stream),std::istreambuf_iterator<char>()};
    checked_in_binding.erase(std::remove(checked_in_binding.begin(),checked_in_binding.end(),'\r'),checked_in_binding.end());
    if(checked_in_binding!=world.managed_bindings_source()) {
        std::cerr<<"Checked-in C# Schema Binding is stale; regenerate it with `noemancer bindings csharp`\n";
        return 27;
    }

    const noemancer::CommandRegistry registry;
    const auto manifest = registry.manifest_json();
    if (manifest.find("world.snapshot") == std::string::npos ||
        manifest.find("asset.registry") == std::string::npos ||
        manifest.find("asset.cook.plan") == std::string::npos ||
        manifest.find("asset.tilemap.stroke") == std::string::npos ||
        manifest.find("asset.tilemap.region") == std::string::npos ||
        manifest.find("asset.source.undo") == std::string::npos ||
        manifest.find("asset.source.redo") == std::string::npos ||
        manifest.find("world.query") == std::string::npos ||
        manifest.find("render.observe") == std::string::npos ||
        manifest.find("physics.observe") == std::string::npos ||
        manifest.find("physics.constraint.observe") == std::string::npos ||
        manifest.find("physics.constraint.edit") == std::string::npos ||
        manifest.find("physics.ray-cast") == std::string::npos ||
        manifest.find("physics.sphere-sweep") == std::string::npos ||
        manifest.find("animation.observe") == std::string::npos ||
        manifest.find("render.sprite.observe") == std::string::npos ||
        manifest.find("animation.state-machine.inspect") == std::string::npos ||
        manifest.find("animation.state-machine.parameter.set") == std::string::npos ||
        manifest.find("animation.graph.instance.observe") == std::string::npos ||
        manifest.find("animation.graph.parameter.set") == std::string::npos ||
        manifest.find("animation.graph.inspect") == std::string::npos ||
        manifest.find("animation.graph.patch") == std::string::npos ||
        manifest.find("animation.skeleton.inspect") == std::string::npos ||
        manifest.find("input.actions.observe") == std::string::npos ||
        manifest.find("input.source.inject") == std::string::npos ||
        manifest.find("audio.clip.load") == std::string::npos ||
        manifest.find("audio.listener.set") == std::string::npos ||
        manifest.find("audio.mixer.observe") == std::string::npos ||
        manifest.find("audio.bus.set") == std::string::npos ||
        manifest.find("audio.voice.play") == std::string::npos ||
        manifest.find("audio.voice.spatial.set") == std::string::npos ||
        manifest.find("gameplay.events.observe") == std::string::npos ||
        manifest.find("scripting.debug.attach-manifest") == std::string::npos ||
        manifest.find("gameplay.ability.catalog") == std::string::npos ||
        manifest.find("gameplay.ability.grant") == std::string::npos ||
        manifest.find("gameplay.ability.activate") == std::string::npos ||
        manifest.find("gameplay.ability.activate-ray") == std::string::npos ||
        manifest.find("gameplay.ability.activate-sweep") == std::string::npos ||
        manifest.find("gameplay.ability.observe") == std::string::npos ||
        manifest.find("gameplay.effect.catalog") == std::string::npos ||
        manifest.find("gameplay.effect.apply") == std::string::npos ||
        manifest.find("network.profile.describe") == std::string::npos ||
        manifest.find("network.snapshot.preview") == std::string::npos ||
        manifest.find("network.loopback.verify") == std::string::npos ||
        manifest.find("network.transport.verify") == std::string::npos ||
        manifest.find("vfx.graph.inspect") == std::string::npos ||
        manifest.find("vfx.benchmark") == std::string::npos ||
        manifest.find("vfx.gpu-program.inspect") == std::string::npos ||
        manifest.find("vfx.graph.patch.plan") == std::string::npos ||
        manifest.find("vfx.graph.patch.apply") == std::string::npos ||
        manifest.find("vfx.graph.undo") == std::string::npos ||
        manifest.find("vfx.observe") == std::string::npos ||
        manifest.find("vfx.preview") == std::string::npos ||
        manifest.find("vfx.spawn") == std::string::npos ||
        manifest.find("gameplay.prefab.spawn") == std::string::npos ||
        manifest.find("gameplay.save.capture") == std::string::npos ||
        manifest.find("gameplay.replay.apply") == std::string::npos ||
        manifest.find("gameplay.prefab.export") == std::string::npos ||
        manifest.find("gameplay.prefab.instantiate") == std::string::npos ||
        manifest.find("scripting.abi.describe") == std::string::npos ||
        manifest.find("scripting.lifecycle.invoke") == std::string::npos ||
        manifest.find("scripting.project.compile") == std::string::npos ||
        manifest.find("scripting.debug.session.start") == std::string::npos ||
        manifest.find("scripting.debug.session.status") == std::string::npos ||
        manifest.find("scripting.debug.session.request") == std::string::npos ||
        manifest.find("scripting.debug.session.events") == std::string::npos ||
        manifest.find("scripting.debug.session.stop") == std::string::npos ||
        manifest.find("editor.inspector.describe") == std::string::npos ||
        manifest.find("ui.observe") == std::string::npos ||
        manifest.find("ui.delta") == std::string::npos ||
        manifest.find("ui.retained.preview") == std::string::npos ||
        manifest.find("ui.resources.inspect") == std::string::npos ||
        manifest.find("ui.text.inspect") == std::string::npos ||
        manifest.find("world.property.plan") == std::string::npos ||
        manifest.find("render.graph.inspect") == std::string::npos ||
        manifest.find("world.change.apply") == std::string::npos ||
        manifest.find("world.undo") == std::string::npos ||
        manifest.find("scene.validate") == std::string::npos ||
        manifest.find("run.headless") == std::string::npos ||
        manifest.find("engine.status") == std::string::npos) {
        std::cerr << "Command manifest is missing registered tools\n";
        return 5;
    }

    const auto physics_observation = registry.invoke("physics.observe", "{}");
    const auto animation_observation = registry.invoke("animation.observe", "{}");
    const auto sprite_observation = registry.invoke("render.sprite.observe", "{}");
    const auto animation_parameter = registry.invoke("animation.state-machine.parameter.set",
        R"({"entityId":"entity.demo-skeletal-cube","parameter":"speed","value":1.0})");
    const auto animation_machine = registry.invoke("animation.state-machine.inspect",
        R"({"entityId":"entity.demo-skeletal-cube"})");
    const auto input_observation = registry.invoke("input.actions.observe", "{}");
    const auto audio_observation = registry.invoke("audio.mixer.observe", "{}");
    const auto audio_play = registry.invoke("audio.voice.play", R"({"assetId":"asset.audio.test"})");
    const auto audio_spatial = registry.invoke("audio.voice.spatial.set", R"({"voiceId":1,"position":{"x":4,"y":0,"z":0}})");
    const auto missing_audio_clip = registry.invoke("audio.clip.load", R"({"assetId":"asset.audio.missing"})");
    const auto gameplay_observation = registry.invoke("gameplay.events.observe", R"({"maxEvents":8})");
    const auto debug_session_status = registry.invoke("scripting.debug.session.status", "{}");
    const auto debug_session_events = registry.invoke("scripting.debug.session.events", "{}");
    const auto debug_session_rejected = registry.invoke("scripting.debug.session.request",
        R"({"command":"evaluate","arguments":{}})");
    const auto ability_grant = registry.invoke("gameplay.ability.grant", R"({"entityId":"entity.demo-cube","abilityId":"ability.combat.impact"})");
    const auto ability_activate = registry.invoke("gameplay.ability.activate", R"({"entityId":"entity.demo-cube","abilityId":"ability.combat.impact","targetId":"entity.demo-sphere"})");
    const auto ability_observe = registry.invoke("gameplay.ability.observe", R"({"entityId":"entity.demo-cube"})");
    const auto effect_catalog = registry.invoke("gameplay.effect.catalog", "{}");
    const auto effect_apply = registry.invoke("gameplay.effect.apply", R"({"sourceEntityId":"entity.demo-cube","targetEntityId":"entity.demo-sphere","effectId":"effect.recovery.minor"})");
    const auto ray_ability_grant = registry.invoke("gameplay.ability.grant", R"({"entityId":"entity.demo-sphere","abilityId":"ability.combat.impact"})");
    const auto ray_ability_activate = registry.invoke("gameplay.ability.activate-ray",
        R"({"entityId":"entity.demo-sphere","abilityId":"ability.combat.impact","origin":{"x":0,"y":3,"z":0},"direction":{"x":0,"y":-5,"z":0}})");
    const auto network_profile = registry.invoke("network.profile.describe", "{}");
    const auto network_snapshot = registry.invoke("network.snapshot.preview", R"({"tick":42,"maxEntities":2})");
    const auto network_loopback = registry.invoke("network.loopback.verify", "{}");
    const auto network_transport = registry.invoke("network.transport.verify", R"({"payloadBytes":384})");
    const auto vfx_graph = registry.invoke("vfx.graph.inspect", "{}");
    const auto vfx_benchmark = registry.invoke("vfx.benchmark",R"({"particleCount":1024,"steps":4})");
    const auto vfx_gpu_program = registry.invoke("vfx.gpu-program.inspect", "{}");
    const auto vfx_preview = registry.invoke("vfx.preview", R"({"seed":42,"steps":20,"maxParticles":2})");
    const auto vfx_preview_repeat = registry.invoke("vfx.preview", R"({"seed":42,"steps":20,"maxParticles":2})");
    const auto vfx_observation = registry.invoke("vfx.observe", R"({"maxParticles":2})");
    const auto vfx_spawn = registry.invoke("vfx.spawn", R"({"position":{"x":2.5,"y":2.2,"z":1},"seed":42})");
    const auto skeleton_observation = registry.invoke("animation.skeleton.inspect",
        R"({"entityId":"entity.demo-skeletal-cube","maxJoints":1})");
    const auto ray_observation = registry.invoke("physics.ray-cast",
        R"({"origin":{"x":0,"y":3,"z":0},"direction":{"x":0,"y":-5,"z":0}})");
    const auto sweep_observation = registry.invoke("physics.sphere-sweep",
        R"({"origin":{"x":0,"y":3,"z":0},"direction":{"x":0,"y":-5,"z":0},"radius":0.2})");
    if (physics_observation.exit_code != 0 || physics_observation.output_json.find("entity.demo-cube") == std::string::npos ||
        physics_observation.output_json.find("entity.demo-sphere") == std::string::npos ||
        physics_observation.output_json.find("sphereCollider") == std::string::npos ||
        physics_observation.output_json.find("jolt/5.6.0") == std::string::npos ||
        animation_observation.exit_code != 0 || animation_observation.output_json.find("asset.animation.test-bob") == std::string::npos ||
        sprite_observation.exit_code != 0 || sprite_observation.output_json.find("noemancer.sprite-world-observation/0.1") == std::string::npos ||
        animation_parameter.exit_code != 0 || animation_parameter.output_json.find(R"("success":true)") == std::string::npos ||
        animation_machine.exit_code != 0 || animation_machine.output_json.find("animation.machine.basic-locomotion") == std::string::npos ||
        input_observation.exit_code != 0 || input_observation.output_json.find("gameplay.jump") == std::string::npos ||
        audio_observation.exit_code != 0 || audio_observation.output_json.find("audio.master") == std::string::npos ||
        audio_play.exit_code != 0 || audio_spatial.exit_code != 0 || audio_spatial.output_json.find("\"success\":true") == std::string::npos ||
        missing_audio_clip.exit_code != 0 || missing_audio_clip.output_json.find("audio.asset-unavailable") == std::string::npos ||
        gameplay_observation.exit_code != 0 || gameplay_observation.output_json.find("noemancer.gameplay-events/0.1") == std::string::npos ||
        debug_session_status.exit_code == 0 || debug_session_status.output_json.find("scripting.debug-session-not-started") == std::string::npos ||
        debug_session_events.exit_code == 0 || debug_session_events.output_json.find("scripting.debug-session-not-started") == std::string::npos ||
        debug_session_rejected.exit_code == 0 || debug_session_rejected.output_json.find("scripting.debug-command-not-allowed") == std::string::npos ||
        ability_grant.exit_code != 0 || ability_activate.exit_code != 0 || ability_activate.output_json.find("combat.hit") == std::string::npos ||
        ability_observe.exit_code != 0 || ability_observe.output_json.find("ability.combat.impact") == std::string::npos ||
        effect_catalog.exit_code != 0 || effect_catalog.output_json.find("effect.damage.impact") == std::string::npos ||
        effect_apply.exit_code != 0 || effect_apply.output_json.find(R"("after":100.0)") == std::string::npos ||
        ray_ability_grant.exit_code != 0 || ray_ability_activate.exit_code != 0 ||
        ray_ability_activate.output_json.find("entity.demo-cube") == std::string::npos ||
        ray_ability_activate.output_json.find(R"("success":true)") == std::string::npos ||
        network_profile.exit_code != 0 || network_profile.output_json.find("network.optional-authoritative") == std::string::npos ||
        network_snapshot.exit_code != 0 || network_snapshot.output_json.find("noemancer.network-snapshot/0.1") == std::string::npos ||
        network_snapshot.output_json.find(R"("truncated":true)") == std::string::npos ||
        network_loopback.exit_code != 0 || network_loopback.output_json.find(R"("converged":true)") == std::string::npos ||
        network_loopback.output_json.find(R"("predictionAccepted":true)") == std::string::npos ||
        network_transport.exit_code != 0 || network_transport.output_json.find(R"("kernelSocket":true)") == std::string::npos ||
        vfx_graph.exit_code != 0 || vfx_graph.output_json.find("noemancer.vfx-graph/0.1") == std::string::npos ||
        vfx_graph.output_json.find("gpu-compute") == std::string::npos ||
        vfx_benchmark.exit_code != 0 || vfx_benchmark.output_json.find("structure-of-arrays/0.1") == std::string::npos ||
        vfx_gpu_program.exit_code != 0 || vfx_gpu_program.output_json.find("vfx_sim.comp") == std::string::npos ||
        vfx_gpu_program.output_json.find(R"("dispatchActive":false)") == std::string::npos ||
        vfx_preview.exit_code != 0 || vfx_preview_repeat.exit_code != 0 ||
        nlohmann::json::parse(vfx_preview.output_json).at("result").at("digest") !=
            nlohmann::json::parse(vfx_preview_repeat.output_json).at("result").at("digest") ||
        vfx_preview.output_json.find("cpu-deterministic-reference") == std::string::npos ||
        vfx_observation.exit_code != 0 || vfx_observation.output_json.find("soa-pool-data-channel-indirect/0.1") == std::string::npos ||
        vfx_spawn.exit_code != 0 || vfx_spawn.output_json.find(R"("spawned":48)") == std::string::npos ||
        animation_observation.output_json.find("rootMotionMode") == std::string::npos ||
        skeleton_observation.exit_code != 0 || skeleton_observation.output_json.find(R"("returnedJointCount":1)") == std::string::npos ||
        skeleton_observation.output_json.find(R"("truncated":true)") == std::string::npos ||
        ray_observation.exit_code != 0 || ray_observation.output_json.find("entity.demo-cube") == std::string::npos ||
        sweep_observation.exit_code != 0 || sweep_observation.output_json.find("entity.demo-cube") == std::string::npos ||
        sweep_observation.output_json.find("penetrationDepth") == std::string::npos) {
        std::cerr << "Simulation observations are not available through the shared command surface\n";
        return 25;
    }
    const auto inspector_observation=registry.invoke("editor.inspector.describe",R"({"entityId":"entity.demo-cube"})");
    if(inspector_observation.exit_code!=0||inspector_observation.output_json.find("editor.panel.inspector")==std::string::npos||
        inspector_observation.output_json.find("engine.entity.material.baseColor")==std::string::npos) {
        std::cerr<<"Declarative Inspector document is not available through the shared command surface\n";
        return 27;
    }
    const auto ui_observation=registry.invoke("ui.observe",R"({"entityId":"entity.demo-cube","roles":["property"],"depth":0,"includeValues":false})");
    if(ui_observation.exit_code!=0||ui_observation.output_json.find("noemancer.ui-observation/0.1")==std::string::npos||
        ui_observation.output_json.find("engine.entity.material.roughness")==std::string::npos||
        ui_observation.output_json.find("world.property.plan")==std::string::npos) {
        std::cerr<<"Bounded generic Semantic UI observation is not available through the shared command surface\n";
        return 29;
    }
    const auto ui_delta=registry.invoke("ui.delta",R"({"entityId":"entity.demo-cube","sinceRevision":999})");
    if(ui_delta.exit_code!=0||ui_delta.output_json.find("noemancer.ui-delta/0.1")==std::string::npos||
        ui_delta.output_json.find("ui.resync-required")==std::string::npos||
        ui_delta.output_json.find("ui.observe")==std::string::npos) {
        std::cerr<<"Revision-bound Semantic UI delta is not available through the shared command surface\n";
        return 30;
    }
    if(!world.configure_project_hud(R"({"schemaVersion":"noemancer.ui-document/0.1","documentId":"project.hud","nodes":[{"id":"project.hud","parentId":null,"role":"hud","label":"Project"},{"id":"project.hud.move","parentId":"project.hud","role":"property","label":"Move","value":0,"binding":{"kind":"input-action","actionId":"gameplay.move.x","field":"value","fallback":0},"state":{"visible":true,"enabled":true,"editable":false}}]})"))return 301;
    static_cast<void>(world.inject_input_json("keyboard.d",1.0F));
    const noemancer::CommandRegistry project_registry(world);
    const auto project_ui=project_registry.invoke("ui.project.observe",R"({"roles":["property"],"includeValues":true})");
    if(project_ui.exit_code!=0||project_ui.output_json.find("noemancer.ui-observation/0.1")==std::string::npos||
        project_ui.output_json.find("project.hud.move")==std::string::npos||project_ui.output_json.find(R"("value":1.0)")==std::string::npos) {
        std::cerr<<"Runtime-bound project HUD is not available through the shared command surface: "<<project_ui.output_json<<"\n";return 302;
    }
    const auto ui_text=registry.invoke("ui.text.inspect",R"({"locale":"ar-SA","text":"\u0627\u0644\u0633\u0644\u0627\u0645","fontSize":20})");
    if(ui_text.exit_code!=0||ui_text.output_json.find("noemancer.ui-text-capabilities/0.1")==std::string::npos||
        ui_text.output_json.find(R"("requiredScript":"Arabic")")==std::string::npos||
        ui_text.output_json.find(R"("committedUtf8":true)")==std::string::npos||
        ui_text.output_json.find(R"("harfBuzz":true)")==std::string::npos||
        ui_text.output_json.find(R"("bidirectionalLayout":true)")==std::string::npos||
        ui_text.output_json.find(R"("layoutPlan")")==std::string::npos||
        ui_text.output_json.find(R"("baseDirection":"rtl")")==std::string::npos) {
        std::cerr<<"International text capability boundary is not available through the shared command surface\n";
        return 31;
    }
    const auto retained_ui=registry.invoke("ui.retained.preview",R"({"entityId":"entity.demo-cube","width":800,"height":600,"densityScale":1.25})");
    if(retained_ui.exit_code!=0||retained_ui.output_json.find("noemancer.retained-ui-preview/0.1")==std::string::npos||
        retained_ui.output_json.find("RmlUi")==std::string::npos||retained_ui.output_json.find("engine.entity.material.roughness")==std::string::npos) {
        std::cerr<<"RmlUi retained layout preview is not available through the shared command surface\n";
        return 30;
    }

    const auto graph_observation = registry.invoke("render.graph.inspect", "{}");
    if (graph_observation.exit_code != 0 ||
        graph_observation.output_json.find("render.pass.shadow-depth") == std::string::npos ||
        graph_observation.output_json.find(R"("valid":true)") == std::string::npos) {
        std::cerr << "Render graph is not available through the shared command surface\n";
        return 24;
    }

    const auto render_observation = registry.invoke("render.observe", "{}");
    if (render_observation.exit_code != 0 ||
        render_observation.output_json.find("entity.camera.editor") == std::string::npos ||
        render_observation.output_json.find("asset.primitive.cube") == std::string::npos ||
        render_observation.output_json.find("entity.sun") == std::string::npos) {
        std::cerr << "Render observation did not expose the ECS render scene\n";
        return 23;
    }

    const auto convention_invocation = registry.invoke("semantic.conventions", "{}");
    if (convention_invocation.exit_code != 0 ||
        convention_invocation.output_json.find("noemancer.semantic-conventions.core") == std::string::npos) {
        std::cerr << "Semantic conventions are not available through the shared registry\n";
        return 6;
    }

    const auto asset_invocation = registry.invoke(
        "asset.query",
        R"({"tags":["gltf"],"limit":2})");
    if (asset_invocation.exit_code != 0 ||
        asset_invocation.output_json.find(R"("total":4)") == std::string::npos ||
        asset_invocation.output_json.find("asset.test.kenney.alien") == std::string::npos) {
        std::cerr << "Asset Registry is not available through the shared command surface\n";
        return 22;
    }

    const auto engine_status = registry.invoke("engine.status", "{}");
    if (engine_status.exit_code != 0 ||
        engine_status.output_json.find(R"("moduleCount":25)") == std::string::npos ||
        engine_status.output_json.find("render.graph") == std::string::npos) {
        std::cerr << "Engine module graph is not available through the shared registry\n";
        return 7;
    }

    const auto invocation = registry.invoke("run.headless", R"({"frames":5})");
    if (invocation.exit_code != 0 ||
        invocation.output_json.find(R"("frames":5)") == std::string::npos ||
        invocation.output_json.find(R"("receipt")") == std::string::npos) {
        std::cerr << "Direct tool invocation did not return a receipt\n";
        return 8;
    }

    const auto invalid = registry.invoke("run.headless", R"({"frames":0})");
    if (invalid.exit_code == 0 ||
        invalid.output_json.find("invalid_arguments") == std::string::npos) {
        std::cerr << "Invalid tool input did not return a stable error\n";
        return 9;
    }

    const auto wrong_type = registry.invoke("run.headless", R"({"frames":"five"})");
    if (wrong_type.exit_code == 0 ||
        wrong_type.output_json.find("frames must be an integer") == std::string::npos) {
        std::cerr << "Tool input type was not validated\n";
        return 10;
    }

    const auto unknown_argument = registry.invoke("schema.get", R"({"unexpected":true})");
    if (unknown_argument.exit_code == 0 ||
        unknown_argument.output_json.find("Unknown argument") == std::string::npos) {
        std::cerr << "Additional properties were not rejected\n";
        return 11;
    }

    const auto unknown = registry.invoke("missing.tool", "{}");
    if (unknown.exit_code == 0 ||
        unknown.output_json.find("unknown_tool") == std::string::npos) {
        std::cerr << "Unknown tool did not return a stable error\n";
        return 12;
    }

    const auto scene_validation = registry.invoke(
        "scene.validate",
        R"({"document":{"schema":"noemancer.scene/0.1","sceneGuid":"scene.test","name":"Test","entities":[]}})");
    if (scene_validation.exit_code != 0 ||
        scene_validation.output_json.find(R"("valid":true)") == std::string::npos ||
        scene_validation.output_json.find("canonicalDocument") == std::string::npos) {
        std::cerr << "Scene validation is not available through the shared registry\n";
        return 13;
    }

    const auto registry_snapshot = registry.invoke("world.snapshot", "{}");
    const auto snapshot_envelope = nlohmann::json::parse(registry_snapshot.output_json);
    const auto base_revision = snapshot_envelope.at("result").at("revision").get<std::uint64_t>();
    const auto scene_edit_dry_run = registry.invoke("scene.entity.edit", nlohmann::json{
        {"operation","create"},{"newEntityId","entity.registry-dry-run"},{"displayName","Dry Run Entity"},
        {"baseRevision",base_revision},{"manager","test.registry"},{"dryRun",true}}.dump());
    const auto scene_edit_envelope = nlohmann::json::parse(scene_edit_dry_run.output_json);
    if (scene_edit_dry_run.exit_code != 0 || !scene_edit_envelope.at("result").value("success",false) ||
        world.entity_count() != 10) {
        std::cerr << "Scene entity edit dry run mutated the shared World\n";
        return 30;
    }
    const auto transform_edit_dry_run=registry.invoke("scene.transform.edit",nlohmann::json{
        {"entityId","entity.demo-cube"},{"position",{{"x",0.0},{"y",1.05},{"z",0.0}}},
        {"rotationQuaternion",{{"x",0.0},{"y",0.38268343},{"z",0.0},{"w",0.92387953}}},
        {"scale",{{"x",1.0},{"y",1.0},{"z",1.0}}},{"baseRevision",base_revision},{"manager","test.transform"},{"dryRun",true}}.dump());
    const auto transform_edit_envelope=nlohmann::json::parse(transform_edit_dry_run.output_json);
    if(transform_edit_dry_run.exit_code!=0||!transform_edit_envelope.at("result").value("success",false)||
        world.canonical_scene_json().find("rotationEulerDegrees")!=std::string::npos) {
        std::cerr<<"Atomic transform dry run mutated the canonical scene\n";return 31;
    }
    const auto registry_plan = registry.invoke(
        "world.transform.plan",
        nlohmann::json{
            {"entityId", "entity.demo-cube"},
            {"baseRevision", base_revision},
            {"manager", "test.registry"},
            {"position", {{"x", 8.0}, {"y", 9.0}, {"z", 10.0}}}
        }.dump());
    const auto plan_envelope = nlohmann::json::parse(registry_plan.output_json);
    const auto registry_apply = registry.invoke(
        "world.change.apply",
        nlohmann::json{{"plan", plan_envelope.at("result")}, {"dryRun", false}}.dump());
    const auto apply_envelope = nlohmann::json::parse(registry_apply.output_json);
    if (registry_apply.exit_code != 0 || !apply_envelope.at("result").at("success").get<bool>() ||
        apply_envelope.at("receipt").at("changedObjects").size() != 1) {
        std::cerr << "Shared registry did not preserve a transactional World session\n";
        return 21;
    }
    const auto registry_property_plan=registry.invoke("world.property.plan",nlohmann::json{{"entityId","entity.demo-cube"},
        {"property","engine.entity.material.metallic"},{"value",0.65},{"baseRevision",apply_envelope.at("result").at("revisionAfter")},
        {"manager","test.registry-property"}}.dump());
    const auto registry_property_envelope=nlohmann::json::parse(registry_property_plan.output_json);
    const auto registry_property_apply=registry.invoke("world.change.apply",nlohmann::json{{"plan",registry_property_envelope.at("result")},{"dryRun",false}}.dump());
    const auto registry_property_result=nlohmann::json::parse(registry_property_apply.output_json);
    if(registry_property_apply.exit_code!=0||!registry_property_result.at("result").at("success").get<bool>()||
        registry.invoke("editor.inspector.describe",R"({"entityId":"entity.demo-cube"})").output_json.find("engine.entity.material.metallic")==std::string::npos) {
        std::cerr<<"Shared registry did not route generic Inspector property plans through world.change.apply\n";
        return 28;
    }
    const auto tile_tool_root=std::filesystem::temp_directory_path()/"noemancer-tilemap-stroke-world-test";
    std::filesystem::remove_all(tile_tool_root);std::filesystem::create_directories(tile_tool_root);
    {
        std::ofstream output(tile_tool_root/"registry.json");
        output<<R"({"schema":"noemancer.assets/0.1","assets":[
          {"id":"palette.tool","displayName":"Tool Palette","kind":"TilePalette","uri":"asset://tool.tile-palette.json","path":"tool.tile-palette.json","license":"CC0","redistribution":"allowed"},
          {"id":"tilemap.tool","displayName":"Tool Map","kind":"Tilemap","uri":"asset://tool.tilemap.json","path":"tool.tilemap.json","license":"CC0","redistribution":"allowed"}]})";
    }
    {
        std::ofstream output(tile_tool_root/"tool.tile-palette.json");
        output<<R"({"schema":"noemancer.tile-palette/0.1","assetId":"palette.tool","spriteAsset":"sprite.tool","tiles":[{"id":"ground","frame":"ground.0","collision":"solid","tags":[]}]})";
    }
    {
        std::ofstream output(tile_tool_root/"tool.tilemap.json");
        output<<R"({"schema":"noemancer.tilemap/0.1","assetId":"tilemap.tool","paletteAsset":"palette.tool","cellSize":[1,1],"chunkSize":[8,8],"layers":[{"id":"ground","sortingLayer":"terrain","sortingOrder":0,"collisionEnabled":true,"chunks":[]}]})";
    }
    noemancer::AssetRegistry tile_assets(tile_tool_root);noemancer::World tile_world;noemancer::CommandRegistry tile_registry(tile_world,tile_assets);
    const auto dry_stroke=tile_registry.invoke("asset.tilemap.stroke",nlohmann::json{{"assetId","tilemap.tool"},{"layerId","ground"},
        {"manager","test.tile-brush"},{"edits",nlohmann::json::array({{{"x",-1},{"y",-1},{"operation","paint"},{"tileId","ground"}}})},
        {"dryRun",true}}.dump());
    const auto dry_stroke_result=nlohmann::json::parse(dry_stroke.output_json).at("result");
    std::ifstream dry_map_stream(tile_tool_root/"tool.tilemap.json");const std::string dry_map{std::istreambuf_iterator<char>(dry_map_stream),{}};
    dry_map_stream.close();
    const auto apply_stroke=tile_registry.invoke("asset.tilemap.stroke",nlohmann::json{{"assetId","tilemap.tool"},{"layerId","ground"},
        {"manager","test.tile-brush"},{"expectedFingerprint",dry_stroke_result.at("plan").at("baseFingerprint")},
        {"edits",nlohmann::json::array({{{"x",-1},{"y",-1},{"operation","paint"},{"tileId","ground"}}})},{"dryRun",false}}.dump());
    const auto apply_stroke_result=nlohmann::json::parse(apply_stroke.output_json).at("result");
    const auto stale_stroke=tile_registry.invoke("asset.tilemap.stroke",nlohmann::json{{"assetId","tilemap.tool"},{"layerId","ground"},
        {"manager","test.tile-brush"},{"expectedFingerprint",dry_stroke_result.at("plan").at("baseFingerprint")},
        {"edits",nlohmann::json::array({{{"x",0},{"y",0},{"operation","paint"},{"tileId","ground"}}})},{"dryRun",false}}.dump());
    const auto stale_result=nlohmann::json::parse(stale_stroke.output_json).at("result");
    std::ifstream applied_map_stream(tile_tool_root/"tool.tilemap.json");const std::string applied_map{std::istreambuf_iterator<char>(applied_map_stream),{}};
    if(dry_stroke.exit_code!=0||!dry_stroke_result.at("success")||dry_map.find(R"("chunks":[])")==std::string::npos||
       apply_stroke.exit_code!=0||!apply_stroke_result.at("success")||applied_map.find("ground") == std::string::npos||
       stale_result.at("success")||stale_result.at("code")!="tilemap.stroke-conflict") {
        std::cerr<<dry_stroke.output_json<<'\n'<<apply_stroke.output_json<<'\n'<<stale_stroke.output_json<<'\n';return 32;
    }
    applied_map_stream.close();
    const auto source_undo=tile_registry.invoke("asset.source.undo",R"({"manager":"test.agent-undo"})");
    const auto undo_result=nlohmann::json::parse(source_undo.output_json).at("result");
    std::ifstream undone_stream(tile_tool_root/"tool.tilemap.json");const std::string undone_map{std::istreambuf_iterator<char>(undone_stream),{}};undone_stream.close();
    const auto source_redo=tile_registry.invoke("asset.source.redo",R"({"manager":"test.agent-redo"})");
    const auto redo_result=nlohmann::json::parse(source_redo.output_json).at("result");
    std::ifstream redone_stream(tile_tool_root/"tool.tilemap.json");const std::string redone_map{std::istreambuf_iterator<char>(redone_stream),{}};redone_stream.close();
    if(source_undo.exit_code!=0||!undo_result.at("success")||!undo_result.at("runtimeReloaded")||undone_map.find(R"("chunks":[])")==std::string::npos||
       source_redo.exit_code!=0||!redo_result.at("success")||!redo_result.at("runtimeReloaded")||redone_map.find("ground")==std::string::npos) {
        std::cerr<<source_undo.output_json<<'\n'<<source_redo.output_json<<'\n';return 33;
    }
    const auto rectangle_region=tile_registry.invoke("asset.tilemap.region",nlohmann::json{{"assetId","tilemap.tool"},{"layerId","ground"},
        {"shape","rectangle"},{"first",{{"x",0},{"y",0}}},{"second",{{"x",1},{"y",1}}},{"operation","paint"},{"tileId","ground"},
        {"manager","test.rectangle"},{"dryRun",false}}.dump());
    const auto rectangle_result=nlohmann::json::parse(rectangle_region.output_json).at("result");
    const auto empty_flood=tile_registry.invoke("asset.tilemap.region",nlohmann::json{{"assetId","tilemap.tool"},{"layerId","ground"},
        {"shape","flood"},{"first",{{"x",100},{"y",100}}},{"operation","erase"},{"manager","test.flood"},{"dryRun",true}}.dump());
    const auto empty_flood_result=nlohmann::json::parse(empty_flood.output_json).at("result");
    const auto compressed_region=tile_registry.invoke("asset.tilemap.region",nlohmann::json{{"assetId","tilemap.tool"},{"layerId","ground"},
        {"shape","rectangle"},{"first",{{"x",10},{"y",10}}},{"second",{{"x",19},{"y",19}}},{"operation","paint"},{"tileId","ground"},
        {"manager","test.compressed-region"},{"dryRun",true}}.dump());
    const auto compressed_result=nlohmann::json::parse(compressed_region.output_json).at("result");
    if(rectangle_region.exit_code!=0||!rectangle_result.at("success")||rectangle_result.at("plan").at("changedCellCount")!=4||
       empty_flood.exit_code==0||empty_flood_result.at("code")!="tilemap.fill-unbounded-empty"||compressed_region.exit_code!=0||
       !compressed_result.at("plan").at("editsTruncated")||compressed_result.at("plan").at("editCount")!=100||compressed_result.at("plan").at("edits").size()!=64) {
        std::cerr<<rectangle_region.output_json<<'\n'<<empty_flood.output_json<<'\n'<<compressed_region.output_json<<'\n';return 34;
    }
    const auto palette_preview=tile_registry.invoke("asset.tile-palette.autotile",nlohmann::json{{"assetId","palette.tool"},{"tileId","ground"},
        {"autotileGroup","terrain"},{"variants",nlohmann::json::array({{{"mask",2},{"frame","ground.0"}},{{"mask",8},{"frame","ground.0"}}})},
        {"manager","test.palette-editor"},{"dryRun",true}}.dump());
    const auto palette_preview_result=nlohmann::json::parse(palette_preview.output_json).at("result");
    const auto palette_apply=tile_registry.invoke("asset.tile-palette.autotile",nlohmann::json{{"assetId","palette.tool"},{"tileId","ground"},
        {"autotileGroup","terrain"},{"variants",nlohmann::json::array({{{"mask",2},{"frame","ground.0"}},{{"mask",8},{"frame","ground.0"}}})},
        {"manager","test.palette-editor"},{"expectedFingerprint",palette_preview_result.at("plan").at("baseFingerprint")},{"dryRun",false}}.dump());
    std::ifstream edited_palette_stream(tile_tool_root/"tool.tile-palette.json");const std::string edited_palette_source{std::istreambuf_iterator<char>(edited_palette_stream),{}};
    edited_palette_stream.close();
    if(palette_preview.exit_code!=0||!palette_preview_result.at("success")||palette_apply.exit_code!=0||
       !nlohmann::json::parse(palette_apply.output_json).at("result").at("success")||edited_palette_source.find("autotile") == std::string::npos)return 36;
    const auto auto_sprite=noemancer::SpriteAssetCodec::parse_json(R"({"schema":"noemancer.sprite-asset/0.1","assetId":"sprite.auto",
      "textureAsset":"texture.auto","textureSize":[48,16],"pixelsPerUnit":16,"sampling":"nearest","alphaMode":"cutout",
      "frames":[{"id":"terrain.base","rect":[0,0,16,16],"trimOffset":[0,0],"sourceSize":[16,16],"pivot":[0.5,0.5],"collisionProfile":""},
        {"id":"terrain.east","rect":[16,0,16,16],"trimOffset":[0,0],"sourceSize":[16,16],"pivot":[0.5,0.5],"collisionProfile":""},
        {"id":"terrain.west","rect":[32,0,16,16],"trimOffset":[0,0],"sourceSize":[16,16],"pivot":[0.5,0.5],"collisionProfile":""}],"clips":[],
      "provenance":{"sourceUri":"auto.png","sourceSha256":"fixture","generator":"test","license":"CC0"}})");
    const auto auto_palette=noemancer::TilemapAssetCodec::parse_palette_json(R"({"schema":"noemancer.tile-palette/0.2","assetId":"palette.auto",
      "spriteAsset":"sprite.auto","tiles":[{"id":"terrain","frame":"terrain.base","collision":"solid","tags":[],
      "autotile":{"group":"terrain","variants":[{"mask":2,"frame":"terrain.east"},{"mask":8,"frame":"terrain.west"}]}}]})");
    const auto auto_map=noemancer::TilemapAssetCodec::parse_tilemap_json(R"({"schema":"noemancer.tilemap/0.1","assetId":"tilemap.auto",
      "paletteAsset":"palette.auto","cellSize":[1,1],"chunkSize":[8,8],"layers":[{"id":"ground","sortingLayer":"terrain","sortingOrder":0,
      "collisionEnabled":true,"chunks":[{"position":[0,0],"cells":[[0,0,"terrain"],[1,0,"terrain"]]},
        {"position":[10,0],"cells":[[0,0,"terrain"]]}]}]})");
    const auto auto_scene=noemancer::SceneDocumentCodec::parse_json(R"({"schema":"noemancer.scene/0.1","sceneGuid":"scene.auto","name":"Auto",
      "entities":[{"guid":"entity.auto","name":"Auto","parent":null,"components":{"Transform":{"position":[0,0,0]},
      "TilemapRenderer":{"tilemapAsset":"tilemap.auto","visible":true,"collisionEnabled":true}}}]})");
    noemancer::World auto_world;if(!auto_sprite||!auto_palette||!auto_map||!auto_scene||!auto_world.register_sprite_asset(*auto_sprite.document)||
       !auto_world.register_tile_palette(*auto_palette.document)||!auto_world.register_tilemap_asset(*auto_map.document)||!auto_world.load_scene(*auto_scene.document).success)return 35;
    const auto auto_views=auto_world.entity_views();const auto auto_entity=std::ranges::find(auto_views,std::string("entity.auto"),&noemancer::WorldEntityView::id);
    if(auto_entity==auto_views.end()||auto_entity->tilemap_cells.size()!=3||auto_entity->tilemap_cells[0].autotile_mask!=2||
       auto_entity->tilemap_cells[0].sprite_frame.frame.id!="terrain.east"||auto_entity->tilemap_cells[1].autotile_mask!=8||
       auto_entity->tilemap_cells[1].sprite_frame.frame.id!="terrain.west"||auto_entity->tilemap_compiled_chunk_count!=2||
       auto_entity->tilemap_compilation_revision==0||auto_entity->tilemap_cells[0].chunk_x!=0||auto_entity->tilemap_cells[0].chunk_y!=0||
       auto_entity->tilemap_cells[0].chunk_content_fingerprint.empty())return 35;
    noemancer::TilemapViewQuery visibility_query{1000,1000,noemancer::TilemapVisibilityCamera{
        {0.0F,0.0F,10.0F},{0.0F,0.0F,0.0F},45.0F,0.1F,100.0F,"orthographic",10.0F}};
    const auto visible_views=auto_world.entity_views(visibility_query);const auto& visible_tilemap=visible_views.front();
    if(!visible_tilemap.tilemap_early_visibility_applied||visible_tilemap.tilemap_resolved_chunk_count!=1||
       visible_tilemap.tilemap_skipped_chunk_count!=1||visible_tilemap.tilemap_cells_skipped_before_resolution!=1||
       visible_tilemap.tilemap_cells.size()!=2)return 37;

    noemancer::SceneDocument constrained_scene;
    constrained_scene.scene_guid="scene.world-constraint";
    constrained_scene.name="World Constraint";
    noemancer::SceneEntityDocument anchor;
    anchor.guid="entity.constraint-anchor";anchor.name="Constraint Anchor";
    anchor.transform=noemancer::SceneTransform{.position={0.0,0.0,0.0}};
    anchor.rigid_body=noemancer::SceneRigidBody{.motion_type="static",.collision_layer=2U,.collision_mask=4U};
    anchor.box_collider=noemancer::SceneBoxCollider{};
    noemancer::SceneEntityDocument payload;
    payload.guid="entity.constraint-payload";payload.name="Constraint Payload";
    payload.transform=noemancer::SceneTransform{.position={0.0,-1.0,0.0}};
    payload.rigid_body=noemancer::SceneRigidBody{.motion_type="dynamic",.allow_sleeping=false,
        .collision_layer=4U,.collision_mask=2U};
    payload.box_collider=noemancer::SceneBoxCollider{};
    constrained_scene.entities={anchor,payload};
    noemancer::PhysicsConstraintSpec distance;
    distance.id="constraint.world.distance";distance.type=noemancer::PhysicsConstraintType::distance;
    distance.body_a=anchor.guid;distance.body_b=payload.guid;
    distance.frame.anchor_a={0.0F,0.0F,0.0F};distance.frame.anchor_b={0.0F,-1.0F,0.0F};
    distance.lower_limit=0.75F;distance.upper_limit=1.25F;distance.rest_length=1.0F;
    constrained_scene.physics_constraints.push_back(distance);
    noemancer::World constrained_world;
    const auto constrained_load=constrained_world.load_scene(constrained_scene);
    constrained_world.tick(1.0F/60.0F);
    const auto constrained_observation=nlohmann::json::parse(constrained_world.physics_observation_json());
    const auto constrained_canonical=nlohmann::json::parse(constrained_world.canonical_scene_json());
    const auto layer_plan=constrained_world.plan_property_update(payload.guid,"engine.entity.rigidBody.collisionMask","7",
        constrained_world.revision(),"test.physics-inspector");
    const auto layer_apply=constrained_world.apply_property_plan(layer_plan,false);
    auto updated_constraint=constrained_canonical.at("physicsConstraints").front();
    updated_constraint["upperLimit"]=1.5;
    const auto constraint_dry=nlohmann::json::parse(constrained_world.edit_physics_constraint_json("upsert",distance.id,
        updated_constraint.dump(),constrained_world.revision(),"test.constraint-panel",true));
    const auto constraint_apply=nlohmann::json::parse(constrained_world.edit_physics_constraint_json("upsert",distance.id,
        updated_constraint.dump(),constrained_world.revision(),"test.constraint-panel",false));
    const auto constraint_undo=constrained_world.undo(constrained_world.revision(),"test.constraint-undo");
    noemancer::CommandRegistry constrained_registry(constrained_world);
    const auto constraint_tool_observe=nlohmann::json::parse(constrained_registry.invoke("physics.constraint.observe","{}").output_json).at("result");
    const auto constraint_tool_remove=nlohmann::json::parse(constrained_registry.invoke("physics.constraint.edit",nlohmann::json{
        {"operation","remove"},{"constraintId",distance.id},{"baseRevision",constrained_world.revision()},
        {"manager","test.agent"},{"dryRun",true}}.dump()).output_json).at("result");
    if(!constrained_load.success||constrained_observation.at("schemaVersion")!="noemancer.physics-observation/0.2"||
       constrained_observation.at("constraints").size()!=1||
       constrained_observation.at("constraints").front().at("id")!=distance.id||
       !constrained_observation.at("constraints").front().at("backendCreated")||
       constrained_observation.at("bodies").front().find("collisionLayer")==constrained_observation.at("bodies").front().end()||
       constrained_canonical.at("physicsConstraints").size()!=1||!layer_plan.valid||!layer_apply.success||
       !constraint_dry.at("success")||!constraint_apply.at("success")||!constraint_undo.success||
       constraint_tool_observe.at("constraints").size()!=1||!constraint_tool_remove.at("success")||
       constrained_world.inspector_document_json(payload.guid).find("engine.entity.rigidBody.collisionMask")==std::string::npos) {
        std::cerr<<"Canonical World did not integrate constraint, collision filtering, observation, and Inspector authority\n";
        return 38;
    }
    std::filesystem::remove_all(tile_tool_root);
    return 0;
    } catch (const std::exception& error) {
        std::cerr << "Unhandled test exception: " << error.what() << '\n';
        return 99;
    } catch (...) {
        std::cerr << "Unhandled non-standard test exception\n";
        return 100;
    }
}
