#include "runtime/native_vulkan_raytracing_context.hpp"

#include <cstdint>
#include <iostream>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace noemancer;

bool check(const bool condition, const std::string_view message) {
    if (!condition)
        std::cerr << "native_vulkan_raytracing_context_tests: " << message << '\n';
    return condition;
}

NativeVulkanRayTracingTriangle test_triangle() {
    NativeVulkanRayTracingTriangle triangle;
    triangle.positions = {{{-1.0F, -1.0F, 0.0F},
                           {1.0F, -1.0F, 0.0F},
                           {0.0F, 1.0F, 0.0F}}};
    return triangle;
}

NativeVulkanRayTracingScene scene_for(
    const std::vector<NativeVulkanRayTracingTriangle>& triangles,
    const std::uint64_t topology_revision = 1U,
    const std::uint64_t content_revision = 1U) {
    return NativeVulkanRayTracingScene{
        .triangles = std::span<const NativeVulkanRayTracingTriangle>(triangles),
        .topology_revision = topology_revision,
        .content_revision = content_revision,
    };
}

bool test_vocabulary_and_contract_bounds() {
    if (!check(native_vulkan_raytracing_context_schema ==
                   "noemancer.native-vulkan-raytracing-context/0.1",
               "context schema drifted"))
        return false;
    if (!check(native_vulkan_raytracing_output_image_contract ==
                   "noemancer.native-vulkan-raytracing-output-image/0.1",
               "output image contract drifted"))
        return false;
    if (!check(native_vulkan_raytracing_full_frame_shader_contract ==
                   "noemancer.native-rt-full-frame/0.1",
               "full-frame Vulkan shader contract drifted"))
        return false;
    if (!check(native_vulkan_raytracing_camera_contract ==
                   "noemancer.native-vulkan-raytracing-camera/0.1" &&
                   native_vulkan_raytracing_camera_contract_version == 1U,
               "camera contract drifted"))
        return false;
    if (!check(native_vulkan_raytracing_context_state_name(
                   NativeVulkanRayTracingContextState::uninitialized) == "uninitialized" &&
                   native_vulkan_raytracing_context_state_name(
                       NativeVulkanRayTracingContextState::ready) == "ready" &&
                   native_vulkan_raytracing_context_state_name(
                       NativeVulkanRayTracingContextState::unsupported) == "unsupported" &&
                   native_vulkan_raytracing_context_state_name(
                       NativeVulkanRayTracingContextState::fallback) == "fallback" &&
                   native_vulkan_raytracing_context_state_name(
                       NativeVulkanRayTracingContextState::error) == "error" &&
                   native_vulkan_raytracing_context_state_name(
                       NativeVulkanRayTracingContextState::shutdown) == "shutdown",
               "context state vocabulary drifted"))
        return false;
    if (!check(native_vulkan_raytracing_context_failure_stage_name(
                   NativeVulkanRayTracingContextFailureStage::acceleration_structure) ==
                   "acceleration-structure" &&
                   native_vulkan_raytracing_context_failure_stage_name(
                       NativeVulkanRayTracingContextFailureStage::physical_device) ==
                   "physical-device" &&
                   native_vulkan_raytracing_context_failure_stage_name(
                       NativeVulkanRayTracingContextFailureStage::readback) == "readback",
               "context failure-stage vocabulary drifted"))
        return false;

    NativeVulkanRayTracingContextOptions options;
    options.maximum_triangles = native_vulkan_raytracing_context_hard_max_triangles + 1U;
    options.output_width = 0U;
    options.output_height = 50000U;
    options.output_depth = 0U;
    NativeVulkanRayTracingContext context(options);
    const auto receipt = context.initialize();
    return check(receipt.output_width == 1U && receipt.output_height == 4096U &&
                     receipt.output_depth == 1U &&
                     receipt.code.size() <= native_vulkan_raytracing_context_max_text_bytes &&
                     receipt.detail.size() <= native_vulkan_raytracing_context_max_text_bytes,
                 "context options were not clamped to bounded output and text contracts");
}

