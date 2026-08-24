#include "engine/asset_cook_pipeline.hpp"

#include <nlohmann/json.hpp>

#include <array>
#include <cstddef>
#include <cstring>
#include <iostream>
#include <span>
#include <string>
#include <vector>

int main() {
    using namespace noemancer;
    const auto profile = cook_platform_profile("windows-x64-debug");
    std::string code;
    std::string detail;
    if (!validate_cook_platform_profile(profile, code, detail) || code != "ok") {
        std::cerr << "Known Windows Cook profile was rejected: " << detail << '\n';
        return 1;
    }
    const CookSource source{
        .asset_id = "texture.test.albedo",
        .source_uri = "asset://test/albedo.png",
        .source_hash = "sha256:fixture-albedo",
        .source_bytes = 128U,
        .importer = "image/fixture"
    };
    TextureCookSettings settings;
    settings.semantic = TextureSemantic::base_color;
    settings.alpha_mode = TextureAlphaMode::blend;
    const auto texture = plan_texture_cook(source, profile, settings);
    const auto texture_repeat = plan_texture_cook(source, profile, settings);
    if (!texture.valid || texture.code != "ok" || texture.payload_format != "ktx2" ||
        texture.payload_target != "bc7-rgba-srgb" || texture.color_space != "srgb" ||
        texture.stream_policy != "mip-tail-pages" || texture.cache_key != texture_repeat.cache_key ||
        texture.artifact_uri != texture_repeat.artifact_uri) {
        std::cerr << "Texture Cook contract was not deterministic or semantic-aware.\n";
        return 2;
    }
    const auto texture_json = nlohmann::json::parse(cook_artifact_json(texture));
    if (texture_json.at("schema") != "noemancer.cook-artifact/0.1" ||
        texture_json.at("payload").at("format") != "ktx2" ||
        texture_json.at("source").at("hash") != source.source_hash ||
        texture_json.at("dependencies").empty()) {
        std::cerr << "Texture Cook JSON did not expose the stable observation contract.\n";
        return 3;
    }

    CookSource mesh_source = source;
    mesh_source.asset_id = "mesh.test.courier";
    mesh_source.source_uri = "asset://test/courier.glb";
    mesh_source.importer = "gltf.binary/0.1";
    MeshCookSettings mesh_settings;
    mesh_settings.lod_ratios = {1.0F, 0.5F, 0.25F};
    const auto mesh = plan_mesh_cook(mesh_source, profile, mesh_settings);
    if (!mesh.valid || mesh.payload_format != "meshopt/meshbin" ||
        mesh.payload_target != "meshopt-index-vertex-fetch" || mesh.lod_ratios.size() != 3U ||
        mesh.stream_policy != "lod-pages") {
        std::cerr << "Mesh Cook contract did not expose meshoptimizer and LOD policy.\n";
        return 4;
    }

    struct Vertex final {
        float position[3];
        float normal[3];
        float uv[2];
    };
    // A small grid with one duplicated position/attribute vertex exercises the
    // remap pass while retaining enough triangles for a meaningful LOD.
    const std::array<Vertex, 10> vertices{{
        {{-1.0F, -1.0F, 0.0F}, {0.0F, 0.0F, 1.0F}, {0.0F, 0.0F}},
        {{ 0.0F, -1.0F, 0.0F}, {0.0F, 0.0F, 1.0F}, {0.5F, 0.0F}},
        {{ 1.0F, -1.0F, 0.0F}, {0.0F, 0.0F, 1.0F}, {1.0F, 0.0F}},
        {{-1.0F,  0.0F, 0.0F}, {0.0F, 0.0F, 1.0F}, {0.0F, 0.5F}},
        {{ 0.0F,  0.0F, 0.0F}, {0.0F, 0.0F, 1.0F}, {0.5F, 0.5F}},
        {{ 1.0F,  0.0F, 0.0F}, {0.0F, 0.0F, 1.0F}, {1.0F, 0.5F}},
        {{-1.0F,  1.0F, 0.0F}, {0.0F, 0.0F, 1.0F}, {0.0F, 1.0F}},
        {{ 0.0F,  1.0F, 0.0F}, {0.0F, 0.0F, 1.0F}, {0.5F, 1.0F}},
        {{ 1.0F,  1.0F, 0.0F}, {0.0F, 0.0F, 1.0F}, {1.0F, 1.0F}},
        {{ 0.0F, -1.0F, 0.0F}, {0.0F, 0.0F, 1.0F}, {0.5F, 0.0F}}
    }};
    const std::vector<std::uint32_t> grid_indices{
        0U, 1U, 4U, 0U, 4U, 3U,
        1U, 2U, 5U, 1U, 5U, 4U,
        3U, 4U, 7U, 3U, 7U, 6U,
        4U, 5U, 8U, 4U, 8U, 7U,
        9U, 4U, 1U // duplicate vertex 9 should remap to vertex 1
    };
    CookMeshInput mesh_input{
        .vertices = std::vector<std::byte>(
            std::as_bytes(std::span<const Vertex>(vertices.data(), vertices.size())).begin(),
            std::as_bytes(std::span<const Vertex>(vertices.data(), vertices.size())).end()),
        .vertex_stride = sizeof(Vertex),
        .position_offset = 0U,
        .indices = grid_indices,
        .vertex_layout = "position:float3@0;normal:float3@12;uv:float2@24"
    };
    const auto cooked_mesh = execute_mesh_cook(source, mesh_input, profile, mesh_settings);
    if (!meshoptimizer_available()) {
        if (cooked_mesh.valid || cooked_mesh.code != "asset.meshopt-unavailable") {
            std::cerr << "Missing meshoptimizer dependency did not produce an explicit error.\n";
            return 8;
        }
        std::cout << "asset_cook_pipeline_tests: meshoptimizer unavailable; executor skipped\n";
    } else {
        const auto cooked_mesh_repeat = execute_mesh_cook(source, mesh_input, profile, mesh_settings);
        if (!cooked_mesh.valid || cooked_mesh.code != "ok" || cooked_mesh.payload.empty() ||
            cooked_mesh.lods.size() != mesh_settings.lod_ratios.size() ||
            cooked_mesh.source_vertex_count != vertices.size() ||
            cooked_mesh.lods.front().vertex_count >= vertices.size() ||
            cooked_mesh.lods.front().index_count != grid_indices.size() ||
            cooked_mesh.quantization_applied ||
            cooked_mesh.payload != cooked_mesh_repeat.payload ||
            cooked_mesh.payload_fingerprint != cooked_mesh_repeat.payload_fingerprint ||
            cooked_mesh.cache_key != cooked_mesh_repeat.cache_key) {
            std::cerr << "meshoptimizer Cook did not produce deterministic remap/LOD output.\n";
            return 9;
        }
        const auto cooked_json = nlohmann::json::parse(mesh_cook_product_json(cooked_mesh));
        if (cooked_json.at("meshoptimizer").at("available") != true ||
            cooked_json.at("payload").at("format") != "meshopt/meshbin" ||
            cooked_json.at("lods").size() != mesh_settings.lod_ratios.size() ||
            cooked_json.at("payload").at("fingerprint").get<std::string>().empty()) {
            std::cerr << "meshoptimizer Cook metadata did not expose runtime evidence.\n";
            return 10;
        }
        auto invalid_index_input = mesh_input;
        invalid_index_input.indices.front() = 999U;
        const auto invalid_index = execute_mesh_cook(source, invalid_index_input, profile, mesh_settings);
        if (invalid_index.valid || invalid_index.code != "asset.mesh-input-index-range") {
            std::cerr << "Out-of-range mesh index was accepted.\n";
            return 11;
        }
        auto invalid_stride_input = mesh_input;
        invalid_stride_input.vertex_stride = 12U;
        const auto invalid_stride = execute_mesh_cook(source, invalid_stride_input, profile, mesh_settings);
        if (invalid_stride.valid || invalid_stride.code != "asset.mesh-input-vertices") {
            std::cerr << "Malformed mesh vertex stride was accepted.\n";
            return 12;
        }
    }

    MeshCookSettings invalid_mesh;
    invalid_mesh.lod_ratios = {1.0F, 0.75F, 0.8F};
    const auto invalid_mesh_plan = plan_mesh_cook(mesh_source, profile, invalid_mesh);
    if (invalid_mesh_plan.valid || invalid_mesh_plan.code != "asset.mesh-settings-invalid") {
        std::cerr << "Invalid mesh LOD ordering was accepted.\n";
        return 13;
    }
    const auto invalid_hdr = plan_texture_cook(source, profile, TextureCookSettings{
        .semantic = TextureSemantic::hdr,
        .alpha_mode = TextureAlphaMode::opaque,
        .srgb = true
    });
    if (invalid_hdr.valid || invalid_hdr.code != "asset.texture-color-space-invalid") {
        std::cerr << "HDR sRGB contract was accepted.\n";
        return 14;
    }
    const auto mobile = cook_platform_profile("android-arm64-release");
    const auto mobile_texture = plan_texture_cook(source, mobile);
    if (!mobile_texture.valid || mobile_texture.payload_target != "astc-6x6-rgba-srgb") {
        std::cerr << "Android profile did not select ASTC.\n";
        return 15;
    }
    std::cout << "asset_cook_pipeline_tests: ok\n";
    return 0;
}
