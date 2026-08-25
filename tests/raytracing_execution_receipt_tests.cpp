#include "engine/raytracing_execution_receipt.hpp"

#include <algorithm>
#include <iostream>
#include <limits>
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace noemancer;

bool check(const bool condition, const std::string_view message) {
    if (!condition)
        std::cerr << "raytracing_execution_receipt_tests: " << message << '\n';
    return condition;
}
RayTracingExecutionReceipt ready_receipt(
    const RayTracingExecutionBackend backend) {
    RayTracingExecutionReceipt result;
    result.backend = backend;
    result.state = RayTracingExecutionState::ready;
    result.adapter_identity = backend == RayTracingExecutionBackend::d3d12
                                  ? "adapter.d3d12.test"
                                  : "adapter.vulkan.test";
    result.device_supported = true;
    result.acceleration_structure_supported = true;
    result.ray_tracing_pipeline_supported = true;

    result.blas = RayTracingExecutionBuildFacts{
        .submitted = true,
        .completed = true,
        .flags_observed = true,
        .allow_update = true,
        .allow_compaction = true,
        .prefer_fast_trace = true,
        .prefer_fast_build = false,
        .build_count = 1U,
        .update_count = 0U,
    };
    result.tlas = RayTracingExecutionBuildFacts{
        .submitted = true,
        .completed = true,
        .flags_observed = true,
        .allow_update = true,
        .allow_compaction = true,
        .prefer_fast_trace = true,
        .prefer_fast_build = false,
        .build_count = 1U,
        .update_count = 0U,
    };
    result.synchronization = RayTracingExecutionSynchronizationFacts{
        .acceleration_structure_barrier_submitted = true,
        .build_to_trace_sync_submitted = true,
        .barrier_validation_passed = true,
        .queue_ownership_transfer_required = true,
        .queue_ownership_transfer_completed = true,
    };
    result.resources = RayTracingExecutionResourceFacts{
        .blas_count = 1U,
        .tlas_count = 1U,
        .blas_result_bytes = 64U * 1024U,
        .tlas_result_bytes = 64U * 1024U,
        .scratch_bytes = 128U * 1024U,
        .sbt_bytes = 256U,
        .output_bytes = 64U * 64U * 16U,
        .allocations_completed = true,
        .resource_addresses_valid = true,
    };
    result.trace_submitted = true;
    result.trace_completed = true;
    result.gpu_timestamps_valid = true;
    return result;
}

bool test_dual_backend_success() {
    const auto d3d12 = ready_receipt(RayTracingExecutionBackend::d3d12);
    const auto vulkan = ready_receipt(RayTracingExecutionBackend::vulkan);
    const auto d3d12_validation = validate_raytracing_execution_receipt(d3d12);
    const auto vulkan_validation = validate_raytracing_execution_receipt(vulkan);
    if (!check(d3d12_validation.valid && d3d12_validation.successful &&
                   vulkan_validation.valid && vulkan_validation.successful,
               "complete backend receipts were rejected"))
        return false;

    const auto aggregate =
        aggregate_raytracing_execution_receipts({d3d12, vulkan});
    if (!check(aggregate.valid && aggregate.native_rhi_ready &&
                   aggregate.blas_tlas_runtime_ready && !aggregate.rtgi_ready,
               "successful D3D12+Vulkan receipts did not aggregate to ready"))
        return false;
    if (!check(aggregate.present[0] && aggregate.present[1] &&
                   aggregate.receipts[0].backend == RayTracingExecutionBackend::d3d12 &&
                   aggregate.receipts[1].backend == RayTracingExecutionBackend::vulkan,
               "aggregate did not normalize backend order"))
        return false;

    const auto json = nlohmann::json::parse(
        raytracing_execution_aggregate_canonical_json(aggregate));
    return check(json.at("nativeRhiReady") == true &&
                     json.at("blasTlasRuntimeReady") == true &&
                     json.at("rtgiReady") == false &&
                     json.at("backends").size() == 2U,
                 "success aggregate JSON omitted readiness contract");
}

