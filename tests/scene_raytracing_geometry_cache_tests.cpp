#include "runtime/scene_raytracing_geometry_cache.hpp"

#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <string_view>
#include <utility>

namespace {

using namespace noemancer;

bool check(const bool condition, const std::string_view message) {
    if (!condition) std::cerr << "scene_raytracing_geometry_cache_tests: " << message << '\n';
    return condition;
}

SceneRayTracingGeometryCacheInput triangle_input() {
    SceneRayTracingGeometryCacheInput input;
    input.scene_id = "cache.fixture";

    SceneRayTracingGeometryInput geometry;
    geometry.geometry_id = "mesh.triangle";
    geometry.source = SceneRayTracingGeometrySourceKind::builtin;
    geometry.positions = {
        {-1.0F, -1.0F, 0.0F},
        {1.0F, -1.0F, 0.0F},
        {0.0F, 1.0F, 0.0F},
    };
    geometry.indices = {0U, 1U, 2U};
    SceneRayTracingPrimitiveInput primitive;
    primitive.primitive_id = "primitive.opaque";
    primitive.first_index = 0U;
    primitive.index_count = 3U;
    geometry.primitives.push_back(std::move(primitive));
    input.geometries.push_back(std::move(geometry));

    SceneRayTracingInstanceInput instance;
    instance.instance_id = "instance.main";
    instance.geometry_id = "mesh.triangle";
    input.instances.push_back(std::move(instance));
    return input;
}

bool test_initial_build_and_stable_reuse() {
    SceneRayTracingGeometryCache cache;
    const auto input = triangle_input();
    const auto first = cache.update(input);
    if (!check(first.accepted && first.state == SceneRayTracingGeometryCacheState::ready &&
                   !first.reused && first.topology_changed && first.content_changed &&
                   first.topology_revision == 1U && first.content_revision == 1U &&
                   first.statistics.world_triangle_count == 1U &&
                   first.statistics.accepted_geometry_count == 1U &&
                   first.statistics.accepted_primitive_count == 1U &&
                   first.statistics.accepted_instance_count == 1U && !first.fallback_active,
               "initial opaque static scene was not accepted"))
        return false;

    const auto& snapshot = cache.snapshot();
    if (!check(snapshot.world_triangles.size() == 1U &&
                   snapshot.primitive_ranges.size() == 1U &&
                   snapshot.world_instances.size() == 1U &&
                   snapshot.world_triangles[0U].instance_id == "instance.main" &&
                   snapshot.world_triangles[0U].geometry_id == "mesh.triangle" &&
                   snapshot.world_triangles[0U].primitive_id == "primitive.opaque" &&
                   snapshot.primitive_ranges[0U].first_triangle == 0U &&
                   snapshot.primitive_ranges[0U].triangle_count == 1U &&
                   snapshot.world_triangles[0U].positions[0U][0U] == -1.0F,
               "world-space triangle or primitive range was not materialized"))
        return false;

    const auto reused = cache.update(input);
    return check(reused.accepted && reused.reused && !reused.topology_changed &&
                     !reused.content_changed && reused.topology_revision == 1U &&
                     reused.content_revision == 1U && reused.code == "cache-reused",
                 "identical source input did not reuse the derived cache");
}

bool test_transform_and_content_revision() {
    SceneRayTracingGeometryCache cache;
    auto input = triangle_input();
    if (!check(cache.update(input).accepted, "revision fixture could not be initialized")) return false;

    input.instances[0U].transform[12U] = 5.0F;
    const auto transform_update = cache.update(input);
    if (!check(transform_update.accepted && transform_update.content_changed &&
                   !transform_update.topology_changed && transform_update.topology_revision == 1U &&
                   transform_update.content_revision == 2U &&
                   cache.snapshot().world_triangles[0U].positions[0U][0U] == 4.0F,
               "instance transform did not update world positions without rebuilding topology"))
        return false;

    input.geometries[0U].positions[0U][0U] = -2.0F;
    const auto geometry_update = cache.update(input);
    if (!check(geometry_update.accepted && geometry_update.content_changed &&
                   !geometry_update.topology_changed && geometry_update.topology_revision == 1U &&
                   geometry_update.content_revision == 3U &&
                   cache.snapshot().world_triangles[0U].positions[0U][0U] == 3.0F,
               "geometry content did not advance only the content revision"))
        return false;

    const auto stable = cache.update(input);
    return check(stable.reused && stable.content_revision == 3U &&
                     stable.topology_revision == 1U,
                 "content snapshot was not stable after an unchanged update");
}

bool test_topology_change_and_imported_source() {
    SceneRayTracingGeometryCache cache;
    auto input = triangle_input();
    input.geometries[0U].source = SceneRayTracingGeometrySourceKind::imported;
    if (!check(cache.update(input).accepted, "imported geometry was not accepted")) return false;

    input.geometries[0U].positions.push_back({0.0F, 0.0F, 1.0F});
    input.geometries[0U].positions.push_back({1.0F, 0.0F, 1.0F});
    input.geometries[0U].positions.push_back({0.0F, 1.0F, 1.0F});
    input.geometries[0U].indices.insert(input.geometries[0U].indices.end(), {3U, 4U, 5U});
    SceneRayTracingPrimitiveInput second;
    second.primitive_id = "primitive.second";
    second.first_index = 3U;
    second.index_count = 3U;
    input.geometries[0U].primitives.push_back(std::move(second));
    const auto topology = cache.update(input);
    return check(topology.accepted && topology.topology_changed && topology.content_changed &&
                     topology.topology_revision == 2U && topology.content_revision == 2U &&
                     topology.statistics.world_triangle_count == 2U &&
                     topology.statistics.accepted_primitive_count == 2U &&
                     cache.snapshot().primitive_ranges[1U].primitive_id == "primitive.second",
                 "primitive topology change did not rebuild the bounded scene");
}

bool test_exclusion_and_fallback() {
    SceneRayTracingGeometryCache cache;
    auto input = triangle_input();
    SceneRayTracingPrimitiveInput masked;
    masked.primitive_id = "primitive.masked";
    masked.index_count = 3U;
    masked.alpha_mode = SceneRayTracingAlphaMode::mask;
    input.geometries[0U].primitives.push_back(std::move(masked));
    SceneRayTracingPrimitiveInput skinned;
    skinned.primitive_id = "primitive.skinned";
    skinned.index_count = 3U;
    skinned.skinned = true;
    input.geometries[0U].primitives.push_back(std::move(skinned));

    const auto partial = cache.update(input);
    if (!check(partial.accepted && partial.state == SceneRayTracingGeometryCacheState::ready &&
                   partial.fallback_active && partial.statistics.world_triangle_count == 1U &&
                   partial.statistics.excluded_unsupported_primitive_count == 2U &&
                   cache.snapshot().fallback.code == "partial-scene-fallback",
               "unsupported MASK/skinned primitives were not explicitly excluded"))
        return false;

    input.geometries[0U].primitives[1U].enabled = false;
    input.geometries[0U].primitives[2U].enabled = false;
    const auto disabled = cache.update(input);
    if (!check(disabled.accepted && disabled.state == SceneRayTracingGeometryCacheState::ready &&
                   !disabled.fallback_active && disabled.statistics.world_triangle_count == 1U &&
                   disabled.statistics.excluded_disabled_primitive_count == 2U,
               "disabled primitives were not excluded without forcing a fallback"))
        return false;

    input.instances[0U].enabled = false;
    const auto no_instance = cache.update(input);
    return check(no_instance.accepted && no_instance.state == SceneRayTracingGeometryCacheState::fallback &&
                     no_instance.fallback_active && no_instance.statistics.world_triangle_count == 0U,
                 "an empty enabled instance set did not select the explicit fallback state");
}

bool test_invalid_input_and_transactional_rejection() {
    SceneRayTracingGeometryCache cache;
    auto input = triangle_input();
    if (!check(cache.update(input).accepted, "invalid-input fixture could not be initialized")) return false;
    const auto prior_fingerprint = cache.snapshot().content_fingerprint;

    input.geometries[0U].indices[2U] = 99U;
    const auto invalid_index = cache.update(input);
    if (!check(!invalid_index.accepted && invalid_index.state == SceneRayTracingGeometryCacheState::rejected &&
                   invalid_index.code == "index-out-of-bounds" &&
                   cache.snapshot().content_fingerprint == prior_fingerprint,
               "invalid index was not rejected transactionally"))
        return false;

    input = triangle_input();
    input.instances[0U].transform[0U] = std::numeric_limits<float>::quiet_NaN();
    const auto invalid_transform = cache.update(input);
    return check(!invalid_transform.accepted && invalid_transform.code == "instance-nonfinite-transform" &&
                     cache.snapshot().content_fingerprint == prior_fingerprint,
                 "non-finite instance transform was not rejected transactionally");
}

bool test_triangle_budget() {
    SceneRayTracingGeometryCacheLimits limits;
    limits.max_triangles = 1U;
    SceneRayTracingGeometryCache cache(limits);
    auto input = triangle_input();
    input.geometries[0U].positions.insert(input.geometries[0U].positions.end(), {
        {0.0F, 0.0F, 1.0F}, {1.0F, 0.0F, 1.0F}, {0.0F, 1.0F, 1.0F}});
    input.geometries[0U].indices = {0U, 1U, 2U, 3U, 4U, 5U};
    input.geometries[0U].primitives[0U].index_count = 6U;
    const auto over_budget = cache.update(input);
    if (!check(!over_budget.accepted && over_budget.state == SceneRayTracingGeometryCacheState::rejected &&
                   over_budget.code == "triangle-budget" && cache.snapshot().world_triangles.empty(),
               "configured triangle budget did not fail closed"))
        return false;

    // Exercise the hard 65536-triangle ceiling independently of a caller's
    // smaller budget.  The cache rejects before allocating world triangles.
    SceneRayTracingGeometryCache hard_limit_cache;
    auto hard_limit_input = triangle_input();
    auto& hard_geometry = hard_limit_input.geometries[0U];
    hard_geometry.positions.clear();
    hard_geometry.indices.clear();
    constexpr std::uint32_t triangle_count = 65537U;
    hard_geometry.positions.reserve(static_cast<std::size_t>(triangle_count) * 3U);
    hard_geometry.indices.reserve(static_cast<std::size_t>(triangle_count) * 3U);
    for (std::uint32_t triangle = 0U; triangle < triangle_count; ++triangle) {
        const auto base = triangle * 3U;
        hard_geometry.positions.push_back({0.0F, 0.0F, static_cast<float>(triangle)});
        hard_geometry.positions.push_back({1.0F, 0.0F, static_cast<float>(triangle)});
        hard_geometry.positions.push_back({0.0F, 1.0F, static_cast<float>(triangle)});
        hard_geometry.indices.insert(hard_geometry.indices.end(), {base, base + 1U, base + 2U});
    }
    hard_geometry.primitives[0U].index_count =
        static_cast<std::uint32_t>(hard_geometry.indices.size());
    const auto hard_rejected = hard_limit_cache.update(hard_limit_input);
    return check(!hard_rejected.accepted && hard_rejected.code == "triangle-budget" &&
                     hard_limit_cache.snapshot().world_triangles.empty(),
                 "the hard 65536-triangle ceiling did not fail closed");
}

} // namespace

int main() {
    if (!test_initial_build_and_stable_reuse()) return 1;
    if (!test_transform_and_content_revision()) return 2;
    if (!test_topology_change_and_imported_source()) return 3;
    if (!test_exclusion_and_fallback()) return 4;
    if (!test_invalid_input_and_transactional_rejection()) return 5;
    if (!test_triangle_budget()) return 6;
    std::cout << "scene_raytracing_geometry_cache_tests: ok\n";
    return 0;
}
