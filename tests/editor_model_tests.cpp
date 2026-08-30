#include "editor/editor_model.hpp"
#include "engine/process_diagnostics.hpp"
#include "engine/semantic_ui.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <iostream>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

int main() {
    noemancer::configure_process_diagnostics("test.editor-model");
    noemancer::World world;
    if (!world.load_scene(noemancer::make_bootstrap_scene_document()).success) {
        std::cerr << "Bootstrap scene did not load for the editor model\n";
        return 1;
    }
    noemancer::AssetRegistry assets;
    noemancer::EditorModel model(world, assets);
    const auto script_project=std::filesystem::path(__FILE__).parent_path()/"fixtures/managed-project/ManagedFixture.csproj";
    if(world.scripting_project_configure_json(script_project.parent_path(),script_project).find(R"("success":true)")==std::string::npos||
       model.compile_scripts_json("Debug").find(R"("success":true)")==std::string::npos||
       model.scripting_status_json().find("ManagedFixture.csproj")==std::string::npos||
       model.scripting_status_json().find("managed-session-")==std::string::npos||
       model.scripting_status_json().find("noemancer.managed-debug-attach/0.1")==std::string::npos||
       model.scripting_status_json().find(R"("targetReady":true)")==std::string::npos){
       std::cerr<<"Editor model did not expose the script build and Host session\n";return 13;
    }
    const auto invalid_project=std::filesystem::path(__FILE__).parent_path()/"fixtures/managed-project-invalid/InvalidFixture.csproj";
    if(world.scripting_project_configure_json(invalid_project.parent_path(),invalid_project).find(R"("success":true)")==std::string::npos){return 14;}
    const auto invalid_compile=model.compile_scripts_json("Debug");
    if(invalid_compile.find(R"("success":false)")==std::string::npos||invalid_compile.find("BrokenScript.cs")==std::string::npos||
       invalid_compile.find(R"("severity":"error")")==std::string::npos){
        std::cerr<<"Editor model did not preserve structured C# diagnostics\n";return 15;
    }
    static_cast<void>(world.scripting_project_configure_json(script_project.parent_path(),script_project));
    if (model.panels().size() != 7 || model.objects().size() != 10 || model.assets().size() != assets.records().size()) {
        std::cerr << "Bootstrap editor model is incomplete\n";
        return 2;
    }
    if (model.selected_asset() == nullptr ||
        model.selected_asset_inspection_json().find(R"("format":"glb")") == std::string::npos) {
        std::cerr << "Editor Asset Browser did not select inspectable imported metadata\n";
        return 9;
    }
    const auto asset_browser_source = model.asset_browser_semantic_ui_document_json({.page_limit = 5U});
    const auto asset_browser = nlohmann::json::parse(asset_browser_source);
    const auto asset_browser_validation = nlohmann::json::parse(
        noemancer::semantic_ui_validation_json(asset_browser_source));
    if (!asset_browser.value("valid", false) || !asset_browser_validation.value("valid", false) ||
        asset_browser.value("revision", 0U) != assets.revision() ||
        asset_browser.at("page").value("total", 0U) != model.assets().size() ||
        asset_browser.at("page").value("returned", 0U) != 5U ||
        !asset_browser.at("page").value("truncated", false)) {
        std::cerr << "Asset Browser semantic document is not valid or bounded\n";
        return 42;
    }
    const auto& asset_nodes = asset_browser.at("nodes");
    const auto& next_page_action = asset_nodes.at(0).at("actions").at(0);
    if (next_page_action.at("binding").value("kind", "") != "editor-asset-browser-page" ||
        next_page_action.at("binding").value("direction", "") != "next" ||
        next_page_action.at("binding").value("cursor", 0U) != 5U) {
        std::cerr << "Asset Browser did not expose retained next-page navigation\n";
        return 48;
    }
    const auto unselected_card = std::ranges::find_if(asset_nodes, [](const auto& node) {
        return node.value("role", std::string{}) == "griditem" &&
            !node.value("state", nlohmann::json::object()).value("selected", false);
    });
    if (unselected_card == asset_nodes.end() || unselected_card->at("actions").size() != 1U ||
        !unselected_card->at("presentation").at("inlineActionIds").empty()) return 51;
    for (std::size_t index = 2U; index < asset_nodes.size(); ++index) {
        if (asset_nodes.at(index - 1U).at("asset").value("id", "") >=
            asset_nodes.at(index).at("asset").value("id", "")) {
            std::cerr << "Asset Browser cards are not stably sorted by Asset ID\n";
            return 43;
        }
    }
    const auto& first_card = asset_nodes.at(1);
    const auto& first_action = first_card.at("actions").at(0);
    if (first_action.at("binding").value("kind", "") != "editor-asset-selection" ||
        first_action.at("binding").value("assetId", "") != first_card.at("asset").value("id", "") ||
        first_action.at("binding").value("sourceRevision", 0U) != assets.revision() ||
        (!first_card.at("state").value("selected", false) &&
            (!first_card.at("presentation").at("inlineActionIds").empty() || first_card.at("actions").size() != 1U))) {
        std::cerr << "Asset card selection binding or existing action handlers are incomplete\n";
        return 44;
    }
    if (!first_card.at("asset").at("thumbnail").value("uri", "").empty() &&
        first_card.at("presentation").value("imageSource", "") !=
            first_card.at("asset").at("thumbnail").value("uri", "")) {
        std::cerr << "Asset card did not project its existing thumbnail artifact identity\n";
        return 49;
    }
    const auto next_cursor = asset_browser.at("page").at("nextCursor").get<std::size_t>();
    const auto second_page_source = model.asset_browser_semantic_ui_document_json(
        {.cursor = next_cursor, .page_limit = 5U});
    const auto second_page = nlohmann::json::parse(second_page_source);
    if (second_page.at("nodes").at(1).at("asset").value("id", "") <=
            asset_nodes.back().at("asset").value("id", "") ||
        model.asset_browser_semantic_ui_document_json({.cursor = next_cursor, .page_limit = 5U}) !=
            second_page_source) {
        std::cerr << "Asset Browser pagination is overlapping or non-deterministic\n";
        return 45;
    }
    if (second_page.at("nodes").at(0).at("actions").at(0).at("binding").value("direction", "") !=
        "previous") return 50;
    const auto selected_asset_id = model.selected_asset()->id;
    const auto filtered_browser = nlohmann::json::parse(model.asset_browser_semantic_ui_document_json(
        {.query = selected_asset_id, .page_limit = 8U}));
    if (filtered_browser.at("page").value("matched", 0U) != 1U ||
        !filtered_browser.at("nodes").at(1).at("state").value("selected", false) ||
        filtered_browser.at("selection").value("assetId", "") != selected_asset_id ||
        filtered_browser.at("nodes").at(1).at("actions").size() != 5U ||
        filtered_browser.at("nodes").at(1).at("presentation").at("inlineActionIds") !=
            nlohmann::json::array({"asset.import","asset.inspect","asset.build-preview","asset.cook"}) ||
        filtered_browser.at("nodes").at(1).at("actions").at(3).at("binding").value("operation", "") !=
            "build-preview" ||
        filtered_browser.at("nodes").at(1).at("actions").at(3).at("binding").value("assetId", "") !=
            selected_asset_id ||
        filtered_browser.at("nodes").at(1).at("actions").at(3).at("binding").value("sourceRevision", 0U) !=
            assets.revision()) {
        std::cerr << "Asset Browser filtering did not retain the authoritative selection\n";
        return 46;
    }
    const auto past_end = nlohmann::json::parse(model.asset_browser_semantic_ui_document_json(
        {.cursor = model.assets().size() + 100U, .page_limit = 8U}));
    const auto hard_limited = nlohmann::json::parse(model.asset_browser_semantic_ui_document_json(
        {.page_limit = 10000U}));
    if (past_end.at("page").value("returned", 1U) != 0U ||
        !past_end.at("page").at("nextCursor").is_null() || past_end.at("nodes").size() != 1U ||
        hard_limited.at("page").value("limit", 0U) != 256U ||
        hard_limited.at("page").value("returned", 0U) > 256U) {
        std::cerr << "Asset Browser cursor boundary or hard page limit is invalid\n";
        return 47;
    }

    const auto json_string_field = [](const std::string& document, const std::string_view key) {
        const auto marker = "\"" + std::string(key) + "\":\"";
        const auto begin = document.find(marker);
        if (begin == std::string::npos) return std::string{};
        const auto value_begin = begin + marker.size();
        const auto value_end = document.find('"', value_begin);
        return value_end == std::string::npos ? std::string{} : document.substr(value_begin, value_end - value_begin);
    };
    const auto wait_for_asset_job_terminal = [&](const std::chrono::seconds timeout) {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        std::string observation;
        while (std::chrono::steady_clock::now() < deadline) {
            observation = model.active_asset_job_json();
            const auto state = json_string_field(observation, "state");
            if (observation.find(R"("valid":true)") != std::string::npos &&
                (state == "succeeded" || state == "failed" || state == "cancelled")) return observation;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return model.active_asset_job_json();
    };

    const auto cook_started = std::chrono::steady_clock::now();
    const auto cook_selected = model.cook_selected_asset();
    const auto cook_elapsed = std::chrono::steady_clock::now() - cook_started;
    const auto queued_observation = model.active_asset_job_json();
    const auto busy_asset_browser = nlohmann::json::parse(model.asset_browser_semantic_ui_document_json(
        {.query = model.selected_asset()->id, .page_limit = 1U}));
    const auto& busy_actions = busy_asset_browser.at("nodes").at(1).at("actions");
    if (!cook_selected.success || cook_selected.code != "asset.job.queued" ||
        cook_elapsed > std::chrono::milliseconds(500) || queued_observation.find(R"("valid":true)") == std::string::npos ||
        json_string_field(queued_observation, "kind") != "cook" || busy_actions.size() != 5U ||
        busy_actions.at(4).at("state").value("enabled", true) ||
        !busy_actions.at(4).at("state").value("busy", false)) {
        std::cerr << "Cook Selected did not return as a bounded background Job\n";
        return 30;
    }
    const auto cooked_observation = wait_for_asset_job_terminal(std::chrono::seconds(20));
    if (json_string_field(cooked_observation, "state") != "succeeded" ||
        cooked_observation.find("\"artifacts\":[") == std::string::npos ||
        cooked_observation.find("generated://") == std::string::npos) {
        std::cerr << "Cook Selected did not expose terminal success and artifact URIs\n";
        return 31;
    }

    const auto thumbnail=model.generate_selected_asset_thumbnail();
    const auto thumbnail_observation=wait_for_asset_job_terminal(std::chrono::seconds(10));
    model.refresh();
    if(!thumbnail.success||json_string_field(thumbnail_observation,"state")!="succeeded"||
       thumbnail_observation.find("cache://thumbnails/")==std::string::npos||
       model.selected_asset()==nullptr||!model.selected_asset()->thumbnail_cached) {
        std::cerr<<"Asset Browser did not generate and discover a cached thumbnail artifact\n";return 32;
    }
    const auto import_revision=assets.revision();
    const auto imported=model.import_selected_asset();
    const auto import_observation=wait_for_asset_job_terminal(std::chrono::seconds(10));
    const auto reconciliation=model.reconcile_active_asset_job();
    if(!imported.success||json_string_field(import_observation,"state")!="succeeded"||!reconciliation||
       !reconciliation->success||assets.revision()!=import_revision+1U||
       model.selected_asset_repair_json().find("noemancer.asset-repair-report/0.1")==std::string::npos) {
        std::cerr<<"Background Import did not reconcile through the live Registry authority\n";return 33;
    }

    if (model.selected_object().id != "entity.demo-cube") {
        std::cerr << "Bootstrap selection is not deterministic\n";
        return 3;
    }
    const auto edit_outliner = nlohmann::json::parse(model.outliner_semantic_ui_document_json());
    const auto& edit_nodes = edit_outliner.at("nodes");
    const auto root_node = std::ranges::find_if(edit_nodes, [](const auto& node) {
        return node.value("id", std::string{}) == "editor.outliner.entity.entity.bootstrap-root";
    });
    const auto cube_node = std::ranges::find_if(edit_nodes, [](const auto& node) {
        return node.value("id", std::string{}) == "editor.outliner.entity.entity.demo-cube";
    });
    const auto edit_outliner_validation = nlohmann::json::parse(
        noemancer::semantic_ui_validation_json(edit_outliner.dump()));
    if (!edit_outliner.value("valid", false) || !edit_outliner_validation.value("valid", false) ||
        edit_outliner.value("authority", "") != "edit-world" ||
        !edit_outliner.value("writable", false) || root_node == edit_nodes.end() || cube_node == edit_nodes.end() ||
        cube_node->at("parentId") != root_node->at("id") ||
        !cube_node->at("state").value("selected", false) ||
        cube_node->at("actions").size() != 6U ||
        cube_node->at("actions").at(0).at("binding").value("kind", "") != "editor-entity-selection" ||
        cube_node->at("actions").at(0).at("binding").value("entityId", "") != "entity.demo-cube" ||
        cube_node->at("actions").at(0).at("binding").value("sourceRevision", 0U) != world.revision() ||
        cube_node->at("presentation").at("inlineActionIds") !=
            nlohmann::json::array({"outliner.rename","outliner.copy","outliner.duplicate",
                "outliner.reparent","outliner.delete"}) ||
        root_node->at("actions").size() != 1U ||
        !root_node->at("presentation").at("inlineActionIds").empty() ||
        edit_nodes.at(0).at("presentation").at("inlineActionIds") !=
            nlohmann::json::array({"outliner.create-empty","outliner.paste"}) ||
        edit_nodes.at(0).at("actions").at(0).at("binding").value("kind", "") !=
            "editor-entity-create" ||
        edit_nodes.at(0).at("actions").at(0).at("binding").value("sourceRevision", 0U) != world.revision() ||
        !edit_nodes.at(0).at("actions").at(0).at("state").value("enabled", false) ||
        cube_node->at("actions").at(2).at("binding").value("operation", "") != "copy" ||
        cube_node->at("actions").at(2).at("binding").value("entityId", "") != "entity.demo-cube" ||
        cube_node->at("actions").at(1).at("input").value("field", "") != "displayName" ||
        cube_node->at("actions").at(1).at("input").value("value", "") != "Demo Cube" ||
        cube_node->at("actions").at(1).at("input").value("maxLength", 0U) != 128U ||
        cube_node->at("actions").at(4).at("input").value("field", "") != "parentEntityId" ||
        cube_node->at("actions").at(4).at("input").value("value", "") != "entity.bootstrap-root" ||
        cube_node->at("actions").at(4).at("input").at("options").at(0).value("label", "") != "Scene Root" ||
        std::ranges::any_of(cube_node->at("actions").at(4).at("input").at("options"), [](const auto& option) {
            return option.value("value", std::string{}) == "entity.demo-cube";
        }) ||
        !cube_node->at("actions").at(5).at("confirmation").value("required", false) ||
        edit_nodes.at(0).at("actions").at(1).at("state").value("enabled", true)) {
        std::cerr << "Edit World Outliner semantic hierarchy or selection is incomplete\n";
        return 38;
    }
    const auto edit_outliner_repeat = model.outliner_semantic_ui_document_json();
    if (edit_outliner.dump() != edit_outliner_repeat) {
        std::cerr << "Outliner semantic ordering is not deterministic\n";
        return 39;
    }
    if (!model.select_object("entity.bootstrap-root")) return 78;
    const auto root_selection_outliner = nlohmann::json::parse(model.outliner_semantic_ui_document_json());
    const auto selected_root = std::ranges::find_if(root_selection_outliner.at("nodes"), [](const auto& node) {
        return node.value("id", std::string{}) == "editor.outliner.entity.entity.bootstrap-root";
    });
    if (selected_root == root_selection_outliner.at("nodes").end() ||
        selected_root->at("actions").at(4).at("input").at("options").size() != 1U ||
        selected_root->at("actions").at(4).at("input").at("options").at(0).value("value", "invalid") != "")
        return 79;
    if (!model.select_object("entity.demo-sphere")) return 56;
    const auto changed_selection_outliner = nlohmann::json::parse(model.outliner_semantic_ui_document_json());
    const auto changed_sphere = std::ranges::find_if(changed_selection_outliner.at("nodes"), [](const auto& node) {
        return node.value("id", std::string{}) == "editor.outliner.entity.entity.demo-sphere";
    });
    const auto changed_cube = std::ranges::find_if(changed_selection_outliner.at("nodes"), [](const auto& node) {
        return node.value("id", std::string{}) == "editor.outliner.entity.entity.demo-cube";
    });
    if (changed_sphere == changed_selection_outliner.at("nodes").end() ||
        changed_cube == changed_selection_outliner.at("nodes").end() ||
        changed_sphere->at("actions").at(1).at("input").value("value", "") != "Jolt Dynamic Sphere" ||
        changed_cube->at("actions").size() != 1U ||
        !changed_cube->at("presentation").at("inlineActionIds").empty()) return 57;
    if (!model.select_object("entity.demo-cube")) return 58;
    const std::vector<std::string> play_selection{"entity.demo-sphere"};
    const auto play_outliner = nlohmann::json::parse(model.outliner_semantic_ui_document_json(
        noemancer::EditorOutlinerAuthorityView{
            .authority = "play-world-read-only", .simulation_state = "playing", .writable = false,
            .world_revision = world.revision() + 10U, .objects = model.objects(),
            .selected_object_ids = play_selection,
            .primary_selected_object_id = "entity.demo-sphere"}));
    const auto play_sphere = std::ranges::find_if(play_outliner.at("nodes"), [](const auto& node) {
        return node.value("id", std::string{}) == "editor.outliner.entity.entity.demo-sphere";
    });
    if (play_outliner.value("authority", "") != "play-world-read-only" ||
        play_outliner.value("writable", true) || play_sphere == play_outliner.at("nodes").end() ||
        !play_sphere->at("state").value("selected", false) || play_sphere->at("actions").size() != 1U ||
        play_sphere->at("actions").at(0).value("handler", "") != "EditorModel.select_object" ||
        !play_outliner.at("nodes").at(0).at("actions").empty() ||
        !play_sphere->at("presentation").at("inlineActionIds").empty() ||
        play_outliner.dump().find("\"input\"") != std::string::npos ||
        play_outliner.dump().find("\"confirmation\"") != std::string::npos) {
        std::cerr << "Play World Outliner did not remain a read-only authority projection\n";
        return 40;
    }
    std::vector<noemancer::EditorObject> bulk_objects;
    bulk_objects.reserve(700U);
    for (std::size_t index = 700U; index-- > 0U;) {
        const auto suffix = std::to_string(10000U + index).substr(1U);
        bulk_objects.push_back({.id = "entity.bulk." + suffix, .name = "Bulk " + suffix, .kind = "Entity"});
    }
    const std::vector<std::string> bulk_selection{"entity.bulk.0699"};
    const auto truncated_outliner = nlohmann::json::parse(model.outliner_semantic_ui_document_json(
        noemancer::EditorOutlinerAuthorityView{
            .authority = "play-world-read-only", .simulation_state = "paused", .writable = false,
            .world_revision = 900U, .objects = bulk_objects, .selected_object_ids = bulk_selection,
            .primary_selected_object_id = "entity.bulk.0699"},
        {.entity_limit = 32U, .selection_limit = 1U}));
    if (!truncated_outliner.at("entities").value("truncated", false) ||
        truncated_outliner.at("entities").value("total", 0U) != bulk_objects.size() ||
        truncated_outliner.at("entities").value("included", 0U) != 32U ||
        truncated_outliner.at("nodes").size() != 33U ||
        truncated_outliner.at("nodes").at(1).at("entity").value("id", "") != "entity.bulk.0000") {
        std::cerr << "Large Outliner projection did not publish bounded truncation metadata\n";
        return 41;
    }
    std::vector<noemancer::EditorObject> boundary_objects;
    boundary_objects.reserve(4096U);
    boundary_objects.push_back({.id="entity.boundary.root",.name="Boundary Root",.kind="Entity"});
    for (std::size_t index=1U;index<4096U;++index) {
        const auto suffix=std::to_string(10000U+index).substr(1U);
        boundary_objects.push_back({.id="entity.boundary."+suffix,.name="Boundary "+suffix,
            .kind="Entity",.parent_id="entity.boundary.root"});
    }
    const std::vector<std::string> boundary_selection{"entity.boundary.4095"};
    const auto boundary_outliner=nlohmann::json::parse(model.outliner_semantic_ui_document_json(
        noemancer::EditorOutlinerAuthorityView{.authority="edit-world",.simulation_state="edit",.writable=true,
            .world_revision=4096U,.objects=boundary_objects,.selected_object_ids=boundary_selection,
            .primary_selected_object_id="entity.boundary.4095"},
        {.entity_limit=4096U,.selection_limit=1U}));
    const auto boundary_selected=std::ranges::find_if(boundary_outliner.at("nodes"),[](const auto& node) {
        return node.value("id",std::string{})=="editor.outliner.entity.entity.boundary.4095";
    });
    if(boundary_outliner.at("entities").value("total",0U)!=4096U||
       boundary_outliner.at("entities").value("truncated",true)||
       boundary_selected==boundary_outliner.at("nodes").end()||
       boundary_selected->at("actions").at(4).at("input").at("options").size()!=256U||
       boundary_selected->at("actions").at(4).at("input").at("options").at(0).value("value","x")!=""||
       std::ranges::any_of(boundary_selected->at("actions").at(4).at("input").at("options"),[](const auto& option) {
           return option.value("value",std::string{})=="entity.boundary.4095";
       })) return 59;
    if(model.inspector_sections().size()<3U||model.semantic_snapshot_json().find("noemancer.inspector-document/0.1")==std::string::npos) {
        std::cerr<<"Editor did not consume the generated declarative Inspector document\n";
        return 11;
    }
    const auto material_receipt=model.set_selected_property("engine.entity.material.metallic","0.42");
    if(!material_receipt.success||world.canonical_scene_json().find("\"metallic\": 0.42") == std::string::npos||
        model.semantic_snapshot_json().find("engine.entity.material.metallic")==std::string::npos) {
        std::cerr<<"Schema-generated material property did not use the shared transaction path"
                 <<" success="<<material_receipt.success
                 <<" code="<<material_receipt.code
                 <<" detail="<<material_receipt.detail
                 <<" canonical="<<(world.canonical_scene_json().find("\"metallic\": 0.42")!=std::string::npos)
                 <<" semantic="<<(model.semantic_snapshot_json().find("engine.entity.material.metallic")!=std::string::npos)<<"\n";
        return 12;
    }

    model.select_object(0);
    if (model.selected_object().id != "entity.bootstrap-root") {
        std::cerr << "Editor selection did not update\n";
        return 4;
    }

    if (!model.select_object("entity.test-alien") || model.selected_object().id != "entity.test-alien" ||
        model.select_object("entity.does-not-exist")) {
        std::cerr << "Stable-ID editor selection is inconsistent\n";
        return 10;
    }

    model.select_object(2);
    const auto revision_before_edit = world.revision();
    const auto update = model.set_selected_transform({7.0F, 8.0F, 9.0F});
    model.refresh();
    if (!update.success || world.revision() != revision_before_edit + 1 ||
        !model.selected_object().transform || model.selected_object().transform->x != 7.0F) {
        std::cerr << "Inspector edit did not update the real World by stable ID\n";
        return 5;
    }
    if (!model.can_undo() || model.focused_observation_json().find("entity.demo-cube") == std::string::npos) {
        std::cerr << "Editor did not expose transaction history and focused observation\n";
        return 7;
    }
    const auto undo = model.undo();
    const auto redo = model.redo();
    model.refresh();
    if (!undo.success || !redo.success || !model.selected_object().transform ||
        model.selected_object().transform->x != 7.0F) {
        std::cerr << "Editor undo/redo did not preserve the selected live entity\n";
        return 8;
    }

    const auto snapshot = model.semantic_snapshot_json();
    if (snapshot.find("editor.panel.scene") == std::string::npos ||
        snapshot.find("entity.demo-cube") == std::string::npos ||
        snapshot.find("asset://scenes/bootstrap.scene.json") == std::string::npos ||
        snapshot.find("entity.bootstrap-root") == std::string::npos) {
        std::cerr << "Semantic editor snapshot is missing stable identities\n";
        return 6;
    }

    const auto verification_root = std::filesystem::absolute("generated/editor-authoring-test");
    std::filesystem::create_directories(verification_root);
    const auto scene_path = verification_root / "authoring.scene.json";
    const auto recovery_path = verification_root / "authoring.scene.json.noemancer-recovery";
    std::filesystem::remove(scene_path);
    std::filesystem::remove(recovery_path);
    auto editable_scene = noemancer::make_bootstrap_scene_document();
    editable_scene.source_uri = scene_path.generic_string();
    noemancer::World editable_world;
    if (!editable_world.load_scene(editable_scene).success) return 13;
    noemancer::EditorModel editable_model(editable_world, assets);
    const auto initial_count = editable_model.objects().size();
    const auto created = editable_model.create_empty_entity("Authoring Entity");
    const auto transformed=editable_model.set_selected_transform({1.0F,2.0F,3.0F,1.5F,0.75F,2.0F,0.0F,0.38268343F,0.0F,0.92387953F});
    const auto undo_transform=editable_model.undo();
    const auto redo_transform=editable_model.redo();
    editable_model.refresh();
    const auto component_added = editable_model.add_component("MeshRenderer");
    const auto undo_component = editable_model.undo();
    const auto redo_component = editable_model.redo();
    editable_model.refresh();
    const auto material_added = editable_model.add_component("PbrMaterial");
    const auto sprite_added=editable_model.add_component("SpriteRenderer");
    const auto sprite_asset_set=editable_model.set_selected_property("engine.entity.sprite.spriteAsset","\"sprite.courier\"");
    const auto sprite_clip_set=editable_model.set_selected_property("engine.entity.sprite.clip","\"run\"");
    const auto sprite_order_set=editable_model.set_selected_property("engine.entity.sprite.sortingOrder","12");
    const auto sprite_flip_set=editable_model.set_selected_property("engine.entity.sprite.flipX","true");
    const auto script_added=editable_model.add_component("ManagedScript");
    const auto script_type=editable_model.set_selected_property("engine.entity.managedScript.typeName","\"Game.PlayerController\"");
    const auto script_properties=editable_model.set_selected_property("engine.entity.managedScript.properties","{\"moveSpeed\":7.5,\"jumpHeight\":3.0}");
    const auto script_disabled=editable_model.set_selected_property("engine.entity.managedScript.enabled","false");
    const auto script_enabled=editable_model.set_selected_property("engine.entity.managedScript.enabled","true");
    const auto duplicated = editable_model.duplicate_selected();
    const auto renamed = editable_model.rename_selected("Renamed Authoring Copy");
    const auto reparented = editable_model.reparent_entity(duplicated.entity_id, created.entity_id);
    const auto rejected_cycle = editable_model.reparent_entity(created.entity_id, duplicated.entity_id);
    const auto renamed_object = std::ranges::find(editable_model.objects(), duplicated.entity_id, &noemancer::EditorObject::id);
    if (!created.success || !transformed.success || !undo_transform.success || !redo_transform.success ||
        editable_world.canonical_scene_json().find("rotationEulerDegrees")==std::string::npos ||
        !component_added.success || !undo_component.success || !redo_component.success ||
        !material_added.success || !sprite_added.success||!sprite_asset_set.success||!sprite_clip_set.success||
        !sprite_order_set.success||!sprite_flip_set.success||!script_added.success || !script_type.success || !script_properties.success ||
        !script_disabled.success || !script_enabled.success || !duplicated.success || !renamed.success || !reparented.success || rejected_cycle.success ||
        editable_world.canonical_scene_json().find("Game.PlayerController")==std::string::npos ||
        editable_world.canonical_scene_json().find("sprite.courier")==std::string::npos||
        editable_model.semantic_snapshot_json().find("engine.entity.sprite.sortingOrder")==std::string::npos||
        editable_world.canonical_scene_json().find("script."+duplicated.entity_id)==std::string::npos ||
        editable_world.scripting_observation_json().find("Game.PlayerController")==std::string::npos ||
        editable_model.semantic_snapshot_json().find("engine.entity.managedScript.properties")==std::string::npos ||
        renamed_object == editable_model.objects().end() || renamed_object->name != "Renamed Authoring Copy" ||
        renamed_object->parent_id != created.entity_id || editable_model.objects().size() != initial_count + 2 ||
        !editable_model.scene_dirty() || !editable_model.can_undo()) {
        std::cerr << "Editor entity authoring did not create transactional canonical scene edits\n";
        return 14;
    }
    const auto deleted = editable_model.delete_selected(false);
    const auto undo_delete = editable_model.undo();
    editable_model.refresh();
    if (!deleted.success || !undo_delete.success || editable_model.objects().size() != initial_count + 2) {
        std::cerr << "Structural scene edit undo did not restore the duplicated entity\n";
        return 15;
    }
    const auto redo_delete = editable_model.redo();
    editable_model.refresh();
    if (!redo_delete.success || editable_model.objects().size() != initial_count + 1) {
        std::cerr << "Structural scene edit redo did not reapply entity deletion\n";
        return 16;
    }
    const auto saved = editable_model.save_scene();
    std::ifstream saved_stream(scene_path, std::ios::binary);
    const std::string saved_text((std::istreambuf_iterator<char>(saved_stream)), std::istreambuf_iterator<char>());
    const auto saved_document = noemancer::SceneDocumentCodec::parse_json(saved_text, scene_path.generic_string());
    if (!saved.success || editable_model.scene_dirty() || !saved_document ||
        saved_document.document->entities.size() != initial_count + 1) {
        std::cerr << "Editor scene save did not persist a valid canonical document\n";
        return 17;
    }
    const auto renamed_for_second_save = editable_model.rename_selected("Second saved name");
    const auto second_saved = editable_model.save_scene();
    std::ifstream second_stream(scene_path, std::ios::binary);
    const std::string second_saved_text((std::istreambuf_iterator<char>(second_stream)), std::istreambuf_iterator<char>());
    std::ifstream recovery_stream(recovery_path, std::ios::binary);
    const std::string recovery_text((std::istreambuf_iterator<char>(recovery_stream)), std::istreambuf_iterator<char>());
    if (!renamed_for_second_save.success || !second_saved.success || recovery_text != saved_text) {
        std::cerr << "Editor scene save did not preserve the previous source revision\n";
        return 18;
    }
    const auto renamed_after_save = editable_model.rename_selected("Unsaved external conflict");
    {
        std::ofstream external_change(scene_path, std::ios::binary | std::ios::trunc);
        external_change << second_saved_text << '\n';
    }
    const auto conflicted_save = editable_model.save_scene();
    std::ifstream preserved_stream(scene_path, std::ios::binary);
    const std::string preserved_text((std::istreambuf_iterator<char>(preserved_stream)), std::istreambuf_iterator<char>());
    if (!renamed_after_save.success || conflicted_save.success || conflicted_save.code != "scene.save-conflict" ||
        preserved_text != second_saved_text + "\n") {
        std::cerr << "Editor scene save overwrote an externally modified source\n";
        return 19;
    }

    const auto save_as_path=verification_root/"save-as.scene.json";
    std::filesystem::remove(save_as_path);
    noemancer::World save_as_world;
    if (!save_as_world.load_scene(noemancer::make_bootstrap_scene_document()).success) return 20;
    noemancer::EditorModel save_as_model(save_as_world,assets);
    const auto save_as=save_as_model.save_scene_as(save_as_path.generic_string());
    const auto dirty_after_save_as=save_as_model.rename_selected("Dirty before open");
    const auto rejected_open=save_as_model.open_scene(scene_path.generic_string());
    const auto undo_dirty=save_as_model.undo();
    save_as_model.refresh();
    const auto opened=save_as_model.open_scene(scene_path.generic_string());
    if (!save_as.success || !std::filesystem::exists(save_as_path) || !dirty_after_save_as.success ||
        rejected_open.success || rejected_open.code!="scene.unsaved-changes" || !undo_dirty.success || !opened.success ||
        save_as_model.scene_source()!=scene_path.generic_string()) {
        std::cerr << "Editor Open/Save As workflow did not preserve unsaved changes or rebind the scene source\n";
        return 21;
    }

    const auto recovery_project=verification_root/"recovery-project";const auto recovery_scenes=recovery_project/"scenes";
    std::filesystem::remove_all(recovery_project);std::filesystem::create_directories(recovery_scenes);
    const auto recovery_target=recovery_scenes/"level.scene.json";const auto recovery_sidecar=std::filesystem::path(
        recovery_target.string()+".noemancer-recovery");
    auto current_scene=noemancer::make_bootstrap_scene_document();current_scene.scene_guid="scene.recovery-test";
    current_scene.name="Current Scene";current_scene.source_uri=recovery_target.generic_string();
    auto recovered_scene=current_scene;recovered_scene.name="Recovered Scene";
    {std::ofstream output(recovery_target,std::ios::binary);output<<noemancer::SceneDocumentCodec::write_canonical_json(current_scene);}
    {std::ofstream output(recovery_sidecar,std::ios::binary);output<<noemancer::SceneDocumentCodec::write_canonical_json(recovered_scene);}
    noemancer::World recovery_world;if(!recovery_world.load_scene(current_scene).success)return 32;
    noemancer::EditorModel recovery_model(recovery_world,assets);
    const auto candidates=nlohmann::json::parse(recovery_model.scene_recovery_candidates_json(recovery_project.generic_string()));
    const auto recovered=recovery_model.recover_scene(recovery_project.generic_string(),"scenes/level.scene.json.noemancer-recovery");
    const auto recovered_saved=recovery_model.save_scene();
    const auto new_scene=recovery_model.new_scene("Fresh Level");
    const auto new_scene_path=recovery_scenes/"fresh.scene.json";const auto new_saved=recovery_model.save_scene_as(new_scene_path.generic_string());
    if(!candidates.at("success")||candidates.at("count")!=1||candidates.at("items").at(0).at("targetPath")!="scenes/level.scene.json"||
       !recovered.success||!recovery_model.scene_source().ends_with("fresh.scene.json")||!recovered_saved.success||!new_scene.success||
       !new_saved.success||recovery_model.scene_dirty()||recovery_world.entity_count()!=3U||
       recovery_world.canonical_scene_json().find("Fresh Level")==std::string::npos) {
        std::cerr<<candidates.dump(2)<<'\n'<<recovered.detail<<'\n'<<new_scene.detail<<'\n';return 32;
    }
    std::filesystem::remove_all(recovery_project);

    noemancer::World multi_world;
    if(!multi_world.load_scene(noemancer::make_bootstrap_scene_document()).success) return 22;
    noemancer::EditorModel multi_model(multi_world,assets);
    if(!multi_model.select_object("entity.demo-cube")||!multi_model.select_object("entity.demo-sphere",true)||
       multi_model.selected_object_ids().size()!=2) return 23;
    const auto multi_initial_count=multi_model.objects().size();const auto multi_revision=multi_world.revision();
    const auto copied=multi_model.copy_selected();const auto pasted=multi_model.paste_copied();
    if(!copied.success||!pasted.success||multi_world.revision()!=multi_revision+1||
       multi_model.objects().size()!=multi_initial_count+2||multi_model.selected_object_ids().size()!=2||
       multi_model.semantic_snapshot_json().find("\"count\":2") == std::string::npos) {
        std::cerr<<"Multi-selection clipboard paste was not one atomic scene transaction\n";return 24;
    }
    const auto pasted_ids=multi_model.selected_object_ids();
    const auto undo_paste=multi_model.undo();multi_model.refresh();
    const auto redo_paste=multi_model.redo();multi_model.refresh();
    if(pasted_ids.size()!=2||!multi_model.select_object(pasted_ids[0])||!multi_model.select_object(pasted_ids[1],true)) return 25;
    const auto deleted_batch=multi_model.delete_selected(true);
    if(!undo_paste.success||!redo_paste.success||!deleted_batch.success||multi_model.objects().size()!=multi_initial_count||
       multi_world.revision()!=multi_revision+4) {
        std::cerr<<"Multi-selection paste/delete did not preserve atomic undo and redo\n";return 26;
    }

    noemancer::World script_authoring_world;
    auto scripted_scene=noemancer::make_bootstrap_scene_document();
    auto scripted_entity=std::ranges::find(scripted_scene.entities,std::string("entity.demo-sphere"),&noemancer::SceneEntityDocument::guid);
    scripted_entity->managed_script=noemancer::SceneManagedScript{"script.editor.catalog","project.script","ManagedFixture.PlayerController",true,"{}"};
    if(!script_authoring_world.load_scene(scripted_scene).success||
       script_authoring_world.scripting_project_configure_json(script_project.parent_path(),script_project).find(R"("success":true)")==std::string::npos||
       script_authoring_world.scripting_project_compile_json("Debug").find(R"("success":true)")==std::string::npos)return 27;
    noemancer::EditorModel script_authoring_model(script_authoring_world,assets);
    if(!script_authoring_model.select_object("entity.demo-sphere"))return 27;
    const auto managed_section=std::ranges::find(script_authoring_model.inspector_sections(),std::string("ManagedScript"),&noemancer::InspectorSection::component);
    if(managed_section==script_authoring_model.inspector_sections().end())return 27;
    const auto type_property=std::ranges::find(managed_section->properties,std::string("engine.entity.managedScript.typeName"),&noemancer::InspectorProperty::property);
    if(type_property==managed_section->properties.end()||type_property->control!="combo"||
       std::ranges::find(type_property->options,std::string("ManagedFixture.PlayerController"))==type_property->options.end()) {
        std::cerr<<"Managed Script Inspector did not consume the reflected type catalog\n";return 27;
    }

    const auto tile_root=std::filesystem::temp_directory_path()/"noemancer-editor-tile-brush-test";
    std::filesystem::remove_all(tile_root);std::filesystem::create_directories(tile_root);
    {std::ofstream output(tile_root/"registry.json");output<<R"({"schema":"noemancer.assets/0.1","assets":[
      {"id":"palette.editor","displayName":"Editor Palette","kind":"TilePalette","uri":"asset://editor.tile-palette.json","path":"editor.tile-palette.json","license":"CC0","redistribution":"allowed"},
      {"id":"tilemap.editor","displayName":"Editor Map","kind":"Tilemap","uri":"asset://editor.tilemap.json","path":"editor.tilemap.json","license":"CC0","redistribution":"allowed"}]})";}
    {std::ofstream output(tile_root/"editor.tile-palette.json");output<<R"({"schema":"noemancer.tile-palette/0.1","assetId":"palette.editor","spriteAsset":"sprite.editor","tiles":[{"id":"ground","frame":"ground.0","collision":"solid","tags":["terrain"]}]})";}
    {std::ofstream output(tile_root/"editor.tilemap.json");output<<R"({"schema":"noemancer.tilemap/0.1","assetId":"tilemap.editor","paletteAsset":"palette.editor","cellSize":[0.5,0.5],"chunkSize":[8,8],"layers":[{"id":"ground","sortingLayer":"terrain","sortingOrder":0,"collisionEnabled":true,"chunks":[]}]})";}
    const auto tile_scene=noemancer::SceneDocumentCodec::parse_json(R"({"schema":"noemancer.scene/0.1","sceneGuid":"scene.editor.tilemap","name":"Tilemap","entities":[{"guid":"entity.editor.tilemap","name":"Tilemap","parent":null,"components":{"Transform":{"position":[2,3,0]},"TilemapRenderer":{"tilemapAsset":"tilemap.editor","visible":true,"collisionEnabled":true}}}]})");
    noemancer::AssetRegistry tile_assets(tile_root);noemancer::World tile_world;
    const auto palette_document=noemancer::TilemapAssetCodec::parse_palette_json(R"({"schema":"noemancer.tile-palette/0.1","assetId":"palette.editor","spriteAsset":"sprite.editor","tiles":[{"id":"ground","frame":"ground.0","collision":"solid","tags":["terrain"]}]})");
    const auto map_document=noemancer::TilemapAssetCodec::parse_tilemap_json(R"({"schema":"noemancer.tilemap/0.1","assetId":"tilemap.editor","paletteAsset":"palette.editor","cellSize":[0.5,0.5],"chunkSize":[8,8],"layers":[{"id":"ground","sortingLayer":"terrain","sortingOrder":0,"collisionEnabled":true,"chunks":[]}]})");
    if(!tile_scene||!palette_document||!map_document||!tile_world.register_tile_palette(*palette_document.document)||
       !tile_world.register_tilemap_asset(*map_document.document)||!tile_world.load_scene(*tile_scene.document).success)return 28;
    noemancer::EditorModel tile_model(tile_world,tile_assets);const auto tile_asset_index=std::ranges::find(tile_model.assets(),std::string("tilemap.editor"),&noemancer::EditorAsset::id);
    if(tile_asset_index==tile_model.assets().end())return 28;tile_model.select_asset(static_cast<std::size_t>(std::distance(tile_model.assets().begin(),tile_asset_index)));
    const auto authoring=tile_model.selected_tilemap_authoring_json();
    const auto fingerprint=noemancer::TilemapAssetCodec::tilemap_fingerprint(*map_document.document);
    const auto tile_receipt=tile_model.apply_selected_tilemap_stroke("ground",{{-2,4,std::string("ground")}},fingerprint,false);
    if(authoring.find("noemancer.tilemap-authoring-document/0.1")==std::string::npos||authoring.find("entity.editor.tilemap")==std::string::npos||
       authoring.find("asset.tilemap.stroke")==std::string::npos||tile_receipt.find(R"("success":true)")==std::string::npos||
       tile_model.semantic_snapshot_json().find("tilemapAuthoring")==std::string::npos) {
        std::cerr<<authoring<<'\n'<<tile_receipt<<'\n';return 28;
    }
    const auto tile_undo=tile_model.undo();{std::ifstream input(tile_root/"editor.tilemap.json");const std::string source{std::istreambuf_iterator<char>(input),{}};
        if(!tile_undo.success||source.find(R"("chunks":[])")==std::string::npos||!tile_model.can_redo())return 29;}
    const auto tile_redo=tile_model.redo();{std::ifstream input(tile_root/"editor.tilemap.json");const std::string source{std::istreambuf_iterator<char>(input),{}};
        if(!tile_redo.success||source.find("ground")==std::string::npos||!tile_model.can_undo())return 29;}
    std::ifstream region_source(tile_root/"editor.tilemap.json");const std::string region_text{std::istreambuf_iterator<char>(region_source),{}};region_source.close();
    const auto region_document=noemancer::TilemapAssetCodec::parse_tilemap_json(region_text);if(!region_document)return 30;
    const auto region_fingerprint=noemancer::TilemapAssetCodec::tilemap_fingerprint(*region_document.document);
    const auto region_receipt=tile_model.apply_selected_tilemap_region("rectangle","ground",{0,0},std::array<std::int32_t,2>{1,1},
        std::string("ground"),false,false,region_fingerprint,false);
    if(region_receipt.find(R"("success":true)")==std::string::npos||region_receipt.find("asset.tilemap.region")==std::string::npos)return 30;
    const auto authoring_after_region=tile_model.selected_tilemap_authoring_json();
    const auto palette_fingerprint=noemancer::TilemapAssetCodec::palette_fingerprint(*palette_document.document);
    const auto palette_preview=tile_model.apply_selected_tile_palette_autotile("ground","terrain",{{2,"ground.0"},{8,"ground.0"}},palette_fingerprint,true);
    const auto palette_commit=tile_model.apply_selected_tile_palette_autotile("ground","terrain",{{2,"ground.0"},{8,"ground.0"}},palette_fingerprint,false);
    std::ifstream palette_source_stream(tile_root/"editor.tile-palette.json");const std::string palette_source{std::istreambuf_iterator<char>(palette_source_stream),{}};palette_source_stream.close();
    if(authoring_after_region.find("asset.tile-palette.autotile")==std::string::npos||
       palette_preview.find(R"("success":true)")==std::string::npos||palette_commit.find(R"("success":true)")==std::string::npos||
       palette_source.find("autotile") == std::string::npos)return 31;
    std::filesystem::remove_all(tile_root);

    const auto graph_root=std::filesystem::temp_directory_path()/"noemancer-editor-animation-graph-test";
    std::filesystem::remove_all(graph_root);std::filesystem::create_directories(graph_root);
    {std::ofstream output(graph_root/"registry.json");output<<R"({"schema":"noemancer.assets/0.1","assets":[
      {"id":"clip.idle","displayName":"Idle","kind":"Animation","uri":"builtin://animation/test-bob","license":"CC0","redistribution":"allowed"},
      {"id":"clip.run","displayName":"Run","kind":"Animation","uri":"builtin://animation/test-bob","license":"CC0","redistribution":"allowed"},
      {"id":"graph.editor","displayName":"Editor Graph","kind":"AnimationGraph","uri":"asset://editor.animation-graph.json","path":"editor.animation-graph.json","license":"CC0","redistribution":"allowed"}]})";}
    {std::ofstream output(graph_root/"editor.animation-graph.json");output<<R"({"schemaVersion":"noemancer.animation-graph/0.1","assetId":"graph.editor",
      "parameters":[{"id":"speed","type":"float","default":0}],
      "nodes":[{"id":"idle","kind":"clip","clipAsset":"clip.idle","looping":true},{"id":"run","kind":"clip","clipAsset":"clip.run","looping":true},
        {"id":"move","kind":"blend-1d","parameter":"speed","children":[{"nodeId":"idle","threshold":0},{"nodeId":"run","threshold":1}]}],
      "layers":[{"id":"base","rootNode":"move","mode":"override","weight":1}],"masks":[],"syncGroups":[],
      "editor":{"nodes":[{"id":"move","position":[20,30],"collapsed":false}],"zoom":1,"pan":[0,0]}})";}
    noemancer::AssetRegistry graph_assets(graph_root);noemancer::World graph_world;
    if(!graph_world.load_scene(noemancer::make_bootstrap_scene_document()).success)return 35;
    noemancer::EditorModel graph_model(graph_world,graph_assets);
    const auto graph_asset_index=std::ranges::find(graph_model.assets(),std::string("graph.editor"),&noemancer::EditorAsset::id);
    if(graph_asset_index==graph_model.assets().end())return 35;
    graph_model.select_asset(static_cast<std::size_t>(std::distance(graph_model.assets().begin(),graph_asset_index)));
    const auto graph_authoring=nlohmann::json::parse(graph_model.selected_animation_graph_authoring_json());
    graph_model.set_focused_panel("editor.panel.animation-graph");
    if(!graph_authoring.value("valid",false)||graph_authoring.at("canvas").at("nodes").size()!=3U||
       graph_model.semantic_snapshot_json().find("animationGraphAuthoring")==std::string::npos||
       graph_model.semantic_snapshot_json().find(R"("focusedPanel":"editor.panel.animation-graph")")==std::string::npos||
       graph_model.focused_observation_json().find(R"("assetId":"graph.editor")")==std::string::npos)return 35;
    const auto graph_noop=graph_model.apply_selected_animation_graph_patch(
        {noemancer::AnimationGraphPatchOperation::set_node_position("move",20.0F,30.0F)},
        graph_authoring.at("fingerprint").get<std::string>(),false);
    if(graph_noop.find(R"("success":true)")==std::string::npos||graph_model.can_undo())return 35;
    const auto graph_receipt=graph_model.apply_selected_animation_graph_patch(
        {noemancer::AnimationGraphPatchOperation::set_node_position("move",120.0F,240.0F)},
        graph_authoring.at("fingerprint").get<std::string>(),false);
    if(graph_receipt.find(R"("success":true)")==std::string::npos||!graph_model.undo().success||!graph_model.redo().success)return 36;
    const auto graph_after=nlohmann::json::parse(graph_model.selected_animation_graph_authoring_json());
    const auto& layouts=graph_after.at("document").at("editor").at("nodes");
    const auto moved=std::ranges::find_if(layouts,[](const auto& value){return value.value("id",std::string{})=="move";});
    if(moved==layouts.end()||moved->at("position")!=nlohmann::json::array({120.0F,240.0F}))return 36;
    const auto graph_topology_receipt=graph_model.apply_selected_animation_graph_patch({
        noemancer::AnimationGraphPatchOperation::create_clip_node("walk","clip.run"),
        noemancer::AnimationGraphPatchOperation::connect_blend_1d_child("move","walk",0.5F),
        noemancer::AnimationGraphPatchOperation::set_node_position("walk",340.0F,240.0F)},
        graph_after.at("fingerprint").get<std::string>(),false);
    const auto graph_topology=nlohmann::json::parse(graph_model.selected_animation_graph_authoring_json());
    const auto& topology_nodes=graph_topology.at("document").at("nodes");
    const auto walk=std::ranges::find_if(topology_nodes,[](const auto& value){return value.value("id",std::string{})=="walk";});
    if(graph_topology_receipt.find(R"("success":true)")==std::string::npos||walk==topology_nodes.end()||
       graph_topology.at("tool").at("operations").size()!=8U||!graph_model.can_undo())return 37;
    std::filesystem::remove_all(graph_root);

    const auto ordinary_asset_root=std::filesystem::temp_directory_path()/"noemancer-editor-ordinary-asset-test";
    std::filesystem::remove_all(ordinary_asset_root);std::filesystem::create_directories(ordinary_asset_root);
    {std::ofstream output(ordinary_asset_root/"registry.json");output<<R"({"schema":"noemancer.assets/0.1","assets":[
      {"id":"texture.ordinary","displayName":"Ordinary Texture","kind":"Texture","uri":"asset://ordinary.bin","path":"ordinary.bin","license":"CC0","redistribution":"allowed"}]})";}
    {std::ofstream output(ordinary_asset_root/"ordinary.bin",std::ios::binary);output<<"not imported metadata";}
    noemancer::AssetRegistry ordinary_assets(ordinary_asset_root);noemancer::World ordinary_world;
    if(!ordinary_world.load_scene(noemancer::make_bootstrap_scene_document()).success)return 34;
    noemancer::EditorModel ordinary_model(ordinary_world,ordinary_assets);
    const auto ordinary_authoring=ordinary_model.selected_tilemap_authoring_json();
    const auto ordinary_animation_authoring=ordinary_model.selected_animation_graph_authoring_json();
    const auto ordinary_snapshot=ordinary_model.semantic_snapshot_json();
    if(ordinary_authoring.find(R"("valid":false)")==std::string::npos||
       ordinary_authoring.find("editor.tilemap-not-selected")==std::string::npos||
       ordinary_animation_authoring.find("editor.animation-graph-not-selected")==std::string::npos||
       ordinary_snapshot.find("tilemapAuthoring")==std::string::npos) {
        std::cerr<<"Ordinary assets were not safely represented as non-tilemap authoring selections\n";return 34;
    }
    std::filesystem::remove_all(ordinary_asset_root);

    return 0;
}
