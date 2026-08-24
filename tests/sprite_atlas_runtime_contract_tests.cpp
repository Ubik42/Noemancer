#include "engine/render_world.hpp"
#include "engine/sprite_atlas_artifact.hpp"
#include "engine/world.hpp"

#include <nlohmann/json.hpp>

#include <array>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

namespace {

std::string sha256_identity(const char digit) {
    return "sha256:" + std::string(64U, digit);
}

noemancer::SpriteAssetDocument make_source_sprite() {
    noemancer::SpriteAssetDocument source;
    source.schema = "noemancer.sprite-asset/0.2";
    source.asset_id = "sprite.runtime.contract";
    source.texture_asset = "texture.runtime.source";
    source.texture_width = 64U;
    source.texture_height = 32U;
    source.pixels_per_unit = 16.0F;
    source.sampling = "nearest";
    source.alpha_mode = "cutout";
    source.frames = {
        noemancer::SpriteFrame{
            .id = "hero.idle.0", .x = 0U, .y = 0U, .width = 16U, .height = 24U,
            .source_width = 16U, .source_height = 24U
        },
        noemancer::SpriteFrame{
            .id = "hero.idle.1", .x = 16U, .y = 0U, .width = 16U, .height = 24U,
            .source_width = 16U, .source_height = 24U
        }
    };
    source.clips = {
        noemancer::SpriteClip{
            .id = "idle", .looping = true,
            .frames = {
                noemancer::SpriteClipFrame{.frame_id = "hero.idle.0", .duration_ms = 100U},
                noemancer::SpriteClipFrame{.frame_id = "hero.idle.1", .duration_ms = 100U}
            }
        }
    };
    source.provenance = noemancer::SpriteProvenance{
        .source_uri = "asset://sprites/runtime-contract.source",
        .source_sha256 = sha256_identity('1'),
        .generator = "sprite-atlas-runtime-contract-test",
        .license = "test-fixture"
    };
    return source;
}

noemancer::SpriteAtlasArtifact make_artifact(const std::string& source_asset_id) {
    noemancer::SpriteAtlasArtifact artifact;
    artifact.valid = true;
    artifact.schema = "noemancer.sprite-atlas-artifact/0.1";
    artifact.code = "ok";
    artifact.detail = "test atlas manifest";
    artifact.source_asset_id = source_asset_id;
    artifact.source_hash = sha256_identity('2');
    artifact.target_profile = "windows-x64-debug";
    artifact.page_width = 32U;
    artifact.page_height = 32U;
    artifact.padding = 1U;
    artifact.layout_fingerprint = 0x1020304050607080ULL;
    artifact.full_page_indices = {0U, 1U};
    artifact.incremental_page_indices = {1U};
    artifact.pages = {
        noemancer::SpriteAtlasPageArtifact{
            .valid = true, .cache_hit = false, .rebuilt = true, .page_index = 0U,
            .asset_id = "texture.runtime.page.0", .width = 32U, .height = 32U,
            .frame_count = 1U, .input_bytes = 1024U,
            .content_fingerprint = sha256_identity('3'), .payload_format = "ktx2",
            .payload_fingerprint = sha256_identity('4'), .payload_bytes = 1024U,
            .cache_key = "cache/runtime/page-0", .code = "ok", .detail = "test page"
        },
        noemancer::SpriteAtlasPageArtifact{
            .valid = true, .cache_hit = false, .rebuilt = true, .page_index = 1U,
            .asset_id = "texture.runtime.page.1", .width = 32U, .height = 32U,
            .frame_count = 1U, .input_bytes = 1024U,
            .content_fingerprint = sha256_identity('5'), .payload_format = "ktx2",
            .payload_fingerprint = sha256_identity('6'), .payload_bytes = 1024U,
            .cache_key = "cache/runtime/page-1", .code = "ok", .detail = "test page"
        }
    };
    artifact.full_page_bytes = 2048U;
    artifact.incremental_page_bytes = 1024U;
    artifact.bindings = {
        noemancer::SpriteAtlasFrameBinding{
            .frame_id = "hero.idle.0", .page_index = 0U,
            .x = 2U, .y = 3U, .width = 16U, .height = 24U
        },
        noemancer::SpriteAtlasFrameBinding{
            .frame_id = "hero.idle.1", .page_index = 1U,
            .x = 4U, .y = 1U, .width = 16U, .height = 24U
        }
    };
    // An empty bundle fingerprint is valid for this hand-authored manifest;
    // page payload identities still flow into the runtime overlay.
    return artifact;
}

const noemancer::RenderSpriteSnapshot* find_sprite(
    const noemancer::RenderWorldSnapshot& snapshot, const std::string& entity_id) {
    for (const auto& sprite : snapshot.sprites)
        if (sprite.entity_id == entity_id) return &sprite;
    return nullptr;
}

int fail(const char* message, const int code) {
    std::cerr << "sprite_atlas_runtime_contract_tests: " << message << '\n';
    return code;
}

} // namespace

