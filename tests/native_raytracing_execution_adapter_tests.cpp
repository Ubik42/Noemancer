#include "runtime/native_raytracing_execution_adapter.hpp"

#include <algorithm>
#include <iostream>
#include <string>
#include <string_view>

namespace {

using namespace noemancer;

bool check(const bool condition, const std::string_view message) {
    if (!condition)
        std::cerr << "native_raytracing_execution_adapter_tests: " << message
                  << '\n';
    return condition;
}

NativeD3D12RayTracingReceipt d3d12_build_receipt() {
    NativeD3D12RayTracingReceipt result;
    result.state = NativeD3D12RayTracingExecutionState::succeeded;
    result.code = "native-d3d12-raytracing.blas-tlas-succeeded";
    result.detail = "build probe";
    result.hardware_probe_completed = true;
    result.hardware_device_created = true;
    result.hardware_raytracing_device_found = true;
    result.raytracing_tier = 1U;
    result.vertex_buffer_bytes = 36U;
    result.blas_prebuild_completed = true;
    result.blas_build_submitted = true;
    result.blas_build_completed = true;
    result.tlas_prebuild_completed = true;
    result.tlas_build_submitted = true;
    result.tlas_build_completed = true;
    result.synchronization_completed = true;
    result.blas_scratch_bytes = 1024U;
    result.blas_result_bytes = 2048U;
    result.tlas_scratch_bytes = 1024U;
    result.tlas_result_bytes = 2048U;
    result.build_only = true;
    result.native_handle_exposed = false;
    return result;
}

NativeVulkanRayTracingExecutionReceipt vulkan_build_receipt() {
    NativeVulkanRayTracingExecutionReceipt result;
    result.state = NativeVulkanRayTracingExecutionState::completed;
    result.code = "native-vulkan-rt.blas-tlas-built";
    result.detail = "build probe";
    result.loader_available = true;
    result.instance_created = true;
    result.device_created = true;
    result.queue_found = true;
    result.feature_chain_enabled = true;
    result.geometry_count = 1U;
    result.instance_count = 1U;
    result.blas_built = true;
    result.tlas_built = true;
    result.submitted = true;
    result.fence_signaled = true;
    result.resources_released = true;
    result.vertex_buffer_bytes = 36U;
    result.instance_buffer_bytes = 64U;
    result.blas_scratch_bytes = 1024U;
    result.blas_result_bytes = 2048U;
    result.tlas_scratch_bytes = 1024U;
    result.tlas_result_bytes = 2048U;
    result.total_allocated_bytes = 8192U;
    return result;
}

NativeD3D12RayTracingReceipt d3d12_trace_receipt() {
    auto result = d3d12_build_receipt();
    result.build_only = false;
    result.blas_flags_observed = true;
    result.tlas_flags_observed = true;
    result.blas_prefer_fast_trace = true;
    result.tlas_prefer_fast_trace = true;
    result.blas_build_count = 1U;
    result.tlas_build_count = 1U;
    result.root_signature_created = true;
    result.state_object_created = true;
    result.shader_table_prepared = true;
    result.shader_table_uploaded = true;
    result.shader_table_record_count = 3U;
    result.shader_table_record_bytes = 64U;
    result.shader_table_bytes = 192U;
    result.output_resource_created = true;
    result.output_width = 1U;
    result.output_height = 1U;
    result.output_pixel_stride_bytes = 16U;
    result.output_bytes = 16U;
    result.output_readback_bytes = 16U;
    result.trace_dispatch_issued = true;
    result.trace_dispatch_submitted = true;
    result.trace_dispatch_completed = true;
    result.output_readback_completed = true;
    result.timestamp_query_created = true;
    result.timestamp_queries_issued = true;
    result.timestamp_data_resolved = true;
    result.gpu_timestamp_frequency_hz = 1'000'000'000ULL;
    result.gpu_timestamp_ticks_begin = 100U;
    result.gpu_timestamp_ticks_end = 200U;
    result.gpu_timestamp_ticks_delta = 100U;
    result.gpu_timestamp_duration_ns = 100U;
    result.gpu_timestamp_readback_bytes = 16U;
    result.gpu_timestamps_valid = true;
    return result;
}

NativeVulkanRayTracingExecutionReceipt vulkan_trace_receipt() {
    auto result = vulkan_build_receipt();
    result.build_only = false;
    result.blas_flags_observed = true;
    result.tlas_flags_observed = true;
    result.blas_prefer_fast_trace = true;
    result.tlas_prefer_fast_trace = true;
    result.blas_build_count = 1U;
    result.tlas_build_count = 1U;
    result.shader_module_created = true;
    result.descriptor_set_layout_created = true;
    result.pipeline_layout_created = true;
    result.descriptor_set_allocated = true;
    result.pipeline_created = true;
    result.sbt_built = true;
    result.shader_table_prepared = true;
    result.shader_table_uploaded = true;
    result.shader_table_record_count = 3U;
    result.shader_table_record_bytes = 64U;
    result.shader_table_bytes = 192U;
    result.sbt_buffer_bytes = 192U;
    result.output_resource_created = true;
    result.output_width = 1U;
    result.output_height = 1U;
    result.output_pixel_stride_bytes = 4U;
    result.output_bytes = 4U;
    result.output_buffer_bytes = 4U;
    result.output_readback_bytes = 4U;
    result.trace_dispatch_issued = true;
    result.trace_dispatch_submitted = true;
    result.trace_dispatch_completed = true;
    result.output_readback_completed = true;
    result.timestamp_query_created = true;
    result.timestamp_queries_issued = true;
    result.timestamp_data_resolved = true;
    result.gpu_timestamp_frequency_hz = 1'000'000'000ULL;
    result.gpu_timestamp_ticks_begin = 100U;
    result.gpu_timestamp_ticks_end = 200U;
    result.gpu_timestamp_ticks_delta = 100U;
    result.gpu_timestamp_duration_ns = 100U;
    result.gpu_timestamp_readback_bytes = 16U;
    result.gpu_timestamps_valid = true;
    return result;
}

NativeRayTracingExecutionTraceEvidence complete_trace_evidence() {
    NativeRayTracingExecutionTraceEvidence result;
    result.build_only = false;
    result.blas_flags_observed = true;
    result.tlas_flags_observed = true;
    result.blas_allow_update = true;
    result.tlas_allow_update = true;
    result.blas_prefer_fast_trace = true;
    result.tlas_prefer_fast_trace = true;
    result.blas_build_count = 1U;
    result.tlas_build_count = 1U;
    result.sbt_ready = true;
    result.sbt_bytes = 256U;
    result.output_ready = true;
    result.output_bytes = 64U * 64U * 16U;
    result.trace_submitted = true;
    result.trace_completed = true;
    result.readback_completed = true;
    result.gpu_timestamps_valid = true;
    result.synchronization = RayTracingExecutionSynchronizationFacts{
        .acceleration_structure_barrier_submitted = true,
        .build_to_trace_sync_submitted = true,
        .barrier_validation_passed = true,
        .queue_ownership_transfer_required = false,
        .queue_ownership_transfer_completed = false,
    };
    result.allocations_completed = true;
    result.resource_addresses_valid = true;
    result.ray_tracing_pipeline_supported = true;
    return result;
}

bool has_code(const RayTracingExecutionReceipt& receipt,
              const std::string_view code) {
    return std::find(receipt.error_codes.begin(), receipt.error_codes.end(),
                     code) != receipt.error_codes.end();
}

bool test_build_only_is_valid_but_not_ready() {
    const auto d3d12 = adapt_native_d3d12_raytracing_receipt(
        d3d12_build_receipt());
    const auto vulkan = adapt_native_vulkan_raytracing_receipt(
        vulkan_build_receipt());
    const auto d3d12_validation = validate_raytracing_execution_receipt(d3d12);
    const auto vulkan_validation = validate_raytracing_execution_receipt(vulkan);

    if (!check(d3d12_validation.valid && !d3d12_validation.successful &&
                   d3d12.state == RayTracingExecutionState::fallback &&
                   d3d12.fallback_active,
               "D3D12 build-only receipt was not a valid fallback"))
        return false;
    if (!check(vulkan_validation.valid && !vulkan_validation.successful &&
                   vulkan.state == RayTracingExecutionState::fallback &&
                   vulkan.fallback_active,
               "Vulkan build-only receipt was not a valid fallback"))
        return false;
    if (!check(has_code(d3d12, "rt.build-only-incomplete") &&
                   has_code(d3d12, "rt.sbt-missing") &&
                   has_code(d3d12, "rt.output-missing") &&
                   has_code(d3d12, "rt.trace-not-dispatched") &&
                   has_code(d3d12, "rt.readback-not-observed") &&
                   has_code(d3d12, "rt.trace-synchronization-incomplete"),
               "D3D12 build-only receipt did not expose missing trace gates"))
        return false;
    if (!check(std::is_sorted(d3d12.error_codes.begin(),
                              d3d12.error_codes.end()) &&
                   std::is_sorted(vulkan.error_codes.begin(),
                                  vulkan.error_codes.end()),
               "adapter error codes were not sorted deterministically"))
        return false;
    return check(d3d12.resources.blas_count == 1U &&
                     d3d12.resources.tlas_count == 1U &&
                     d3d12.resources.scratch_bytes == 2048U &&
                     d3d12.resources.sbt_bytes == 0U &&
                     d3d12.resources.output_bytes == 0U &&
                     d3d12.blas.completed && d3d12.tlas.completed,
                 "native build facts were not projected conservatively");
}

bool test_missing_readback_never_becomes_ready() {
    auto native = d3d12_build_receipt();
    native.build_only = false;
    auto evidence = complete_trace_evidence();
    evidence.readback_completed = false;
    const auto receipt = adapt_native_d3d12_raytracing_receipt(native, evidence);
    const auto validation = validate_raytracing_execution_receipt(receipt);
    if (!check(validation.valid && !validation.successful &&
                   receipt.state == RayTracingExecutionState::fallback &&
                   has_code(receipt, "rt.readback-not-observed"),
               "missing readback was incorrectly reported ready"))
        return false;

    auto complete = complete_trace_evidence();
    const auto ready = adapt_native_d3d12_raytracing_receipt(native, complete);
    const auto ready_validation = validate_raytracing_execution_receipt(ready);
    return check(ready_validation.valid && ready_validation.successful &&
                     ready.state == RayTracingExecutionState::ready &&
                     ready.resources.sbt_bytes == complete.sbt_bytes &&
                     ready.resources.output_bytes == complete.output_bytes,
                 "complete explicit trace evidence did not construct ready receipt");
}

bool test_unsupported_unavailable_and_failed_mapping() {
    auto unsupported_native = d3d12_build_receipt();
    unsupported_native.state = NativeD3D12RayTracingExecutionState::unsupported;
    unsupported_native.hardware_probe_completed = true;
    unsupported_native.hardware_device_created = false;
    unsupported_native.hardware_raytracing_device_found = false;
    unsupported_native.raytracing_tier = 0U;
    unsupported_native.code = "native-d3d12-raytracing.hardware-unsupported";
    const auto unsupported = adapt_native_d3d12_raytracing_receipt(
        unsupported_native);
    const auto unsupported_validation =
        validate_raytracing_execution_receipt(unsupported);
    if (!check(unsupported_validation.valid &&
                   unsupported.state == RayTracingExecutionState::unsupported &&
                   !unsupported.fallback_active && has_code(unsupported, "rt.unsupported"),
               "unsupported native state did not map to stable unsupported"))
        return false;

    auto unavailable_native = vulkan_build_receipt();
    unavailable_native.state = NativeVulkanRayTracingExecutionState::unavailable;
    unavailable_native.device_created = false;
    unavailable_native.feature_chain_enabled = false;
    unavailable_native.code = "native-vulkan-rt.loader-unavailable";
    const auto unavailable = adapt_native_vulkan_raytracing_receipt(
        unavailable_native);
    const auto unavailable_validation =
        validate_raytracing_execution_receipt(unavailable);
    if (!check(unavailable_validation.valid &&
                   unavailable.state == RayTracingExecutionState::fallback &&
                   unavailable.fallback_active &&
                   has_code(unavailable, "rt.backend-unavailable"),
               "unavailable native state did not activate raster fallback"))
        return false;

    auto failed_native = d3d12_build_receipt();
    failed_native.state = NativeD3D12RayTracingExecutionState::failed;
    failed_native.code = "native-d3d12-raytracing.command-list-close-failed";
    const auto failed = adapt_native_d3d12_raytracing_receipt(failed_native);
    const auto failed_validation = validate_raytracing_execution_receipt(failed);
    return check(failed_validation.valid && failed.state == RayTracingExecutionState::error &&
                     failed.fallback_active && has_code(failed, "rt.execution-failed") &&
                     has_code(failed, "native-d3d12-raytracing.command-list-close-failed"),
                 "failed native state did not map to error plus fallback");
}

bool test_native_handle_boundary_and_dual_backend_aggregate() {
    auto leaked = d3d12_build_receipt();
    leaked.native_handle_exposed = true;
    const auto leaked_projection = adapt_native_d3d12_raytracing_receipt(leaked);
    const auto leaked_validation = validate_raytracing_execution_receipt(
        leaked_projection);
    if (!check(!leaked_validation.valid && leaked_projection.native_handles_exposed,
               "native handle exposure crossed the engine receipt boundary"))
        return false;

    auto d3d12_native = d3d12_build_receipt();
    d3d12_native.build_only = false;
    auto vulkan_native = vulkan_build_receipt();
    vulkan_native.build_only = false;
    auto d3d12_evidence = complete_trace_evidence();
    auto vulkan_evidence = complete_trace_evidence();
    vulkan_evidence.ray_tracing_pipeline_supported = true;
    const auto aggregate = aggregate_native_raytracing_execution_receipts(
        d3d12_native, vulkan_native, d3d12_evidence, vulkan_evidence);
    if (!check(aggregate.valid && aggregate.native_rhi_ready &&
                   aggregate.blas_tlas_runtime_ready && !aggregate.rtgi_ready &&
                   aggregate.present[0] && aggregate.present[1] &&
                   aggregate.receipts[0].backend == RayTracingExecutionBackend::d3d12 &&
                   aggregate.receipts[1].backend == RayTracingExecutionBackend::vulkan,
               "complete backend projections did not aggregate in stable order"))
        return false;

    const auto build_only_aggregate =
        aggregate_native_raytracing_execution_receipts(
            d3d12_build_receipt(), vulkan_build_receipt());
    return check(build_only_aggregate.valid &&
                     !build_only_aggregate.native_rhi_ready &&
                     !build_only_aggregate.blas_tlas_runtime_ready &&
                     !build_only_aggregate.rtgi_ready,
                 "build-only backend aggregate was reported ready");
}

bool test_native_02_receipts_are_direct_source() {
    const auto d3d12 = adapt_native_d3d12_raytracing_receipt(
        d3d12_trace_receipt());
    const auto vulkan = adapt_native_vulkan_raytracing_receipt(
        vulkan_trace_receipt());
    const auto d3d12_validation = validate_raytracing_execution_receipt(d3d12);
    const auto vulkan_validation = validate_raytracing_execution_receipt(vulkan);
    if (!check(d3d12_validation.valid && d3d12_validation.successful &&
                   d3d12.state == RayTracingExecutionState::ready &&
                   d3d12.blas.flags_observed && d3d12.tlas.flags_observed &&
                   d3d12.blas.build_count == 1U && d3d12.tlas.build_count == 1U &&
                   d3d12.resources.sbt_bytes == 192U &&
                   d3d12.resources.output_bytes == 16U &&
                   d3d12.gpu_timestamps_valid,
               "D3D12 native 0.2 trace facts were not projected directly"))
        return false;
    if (!check(vulkan_validation.valid && vulkan_validation.successful &&
                   vulkan.state == RayTracingExecutionState::ready &&
                   vulkan.blas.flags_observed && vulkan.tlas.flags_observed &&
                   vulkan.blas.build_count == 1U && vulkan.tlas.build_count == 1U &&
                   vulkan.resources.sbt_bytes == 192U &&
                   vulkan.resources.output_bytes == 4U &&
                   vulkan.gpu_timestamps_valid,
               "Vulkan native 0.2 trace facts were not projected directly"))
        return false;

    const auto aggregate = aggregate_native_raytracing_execution_receipts(
        d3d12_trace_receipt(), vulkan_trace_receipt());
    return check(aggregate.valid && aggregate.native_rhi_ready &&
                     aggregate.blas_tlas_runtime_ready && !aggregate.rtgi_ready &&
                     aggregate.present[0] && aggregate.present[1],
                 "native 0.2 receipts did not produce a ready default aggregate");
}

} // namespace

int main() {
    if (!test_build_only_is_valid_but_not_ready()) return 1;
    if (!test_missing_readback_never_becomes_ready()) return 2;
    if (!test_unsupported_unavailable_and_failed_mapping()) return 3;
    if (!test_native_handle_boundary_and_dual_backend_aggregate()) return 4;
    if (!test_native_02_receipts_are_direct_source()) return 5;
    std::cout << "native_raytracing_execution_adapter_tests: ok\n";
    return 0;
}