bool test_lifecycle_and_stable_scene_cache() {
    NativeVulkanRayTracingContext context;
    const auto initialized = context.initialize();
    const bool native = initialized.state == NativeVulkanRayTracingContextState::ready &&
                        initialized.persistent_backend;
    const bool fallback = initialized.state == NativeVulkanRayTracingContextState::fallback &&
                          initialized.fallback_active;
    std::cout << "native_vulkan_raytracing_context_tests: backend="
              << (native ? "native" : (fallback ? "fallback" : "error")) << '\n';
    if (!native)
        std::cout << "native_vulkan_raytracing_context_tests: init=" << initialized.code
                  << " detail=" << initialized.detail << '\n';
    if (!check((native || fallback) && initialized.initialized &&
                   initialized.resources_live == native &&
                   !initialized.build_submitted && !initialized.build_completed &&
                   !initialized.trace_submitted && !initialized.trace_completed &&
                   !initialized.camera_requested && !initialized.camera_valid &&
                    !initialized.camera_shader_consumed && !initialized.full_frame_dispatch &&
                    initialized.camera_contract.empty() &&
                   initialized.camera_contract_version == 0U &&
                   initialized.camera_boundary == "not-requested" &&
                   initialized.generation == 1U,
               "initialization did not expose an honest native-or-fallback lifecycle receipt"))
        return false;
    if (!check(initialized.output_image_contract ==
                   "noemancer.native-vulkan-raytracing-output-image/0.1",
               "initialization did not expose the output image contract identity"))
        return false;
    if (native) {
        if (!check(initialized.full_frame_shader_ready &&
                       initialized.shader_contract == "noemancer.native-rt-full-frame/0.1" &&
                       initialized.output_image_live && initialized.output_image_view_live &&
                       initialized.output_image_runtime_private && !initialized.output_image_interop_ready &&
                       !initialized.output_image_external_import_supported &&
                       initialized.output_image_same_device_required && initialized.output_image_layout_ready &&
                       initialized.output_image_sync_complete && !initialized.output_image_trace_written &&
                       !initialized.output_image_cpu_readback_supported &&
                       initialized.output_image_format == "r32_uint" &&
                       initialized.output_image_layout == "general" &&
                       initialized.output_image_access == "storage-read-write" &&
                       initialized.output_image_sync_kind == "fence" &&
                       initialized.output_image_generation > 0U && initialized.output_image_bytes > 0U &&
                       initialized.output_image_sync_value > 0U &&
                       initialized.output_image_interop_boundary.find("no handle export") != std::string::npos,
                   "native initialization did not expose a safe runtime-private output image contract"))
            return false;
    } else if (!check(!initialized.output_image_live && !initialized.output_image_view_live &&
                          !initialized.full_frame_shader_ready && initialized.shader_contract.empty() &&
                          !initialized.output_image_runtime_private && !initialized.output_image_interop_ready &&
                          !initialized.output_image_layout_ready && !initialized.output_image_sync_complete &&
                          initialized.output_image_format == "none" && initialized.output_image_layout == "none" &&
                          initialized.output_image_sync_kind == "none" &&
                          initialized.output_image_interop_boundary == "unavailable",
                      "fallback initialization overclaimed a Vulkan output image"))
        return false;
    const auto output_image_generation = initialized.output_image_generation;

    const auto initialized_again = context.initialize();
    if (!check(initialized_again.generation == initialized.generation &&
                   initialized_again.code.find("already-initialized") != std::string::npos,
               "initialization was not idempotent"))
        return false;

    std::vector<NativeVulkanRayTracingTriangle> triangles{test_triangle()};
    const auto scene = scene_for(triangles);
    const auto ensured = context.ensure_scene(scene);
    if (!check(ensured.state == (native ? NativeVulkanRayTracingContextState::ready
                                        : NativeVulkanRayTracingContextState::fallback) &&
                   ensured.scene_ready && ensured.scene_rebuilt && !ensured.scene_updated &&
                   !ensured.scene_reused && ensured.triangle_count == 1U &&
                   ensured.persistent_backend == native && ensured.generation == 2U,
               "first scene submission did not create a deterministic snapshot"))
        return false;
    if (!check(ensured.output_image_generation == output_image_generation &&
                   ensured.output_image_live == native && ensured.output_image_runtime_private == native,
               "scene submission did not preserve the output image lifetime contract"))
        return false;

    const auto built = context.build_or_update();
    if (!check(built.scene_ready &&
                   ((native && built.state == NativeVulkanRayTracingContextState::ready &&
                     built.persistent_backend && built.build_submitted && built.build_completed) ||
                    (fallback && built.state == NativeVulkanRayTracingContextState::fallback &&
                     !built.persistent_backend && !built.build_submitted && !built.build_completed)),
               "first build did not expose an honest native AS build or fallback receipt"))
        return false;

    // A render loop must be able to submit the same scene repeatedly without
    // rebuilding AS resources.  Keep this as three explicit frame attempts so
    // the contract covers the common first-frame + two steady-state pattern.
    const auto generation_after_build = context.generation();
    for (int frame = 0; frame < 2; ++frame) {
        const auto frame_scene = context.ensure_scene(scene);
        const auto frame_build = context.build_or_update();
        if (!check(frame_scene.scene_reused && !frame_scene.scene_rebuilt &&
                       !frame_scene.scene_updated && frame_scene.generation == generation_after_build &&
                       frame_build.generation == generation_after_build &&
                       !frame_build.build_submitted && !frame_build.build_completed &&
                       frame_build.scene_ready,
                   "steady-state frame unexpectedly rebuilt or resubmitted the native scene"))
            return false;
        if (!check(frame_scene.output_image_generation == output_image_generation &&
                       frame_build.output_image_generation == output_image_generation &&
                       frame_scene.output_image_live == native && frame_build.output_image_live == native,
                   "steady-state frame changed the output image generation or lifetime"))
            return false;
    }

    const auto traced = context.trace();
    if (!check(traced.state == (native ? NativeVulkanRayTracingContextState::ready
                                       : NativeVulkanRayTracingContextState::fallback) &&
                   traced.trace_completed && traced.trace_submitted == native &&
                   !traced.camera_requested && !traced.camera_valid &&
                   !traced.camera_shader_consumed && traced.camera_contract.empty() &&
                   traced.camera_contract_version == 0U &&
                   traced.camera_boundary == "not-requested" &&
                   traced.full_frame_shader_ready == native &&
                    !traced.full_frame_dispatch &&
                   traced.output_image_trace_written == native &&
                   traced.persistent_backend == native && traced.output_bytes == sizeof(std::uint32_t),
               native ? "native trace did not complete through the persistent pipeline"
                      : "fallback trace did not complete with an honest receipt"))
        return false;

    const auto readback = context.readback();
    if (!check(readback.readback_completed &&
                   readback.readback_bytes == sizeof(std::uint32_t) &&
                   readback.output_hash != 0U && readback.persistent_backend == native &&
                   readback.output_hit == 1U && readback.output_value == 0x48495421U,
               native ? "native output readback did not return the shader hit marker"
                      : "fallback readback receipt was incomplete or overclaimed native work"))
        return false;
    if (!check(readback.output_image_generation == output_image_generation &&
                   readback.output_image_live == native &&
                   readback.output_image_trace_written == native &&
                   readback.full_frame_shader_ready == native &&
                   (!native || readback.shader_contract == "noemancer.native-rt-full-frame/0.1"),
               "trace/readback did not preserve the runtime-private output image boundary"))
        return false;

    NativeVulkanRayTracingTraceRequest camera_request;
    camera_request.camera_enabled = true;
    camera_request.camera.position = {0.0F, 0.5F, -2.0F};
    camera_request.camera.forward = {0.0F, -0.1F, 1.0F};
    camera_request.camera.up = {0.0F, 1.0F, 0.1F};
    const auto camera_trace = context.trace(camera_request);
    if (!check(camera_trace.state == (native ? NativeVulkanRayTracingContextState::ready
                                              : NativeVulkanRayTracingContextState::fallback) &&
                    camera_trace.camera_requested && camera_trace.camera_valid &&
                    camera_trace.camera_shader_consumed == native &&
                    !camera_trace.full_frame_dispatch &&
                    camera_trace.camera_contract ==
                        "noemancer.native-vulkan-raytracing-camera/0.1" &&
                    camera_trace.camera_contract_version == 1U &&
                    ((native && camera_trace.camera_boundary.find("RayGen consumed") != std::string::npos) ||
                     (!native && camera_trace.camera_boundary.find("fallback shader not applicable") != std::string::npos)),
                "valid camera input was not accepted with an honest shader-consumption boundary"))
        return false;

    const auto generation_before_reuse = context.generation();
    const auto reused = context.ensure_scene(scene);
    if (!check(reused.scene_reused && !reused.scene_rebuilt && !reused.scene_updated &&
                   reused.generation == generation_before_reuse,
               "identical scene submission did not reuse its stable snapshot"))
        return false;

    triangles[0U].positions[0U][0U] = -0.5F;
    const auto updated_scene = scene_for(triangles, 1U, 2U);
    const auto updated = context.ensure_scene(updated_scene);
    if (!check(updated.scene_updated && !updated.scene_rebuilt && !updated.scene_reused &&
                   updated.generation > generation_before_reuse &&
                   updated.scene_content_revision == 2U,
               "in-place geometry content change did not produce an update"))
        return false;
    const auto updated_build = context.build_or_update();
    if (!check((native && updated_build.state == NativeVulkanRayTracingContextState::ready &&
                updated_build.persistent_backend && updated_build.build_submitted &&
                updated_build.build_completed) ||
               (fallback && updated_build.state == NativeVulkanRayTracingContextState::fallback &&
                !updated_build.persistent_backend && !updated_build.build_submitted &&
                !updated_build.build_completed),
               "content update did not expose an honest native update or fallback receipt"))
        return false;

    triangles.push_back(test_triangle());
    const auto rebuilt_scene = scene_for(triangles, 2U, 3U);
    const auto rebuilt = context.ensure_scene(rebuilt_scene);
    if (!check(rebuilt.scene_rebuilt && !rebuilt.scene_updated && !rebuilt.scene_reused &&
                   rebuilt.triangle_count == 2U && rebuilt.scene_topology_revision == 2U &&
                   rebuilt.generation > updated.generation,
               "topology change did not produce a rebuild"))
        return false;
    const auto rebuilt_build = context.build_or_update();
    return check((native && rebuilt_build.state == NativeVulkanRayTracingContextState::ready &&
                  rebuilt_build.persistent_backend && rebuilt_build.build_submitted &&
                  rebuilt_build.build_completed) ||
                     (fallback && rebuilt_build.state == NativeVulkanRayTracingContextState::fallback &&
                      !rebuilt_build.persistent_backend && !rebuilt_build.build_submitted &&
                      !rebuilt_build.build_completed),
                 "topology rebuild did not expose an honest native build or fallback receipt");
}

