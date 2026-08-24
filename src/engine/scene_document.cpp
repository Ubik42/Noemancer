#include "engine/scene_document.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <unordered_map>
#include <unordered_set>

namespace noemancer {
namespace {

using Json = nlohmann::json;

void add_error(
    std::vector<SceneDocumentError>& errors,
    std::string code,
    std::string path,
    std::string message) {
    errors.push_back({std::move(code), std::move(path), std::move(message)});
}

bool read_vector3(
    const Json& value,
    const std::string& path,
    SceneVector3& result,
    std::vector<SceneDocumentError>& errors) {
    if (!value.is_array() || value.size() != 3 ||
        !value[0].is_number() || !value[1].is_number() || !value[2].is_number()) {
        add_error(errors, "scene.invalid-vector3", path, "Expected an array containing three numbers.");
        return false;
    }
    result = {
        value[0].get<double>(),
        value[1].get<double>(),
        value[2].get<double>()
    };
    return true;
}

bool finite(const SceneVector3& value) {
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

bool convex_has_volume(const std::vector<SceneVector3>& points) {
    if(points.size()<4U) return false;
    const auto subtract=[](const SceneVector3& a,const SceneVector3& b){return SceneVector3{a.x-b.x,a.y-b.y,a.z-b.z};};
    const auto cross=[](const SceneVector3& a,const SceneVector3& b){return SceneVector3{a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x};};
    const auto dot=[](const SceneVector3& a,const SceneVector3& b){return a.x*b.x+a.y*b.y+a.z*b.z;};
    constexpr double epsilon=1.0e-9;
    for(std::size_t a=1;a<points.size();++a) {
        const auto edge=subtract(points[a],points[0]);
        for(std::size_t b=a+1;b<points.size();++b) {
            const auto normal=cross(edge,subtract(points[b],points[0]));
            if(dot(normal,normal)<=epsilon) continue;
            for(std::size_t c=b+1;c<points.size();++c)
                if(std::abs(dot(normal,subtract(points[c],points[0])))>epsilon) return true;
        }
    }
    return false;
}

bool read_number(
    const Json& object,
    const std::string_view name,
    const std::string& path,
    double& result,
    std::vector<SceneDocumentError>& errors) {
    if (!object.contains(name) || !object.at(name).is_number()) {
        add_error(errors, "scene.invalid-number", path + "/" + std::string(name), "Expected a finite number.");
        return false;
    }
    result = object.at(name).get<double>();
    if (!std::isfinite(result)) {
        add_error(errors, "scene.non-finite-number", path + "/" + std::string(name), "Scene numbers must be finite.");
        return false;
    }
    return true;
}

bool read_bool(
    const Json& object,
    const std::string_view name,
    const std::string& path,
    bool& result,
    std::vector<SceneDocumentError>& errors) {
    if (!object.contains(name) || !object.at(name).is_boolean()) {
        add_error(errors, "scene.invalid-boolean", path + "/" + std::string(name), "Expected a boolean.");
        return false;
    }
    result = object.at(name).get<bool>();
    return true;
}

Json vector3_json(const SceneVector3& value) {
    return Json::array({value.x, value.y, value.z});
}

void reject_unknown_fields(
    const Json& object,
    const std::initializer_list<std::string_view> allowed,
    const std::string& path,
    std::vector<SceneDocumentError>& errors) {
    for (const auto& [name, unused] : object.items()) {
        static_cast<void>(unused);
        if (std::ranges::none_of(allowed, [&name](const std::string_view candidate) {
                return candidate == name;
            })) {
            add_error(errors, "scene.unknown-field", path + "/" + name, "Unknown fields are rejected to prevent silent scene data loss.");
        }
    }
}

} // namespace

SceneDocumentParseResult SceneDocumentCodec::parse_json(
    const std::string_view json,
    std::string source_uri) {
    SceneDocumentParseResult result;
    const auto input = Json::parse(json, nullptr, false);
    if (input.is_discarded()) {
        add_error(result.errors, "scene.invalid-json", "", "The scene document is not valid JSON.");
        return result;
    }
    if (!input.is_object()) {
        add_error(result.errors, "scene.invalid-root", "", "The scene document root must be an object.");
        return result;
    }
    reject_unknown_fields(input, {"schema", "sceneGuid", "name", "entities"}, "", result.errors);

    SceneDocument document;
    document.source_uri = std::move(source_uri);
    if (!input.contains("schema") || !input["schema"].is_string()) {
        add_error(result.errors, "scene.missing-field", "/schema", "A string schema identifier is required.");
    } else {
        document.schema = input["schema"].get<std::string>();
    }
    if (!input.contains("sceneGuid") || !input["sceneGuid"].is_string()) {
        add_error(result.errors, "scene.missing-field", "/sceneGuid", "A string sceneGuid is required.");
    } else {
        document.scene_guid = input["sceneGuid"].get<std::string>();
    }
    if (!input.contains("name") || !input["name"].is_string()) {
        add_error(result.errors, "scene.missing-field", "/name", "A string scene name is required.");
    } else {
        document.name = input["name"].get<std::string>();
    }
    if (!input.contains("entities") || !input["entities"].is_array()) {
        add_error(result.errors, "scene.missing-field", "/entities", "An entities array is required.");
    } else {
        for (std::size_t index = 0; index < input["entities"].size(); ++index) {
            const auto& entity_json = input["entities"][index];
            const auto base_path = "/entities/" + std::to_string(index);
            if (!entity_json.is_object()) {
                add_error(result.errors, "scene.invalid-entity", base_path, "An entity must be an object.");
                continue;
            }
            reject_unknown_fields(entity_json, {"guid", "name", "parent", "components"}, base_path, result.errors);
            SceneEntityDocument entity;
            if (entity_json.contains("guid") && entity_json["guid"].is_string()) {
                entity.guid = entity_json["guid"].get<std::string>();
            } else {
                add_error(result.errors, "scene.missing-field", base_path + "/guid", "A string entity guid is required.");
            }
            if (entity_json.contains("name") && entity_json["name"].is_string()) {
                entity.name = entity_json["name"].get<std::string>();
            } else {
                add_error(result.errors, "scene.missing-field", base_path + "/name", "A string entity name is required.");
            }
            if (entity_json.contains("parent") && !entity_json["parent"].is_null()) {
                if (entity_json["parent"].is_string()) {
                    entity.parent_guid = entity_json["parent"].get<std::string>();
                } else {
                    add_error(result.errors, "scene.invalid-parent", base_path + "/parent", "Parent must be null or an entity guid.");
                }
            }

            if (!entity_json.contains("components") || !entity_json["components"].is_object()) {
                add_error(result.errors, "scene.missing-field", base_path + "/components", "A components object is required.");
            } else {
                const auto& components = entity_json["components"];
                for (const auto& [component_name, component] : components.items()) {
                    if (component_name == "Transform") {
                        if (!component.is_object() || !component.contains("position")) {
                            add_error(result.errors, "scene.invalid-component", base_path + "/components/Transform", "Transform.position is required.");
                            continue;
                        }
                        reject_unknown_fields(component, {"position", "scale", "rotationEulerDegrees"}, base_path + "/components/Transform", result.errors);
                        SceneTransform transform;
                        bool valid = read_vector3(component["position"], base_path + "/components/Transform/position", transform.position, result.errors);
                        if (component.contains("scale"))
                            valid &= read_vector3(component["scale"], base_path + "/components/Transform/scale", transform.scale, result.errors);
                        if (component.contains("rotationEulerDegrees"))
                            valid &= read_vector3(component["rotationEulerDegrees"], base_path + "/components/Transform/rotationEulerDegrees", transform.rotation_euler_degrees, result.errors);
                        if (valid) {
                            entity.transform = transform;
                        }
                    } else if (component_name == "Velocity") {
                        if (!component.is_object() || !component.contains("linear")) {
                            add_error(result.errors, "scene.invalid-component", base_path + "/components/Velocity", "Velocity.linear is required.");
                            continue;
                        }
                        reject_unknown_fields(component, {"linear"}, base_path + "/components/Velocity", result.errors);
                        SceneVelocity velocity;
                        if (read_vector3(component["linear"], base_path + "/components/Velocity/linear", velocity.linear, result.errors)) {
                            entity.velocity = velocity;
                        }
                    } else if (component_name == "RigidBody") {
                        const auto path = base_path + "/components/RigidBody";
                        if (!component.is_object()) {
                            add_error(result.errors, "scene.invalid-component", path, "RigidBody must be an object.");
                            continue;
                        }
                        reject_unknown_fields(component, {"motionType", "mass", "gravityFactor", "linearDamping"}, path, result.errors);
                        SceneRigidBody body;
                        bool valid = component.contains("motionType") && component.at("motionType").is_string();
                        if (!valid) add_error(result.errors, "scene.invalid-string", path + "/motionType", "A motion type is required.");
                        else body.motion_type = component.at("motionType").get<std::string>();
                        valid &= read_number(component, "mass", path, body.mass, result.errors);
                        valid &= read_number(component, "gravityFactor", path, body.gravity_factor, result.errors);
                        valid &= read_number(component, "linearDamping", path, body.linear_damping, result.errors);
                        if (valid) entity.rigid_body = body;
                    } else if (component_name == "BoxCollider") {
                        const auto path = base_path + "/components/BoxCollider";
                        if (!component.is_object()) {
                            add_error(result.errors, "scene.invalid-component", path, "BoxCollider must be an object.");
                            continue;
                        }
                        reject_unknown_fields(component, {"halfExtents", "friction", "restitution", "isTrigger"}, path, result.errors);
                        SceneBoxCollider collider;
                        bool valid = read_vector3(component.value("halfExtents", Json{}), path + "/halfExtents", collider.half_extents, result.errors) &
                            read_number(component, "friction", path, collider.friction, result.errors) &
                            read_number(component, "restitution", path, collider.restitution, result.errors);
                        if(component.contains("isTrigger"))valid&=read_bool(component,"isTrigger",path,collider.is_trigger,result.errors);
                        if (valid) entity.box_collider = collider;
                    } else if (component_name == "SphereCollider") {
                        const auto path = base_path + "/components/SphereCollider";
                        if (!component.is_object()) {
                            add_error(result.errors, "scene.invalid-component", path, "SphereCollider must be an object.");
                            continue;
                        }
                        reject_unknown_fields(component, {"radius", "friction", "restitution", "isTrigger"}, path, result.errors);
                        SceneSphereCollider collider;
                        bool valid = read_number(component, "radius", path, collider.radius, result.errors) &
                            read_number(component, "friction", path, collider.friction, result.errors) &
                            read_number(component, "restitution", path, collider.restitution, result.errors);
                        if(component.contains("isTrigger"))valid&=read_bool(component,"isTrigger",path,collider.is_trigger,result.errors);
                        if (valid) entity.sphere_collider = collider;
                    } else if (component_name == "CapsuleCollider") {
                        const auto path = base_path + "/components/CapsuleCollider";
                        if (!component.is_object()) {
                            add_error(result.errors, "scene.invalid-component", path, "CapsuleCollider must be an object.");
                            continue;
                        }
                        reject_unknown_fields(component, {"radius", "halfHeight", "friction", "restitution", "isTrigger"}, path, result.errors);
                        SceneCapsuleCollider collider;
                        bool valid = read_number(component, "radius", path, collider.radius, result.errors) &
                            read_number(component, "halfHeight", path, collider.half_height, result.errors) &
                            read_number(component, "friction", path, collider.friction, result.errors) &
                            read_number(component, "restitution", path, collider.restitution, result.errors);
                        if(component.contains("isTrigger"))valid&=read_bool(component,"isTrigger",path,collider.is_trigger,result.errors);
                        if (valid) entity.capsule_collider = collider;
                    } else if (component_name == "CharacterMotor2D") {
                        const auto path = base_path + "/components/CharacterMotor2D";
                        if (!component.is_object()) {
                            add_error(result.errors, "scene.invalid-component", path, "CharacterMotor2D must be an object.");
                            continue;
                        }
                        reject_unknown_fields(component, {"maximumSpeed", "groundAcceleration", "airAcceleration",
                            "groundDeceleration", "jumpSpeed", "maximumFallSpeed", "coyoteTimeSeconds",
                            "jumpBufferSeconds", "groundProbeDistance", "minimumGroundNormalY", "jumpReleaseVelocityFactor"}, path, result.errors);
                        SceneCharacterMotor2D motor;
                        const bool valid = read_number(component, "maximumSpeed", path, motor.maximum_speed, result.errors) &
                            read_number(component, "groundAcceleration", path, motor.ground_acceleration, result.errors) &
                            read_number(component, "airAcceleration", path, motor.air_acceleration, result.errors) &
                            read_number(component, "groundDeceleration", path, motor.ground_deceleration, result.errors) &
                            read_number(component, "jumpSpeed", path, motor.jump_speed, result.errors) &
                            read_number(component, "maximumFallSpeed", path, motor.maximum_fall_speed, result.errors) &
                            read_number(component, "coyoteTimeSeconds", path, motor.coyote_time_seconds, result.errors) &
                            read_number(component, "jumpBufferSeconds", path, motor.jump_buffer_seconds, result.errors) &
                            read_number(component, "groundProbeDistance", path, motor.ground_probe_distance, result.errors) &
                            read_number(component, "minimumGroundNormalY", path, motor.minimum_ground_normal_y, result.errors);
                        bool final_valid = valid;
                        if (component.contains("jumpReleaseVelocityFactor"))
                            final_valid &= read_number(component, "jumpReleaseVelocityFactor", path, motor.jump_release_velocity_factor, result.errors);
                        if (final_valid) entity.character_motor_2d = motor;
                    } else if (component_name == "Platform2D") {
                        const auto path = base_path + "/components/Platform2D";
                        if (!component.is_object()) { add_error(result.errors,"scene.invalid-component",path,"Platform2D must be an object."); continue; }
                        reject_unknown_fields(component,{"collisionMode","motionAxis","motionDistance","motionPeriodSeconds","motionPhase"},path,result.errors);
                        ScenePlatform2D platform;
                        bool valid=component.contains("collisionMode")&&component.at("collisionMode").is_string();
                        if(!valid) add_error(result.errors,"scene.invalid-string",path+"/collisionMode","Platform2D collisionMode is required.");
                        else platform.collision_mode=component.at("collisionMode").get<std::string>();
                        valid &= read_vector3(component.value("motionAxis",Json{}),path+"/motionAxis",platform.motion_axis,result.errors);
                        valid &= read_number(component,"motionDistance",path,platform.motion_distance,result.errors);
                        valid &= read_number(component,"motionPeriodSeconds",path,platform.motion_period_seconds,result.errors);
                        valid &= read_number(component,"motionPhase",path,platform.motion_phase,result.errors);
                        if(valid) entity.platform_2d=platform;
                    } else if (component_name == "ConvexHullCollider") {
                        const auto path = base_path + "/components/ConvexHullCollider";
                        if (!component.is_object()) { add_error(result.errors,"scene.invalid-component",path,"ConvexHullCollider must be an object."); continue; }
                        reject_unknown_fields(component,{"points","friction","restitution","isTrigger"},path,result.errors);
                        SceneConvexHullCollider collider;
                        bool valid=component.contains("points")&&component.at("points").is_array();
                        if(!valid) add_error(result.errors,"scene.invalid-convex-points",path+"/points","Convex hull points must be an array.");
                        else for(std::size_t point=0;point<component.at("points").size();++point) {
                            SceneVector3 value; valid &= read_vector3(component.at("points").at(point),path+"/points/"+std::to_string(point),value,result.errors);
                            if(valid) collider.points.push_back(value);
                        }
                        valid &= read_number(component,"friction",path,collider.friction,result.errors);
                        valid &= read_number(component,"restitution",path,collider.restitution,result.errors);
                        if(component.contains("isTrigger"))valid&=read_bool(component,"isTrigger",path,collider.is_trigger,result.errors);
                        if(valid) entity.convex_hull_collider=std::move(collider);
                    } else if (component_name == "AnimationPlayer") {
                        const auto path = base_path + "/components/AnimationPlayer";
                        if (!component.is_object()) {
                            add_error(result.errors, "scene.invalid-component", path, "AnimationPlayer must be an object.");
                            continue;
                        }
                        reject_unknown_fields(component, {"clipAsset", "playbackSpeed", "looping", "playing", "nextClipAsset",
                            "transitionDurationSeconds", "rootMotionMode", "stateMachineAsset", "animationGraphAsset"}, path, result.errors);
                        SceneAnimationPlayer player;
                        bool valid = component.contains("clipAsset") && component.at("clipAsset").is_string();
                        if (!valid) add_error(result.errors, "scene.invalid-string", path + "/clipAsset", "A clip asset ID is required.");
                        else player.clip_asset = component.at("clipAsset").get<std::string>();
                        valid &= read_number(component, "playbackSpeed", path, player.playback_speed, result.errors);
                        valid &= read_bool(component, "looping", path, player.looping, result.errors);
                        valid &= read_bool(component, "playing", path, player.playing, result.errors);
                        if (component.contains("nextClipAsset")) {
                            if (!component.at("nextClipAsset").is_string()) {
                                add_error(result.errors, "scene.invalid-string", path + "/nextClipAsset", "nextClipAsset must be a string.");
                                valid = false;
                            } else player.next_clip_asset = component.at("nextClipAsset").get<std::string>();
                        }
                        if (component.contains("transitionDurationSeconds"))
                            valid &= read_number(component, "transitionDurationSeconds", path, player.transition_duration_seconds, result.errors);
                        if (component.contains("rootMotionMode")) {
                            if (!component.at("rootMotionMode").is_string()) {
                                add_error(result.errors, "scene.invalid-string", path + "/rootMotionMode", "rootMotionMode must be a string.");
                                valid = false;
                            } else player.root_motion_mode = component.at("rootMotionMode").get<std::string>();
                        }
                        if (component.contains("stateMachineAsset")) {
                            if (!component.at("stateMachineAsset").is_string()) {
                                add_error(result.errors, "scene.invalid-string", path + "/stateMachineAsset", "stateMachineAsset must be a string.");
                                valid = false;
                            } else player.state_machine_asset = component.at("stateMachineAsset").get<std::string>();
                        }
                        if (component.contains("animationGraphAsset")) {
                            if (!component.at("animationGraphAsset").is_string()) {
                                add_error(result.errors, "scene.invalid-string", path + "/animationGraphAsset", "animationGraphAsset must be a string.");
                                valid = false;
                            } else player.animation_graph_asset = component.at("animationGraphAsset").get<std::string>();
                        }
                        if (valid) entity.animation_player = player;
                    } else if (component_name == "Camera") {
                        const auto path = base_path + "/components/Camera";
                        if (!component.is_object()) {
                            add_error(result.errors, "scene.invalid-component", path, "Camera must be an object.");
                            continue;
                        }
                        reject_unknown_fields(component, {"target", "verticalFovDegrees", "nearClip", "farClip", "primary",
                            "projection", "orthographicHeight"}, path, result.errors);
                        SceneCamera camera;
                        bool valid = read_vector3(component.value("target", Json{}), path + "/target", camera.target, result.errors) &
                            read_number(component, "verticalFovDegrees", path, camera.vertical_fov_degrees, result.errors) &
                            read_number(component, "nearClip", path, camera.near_clip, result.errors) &
                            read_number(component, "farClip", path, camera.far_clip, result.errors) &
                            read_bool(component, "primary", path, camera.primary, result.errors);
                        if (component.contains("projection")) {
                            if (!component.at("projection").is_string()) {
                                add_error(result.errors,"scene.invalid-string",path+"/projection","Camera projection must be a string."); valid=false;
                            } else camera.projection=component.at("projection").get<std::string>();
                        }
                        if (component.contains("orthographicHeight"))
                            valid &= read_number(component,"orthographicHeight",path,camera.orthographic_height,result.errors);
                        if (valid) entity.camera = camera;
                    } else if (component_name == "CameraFollow2D") {
                        const auto path=base_path+"/components/CameraFollow2D";
                        if(!component.is_object()){add_error(result.errors,"scene.invalid-component",path,"CameraFollow2D must be an object.");continue;}
                        reject_unknown_fields(component,{"targetEntityId","positionOffset","deadZone","lookAheadDistance","smoothing","minimumCenter","maximumCenter"},path,result.errors);
                        SceneCameraFollow2D follow;
                        bool valid=component.contains("targetEntityId")&&component.at("targetEntityId").is_string();
                        if(!valid)add_error(result.errors,"scene.invalid-string",path+"/targetEntityId","CameraFollow2D targetEntityId is required.");
                        else follow.target_entity_id=component.at("targetEntityId").get<std::string>();
                        valid &= read_vector3(component.value("positionOffset",Json{}),path+"/positionOffset",follow.position_offset,result.errors);
                        valid &= read_vector3(component.value("deadZone",Json{}),path+"/deadZone",follow.dead_zone,result.errors);
                        valid &= read_number(component,"lookAheadDistance",path,follow.look_ahead_distance,result.errors);
                        valid &= read_number(component,"smoothing",path,follow.smoothing,result.errors);
                        valid &= read_vector3(component.value("minimumCenter",Json{}),path+"/minimumCenter",follow.minimum_center,result.errors);
                        valid &= read_vector3(component.value("maximumCenter",Json{}),path+"/maximumCenter",follow.maximum_center,result.errors);
                        if(valid) entity.camera_follow_2d=follow;
                    } else if (component_name == "DirectionalLight") {
                        const auto path = base_path + "/components/DirectionalLight";
                        if (!component.is_object()) {
                            add_error(result.errors, "scene.invalid-component", path, "DirectionalLight must be an object.");
                            continue;
                        }
                        reject_unknown_fields(component, {"direction", "color", "intensity", "ambientIntensity", "castsShadows"}, path, result.errors);
                        SceneDirectionalLight light;
                        const bool valid = read_vector3(component.value("direction", Json{}), path + "/direction", light.direction, result.errors) &
                            read_vector3(component.value("color", Json{}), path + "/color", light.color, result.errors) &
                            read_number(component, "intensity", path, light.intensity, result.errors) &
                            read_number(component, "ambientIntensity", path, light.ambient_intensity, result.errors) &
                            read_bool(component, "castsShadows", path, light.casts_shadows, result.errors);
                        if (valid) entity.directional_light = light;
                    } else if (component_name == "LocalLight") {
                        const auto path=base_path+"/components/LocalLight";
                        if(!component.is_object()){add_error(result.errors,"scene.invalid-component",path,"LocalLight must be an object.");continue;}
                        reject_unknown_fields(component,{"kind","color","luminousPowerLumens","rangeMeters","direction",
                            "innerConeDegrees","outerConeDegrees","sourceRadiusMeters","castsShadows"},path,result.errors);
                        SceneLocalLight light;bool valid=true;
                        if(!component.contains("kind")||!component.at("kind").is_string()) {
                            add_error(result.errors,"scene.invalid-string",path+"/kind","LocalLight kind must be a string.");valid=false;
                        } else light.kind=component.at("kind").get<std::string>();
                        valid &= read_vector3(component.value("color",Json{}),path+"/color",light.color,result.errors);
                        valid &= read_number(component,"luminousPowerLumens",path,light.luminous_power_lumens,result.errors);
                        valid &= read_number(component,"rangeMeters",path,light.range_meters,result.errors);
                        valid &= read_vector3(component.value("direction",Json{}),path+"/direction",light.direction,result.errors);
                        valid &= read_number(component,"innerConeDegrees",path,light.inner_cone_degrees,result.errors);
                        valid &= read_number(component,"outerConeDegrees",path,light.outer_cone_degrees,result.errors);
                        valid &= read_number(component,"sourceRadiusMeters",path,light.source_radius_meters,result.errors);
                        valid &= read_bool(component,"castsShadows",path,light.casts_shadows,result.errors);
                        if(valid)entity.local_light=std::move(light);
                    } else if (component_name == "MeshRenderer") {
                        const auto path = base_path + "/components/MeshRenderer";
                        if (!component.is_object()) {
                            add_error(result.errors, "scene.invalid-component", path, "MeshRenderer must be an object.");
                            continue;
                        }
                        reject_unknown_fields(component, {"meshAsset", "visible", "castsShadows", "receivesShadows"}, path, result.errors);
                        SceneMeshRenderer renderer;
                        bool valid = true;
                        if (!component.contains("meshAsset") || !component.at("meshAsset").is_string()) {
                            add_error(result.errors, "scene.invalid-string", path + "/meshAsset", "A mesh asset ID is required.");
                            valid = false;
                        } else {
                            renderer.mesh_asset = component.at("meshAsset").get<std::string>();
                        }
                        valid &= read_bool(component, "visible", path, renderer.visible, result.errors);
                        valid &= read_bool(component, "castsShadows", path, renderer.casts_shadows, result.errors);
                        valid &= read_bool(component, "receivesShadows", path, renderer.receives_shadows, result.errors);
                        if (valid) entity.mesh_renderer = std::move(renderer);
                    } else if(component_name=="SpriteRenderer") {
                        const auto path=base_path+"/components/SpriteRenderer";
                        if(!component.is_object()){add_error(result.errors,"scene.invalid-component",path,"SpriteRenderer must be an object.");continue;}
                        reject_unknown_fields(component,{"spriteAsset","clip","playbackSpeed","playing","flipX","flipY","sortingLayer","sortingOrder","visible"},path,result.errors);
                        SceneSpriteRenderer renderer;bool valid=true;
                        if(!component.contains("spriteAsset")||!component.at("spriteAsset").is_string()||
                           !component.contains("clip")||!component.at("clip").is_string()) {
                            add_error(result.errors,"scene.invalid-sprite-renderer",path,"spriteAsset and clip must be strings.");valid=false;
                        } else {renderer.sprite_asset=component.at("spriteAsset").get<std::string>();renderer.clip=component.at("clip").get<std::string>();}
                        valid&=read_number(component,"playbackSpeed",path,renderer.playback_speed,result.errors);
                        valid&=read_bool(component,"playing",path,renderer.playing,result.errors);
                        valid&=read_bool(component,"flipX",path,renderer.flip_x,result.errors);
                        valid&=read_bool(component,"flipY",path,renderer.flip_y,result.errors);
                        valid&=read_bool(component,"visible",path,renderer.visible,result.errors);
                        if(!component.contains("sortingLayer")||!component.at("sortingLayer").is_string()) {
                            add_error(result.errors,"scene.invalid-string",path+"/sortingLayer","sortingLayer must be a string.");valid=false;
                        } else renderer.sorting_layer=component.at("sortingLayer").get<std::string>();
                        if(!component.contains("sortingOrder")||!component.at("sortingOrder").is_number_integer()) {
                            add_error(result.errors,"scene.invalid-integer",path+"/sortingOrder","sortingOrder must be a signed integer.");valid=false;
                        } else {const auto order=component.at("sortingOrder").get<std::int64_t>();
                            if(order<std::numeric_limits<std::int32_t>::min()||order>std::numeric_limits<std::int32_t>::max()) {
                                add_error(result.errors,"scene.integer-range",path+"/sortingOrder","sortingOrder exceeds 32-bit range.");valid=false;
                            } else renderer.sorting_order=static_cast<std::int32_t>(order);}
                        if(valid)entity.sprite_renderer=std::move(renderer);
                    } else if(component_name=="TilemapRenderer") {
                        const auto path=base_path+"/components/TilemapRenderer";
                        if(!component.is_object()){add_error(result.errors,"scene.invalid-component",path,"TilemapRenderer must be an object.");continue;}
                        reject_unknown_fields(component,{"tilemapAsset","visible","collisionEnabled"},path,result.errors);
                        SceneTilemapRenderer renderer;bool valid=true;
                        if(!component.contains("tilemapAsset")||!component.at("tilemapAsset").is_string()) {
                            add_error(result.errors,"scene.invalid-string",path+"/tilemapAsset","A Tilemap asset ID is required.");valid=false;
                        } else renderer.tilemap_asset=component.at("tilemapAsset").get<std::string>();
                        valid&=read_bool(component,"visible",path,renderer.visible,result.errors);
                        valid&=read_bool(component,"collisionEnabled",path,renderer.collision_enabled,result.errors);
                        if(valid)entity.tilemap_renderer=std::move(renderer);
                    } else if (component_name == "PbrMaterial") {
                        const auto path = base_path + "/components/PbrMaterial";
                        if (!component.is_object()) {
                            add_error(result.errors, "scene.invalid-component", path, "PbrMaterial must be an object.");
                            continue;
                        }
                        reject_unknown_fields(component, {"baseColor", "metallic", "roughness", "baseColorTexture", "emissiveColor", "emissiveIntensity"}, path, result.errors);
                        ScenePbrMaterial material;
                        bool valid = read_vector3(component.value("baseColor", Json{}), path + "/baseColor", material.base_color, result.errors) &
                            read_number(component, "metallic", path, material.metallic, result.errors) &
                            read_number(component, "roughness", path, material.roughness, result.errors);
                        if (component.contains("baseColorTexture")) {
                            if (!component.at("baseColorTexture").is_string()) {
                                add_error(result.errors, "scene.invalid-field", path + "/baseColorTexture", "baseColorTexture must be an asset ID string.");
                            } else material.base_color_texture = component.at("baseColorTexture").get<std::string>();
                        }
                        if (component.contains("emissiveColor"))
                            valid &= read_vector3(component.at("emissiveColor"), path + "/emissiveColor", material.emissive_color, result.errors);
                        if (component.contains("emissiveIntensity"))
                            valid &= read_number(component, "emissiveIntensity", path, material.emissive_intensity, result.errors);
                        if (valid && result.errors.empty()) entity.pbr_material = material;
                    } else if (component_name == "ManagedScript") {
                        const auto path=base_path+"/components/ManagedScript";
                        if(!component.is_object()) {add_error(result.errors,"scene.invalid-component",path,"ManagedScript must be an object.");continue;}
                        reject_unknown_fields(component,{"instanceId","assemblyAsset","typeName","enabled","properties"},path,result.errors);
                        SceneManagedScript script;
                        bool valid=component.contains("instanceId")&&component.at("instanceId").is_string()&&
                            component.contains("typeName")&&component.at("typeName").is_string();
                        if(!valid) add_error(result.errors,"scene.invalid-managed-script",path,"ManagedScript requires string instanceId and typeName.");
                        else {script.instance_id=component.at("instanceId").get<std::string>();script.type_name=component.at("typeName").get<std::string>();}
                        if(component.contains("assemblyAsset")) {
                            if(!component.at("assemblyAsset").is_string()){add_error(result.errors,"scene.invalid-string",path+"/assemblyAsset","assemblyAsset must be a string.");valid=false;}
                            else script.assembly_asset=component.at("assemblyAsset").get<std::string>();
                        }
                        if(component.contains("enabled")) valid &= read_bool(component,"enabled",path,script.enabled,result.errors);
                        if(component.contains("properties")) {
                            if(!component.at("properties").is_object()){add_error(result.errors,"scene.invalid-field",path+"/properties","properties must be an object.");valid=false;}
                            else script.properties_json=component.at("properties").dump();
                        }
                        if(valid) entity.managed_script=std::move(script);
                    } else {
                        add_error(result.errors, "scene.unknown-component", base_path + "/components/" + component_name, "The component type is not registered for scene persistence.");
                    }
                }
            }
            document.entities.push_back(std::move(entity));
        }
    }

    if (result.errors.empty()) {
        result.errors = validate(document);
    }
    if (result.errors.empty()) {
        result.document = std::move(document);
    }
    return result;
}

std::vector<SceneDocumentError> SceneDocumentCodec::validate(const SceneDocument& document) {
    std::vector<SceneDocumentError> errors;
    if (document.schema != "noemancer.scene/0.1") {
        add_error(errors, "scene.unsupported-schema", "/schema", "Only noemancer.scene/0.1 is supported.");
    }
    if (document.scene_guid.empty()) {
        add_error(errors, "scene.empty-guid", "/sceneGuid", "Scene guid cannot be empty.");
    }
    if (document.name.empty()) {
        add_error(errors, "scene.empty-name", "/name", "Scene name cannot be empty.");
    }

    std::unordered_map<std::string, std::size_t> indices;
    std::unordered_set<std::string> script_instance_ids;
    for (std::size_t index = 0; index < document.entities.size(); ++index) {
        const auto& entity = document.entities[index];
        const auto path = "/entities/" + std::to_string(index);
        if (entity.guid.empty()) {
            add_error(errors, "scene.empty-guid", path + "/guid", "Entity guid cannot be empty.");
        } else if (!indices.emplace(entity.guid, index).second) {
            add_error(errors, "scene.duplicate-guid", path + "/guid", "Entity guid must be unique within the scene.");
        }
        if (entity.name.empty()) {
            add_error(errors, "scene.empty-name", path + "/name", "Entity name cannot be empty.");
        }
        if (entity.transform && !finite(entity.transform->position)) {
            add_error(errors, "scene.non-finite-number", path + "/components/Transform/position", "Scene numbers must be finite.");
        }
        if (entity.transform && !finite(entity.transform->rotation_euler_degrees)) {
            add_error(errors, "scene.non-finite-number", path + "/components/Transform/rotationEulerDegrees", "Transform rotation must contain finite degree values.");
        }
        if (entity.transform && (!finite(entity.transform->scale) || entity.transform->scale.x <= 0.0 ||
            entity.transform->scale.y <= 0.0 || entity.transform->scale.z <= 0.0)) {
            add_error(errors, "scene.invalid-transform-scale", path + "/components/Transform/scale",
                "Transform scale must contain finite positive values.");
        }
        if (entity.velocity && !finite(entity.velocity->linear)) {
            add_error(errors, "scene.non-finite-number", path + "/components/Velocity/linear", "Scene numbers must be finite.");
        }
        if (entity.rigid_body && ((entity.rigid_body->motion_type != "static" && entity.rigid_body->motion_type != "dynamic" &&
            entity.rigid_body->motion_type != "kinematic") || entity.rigid_body->mass <= 0.0 ||
            entity.rigid_body->gravity_factor < 0.0 || entity.rigid_body->linear_damping < 0.0)) {
            add_error(errors, "scene.invalid-rigid-body", path + "/components/RigidBody", "RigidBody values or motionType are invalid.");
        }
        if (entity.box_collider && (!finite(entity.box_collider->half_extents) || entity.box_collider->half_extents.x <= 0.0 ||
            entity.box_collider->half_extents.y <= 0.0 || entity.box_collider->half_extents.z <= 0.0 ||
            entity.box_collider->friction < 0.0 || entity.box_collider->restitution < 0.0 || entity.box_collider->restitution > 1.0)) {
            add_error(errors, "scene.invalid-box-collider", path + "/components/BoxCollider", "BoxCollider values are invalid.");
        }
        if (entity.sphere_collider && (entity.sphere_collider->radius <= 0.0 || entity.sphere_collider->friction < 0.0 ||
            entity.sphere_collider->restitution < 0.0 || entity.sphere_collider->restitution > 1.0)) {
            add_error(errors, "scene.invalid-sphere-collider", path + "/components/SphereCollider", "SphereCollider values are invalid.");
        }
        if (entity.capsule_collider && (entity.capsule_collider->radius <= 0.0 || entity.capsule_collider->half_height <= 0.0 ||
            entity.capsule_collider->friction < 0.0 || entity.capsule_collider->restitution < 0.0 || entity.capsule_collider->restitution > 1.0)) {
            add_error(errors, "scene.invalid-capsule-collider", path + "/components/CapsuleCollider", "CapsuleCollider values are invalid.");
        }
        if (entity.character_motor_2d && (entity.character_motor_2d->maximum_speed <= 0.0 ||
            entity.character_motor_2d->ground_acceleration <= 0.0 || entity.character_motor_2d->air_acceleration <= 0.0 ||
            entity.character_motor_2d->ground_deceleration <= 0.0 || entity.character_motor_2d->jump_speed <= 0.0 ||
            entity.character_motor_2d->maximum_fall_speed <= 0.0 || entity.character_motor_2d->coyote_time_seconds < 0.0 ||
            entity.character_motor_2d->jump_buffer_seconds < 0.0 || entity.character_motor_2d->ground_probe_distance <= 0.0 ||
            entity.character_motor_2d->minimum_ground_normal_y < 0.0 || entity.character_motor_2d->minimum_ground_normal_y > 1.0 ||
            entity.character_motor_2d->jump_release_velocity_factor < 0.0 || entity.character_motor_2d->jump_release_velocity_factor > 1.0 ||
            !entity.transform || !entity.velocity || !entity.rigid_body || entity.rigid_body->motion_type != "dynamic" ||
            !entity.capsule_collider)) {
            add_error(errors, "scene.invalid-character-motor-2d", path + "/components/CharacterMotor2D",
                "CharacterMotor2D requires finite positive tuning, Transform, Velocity, a dynamic RigidBody, and CapsuleCollider.");
        }
        if(entity.platform_2d&&((entity.platform_2d->collision_mode!="solid"&&entity.platform_2d->collision_mode!="one-way")||
            !finite(entity.platform_2d->motion_axis)||!std::isfinite(entity.platform_2d->motion_distance)||entity.platform_2d->motion_distance<0.0||
            !std::isfinite(entity.platform_2d->motion_period_seconds)||entity.platform_2d->motion_period_seconds<=0.0||
            !std::isfinite(entity.platform_2d->motion_phase)||!entity.transform||!entity.rigid_body||!entity.box_collider||
            (entity.platform_2d->motion_distance>0.0&&entity.rigid_body->motion_type!="kinematic")||
            (entity.platform_2d->motion_distance>0.0&&entity.platform_2d->motion_axis.x==0.0&&entity.platform_2d->motion_axis.y==0.0&&entity.platform_2d->motion_axis.z==0.0)))
            add_error(errors,"scene.invalid-platform-2d",path+"/components/Platform2D","Platform2D requires a BoxCollider and static/kinematic body; moving platforms require a non-zero axis and kinematic body.");
        if(entity.convex_hull_collider && (entity.convex_hull_collider->points.size()<4U || entity.convex_hull_collider->points.size()>256U ||
            std::ranges::any_of(entity.convex_hull_collider->points,[](const SceneVector3& point){return !finite(point);}) ||
            !convex_has_volume(entity.convex_hull_collider->points) ||
            entity.convex_hull_collider->friction<0.0 || entity.convex_hull_collider->restitution<0.0 || entity.convex_hull_collider->restitution>1.0))
            add_error(errors,"scene.invalid-convex-hull-collider",path+"/components/ConvexHullCollider","ConvexHullCollider requires 4-256 finite, non-coplanar points and valid material values.");
        const auto collider_count = static_cast<int>(entity.box_collider.has_value()) + static_cast<int>(entity.sphere_collider.has_value()) +
            static_cast<int>(entity.capsule_collider.has_value()) + static_cast<int>(entity.convex_hull_collider.has_value());
        if (collider_count > 1)
            add_error(errors, "scene.multiple-colliders", path + "/components", "An entity may define only one collider shape.");
        if (entity.animation_player && (entity.animation_player->clip_asset.empty() || !std::isfinite(entity.animation_player->playback_speed) ||
            !std::isfinite(entity.animation_player->transition_duration_seconds) || entity.animation_player->transition_duration_seconds < 0.0 ||
            (!entity.animation_player->next_clip_asset.empty() && entity.animation_player->transition_duration_seconds <= 0.0) ||
            (entity.animation_player->root_motion_mode != "ignore" && entity.animation_player->root_motion_mode != "apply"))) {
            add_error(errors, "scene.invalid-animation-player", path + "/components/AnimationPlayer", "Animation clip and playback speed are invalid.");
        }
        if (entity.camera && (!finite(entity.camera->target) || entity.camera->vertical_fov_degrees <= 1.0 ||
            entity.camera->vertical_fov_degrees >= 179.0 || entity.camera->near_clip <= 0.0 ||
            entity.camera->far_clip <= entity.camera->near_clip ||
            (entity.camera->projection != "perspective" && entity.camera->projection != "orthographic") ||
            !std::isfinite(entity.camera->orthographic_height) || entity.camera->orthographic_height <= 0.0)) {
            add_error(errors, "scene.invalid-camera", path + "/components/Camera", "Camera target and clip/fov values are invalid.");
        }
        if(entity.camera_follow_2d&&(!entity.camera||!entity.transform||entity.camera_follow_2d->target_entity_id.empty()||
            !finite(entity.camera_follow_2d->position_offset)||!finite(entity.camera_follow_2d->dead_zone)||
            !finite(entity.camera_follow_2d->minimum_center)||!finite(entity.camera_follow_2d->maximum_center)||
            entity.camera_follow_2d->dead_zone.x<0.0||entity.camera_follow_2d->dead_zone.y<0.0||
            entity.camera_follow_2d->look_ahead_distance<0.0||entity.camera_follow_2d->smoothing<0.0||
            entity.camera_follow_2d->minimum_center.x>entity.camera_follow_2d->maximum_center.x||
            entity.camera_follow_2d->minimum_center.y>entity.camera_follow_2d->maximum_center.y))
            add_error(errors,"scene.invalid-camera-follow-2d",path+"/components/CameraFollow2D","CameraFollow2D requires Camera/Transform, valid bounds, dead zone, smoothing and target ID.");
        if (entity.directional_light && (!finite(entity.directional_light->direction) ||
            !finite(entity.directional_light->color) || entity.directional_light->intensity < 0.0 ||
            entity.directional_light->ambient_intensity < 0.0)) {
            add_error(errors, "scene.invalid-light", path + "/components/DirectionalLight", "Directional light values are invalid.");
        }
        if(entity.local_light&&(!entity.transform||(entity.local_light->kind!="point"&&entity.local_light->kind!="spot")||
            !finite(entity.local_light->color)||!finite(entity.local_light->direction)||
            !std::isfinite(entity.local_light->luminous_power_lumens)||entity.local_light->luminous_power_lumens<0.0||
            !std::isfinite(entity.local_light->range_meters)||entity.local_light->range_meters<=0.0||
            !std::isfinite(entity.local_light->source_radius_meters)||entity.local_light->source_radius_meters<0.0||
            !std::isfinite(entity.local_light->inner_cone_degrees)||!std::isfinite(entity.local_light->outer_cone_degrees)||
            entity.local_light->inner_cone_degrees<0.0||entity.local_light->outer_cone_degrees>89.0||
            entity.local_light->inner_cone_degrees>entity.local_light->outer_cone_degrees||
            (entity.local_light->kind=="spot"&&entity.local_light->direction.x==0.0&&entity.local_light->direction.y==0.0&&entity.local_light->direction.z==0.0)))
            add_error(errors,"scene.invalid-local-light",path+"/components/LocalLight",
                "LocalLight requires Transform, point/spot kind, finite photometric values, positive range, and valid cone/direction.");
        if (entity.mesh_renderer && entity.mesh_renderer->mesh_asset.empty()) {
            add_error(errors, "scene.invalid-mesh", path + "/components/MeshRenderer/meshAsset", "Mesh asset ID cannot be empty.");
        }
        if(entity.sprite_renderer&&(entity.sprite_renderer->sprite_asset.empty()||entity.sprite_renderer->clip.empty()||
           entity.sprite_renderer->sorting_layer.empty()||!std::isfinite(entity.sprite_renderer->playback_speed)||
           entity.sprite_renderer->playback_speed<0.0))
            add_error(errors,"scene.invalid-sprite-renderer",path+"/components/SpriteRenderer",
                "SpriteRenderer requires stable asset/clip/layer IDs and a finite non-negative playback speed.");
        if(entity.tilemap_renderer&&(entity.tilemap_renderer->tilemap_asset.empty()||!entity.transform))
            add_error(errors,"scene.invalid-tilemap-renderer",path+"/components/TilemapRenderer",
                "TilemapRenderer requires a stable Tilemap asset ID and Transform.");
        if (entity.pbr_material && (!finite(entity.pbr_material->base_color) || !finite(entity.pbr_material->emissive_color) ||
            entity.pbr_material->emissive_intensity < 0.0 || entity.pbr_material->metallic < 0.0 ||
            entity.pbr_material->metallic > 1.0 || entity.pbr_material->roughness < 0.0 || entity.pbr_material->roughness > 1.0)) {
            add_error(errors, "scene.invalid-material", path + "/components/PbrMaterial", "PBR material values must be finite and normalized.");
        }
        if(entity.managed_script) {
            const auto properties=Json::parse(entity.managed_script->properties_json,nullptr,false);
            if(entity.managed_script->instance_id.empty()||entity.managed_script->assembly_asset.empty()||
               entity.managed_script->type_name.empty()||properties.is_discarded()||!properties.is_object())
                add_error(errors,"scene.invalid-managed-script",path+"/components/ManagedScript",
                    "ManagedScript requires stable instance/assembly/type IDs and object properties.");
            else if(!script_instance_ids.insert(entity.managed_script->instance_id).second)
                add_error(errors,"scene.duplicate-script-instance",path+"/components/ManagedScript/instanceId",
                    "ManagedScript instanceId must be unique within the scene.");
        }
    }

    for (std::size_t index = 0; index < document.entities.size(); ++index) {
        const auto& entity = document.entities[index];
        if (!entity.parent_guid.empty()) {
            const auto path = "/entities/" + std::to_string(index) + "/parent";
            if (entity.parent_guid == entity.guid) {
                add_error(errors, "scene.parent-cycle", path, "An entity cannot parent itself.");
            } else if (!indices.contains(entity.parent_guid)) {
                add_error(errors, "scene.missing-parent", path, "Parent guid does not exist in this scene.");
            }
        }
        if(entity.camera_follow_2d&&!indices.contains(entity.camera_follow_2d->target_entity_id))
            add_error(errors,"scene.missing-camera-follow-target","/entities/"+std::to_string(index)+"/components/CameraFollow2D/targetEntityId","CameraFollow2D target entity does not exist in this scene.");
    }

    enum class Visit { Unvisited, Visiting, Complete };
    std::vector<Visit> visits(document.entities.size(), Visit::Unvisited);
    std::function<void(std::size_t)> visit = [&](const std::size_t index) {
        if (visits[index] == Visit::Complete) return;
        if (visits[index] == Visit::Visiting) {
            add_error(errors, "scene.parent-cycle", "/entities/" + std::to_string(index) + "/parent", "Entity parent relationships contain a cycle.");
            return;
        }
        visits[index] = Visit::Visiting;
        const auto& parent = document.entities[index].parent_guid;
        const auto found = indices.find(parent);
        if (!parent.empty() && found != indices.end()) visit(found->second);
        visits[index] = Visit::Complete;
    };
    for (std::size_t index = 0; index < document.entities.size(); ++index) visit(index);
    return errors;
}

std::string SceneDocumentCodec::write_canonical_json(const SceneDocument& document) {
    auto entities = document.entities;
    std::ranges::sort(entities, {}, &SceneEntityDocument::guid);
    Json entity_values = Json::array();
    for (const auto& entity : entities) {
        Json components = Json::object();
        if (entity.transform) {
            components["Transform"] = {{"position", vector3_json(entity.transform->position)}};
            if (entity.transform->scale.x != 1.0 || entity.transform->scale.y != 1.0 || entity.transform->scale.z != 1.0)
                components["Transform"]["scale"] = vector3_json(entity.transform->scale);
            if (entity.transform->rotation_euler_degrees.x != 0.0 || entity.transform->rotation_euler_degrees.y != 0.0 ||
                entity.transform->rotation_euler_degrees.z != 0.0)
                components["Transform"]["rotationEulerDegrees"] = vector3_json(entity.transform->rotation_euler_degrees);
        }
        if (entity.velocity) {
            components["Velocity"] = {{"linear", vector3_json(entity.velocity->linear)}};
        }
        if (entity.rigid_body) {
            components["RigidBody"] = {{"gravityFactor", entity.rigid_body->gravity_factor}, {"linearDamping", entity.rigid_body->linear_damping},
                {"mass", entity.rigid_body->mass}, {"motionType", entity.rigid_body->motion_type}};
        }
        if (entity.box_collider) {
            components["BoxCollider"] = {{"friction", entity.box_collider->friction},
                {"halfExtents", vector3_json(entity.box_collider->half_extents)}, {"restitution", entity.box_collider->restitution}};
            if(entity.box_collider->is_trigger)components["BoxCollider"]["isTrigger"]=true;
        }
        if (entity.sphere_collider) {
            components["SphereCollider"] = {{"friction", entity.sphere_collider->friction}, {"radius", entity.sphere_collider->radius},
                {"restitution", entity.sphere_collider->restitution}};
            if(entity.sphere_collider->is_trigger)components["SphereCollider"]["isTrigger"]=true;
        }
        if (entity.capsule_collider) {
            components["CapsuleCollider"] = {{"friction", entity.capsule_collider->friction}, {"halfHeight", entity.capsule_collider->half_height},
                {"radius", entity.capsule_collider->radius}, {"restitution", entity.capsule_collider->restitution}};
            if(entity.capsule_collider->is_trigger)components["CapsuleCollider"]["isTrigger"]=true;
        }
        if (entity.character_motor_2d) {
            components["CharacterMotor2D"] = {
                {"airAcceleration", entity.character_motor_2d->air_acceleration},
                {"coyoteTimeSeconds", entity.character_motor_2d->coyote_time_seconds},
                {"groundAcceleration", entity.character_motor_2d->ground_acceleration},
                {"groundDeceleration", entity.character_motor_2d->ground_deceleration},
                {"groundProbeDistance", entity.character_motor_2d->ground_probe_distance},
                {"jumpBufferSeconds", entity.character_motor_2d->jump_buffer_seconds},
                {"jumpSpeed", entity.character_motor_2d->jump_speed},
                {"maximumFallSpeed", entity.character_motor_2d->maximum_fall_speed},
                {"maximumSpeed", entity.character_motor_2d->maximum_speed},
                {"minimumGroundNormalY", entity.character_motor_2d->minimum_ground_normal_y},
                {"jumpReleaseVelocityFactor", entity.character_motor_2d->jump_release_velocity_factor}
            };
        }
        if(entity.platform_2d) components["Platform2D"]={{"collisionMode",entity.platform_2d->collision_mode},
            {"motionAxis",vector3_json(entity.platform_2d->motion_axis)},{"motionDistance",entity.platform_2d->motion_distance},
            {"motionPeriodSeconds",entity.platform_2d->motion_period_seconds},{"motionPhase",entity.platform_2d->motion_phase}};
        if(entity.convex_hull_collider) {
            Json points=Json::array(); for(const auto& point:entity.convex_hull_collider->points) points.push_back(vector3_json(point));
            components["ConvexHullCollider"]={{"friction",entity.convex_hull_collider->friction},{"points",std::move(points)},
                {"restitution",entity.convex_hull_collider->restitution}};
            if(entity.convex_hull_collider->is_trigger)components["ConvexHullCollider"]["isTrigger"]=true;
        }
        if (entity.animation_player) {
            components["AnimationPlayer"] = {{"clipAsset", entity.animation_player->clip_asset}, {"looping", entity.animation_player->looping},
                {"playbackSpeed", entity.animation_player->playback_speed}, {"playing", entity.animation_player->playing}};
            if (!entity.animation_player->next_clip_asset.empty()) {
                components["AnimationPlayer"]["nextClipAsset"] = entity.animation_player->next_clip_asset;
                components["AnimationPlayer"]["transitionDurationSeconds"] = entity.animation_player->transition_duration_seconds;
            }
            if (entity.animation_player->root_motion_mode != "ignore")
                components["AnimationPlayer"]["rootMotionMode"] = entity.animation_player->root_motion_mode;
            if (!entity.animation_player->state_machine_asset.empty())
                components["AnimationPlayer"]["stateMachineAsset"] = entity.animation_player->state_machine_asset;
            if (!entity.animation_player->animation_graph_asset.empty())
                components["AnimationPlayer"]["animationGraphAsset"] = entity.animation_player->animation_graph_asset;
        }
        if (entity.camera) {
            components["Camera"] = {
                {"farClip", entity.camera->far_clip},
                {"nearClip", entity.camera->near_clip},
                {"primary", entity.camera->primary},
                {"target", vector3_json(entity.camera->target)},
                {"verticalFovDegrees", entity.camera->vertical_fov_degrees}
            };
            if (entity.camera->projection != "perspective") components["Camera"]["projection"] = entity.camera->projection;
            if (entity.camera->projection == "orthographic") components["Camera"]["orthographicHeight"] = entity.camera->orthographic_height;
        }
        if(entity.camera_follow_2d) components["CameraFollow2D"]={{"targetEntityId",entity.camera_follow_2d->target_entity_id},
            {"positionOffset",vector3_json(entity.camera_follow_2d->position_offset)},{"deadZone",vector3_json(entity.camera_follow_2d->dead_zone)},
            {"lookAheadDistance",entity.camera_follow_2d->look_ahead_distance},{"smoothing",entity.camera_follow_2d->smoothing},
            {"minimumCenter",vector3_json(entity.camera_follow_2d->minimum_center)},{"maximumCenter",vector3_json(entity.camera_follow_2d->maximum_center)}};
        if (entity.directional_light) {
            components["DirectionalLight"] = {
                {"ambientIntensity", entity.directional_light->ambient_intensity},
                {"castsShadows", entity.directional_light->casts_shadows},
                {"color", vector3_json(entity.directional_light->color)},
                {"direction", vector3_json(entity.directional_light->direction)},
                {"intensity", entity.directional_light->intensity}
            };
        }
        if(entity.local_light)components["LocalLight"]={{"castsShadows",entity.local_light->casts_shadows},
            {"color",vector3_json(entity.local_light->color)},{"direction",vector3_json(entity.local_light->direction)},
            {"innerConeDegrees",entity.local_light->inner_cone_degrees},{"kind",entity.local_light->kind},
            {"luminousPowerLumens",entity.local_light->luminous_power_lumens},{"outerConeDegrees",entity.local_light->outer_cone_degrees},
            {"rangeMeters",entity.local_light->range_meters},{"sourceRadiusMeters",entity.local_light->source_radius_meters}};
        if (entity.mesh_renderer) {
            components["MeshRenderer"] = {
                {"castsShadows", entity.mesh_renderer->casts_shadows},
                {"meshAsset", entity.mesh_renderer->mesh_asset},
                {"receivesShadows", entity.mesh_renderer->receives_shadows},
                {"visible", entity.mesh_renderer->visible}
            };
        }
        if(entity.sprite_renderer)components["SpriteRenderer"]={{"clip",entity.sprite_renderer->clip},
            {"flipX",entity.sprite_renderer->flip_x},{"flipY",entity.sprite_renderer->flip_y},
            {"playbackSpeed",entity.sprite_renderer->playback_speed},{"playing",entity.sprite_renderer->playing},
            {"sortingLayer",entity.sprite_renderer->sorting_layer},{"sortingOrder",entity.sprite_renderer->sorting_order},
            {"spriteAsset",entity.sprite_renderer->sprite_asset},{"visible",entity.sprite_renderer->visible}};
        if(entity.tilemap_renderer)components["TilemapRenderer"]={{"collisionEnabled",entity.tilemap_renderer->collision_enabled},
            {"tilemapAsset",entity.tilemap_renderer->tilemap_asset},{"visible",entity.tilemap_renderer->visible}};
        if (entity.pbr_material) {
            components["PbrMaterial"] = {
                {"baseColor", vector3_json(entity.pbr_material->base_color)},
                {"metallic", entity.pbr_material->metallic},
                {"roughness", entity.pbr_material->roughness}
            };
            if (!entity.pbr_material->base_color_texture.empty())
                components["PbrMaterial"]["baseColorTexture"] = entity.pbr_material->base_color_texture;
            if (entity.pbr_material->emissive_intensity > 0.0) {
                components["PbrMaterial"]["emissiveColor"] = vector3_json(entity.pbr_material->emissive_color);
                components["PbrMaterial"]["emissiveIntensity"] = entity.pbr_material->emissive_intensity;
            }
        }
        if(entity.managed_script) {
            auto properties=Json::parse(entity.managed_script->properties_json,nullptr,false);
            if(properties.is_discarded()||!properties.is_object()) properties=Json::object();
            components["ManagedScript"]={{"assemblyAsset",entity.managed_script->assembly_asset},{"enabled",entity.managed_script->enabled},
                {"instanceId",entity.managed_script->instance_id},{"properties",std::move(properties)},{"typeName",entity.managed_script->type_name}};
        }
        entity_values.push_back({
            {"components", std::move(components)},
            {"guid", entity.guid},
            {"name", entity.name},
            {"parent", entity.parent_guid.empty() ? Json(nullptr) : Json(entity.parent_guid)}
        });
    }
    const Json output = {
        {"entities", std::move(entity_values)},
        {"name", document.name},
        {"sceneGuid", document.scene_guid},
        {"schema", document.schema}
    };
    return output.dump(2) + "\n";
}

SceneDocument make_bootstrap_scene_document() {
    return SceneDocument{
        .scene_guid = "scene.bootstrap",
        .name = "Bootstrap Scene",
        .source_uri = "asset://scenes/bootstrap.scene.json",
        .entities = {
            SceneEntityDocument{
                .guid = "entity.bootstrap-root",
                .name = "Bootstrap Root"
            },
            SceneEntityDocument{
                .guid = "entity.camera.editor",
                .name = "Editor Camera",
                .parent_guid = "entity.bootstrap-root",
                .transform = SceneTransform{{7.0, 5.5, 8.5}},
                .camera = SceneCamera{{0.0, 1.0, 0.0}, 45.0, 0.1, 100.0, true}
            },
            SceneEntityDocument{
                .guid = "entity.demo-cube",
                .name = "Demo Cube",
                .parent_guid = "entity.bootstrap-root",
                .transform = SceneTransform{{0.0, 1.05, 0.0}},
                .velocity = SceneVelocity{{0.01, 0.0, 0.0}},
                .rigid_body = SceneRigidBody{"dynamic", 1.0, 1.0, 0.08},
                .box_collider = SceneBoxCollider{{0.5, 0.5, 0.5}, 0.55, 0.05},
                .mesh_renderer = SceneMeshRenderer{"asset.primitive.cube", true, true, true},
                .pbr_material = ScenePbrMaterial{{0.15, 0.58, 0.92}, 0.1, 0.42}
            },
            SceneEntityDocument{
                .guid = "entity.demo-cube-secondary",
                .name = "Secondary Cube",
                .parent_guid = "entity.bootstrap-root",
                .transform = SceneTransform{{2.7, 1.05, -1.4}},
                .mesh_renderer = SceneMeshRenderer{"asset.primitive.cube", true, true, true},
                .pbr_material = ScenePbrMaterial{{0.92, 0.32, 0.18}, 0.0, 0.58, "asset.texture.checker", {1.0, 0.18, 0.04}, 4.0}
            },
            SceneEntityDocument{
                .guid = "entity.demo-skeletal-cube",
                .name = "Ozz Skeletal Cube",
                .parent_guid = "entity.bootstrap-root",
                .transform = SceneTransform{{2.5, 1.2, 2.4}},
                .animation_player = SceneAnimationPlayer{"asset.animation.test-bob", 1.0, true, true},
                .mesh_renderer = SceneMeshRenderer{"asset.primitive.cube", true, true, true},
                .pbr_material = ScenePbrMaterial{{0.64, 0.28, 0.95}, 0.08, 0.36}
            },
            SceneEntityDocument{
                .guid = "entity.demo-sphere",
                .name = "Jolt Dynamic Sphere",
                .parent_guid = "entity.bootstrap-root",
                .transform = SceneTransform{{-1.8, 3.2, -0.8}},
                .rigid_body = SceneRigidBody{"dynamic", 1.2, 1.0, 0.03},
                .sphere_collider = SceneSphereCollider{1.0, 0.45, 0.35},
                .mesh_renderer = SceneMeshRenderer{"asset.primitive.sphere", true, true, true},
                .pbr_material = ScenePbrMaterial{{0.12, 0.78, 0.55}, 0.15, 0.3}
            },
            SceneEntityDocument{
                .guid = "entity.ground",
                .name = "Ground Plane",
                .parent_guid = "entity.bootstrap-root",
                .transform = SceneTransform{{0.0, 0.0, 0.0}},
                .rigid_body = SceneRigidBody{"static", 1.0, 0.0, 0.0},
                .box_collider = SceneBoxCollider{{7.0, 0.05, 7.0}, 0.8, 0.0},
                .mesh_renderer = SceneMeshRenderer{"asset.primitive.plane", true, false, true},
                .pbr_material = ScenePbrMaterial{{0.24, 0.29, 0.34}, 0.0, 0.8}
            },
            SceneEntityDocument{
                .guid = "entity.sun",
                .name = "Sun",
                .parent_guid = "entity.bootstrap-root",
                .directional_light = SceneDirectionalLight{{-0.55, -1.0, -0.35}, {1.0, 0.96, 0.88}, 0.95, 0.18, true}
            },
            SceneEntityDocument{
                .guid = "entity.test-alien",
                .name = "Kenney Alien",
                .parent_guid = "entity.bootstrap-root",
                .transform = SceneTransform{{-3.2, 0.4, 2.8}},
                .animation_player = SceneAnimationPlayer{"asset.animation.test-bob", 1.0, true, true},
                .mesh_renderer = SceneMeshRenderer{"asset.test.kenney.alien", true, true, true}
            },
            SceneEntityDocument{
                .guid = "entity.test-mixamo-rumba",
                .name = "Mixamo Rumba Dancer",
                .parent_guid = "entity.bootstrap-root",
                .transform = SceneTransform{{-4.0, 0.0, -1.5}},
                .animation_player = SceneAnimationPlayer{
                    "asset.local.mixamo.rumba-02/skin/unnamed/animation/take-001", 1.0, true, true},
                .mesh_renderer = SceneMeshRenderer{"asset.local.mixamo.rumba-02", true, true, true}
            }
        }
    };
}

SceneDocument make_render_stress_scene_document(const std::uint32_t instance_count,
                                                const std::uint32_t offscreen_percent) {
    auto document = make_bootstrap_scene_document();
    std::erase_if(document.entities,[](const SceneEntityDocument& entity) {
        return entity.guid=="entity.test-mixamo-rumba";
    });
    const auto bounded_offscreen_percent=std::min(offscreen_percent,100U);
    document.scene_guid = bounded_offscreen_percent>0?"scene.gpu-visibility-stress":"scene.render-stress";
    document.name = bounded_offscreen_percent>0?"GPU Visibility Stress Scene":"Render Stress Scene";
    document.source_uri = bounded_offscreen_percent>0
        ?"generated://scenes/gpu-visibility-stress.scene.json"
        :"generated://scenes/render-stress.scene.json";
    std::uint32_t columns = 1;
    while (columns * columns < instance_count) ++columns;
    constexpr double spacing = 2.05;
    const double center = static_cast<double>(columns - 1U) * 0.5;
    const double grid_extent=std::max(12.0,static_cast<double>(columns)*spacing);
    document.entities[1].transform->position={0.0,grid_extent*0.72,grid_extent*0.82};
    document.entities[1].camera->target={0.0,1.0,0.0};
    document.entities[1].camera->vertical_fov_degrees = 62.0;
    document.entities[1].camera->far_clip=std::max(200.0,grid_extent*4.0);
    document.entities.reserve(document.entities.size() + instance_count);
    const auto offscreen_count=static_cast<std::uint32_t>(
        (static_cast<std::uint64_t>(instance_count)*bounded_offscreen_percent)/100U);
    const auto first_offscreen=instance_count-offscreen_count;
    for (std::uint32_t index = 0; index < instance_count; ++index) {
        const auto column = index % columns;
        const auto row = index / columns;
        const auto offscreen=index>=first_offscreen;
        document.entities.push_back(SceneEntityDocument{
            .guid = "entity.render-stress-cube-" + std::to_string(index),
            .name = "Render Stress Cube " + std::to_string(index),
            .parent_guid = "entity.bootstrap-root",
            .transform = SceneTransform{{
                (static_cast<double>(column) - center) * spacing+(offscreen?grid_extent*12.0:0.0),
                1.05,
                (static_cast<double>(row) - center) * spacing}},
            .mesh_renderer = SceneMeshRenderer{"asset.primitive.cube", true, true, true},
            .pbr_material = ScenePbrMaterial{{
                0.18+0.62*static_cast<double>((index*17U)%97U)/96.0,
                0.16+0.68*static_cast<double>((index*31U)%89U)/88.0,
                0.20+0.60*static_cast<double>((index*47U)%83U)/82.0},
                0.02+0.78*static_cast<double>(index%11U)/10.0,
                0.12+0.76*static_cast<double>((index/11U)%13U)/12.0}
        });
    }
    return document;
}

SceneDocument make_animation_physics_stress_scene_document() {
    SceneDocument document{
        .scene_guid="scene.animation-physics-stress",
        .name="Animation Physics Stress Scene",
        .source_uri="generated://scenes/animation-physics-stress.scene.json"
    };
    document.entities={
        SceneEntityDocument{.guid="entity.sim-stress-root",.name="Simulation Stress Root",.transform=SceneTransform{}},
        SceneEntityDocument{.guid="entity.sim-stress-camera",.name="Simulation Stress Camera",.parent_guid="entity.sim-stress-root",
            .transform=SceneTransform{{0.0,20.0,44.0}},.camera=SceneCamera{{0.0,4.0,0.0},58.0,0.1,180.0,true}},
        SceneEntityDocument{.guid="entity.sim-stress-sun",.name="Simulation Stress Sun",.parent_guid="entity.sim-stress-root",
            .directional_light=SceneDirectionalLight{{-0.45,-1.0,-0.3},{1.0,0.96,0.9},1.05,0.2,true}},
        SceneEntityDocument{.guid="entity.sim-stress-floor",.name="Containment Floor",.parent_guid="entity.sim-stress-root",
            .transform=SceneTransform{{0.0,-0.6,0.0},{22.0,0.5,22.0}},.rigid_body=SceneRigidBody{"static",1.0,0.0,0.0},
            .box_collider=SceneBoxCollider{{22.0,0.5,22.0},0.55,0.88},.mesh_renderer=SceneMeshRenderer{"asset.primitive.cube",true,true,true},
            .pbr_material=ScenePbrMaterial{{0.13,0.16,0.2},0.15,0.72}}
    };
    constexpr std::array<SceneVector3,4> wall_positions{{{-22.0,10.0,0.0},{22.0,10.0,0.0},{0.0,10.0,-22.0},{0.0,10.0,22.0}}};
    constexpr std::array<SceneVector3,4> wall_extents{{{0.5,11.0,22.0},{0.5,11.0,22.0},{22.0,11.0,0.5},{22.0,11.0,0.5}}};
    for(std::size_t index=0;index<wall_positions.size();++index)document.entities.push_back(SceneEntityDocument{
        .guid="entity.sim-stress-wall-"+std::to_string(index),.name="Containment Wall "+std::to_string(index),
        .parent_guid="entity.sim-stress-root",.transform=SceneTransform{wall_positions[index]},
        .rigid_body=SceneRigidBody{"static",1.0,0.0,0.0},.box_collider=SceneBoxCollider{wall_extents[index],0.35,0.92}});

    constexpr std::uint32_t animated_actor_count=64;
    document.entities.reserve(document.entities.size()+animated_actor_count+256U);
    for(std::uint32_t index=0;index<animated_actor_count;++index) {
        const auto column=index%8U,row=index/8U;
        document.entities.push_back(SceneEntityDocument{
            .guid="entity.sim-stress-animated-"+std::to_string(index),.name="Animated Actor "+std::to_string(index),
            .parent_guid="entity.sim-stress-root",
            .transform=SceneTransform{{-10.0+(static_cast<double>(column)-3.5)*2.2,0.0,(static_cast<double>(row)-3.5)*2.4}},
            .animation_player=SceneAnimationPlayer{"asset.local.mixamo.rumba-02/skin/unnamed/animation/take-001",
                0.82+static_cast<double>(index%7U)*0.06,true,true},
            .mesh_renderer=SceneMeshRenderer{"asset.local.mixamo.rumba-02",true,true,true}});
    }
    constexpr std::uint32_t dynamic_body_count=256;
    for(std::uint32_t index=0;index<dynamic_body_count;++index) {
        const auto x=index%16U,z=(index/16U)%8U,y=index/128U;
        const double vx=((index*17U)%19U<9U?-1.0:1.0)*(1.5+static_cast<double>(index%5U)*0.2);
        const double vz=((index*29U)%23U<11U?-1.0:1.0)*(1.2+static_cast<double>(index%7U)*0.16);
        document.entities.push_back(SceneEntityDocument{
            .guid="entity.sim-stress-body-"+std::to_string(index),.name="Dynamic Body "+std::to_string(index),
            .parent_guid="entity.sim-stress-root",
            .transform=SceneTransform{{2.5+static_cast<double>(x)*1.15,4.0+static_cast<double>(y)*1.2,
                -9.0+static_cast<double>(z)*1.2}},.velocity=SceneVelocity{{vx,0.2,vz}},
            .rigid_body=SceneRigidBody{"dynamic",0.8+static_cast<double>(index%5U)*0.15,0.18,0.001},
            .sphere_collider=SceneSphereCollider{0.48,0.24,0.92},
            .mesh_renderer=SceneMeshRenderer{"asset.primitive.sphere",true,true,true},
            .pbr_material=ScenePbrMaterial{{0.18+0.65*static_cast<double>((index*13U)%31U)/30.0,
                0.2+0.58*static_cast<double>((index*7U)%29U)/28.0,0.28+0.54*static_cast<double>((index*19U)%37U)/36.0},
                0.08,0.34}}
        );
    }
    return document;
}

} // namespace noemancer
