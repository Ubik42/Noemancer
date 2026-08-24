#include "engine/play_world_apply.hpp"
#include "engine/scene_document.hpp"
#include "engine/world.hpp"

#include <algorithm>
#include <iostream>
#include <nlohmann/json.hpp>

#define CHECK(condition) do { if (!(condition)) { \
    std::cerr << "play_world_apply check failed at line " << __LINE__ << ": " #condition "\n"; \
    return 1; } } while (false)

int main() {
    using namespace noemancer;
    auto base = make_bootstrap_scene_document();
    auto runtime = base;
    CHECK(!runtime.entities.empty());
    auto moved = std::find_if(runtime.entities.begin(), runtime.entities.end(),
                              [](const SceneEntityDocument& entity) { return entity.transform.has_value(); });
    CHECK(moved != runtime.entities.end());
    moved->transform->position.x += 4.0;
    moved->name += " Runtime";
    const auto plan = plan_play_world_apply(SceneDocumentCodec::write_canonical_json(base),
                                            SceneDocumentCodec::write_canonical_json(runtime), 41U);
    CHECK(plan.valid);
    CHECK(plan.code == "ok");
    CHECK(plan.base_revision == 41U);
    CHECK(plan.changes.size() == 2U);
    CHECK(std::ranges::any_of(plan.changes,[](const PlayWorldChange& change){return change.field=="engine.scene.component.Transform";}));
    CHECK(std::ranges::all_of(plan.changes,[](const PlayWorldChange& change){return !change.change_id.empty()&&!change.operation.empty();}));
    const auto encoded = nlohmann::json::parse(plan.to_json());
    CHECK(encoded["changeCount"] == 2U);
    CHECK(encoded["schemaVersion"] == "noemancer.play-world-apply-plan/0.2");
    CHECK(encoded["changes"][0]["entityId"] == moved->guid);

    std::vector<std::string> all_change_ids;for(const auto& change:plan.changes)all_change_ids.push_back(change.change_id);
    const auto all_selected=plan_play_world_apply_selection(plan,all_change_ids);
    CHECK(all_selected.valid&&all_selected.changes.size()==plan.changes.size());
    CHECK(all_selected.candidate_scene_json==plan.candidate_scene_json);
    const auto empty_selected=plan_play_world_apply_selection(plan,{});
    CHECK(empty_selected.valid&&empty_selected.code=="play.apply.selection-empty"&&empty_selected.changes.empty());
    CHECK(empty_selected.candidate_scene_json==SceneDocumentCodec::write_canonical_json(base));
    const auto transform_change=std::ranges::find(plan.changes,std::string{"engine.scene.component.Transform"},&PlayWorldChange::field);
    CHECK(transform_change!=plan.changes.end());
    const auto transform_only=plan_play_world_apply_selection(plan,{transform_change->change_id});
    CHECK(transform_only.valid&&transform_only.changes.size()==1U);
    const auto transform_scene=SceneDocumentCodec::parse_json(transform_only.candidate_scene_json,"test://selection");
    CHECK(transform_scene);
    const auto transformed=std::ranges::find(transform_scene.document->entities,moved->guid,&SceneEntityDocument::guid);
    CHECK(transformed!=transform_scene.document->entities.end()&&transformed->name!=moved->name&&
          transformed->transform&&transformed->transform->position.x==moved->transform->position.x);
    CHECK(!plan_play_world_apply_selection(plan,{"play-change-unknown"}).valid);
    CHECK(!plan_play_world_apply_selection(plan,{transform_change->change_id,transform_change->change_id}).valid);
    const auto deterministic=plan_play_world_apply(SceneDocumentCodec::write_canonical_json(base),
        SceneDocumentCodec::write_canonical_json(runtime),41U);
    CHECK(deterministic.changes.size()==plan.changes.size());
    for(std::size_t index=0;index<plan.changes.size();++index)CHECK(deterministic.changes[index].change_id==plan.changes[index].change_id);
    auto conflicting=plan;
    conflicting.changes={{.entity_id=moved->guid,.field="engine.scene.entity.removed",.before_json="{}",.after_json="null",
        .change_id="play-change-remove",.operation="remove-entity"},
        {.entity_id=moved->guid,.field="engine.entity.name",.before_json="\"before\"",.after_json="\"after\"",
        .change_id="play-change-name",.operation="set-name"}};
    CHECK(!plan_play_world_apply_selection(conflicting,{"play-change-remove","play-change-name"}).valid);

    const auto unchanged = plan_play_world_apply(SceneDocumentCodec::write_canonical_json(base),
                                                  SceneDocumentCodec::write_canonical_json(base), 42U);
    CHECK(unchanged.valid && unchanged.code == "play.apply.no-change" && unchanged.changes.empty());

    runtime.scene_guid = "scene.other";
    const auto mismatched = plan_play_world_apply(SceneDocumentCodec::write_canonical_json(base),
                                                   SceneDocumentCodec::write_canonical_json(runtime), 43U);
    CHECK(!mismatched.valid && mismatched.code == "play.apply.scene-mismatch");

    SceneDocument moving;
    moving.scene_guid = "scene.play-apply.integration";
    moving.name = "Play Apply Integration";
    moving.source_uri = "memory://play-apply.scene.json";
    SceneEntityDocument actor;
    actor.guid = "entity.actor";
    actor.name = "Actor";
    actor.transform = SceneTransform{};
    actor.velocity = SceneVelocity{.linear = {2.0, 0.0, 0.0}};
    moving.entities.push_back(actor);
    World edit_world;
    World play_world;
    CHECK(edit_world.load_scene(moving).success);
    CHECK(play_world.load_scene(moving).success);
    const auto edit_revision = edit_world.revision();
    const auto edit_baseline = edit_world.canonical_scene_json();
    play_world.tick(0.5F);
    const auto integration_plan = plan_play_world_apply(
        edit_baseline, play_world.runtime_authoring_scene_json(), edit_revision);
    CHECK(integration_plan.valid && !integration_plan.changes.empty());
    const auto preview = nlohmann::json::parse(edit_world.replace_scene_document_json(
        integration_plan.candidate_scene_json, edit_revision, "test.play-apply", true));
    CHECK(preview["success"] && preview["dryRun"]);
    const auto receipt = nlohmann::json::parse(edit_world.replace_scene_document_json(
        integration_plan.candidate_scene_json, edit_revision, "test.play-apply", false));
    CHECK(receipt["success"] && edit_world.can_undo());
    CHECK(edit_world.canonical_scene_json() == integration_plan.candidate_scene_json);
    const auto undo = edit_world.undo(edit_world.revision(), "test.play-apply.undo");
    CHECK(undo.success && edit_world.canonical_scene_json() == edit_baseline);
    return 0;
}