bool test_missing_invalid_and_unsupported_paths() {
    NativeVulkanRayTracingContext context;
    const auto empty = context.ensure_scene({});
    if (!check(empty.state == NativeVulkanRayTracingContextState::error &&
                   empty.failure_stage == NativeVulkanRayTracingContextFailureStage::scene &&
                   empty.code.find("scene-empty") != std::string::npos && !context.scene_ready(),
               "empty scene was not rejected with a scene diagnostic"))
        return false;

    std::vector<NativeVulkanRayTracingTriangle> triangles{test_triangle()};
    triangles[0U].positions[1U][2U] = std::numeric_limits<float>::quiet_NaN();
    const auto nonfinite = context.ensure_scene(scene_for(triangles));
    if (!check(nonfinite.state == NativeVulkanRayTracingContextState::error &&
                   nonfinite.code.find("scene-nonfinite") != std::string::npos &&
                   !context.scene_ready(),
               "non-finite scene data was accepted"))
        return false;

    NativeVulkanRayTracingContextOptions limited_options;
    limited_options.maximum_triangles = 1U;
    NativeVulkanRayTracingContext limited(limited_options);
    std::vector<NativeVulkanRayTracingTriangle> two_triangles{test_triangle(), test_triangle()};
    const auto over_budget = limited.ensure_scene(scene_for(two_triangles));
    if (!check(over_budget.state == NativeVulkanRayTracingContextState::error &&
                   over_budget.code.find("scene-limit") != std::string::npos,
               "scene triangle budget was not enforced"))
        return false;

    NativeVulkanRayTracingContextOptions no_fallback_options;
    no_fallback_options.allow_fallback = false;
    NativeVulkanRayTracingContext no_fallback(no_fallback_options);
    const auto unsupported = no_fallback.initialize();
    const bool native_without_fallback =
        unsupported.state == NativeVulkanRayTracingContextState::ready &&
        unsupported.persistent_backend;
    const bool explicitly_unsupported =
        unsupported.state == NativeVulkanRayTracingContextState::unsupported &&
        !unsupported.fallback_active && !unsupported.persistent_backend;
    if (!check((native_without_fallback || explicitly_unsupported) && unsupported.initialized,
               "fallback-disabled initialization was neither native nor explicitly unsupported"))
        return false;

    NativeVulkanRayTracingContextOptions incomplete_borrowed_options;
    incomplete_borrowed_options.allow_fallback = false;
    incomplete_borrowed_options.borrowed_device.device = reinterpret_cast<void*>(static_cast<std::uintptr_t>(1U));
    NativeVulkanRayTracingContext incomplete_borrowed(incomplete_borrowed_options);
    const auto incomplete_borrowed_init = incomplete_borrowed.initialize();
    if (!check(incomplete_borrowed_init.state == NativeVulkanRayTracingContextState::unsupported &&
                   incomplete_borrowed_init.code.find("shared-device-incomplete") != std::string::npos &&
                   !incomplete_borrowed_init.persistent_backend && !incomplete_borrowed_init.resources_live,
               "incomplete borrowed Vulkan handles were not rejected without creating a private device"))
        return false;
    const auto unsupported_scene = no_fallback.ensure_scene(scene_for(two_triangles));
    if (!check(unsupported_scene.state ==
                   (native_without_fallback ? NativeVulkanRayTracingContextState::ready
                                             : NativeVulkanRayTracingContextState::unsupported),
               native_without_fallback ? "native context rejected scene work"
                                       : "unsupported context accepted scene work"))
        return false;

    std::vector<NativeVulkanRayTracingTriangle> valid_triangles{test_triangle()};
    NativeVulkanRayTracingContext valid;
    if (!check(valid.ensure_scene(scene_for(valid_triangles)).scene_ready,
               "valid context setup failed"))
        return false;
    auto invalid_request = NativeVulkanRayTracingTraceRequest{};
    invalid_request.direction = {0.0F, 0.0F, 0.0F};
    const auto invalid_trace = valid.trace(invalid_request);
    if (!check(invalid_trace.state == NativeVulkanRayTracingContextState::error &&
                   invalid_trace.failure_stage == NativeVulkanRayTracingContextFailureStage::trace &&
                   invalid_trace.code.find("trace-invalid-request") != std::string::npos &&
                   !invalid_trace.trace_completed,
               "zero-length trace direction was accepted"))
        return false;

    auto invalid_camera_request = NativeVulkanRayTracingTraceRequest{};
    invalid_camera_request.camera_enabled = true;
    invalid_camera_request.camera.contract_version = 99U;
    invalid_camera_request.camera.forward = {0.0F, 0.0F, 0.0F};
    const auto invalid_camera_trace = valid.trace(invalid_camera_request);
    if (!check(invalid_camera_trace.state == NativeVulkanRayTracingContextState::error &&
                   invalid_camera_trace.failure_stage == NativeVulkanRayTracingContextFailureStage::trace &&
                   invalid_camera_trace.code.find("camera-invalid") != std::string::npos &&
                   invalid_camera_trace.camera_requested && !invalid_camera_trace.camera_valid &&
                   !invalid_camera_trace.camera_shader_consumed &&
                   invalid_camera_trace.camera_contract ==
                       "noemancer.native-vulkan-raytracing-camera/0.1" &&
                   invalid_camera_trace.camera_contract_version == 99U &&
                   invalid_camera_trace.camera_boundary.find("rejected") != std::string::npos,
               "invalid camera input was not rejected before any trace submission"))
        return false;

    NativeVulkanRayTracingContext before_trace;
    if (!check(before_trace.ensure_scene(scene_for(valid_triangles)).scene_ready,
               "readback precondition setup failed"))
        return false;
    const auto missing_trace = before_trace.readback();
    return check(missing_trace.state == NativeVulkanRayTracingContextState::error &&
                     missing_trace.failure_stage == NativeVulkanRayTracingContextFailureStage::readback &&
                     missing_trace.code.find("trace-missing") != std::string::npos,
                 "readback without a trace was not rejected");
}

