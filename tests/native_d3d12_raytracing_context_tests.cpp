#include "runtime/native_d3d12_raytracing_context.hpp"

#include <cstdint>
#include <iostream>
#include <string>

namespace {

using noemancer::NativeD3D12RayTracingContext;
using noemancer::NativeD3D12RayTracingContextFailureStage;
using noemancer::NativeD3D12RayTracingScene;
using noemancer::NativeD3D12RayTracingGeometry;
using noemancer::NativeD3D12RayTracingContextState;

NativeD3D12RayTracingScene triangle_scene(const std::uint64_t revision = 9U) {
    NativeD3D12RayTracingScene scene;
    scene.scene_id = "context.fixture";
    scene.revision = revision;
    scene.geometries.push_back(NativeD3D12RayTracingGeometry{
        .geometry_id = "triangle.main",
        .position_xyz = {-1.0F, -1.0F, 0.0F,
                          1.0F, -1.0F, 0.0F,
                          0.0F,  1.0F, 0.0F},
        .indices = {0U, 1U, 2U},
        .allow_update = true});
    return scene;
}

int fail(const char* message, const int code) {
    std::cerr << "native_d3d12_raytracing_context_tests: " << message << '\n';
    return code;
}

bool bounded_receipt(const noemancer::NativeD3D12RayTracingContextReceipt& receipt) {
    return receipt.schema == noemancer::native_d3d12_raytracing_context_schema &&
        receipt.backend == "d3d12" && !receipt.native_handle_exposed &&
        receipt.code.size() <= noemancer::native_d3d12_raytracing_context_max_text_bytes &&
        receipt.detail.size() <= noemancer::native_d3d12_raytracing_context_max_text_bytes;
}

} // namespace

int main() {
    using namespace noemancer;

    NativeD3D12RayTracingContext context;
    const auto initial = context.status();
    if (!bounded_receipt(initial) || initial.state != NativeD3D12RayTracingContextState::uninitialized ||
        initial.generation != 0U || context.is_shutdown()) {
        return fail("fresh context did not expose an empty lifecycle state", 1);
    }

    NativeD3D12RayTracingScene invalid;
    invalid.scene_id = "invalid";
    const auto invalid_scene = context.ensure_scene(invalid);
    if (!bounded_receipt(invalid_scene) || invalid_scene.state != NativeD3D12RayTracingContextState::failed ||
        invalid_scene.failure_stage != NativeD3D12RayTracingContextFailureStage::scene ||
        invalid_scene.code != "native-d3d12.context.scene-empty") {
        return fail("invalid scene input did not produce a bounded scene diagnostic", 2);
    }

    const auto initialized = context.initialize();
    if (!bounded_receipt(initialized) || initialized.generation > 1U) {
        return fail("initialization receipt was not bounded or generation was unstable", 3);
    }
    const auto generation = context.generation();
    const auto initialized_again = context.initialize();
    if (!bounded_receipt(initialized_again) || context.generation() != generation ||
        initialized_again.code != "native-d3d12.context.already-initialized" &&
            initialized_again.code != "native-d3d12.context.platform-unavailable" &&
            initialized_again.code != "native-d3d12.context.loader-unavailable" &&
            initialized_again.code != "native-d3d12.context.hardware-unsupported") {
        return fail("second initialization was not idempotent", 4);
    }

    const auto attached = context.ensure_scene(triangle_scene());
    if (!bounded_receipt(attached) || !attached.scene_received || !attached.scene_changed ||
        attached.scene_generation != 1U || attached.geometry_count != 1U ||
        attached.instance_count != 1U || attached.scene_revision != 9U) {
        return fail("valid scene was not retained with a stable scene generation", 5);
    }
    const auto attached_again = context.ensure_scene(triangle_scene());
    if (!bounded_receipt(attached_again) || attached_again.scene_changed ||
        attached_again.scene_generation != attached.scene_generation ||
        context.scene_generation() != 1U) {
        return fail("unchanged scene was not recognized by its stable fingerprint", 6);
    }

    const auto build = context.build_or_update();
    if (!bounded_receipt(build) || build.blas_ready || build.tlas_ready ||
        build.build_completed || build.code.find("as-materialization") == std::string::npos) {
        return fail("AS build was reported ready instead of explicit not-integrated fallback", 7);
    }
    const auto trace = context.trace();
    if (!bounded_receipt(trace) || trace.trace_completed || trace.shader_pipeline_ready ||
        trace.code.find("trace-") == std::string::npos) {
        return fail("trace was not gated by the unimplemented persistent AS/SBT path", 8);
    }
    const auto readback = context.readback();
    if (!bounded_receipt(readback) || readback.readback_completed ||
        readback.code != "native-d3d12.context.readback-not-ready") {
        return fail("readback became ready without a completed trace", 9);
    }

    const auto changed = context.ensure_scene(triangle_scene(10U));
    if (!bounded_receipt(changed) || !changed.scene_changed || changed.scene_generation != 2U ||
        context.scene_generation() != 2U) {
        return fail("scene revision change did not advance exactly one scene generation", 10);
    }

    const auto stopped = context.shutdown();
    if (!bounded_receipt(stopped) || stopped.state != NativeD3D12RayTracingContextState::shutdown ||
        !stopped.shutdown_completed || context.generation() != generation || !context.is_shutdown()) {
        return fail("shutdown did not release the context into a stable terminal state", 11);
    }
    const auto stopped_again = context.shutdown();
    if (!bounded_receipt(stopped_again) || stopped_again.state != NativeD3D12RayTracingContextState::shutdown ||
        !stopped_again.shutdown_completed ||
        stopped_again.code != "native-d3d12.context.shutdown-idempotent") {
        return fail("shutdown was not idempotent", 12);
    }
    const auto after_shutdown = context.ensure_scene(triangle_scene(11U));
    if (!bounded_receipt(after_shutdown) ||
        after_shutdown.state != NativeD3D12RayTracingContextState::shutdown ||
        after_shutdown.code != "native-d3d12.context.already-shutdown") {
        return fail("post-shutdown scene attachment was not rejected safely", 13);
    }

    std::cout << "native_d3d12_raytracing_context_tests: ok\n";
    return 0;
}
