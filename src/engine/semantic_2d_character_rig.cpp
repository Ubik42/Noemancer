#include "engine/semantic_2d_character_rig.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <nlohmann/json.hpp>
#include <set>
#include <unordered_map>
#include <unordered_set>

namespace noemancer {
namespace {

using Json = nlohmann::json;

void issue(std::vector<Semantic2DCharacterRigError>& errors, std::string code,
    std::string path, std::string message) {
    errors.push_back({std::move(code), std::move(path), std::move(message)});
}

bool object_fields(const Json& value, const std::initializer_list<std::string_view> allowed,
    const std::string& path, std::vector<Semantic2DCharacterRigError>& errors) {
    if (!value.is_object()) {
        issue(errors, "semantic-2d-rig.invalid-object", path, "Expected an object.");
        return false;
    }
    bool valid = true;
    for (const auto& [name, unused] : value.items()) {
        static_cast<void>(unused);
        if (std::ranges::find(allowed, name) == allowed.end()) {
            issue(errors, "semantic-2d-rig.unknown-field", path + "/" + name,
                "Unknown field in prototype document.");
            valid = false;
        }
    }
    return valid;
}

bool text(const Json& object, const char* name, const std::string& path, std::string& output,
    std::vector<Semantic2DCharacterRigError>& errors, const bool required = true) {
    if (!object.contains(name)) {
        if (required) issue(errors, "semantic-2d-rig.missing-field", path + "/" + name,
            "Required string is missing.");
        return !required;
    }
    if (!object.at(name).is_string()) {
        issue(errors, "semantic-2d-rig.invalid-string", path + "/" + name,
            "Expected a string.");
        return false;
    }
    output = object.at(name).get<std::string>();
    if (required && output.empty()) {
        issue(errors, "semantic-2d-rig.empty-string", path + "/" + name,
            "Required string cannot be empty.");
        return false;
    }
    return true;
}

template <typename Number>
void number(const Json& object, const char* name, const std::string& path, Number& output,
    std::vector<Semantic2DCharacterRigError>& errors, const bool required = true) {
    if (!object.contains(name)) {
        if (required) issue(errors, "semantic-2d-rig.missing-field", path + "/" + name,
            "Required number is missing.");
    } else if (!object.at(name).is_number()) {
        issue(errors, "semantic-2d-rig.invalid-number", path + "/" + name,
            "Expected a number.");
    } else output = object.at(name).get<Number>();
}

void boolean(const Json& object, const char* name, const std::string& path, bool& output,
    std::vector<Semantic2DCharacterRigError>& errors, const bool required = false) {
    if (!object.contains(name)) {
        if (required) issue(errors, "semantic-2d-rig.missing-field", path + "/" + name,
            "Required boolean is missing.");
    } else if (!object.at(name).is_boolean()) {
        issue(errors, "semantic-2d-rig.invalid-boolean", path + "/" + name,
            "Expected a boolean.");
    } else output = object.at(name).get<bool>();
}

bool id_shape(const std::string& id) {
    if (id.empty() || id.size() > semantic_2d_character_rig_max_id_bytes) return false;
    return std::ranges::all_of(id, [](const unsigned char value) {
        return std::isalnum(value) != 0 || value == '.' || value == '-' || value == '_' || value == ':';
    });
}

template <typename Value, typename Member>
bool unique_ids(const std::vector<Value>& values, Member member, const std::string& path,
    std::vector<Semantic2DCharacterRigError>& errors) {
    std::unordered_set<std::string> ids;
    bool valid = true;
    for (std::size_t index = 0; index < values.size(); ++index) {
        const auto& id = values[index].*member;
        if (!id_shape(id) || !ids.insert(id).second) {
            issue(errors, "semantic-2d-rig.invalid-or-duplicate-id",
                path + "/" + std::to_string(index) + "/id",
                "Semantic IDs must be bounded, portable and unique in their domain.");
            valid = false;
        }
    }
    return valid;
}

template <typename Value, typename Member>
std::vector<Value> sorted(std::vector<Value> values, Member member) {
    std::ranges::sort(values, {}, member);
    return values;
}

} // namespace

Semantic2DCharacterRigParseResult Semantic2DCharacterRigCodec::parse_json(
    const std::string_view source) {
    Semantic2DCharacterRigParseResult result;
    if (source.size() > semantic_2d_character_rig_max_source_bytes) {
        issue(result.errors, "semantic-2d-rig.source-too-large", "/", "Prototype source exceeds 256 KiB.");
        return result;
    }
    const auto input = Json::parse(source, nullptr, false);
    if (input.is_discarded() || !input.is_object()) {
        issue(result.errors, "semantic-2d-rig.invalid-json", "/", "Rig source must be a JSON object.");
        return result;
    }
    object_fields(input, {"schema", "characterId", "displayName", "parts", "joints",
        "materialChannels", "poses", "actions", "directions", "provenance"}, "", result.errors);
    Semantic2DCharacterRigDocument document;
    text(input, "schema", "", document.schema, result.errors);
    text(input, "characterId", "", document.character_id, result.errors);
    text(input, "displayName", "", document.display_name, result.errors);

    const auto array = [&](const char* name) -> const Json* {
        if (!input.contains(name) || !input.at(name).is_array()) {
            issue(result.errors, "semantic-2d-rig.invalid-array", std::string("/") + name,
                "Expected an array.");
            return nullptr;
        }
        return &input.at(name);
    };
    if (const auto* values = array("parts")) for (std::size_t i = 0; i < values->size(); ++i) {
        const auto path = "/parts/" + std::to_string(i); const auto& value = values->at(i);
        if (!object_fields(value, {"id", "displayName", "jointId", "sourceAsset",
            "materialChannelId", "zOrder", "pivot", "visible"}, path, result.errors)) continue;
        Semantic2DCharacterRigPart part;
        text(value, "id", path, part.id, result.errors); text(value, "displayName", path, part.display_name, result.errors);
        text(value, "jointId", path, part.joint_id, result.errors); text(value, "sourceAsset", path, part.source_asset, result.errors);
        text(value, "materialChannelId", path, part.material_channel_id, result.errors);
        number(value, "zOrder", path, part.z_order, result.errors); boolean(value, "visible", path, part.visible, result.errors);
        if (value.contains("pivot") && value.at("pivot").is_array() && value.at("pivot").size() == 2U &&
            value.at("pivot")[0].is_number() && value.at("pivot")[1].is_number()) {
            part.pivot_x = value.at("pivot")[0].get<float>(); part.pivot_y = value.at("pivot")[1].get<float>();
        } else issue(result.errors, "semantic-2d-rig.invalid-pair", path + "/pivot", "Expected [x,y].");
        document.parts.push_back(std::move(part));
    }
    if (const auto* values = array("joints")) for (std::size_t i = 0; i < values->size(); ++i) {
        const auto path = "/joints/" + std::to_string(i); const auto& value = values->at(i);
        if (!object_fields(value, {"id", "parentId", "rest"}, path, result.errors)) continue;
        Semantic2DCharacterRigJoint joint; text(value, "id", path, joint.id, result.errors);
        text(value, "parentId", path, joint.parent_id, result.errors, false);
        if (value.contains("rest") && value.at("rest").is_array() && value.at("rest").size() == 3U &&
            std::ranges::all_of(value.at("rest"), [](const auto& item) { return item.is_number(); })) {
            joint.rest_x = value.at("rest")[0].get<float>(); joint.rest_y = value.at("rest")[1].get<float>();
            joint.rest_rotation_degrees = value.at("rest")[2].get<float>();
        } else issue(result.errors, "semantic-2d-rig.invalid-triple", path + "/rest", "Expected [x,y,rotationDegrees].");
        document.joints.push_back(std::move(joint));
    }
    if (const auto* values = array("materialChannels")) for (std::size_t i = 0; i < values->size(); ++i) {
        const auto path = "/materialChannels/" + std::to_string(i); const auto& value = values->at(i);
        if (!object_fields(value, {"id", "semantic", "sourceAsset", "textureAsset", "colorSpace", "intensity", "optional"}, path, result.errors)) continue;
        Semantic2DCharacterRigMaterialChannel channel;
        text(value, "id", path, channel.id, result.errors); text(value, "semantic", path, channel.semantic, result.errors);
        text(value, "sourceAsset", path, channel.source_asset, result.errors, false);
        text(value, "textureAsset", path, channel.texture_asset, result.errors, false);
        text(value, "colorSpace", path, channel.color_space, result.errors);
        number(value, "intensity", path, channel.intensity, result.errors); boolean(value, "optional", path, channel.optional, result.errors);
        document.material_channels.push_back(std::move(channel));
    }
    if (const auto* values = array("poses")) for (std::size_t i = 0; i < values->size(); ++i) {
        const auto path = "/poses/" + std::to_string(i); const auto& value = values->at(i);
        if (!object_fields(value, {"id", "directionId", "frameKey", "partTransforms"}, path, result.errors)) continue;
        Semantic2DCharacterRigPose pose; text(value, "id", path, pose.id, result.errors);
        text(value, "directionId", path, pose.direction_id, result.errors); text(value, "frameKey", path, pose.frame_key, result.errors);
        if (!value.contains("partTransforms") || !value.at("partTransforms").is_array()) {
            issue(result.errors, "semantic-2d-rig.invalid-array", path + "/partTransforms", "Expected an array.");
        } else for (std::size_t j = 0; j < value.at("partTransforms").size(); ++j) {
            const auto item_path = path + "/partTransforms/" + std::to_string(j); const auto& item = value.at("partTransforms")[j];
            if (!object_fields(item, {"partId", "position", "rotationDegrees", "scale", "opacity", "visible", "materialChannelId", "celKey"}, item_path, result.errors)) continue;
            Semantic2DCharacterRigPosePartTransform transform; text(item, "partId", item_path, transform.part_id, result.errors);
            number(item, "rotationDegrees", item_path, transform.rotation_degrees, result.errors);
            number(item, "opacity", item_path, transform.opacity, result.errors); boolean(item, "visible", item_path, transform.visible, result.errors);
            text(item, "materialChannelId", item_path, transform.material_channel_id, result.errors, false);
            text(item, "celKey", item_path, transform.cel_key, result.errors, false);
            const auto pair = [&](const char* name, float& first, float& second) {
                if (item.contains(name) && item.at(name).is_array() && item.at(name).size() == 2U && item.at(name)[0].is_number() && item.at(name)[1].is_number()) {
                    first = item.at(name)[0].get<float>(); second = item.at(name)[1].get<float>();
                } else issue(result.errors, "semantic-2d-rig.invalid-pair", item_path + "/" + name, "Expected two numbers.");
            };
            pair("position", transform.x, transform.y); pair("scale", transform.scale_x, transform.scale_y);
            pose.part_transforms.push_back(std::move(transform));
        }
        document.poses.push_back(std::move(pose));
    }
    if (const auto* values = array("actions")) for (std::size_t i = 0; i < values->size(); ++i) {
        const auto path = "/actions/" + std::to_string(i); const auto& value = values->at(i);
        if (!object_fields(value, {"id", "displayName", "looping", "sampleRate", "frames"}, path, result.errors)) continue;
        Semantic2DCharacterRigAction action; text(value, "id", path, action.id, result.errors);
        text(value, "displayName", path, action.display_name, result.errors); boolean(value, "looping", path, action.looping, result.errors);
        number(value, "sampleRate", path, action.sample_rate, result.errors);
        if (!value.contains("frames") || !value.at("frames").is_array()) issue(result.errors, "semantic-2d-rig.invalid-array", path + "/frames", "Expected an array.");
        else for (std::size_t j = 0; j < value.at("frames").size(); ++j) {
            const auto item_path = path + "/frames/" + std::to_string(j); const auto& item = value.at("frames")[j];
            if (!object_fields(item, {"key", "poseId", "durationMs"}, item_path, result.errors)) continue;
            Semantic2DCharacterRigActionFrame frame; text(item, "key", item_path, frame.key, result.errors);
            text(item, "poseId", item_path, frame.pose_id, result.errors);
            if (!item.contains("durationMs") || !item.at("durationMs").is_number_unsigned()) issue(result.errors, "semantic-2d-rig.invalid-duration", item_path + "/durationMs", "Expected an unsigned duration.");
            else frame.duration_ms = item.at("durationMs").get<std::uint32_t>();
            action.frames.push_back(std::move(frame));
        }
        document.actions.push_back(std::move(action));
    }
    if (const auto* values = array("directions")) for (std::size_t i = 0; i < values->size(); ++i) {
        const auto path = "/directions/" + std::to_string(i); const auto& value = values->at(i);
        if (!object_fields(value, {"id", "displayName", "angleDegrees", "mirrorOfDirectionId", "overrideOfDirectionId", "mirrorX", "poseOverrides"}, path, result.errors)) continue;
        Semantic2DCharacterRigDirection direction; text(value, "id", path, direction.id, result.errors);
        text(value, "displayName", path, direction.display_name, result.errors);
        number(value, "angleDegrees", path, direction.angle_degrees, result.errors);
        text(value, "mirrorOfDirectionId", path, direction.mirror_of_direction_id, result.errors, false);
        text(value, "overrideOfDirectionId", path, direction.override_of_direction_id, result.errors, false);
        boolean(value, "mirrorX", path, direction.mirror_x, result.errors);
        if (!value.contains("poseOverrides") || !value.at("poseOverrides").is_object()) issue(result.errors, "semantic-2d-rig.invalid-object", path + "/poseOverrides", "Expected an ID map.");
        else for (const auto& [base, replacement] : value.at("poseOverrides").items()) {
            if (!replacement.is_string()) issue(result.errors, "semantic-2d-rig.invalid-string", path + "/poseOverrides/" + base, "Expected a pose ID.");
            else direction.pose_overrides.emplace(base, replacement.get<std::string>());
        }
        document.directions.push_back(std::move(direction));
    }
    if (!input.contains("provenance") || !input.at("provenance").is_object()) issue(result.errors, "semantic-2d-rig.invalid-object", "/provenance", "Expected provenance object.");
    else {
        const auto& value = input.at("provenance"); object_fields(value, {"sourceUri", "sourceSha256", "generator", "license"}, "/provenance", result.errors);
        text(value, "sourceUri", "/provenance", document.provenance.source_uri, result.errors);
        text(value, "sourceSha256", "/provenance", document.provenance.source_sha256, result.errors);
        text(value, "generator", "/provenance", document.provenance.generator, result.errors);
        text(value, "license", "/provenance", document.provenance.license, result.errors);
    }
    auto semantic = validate(document); result.errors.insert(result.errors.end(), semantic.begin(), semantic.end());
    if (result.errors.empty()) result.document = std::move(document);
    return result;
}

std::vector<Semantic2DCharacterRigError> Semantic2DCharacterRigCodec::validate(
    const Semantic2DCharacterRigDocument& document) {
    std::vector<Semantic2DCharacterRigError> errors;
    if (document.schema != semantic_2d_character_rig_schema) issue(errors, "semantic-2d-rig.unsupported-schema", "/schema", "Expected the disposable 0.1 prototype schema.");
    if (!id_shape(document.character_id)) issue(errors, "semantic-2d-rig.invalid-character-id", "/characterId", "Character ID is not portable.");
    const auto bounded = [&](const std::size_t size, const std::size_t maximum, const char* path) {
        if (size == 0U || size > maximum) issue(errors, "semantic-2d-rig.invalid-count", path, "Collection is empty or exceeds its prototype bound.");
    };
    bounded(document.parts.size(), semantic_2d_character_rig_max_parts, "/parts");
    bounded(document.joints.size(), semantic_2d_character_rig_max_joints, "/joints");
    bounded(document.material_channels.size(), semantic_2d_character_rig_max_material_channels, "/materialChannels");
    bounded(document.poses.size(), semantic_2d_character_rig_max_poses, "/poses");
    bounded(document.actions.size(), semantic_2d_character_rig_max_actions, "/actions");
    bounded(document.directions.size(), semantic_2d_character_rig_max_directions, "/directions");
    unique_ids(document.parts, &Semantic2DCharacterRigPart::id, "/parts", errors);
    unique_ids(document.joints, &Semantic2DCharacterRigJoint::id, "/joints", errors);
    unique_ids(document.material_channels, &Semantic2DCharacterRigMaterialChannel::id, "/materialChannels", errors);
    unique_ids(document.poses, &Semantic2DCharacterRigPose::id, "/poses", errors);
    unique_ids(document.actions, &Semantic2DCharacterRigAction::id, "/actions", errors);
    unique_ids(document.directions, &Semantic2DCharacterRigDirection::id, "/directions", errors);
    std::unordered_set<std::string> part_ids, joint_ids, material_ids, pose_ids, direction_ids;
    for (const auto& value : document.parts) part_ids.insert(value.id);
    for (const auto& value : document.joints) joint_ids.insert(value.id);
    for (const auto& value : document.material_channels) material_ids.insert(value.id);
    for (const auto& value : document.poses) pose_ids.insert(value.id);
    for (const auto& value : document.directions) direction_ids.insert(value.id);
    const auto bounded_text = [&](const std::string& value, const std::string& path, const bool required = false) {
        if ((required && value.empty()) || value.size() > semantic_2d_character_rig_max_string_bytes)
            issue(errors, "semantic-2d-rig.invalid-string-size", path, "Text is empty or exceeds the prototype bound.");
    };
    bounded_text(document.display_name, "/displayName", true);
    for (const auto& joint : document.joints) {
        if (!joint.parent_id.empty() && !joint_ids.contains(joint.parent_id)) issue(errors, "semantic-2d-rig.joint-parent-not-found", "/joints", joint.id);
        if (!std::isfinite(joint.rest_x) || !std::isfinite(joint.rest_y) ||
            !std::isfinite(joint.rest_rotation_degrees)) issue(errors, "semantic-2d-rig.invalid-joint-rest", "/joints", joint.id);
    }
    for (const auto& part : document.parts) {
        bounded_text(part.display_name, "/parts/" + part.id + "/displayName", true);
        bounded_text(part.source_asset, "/parts/" + part.id + "/sourceAsset", true);
        if (!joint_ids.contains(part.joint_id)) issue(errors, "semantic-2d-rig.joint-not-found", "/parts", part.id);
        if (!material_ids.contains(part.material_channel_id)) issue(errors, "semantic-2d-rig.material-not-found", "/parts", part.id);
        if (part.source_asset.empty()) issue(errors, "semantic-2d-rig.source-asset-empty", "/parts", part.id);
        if (!std::isfinite(part.pivot_x) || !std::isfinite(part.pivot_y)) issue(errors, "semantic-2d-rig.invalid-part-pivot", "/parts", part.id);
    }
    for (const auto& material : document.material_channels) {
        bounded_text(material.semantic, "/materialChannels/" + material.id + "/semantic", true);
        bounded_text(material.source_asset, "/materialChannels/" + material.id + "/sourceAsset");
        bounded_text(material.texture_asset, "/materialChannels/" + material.id + "/textureAsset");
        if ((material.source_asset.empty() && material.texture_asset.empty()) ||
            !std::isfinite(material.intensity) || material.intensity < 0.0F || material.intensity > 100.0F ||
            (material.color_space != "srgb" && material.color_space != "linear"))
            issue(errors, "semantic-2d-rig.invalid-material-channel", "/materialChannels", material.id);
    }
    for (const auto& pose : document.poses) {
        if (!direction_ids.contains(pose.direction_id)) issue(errors, "semantic-2d-rig.direction-not-found", "/poses", pose.id);
        if (pose.part_transforms.empty() || pose.part_transforms.size() > semantic_2d_character_rig_max_pose_part_transforms) issue(errors, "semantic-2d-rig.invalid-transform-count", "/poses", pose.id);
        std::unordered_set<std::string> transformed;
        for (const auto& transform : pose.part_transforms) {
            if (!part_ids.contains(transform.part_id) || !transformed.insert(transform.part_id).second) issue(errors, "semantic-2d-rig.invalid-pose-part", "/poses", transform.part_id);
            if (!transform.material_channel_id.empty() && !material_ids.contains(transform.material_channel_id)) issue(errors, "semantic-2d-rig.material-not-found", "/poses", transform.material_channel_id);
            if (!std::isfinite(transform.x) || !std::isfinite(transform.y) || !std::isfinite(transform.rotation_degrees) || !std::isfinite(transform.scale_x) || !std::isfinite(transform.scale_y) || !std::isfinite(transform.opacity) || transform.scale_x == 0.0F || transform.scale_y == 0.0F || transform.opacity < 0.0F || transform.opacity > 1.0F) issue(errors, "semantic-2d-rig.invalid-transform", "/poses", pose.id);
        }
    }
    for (const auto& action : document.actions) {
        bounded_text(action.display_name, "/actions/" + action.id + "/displayName", true);
        if (!std::isfinite(action.sample_rate) || action.sample_rate <= 0.0F || action.sample_rate > 240.0F || action.frames.empty() || action.frames.size() > semantic_2d_character_rig_max_action_frames) issue(errors, "semantic-2d-rig.invalid-action", "/actions", action.id);
        std::unordered_set<std::string> keys;
        for (const auto& frame : action.frames) if (!id_shape(frame.key) || !keys.insert(frame.key).second || !pose_ids.contains(frame.pose_id) || frame.duration_ms == 0U || frame.duration_ms > 60'000U) issue(errors, "semantic-2d-rig.invalid-action-frame", "/actions", action.id + "/" + frame.key);
    }
    for (const auto& direction : document.directions) {
        bounded_text(direction.display_name, "/directions/" + direction.id + "/displayName", true);
        if (!std::isfinite(direction.angle_degrees)) issue(errors, "semantic-2d-rig.invalid-direction-angle", "/directions", direction.id);
        if ((!direction.mirror_of_direction_id.empty() && !direction_ids.contains(direction.mirror_of_direction_id)) || (!direction.override_of_direction_id.empty() && !direction_ids.contains(direction.override_of_direction_id))) issue(errors, "semantic-2d-rig.direction-reference-not-found", "/directions", direction.id);
        for (const auto& [base, replacement] : direction.pose_overrides) if (!pose_ids.contains(base) || !pose_ids.contains(replacement)) issue(errors, "semantic-2d-rig.pose-override-not-found", "/directions", base + "->" + replacement);
    }
    bounded_text(document.provenance.source_uri, "/provenance/sourceUri", true);
    bounded_text(document.provenance.generator, "/provenance/generator", true);
    bounded_text(document.provenance.license, "/provenance/license", true);
    const bool sha256_shape = document.provenance.source_sha256.size() == 64U &&
        std::ranges::all_of(document.provenance.source_sha256, [](const unsigned char value) {
            return std::isxdigit(value) != 0;
        });
    if (!sha256_shape) issue(errors, "semantic-2d-rig.invalid-provenance-hash", "/provenance/sourceSha256", "Expected a 64-character SHA-256.");
    return errors;
}

std::string Semantic2DCharacterRigCodec::write_canonical_json(
    const Semantic2DCharacterRigDocument& source) {
    auto document = source;
    document.parts = sorted(std::move(document.parts), &Semantic2DCharacterRigPart::id);
    document.joints = sorted(std::move(document.joints), &Semantic2DCharacterRigJoint::id);
    document.material_channels = sorted(std::move(document.material_channels), &Semantic2DCharacterRigMaterialChannel::id);
    document.poses = sorted(std::move(document.poses), &Semantic2DCharacterRigPose::id);
    document.actions = sorted(std::move(document.actions), &Semantic2DCharacterRigAction::id);
    document.directions = sorted(std::move(document.directions), &Semantic2DCharacterRigDirection::id);
    Json result{{"schema", document.schema}, {"characterId", document.character_id}, {"displayName", document.display_name}};
    result["parts"] = Json::array(); for (const auto& value : document.parts) result["parts"].push_back({{"id",value.id},{"displayName",value.display_name},{"jointId",value.joint_id},{"sourceAsset",value.source_asset},{"materialChannelId",value.material_channel_id},{"zOrder",value.z_order},{"pivot",{value.pivot_x,value.pivot_y}},{"visible",value.visible}});
    result["joints"] = Json::array(); for (const auto& value : document.joints) result["joints"].push_back({{"id",value.id},{"parentId",value.parent_id},{"rest",{value.rest_x,value.rest_y,value.rest_rotation_degrees}}});
    result["materialChannels"] = Json::array(); for (const auto& value : document.material_channels) result["materialChannels"].push_back({{"id",value.id},{"semantic",value.semantic},{"sourceAsset",value.source_asset},{"textureAsset",value.texture_asset},{"colorSpace",value.color_space},{"intensity",value.intensity},{"optional",value.optional}});
    result["poses"] = Json::array(); for (auto value : document.poses) { value.part_transforms = sorted(std::move(value.part_transforms), &Semantic2DCharacterRigPosePartTransform::part_id); Json transforms=Json::array(); for(const auto& transform:value.part_transforms) transforms.push_back({{"partId",transform.part_id},{"position",{transform.x,transform.y}},{"rotationDegrees",transform.rotation_degrees},{"scale",{transform.scale_x,transform.scale_y}},{"opacity",transform.opacity},{"visible",transform.visible},{"materialChannelId",transform.material_channel_id},{"celKey",transform.cel_key}}); result["poses"].push_back({{"id",value.id},{"directionId",value.direction_id},{"frameKey",value.frame_key},{"partTransforms",std::move(transforms)}}); }
    result["actions"] = Json::array(); for (auto value : document.actions) { value.frames=sorted(std::move(value.frames),&Semantic2DCharacterRigActionFrame::key); Json frames=Json::array(); for(const auto& frame:value.frames) frames.push_back({{"key",frame.key},{"poseId",frame.pose_id},{"durationMs",frame.duration_ms}}); result["actions"].push_back({{"id",value.id},{"displayName",value.display_name},{"looping",value.looping},{"sampleRate",value.sample_rate},{"frames",std::move(frames)}}); }
    result["directions"] = Json::array(); for (const auto& value : document.directions) result["directions"].push_back({{"id",value.id},{"displayName",value.display_name},{"angleDegrees",value.angle_degrees},{"mirrorOfDirectionId",value.mirror_of_direction_id},{"overrideOfDirectionId",value.override_of_direction_id},{"mirrorX",value.mirror_x},{"poseOverrides",value.pose_overrides}});
    result["provenance"]={{"sourceUri",document.provenance.source_uri},{"sourceSha256",document.provenance.source_sha256},{"generator",document.provenance.generator},{"license",document.provenance.license}};
    return result.dump(2) + "\n";
}

std::vector<std::string> Semantic2DCharacterRigCodec::source_dependencies(
    const Semantic2DCharacterRigDocument& document) {
    std::set<std::string> dependencies;
    for (const auto& part : document.parts) if (!part.source_asset.empty()) dependencies.insert(part.source_asset);
    for (const auto& channel : document.material_channels) {
        if (!channel.source_asset.empty()) dependencies.insert(channel.source_asset);
        if (!channel.texture_asset.empty()) dependencies.insert(channel.texture_asset);
    }
    return {dependencies.begin(), dependencies.end()};
}

} // namespace noemancer