bool test_unsupported_and_fallback_are_not_ready() {
    auto unsupported = ready_receipt(RayTracingExecutionBackend::d3d12);
    unsupported.state = RayTracingExecutionState::unsupported;
    unsupported.device_supported = false;
    unsupported.acceleration_structure_supported = false;
    unsupported.ray_tracing_pipeline_supported = false;
    unsupported.blas = {};
    unsupported.tlas = {};
    unsupported.synchronization = {};
    unsupported.resources = {};
    unsupported.trace_submitted = false;
    unsupported.trace_completed = false;
    unsupported.gpu_timestamps_valid = false;
    unsupported.error_codes = {"device-unsupported"};

    auto fallback = ready_receipt(RayTracingExecutionBackend::vulkan);
    fallback.state = RayTracingExecutionState::fallback;
    fallback.fallback_active = true;
    fallback.fallback_reason = "rt.pipeline.validation-failed";
    fallback.blas = {};
    fallback.tlas = {};
    fallback.synchronization = {};
    fallback.resources = {};
    fallback.trace_submitted = false;
    fallback.trace_completed = false;
    fallback.gpu_timestamps_valid = false;

    const auto unsupported_validation =
        validate_raytracing_execution_receipt(unsupported);
    const auto fallback_validation =
        validate_raytracing_execution_receipt(fallback);
    if (!check(unsupported_validation.valid && !unsupported_validation.successful &&
                   fallback_validation.valid && !fallback_validation.successful,
               "unsupported/fallback facts were treated as malformed"))
        return false;

    const auto aggregate =
        aggregate_raytracing_execution_receipts({unsupported, fallback});
    return check(aggregate.valid && !aggregate.native_rhi_ready &&
                     !aggregate.blas_tlas_runtime_ready && !aggregate.rtgi_ready,
                 "unsupported/fallback backend was reported ready");
}

bool test_fabricated_missing_and_bounded_facts_fail_closed() {
    auto missing_flags = ready_receipt(RayTracingExecutionBackend::d3d12);
    missing_flags.blas.flags_observed = false;
    const auto flags_validation =
        validate_raytracing_execution_receipt(missing_flags);
    if (!check(!flags_validation.valid && !flags_validation.successful,
               "missing real BLAS flags passed validation"))
        return false;

    auto exposed_handle = ready_receipt(RayTracingExecutionBackend::vulkan);
    exposed_handle.native_handles_exposed = true;
    const auto handle_validation =
        validate_raytracing_execution_receipt(exposed_handle);
    if (!check(!handle_validation.valid,
               "native handle exposure passed the engine-neutral boundary"))
        return false;

    auto oversized = ready_receipt(RayTracingExecutionBackend::d3d12);
    oversized.resources.scratch_bytes =
        raytracing_execution_max_resource_bytes + 1U;
    const auto oversized_validation =
        validate_raytracing_execution_receipt(oversized);
    if (!check(!oversized_validation.valid,
               "oversized resource bytes passed bounded validation"))
        return false;

    auto long_text = ready_receipt(RayTracingExecutionBackend::vulkan);
    long_text.adapter_identity.assign(raytracing_execution_max_text_bytes + 1U,
                                      'x');
    const auto long_text_validation =
        validate_raytracing_execution_receipt(long_text);
    if (!check(!long_text_validation.valid,
               "oversized adapter identity passed validation"))
        return false;

    auto error = ready_receipt(RayTracingExecutionBackend::d3d12);
    error.error_codes = {"unexpected-native-error"};
    const auto error_validation = validate_raytracing_execution_receipt(error);
    if (!check(!error_validation.valid,
               "ready receipt with errors passed validation"))
        return false;

    const auto missing_backend = aggregate_raytracing_execution_receipts(
        {ready_receipt(RayTracingExecutionBackend::d3d12)});
    return check(!missing_backend.valid && !missing_backend.native_rhi_ready &&
                     !missing_backend.blas_tlas_runtime_ready,
                 "missing backend did not fail aggregate closed");
}

bool test_order_stability_and_explicit_rtgi_false() {
    auto d3d12 = ready_receipt(RayTracingExecutionBackend::d3d12);
    auto vulkan = ready_receipt(RayTracingExecutionBackend::vulkan);
    d3d12.error_codes.clear();
    vulkan.error_codes.clear();
    const auto first = aggregate_raytracing_execution_receipts({d3d12, vulkan});
    const auto second = aggregate_raytracing_execution_receipts({vulkan, d3d12});
    if (!check(raytracing_execution_aggregate_canonical_json(first) ==
                   raytracing_execution_aggregate_canonical_json(second) &&
                   raytracing_execution_aggregate_fingerprint(first) ==
                       raytracing_execution_aggregate_fingerprint(second),
               "receipt input order changed canonical aggregate"))
        return false;

    const auto first_json = nlohmann::json::parse(
        raytracing_execution_aggregate_canonical_json(first));
    return check(first_json.at("rtgiReady") == false &&
                     first_json.at("diagnostics").empty(),
                 "successful aggregate did not make the RTGI non-claim explicit");
}

} // namespace

int main() {
    if (!test_dual_backend_success()) return 1;
    if (!test_unsupported_and_fallback_are_not_ready()) return 2;
    if (!test_fabricated_missing_and_bounded_facts_fail_closed()) return 3;
    if (!test_order_stability_and_explicit_rtgi_false()) return 4;
    std::cout << "raytracing_execution_receipt_tests: ok\n";
    return 0;
}
