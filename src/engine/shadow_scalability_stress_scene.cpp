#include "engine/shadow_scalability_stress_scene.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <nlohmann/json.hpp>
#include <ranges>
#include <string>
#include <unordered_set>
#include <utility>

namespace noemancer {
namespace {

using Json = nlohmann::json;

constexpr std::string_view scene_guid = "scene.shadow-scalability-stress";
constexpr std::string_view root_id = "entity.shadow-scalability-root";
constexpr std::string_view camera_id = "entity.shadow-scalability-camera";
constexpr std::string_view directional_light_id = "entity.shadow-scalability-sun";
constexpr std::uint32_t point_shadow_face_count = 6U;

void add_error(std::vector<ShadowScalabilityStressSceneContractError>& errors,
               std::string code, std::string path, std::string message) {
    if (errors.size() >= 32U) return;
    errors.push_back({std::move(code), std::move(path), std::move(message)});
}

bool allowed_field(const std::string_view name,
                   const std::initializer_list<std::string_view> allowed) {
    return std::ranges::any_of(allowed, [name](const std::string_view candidate) {
        return candidate == name;
    });
}

void reject_unknown_fields(
    const Json& object,
    const std::initializer_list<std::string_view> allowed,
    const std::string_view path,
    std::vector<ShadowScalabilityStressSceneContractError>& errors) {
    if (!object.is_object()) return;
    for (const auto& [name, unused] : object.items()) {
        static_cast<void>(unused);
        if (!allowed_field(name, allowed)) {
            add_error(errors, "shadow-scalability-stress.unknown-field",
                      std::string(path) + "/" + name,
                      "Unknown contract fields are rejected to preserve the workload meaning.");
        }
    }
}

bool read_required_string(
    const Json& object, const std::string_view name, const std::string_view path,
    std::string& output,
    std::vector<ShadowScalabilityStressSceneContractError>& errors) {
    if (!object.contains(name) || !object.at(name).is_string()) {
        add_error(errors, "shadow-scalability-stress.invalid-string",
                  std::string(path) + "/" + std::string(name),
                  "Expected a non-empty bounded string.");
        return false;
    }
    output = object.at(name).get<std::string>();
    if (output.empty() || output.size() > shadow_scalability_stress_scene_max_text_bytes) {
        add_error(errors, "shadow-scalability-stress.string-range",
                  std::string(path) + "/" + std::string(name),
                  "String is empty or exceeds the bounded contract length.");
        return false;
    }
    return true;
}

bool read_required_uint32(
    const Json& object, const std::string_view name, const std::string_view path,
    std::uint32_t& output,
    std::vector<ShadowScalabilityStressSceneContractError>& errors) {
    const auto full_path = std::string(path) + "/" + std::string(name);
    if (!object.contains(name) || !object.at(name).is_number_integer()) {
        add_error(errors, "shadow-scalability-stress.invalid-integer", full_path,
                  "Expected a non-negative bounded integer.");
        return false;
    }
    const auto value = object.at(name).get<std::int64_t>();
    if (value < 0 || static_cast<std::uint64_t>(value) >
                         static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max())) {
        add_error(errors, "shadow-scalability-stress.integer-range", full_path,
                  "Integer is outside the uint32 range.");
        return false;
    }
    output = static_cast<std::uint32_t>(value);
    return true;
}

bool read_required_bool(
    const Json& object, const std::string_view name, const std::string_view path,
    bool& output,
    std::vector<ShadowScalabilityStressSceneContractError>& errors) {
    if (!object.contains(name) || !object.at(name).is_boolean()) {
        add_error(errors, "shadow-scalability-stress.invalid-boolean",
                  std::string(path) + "/" + std::string(name),
                  "Expected a boolean.");
        return false;
    }
    output = object.at(name).get<bool>();
    return true;
}

bool read_id_array(
    const Json& object, const std::string_view name, const std::string_view path,
    const std::size_t maximum, std::vector<std::string>& output,
    std::vector<ShadowScalabilityStressSceneContractError>& errors) {
    const auto full_path = std::string(path) + "/" + std::string(name);
    if (!object.contains(name) || !object.at(name).is_array()) {
        add_error(errors, "shadow-scalability-stress.invalid-id-array", full_path,
                  "Expected an array of stable entity IDs.");
        return false;
    }
    const auto& values = object.at(name);
    if (values.size() > maximum) {
        add_error(errors, "shadow-scalability-stress.id-array-range", full_path,
                  "The ID array exceeds the bounded workload size.");
        return false;
    }
    output.clear();
    output.reserve(values.size());
    std::unordered_set<std::string> seen;
    for (std::size_t index = 0U; index < values.size(); ++index) {
        const auto item_path = full_path + "/" + std::to_string(index);
        if (!values[index].is_string()) {
            add_error(errors, "shadow-scalability-stress.invalid-id", item_path,
                      "Every workload ID must be a string.");
            continue;
        }
        const auto value = values[index].get<std::string>();
        if (value.empty() || value.size() > shadow_scalability_stress_scene_max_text_bytes) {
            add_error(errors, "shadow-scalability-stress.id-range", item_path,
                      "Workload IDs must be non-empty and bounded.");
            continue;
        }
        if (!seen.insert(value).second) {
            add_error(errors, "shadow-scalability-stress.duplicate-id", item_path,
                      "An ID occurs more than once in one workload category.");
            continue;
        }
        output.push_back(value);
    }
    return true;
}

std::string bounded_text(const std::string_view value) {
    return std::string(value.substr(0U, shadow_scalability_stress_scene_max_text_bytes));
}

Json bounded_id_array(const std::vector<std::string>& values, const std::size_t maximum) {
    std::vector<std::string> sorted;
    sorted.reserve(std::min(values.size(), maximum));
    for (std::size_t index = 0U; index < values.size() && index < maximum; ++index)
        sorted.push_back(bounded_text(values[index]));
    std::ranges::sort(sorted);
    Json output = Json::array();
    for (const auto& value : sorted) output.push_back(value);
    return output;
}

bool validate_config(const ShadowScalabilityStressSceneConfig& config,
                     std::string& code, std::string& detail) {
    if (config.caster_count < shadow_scalability_stress_scene_min_casters ||
        config.caster_count > shadow_scalability_stress_scene_max_casters) {
        code = "shadow-scalability-stress.caster-count-range";
        detail = "caster_count must be within the inclusive 32..4096 stress range.";
        return false;
    }
    if (config.point_light_count == 0U ||
        config.point_light_count > shadow_scalability_stress_scene_max_local_lights) {
        code = "shadow-scalability-stress.point-light-count-range";
        detail = "point_light_count must be within the bounded 1..128 range.";
        return false;
    }
    if (config.spot_light_count == 0U ||
        config.spot_light_count > shadow_scalability_stress_scene_max_local_lights) {
        code = "shadow-scalability-stress.spot-light-count-range";
        detail = "spot_light_count must be within the bounded 1..128 range.";
        return false;
    }
    if (config.point_light_count > shadow_scalability_stress_scene_max_local_lights -
                                      config.spot_light_count) {
        code = "shadow-scalability-stress.local-light-count-range";
        detail = "The combined point and spot requests exceed the bounded 128-light workload.";
        return false;
    }
    if (config.control_object_count == 0U ||
        config.control_object_count > shadow_scalability_stress_scene_max_controls) {
        code = "shadow-scalability-stress.control-count-range";
        detail = "control_object_count must be within the bounded 1..128 range.";
        return false;
    }
    if (config.point_light_capacity == 0U ||
        config.point_light_capacity > shadow_scalability_stress_scene_max_capacity) {
        code = "shadow-scalability-stress.point-capacity-range";
        detail = "point_light_capacity must be within the bounded 1..64 range.";
        return false;
    }
    if (config.spot_light_capacity == 0U ||
        config.spot_light_capacity > shadow_scalability_stress_scene_max_capacity) {
        code = "shadow-scalability-stress.spot-capacity-range";
        detail = "spot_light_capacity must be within the bounded 1..64 range.";
        return false;
    }
    if (config.local_atlas_layer_capacity == 0U ||
        config.local_atlas_layer_capacity > shadow_scalability_stress_scene_max_capacity) {
        code = "shadow-scalability-stress.atlas-capacity-range";
        detail = "local_atlas_layer_capacity must be within the bounded 1..64 range.";
        return false;
    }
    return true;
}

std::string make_workload_identity(const ShadowScalabilityStressSceneConfig& config) {
    return "shadow-scalability-stress.v1.casters-" + std::to_string(config.caster_count) +
        ".points-" + std::to_string(config.point_light_count) +
        ".spots-" + std::to_string(config.spot_light_count) +
        ".controls-" + std::to_string(config.control_object_count) +
        ".atlas-" + std::to_string(config.local_atlas_layer_capacity) +
        ".point-cap-" + std::to_string(config.point_light_capacity) +
        ".spot-cap-" + std::to_string(config.spot_light_capacity);
}

std::uint32_t selected_point_lights(const ShadowScalabilityStressSceneConfig& config) {
    return std::min(config.point_light_count,
                    std::min(config.point_light_capacity,
                             config.local_atlas_layer_capacity / point_shadow_face_count));
}

std::uint32_t selected_spot_lights(const ShadowScalabilityStressSceneConfig& config,
                                   const std::uint32_t selected_points) {
    const auto used_layers = selected_points * point_shadow_face_count;
    const auto remaining_layers = config.local_atlas_layer_capacity - used_layers;
    return std::min({config.spot_light_count, config.spot_light_capacity, remaining_layers});
}

SceneEntityDocument make_caster(const std::uint32_t index, const std::uint32_t columns,
                                const double spacing, const double center) {
    const auto column = index % columns;
    const auto row = index / columns;
    const auto color_component = [](const std::uint32_t value, const std::uint32_t modulus,
                                    const double minimum, const double range) {
        return minimum + range * static_cast<double>(value % modulus) /
            static_cast<double>(modulus - 1U);
    };
    return SceneEntityDocument{
        .guid = "entity.shadow-scalability-caster-" + std::to_string(index),
        .name = "Shadow Caster Fixture " + std::to_string(index),
        .parent_guid = std::string(root_id),
        .transform = SceneTransform{
            { (static_cast<double>(column) - center) * spacing, 0.85,
              (static_cast<double>(row) - center) * spacing },
            { 0.75 + 0.15 * static_cast<double>((index * 13U) % 5U),
              0.70 + 0.18 * static_cast<double>((index * 7U) % 4U),
              0.75 + 0.15 * static_cast<double>((index * 19U) % 5U) },
            { 0.0, static_cast<double>((index * 23U) % 360U), 0.0 }},
        .mesh_renderer = SceneMeshRenderer{"asset.primitive.cube", true, true, true},
        .pbr_material = ScenePbrMaterial{
            { color_component(index * 17U, 97U, 0.18, 0.62),
              color_component(index * 31U, 89U, 0.16, 0.68),
              color_component(index * 47U, 83U, 0.20, 0.60) },
            0.05 * static_cast<double>(index % 8U),
            0.22 + 0.07 * static_cast<double>(index % 8U) }};
}

SceneEntityDocument make_local_light(const std::string& id, const std::string& name,
                                     const std::string& kind, const std::uint32_t index,
                                     const std::uint32_t total_count) {
    constexpr double pi = 3.14159265358979323846;
    const auto angle = 2.0 * pi * static_cast<double>(index) /
        static_cast<double>(std::max(total_count, 1U));
    const SceneVector3 position{16.0 * std::cos(angle), 10.0, 16.0 * std::sin(angle)};
    const bool spot = kind == "spot";
    return SceneEntityDocument{
        .guid = id,
        .name = name,
        .parent_guid = std::string(root_id),
        .transform = SceneTransform{position},
        .local_light = SceneLocalLight{
            kind,
            spot ? SceneVector3{0.92, 0.72, 0.58} : SceneVector3{1.0, 0.84, 0.60},
            900.0,
            30.0,
            {0.0, -1.0, 0.0},
            spot ? 22.0 : 25.0,
            spot ? 32.0 : 35.0,
            0.05,
            true }};
}

bool contains_id(const std::vector<std::string>& values, const std::string& value) {
    return std::ranges::find(values, value) != values.end();
}

} // namespace

