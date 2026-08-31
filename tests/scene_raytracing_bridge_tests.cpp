#include "runtime/scene_raytracing_bridge.hpp"

#include <cstdint>
#include <iostream>
#include <string_view>

namespace {

using namespace noemancer;

bool check(const bool condition, const std::string_view message) {
    if (!condition)
        std::cerr << "scene_raytracing_bridge_tests: " << message << '\n';
    return condition;
}

SceneRayTracingGeometryCacheSnapshot ready_snapshot(
    const std::uint64_t topology_revision = 1U,
    const std::uint64_t content_revision = 1U,
    const std::uint64_t topology_fingerprint = 11U,
    const std::uint64_t content_fingerprint = 101U) {
    SceneRayTracingGeometryCacheSnapshot snapshot;
    snapshot.state = SceneRayTracingGeometryCacheState::ready;
    snapshot.scene_id = "bridge.fixture.scene";
    snapshot.topology_revision = topology_revision;
    snapshot.content_revision = content_revision;
    snapshot.topology_fingerprint = topology_fingerprint;
    snapshot.content_fingerprint = content_fingerprint;
    snapshot.accepted = true;
    snapshot.world_triangles.push_back(SceneRayTracingGeometryCacheWorldTriangle{
        .positions = {{{-1.0F, -1.0F, 0.0F},
                       {1.0F, -1.0F, 0.0F},
                       {0.0F, 1.0F, 0.0F}}},
        .instance_id = "instance.main",
        .geometry_id = "mesh.triangle",
        .primitive_id = "primitive.opaque",
        .source_triangle_index = 0U});
    snapshot.statistics.world_triangle_count = 1U;
    return snapshot;
}

bool test_persistent_frames_and_revision_routing() {
    SceneRayTracingBridge bridge;
    const auto first = bridge.update(
        SceneRayTracingBridgeRequest{"vulkan", true, true, true}, ready_snapshot());
    if (!check(first.requested && first.scene_accepted && first.plan_valid &&
                   !first.plan_fingerprint.empty() && first.backend == "vulkan" &&
                   first.cache_state == "ready" && first.triangle_count == 1U &&
                   first.visual_path == "ssgi-raster-fallback" &&
                   first.frame_generation == 1U && first.graph_generation == 1U,
               "initial cache snapshot did not produce a valid persistent plan"))
        return false;

    const auto second = bridge.update(
        "VULKAN", ready_snapshot(), true, true, true);
    if (!check(second.requested && second.scene_accepted && second.plan_valid &&
                   second.backend == "vulkan" && second.frame_generation == 2U &&
                   !second.topology_rebuilt && !second.content_updated &&
                   second.visual_path == "ssgi-raster-fallback" &&
                   (!second.native_trace_ready ||
                    second.fallback_code == "native-output-not-shared"),
               "identical input did not remain on the long-lived bridge path"))
        return false;

    const auto updated = bridge.update(
        SceneRayTracingBridgeRequest{"vulkan", true, true, false},
        ready_snapshot(1U, 2U, 11U, 102U));
    if (!check(updated.requested && updated.scene_accepted && updated.plan_valid &&
                   updated.content_updated && !updated.topology_rebuilt &&
                   updated.frame_generation == 3U &&
                   updated.visual_path == "ssgi-raster-fallback",
               "content revision did not select the update path"))
        return false;

    const auto rebuilt = bridge.execute(
        SceneRayTracingBridgeRequest{"vulkan", true, true, true},
        ready_snapshot(2U, 3U, 12U, 103U));
    const auto observed = bridge.observation();
    return check(rebuilt.requested && rebuilt.scene_accepted && rebuilt.plan_valid &&
                     rebuilt.topology_rebuilt && rebuilt.frame_generation == 4U &&
                     rebuilt.visual_path == "ssgi-raster-fallback" &&
                     observed.plan_fingerprint == rebuilt.plan_fingerprint &&
                     observed.topology_rebuilt,
                 "topology revision did not select the rebuild path");
}

bool test_explicit_fallbacks_and_shutdown() {
    SceneRayTracingBridge bridge;
    const auto snapshot = ready_snapshot();
    const auto unsupported = bridge.execute(
        SceneRayTracingBridgeRequest{"metal", true, true, true}, snapshot);
    if (!check(unsupported.requested && unsupported.scene_accepted &&
                   unsupported.backend == "metal" && unsupported.failed &&
                   unsupported.fallback_code == "bridge.backend-unsupported" &&
                   unsupported.visual_path == "ssgi-raster-fallback",
               "unsupported backend was not projected as an explicit fallback"))
        return false;

    const auto disabled = bridge.execute(
        SceneRayTracingBridgeRequest{"vulkan", false, true, true}, snapshot);
    if (!check(!disabled.requested && disabled.scene_accepted &&
                   disabled.fallback_code == "bridge.disabled" &&
                   disabled.visual_path == "ssgi-raster-fallback" &&
                   !disabled.session_executed,
               "disabled bridge request touched the session or lost fallback evidence"))
        return false;

    auto empty = snapshot;
    empty.state = SceneRayTracingGeometryCacheState::fallback;
    empty.accepted = false;
    empty.world_triangles.clear();
    empty.fallback = SceneRayTracingGeometryCacheFallback{
        true, "no-supported-triangles", "No supported triangles remain."};
    const auto cache_fallback = bridge.execute(
        SceneRayTracingBridgeRequest{"vulkan", true, true, true}, empty);
    if (!check(!cache_fallback.scene_accepted && cache_fallback.failed &&
                   cache_fallback.cache_state == "fallback" &&
                   cache_fallback.fallback_code == "no-supported-triangles" &&
                   cache_fallback.visual_path == "raster-pbr",
               "empty cache fallback was not rejected before backend execution"))
        return false;

    const auto stopped = bridge.shutdown();
    if (!check(stopped.shutdown_completed && stopped.code == "bridge.shutdown-complete",
               "bridge shutdown did not complete"))
        return false;
    const auto stopped_again = bridge.shutdown();
    if (!check(stopped_again.shutdown_completed &&
                   stopped_again.code == "bridge.shutdown-idempotent",
               "bridge shutdown was not idempotent"))
        return false;
    const auto after_shutdown = bridge.execute(
        SceneRayTracingBridgeRequest{"vulkan", true, true, true}, snapshot);
    return check(after_shutdown.failed &&
                     after_shutdown.code == "bridge.already-shutdown",
                 "post-shutdown bridge execution was not rejected safely");
}

} // namespace

int main() {
    if (!test_persistent_frames_and_revision_routing()) return 1;
    if (!test_explicit_fallbacks_and_shutdown()) return 2;
    std::cout << "scene_raytracing_bridge_tests: ok\n";
    return 0;
}