bool test_shutdown_is_idempotent_and_terminal() {
    NativeVulkanRayTracingContext never_initialized;
    const auto shutdown_before_initialize = never_initialized.shutdown();
    if (!check(shutdown_before_initialize.state == NativeVulkanRayTracingContextState::shutdown &&
                   !shutdown_before_initialize.initialized,
               "shutdown before initialization did not remain a terminal state"))
        return false;
    const auto initialize_after_shutdown = never_initialized.initialize();
    if (!check(initialize_after_shutdown.state == NativeVulkanRayTracingContextState::error &&
                   initialize_after_shutdown.failure_stage ==
                       NativeVulkanRayTracingContextFailureStage::shutdown,
               "shutdown-before-initialize context was reinitialized"))
        return false;

    NativeVulkanRayTracingContext context;
    std::vector<NativeVulkanRayTracingTriangle> triangles{test_triangle()};
    static_cast<void>(context.ensure_scene(scene_for(triangles)));
    static_cast<void>(context.trace());
    const auto shutdown = context.shutdown();
    if (!check(shutdown.state == NativeVulkanRayTracingContextState::shutdown &&
                   shutdown.shutdown && !shutdown.resources_live && !shutdown.scene_ready &&
                   !shutdown.trace_completed && !shutdown.output_image_live &&
                   !shutdown.output_image_view_live && !shutdown.output_image_runtime_private &&
                   !shutdown.output_image_interop_ready && shutdown.output_image_generation == 0U &&
                   shutdown.output_image_bytes == 0U && shutdown.output_image_sync_value == 0U &&
                   shutdown.output_image_layout == "none" &&
                   shutdown.output_image_interop_boundary == "unavailable",
               "shutdown did not clear the context-owned lifecycle state"))
        return false;
    const auto shutdown_again = context.shutdown();
    if (!check(shutdown_again.state == NativeVulkanRayTracingContextState::shutdown &&
                   shutdown_again.code.find("already-complete") != std::string::npos,
               "shutdown was not idempotent"))
        return false;
    const auto after_shutdown = context.ensure_scene(scene_for(triangles));
    return check(after_shutdown.state == NativeVulkanRayTracingContextState::error &&
                     after_shutdown.failure_stage == NativeVulkanRayTracingContextFailureStage::shutdown &&
                     after_shutdown.shutdown,
                 "terminal shutdown context accepted new scene work");
}

} // namespace

int main() {
    if (!test_vocabulary_and_contract_bounds()) return 1;
    if (!test_lifecycle_and_stable_scene_cache()) return 2;
    if (!test_missing_invalid_and_unsupported_paths()) return 3;
    if (!test_shutdown_is_idempotent_and_terminal()) return 4;
    std::cout << "native_vulkan_raytracing_context_tests: ok\n";
    return 0;
}