ShadowScalabilityStressSceneBuildResult build_shadow_scalability_stress_scene(
    const ShadowScalabilityStressSceneConfig& config) {
    ShadowScalabilityStressSceneBuildResult result;
    if (!validate_config(config, result.code, result.detail)) return result;

    const auto selected_points = selected_point_lights(config);
    const auto selected_spots = selected_spot_lights(config, selected_points);
    const auto requested_local_lights = config.point_light_count + config.spot_light_count;
    const auto selected_local_lights = selected_points + selected_spots;
    const auto requested_layers = config.point_light_count * point_shadow_face_count +
        config.spot_light_count;
    const auto selected_layers = selected_points * point_shadow_face_count + selected_spots;

    ShadowScalabilityStressSceneContract contract;
    contract.workload_identity = make_workload_identity(config);
    contract.scene_guid = std::string(scene_guid);
    contract.camera_id = std::string(camera_id);
    contract.directional_light_id = std::string(directional_light_id);
    contract.expected_caster_count = config.caster_count;
    contract.expected_control_count = config.control_object_count;
    contract.expected_requested_shadow_count = 1U + requested_local_lights;
    contract.expected_selected_shadow_count = 1U + selected_local_lights;
    contract.expected_dropped_shadow_count = requested_local_lights - selected_local_lights;
    contract.expected_requested_local_shadow_layers = requested_layers;
    contract.expected_selected_local_shadow_layers = selected_layers;
    contract.expected_dropped_local_shadow_layers = requested_layers - selected_layers;
    contract.point_light_capacity = config.point_light_capacity;
    contract.spot_light_capacity = config.spot_light_capacity;
    contract.local_atlas_layer_capacity = config.local_atlas_layer_capacity;
    contract.pressure_over_capacity =
        contract.expected_dropped_shadow_count > 0U ||
        contract.expected_requested_local_shadow_layers > config.local_atlas_layer_capacity;
    contract.caster_ids.reserve(config.caster_count);
    contract.local_light_ids.reserve(requested_local_lights);
    contract.point_light_ids.reserve(config.point_light_count);
    contract.spot_light_ids.reserve(config.spot_light_count);
    contract.control_ids.reserve(config.control_object_count);

    SceneDocument document{
        .scene_guid = std::string(scene_guid),
        .name = "Shadow Scalability Stress Scene",
        .source_uri = "generated://scenes/shadow-scalability-stress.scene.json"};
    document.entities.reserve(3U + config.caster_count + requested_local_lights +
                             config.control_object_count);
    document.entities.push_back(SceneEntityDocument{
        .guid = std::string(root_id), .name = "Shadow Scalability Stress Root"});
    document.entities.push_back(SceneEntityDocument{
        .guid = std::string(camera_id),
        .name = "Fixed Stress Camera",
        .parent_guid = std::string(root_id),
        // Avoid a vertical look-at singularity: camera position and target
        // must not make the forward vector collinear with the world-up basis.
        // This fixed view also frames the bounded 4096-caster grid.
        .transform = SceneTransform{{0.0, 80.0, 120.0}},
        .camera = SceneCamera{{0.0, 0.0, 0.0}, 55.0, 0.1, 400.0, true}});
    document.entities.push_back(SceneEntityDocument{
        .guid = std::string(directional_light_id),
        .name = "Fixed Directional Shadow Light",
        .parent_guid = std::string(root_id),
        .directional_light = SceneDirectionalLight{
            {-0.55, -1.0, -0.35}, {1.0, 0.96, 0.88}, 1.05, 0.18, true }});

    std::uint32_t columns = 1U;
    while (columns * columns < config.caster_count) ++columns;
    constexpr double spacing = 2.25;
    const double center = static_cast<double>(columns - 1U) * 0.5;
    for (std::uint32_t index = 0U; index < config.caster_count; ++index) {
        const auto id = "entity.shadow-scalability-caster-" + std::to_string(index);
        contract.caster_ids.push_back(id);
        document.entities.push_back(make_caster(index, columns, spacing, center));
    }

    const auto total_local_lights = requested_local_lights;
    for (std::uint32_t index = 0U; index < config.point_light_count; ++index) {
        const auto id = "entity.shadow-scalability-point-light-" + std::to_string(index);
        contract.local_light_ids.push_back(id);
        contract.point_light_ids.push_back(id);
        document.entities.push_back(make_local_light(
            id, "Point Shadow Request " + std::to_string(index), "point", index,
            total_local_lights));
    }
    for (std::uint32_t index = 0U; index < config.spot_light_count; ++index) {
        const auto id = "entity.shadow-scalability-spot-light-" + std::to_string(index);
        contract.local_light_ids.push_back(id);
        contract.spot_light_ids.push_back(id);
        document.entities.push_back(make_local_light(
            id, "Spot Shadow Request " + std::to_string(index), "spot",
            config.point_light_count + index, total_local_lights));
    }

    const double control_base = (static_cast<double>(columns) * spacing) * 0.5 + 4.0;
    for (std::uint32_t index = 0U; index < config.control_object_count; ++index) {
        const auto id = "entity.shadow-scalability-control-" + std::to_string(index);
        contract.control_ids.push_back(id);
        document.entities.push_back(SceneEntityDocument{
            .guid = id,
            .name = "Control Object (Non-Caster) " + std::to_string(index),
            .parent_guid = std::string(root_id),
            .transform = SceneTransform{
                {index % 2U == 0U ? -control_base : control_base,
                 0.85,
                 (static_cast<double>(index) -
                  static_cast<double>(config.control_object_count - 1U) * 0.5) * 2.0},
                {0.7, 0.7, 0.7}},
            .mesh_renderer = SceneMeshRenderer{"asset.primitive.cube", true, false, true},
            .pbr_material = ScenePbrMaterial{{0.08, 0.10, 0.12}, 0.0, 0.8}});
    }

    const auto validation = SceneDocumentCodec::validate(document);
    if (!validation.empty()) {
        result.code = "shadow-scalability-stress.scene-invalid";
        result.detail = validation.front().code + ":" + validation.front().message;
        return result;
    }
    result.valid = true;
    result.code = "shadow-scalability-stress.ok";
    result.detail = "Deterministic shadow pressure scene and bounded contract created.";
    result.document = std::move(document);
    result.contract = std::move(contract);
    return result;
}

