#include "engine/raytracing_capability_contract.hpp"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>
#include <string>
#include <string_view>

namespace {

using namespace noemancer;

bool check(const bool condition, const std::string_view message) {
    if (!condition)
        std::cerr << "raytracing_capability_contract_tests: " << message << '\n';
    return condition;
}

RayTracingBackendFacts supported_backend() {
    return RayTracingBackendFacts{
        .schema = std::string(raytracing_capability_contract_schema),
        .backend = RayTracingBackend::d3d12,
        .device_supported = true,
        .acceleration_structure_supported = true,
        .ray_tracing_pipeline_supported = true,
        .update_supported = true,
        .compaction_supported = true,
        .compute_build_supported = true,
        .compute_trace_supported = true,
        .cross_queue_synchronization_supported = true,
        .max_recursion_depth = 8U,
        .max_instance_count = 4096U,
        .max_result_bytes = 1ULL << 30U,
    };
}

RayTracingGeometryInput geometry(const std::string& id,
                                 const std::uint64_t revision = 1U) {
    return RayTracingGeometryInput{
        .geometry_id = id,
        .revision = revision,
        .vertex_count = 128U,
        .index_count = 192U,
        .primitive_count = 64U,
        .bounds_min = {-1.0F, -2.0F, -3.0F},
        .bounds_max = {1.0F, 2.0F, 3.0F},
        .update_requested = false,
    };
}

RayTracingInstanceInput instance(const std::string& id,
                                 const std::string& geometry_id,
                                 const bool update = false) {
    auto result = RayTracingInstanceInput{
        .instance_id = id,
        .geometry_id = geometry_id,
        .revision = 2U,
        .world_transform = {},
        .update_requested = update,
    };
    result.world_transform[0] = 1.0F;
    result.world_transform[5] = 1.0F;
    result.world_transform[10] = 1.0F;
    result.world_transform[15] = 1.0F;
    return result;
}

RayTracingCapabilityRequest complete_request() {
    RayTracingCapabilityRequest request;
    request.backend = supported_backend();
    request.policy.compaction = RayTracingCompactionPolicy::if_supported;
    request.budget = RayTracingMemoryBudget{
        .scratch_bytes = 128ULL * 1024ULL * 1024ULL,
        .result_bytes = 128ULL * 1024ULL * 1024ULL,
        .compaction_scratch_bytes = 128ULL * 1024ULL * 1024ULL,
    };
    request.queues = RayTracingQueueRequirements{
        .build_queue = RayTracingQueueRole::compute,
        .trace_queue = RayTracingQueueRole::graphics,
        .require_acceleration_structure_barrier = true,
        .require_build_to_trace_fence = true,
    };
    request.geometries.push_back(geometry("geometry.z"));
    request.geometries.push_back(geometry("geometry.a"));
    request.geometries.back().update_requested = true;
    request.instances.push_back(instance("instance.z", "geometry.z"));
    request.instances.push_back(instance("instance.a", "geometry.a", true));
    return request;
}

bool test_vocabulary_and_supported_plan() {
    if (!check(raytracing_capability_contract_schema ==
                   "noemancer.raytracing-capability-contract/0.1",
               "schema drifted"))
        return false;
    if (!check(raytracing_backend_name(RayTracingBackend::d3d12) == "d3d12" &&
                   raytracing_backend_name(RayTracingBackend::vulkan) == "vulkan" &&
                   raytracing_compaction_policy_name(
                       RayTracingCompactionPolicy::if_supported) == "if-supported" &&
                   raytracing_unsupported_reason_name(
                       RayTracingUnsupportedReason::budget_exceeded) ==
                       "budget-exceeded",
               "vocabulary drifted"))
        return false;

    const auto plan = evaluate_raytracing_capability_contract(complete_request());
    if (!check(plan.valid && plan.supported &&
                   plan.state == RayTracingSupportState::supported &&
                   plan.unsupported_reason == RayTracingUnsupportedReason::none,
               "complete D3D12 capability facts did not produce a supported plan"))
        return false;
    if (!check(plan.blas.size() == 2U && plan.blas[0].geometry_id == "geometry.a" &&
                   plan.blas[1].geometry_id == "geometry.z" &&
                   plan.blas[0].operation == RayTracingBuildOperation::update &&
                   !plan.blas[0].compaction_planned &&
                   plan.blas[1].compaction_planned,
               "BLAS identity sorting or update/compaction policy was incorrect"))
        return false;
    return check(plan.instances.size() == 2U &&
                     plan.instances[0].instance_id == "instance.a" &&
                     plan.instances[1].instance_id == "instance.z" &&
                     plan.tlas.operation == RayTracingBuildOperation::update &&
                     plan.queues.requires_queue_ownership_transfer &&
                     plan.budget_report.fits,
                 "TLAS sorting, queue plan or budget report was incorrect");
}

bool test_capability_and_fallback_reasons() {
    auto unsupported = complete_request();
    unsupported.backend.device_supported = false;
    const auto device_plan = evaluate_raytracing_capability_contract(unsupported);
    if (!check(!device_plan.supported && device_plan.raster_fallback_active &&
                   device_plan.unsupported_reason ==
                       RayTracingUnsupportedReason::device_unsupported &&
                   device_plan.raster_fallback.mode ==
                       RayTracingRasterFallbackMode::forward_pbr,
               "unsupported device did not activate explicit raster fallback"))
        return false;

    auto compaction = complete_request();
    compaction.policy.compaction = RayTracingCompactionPolicy::required;
    compaction.backend.compaction_supported = false;
    compaction.geometries[0].update_requested = false;
    compaction.geometries[1].update_requested = false;
    compaction.instances[0].update_requested = false;
    compaction.instances[1].update_requested = false;
    const auto compaction_plan = evaluate_raytracing_capability_contract(compaction);
    if (!check(!compaction_plan.supported &&
                   compaction_plan.unsupported_reason ==
                       RayTracingUnsupportedReason::compaction_unsupported,
               "required compaction was silently downgraded"))
        return false;

    auto budget = complete_request();
    budget.budget.result_bytes = 1U;
    const auto budget_plan = evaluate_raytracing_capability_contract(budget);
    return check(!budget_plan.supported &&
                     budget_plan.unsupported_reason ==
                         RayTracingUnsupportedReason::budget_exceeded &&
                     !budget_plan.budget_report.fits,
                 "insufficient result budget was not rejected");
}

bool test_invalid_nonfinite_bounds_and_identities() {
    auto invalid = complete_request();
    invalid.geometries[0].bounds_min[1] =
        std::numeric_limits<float>::quiet_NaN();
    invalid.geometries[1].geometry_id = invalid.geometries[0].geometry_id;
    invalid.instances[0].geometry_id = "missing-geometry";
    invalid.instances[1].world_transform[7] =
        std::numeric_limits<float>::infinity();
    const auto diagnostics = validate_raytracing_capability_request(invalid);
    const auto plan = evaluate_raytracing_capability_contract(invalid);
    if (!check(!diagnostics.empty() && !plan.valid && !plan.supported &&
                   plan.state == RayTracingSupportState::invalid &&
                   plan.unsupported_reason == RayTracingUnsupportedReason::invalid_identity,
               "non-finite or duplicate identity input was accepted"))
        return false;

    auto invalid_backend = complete_request();
    invalid_backend.backend.backend = static_cast<RayTracingBackend>(99U);
    const auto backend_plan = evaluate_raytracing_capability_contract(invalid_backend);
    return check(!backend_plan.valid &&
                     backend_plan.unsupported_reason ==
                         RayTracingUnsupportedReason::invalid_backend,
                 "unknown backend enum was accepted");
}

bool test_queue_contract_and_deterministic_evidence() {
    auto no_sync = complete_request();
    no_sync.backend.cross_queue_synchronization_supported = false;
    const auto no_sync_plan = evaluate_raytracing_capability_contract(no_sync);
    if (!check(!no_sync_plan.supported &&
                   no_sync_plan.unsupported_reason ==
                       RayTracingUnsupportedReason::synchronization_unsupported &&
                   no_sync_plan.queues.requires_queue_ownership_transfer,
               "cross-queue synchronization failure was not explicit"))
        return false;

    auto first_request = complete_request();
    auto second_request = complete_request();
    std::reverse(second_request.geometries.begin(), second_request.geometries.end());
    std::reverse(second_request.instances.begin(), second_request.instances.end());
    const auto first = evaluate_raytracing_capability_contract(first_request);
    const auto second = evaluate_raytracing_capability_contract(second_request);
    if (!check(raytracing_capability_canonical_evidence(first) ==
                   raytracing_capability_canonical_evidence(second) &&
                   raytracing_capability_fingerprint(first) ==
                       raytracing_capability_fingerprint(second),
               "input ordering changed canonical evidence"))
        return false;

    auto changed = first_request;
    changed.backend.backend = RayTracingBackend::vulkan;
    const auto changed_plan = evaluate_raytracing_capability_contract(changed);
    if (!check(raytracing_capability_fingerprint(first) !=
                   raytracing_capability_fingerprint(changed_plan),
               "backend semantic change did not change fingerprint"))
        return false;

    auto large = complete_request();
    large.geometries.clear();
    large.instances.clear();
    for (std::uint32_t index = 0U; index < 300U; ++index)
        large.geometries.push_back(geometry("geometry." + std::to_string(index)));
    const auto large_plan = evaluate_raytracing_capability_contract(large);
    const auto evidence = raytracing_capability_canonical_evidence(large_plan);
    return check(large_plan.valid && evidence.find("blasTruncated") != std::string::npos &&
                     evidence.size() < 512U * 1024U &&
                     evidence.find("capability-and-build-policy-only") != std::string::npos &&
                     evidence.find("rtgi") == std::string::npos,
                 "canonical evidence was unbounded or claimed a lighting implementation");
}

} // namespace

int main() {
    if (!test_vocabulary_and_supported_plan()) return 1;
    if (!test_capability_and_fallback_reasons()) return 2;
    if (!test_invalid_nonfinite_bounds_and_identities()) return 3;
    if (!test_queue_contract_and_deterministic_evidence()) return 4;
    std::cout << "raytracing_capability_contract_tests: ok\n";
    return 0;
}
