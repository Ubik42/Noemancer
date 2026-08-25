#include "runtime/native_d3d12_raytracing_executor.hpp"

#include <iostream>
#include <string_view>

namespace {

using namespace noemancer;

bool check(const bool condition, const std::string_view message) {
    if (!condition)
        std::cerr << "native_d3d12_raytracing_executor_tests: " << message << '\n';
    return condition;
}

bool validate_receipt(const NativeD3D12RayTracingReceipt& receipt) {
    if (!check(receipt.schema == native_d3d12_raytracing_executor_schema,
               "receipt schema drifted"))
        return false;
    if (!check(receipt.backend == "d3d12" && !receipt.native_handle_exposed,
               "receipt exposed a non-D3D12 identity or native handle"))
        return false;
    if (!check(receipt.code.size() <= native_d3d12_raytracing_executor_max_text_bytes &&
                   receipt.detail.size() <= native_d3d12_raytracing_executor_max_text_bytes &&
                   receipt.device_name.size() <= native_d3d12_raytracing_executor_max_text_bytes,
               "receipt text exceeded the bounded contract"))
        return false;
    if (!check(!receipt.code.empty() && !receipt.detail.empty(),
               "receipt omitted diagnostic code/detail"))
        return false;
    if (!check(receipt.hardware_raytracing_device_count <=
                   receipt.hardware_adapter_count,
               "hardware RT device count exceeded hardware adapter count"))
        return false;
    if (receipt.state == NativeD3D12RayTracingExecutionState::succeeded) {
        if (!check(receipt.hardware_raytracing_device_found &&
                       receipt.hardware_device_created &&
                       receipt.hardware_adapter_count > 0U &&
                       receipt.hardware_raytracing_device_count > 0U &&
                       !receipt.warp_fallback_attempted && receipt.raytracing_tier > 0U,
                   "success was not backed by a hardware RT device"))
            return false;
        if (!check(receipt.blas_prebuild_completed && receipt.blas_build_submitted &&
                       receipt.blas_build_completed && receipt.tlas_prebuild_completed &&
                       receipt.tlas_build_submitted && receipt.tlas_build_completed &&
                       receipt.synchronization_completed &&
                       receipt.build_only && !receipt.compaction_executed &&
                       !receipt.trace_dispatch_issued &&
                       receipt.vertex_buffer_bytes > 0U &&
                       receipt.blas_scratch_bytes > 0U &&
                       receipt.blas_result_bytes > 0U &&
                       receipt.tlas_scratch_bytes > 0U &&
                       receipt.tlas_result_bytes > 0U,
                   "successful receipt omitted a completed BLAS/TLAS phase"))
            return false;
    } else {
        if (!check(receipt.state == NativeD3D12RayTracingExecutionState::unsupported ||
                       receipt.state == NativeD3D12RayTracingExecutionState::unavailable,
                   "unsupported hardware did not produce unsupported/unavailable state"))
            return false;
    }
    return true;
}

bool test_vocabulary() {
    if (!check(native_d3d12_raytracing_execution_state_name(
                   NativeD3D12RayTracingExecutionState::unavailable) == "unavailable" &&
                   native_d3d12_raytracing_execution_state_name(
                       NativeD3D12RayTracingExecutionState::unsupported) == "unsupported" &&
                   native_d3d12_raytracing_execution_state_name(
                       NativeD3D12RayTracingExecutionState::failed) == "failed" &&
                   native_d3d12_raytracing_execution_state_name(
                       NativeD3D12RayTracingExecutionState::succeeded) == "succeeded",
               "execution state vocabulary drifted"))
        return false;
    return check(native_d3d12_raytracing_failure_stage_name(
                      NativeD3D12RayTracingFailureStage::blas_build) == "blas-build" &&
                     native_d3d12_raytracing_failure_stage_name(
                         NativeD3D12RayTracingFailureStage::tlas_build) == "tlas-build" &&
                     native_d3d12_raytracing_failure_stage_name(
                         NativeD3D12RayTracingFailureStage::synchronization) ==
                         "synchronization",
                 "failure stage vocabulary drifted");
}

bool test_execution() {
    const auto receipt = run_native_d3d12_raytracing_executor(
        NativeD3D12RayTracingExecutorOptions{.probe_warp_fallback = true});
    if (!validate_receipt(receipt)) return false;
    if (receipt.state == NativeD3D12RayTracingExecutionState::succeeded) {
        std::cout << "native_d3d12_raytracing_executor_tests: hardware BLAS/TLAS ok\n";
    } else {
        std::cout << "native_d3d12_raytracing_executor_tests: unsupported (" <<
            receipt.code << ")\n";
    }
    return true;
}

} // namespace

int main() {
    if (!test_vocabulary()) return 1;
    if (!test_execution()) return 2;
    return 0;
}
