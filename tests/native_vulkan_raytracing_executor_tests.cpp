#include "runtime/native_vulkan_raytracing_executor.hpp"

#include <iostream>
#include <string_view>

namespace {

using namespace noemancer;

bool check(const bool condition, const std::string_view message) {
    if (!condition) std::cerr << "native_vulkan_raytracing_executor_tests: " << message << '\n';
    return condition;
}

bool test_vocabulary_and_bounded_receipt() {
    if (!check(native_vulkan_raytracing_execution_state_name(
                   NativeVulkanRayTracingExecutionState::unavailable) == "unavailable" &&
                   native_vulkan_raytracing_execution_state_name(
                       NativeVulkanRayTracingExecutionState::unsupported) == "unsupported" &&
                   native_vulkan_raytracing_execution_state_name(
                       NativeVulkanRayTracingExecutionState::completed) == "completed" &&
                   native_vulkan_raytracing_execution_state_name(
                       NativeVulkanRayTracingExecutionState::failed) == "failed",
               "execution state vocabulary drifted")) return false;
    if (!check(native_vulkan_raytracing_failure_stage_name(
                   NativeVulkanRayTracingFailureStage::acceleration_structure) ==
                   "acceleration-structure" &&
                   native_vulkan_raytracing_failure_stage_name(
                       NativeVulkanRayTracingFailureStage::submit) == "submit",
               "failure-stage vocabulary drifted")) return false;

    const auto receipt = execute_native_vulkan_raytracing_blas_tlas();
    if (!check(receipt.schema == native_vulkan_raytracing_executor_schema,
               "executor schema drifted")) return false;
    if (!check(receipt.code.size() <= native_vulkan_raytracing_executor_max_text_bytes &&
                   receipt.detail.size() <= native_vulkan_raytracing_executor_max_text_bytes &&
                   receipt.device_name.size() <= native_vulkan_raytracing_executor_max_text_bytes &&
                   receipt.driver_name.size() <= native_vulkan_raytracing_executor_max_text_bytes,
               "executor receipt exceeded bounded text limits")) return false;
    if (!check(!receipt.code.empty() && !receipt.detail.empty(),
               "executor did not return a diagnostic code and detail")) return false;
    if (!check(receipt.resources_released,
               "executor returned while temporary Vulkan resources remained live")) return false;
    if (!check(receipt.geometry_count == 1U && receipt.instance_count == 1U,
               "executor workload is not the required one-triangle/one-instance contract")) return false;
    return true;
}

bool test_success_or_explicit_unsupported() {
    const auto receipt = execute_native_vulkan_raytracing_blas_tlas();
    if (receipt.state == NativeVulkanRayTracingExecutionState::completed) {
        if (!check(receipt.failure_stage == NativeVulkanRayTracingFailureStage::none &&
                       receipt.loader_available && receipt.instance_created &&
                       receipt.device_created && receipt.queue_found &&
                       receipt.feature_chain_enabled && receipt.blas_built && receipt.tlas_built &&
                       receipt.submitted && receipt.fence_signaled && receipt.resources_released &&
                       receipt.shader_module_created && receipt.descriptor_set_layout_created &&
                       receipt.pipeline_layout_created && receipt.descriptor_set_allocated &&
                       receipt.pipeline_created && receipt.sbt_built &&
                       receipt.shader_table_prepared && receipt.shader_table_uploaded &&
                       receipt.trace_dispatch_issued && receipt.trace_dispatch_submitted &&
                       receipt.trace_dispatch_completed && receipt.output_resource_created &&
                       receipt.output_readback_completed && receipt.synchronization_completed &&
                       receipt.timestamp_query_created && receipt.timestamp_queries_issued &&
                       receipt.timestamp_data_resolved && receipt.gpu_timestamps_valid &&
                       !receipt.build_only,
                   "completed receipt omitted real execution evidence")) return false;
        if (!check(receipt.blas_flags_observed && receipt.tlas_flags_observed &&
                       receipt.blas_prefer_fast_trace && receipt.tlas_prefer_fast_trace &&
                       !receipt.blas_prefer_fast_build && !receipt.tlas_prefer_fast_build &&
                       !receipt.blas_allow_update && !receipt.tlas_allow_update &&
                       !receipt.blas_allow_compaction && !receipt.tlas_allow_compaction &&
                       receipt.blas_build_count == 1U && receipt.tlas_build_count == 1U &&
                       receipt.blas_update_count == 0U && receipt.tlas_update_count == 0U &&
                       receipt.blas_build_submitted && receipt.tlas_build_submitted &&
                       receipt.blas_build_completed && receipt.tlas_build_completed &&
                       !receipt.compaction_executed,
                   "completed receipt omitted observed acceleration-structure build flags"))
            return false;
        if (!check(receipt.vertex_buffer_bytes > 0U && receipt.instance_buffer_bytes > 0U &&
                       receipt.blas_result_bytes > 0U && receipt.blas_scratch_bytes > 0U &&
                       receipt.tlas_result_bytes > 0U && receipt.tlas_scratch_bytes > 0U &&
                       receipt.sbt_buffer_bytes > 0U && receipt.output_buffer_bytes > 0U &&
                       receipt.shader_group_count == 3U && receipt.trace_width == 1U &&
                       receipt.trace_height == 1U && receipt.trace_depth == 1U &&
                       receipt.shader_table_record_count == 3U &&
                       receipt.shader_table_record_bytes > 0U && receipt.shader_table_bytes > 0U &&
                       receipt.output_width == 1U && receipt.output_height == 1U &&
                       receipt.output_pixel_stride_bytes == sizeof(std::uint32_t) &&
                       receipt.output_bytes == sizeof(std::uint32_t) &&
                       receipt.output_readback_bytes == sizeof(std::uint32_t) &&
                       receipt.output_readback_bytes == receipt.output_bytes &&
                       receipt.output_value == 0x48495421U &&
                       receipt.output_hit == receipt.output_value &&
                       receipt.output_sentinel == 0x4E4F5254U &&
                       receipt.output_value != receipt.output_sentinel &&
                       receipt.output_pixel_x == 0U && receipt.output_pixel_y == 0U &&
                       receipt.output_hash != 0U &&
                       receipt.gpu_timestamp_frequency_hz > 0U &&
                       receipt.gpu_timestamp_ticks_end > receipt.gpu_timestamp_ticks_begin &&
                       receipt.gpu_timestamp_ticks_delta > 0U &&
                       receipt.gpu_timestamp_duration_ns > 0U &&
                       receipt.gpu_timestamp_readback_bytes == 2U * sizeof(std::uint64_t) &&
                       receipt.total_allocated_bytes >= receipt.vertex_buffer_bytes +
                           receipt.instance_buffer_bytes,
                   "completed receipt omitted resource allocation evidence")) return false;
    } else if (receipt.state == NativeVulkanRayTracingExecutionState::unsupported) {
        if (!check(receipt.failure_stage == NativeVulkanRayTracingFailureStage::physical_device ||
                       receipt.failure_stage == NativeVulkanRayTracingFailureStage::loader,
                   "unsupported receipt did not identify capability failure stage")) return false;
    } else if (receipt.state == NativeVulkanRayTracingExecutionState::failed) {
        if (!check(receipt.failure_stage != NativeVulkanRayTracingFailureStage::none,
                   "failed receipt omitted a concrete failure stage")) return false;
    } else if (!check(receipt.state == NativeVulkanRayTracingExecutionState::unavailable,
                      "executor returned an unknown state")) return false;
    return true;
}

} // namespace

int main() {
    if (!test_vocabulary_and_bounded_receipt()) return 1;
    if (!test_success_or_explicit_unsupported()) return 2;
    const auto receipt = execute_native_vulkan_raytracing_blas_tlas();
    std::cout << "native_vulkan_raytracing_executor_tests: ok state="
              << native_vulkan_raytracing_execution_state_name(receipt.state)
              << " code=" << receipt.code << " detail=" << receipt.detail
              << " device=" << receipt.device_name << '\n';
    return 0;
}
