#include "runtime/native_d3d12_raytracing_context.hpp"

#include <cstdint>
#include <cstddef>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

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

NativeD3D12RayTracingScene two_triangle_scene(const std::uint64_t revision = 11U) {
    auto scene = triangle_scene(revision);
    scene.geometries.push_back(NativeD3D12RayTracingGeometry{
        .geometry_id = "triangle.extra",
        .position_xyz = {-0.5F, -0.5F, 0.5F,
                          0.5F, -0.5F, 0.5F,
                          0.0F,  0.5F, 0.5F},
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

    const bool native_available = initialized.state == NativeD3D12RayTracingContextState::ready &&
        initialized.device_ready && initialized.command_queue_ready && initialized.fence_ready;
    const auto build = context.build_or_update();
    if (!bounded_receipt(build)) return fail("first AS build receipt was unbounded", 7);
    if (native_available) {
        if (build.state != NativeD3D12RayTracingContextState::ready || !build.blas_ready ||
            !build.tlas_ready || !build.build_submitted || !build.build_completed ||
            build.update_submitted || !build.synchronization_completed ||
            build.resource_generation != 1U || build.vertex_buffer_bytes == 0U ||
            build.index_buffer_bytes == 0U || build.blas_result_bytes == 0U ||
            build.blas_scratch_bytes == 0U || build.tlas_result_bytes == 0U ||
            build.tlas_scratch_bytes == 0U ||
            build.code != "native-d3d12.context.as-build-complete") {
            return fail("persistent BLAS/TLAS build did not complete with truthful resource evidence", 7);
        }
    } else if (build.state != NativeD3D12RayTracingContextState::unsupported ||
               build.blas_ready || build.tlas_ready || build.build_completed ||
               !build.fallback_active) {
        return fail("unsupported D3D12 context did not retain an explicit fallback", 7);
    }

    const auto reused = context.build_or_update();
    if (!bounded_receipt(reused)) return fail("resource reuse receipt was unbounded", 8);
    if (native_available) {
        if (reused.code != "native-d3d12.context.resources-reused" ||
            !reused.blas_ready || !reused.tlas_ready || reused.build_submitted ||
            reused.update_submitted || reused.resource_generation != build.resource_generation ||
            !reused.synchronization_completed) {
            return fail("second build did not reuse the persistent AS resources", 8);
        }
    } else if (reused.state != NativeD3D12RayTracingContextState::unsupported ||
               !reused.fallback_active) {
        return fail("unsupported D3D12 context changed fallback state on reuse", 8);
    }

    const auto trace = context.trace();
    if (!bounded_receipt(trace)) return fail("trace receipt was unbounded", 9);
    if (native_available) {
        if (trace.state != NativeD3D12RayTracingContextState::ready ||
            !trace.shader_pipeline_ready || !trace.shader_table_ready ||
            !trace.output_resource_ready || !trace.trace_submitted ||
            !trace.trace_completed || !trace.synchronization_completed ||
            trace.shader_table_bytes == 0U || trace.output_bytes == 0U ||
            trace.code != "native-d3d12.context.trace-complete") {
            return fail("persistent TraceRays did not complete with truthful pipeline/output evidence", 9);
        }
    } else if (trace.trace_completed || trace.shader_pipeline_ready ||
               trace.code.find("trace-") == std::string::npos) {
        return fail("unsupported D3D12 context did not retain an explicit trace fallback", 9);
    }
    const auto output_surface = context.output_surface_metadata();
    const auto output_view = context.private_output_surface_view();
    if (native_available) {
        if (!output_surface.valid || !output_surface.resource_ready ||
            !output_surface.gpu_write_complete || output_surface.cpu_readback_required ||
            output_surface.resource_state !=
                NativeD3D12RayTracingOutputSurfaceState::copy_source ||
            output_surface.resource_kind != "buffer" ||
            output_surface.format != "R32G32B32A32_UINT" ||
            output_surface.width != trace.output_width ||
            output_surface.height != trace.output_height ||
            output_surface.bytes != trace.output_bytes ||
            output_surface.resource_generation == 0U ||
            output_surface.context_generation != trace.generation ||
            output_surface.submitted_fence_value == 0U ||
            output_surface.completed_fence_value < output_surface.submitted_fence_value ||
            output_surface.shared_device || output_surface.shared_command_queue ||
            output_surface.direct_sdl_gpu_import_supported ||
            output_view.access_token == 0U || output_view.resource == nullptr ||
            output_view.device == nullptr || output_view.command_queue == nullptr ||
            output_view.fence == nullptr ||
            !context.is_private_output_surface_view_current(output_view)) {
            return fail("completed output did not expose a valid fenced runtime-private view", 9);
        }
        auto stale_view = output_view;
        ++stale_view.metadata.resource_generation;
        if (context.is_private_output_surface_view_current(stale_view)) {
            return fail("output view generation guard accepted a stale view", 9);
        }
        const auto rejected_copy = context.copy_output_to(output_view, nullptr);
        if (!bounded_receipt(rejected_copy) ||
            rejected_copy.state != NativeD3D12RayTracingContextState::failed ||
            rejected_copy.code != "native-d3d12.context.output-destination-null" ||
            rejected_copy.output_surface.valid == false) {
            return fail("runtime-private output copy rejected an invalid destination incorrectly", 9);
        }
    } else if (output_surface.valid || output_surface.resource_ready ||
               output_surface.gpu_write_complete || output_view.access_token != 0U ||
               context.is_private_output_surface_view_current(output_view)) {
        return fail("unsupported D3D12 context reported a live output surface", 9);
    }
    const auto readback = context.readback();
    if (!bounded_receipt(readback)) return fail("readback receipt was unbounded", 10);
    if (native_available) {
        if (readback.state != NativeD3D12RayTracingContextState::ready ||
            !readback.readback_completed || readback.output_sentinel != 0x52415931U ||
            readback.output_hit != 1U || readback.output_hash == 0U ||
            readback.code != "native-d3d12.context.readback-complete") {
            return fail("persistent output readback did not expose the deterministic hit proof", 10);
        }
        const auto trace_reused = context.trace();
        if (!bounded_receipt(trace_reused) ||
            trace_reused.state != NativeD3D12RayTracingContextState::ready ||
            !trace_reused.shader_pipeline_ready || !trace_reused.shader_table_ready ||
            !trace_reused.output_resource_ready || !trace_reused.trace_submitted ||
            !trace_reused.trace_completed ||
            trace_reused.shader_table_bytes != trace.shader_table_bytes ||
            trace_reused.output_bytes != trace.output_bytes ||
            trace_reused.code != "native-d3d12.context.trace-complete") {
            return fail("second TraceRays dispatch did not reuse the retained pipeline/resources", 10);
        }
        const auto readback_reused = context.readback();
        if (!bounded_receipt(readback_reused) ||
            !readback_reused.readback_completed ||
            readback_reused.output_hash != readback.output_hash ||
            readback_reused.output_sentinel != 0x52415931U ||
            readback_reused.output_hit != 1U ||
            readback_reused.code != "native-d3d12.context.readback-complete") {
            return fail("second output readback did not reuse the retained output resource", 10);
        }
    } else if (readback.readback_completed ||
               readback.code != "native-d3d12.context.readback-not-ready") {
        return fail("unsupported D3D12 context reported output readback without TraceRays", 10);
    }

    auto changed_scene = triangle_scene(10U);
    changed_scene.geometries[0].position_xyz[0] = -0.9F;
    const auto changed = context.ensure_scene(changed_scene);
    if (!bounded_receipt(changed) || !changed.scene_changed || changed.scene_generation != 2U ||
        context.scene_generation() != 2U) {
        return fail("scene revision change did not advance exactly one scene generation", 11);
    }
    if (native_available && context.is_private_output_surface_view_current(output_view)) {
        return fail("scene change did not invalidate the previous output view", 11);
    }

    const auto updated = context.build_or_update();
    if (!bounded_receipt(updated)) return fail("content update receipt was unbounded", 12);
    if (native_available) {
        if (updated.state != NativeD3D12RayTracingContextState::ready || !updated.blas_ready ||
            !updated.tlas_ready || !updated.build_submitted || !updated.build_completed ||
            !updated.update_submitted || !updated.update_completed ||
            updated.resource_generation != build.resource_generation ||
            updated.code != "native-d3d12.context.as-update-complete") {
            return fail("same-topology content revision did not use persistent AS update", 12);
        }
    } else if (updated.state != NativeD3D12RayTracingContextState::unsupported ||
               !updated.fallback_active) {
        return fail("unsupported D3D12 context failed after a scene revision", 12);
    }

    if (native_available) {
        const auto trace_after_update = context.trace();
        if (!bounded_receipt(trace_after_update) ||
            trace_after_update.state != NativeD3D12RayTracingContextState::ready ||
            !trace_after_update.trace_completed ||
            trace_after_update.code != "native-d3d12.context.trace-complete") {
            return fail("TraceRays did not rerun after a same-topology scene update", 13);
        }
        const auto output_view_after_update = context.private_output_surface_view();
        if (!context.is_private_output_surface_view_current(output_view_after_update) ||
            output_view_after_update.access_token == output_view.access_token ||
            output_view_after_update.metadata.resource_generation !=
                output_surface.resource_generation) {
            return fail("same-topology trace did not publish a fresh output view generation", 13);
        }
        const auto readback_after_update = context.readback();
        if (!bounded_receipt(readback_after_update) ||
            !readback_after_update.readback_completed ||
            readback_after_update.output_sentinel != 0x52415931U ||
            readback_after_update.output_hit != 1U ||
            readback_after_update.code != "native-d3d12.context.readback-complete") {
            return fail("output readback did not remain valid after a scene update", 13);
        }
    }

    const auto topology_changed = context.ensure_scene(two_triangle_scene());
    if (!bounded_receipt(topology_changed) || !topology_changed.scene_changed ||
        topology_changed.scene_generation != 3U) {
        return fail("topology change did not advance scene generation", 14);
    }
    const auto rebuilt = context.build_or_update();
    if (!bounded_receipt(rebuilt)) return fail("topology rebuild receipt was unbounded", 15);
    if (native_available) {
        if (rebuilt.state != NativeD3D12RayTracingContextState::ready || !rebuilt.blas_ready ||
            !rebuilt.tlas_ready || !rebuilt.build_submitted || !rebuilt.build_completed ||
            rebuilt.update_submitted || rebuilt.update_completed ||
            rebuilt.resource_generation != build.resource_generation + 1U ||
            rebuilt.geometry_count != 2U || rebuilt.instance_count != 2U ||
            rebuilt.code != "native-d3d12.context.as-build-complete") {
            return fail("topology change did not rebuild persistent BLAS/TLAS resources", 15);
        }
    } else if (rebuilt.state != NativeD3D12RayTracingContextState::unsupported ||
               !rebuilt.fallback_active) {
        return fail("unsupported D3D12 context failed after a topology change", 15);
    }

    const auto stopped = context.shutdown();
    if (!bounded_receipt(stopped) || stopped.state != NativeD3D12RayTracingContextState::shutdown ||
        !stopped.shutdown_completed || context.generation() != generation || !context.is_shutdown()) {
        return fail("shutdown did not release the context into a stable terminal state", 16);
    }
    const auto output_after_shutdown = context.output_surface_metadata();
    if (output_after_shutdown.valid || output_after_shutdown.resource_ready ||
        !output_after_shutdown.expired ||
        output_after_shutdown.resource_state !=
            NativeD3D12RayTracingOutputSurfaceState::released ||
        context.is_private_output_surface_view_current(output_view)) {
        return fail("shutdown did not invalidate the runtime-private output surface", 16);
    }
    const auto stopped_again = context.shutdown();
    if (!bounded_receipt(stopped_again) || stopped_again.state != NativeD3D12RayTracingContextState::shutdown ||
        !stopped_again.shutdown_completed ||
        stopped_again.code != "native-d3d12.context.shutdown-idempotent") {
        return fail("shutdown was not idempotent", 17);
    }
    const auto after_shutdown = context.ensure_scene(triangle_scene(11U));
    if (!bounded_receipt(after_shutdown) ||
        after_shutdown.state != NativeD3D12RayTracingContextState::shutdown ||
        after_shutdown.code != "native-d3d12.context.already-shutdown") {
        return fail("post-shutdown scene attachment was not rejected safely", 18);
    }

#if defined(_WIN32)
    if (native_available) {
        std::ifstream shader_input(NOEMANCER_NATIVE_RT_TEST_DXIL,
                                   std::ios::binary | std::ios::ate);
        if (!shader_input) return fail("versioned full-frame DXIL was not generated", 19);
        const auto shader_size = shader_input.tellg();
        std::vector<std::byte> shader_bytes(static_cast<std::size_t>(shader_size));
        shader_input.seekg(0);
        shader_input.read(reinterpret_cast<char*>(shader_bytes.data()), shader_size);
        noemancer::NativeD3D12RayTracingContextOptions full_frame_options;
        full_frame_options.output_width = 16U;
        full_frame_options.output_height = 2U;
        full_frame_options.shaders.full_frame_contract =
            "noemancer.native-rt-full-frame/0.1";
        full_frame_options.shaders.full_frame_library_dxil = std::move(shader_bytes);
        NativeD3D12RayTracingContext full_frame(full_frame_options);
        const auto full_frame_init = full_frame.initialize();
        if (!bounded_receipt(full_frame_init) ||
            full_frame_init.state != NativeD3D12RayTracingContextState::ready ||
            !full_frame_init.full_frame_shader_ready ||
            full_frame_init.shader_contract != "noemancer.native-rt-full-frame/0.1") {
            return fail("versioned full-frame DXR library did not initialize", 19);
        }
        const auto full_frame_scene = full_frame.ensure_scene(triangle_scene());
        const auto full_frame_build = full_frame.build_or_update();
        const auto full_frame_trace = full_frame.trace();
        if (!full_frame_scene.scene_received || !full_frame_build.build_completed ||
            !full_frame_trace.trace_completed || !full_frame_trace.full_frame_shader_ready ||
            full_frame_trace.output_width != 16U || full_frame_trace.output_height != 2U) {
            return fail("versioned full-frame DXR library did not execute its bounded dispatch", 19);
        }
        static_cast<void>(full_frame.shutdown());
    }

    NativeD3D12RayTracingContextOptions malformed_borrowed_options;
    malformed_borrowed_options.borrowed_command_queue =
        reinterpret_cast<void*>(static_cast<std::uintptr_t>(1U));
    NativeD3D12RayTracingContext malformed_borrowed(malformed_borrowed_options);
    const auto malformed_borrowed_receipt = malformed_borrowed.initialize();
    if (!bounded_receipt(malformed_borrowed_receipt) ||
        malformed_borrowed_receipt.state != NativeD3D12RayTracingContextState::failed ||
        malformed_borrowed_receipt.code !=
            "native-d3d12.context.borrowed-queue-without-device" ||
        malformed_borrowed_receipt.native_handle_exposed) {
        return fail("borrowed queue without its device was not rejected at the runtime boundary", 20);
    }
    static_cast<void>(malformed_borrowed.shutdown());
#endif

    std::cout << "native_d3d12_raytracing_context_tests: ok\n";
    return 0;
}
