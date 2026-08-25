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
                       receipt.submitted && receipt.fence_signaled && receipt.resources_released,
                   "completed receipt omitted real execution evidence")) return false;
        if (!check(receipt.vertex_buffer_bytes > 0U && receipt.instance_buffer_bytes > 0U &&
                       receipt.blas_result_bytes > 0U && receipt.blas_scratch_bytes > 0U &&
                       receipt.tlas_result_bytes > 0U && receipt.tlas_scratch_bytes > 0U &&
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
              << " code=" << receipt.code << " device=" << receipt.device_name << '\n';
    return 0;
}
