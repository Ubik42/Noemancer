#include "engine/shadow_scalability_stress_scene.hpp"

#include <algorithm>
#include <iostream>
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>

namespace {

using namespace noemancer;
using Json = nlohmann::json;

bool check(const bool condition, const std::string_view message) {
    if (!condition) std::cerr << "shadow_scalability_stress_scene_tests: " << message << '\n';
    return condition;
}

const SceneEntityDocument* find_entity(const SceneDocument& document, const std::string& id) {
    const auto iterator = std::ranges::find(document.entities, id, &SceneEntityDocument::guid);
    return iterator == document.entities.end() ? nullptr : &*iterator;
}

bool test_default_scene_and_contract_round_trip() {
    const auto result = build_shadow_scalability_stress_scene();
    if (!check(result.valid && result.document.has_value() && result.contract.has_value(),
               "default stress scene did not build")) return false;
    const auto& document = *result.document;
    const auto& contract = *result.contract;
    if (!check(SceneDocumentCodec::validate(document).empty(),
               "generated scene failed SceneDocument validation")) return false;
    if (!check(contract.pressure_over_capacity &&
                   contract.expected_caster_count == 128U &&
                   contract.expected_control_count == 4U &&
                   contract.expected_requested_shadow_count == 7U &&
                   contract.expected_selected_shadow_count == 4U &&
                   contract.expected_dropped_shadow_count == 3U &&
                   contract.expected_requested_local_shadow_layers == 16U &&
                   contract.expected_selected_local_shadow_layers == 8U &&
                   contract.expected_dropped_local_shadow_layers == 8U,
               "default pressure arithmetic does not describe the current local atlas")) return false;
    if (!check(document.entities.size() == 141U &&
                   document.entities.front().guid == "entity.shadow-scalability-root",
               "generated scene entity count or root identity drifted")) return false;

    const auto* caster = find_entity(document, contract.caster_ids.front());
    const auto* control = find_entity(document, contract.control_ids.front());
    const auto* point = find_entity(document, contract.point_light_ids.front());
    const auto* spot = find_entity(document, contract.spot_light_ids.front());
    const auto* camera = find_entity(document, contract.camera_id);
    const auto* sun = find_entity(document, contract.directional_light_id);
    if (!check(caster != nullptr && caster->mesh_renderer.has_value() &&
                   caster->mesh_renderer->casts_shadows && !caster->rigid_body.has_value(),
               "caster fixture is not a static shadow caster")) return false;
    if (!check(control != nullptr && control->mesh_renderer.has_value() &&
                   !control->mesh_renderer->casts_shadows,
               "control fixture is not an explicit non-caster")) return false;
    if (!check(point != nullptr && point->local_light.has_value() &&
                   point->local_light->kind == "point" && point->local_light->casts_shadows &&
                   spot != nullptr && spot->local_light.has_value() &&
                   spot->local_light->kind == "spot" && spot->local_light->casts_shadows,
               "point/spot shadow requests are not represented in the scene")) return false;
    if (!check(camera != nullptr && camera->camera.has_value() && camera->camera->primary &&
                   sun != nullptr && sun->directional_light.has_value() &&
                   sun->directional_light->casts_shadows,
               "fixed camera or directional shadow light is missing")) return false;

    const auto scene_json = SceneDocumentCodec::write_canonical_json(document);
    const auto scene_round_trip = SceneDocumentCodec::parse_json(scene_json);
    if (!check(scene_round_trip &&
                   SceneDocumentCodec::write_canonical_json(*scene_round_trip.document) == scene_json,
               "scene canonical JSON did not round-trip")) return false;
    const auto contract_json = write_shadow_scalability_stress_scene_contract_json(contract);
    const auto contract_round_trip = parse_shadow_scalability_stress_scene_contract_json(contract_json);
    return check(contract_round_trip &&
                     write_shadow_scalability_stress_scene_contract_json(
                         *contract_round_trip.contract) == contract_json,
                 "stress contract canonical JSON did not round-trip");
}

bool test_caster_boundaries_and_invalid_parameters() {
    ShadowScalabilityStressSceneConfig minimum;
    minimum.caster_count = shadow_scalability_stress_scene_min_casters;
    const auto min_result = build_shadow_scalability_stress_scene(minimum);
    if (!check(min_result.valid && min_result.contract->caster_ids.size() == 32U,
               "inclusive minimum caster boundary was rejected")) return false;

    ShadowScalabilityStressSceneConfig maximum;
    maximum.caster_count = shadow_scalability_stress_scene_max_casters;
    maximum.control_object_count = 1U;
    const auto max_result = build_shadow_scalability_stress_scene(maximum);
    if (!check(max_result.valid &&
                   max_result.contract->caster_ids.size() == 4096U &&
                   max_result.document->entities.size() == 3U + 4096U + 6U + 1U,
               "inclusive maximum caster boundary was rejected")) return false;

    auto below = minimum;
    below.caster_count = shadow_scalability_stress_scene_min_casters - 1U;
    if (!check(!build_shadow_scalability_stress_scene(below).valid,
               "caster count below the bounded minimum was accepted")) return false;
    auto above = minimum;
    above.caster_count = shadow_scalability_stress_scene_max_casters + 1U;
    if (!check(!build_shadow_scalability_stress_scene(above).valid,
               "caster count above the bounded maximum was accepted")) return false;
    auto no_point = minimum;
    no_point.point_light_count = 0U;
    if (!check(!build_shadow_scalability_stress_scene(no_point).valid,
               "zero point requests were accepted")) return false;
    auto no_spot = minimum;
    no_spot.spot_light_count = 0U;
    if (!check(!build_shadow_scalability_stress_scene(no_spot).valid,
               "zero spot requests were accepted")) return false;
    auto no_control = minimum;
    no_control.control_object_count = 0U;
    return check(!build_shadow_scalability_stress_scene(no_control).valid,
                  "a stress scene without a control object was accepted");
}

bool test_stable_workload_identity_and_contract_rejection() {
    ShadowScalabilityStressSceneConfig config;
    config.caster_count = 64U;
    config.point_light_count = 3U;
    config.spot_light_count = 5U;
    config.control_object_count = 6U;
    const auto first = build_shadow_scalability_stress_scene(config);
    const auto second = build_shadow_scalability_stress_scene(config);
    if (!check(first.valid && second.valid && first.contract->workload_identity ==
                   second.contract->workload_identity &&
                   write_shadow_scalability_stress_scene_contract_json(*first.contract) ==
                   write_shadow_scalability_stress_scene_contract_json(*second.contract) &&
                   SceneDocumentCodec::write_canonical_json(*first.document) ==
                   SceneDocumentCodec::write_canonical_json(*second.document),
               "equivalent pressure scenes were not deterministic")) return false;

    auto changed = config;
    changed.caster_count = 65U;
    const auto changed_result = build_shadow_scalability_stress_scene(changed);
    if (!check(changed_result.valid && changed_result.contract->workload_identity !=
                   first.contract->workload_identity,
               "workload identity did not change with the workload")) return false;

    auto malformed = Json::parse(
        write_shadow_scalability_stress_scene_contract_json(*first.contract));
    malformed["expected"]["droppedShadowCount"] =
        malformed["expected"]["droppedShadowCount"].get<std::uint32_t>() + 1U;
    const auto rejected_counts = parse_shadow_scalability_stress_scene_contract_json(
        malformed.dump());
    if (!check(!rejected_counts && !rejected_counts.errors.empty(),
               "inconsistent pressure counts were accepted")) return false;

    malformed = Json::parse(write_shadow_scalability_stress_scene_contract_json(*first.contract));
    malformed["schema"] = "noemancer.shadow-scalability-stress/9.9";
    const auto rejected_schema = parse_shadow_scalability_stress_scene_contract_json(
        malformed.dump());
    if (!check(!rejected_schema && !rejected_schema.errors.empty(),
               "unsupported stress contract schema was accepted")) return false;

    const std::string oversized(shadow_scalability_stress_scene_max_contract_bytes + 1U, 'x');
    const auto rejected_size = parse_shadow_scalability_stress_scene_contract_json(oversized);
    return check(!rejected_size && !rejected_size.errors.empty(),
                 "oversized stress contract was accepted");
}

} // namespace

int main() {
    if (!test_default_scene_and_contract_round_trip()) return 1;
    if (!test_caster_boundaries_and_invalid_parameters()) return 2;
    if (!test_stable_workload_identity_and_contract_rejection()) return 3;
    std::cout << "shadow_scalability_stress_scene_tests: ok\n";
    return 0;
}