ShadowScalabilityStressSceneBuildResult make_shadow_scalability_stress_scene(
    const ShadowScalabilityStressSceneConfig& config) {
    return build_shadow_scalability_stress_scene(config);
}

std::string write_shadow_scalability_stress_scene_contract_json(
    const ShadowScalabilityStressSceneContract& contract) {
    const Json output = {
        {"schema", bounded_text(contract.schema)},
        {"workloadIdentity", bounded_text(contract.workload_identity)},
        {"sceneGuid", bounded_text(contract.scene_guid)},
        {"cameraId", bounded_text(contract.camera_id)},
        {"directionalLightId", bounded_text(contract.directional_light_id)},
        {"casterIds", bounded_id_array(contract.caster_ids,
                                        shadow_scalability_stress_scene_max_casters)},
        {"localLightIds", bounded_id_array(contract.local_light_ids,
                                            shadow_scalability_stress_scene_max_local_lights)},
        {"pointLightIds", bounded_id_array(contract.point_light_ids,
                                            shadow_scalability_stress_scene_max_local_lights)},
        {"spotLightIds", bounded_id_array(contract.spot_light_ids,
                                           shadow_scalability_stress_scene_max_local_lights)},
        {"controlIds", bounded_id_array(contract.control_ids,
                                         shadow_scalability_stress_scene_max_controls)},
        {"expected", {
            {"casterCount", contract.expected_caster_count},
            {"controlCount", contract.expected_control_count},
            {"requestedShadowCount", contract.expected_requested_shadow_count},
            {"selectedShadowCount", contract.expected_selected_shadow_count},
            {"droppedShadowCount", contract.expected_dropped_shadow_count},
            {"requestedLocalShadowLayers", contract.expected_requested_local_shadow_layers},
            {"selectedLocalShadowLayers", contract.expected_selected_local_shadow_layers},
            {"droppedLocalShadowLayers", contract.expected_dropped_local_shadow_layers},
        }},
        {"capacity", {
            {"pointLights", contract.point_light_capacity},
            {"spotLights", contract.spot_light_capacity},
            {"localAtlasLayers", contract.local_atlas_layer_capacity},
        }},
        {"pressureOverCapacity", contract.pressure_over_capacity},
    };
    return output.dump(2) + "\n";
}

