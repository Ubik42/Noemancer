#include "engine/semantic_2d_character_bake.hpp"

#include "engine/content_hash.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <nlohmann/json.hpp>
#include <span>
#include <unordered_map>

namespace noemancer {
namespace {

using Json = nlohmann::json;

std::string hash_text(const std::string_view text) {
    const auto bytes = std::span<const std::byte>(
        reinterpret_cast<const std::byte*>(text.data()), text.size());
    const auto result = sha256_bytes(bytes);
    return result.success ? result.value : std::string{};
}

Json draw_json(const Semantic2DBakePartDraw& draw) {
    return {{"partId", draw.part_id}, {"jointId", draw.joint_id},
        {"sourceAsset", draw.source_asset}, {"materialChannelId", draw.material_channel_id},
        {"celKey", draw.cel_key}, {"zOrder", draw.z_order}, {"x", draw.x}, {"y", draw.y},
        {"rotationDegrees", draw.rotation_degrees}, {"scale", {draw.scale_x, draw.scale_y}},
        {"opacity", draw.opacity}, {"visible", draw.visible}};
}

Json frame_json(const Semantic2DBakeFrame& frame, const bool include_fingerprint) {
    Json draws = Json::array();
    for (const auto& draw : frame.draws) draws.push_back(draw_json(draw));
    Json result{{"id", frame.id}, {"actionId", frame.action_id},
        {"directionId", frame.direction_id}, {"frameKey", frame.frame_key},
        {"poseId", frame.pose_id}, {"durationMs", frame.duration_ms},
        {"mirrored", frame.mirrored}, {"draws", std::move(draws)}};
    if (include_fingerprint) result["contentFingerprint"] = frame.content_fingerprint;
    return result;
}

const Semantic2DCharacterRigPose* find_pose(
    const std::unordered_map<std::string, const Semantic2DCharacterRigPose*>& poses,
    const Semantic2DCharacterRigDirection& direction, const std::string& base_pose_id) {
    auto pose_id = base_pose_id;
    if (const auto override = direction.pose_overrides.find(base_pose_id);
        override != direction.pose_overrides.end()) {
        pose_id = override->second;
    }
    const auto found = poses.find(pose_id);
    return found == poses.end() ? nullptr : found->second;
}

} // namespace

Semantic2DBakePlan Semantic2DCharacterBakePrototype::plan(
    const Semantic2DCharacterRigDocument& rig, const Semantic2DBakeSettings& settings) {
    Semantic2DBakePlan result;
    result.character_id = rig.character_id;
    const auto rig_errors = Semantic2DCharacterRigCodec::validate(rig);
    if (!rig_errors.empty()) {
        result.code = "semantic-2d-bake.invalid-rig";
        result.detail = rig_errors.front().code + " at " + rig_errors.front().path;
        return result;
    }
    if (settings.sprite_asset_id.empty() || settings.texture_asset_id.empty() ||
        settings.atlas_width == 0U || settings.atlas_height == 0U ||
        settings.frame_width == 0U || settings.frame_height == 0U ||
        settings.frame_width > settings.atlas_width || settings.frame_height > settings.atlas_height ||
        !std::isfinite(settings.pixels_per_unit) || settings.pixels_per_unit <= 0.0F) {
        result.code = "semantic-2d-bake.invalid-settings";
        result.detail = "Bake settings require stable asset IDs, bounded dimensions and positive pixels-per-unit.";
        return result;
    }

    std::unordered_map<std::string, const Semantic2DCharacterRigPart*> parts;
    std::unordered_map<std::string, const Semantic2DCharacterRigPose*> poses;
    for (const auto& part : rig.parts) parts.emplace(part.id, &part);
    for (const auto& pose : rig.poses) poses.emplace(pose.id, &pose);
    std::vector<const Semantic2DCharacterRigAction*> actions;
    std::vector<const Semantic2DCharacterRigDirection*> directions;
    for (const auto& action : rig.actions) actions.push_back(&action);
    for (const auto& direction : rig.directions) directions.push_back(&direction);
    std::ranges::sort(actions, {}, &Semantic2DCharacterRigAction::id);
    std::ranges::sort(directions, {}, &Semantic2DCharacterRigDirection::id);

    struct PendingFrame final { Semantic2DBakeFrame frame; bool looping{}; };
    std::vector<PendingFrame> pending;
    for (const auto* action : actions) {
        for (const auto* direction : directions) {
            for (const auto& action_frame : action->frames) {
                const auto* pose = find_pose(poses, *direction, action_frame.pose_id);
                if (pose == nullptr) {
                    result.code = "semantic-2d-bake.pose-not-found";
                    result.detail = action->id + "/" + direction->id + "/" + action_frame.pose_id;
                    return result;
                }
                Semantic2DBakeFrame frame;
                frame.id = "frame." + rig.character_id + "." + action->id + "." +
                    direction->id + "." + action_frame.key;
                frame.action_id = action->id;
                frame.direction_id = direction->id;
                frame.frame_key = action_frame.key;
                frame.pose_id = pose->id;
                frame.duration_ms = action_frame.duration_ms;
                frame.mirrored = direction->mirror_x;
                for (const auto& transform : pose->part_transforms) {
                    const auto part = parts.find(transform.part_id);
                    if (part == parts.end()) {
                        result.code = "semantic-2d-bake.part-not-found";
                        result.detail = pose->id + "/" + transform.part_id;
                        return result;
                    }
                    Semantic2DBakePartDraw draw;
                    draw.part_id = part->second->id;
                    draw.joint_id = part->second->joint_id;
                    draw.source_asset = part->second->source_asset;
                    draw.material_channel_id = transform.material_channel_id.empty()
                        ? part->second->material_channel_id : transform.material_channel_id;
                    draw.cel_key = transform.cel_key;
                    draw.z_order = part->second->z_order;
                    draw.x = direction->mirror_x ? -transform.x : transform.x;
                    draw.y = transform.y;
                    draw.rotation_degrees = direction->mirror_x
                        ? -transform.rotation_degrees : transform.rotation_degrees;
                    draw.scale_x = transform.scale_x;
                    draw.scale_y = transform.scale_y;
                    draw.opacity = transform.opacity;
                    draw.visible = part->second->visible && transform.visible;
                    frame.draws.push_back(std::move(draw));
                }
                std::ranges::sort(frame.draws, [](const auto& left, const auto& right) {
                    if (left.z_order != right.z_order) return left.z_order < right.z_order;
                    return left.part_id < right.part_id;
                });
                frame.content_fingerprint = hash_text(frame_json(frame, false).dump());
                pending.push_back({std::move(frame), action->looping});
            }
        }
    }
    std::ranges::sort(pending, [](const auto& left, const auto& right) {
        return left.frame.id < right.frame.id;
    });
    const auto columns = settings.atlas_width / settings.frame_width;
    const auto rows = settings.atlas_height / settings.frame_height;
    if (columns == 0U || rows == 0U || pending.size() > static_cast<std::size_t>(columns) * rows) {
        result.code = "semantic-2d-bake.atlas-capacity-exceeded";
        result.detail = "Atlas cannot hold every deterministic action/direction frame.";
        return result;
    }

    result.sprite.schema = "noemancer.sprite-asset/0.2";
    result.sprite.asset_id = settings.sprite_asset_id;
    result.sprite.texture_asset = settings.texture_asset_id;
    result.sprite.texture_width = settings.atlas_width;
    result.sprite.texture_height = settings.atlas_height;
    result.sprite.pixels_per_unit = settings.pixels_per_unit;
    result.sprite.sampling = "nearest";
    result.sprite.alpha_mode = "cutout";
    result.sprite.provenance = {rig.provenance.source_uri, rig.provenance.source_sha256,
        "Noemancer Semantic 2D Character Bake prototype", rig.provenance.license};
    std::map<std::pair<std::string, std::string>, SpriteClip> clips;
    std::size_t direct_authoring_bytes{};
    for (std::size_t index = 0; index < pending.size(); ++index) {
        auto& item = pending[index];
        const auto x = static_cast<std::uint32_t>(index % columns) * settings.frame_width;
        const auto y = static_cast<std::uint32_t>(index / columns) * settings.frame_height;
        result.sprite.frames.push_back({item.frame.id, x, y, settings.frame_width,
            settings.frame_height, 0U, 0U, settings.frame_width, settings.frame_height,
            0.5F, 0.0F, {}});
        const auto clip_key = std::pair(item.frame.action_id, item.frame.direction_id);
        auto [clip, inserted] = clips.try_emplace(clip_key);
        if (inserted) {
            clip->second.id = item.frame.action_id + "." + item.frame.direction_id;
            clip->second.looping = item.looping;
        }
        clip->second.frames.push_back({item.frame.id, item.frame.duration_ms, {}});
        direct_authoring_bytes += frame_json(item.frame, false).dump().size();
        result.frames.push_back(std::move(item.frame));
    }
    for (auto& [key, clip] : clips) {
        static_cast<void>(key);
        std::ranges::sort(clip.frames, [](const auto& left, const auto& right) {
            return left.frame_id < right.frame_id;
        });
        result.sprite.clips.push_back(std::move(clip));
    }
    const auto sprite_errors = SpriteAssetCodec::validate(result.sprite);
    if (!sprite_errors.empty()) {
        result.code = "semantic-2d-bake.invalid-sprite-adapter";
        result.detail = sprite_errors.front().code + " at " + sprite_errors.front().path;
        return result;
    }
    result.source_dependencies = Semantic2DCharacterRigCodec::source_dependencies(rig);
    result.registry_dependencies = SpriteAssetCodec::asset_dependencies(result.sprite);
    const auto canonical_rig = Semantic2DCharacterRigCodec::write_canonical_json(rig);
    result.source_fingerprint = hash_text(canonical_rig);
    result.metrics.rig_source_bytes = canonical_rig.size();
    result.metrics.direct_frame_authoring_bytes = direct_authoring_bytes;
    result.metrics.sprite_document_bytes = SpriteAssetCodec::write_canonical_json(result.sprite).size();
    result.metrics.package_metadata_bytes = result.metrics.sprite_document_bytes;
    result.valid = true;
    result.code = "semantic-2d-bake.planned";
    result.detail = "Prototype composed stable semantic parts into existing Sprite clips; no second animation runtime.";
    auto observation = Json::parse(write_observation_json(result));
    observation.erase("planFingerprint");
    result.metrics.bake_manifest_bytes = observation.dump().size();
    observation["metrics"]["bakeManifestBytes"] = result.metrics.bake_manifest_bytes;
    result.plan_fingerprint = hash_text(observation.dump());
    return result;
}

Semantic2DBakeEditComparison Semantic2DCharacterBakePrototype::compare(
    const Semantic2DBakePlan& before, const Semantic2DBakePlan& after) {
    Semantic2DBakeEditComparison result;
    result.before_frames = before.frames.size();
    result.after_frames = after.frames.size();
    if (!before.valid || !after.valid || before.character_id != after.character_id) {
        result.code = "semantic-2d-bake.incomparable";
        return result;
    }
    std::map<std::string, std::string> before_frames;
    for (const auto& frame : before.frames) before_frames.emplace(frame.id, frame.content_fingerprint);
    for (const auto& frame : after.frames) {
        const auto found = before_frames.find(frame.id);
        if (found != before_frames.end()) {
            ++result.unchanged_frame_ids;
            if (found->second != frame.content_fingerprint) {
                result.affected_frame_ids.push_back(frame.id);
            }
        } else {
            result.affected_frame_ids.push_back(frame.id);
        }
    }
    result.affected_frames = result.affected_frame_ids.size();
    result.comparable = true;
    result.code = "semantic-2d-bake.compared";
    return result;
}

std::string Semantic2DCharacterBakePrototype::write_observation_json(
    const Semantic2DBakePlan& plan) {
    Json frames = Json::array();
    for (const auto& frame : plan.frames) frames.push_back(frame_json(frame, true));
    return Json{{"schema", plan.schema}, {"valid", plan.valid}, {"code", plan.code},
        {"detail", plan.detail}, {"characterId", plan.character_id},
        {"sourceFingerprint", plan.source_fingerprint}, {"planFingerprint", plan.plan_fingerprint},
        {"spriteAssetId", plan.sprite.asset_id}, {"spriteClipCount", plan.sprite.clips.size()},
        {"frames", std::move(frames)}, {"sourceDependencies", plan.source_dependencies},
        {"registryDependencies", plan.registry_dependencies},
        {"metrics", {{"rigSourceBytes", plan.metrics.rig_source_bytes},
            {"directFrameAuthoringBytes", plan.metrics.direct_frame_authoring_bytes},
            {"spriteDocumentBytes", plan.metrics.sprite_document_bytes},
            {"bakeManifestBytes", plan.metrics.bake_manifest_bytes},
            {"packageMetadataBytes", plan.metrics.package_metadata_bytes}}}}.dump();
}

} // namespace noemancer