int main() {
    using namespace noemancer;
    const auto source = make_source_sprite();
    const auto artifact = make_artifact(source.asset_id);
    if (!validate_sprite_atlas_artifact(artifact).empty())
        return fail("hand-authored atlas artifact failed structural validation", 1);

    // Round-trip the same bounded artifact manifest used by an external Cook
    // or Agent boundary. No image, window, GPU device or KTX decoder is needed
    // for this contract test.
    const auto manifest = sprite_atlas_artifact_json(artifact);
    const auto parsed = parse_sprite_atlas_artifact_json(manifest);
    if (!parsed || !parsed.artifact || parsed.artifact->pages.size() != 2U ||
        parsed.artifact->bindings.size() != 2U ||
        nlohmann::json::parse(manifest).at("pages").at("emitted") != 2U)
        return fail("atlas artifact manifest did not round-trip", 2);
    const auto bindings = sprite_runtime_page_bindings(*parsed.artifact);
    if (bindings.size() != 2U || bindings[0].derived_texture_asset_id != "texture.runtime.page.0" ||
        bindings[0].page_fingerprint != sha256_identity('4') ||
        bindings[1].derived_texture_asset_id != "texture.runtime.page.1")
        return fail("artifact-to-runtime page binding projection lost page identity", 3);

    World world;
    if (!world.register_sprite_asset(source))
        return fail("source SpriteAsset 0.2 was rejected", 4);
    const auto applied = world.register_sprite_page_bindings(source.asset_id, bindings);
    if (!applied.success || applied.code != "ok" || applied.revision != 1U ||
        applied.binding_count != bindings.size())
        return fail("World did not atomically register the atlas overlay", 5);

    auto scene = make_bootstrap_scene_document();
    SceneEntityDocument* sprite_entity = nullptr;
    for (auto& entity : scene.entities) {
        if (entity.guid == "entity.demo-sphere") {
            sprite_entity = &entity;
            break;
        }
    }
    if (sprite_entity == nullptr)
        return fail("bootstrap scene does not expose a stable sprite test entity", 6);
    sprite_entity->mesh_renderer.reset();
    sprite_entity->sprite_renderer = SceneSpriteRenderer{
        .sprite_asset = source.asset_id, .clip = "idle", .playback_speed = 1.0,
        .playing = true, .flip_x = false, .flip_y = false,
        .sorting_layer = "default", .sorting_order = 0, .visible = true
    };
    if (!world.load_scene(scene).success)
        return fail("World failed to load the sprite scene", 7);

    const auto sprite_observation = nlohmann::json::parse(
        world.sprite_observation_json("entity.demo-sphere"));
    if (!sprite_observation.at("items").is_array() ||
        sprite_observation.at("items").size() != 1U)
        return fail("World sprite observation did not contain the scene sprite", 8);
    const auto& playback = sprite_observation.at("items").at(0U).at("playback");
    if (!playback.at("valid").get<bool>() ||
        playback.at("frame").at("id") != "hero.idle.0" ||
        playback.at("frame").at("rect") != nlohmann::json::array({2U, 3U, 16U, 24U}) ||
        playback.at("pageBinding").at("derivedTextureAssetId") != "texture.runtime.page.0" ||
        playback.at("pageBinding").at("pageSize") != nlohmann::json::array({32U, 32U}))
        return fail("World observation did not expose the page-local runtime resolve", 9);

    auto render_snapshot = RenderWorldExtractor::extract(
        world.revision(), 17U, world.entity_views());
    const auto* resolved_sprite = find_sprite(render_snapshot, "entity.demo-sphere");
    if (resolved_sprite == nullptr || resolved_sprite->texture_asset != "texture.runtime.page.0" ||
        resolved_sprite->texture_size != std::array<std::uint32_t, 2>{32U, 32U} ||
        resolved_sprite->pixel_rect != std::array<std::uint32_t, 4>{2U, 3U, 16U, 24U} ||
        std::abs(resolved_sprite->uv_rect[0] - 2.0F / 32.0F) > 1.0e-6F ||
        std::abs(resolved_sprite->uv_rect[1] - 3.0F / 32.0F) > 1.0e-6F ||
        std::abs(resolved_sprite->uv_rect[2] - 18.0F / 32.0F) > 1.0e-6F ||
        std::abs(resolved_sprite->uv_rect[3] - 27.0F / 32.0F) > 1.0e-6F)
        return fail("RenderWorld did not resolve the derived page texture and local rect", 10);
    const auto render_observation = nlohmann::json::parse(render_world_json(render_snapshot));
    const auto& rendered_items = render_observation.at("sprites");
    if (!rendered_items.is_array() || rendered_items.size() != 1U ||
        rendered_items.at(0U).at("textureAsset") != "texture.runtime.page.0" ||
        rendered_items.at(0U).at("pixelRect") != nlohmann::json::array({2U, 3U, 16U, 24U}))
        return fail("RenderWorld JSON dropped the derived page projection", 11);

    // World has no second clear authority: an empty replacement is the same
    // atomic clear operation exposed by SpriteAssetLibrary.
    const auto cleared = world.register_sprite_page_bindings(source.asset_id, {});
    if (!cleared.success || cleared.code != "ok" || cleared.revision != applied.revision + 1U ||
        cleared.binding_count != 0U)
        return fail("World did not clear the page overlay atomically", 12);
    const auto legacy_observation = nlohmann::json::parse(
        world.sprite_observation_json("entity.demo-sphere"));
    const auto& legacy_playback = legacy_observation.at("items").at(0U).at("playback");
    if (legacy_playback.at("pageBinding") != nullptr ||
        legacy_playback.at("frame").at("rect") != nlohmann::json::array({0U, 0U, 16U, 24U}))
        return fail("clearing the overlay did not restore source frame observation", 13);
    render_snapshot = RenderWorldExtractor::extract(world.revision(), 18U, world.entity_views());
    resolved_sprite = find_sprite(render_snapshot, "entity.demo-sphere");
    if (resolved_sprite == nullptr || resolved_sprite->texture_asset != source.texture_asset ||
        resolved_sprite->texture_size != std::array<std::uint32_t, 2>{64U, 32U} ||
        resolved_sprite->pixel_rect != std::array<std::uint32_t, 4>{0U, 0U, 16U, 24U})
        return fail("legacy single-atlas RenderWorld compatibility was not restored", 14);

    std::cout << "sprite_atlas_runtime_contract_tests: ok\n";
    return 0;
}