ShadowScalabilityStressSceneContractParseResult
parse_shadow_scalability_stress_scene_contract_json(const std::string_view json) {
    ShadowScalabilityStressSceneContractParseResult result;
    if (json.size() > shadow_scalability_stress_scene_max_contract_bytes) {
        add_error(result.errors, "shadow-scalability-stress.document-too-large", "",
                  "The stress contract exceeds its bounded JSON size.");
        return result;
    }
    const auto input = Json::parse(json, nullptr, false);
    if (input.is_discarded()) {
        add_error(result.errors, "shadow-scalability-stress.invalid-json", "",
                  "The stress contract is not valid JSON.");
        return result;
    }
    if (!input.is_object()) {
        add_error(result.errors, "shadow-scalability-stress.invalid-root", "",
                  "The stress contract root must be an object.");
        return result;
    }
    reject_unknown_fields(input,
        {"schema", "workloadIdentity", "sceneGuid", "cameraId", "directionalLightId",
         "casterIds", "localLightIds", "pointLightIds", "spotLightIds", "controlIds",
         "expected", "capacity", "pressureOverCapacity"}, "", result.errors);

    ShadowScalabilityStressSceneContract contract;
    read_required_string(input, "schema", "", contract.schema, result.errors);
    read_required_string(input, "workloadIdentity", "", contract.workload_identity, result.errors);
    read_required_string(input, "sceneGuid", "", contract.scene_guid, result.errors);
    read_required_string(input, "cameraId", "", contract.camera_id, result.errors);
    read_required_string(input, "directionalLightId", "", contract.directional_light_id,
                         result.errors);
    if (contract.schema != shadow_scalability_stress_scene_schema) {
        add_error(result.errors, "shadow-scalability-stress.unsupported-schema", "/schema",
                  "Expected noemancer.shadow-scalability-stress/0.1.");
    }
    read_id_array(input, "casterIds", "", shadow_scalability_stress_scene_max_casters,
                  contract.caster_ids, result.errors);
    read_id_array(input, "localLightIds", "", shadow_scalability_stress_scene_max_local_lights,
                  contract.local_light_ids, result.errors);
    read_id_array(input, "pointLightIds", "", shadow_scalability_stress_scene_max_local_lights,
                  contract.point_light_ids, result.errors);
    read_id_array(input, "spotLightIds", "", shadow_scalability_stress_scene_max_local_lights,
                  contract.spot_light_ids, result.errors);
    read_id_array(input, "controlIds", "", shadow_scalability_stress_scene_max_controls,
                  contract.control_ids, result.errors);

    if (!input.contains("expected") || !input.at("expected").is_object()) {
        add_error(result.errors, "shadow-scalability-stress.invalid-expected", "/expected",
                  "The expected pressure counts object is required.");
    } else {
        const auto& expected = input.at("expected");
        reject_unknown_fields(expected,
            {"casterCount", "controlCount", "requestedShadowCount", "selectedShadowCount",
             "droppedShadowCount", "requestedLocalShadowLayers", "selectedLocalShadowLayers",
             "droppedLocalShadowLayers"}, "/expected", result.errors);
        read_required_uint32(expected, "casterCount", "/expected",
                             contract.expected_caster_count, result.errors);
        read_required_uint32(expected, "controlCount", "/expected",
                             contract.expected_control_count, result.errors);
        read_required_uint32(expected, "requestedShadowCount", "/expected",
                             contract.expected_requested_shadow_count, result.errors);
        read_required_uint32(expected, "selectedShadowCount", "/expected",
                             contract.expected_selected_shadow_count, result.errors);
        read_required_uint32(expected, "droppedShadowCount", "/expected",
                             contract.expected_dropped_shadow_count, result.errors);
        read_required_uint32(expected, "requestedLocalShadowLayers", "/expected",
                             contract.expected_requested_local_shadow_layers, result.errors);
        read_required_uint32(expected, "selectedLocalShadowLayers", "/expected",
                             contract.expected_selected_local_shadow_layers, result.errors);
        read_required_uint32(expected, "droppedLocalShadowLayers", "/expected",
                             contract.expected_dropped_local_shadow_layers, result.errors);
    }
    if (!input.contains("capacity") || !input.at("capacity").is_object()) {
        add_error(result.errors, "shadow-scalability-stress.invalid-capacity", "/capacity",
                  "The capacity object is required.");
    } else {
        const auto& capacity = input.at("capacity");
        reject_unknown_fields(capacity, {"pointLights", "spotLights", "localAtlasLayers"},
                              "/capacity", result.errors);
        read_required_uint32(capacity, "pointLights", "/capacity",
                             contract.point_light_capacity, result.errors);
        read_required_uint32(capacity, "spotLights", "/capacity",
                             contract.spot_light_capacity, result.errors);
        read_required_uint32(capacity, "localAtlasLayers", "/capacity",
                             contract.local_atlas_layer_capacity, result.errors);
    }
    read_required_bool(input, "pressureOverCapacity", "", contract.pressure_over_capacity,
                       result.errors);

    if (!result.errors.empty()) return result;

    if (contract.expected_caster_count < shadow_scalability_stress_scene_min_casters ||
        contract.expected_caster_count > shadow_scalability_stress_scene_max_casters ||
        contract.expected_caster_count != contract.caster_ids.size()) {
        add_error(result.errors, "shadow-scalability-stress.caster-count-mismatch",
                  "/expected/casterCount", "Caster IDs and expected count do not agree.");
    }
    if (contract.expected_control_count == 0U ||
        contract.expected_control_count > shadow_scalability_stress_scene_max_controls ||
        contract.expected_control_count != contract.control_ids.size()) {
        add_error(result.errors, "shadow-scalability-stress.control-count-mismatch",
                  "/expected/controlCount", "Control IDs and expected count do not agree.");
    }
    if (contract.local_light_ids.size() !=
            contract.point_light_ids.size() + contract.spot_light_ids.size() ||
        contract.local_light_ids.empty()) {
        add_error(result.errors, "shadow-scalability-stress.local-light-ids-mismatch",
                  "/localLightIds", "Point and spot ID arrays must partition localLightIds.");
    }
    std::unordered_set<std::string> typed_local_ids;
    for (const auto& id : contract.point_light_ids) {
        if (!typed_local_ids.insert(id).second)
            add_error(result.errors, "shadow-scalability-stress.local-light-ids-mismatch",
                      "/pointLightIds", "Point and spot ID arrays must not overlap.");
        if (!contains_id(contract.local_light_ids, id))
            add_error(result.errors, "shadow-scalability-stress.local-light-ids-mismatch",
                      "/pointLightIds", "Every point light ID must occur in localLightIds.");
    }
    for (const auto& id : contract.spot_light_ids) {
        if (!typed_local_ids.insert(id).second)
            add_error(result.errors, "shadow-scalability-stress.local-light-ids-mismatch",
                      "/spotLightIds", "Point and spot ID arrays must not overlap.");
        if (!contains_id(contract.local_light_ids, id))
            add_error(result.errors, "shadow-scalability-stress.local-light-ids-mismatch",
                      "/spotLightIds", "Every spot light ID must occur in localLightIds.");
    }
    if (contract.expected_requested_shadow_count != contract.local_light_ids.size() + 1U ||
        contract.expected_selected_shadow_count < 1U ||
        static_cast<std::uint64_t>(contract.expected_selected_shadow_count) +
                static_cast<std::uint64_t>(contract.expected_dropped_shadow_count) !=
            static_cast<std::uint64_t>(contract.expected_requested_shadow_count)) {
        add_error(result.errors, "shadow-scalability-stress.shadow-count-mismatch",
                  "/expected/requestedShadowCount",
                  "Directional and local shadow request counts do not partition consistently.");
    }
    const auto recomputed_requested_layers =
        static_cast<std::uint32_t>(contract.point_light_ids.size()) * point_shadow_face_count +
        static_cast<std::uint32_t>(contract.spot_light_ids.size());
    if (contract.expected_requested_local_shadow_layers != recomputed_requested_layers ||
        contract.expected_selected_local_shadow_layers >
            contract.expected_requested_local_shadow_layers ||
        static_cast<std::uint64_t>(contract.expected_selected_local_shadow_layers) +
                static_cast<std::uint64_t>(contract.expected_dropped_local_shadow_layers) !=
            static_cast<std::uint64_t>(contract.expected_requested_local_shadow_layers)) {
        add_error(result.errors, "shadow-scalability-stress.layer-count-mismatch",
                  "/expected/requestedLocalShadowLayers",
                  "Local shadow layer pressure counts do not partition consistently.");
    }
    if (contract.point_light_capacity == 0U ||
        contract.point_light_capacity > shadow_scalability_stress_scene_max_capacity ||
        contract.spot_light_capacity == 0U ||
        contract.spot_light_capacity > shadow_scalability_stress_scene_max_capacity ||
        contract.local_atlas_layer_capacity == 0U ||
        contract.local_atlas_layer_capacity > shadow_scalability_stress_scene_max_capacity ||
        contract.expected_selected_local_shadow_layers > contract.local_atlas_layer_capacity) {
        add_error(result.errors, "shadow-scalability-stress.capacity-range", "/capacity",
                  "Capacity values are outside the bounded range or selected layers exceed the atlas.");
    }
    const bool expected_pressure = contract.expected_dropped_shadow_count > 0U ||
        contract.expected_requested_local_shadow_layers > contract.local_atlas_layer_capacity;
    if (contract.pressure_over_capacity != expected_pressure) {
        add_error(result.errors, "shadow-scalability-stress.pressure-mismatch",
                  "/pressureOverCapacity", "Pressure flag does not match the expected counts.");
    }

    std::unordered_set<std::string> all_ids;
    const auto add_unique = [&all_ids, &result](const std::string& id, const std::string& path) {
        if (!all_ids.insert(id).second)
            add_error(result.errors, "shadow-scalability-stress.duplicate-id", path,
                      "Entity IDs must be unique across all workload categories.");
    };
    add_unique(contract.scene_guid, "/sceneGuid");
    add_unique(contract.camera_id, "/cameraId");
    add_unique(contract.directional_light_id, "/directionalLightId");
    for (const auto& id : contract.caster_ids) add_unique(id, "/casterIds");
    for (const auto& id : contract.local_light_ids) add_unique(id, "/localLightIds");
    for (const auto& id : contract.control_ids) add_unique(id, "/controlIds");

    if (!result.errors.empty()) return result;
    result.contract = std::move(contract);
    return result;
}

} // namespace noemancer
