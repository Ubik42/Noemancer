#include "engine/scene_document.hpp"
#include "engine/semantic_ui.hpp"
#include "engine/world.hpp"

#include <nlohmann/json.hpp>

#include <cmath>
#include <iostream>

int main() {
    noemancer::World world;
    if (!world.load_scene(noemancer::make_bootstrap_scene_document()).success) return 1;

    const auto document = nlohmann::json::parse(world.semantic_ui_document_json("entity.demo-cube", "zh-CN"));
    bool revision_bound{};
    for(const auto& node:document.at("nodes"))if(node.value("binding",nlohmann::json::object()).value("revision",0ULL)==world.revision())revision_bound=true;
    if(!revision_bound)return 14;
    if (!document.at("valid").get<bool>() || document.at("schemaVersion") != "noemancer.ui-document/0.1" ||
        document.at("documentId") != "editor.inspector.entity.demo-cube" || document.at("locale") != "zh-CN" ||
        !document.at("validation").at("valid").get<bool>() || document.at("nodes").size() < 10) {
        std::cerr << "Inspector did not project to a valid generic Semantic UI document\n";
        return 2;
    }
    if(document.at("themeId")!="theme.noemancer-dark"||!document.contains("resourceRevision")||
       !document.at("capabilities").at("localizationDiagnostics")||
       document.at("nodes").at(0).at("label")!="\xE6\xA3\x80\xE6\x9F\xA5\xE5\x99\xA8"||
       document.at("localizationDiagnostics").at("requiredScript")!="Han"||
       !document.at("localizationDiagnostics").at("missingGlyphRisk")) return 8;
    const auto arabic=nlohmann::json::parse(world.semantic_ui_document_json("entity.demo-cube","ar-SA"));
    if(arabic.at("textDirection")!="rtl"||
       arabic.at("nodes").at(0).at("label")!="\xD9\x84\xD9\x88\xD8\xAD\xD8\xA9\x20\xD8\xA7\xD9\x84\xD8\xAE\xD8\xB5\xD8\xA7\xD8\xA6\xD8\xB5"||
       !arabic.at("localizationDiagnostics").at("bidirectionalLayoutRequired").get<bool>()||
       arabic.at("localizationDiagnostics").at("runtimeCapabilityTool")!="ui.text.inspect"||
       !arabic.at("localizationDiagnostics").at("rendererNeutralShapingPlanReady").get<bool>()||
       !arabic.at("localizationDiagnostics").at("retainedGlyphRunConsumerReady").get<bool>()) return 16;
    const auto resources=nlohmann::json::parse(noemancer::semantic_ui_resource_status_json("qps-ploc"));
    if(!resources.at("valid")||!resources.at("requestedMessagesLoaded")||!resources.at("stylesheetLoaded")||
       resources.at("reloadModel")!="poll-fingerprint-and-reload-document") return 9;

    const auto roughness_id = "editor.inspector.entity.demo-cube.PbrMaterial.roughness";
    noemancer::SemanticUiQuery focused{.node_ids = {roughness_id}, .depth = 0, .byte_budget = 8 * 1024};
    const auto observation = nlohmann::json::parse(world.semantic_ui_observation_json("entity.demo-cube", focused, "zh-CN"));
    if (!observation.at("valid").get<bool>() || observation.at("code") != "ok" || observation.at("returnedNodes") != 3 ||
        observation.dump().find("world.property.plan") == std::string::npos ||
        observation.dump().find("engine.entity.material.roughness") == std::string::npos ||
        observation.at("nodes").back().at("label")!="\xE7\xB2\x97\xE7\xB3\x99\xE5\xBA\xA6" ||
        observation.dump().find("fnv1a64:") == std::string::npos ||
        observation.at("document").at("contentFingerprint").get<std::string>().find("fnv1a64:") != 0 ||
        observation.at("document").at("localizationDiagnostics").at("requiredScript")!="Han") {
        std::cerr << "Focused UI observation did not preserve ancestors, binding, action and fingerprint\n";
        return 3;
    }

    noemancer::SemanticUiQuery redacted{.roles = {"property"}, .depth = 0, .byte_budget = 64 * 1024,
                                         .include_values = false};
    const auto redacted_observation = nlohmann::json::parse(world.semantic_ui_observation_json("entity.demo-cube", redacted));
    for (const auto& node : redacted_observation.at("nodes"))
        if (node.at("role") == "property" && node.contains("value")) return 4;

    noemancer::SemanticUiQuery bounded{.depth = 2, .byte_budget = 512};
    const auto bounded_observation = nlohmann::json::parse(world.semantic_ui_observation_json("entity.demo-cube", bounded));
    if (bounded_observation.at("code") != "ui.byte-budget-too-small" ||
        bounded_observation.at("minimumRequiredBytes").get<std::size_t>() <= 512) return 5;

    auto duplicate = document;
    duplicate.at("nodes").push_back(duplicate.at("nodes").front());
    const auto invalid = nlohmann::json::parse(noemancer::semantic_ui_validation_json(duplicate.dump()));
    if (invalid.at("valid").get<bool>() || invalid.dump().find("ui.duplicate-node-id") == std::string::npos) return 6;

    const auto delta_base = world.revision();
    const auto first_plan = world.plan_property_update(
        "entity.demo-cube", "engine.entity.material.roughness", "0.25", world.revision(), "test.ui-delta.first");
    if (!world.apply_property_plan(first_plan, false).success) return 10;
    const auto second_plan = world.plan_property_update(
        "entity.demo-cube", "engine.entity.material.roughness", "0.4", world.revision(), "test.ui-delta.second");
    if (!world.apply_property_plan(second_plan, false).success) return 11;
    const auto delta = nlohmann::json::parse(world.semantic_ui_delta_json(
        "entity.demo-cube", delta_base,
        noemancer::SemanticUiDeltaQuery{.byte_budget = 8 * 1024,
            .base_fingerprint = observation.at("document").at("contentFingerprint")}, "zh-CN"));
    if (!delta.at("valid").get<bool>() || delta.at("resyncRequired").get<bool>() ||
        delta.at("totalChanges") != 1 || delta.at("compression").at("sourceChanges") != 2 ||
        delta.at("changes").at(0).at("nodeId") != roughness_id ||
        std::abs(delta.at("changes").at(0).at("before").get<double>() - 0.42) > 0.0001 ||
        std::abs(delta.at("changes").at(0).at("after").get<double>() - 0.4) > 0.0001 ||
        std::abs(delta.at("changes").at(0).at("currentValue").get<double>() - 0.4) > 0.0001 ||
        delta.at("changes").at(0).at("managers").size() != 2 ||
        delta.at("document").at("contentFingerprint").get<std::string>().find("fnv1a64:") != 0) {
        std::cerr << "UI delta did not project and coalesce World property changes by stable node ID: "
                  << delta.dump(2) << '\n';
        return 12;
    }
    const auto redacted_delta = nlohmann::json::parse(world.semantic_ui_delta_json(
        "entity.demo-cube", delta_base, noemancer::SemanticUiDeltaQuery{.byte_budget = 8 * 1024, .include_values = false}));
    if (redacted_delta.at("changes").at(0).contains("before") ||
        redacted_delta.at("changes").at(0).contains("currentValue")) return 13;
    const auto resync_delta = nlohmann::json::parse(world.semantic_ui_delta_json(
        "entity.demo-cube", world.revision() + 1, noemancer::SemanticUiDeltaQuery{}));
    if (!resync_delta.at("resyncRequired").get<bool>() || resync_delta.at("code") != "ui.resync-required" ||
        resync_delta.at("resync").at("command") != "ui.observe") return 14;
    const auto fingerprint_resync = nlohmann::json::parse(world.semantic_ui_delta_json(
        "entity.demo-cube", world.revision(),
        noemancer::SemanticUiDeltaQuery{.base_fingerprint = "fnv1a64:0000000000000000"}));
    if (!fingerprint_resync.at("resyncRequired").get<bool>() ||
        fingerprint_resync.at("resyncReason") != "content-fingerprint-mismatch") return 15;

    static_cast<void>(world.gameplay_ability_grant_json("entity.demo-cube","ability.combat.impact"));
    const auto hud=nlohmann::json::parse(noemancer::semantic_ui_game_hud_document(
        world.gameplay_ability_observation_json("entity.demo-cube"),"entity.demo-cube","en-US"));
    if (!hud.at("valid").get<bool>() || hud.at("surface")!="game" || hud.at("kind")!="hud" ||
        !hud.at("validation").at("valid").get<bool>() || hud.dump().find("gameplay-attribute")==std::string::npos ||
        hud.dump().find("gameplay.ability.activate")==std::string::npos || !hud.contains("designTokens")) {
        std::cerr << "Gameplay state did not project to a shared Semantic UI HUD document\n";
        return 7;
    }
    const auto project_hud=noemancer::semantic_ui_project_runtime_document(
        R"({"schemaVersion":"noemancer.ui-document/0.1","documentId":"project.hud","revision":7,"components":[{"id":"control.readout","role":"property","presentation":{"control":"label","constraints":{"compact":true}},"state":{"visible":true,"enabled":true,"editable":false}}],"nodes":[{"id":"project.hud","parentId":null,"role":"hud","label":"Project"},{"id":"project.hud.score","parentId":"project.hud","componentRef":"control.readout","label":"Score","value":0,"binding":{"kind":"script-state","instanceId":"script.player","member":"Score","fallback":0}}],"designTokens":{"accentColor":"#abcdef"}})",
        R"({"revision":4,"instances":[{"id":"script.player","publicState":{"Score":12}}]})",
        R"({"revision":5,"actions":[]})",R"({"revision":3,"actors":[]})","en-US");
    const auto projected=nlohmann::json::parse(project_hud);
    if(!projected.at("valid")||projected.at("revision")!=5U||projected.at("nodes").at(1).at("value")!=12||
        !projected.at("nodes").at(1).at("bindingState").at("resolved")||
        projected.at("designTokens").at("accentColor")!="#abcdef"||
        projected.at("sourceDocumentRevision")!=7U||projected.at("nodes").at(1).at("role")!="property"||
        projected.at("nodes").at(1).at("presentation").at("control")!="label"||
        projected.at("nodes").at(1).at("componentChain").at(0)!="control.readout") {
        std::cerr << "Resolved project UI projection mismatch: " << projected.dump(2) << '\n';
        return 17;
    }

    return 0;
}
