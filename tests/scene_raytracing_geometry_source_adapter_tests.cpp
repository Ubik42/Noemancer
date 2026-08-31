#include "runtime/scene_raytracing_geometry_source_adapter.hpp"

#include <array>
#include <cstdint>
#include <iostream>
#include <span>
#include <string_view>
#include <vector>

namespace {

using namespace noemancer;

bool check(const bool condition, const std::string_view message) {
    if (!condition) std::cerr << "scene_raytracing_geometry_source_adapter_tests: " << message << '\n';
    return condition;
}

DecodedSceneAsset decoded_fixture() {
    DecodedSceneAsset asset;
    asset.vertices = {
        GltfDecodedVertex{.position = {-1.0F, -1.0F, 0.0F}},
        GltfDecodedVertex{.position = {1.0F, -1.0F, 0.0F}},
        GltfDecodedVertex{.position = {0.0F, 1.0F, 0.0F}},
    };
    asset.indices = {0U, 1U, 2U};

    GltfDecodedPrimitive named;
    named.first_index = 0U;
    named.index_count = 3U;
    named.node_name = "Hero";
    named.mesh_name = "Body";
    named.alpha_mode = "OPAQUE";
    asset.primitives.push_back(named);

    GltfDecodedPrimitive unnamed;
    unnamed.first_index = 0U;
    unnamed.index_count = 3U;
    unnamed.alpha_mode = "MASK";
    asset.primitives.push_back(unnamed);

    GltfDecodedPrimitive duplicate_name;
    duplicate_name.first_index = 0U;
    duplicate_name.index_count = 3U;
    duplicate_name.node_name = "Hero";
    duplicate_name.mesh_name = "Body";
    duplicate_name.alpha_mode = "BLEND";
    duplicate_name.skin = 0;
    asset.primitives.push_back(duplicate_name);
    return asset;
}

bool test_decoded_identity_and_copy() {
    const auto decoded = decoded_fixture();
    const auto first = make_scene_raytracing_geometry_input("asset://hero", decoded);
    const auto second = make_scene_raytracing_geometry_input("asset://hero", decoded);
    if (!check(first.geometry_id == "asset://hero" &&
                   first.source == SceneRayTracingGeometrySourceKind::imported &&
                   first.positions.size() == decoded.vertices.size() &&
                   first.indices == decoded.indices && first.primitives.size() == 3U,
               "decoded asset was not copied into imported geometry input"))
        return false;
    if (!check(first.primitives[0U].primitive_id == "Hero/Body#0" &&
                   first.primitives[1U].primitive_id == "primitive#1" &&
                   first.primitives[2U].primitive_id == "Hero/Body#2" &&
                   first.primitives[0U].primitive_id != first.primitives[2U].primitive_id,
               "primitive identity did not remain stable for empty or repeated names"))
        return false;
    return check(first.primitives[0U].primitive_id == second.primitives[0U].primitive_id &&
                     first.primitives[1U].primitive_id == second.primitives[1U].primitive_id &&
                     first.primitives[2U].primitive_id == second.primitives[2U].primitive_id &&
                     first.positions == second.positions && first.indices == second.indices,
                 "the same decoded asset did not produce identical plain data");
}

bool test_material_and_skin_mapping() {
    const auto geometry = make_scene_raytracing_geometry_input("asset://hero", decoded_fixture());
    return check(geometry.primitives[0U].alpha_mode == SceneRayTracingAlphaMode::opaque &&
                     geometry.primitives[1U].alpha_mode == SceneRayTracingAlphaMode::mask &&
                     geometry.primitives[2U].alpha_mode == SceneRayTracingAlphaMode::blend &&
                     !geometry.primitives[0U].skinned && !geometry.primitives[1U].skinned &&
                     geometry.primitives[2U].skinned,
                 "decoded alpha mode or skin metadata was mapped incorrectly");
}

bool test_builtin_helper() {
    const std::array<std::array<float, 3U>, 3U> positions{{
        {-1.0F, -1.0F, 0.0F}, {1.0F, -1.0F, 0.0F}, {0.0F, 1.0F, 0.0F}}};
    const std::array<std::uint32_t, 3U> indices{0U, 1U, 2U};
    SceneRayTracingPrimitiveInput range;
    range.primitive_id = "builtin.triangle";
    range.first_index = 0U;
    range.index_count = 3U;
    const auto geometry = make_builtin_scene_raytracing_geometry_input(
        "builtin.triangle", std::span<const std::array<float, 3U>>(positions),
        std::span<const std::uint32_t>(indices),
        std::span<const SceneRayTracingPrimitiveInput>(&range, 1U));
    return check(geometry.geometry_id == "builtin.triangle" &&
                     geometry.source == SceneRayTracingGeometrySourceKind::builtin &&
                     geometry.positions.size() == 3U && geometry.indices.size() == 3U &&
                     geometry.primitives.size() == 1U &&
                     geometry.primitives[0U].primitive_id == "builtin.triangle" &&
                     geometry.positions[2U][1U] == 1.0F,
                 "builtin geometry helper did not preserve the supplied spans and ranges");
}

} // namespace

int main() {
    if (!test_decoded_identity_and_copy()) return 1;
    if (!test_material_and_skin_mapping()) return 2;
    if (!test_builtin_helper()) return 3;
    std::cout << "scene_raytracing_geometry_source_adapter_tests: ok\n";
    return 0;
}
