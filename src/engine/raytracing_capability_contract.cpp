#include "engine/raytracing_capability_contract.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <nlohmann/json.hpp>
#include <set>
#include <utility>

namespace noemancer {
namespace {

using Json = nlohmann::json;

constexpr std::uint64_t kGeometryHeaderBytes = 64ULL * 1024ULL;
constexpr std::uint64_t kTlasHeaderBytes = 64ULL * 1024ULL;
constexpr std::uint64_t kVertexRecordBytes = 32ULL;
constexpr std::uint64_t kIndexRecordBytes = 4ULL;
constexpr std::uint64_t kPrimitiveRecordBytes = 64ULL;
constexpr std::uint64_t kBlasScratchMinimumBytes = 64ULL * 1024ULL;
constexpr std::uint64_t kTlasInstanceBytes = 128ULL;
constexpr std::uint64_t kCompactionScratchMinimumBytes = 64ULL * 1024ULL;

std::string bounded_text(const std::string_view value) {
    return std::string(value.substr(0U, raytracing_capability_max_text_bytes));
}

bool text_valid(const std::string_view value) {
    if (value.empty() || value.size() > raytracing_capability_max_text_bytes)
        return false;
    for (const auto character : value) {
        // NUL and other ASCII controls are not stable semantic identities.
        if (static_cast<unsigned char>(character) < 0x20U && character != '\t')
            return false;
    }
    return true;
}

void add_diagnostic(std::vector<RayTracingDiagnostic>& diagnostics,
                    std::string code, std::string path, std::string message) {
    if (diagnostics.size() >= raytracing_capability_max_diagnostics)
        return;
    diagnostics.push_back(RayTracingDiagnostic{
        .code = bounded_text(code),
        .path = bounded_text(path),
        .message = bounded_text(message),
    });
}

void sort_diagnostics(std::vector<RayTracingDiagnostic>& diagnostics) {
    std::sort(diagnostics.begin(), diagnostics.end(),
              [](const RayTracingDiagnostic& left,
                 const RayTracingDiagnostic& right) {
                  if (left.code != right.code) return left.code < right.code;
                  if (left.path != right.path) return left.path < right.path;
                  return left.message < right.message;
              });
}

bool checked_add(const std::uint64_t left, const std::uint64_t right,
                 std::uint64_t& output) noexcept {
    if (right > std::numeric_limits<std::uint64_t>::max() - left) return false;
    output = left + right;
    return true;
}

bool checked_mul(const std::uint64_t left, const std::uint64_t right,
                 std::uint64_t& output) noexcept {
    if (left != 0U && right > std::numeric_limits<std::uint64_t>::max() / left)
        return false;
    output = left * right;
    return true;
}

bool checked_add_product(const std::uint64_t base, const std::uint64_t count,
                         const std::uint64_t stride,
                         std::uint64_t& output) noexcept {
    std::uint64_t product = 0U;
    return checked_mul(count, stride, product) && checked_add(base, product, output);
}

std::uint64_t align_up_256(const std::uint64_t value, bool& valid) noexcept {
    constexpr std::uint64_t alignment = 256U;
    const auto remainder = value % alignment;
    if (remainder == 0U) return value;
    const auto delta = alignment - remainder;
    std::uint64_t result = 0U;
    if (!checked_add(value, delta, result)) {
        valid = false;
        return 0U;
    }
    return result;
}

bool finite_bounds(const RayTracingGeometryInput& geometry) noexcept {
    for (std::size_t index = 0U; index < geometry.bounds_min.size(); ++index) {
        if (!std::isfinite(geometry.bounds_min[index]) ||
            !std::isfinite(geometry.bounds_max[index]) ||
            std::fabs(geometry.bounds_min[index]) >
                raytracing_capability_max_world_value ||
            std::fabs(geometry.bounds_max[index]) >
                raytracing_capability_max_world_value ||
            geometry.bounds_min[index] > geometry.bounds_max[index]) {
            return false;
        }
    }
    return true;
}

bool finite_transform(const RayTracingInstanceInput& instance) noexcept {
    for (const auto value : instance.world_transform)
        if (!std::isfinite(value) ||
            std::fabs(value) > raytracing_capability_max_world_value)
            return false;
    return true;
}

bool calculate_blas_bytes(const RayTracingGeometryInput& geometry,
                          std::uint64_t& result_bytes,
                          std::uint64_t& scratch_bytes,
                          std::uint64_t& compaction_scratch_bytes) noexcept {
    bool arithmetic_valid = true;
    std::uint64_t result = kGeometryHeaderBytes;
    if (!checked_add_product(result, geometry.vertex_count, kVertexRecordBytes,
                             result) ||
        !checked_add_product(result, geometry.index_count, kIndexRecordBytes,
                             result) ||
        !checked_add_product(result, geometry.primitive_count,
                             kPrimitiveRecordBytes, result)) {
        arithmetic_valid = false;
    }
    result = align_up_256(result, arithmetic_valid);
    if (!arithmetic_valid) return false;

    result_bytes = result;
    scratch_bytes = std::max(kBlasScratchMinimumBytes, result / 2U);
    compaction_scratch_bytes =
        std::max(kCompactionScratchMinimumBytes, result / 4U);
    return true;
}

bool calculate_tlas_bytes(const std::uint64_t instance_count,
                          std::uint64_t& result_bytes,
                          std::uint64_t& scratch_bytes) noexcept {
    bool arithmetic_valid = true;
    std::uint64_t result = kTlasHeaderBytes;
    if (!checked_add_product(result, instance_count, kTlasInstanceBytes, result))
        arithmetic_valid = false;
    result = align_up_256(result, arithmetic_valid);
    if (!arithmetic_valid) return false;

    result_bytes = result;
    scratch_bytes = std::max(kBlasScratchMinimumBytes, result / 2U);
    return true;
}

Json finite_json(const float value) {
    return std::isfinite(value) ? Json(value) : Json(nullptr);
}

Json array_json(const std::array<float, 3U>& values) {
    Json result = Json::array();
    for (const auto value : values) result.push_back(finite_json(value));
    return result;
}

Json transform_json(const std::array<float, 16U>& values) {
    Json result = Json::array();
    for (const auto value : values) result.push_back(finite_json(value));
    return result;
}

Json backend_json(const RayTracingBackendFacts& backend) {
    return Json{
        {"schema", bounded_text(backend.schema)},
        {"backend", raytracing_backend_name(backend.backend)},
        {"deviceSupported", backend.device_supported},
        {"accelerationStructureSupported", backend.acceleration_structure_supported},
        {"rayTracingPipelineSupported", backend.ray_tracing_pipeline_supported},
        {"updateSupported", backend.update_supported},
        {"compactionSupported", backend.compaction_supported},
        {"computeBuildSupported", backend.compute_build_supported},
        {"computeTraceSupported", backend.compute_trace_supported},
        {"crossQueueSynchronizationSupported",
         backend.cross_queue_synchronization_supported},
        {"maxRecursionDepth", backend.max_recursion_depth},
        {"maxInstanceCount", backend.max_instance_count},
        {"maxResultBytes", backend.max_result_bytes},
    };
}

Json policy_json(const RayTracingBuildPolicy& policy) {
    return Json{
        {"schema", bounded_text(policy.schema)},
        {"allowUpdates", policy.allow_updates},
        {"compaction", raytracing_compaction_policy_name(policy.compaction)},
        {"allowRasterFallback", policy.allow_raster_fallback},
        {"rasterFallbackMode",
         raytracing_raster_fallback_mode_name(policy.raster_fallback_mode)},
    };
}

Json budget_json(const RayTracingMemoryBudget& budget) {
    return Json{
        {"scratchBytes", budget.scratch_bytes},
        {"resultBytes", budget.result_bytes},
        {"compactionScratchBytes", budget.compaction_scratch_bytes},
    };
}

Json queue_requirements_json(const RayTracingQueueRequirements& queues) {
    return Json{
        {"buildQueue", raytracing_queue_role_name(queues.build_queue)},
        {"traceQueue", raytracing_queue_role_name(queues.trace_queue)},
        {"requireAccelerationStructureBarrier",
         queues.require_acceleration_structure_barrier},
        {"requireBuildToTraceFence", queues.require_build_to_trace_fence},
    };
}

Json geometry_json(const RayTracingGeometryInput& geometry) {
    return Json{
        {"geometryId", bounded_text(geometry.geometry_id)},
        {"revision", geometry.revision},
        {"vertexCount", geometry.vertex_count},
        {"indexCount", geometry.index_count},
        {"primitiveCount", geometry.primitive_count},
        {"boundsMin", array_json(geometry.bounds_min)},
        {"boundsMax", array_json(geometry.bounds_max)},
        {"updateRequested", geometry.update_requested},
    };
}

Json instance_json(const RayTracingInstanceInput& instance) {
    return Json{
        {"instanceId", bounded_text(instance.instance_id)},
        {"geometryId", bounded_text(instance.geometry_id)},
        {"revision", instance.revision},
        {"worldTransform", transform_json(instance.world_transform)},
        {"updateRequested", instance.update_requested},
    };
}

template <typename Element, typename Compare>
std::vector<std::size_t> sorted_indices(const std::vector<Element>& values,
                                        Compare compare) {
    std::vector<std::size_t> indices;
    indices.reserve(values.size());
    for (std::size_t index = 0U; index < values.size(); ++index)
        indices.push_back(index);
    std::sort(indices.begin(), indices.end(), [&](const std::size_t left,
                                                  const std::size_t right) {
        return compare(values[left], values[right]);
    });
    return indices;
}

Json diagnostic_json(const RayTracingDiagnostic& diagnostic) {
    return Json{
        {"code", bounded_text(diagnostic.code)},
        {"path", bounded_text(diagnostic.path)},
        {"message", bounded_text(diagnostic.message)},
    };
}

Json budget_report_json(const RayTracingBudgetReport& report) {
    return Json{
        {"requiredScratchBytes", report.required_scratch_bytes},
        {"requiredResultBytes", report.required_result_bytes},
        {"requiredCompactionScratchBytes",
         report.required_compaction_scratch_bytes},
        {"availableScratchBytes", report.available_scratch_bytes},
        {"availableResultBytes", report.available_result_bytes},
        {"availableCompactionScratchBytes",
         report.available_compaction_scratch_bytes},
        {"fits", report.fits},
    };
}

Json queue_plan_json(const RayTracingQueuePlan& queues) {
    return Json{
        {"buildQueue", raytracing_queue_role_name(queues.build_queue)},
        {"traceQueue", raytracing_queue_role_name(queues.trace_queue)},
        {"requiresAccelerationStructureBarrier",
         queues.requires_acceleration_structure_barrier},
        {"requiresBuildToTraceFence", queues.requires_build_to_trace_fence},
        {"requiresQueueOwnershipTransfer",
         queues.requires_queue_ownership_transfer},
        {"supported", queues.supported},
    };
}

Json fallback_json(const RayTracingRasterFallback& fallback) {
    return Json{
        {"active", fallback.active},
        {"mode", raytracing_raster_fallback_mode_name(fallback.mode)},
        {"reason", bounded_text(fallback.reason)},
    };
}

Json blas_json(const RayTracingBlasPlan& blas) {
    return Json{
        {"geometryId", bounded_text(blas.geometry_id)},
        {"revision", blas.revision},
        {"operation", raytracing_build_operation_name(blas.operation)},
        {"compactionRequested", blas.compaction_requested},
        {"compactionPlanned", blas.compaction_planned},
        {"resultBytes", blas.result_bytes},
        {"scratchBytes", blas.scratch_bytes},
        {"compactionScratchBytes", blas.compaction_scratch_bytes},
    };
}

Json instance_plan_json(const RayTracingInstancePlan& instance) {
    return Json{
        {"instanceId", bounded_text(instance.instance_id)},
        {"geometryId", bounded_text(instance.geometry_id)},
        {"revision", instance.revision},
        {"operation", raytracing_build_operation_name(instance.operation)},
    };
}

Json tlas_json(const RayTracingTlasPlan& tlas) {
    return Json{
        {"operation", raytracing_build_operation_name(tlas.operation)},
        {"instanceCount", tlas.instance_count},
        {"resultBytes", tlas.result_bytes},
        {"scratchBytes", tlas.scratch_bytes},
    };
}

void set_first_reason(RayTracingCapabilityPlan& plan,
                      const RayTracingUnsupportedReason reason,
                      std::string code, std::string path, std::string message) {
    add_diagnostic(plan.diagnostics, std::move(code), std::move(path),
                   std::move(message));
    if (plan.unsupported_reason == RayTracingUnsupportedReason::none)
        plan.unsupported_reason = reason;
}

RayTracingUnsupportedReason reason_from_diagnostics(
    const std::vector<RayTracingDiagnostic>& diagnostics) noexcept {
    const auto has_code = [&](const std::string_view code) {
        return std::any_of(diagnostics.begin(), diagnostics.end(),
                           [&](const RayTracingDiagnostic& diagnostic) {
                               return diagnostic.code == code;
                           });
    };
    // Diagnostics are sorted for canonical evidence.  Reason selection uses
    // this explicit semantic priority instead of depending on lexical order.
    if (has_code("raytracing.schema"))
        return RayTracingUnsupportedReason::invalid_schema;
    if (has_code("raytracing.backend-enum"))
        return RayTracingUnsupportedReason::invalid_backend;
    if (has_code("raytracing.backend-facts"))
        return RayTracingUnsupportedReason::invalid_capability_facts;
    if (has_code("raytracing.policy"))
        return RayTracingUnsupportedReason::invalid_policy;
    if (has_code("raytracing.identity"))
        return RayTracingUnsupportedReason::invalid_identity;
    if (has_code("raytracing.geometry"))
        return RayTracingUnsupportedReason::invalid_geometry;
    if (has_code("raytracing.instance"))
        return RayTracingUnsupportedReason::invalid_instance;
    if (has_code("raytracing.missing-geometry"))
        return RayTracingUnsupportedReason::missing_geometry;
    if (has_code("raytracing.count"))
        return RayTracingUnsupportedReason::count_limit;
    if (has_code("raytracing.budget"))
        return RayTracingUnsupportedReason::invalid_budget;
    if (has_code("raytracing.queue"))
        return RayTracingUnsupportedReason::queue_unsupported;
    return RayTracingUnsupportedReason::invalid_capability_facts;
}

Json bounded_plan_array(const std::vector<RayTracingBlasPlan>& plans,
                        bool& truncated) {
    Json result = Json::array();
    const auto count = std::min(plans.size(), raytracing_capability_max_evidence_items);
    truncated = plans.size() > count;
    for (std::size_t index = 0U; index < count; ++index)
        result.push_back(blas_json(plans[index]));
    return result;
}

Json bounded_instance_array(const std::vector<RayTracingInstancePlan>& plans,
                            bool& truncated) {
    Json result = Json::array();
    const auto count = std::min(plans.size(), raytracing_capability_max_evidence_items);
    truncated = plans.size() > count;
    for (std::size_t index = 0U; index < count; ++index)
        result.push_back(instance_plan_json(plans[index]));
    return result;
}

std::uint64_t fnv1a(const std::string_view value) noexcept {
    std::uint64_t hash = 1469598103934665603ULL;
    for (const auto character : value) {
        hash ^= static_cast<std::uint8_t>(character);
        hash *= 1099511628211ULL;
    }
    return hash;
}

std::string hex_u64(const std::uint64_t value) {
    constexpr char digits[] = "0123456789abcdef";
    std::string result(16U, '0');
    for (std::size_t index = 0U; index < result.size(); ++index) {
        const auto shift = static_cast<unsigned int>((result.size() - 1U - index) * 4U);
        result[index] = digits[(value >> shift) & 0x0fU];
    }
    return result;
}

} // namespace

std::string_view raytracing_backend_name(const RayTracingBackend backend) noexcept {
    switch (backend) {
    case RayTracingBackend::d3d12: return "d3d12";
    case RayTracingBackend::vulkan: return "vulkan";
    }
    return "unknown";
}

std::optional<RayTracingBackend> raytracing_backend_from_string(
    const std::string_view value) noexcept {
    if (value == "d3d12") return RayTracingBackend::d3d12;
    if (value == "vulkan") return RayTracingBackend::vulkan;
    return std::nullopt;
}

bool raytracing_backend_valid(const RayTracingBackend backend) noexcept {
    return backend == RayTracingBackend::d3d12 || backend == RayTracingBackend::vulkan;
}

std::string_view raytracing_support_state_name(
    const RayTracingSupportState state) noexcept {
    switch (state) {
    case RayTracingSupportState::invalid: return "invalid";
    case RayTracingSupportState::unsupported: return "unsupported";
    case RayTracingSupportState::supported: return "supported";
    }
    return "invalid";
}

std::string_view raytracing_build_operation_name(
    const RayTracingBuildOperation operation) noexcept {
    switch (operation) {
    case RayTracingBuildOperation::build: return "build";
    case RayTracingBuildOperation::update: return "update";
    }
    return "build";
}

std::string_view raytracing_compaction_policy_name(
    const RayTracingCompactionPolicy policy) noexcept {
    switch (policy) {
    case RayTracingCompactionPolicy::disabled: return "disabled";
    case RayTracingCompactionPolicy::if_supported: return "if-supported";
    case RayTracingCompactionPolicy::required: return "required";
    }
    return "disabled";
}

std::string_view raytracing_queue_role_name(const RayTracingQueueRole role) noexcept {
    switch (role) {
    case RayTracingQueueRole::graphics: return "graphics";
    case RayTracingQueueRole::compute: return "compute";
    }
    return "graphics";
}

std::string_view raytracing_raster_fallback_mode_name(
    const RayTracingRasterFallbackMode mode) noexcept {
    switch (mode) {
    case RayTracingRasterFallbackMode::forward_pbr: return "forward-pbr";
    case RayTracingRasterFallbackMode::disabled: return "disabled";
    }
    return "disabled";
}

std::string_view raytracing_unsupported_reason_name(
    const RayTracingUnsupportedReason reason) noexcept {
    switch (reason) {
    case RayTracingUnsupportedReason::none: return "none";
    case RayTracingUnsupportedReason::invalid_schema: return "invalid-schema";
    case RayTracingUnsupportedReason::invalid_backend: return "invalid-backend";
    case RayTracingUnsupportedReason::invalid_capability_facts:
        return "invalid-capability-facts";
    case RayTracingUnsupportedReason::device_unsupported: return "device-unsupported";
    case RayTracingUnsupportedReason::acceleration_structure_unsupported:
        return "acceleration-structure-unsupported";
    case RayTracingUnsupportedReason::ray_tracing_pipeline_unsupported:
        return "ray-tracing-pipeline-unsupported";
    case RayTracingUnsupportedReason::update_unsupported: return "update-unsupported";
    case RayTracingUnsupportedReason::compaction_unsupported:
        return "compaction-unsupported";
    case RayTracingUnsupportedReason::update_compaction_conflict:
        return "update-compaction-conflict";
    case RayTracingUnsupportedReason::invalid_policy: return "invalid-policy";
    case RayTracingUnsupportedReason::invalid_identity: return "invalid-identity";
    case RayTracingUnsupportedReason::invalid_geometry: return "invalid-geometry";
    case RayTracingUnsupportedReason::invalid_instance: return "invalid-instance";
    case RayTracingUnsupportedReason::missing_geometry: return "missing-geometry";
    case RayTracingUnsupportedReason::count_limit: return "count-limit";
    case RayTracingUnsupportedReason::invalid_budget: return "invalid-budget";
    case RayTracingUnsupportedReason::budget_exceeded: return "budget-exceeded";
    case RayTracingUnsupportedReason::queue_unsupported: return "queue-unsupported";
    case RayTracingUnsupportedReason::synchronization_unsupported:
        return "synchronization-unsupported";
    }
    return "invalid-capability-facts";
}

bool raytracing_support_state_valid(const RayTracingSupportState state) noexcept {
    return state == RayTracingSupportState::invalid ||
        state == RayTracingSupportState::unsupported ||
        state == RayTracingSupportState::supported;
}

bool raytracing_build_operation_valid(
    const RayTracingBuildOperation operation) noexcept {
    return operation == RayTracingBuildOperation::build ||
        operation == RayTracingBuildOperation::update;
}

bool raytracing_compaction_policy_valid(
    const RayTracingCompactionPolicy policy) noexcept {
    return policy == RayTracingCompactionPolicy::disabled ||
        policy == RayTracingCompactionPolicy::if_supported ||
        policy == RayTracingCompactionPolicy::required;
}

bool raytracing_queue_role_valid(const RayTracingQueueRole role) noexcept {
    return role == RayTracingQueueRole::graphics || role == RayTracingQueueRole::compute;
}

bool raytracing_raster_fallback_mode_valid(
    const RayTracingRasterFallbackMode mode) noexcept {
    return mode == RayTracingRasterFallbackMode::forward_pbr ||
        mode == RayTracingRasterFallbackMode::disabled;
}

std::vector<RayTracingDiagnostic> validate_raytracing_capability_request(
    const RayTracingCapabilityRequest& request) {
    std::vector<RayTracingDiagnostic> diagnostics;
    if (request.schema != raytracing_capability_contract_schema ||
        request.backend.schema != raytracing_capability_contract_schema ||
        request.policy.schema != raytracing_capability_contract_schema) {
        add_diagnostic(diagnostics, "raytracing.schema", "/schema",
                       "Request, backend and policy must use the RT capability contract schema.");
    }
    if (!raytracing_backend_valid(request.backend.backend)) {
        add_diagnostic(diagnostics, "raytracing.backend-enum", "/backend/backend",
                       "backend must be d3d12 or vulkan.");
    }
    const auto& backend = request.backend;
    if (backend.max_recursion_depth == 0U || backend.max_recursion_depth > 64U ||
        backend.max_instance_count == 0U ||
        backend.max_instance_count > raytracing_capability_max_instance_count ||
        backend.max_result_bytes == 0U ||
        backend.max_result_bytes > raytracing_capability_max_memory_bytes) {
        add_diagnostic(diagnostics, "raytracing.backend-facts", "/backend/limits",
                       "Backend limits must be non-zero and inside the bounded contract.");
    }
    if (!raytracing_compaction_policy_valid(request.policy.compaction) ||
        !raytracing_raster_fallback_mode_valid(request.policy.raster_fallback_mode) ||
        (request.policy.allow_raster_fallback &&
         request.policy.raster_fallback_mode == RayTracingRasterFallbackMode::disabled)) {
        add_diagnostic(diagnostics, "raytracing.policy", "/policy",
                       "Build policy contains an unknown compaction or fallback mode.");
    }
    const auto& budget = request.budget;
    if (budget.scratch_bytes == 0U || budget.result_bytes == 0U ||
        budget.compaction_scratch_bytes == 0U ||
        budget.scratch_bytes > raytracing_capability_max_memory_bytes ||
        budget.result_bytes > raytracing_capability_max_memory_bytes ||
        budget.compaction_scratch_bytes > raytracing_capability_max_memory_bytes) {
        add_diagnostic(diagnostics, "raytracing.budget", "/budget",
                       "Scratch and result reservations must be non-zero and bounded.");
    }
    if (!raytracing_queue_role_valid(request.queues.build_queue) ||
        !raytracing_queue_role_valid(request.queues.trace_queue)) {
        add_diagnostic(diagnostics, "raytracing.queue", "/queues",
                       "Build and trace queues must be graphics or compute.");
    }
    if (request.geometries.size() > raytracing_capability_max_geometry_count ||
        request.instances.size() > raytracing_capability_max_instance_count) {
        add_diagnostic(diagnostics, "raytracing.count", "/geometries|/instances",
                       "Geometry and instance counts exceed the bounded contract.");
    }

    std::set<std::string> geometry_ids;
    for (std::size_t index = 0U; index < request.geometries.size(); ++index) {
        const auto& geometry = request.geometries[index];
        const auto path = "/geometries/" + std::to_string(index);
        if (!text_valid(geometry.geometry_id) ||
            !geometry_ids.insert(geometry.geometry_id).second) {
            add_diagnostic(diagnostics, "raytracing.identity", path + "/geometryId",
                          "Geometry IDs must be unique, printable and bounded.");
        }
        if (geometry.vertex_count == 0U ||
            geometry.vertex_count > raytracing_capability_max_geometry_vertices ||
            geometry.index_count > raytracing_capability_max_geometry_indices ||
            geometry.primitive_count == 0U ||
            geometry.primitive_count > raytracing_capability_max_geometry_primitives ||
            !finite_bounds(geometry)) {
            add_diagnostic(diagnostics, "raytracing.geometry", path,
                          "Geometry counts and finite ordered bounds are required.");
        }
    }

    std::set<std::string> instance_ids;
    std::set<std::string> known_geometry_ids;
    for (const auto& geometry : request.geometries)
        known_geometry_ids.insert(geometry.geometry_id);
    for (std::size_t index = 0U; index < request.instances.size(); ++index) {
        const auto& instance = request.instances[index];
        const auto path = "/instances/" + std::to_string(index);
        if (!text_valid(instance.instance_id) ||
            !instance_ids.insert(instance.instance_id).second) {
            add_diagnostic(diagnostics, "raytracing.identity", path + "/instanceId",
                          "Instance IDs must be unique, printable and bounded.");
        }
        if (!text_valid(instance.geometry_id) ||
            known_geometry_ids.find(instance.geometry_id) == known_geometry_ids.end()) {
            add_diagnostic(diagnostics, "raytracing.missing-geometry",
                          path + "/geometryId",
                          "Each instance must reference one declared geometry ID.");
        }
        if (!finite_transform(instance)) {
            add_diagnostic(diagnostics, "raytracing.instance", path + "/worldTransform",
                          "Instance transforms must contain only finite values.");
        }
    }
    sort_diagnostics(diagnostics);
    return diagnostics;
}

RayTracingCapabilityPlan evaluate_raytracing_capability_contract(
    const RayTracingCapabilityRequest& request) {
    RayTracingCapabilityPlan plan;
    plan.schema = std::string(raytracing_capability_contract_schema);
    plan.backend = request.backend.backend;
    plan.backend_facts = request.backend;
    plan.policy = request.policy;
    plan.budget = request.budget;
    plan.queues.build_queue = request.queues.build_queue;
    plan.queues.trace_queue = request.queues.trace_queue;
    plan.queues.requires_acceleration_structure_barrier =
        request.queues.require_acceleration_structure_barrier;
    plan.queues.requires_build_to_trace_fence =
        request.queues.require_build_to_trace_fence ||
        request.queues.build_queue != request.queues.trace_queue;
    plan.queues.requires_queue_ownership_transfer =
        request.queues.build_queue != request.queues.trace_queue;
    plan.raster_fallback.mode = request.policy.raster_fallback_mode;

    const auto validation = validate_raytracing_capability_request(request);
    plan.diagnostics = validation;
    if (!validation.empty()) {
        plan.state = RayTracingSupportState::invalid;
        plan.valid = false;
        plan.supported = false;
        plan.unsupported_reason = reason_from_diagnostics(validation);
        plan.raster_fallback_active = request.policy.allow_raster_fallback;
        plan.raster_fallback.active = plan.raster_fallback_active;
        plan.raster_fallback.reason = "invalid-input";
        return plan;
    }

    plan.valid = true;
    // Device-level capability failures are more fundamental than queue
    // routing details, so they own the primary reason when both are absent.
    const auto& backend = request.backend;
    if (!backend.device_supported) {
        set_first_reason(plan, RayTracingUnsupportedReason::device_unsupported,
                         "raytracing.device-unsupported", "/backend/deviceSupported",
                         "The selected backend device did not advertise ray-tracing support.");
    } else if (!backend.acceleration_structure_supported) {
        set_first_reason(plan,
                         RayTracingUnsupportedReason::acceleration_structure_unsupported,
                         "raytracing.acceleration-structure-unsupported",
                         "/backend/accelerationStructureSupported",
                         "Acceleration-structure build support is unavailable.");
    } else if (!backend.ray_tracing_pipeline_supported) {
        set_first_reason(plan,
                         RayTracingUnsupportedReason::ray_tracing_pipeline_unsupported,
                         "raytracing.pipeline-unsupported",
                         "/backend/rayTracingPipelineSupported",
                         "The backend cannot dispatch the ray-tracing pipeline required by this foundation.");
    }
    plan.queues.supported = true;
    if (request.instances.size() > backend.max_instance_count) {
        set_first_reason(plan, RayTracingUnsupportedReason::count_limit,
                         "raytracing.instance-count-limit", "/instances",
                         "The instance set exceeds the selected backend maxInstanceCount fact.");
    }
    if (request.queues.build_queue == RayTracingQueueRole::compute &&
        !backend.compute_build_supported) {
        plan.queues.supported = false;
        set_first_reason(plan, RayTracingUnsupportedReason::queue_unsupported,
                         "raytracing.queue-build-unsupported", "/queues/buildQueue",
                         "The selected backend does not support compute acceleration-structure builds.");
    }
    if (request.queues.trace_queue == RayTracingQueueRole::compute &&
        !backend.compute_trace_supported) {
        plan.queues.supported = false;
        set_first_reason(plan, RayTracingUnsupportedReason::queue_unsupported,
                         "raytracing.queue-trace-unsupported", "/queues/traceQueue",
                         "The selected backend does not support compute ray-tracing dispatch.");
    }
    if (plan.queues.requires_queue_ownership_transfer &&
        !backend.cross_queue_synchronization_supported) {
        plan.queues.supported = false;
        set_first_reason(plan, RayTracingUnsupportedReason::synchronization_unsupported,
                         "raytracing.cross-queue-sync-unsupported", "/queues",
                         "Separate build and trace queues require a backend cross-queue synchronization contract.");
    }

    const auto geometry_indices = sorted_indices(
        request.geometries,
        [](const RayTracingGeometryInput& left, const RayTracingGeometryInput& right) {
            if (left.geometry_id != right.geometry_id)
                return left.geometry_id < right.geometry_id;
            return left.revision < right.revision;
        });
    for (const auto index : geometry_indices) {
        const auto& geometry = request.geometries[index];
        RayTracingBlasPlan blas;
        blas.geometry_id = geometry.geometry_id;
        blas.revision = geometry.revision;
        blas.operation = geometry.update_requested
            ? RayTracingBuildOperation::update
            : RayTracingBuildOperation::build;
        blas.compaction_requested = request.policy.compaction !=
            RayTracingCompactionPolicy::disabled;
        blas.compaction_planned = blas.compaction_requested &&
            blas.operation == RayTracingBuildOperation::build &&
            backend.compaction_supported;
        if (!calculate_blas_bytes(geometry, blas.result_bytes, blas.scratch_bytes,
                                  blas.compaction_scratch_bytes)) {
            set_first_reason(plan, RayTracingUnsupportedReason::invalid_geometry,
                             "raytracing.geometry-size-overflow", "/geometries",
                             "Geometry size calculation overflowed the bounded integer contract.");
        }
        if (blas.operation == RayTracingBuildOperation::update &&
            (!request.policy.allow_updates || !backend.update_supported)) {
            set_first_reason(plan, RayTracingUnsupportedReason::update_unsupported,
                             "raytracing.update-unsupported", "/policy/allowUpdates",
                             "A requested BLAS update is not supported by policy or backend facts.");
        }
        if (blas.operation == RayTracingBuildOperation::update &&
            request.policy.compaction == RayTracingCompactionPolicy::required) {
            set_first_reason(plan,
                             RayTracingUnsupportedReason::update_compaction_conflict,
                             "raytracing.update-compaction-conflict", "/policy/compaction",
                             "A compacted BLAS cannot be updated under this bounded plan.");
            blas.compaction_planned = false;
        }
        plan.blas.push_back(std::move(blas));
    }

    const auto instance_indices = sorted_indices(
        request.instances,
        [](const RayTracingInstanceInput& left, const RayTracingInstanceInput& right) {
            if (left.instance_id != right.instance_id)
                return left.instance_id < right.instance_id;
            if (left.geometry_id != right.geometry_id)
                return left.geometry_id < right.geometry_id;
            return left.revision < right.revision;
        });
    bool any_instance_update = false;
    for (const auto index : instance_indices) {
        const auto& instance = request.instances[index];
        RayTracingInstancePlan output;
        output.instance_id = instance.instance_id;
        output.geometry_id = instance.geometry_id;
        output.revision = instance.revision;
        output.operation = instance.update_requested
            ? RayTracingBuildOperation::update
            : RayTracingBuildOperation::build;
        any_instance_update = any_instance_update || instance.update_requested;
        if (output.operation == RayTracingBuildOperation::update &&
            (!request.policy.allow_updates || !backend.update_supported)) {
            set_first_reason(plan, RayTracingUnsupportedReason::update_unsupported,
                             "raytracing.update-unsupported", "/policy/allowUpdates",
                             "A requested TLAS instance update is not supported by policy or backend facts.");
        }
        plan.instances.push_back(std::move(output));
    }

    plan.tlas.operation = any_instance_update
        ? RayTracingBuildOperation::update
        : RayTracingBuildOperation::build;
    plan.tlas.instance_count = static_cast<std::uint32_t>(request.instances.size());
    if (!calculate_tlas_bytes(request.instances.size(), plan.tlas.result_bytes,
                              plan.tlas.scratch_bytes)) {
        set_first_reason(plan, RayTracingUnsupportedReason::invalid_instance,
                         "raytracing.tlas-size-overflow", "/instances",
                         "TLAS size calculation overflowed the bounded integer contract.");
    }

    std::uint64_t result_bytes = plan.tlas.result_bytes;
    std::uint64_t required_scratch = plan.tlas.scratch_bytes;
    std::uint64_t compaction_scratch = 0U;
    for (const auto& blas : plan.blas) {
        if (!checked_add(result_bytes, blas.result_bytes, result_bytes) ||
            !checked_add(compaction_scratch, blas.compaction_planned
                             ? blas.compaction_scratch_bytes : 0U,
                         compaction_scratch)) {
            set_first_reason(plan, RayTracingUnsupportedReason::budget_exceeded,
                             "raytracing.budget-arithmetic-overflow", "/budget",
                             "Required acceleration-structure bytes exceeded bounded arithmetic.");
        }
        required_scratch = std::max(required_scratch, blas.scratch_bytes);
    }
    plan.budget_report.required_result_bytes = result_bytes;
    plan.budget_report.required_scratch_bytes = required_scratch;
    plan.budget_report.required_compaction_scratch_bytes = compaction_scratch;
    plan.budget_report.available_scratch_bytes = request.budget.scratch_bytes;
    plan.budget_report.available_result_bytes = request.budget.result_bytes;
    plan.budget_report.available_compaction_scratch_bytes =
        request.budget.compaction_scratch_bytes;
    plan.budget_report.fits =
        result_bytes <= request.budget.result_bytes &&
        required_scratch <= request.budget.scratch_bytes &&
        compaction_scratch <= request.budget.compaction_scratch_bytes &&
        result_bytes <= backend.max_result_bytes;
    if (!plan.budget_report.fits) {
        set_first_reason(plan, RayTracingUnsupportedReason::budget_exceeded,
                         "raytracing.budget-exceeded", "/budget",
                         "Conservative BLAS/TLAS scratch or result requirements exceed the reservation or backend limit.");
    }
    if (request.policy.compaction == RayTracingCompactionPolicy::required &&
        !backend.compaction_supported) {
        set_first_reason(plan, RayTracingUnsupportedReason::compaction_unsupported,
                         "raytracing.compaction-unsupported", "/backend/compactionSupported",
                         "Compaction was required but the backend did not advertise it.");
    }

    plan.supported = plan.unsupported_reason == RayTracingUnsupportedReason::none;
    plan.state = plan.supported ? RayTracingSupportState::supported
                                : RayTracingSupportState::unsupported;
    plan.raster_fallback_active = !plan.supported && request.policy.allow_raster_fallback;
    plan.raster_fallback.active = plan.raster_fallback_active;
    plan.raster_fallback.reason = plan.supported
        ? "capability-ready-no-runtime-claim"
        : std::string(raytracing_unsupported_reason_name(plan.unsupported_reason));
    sort_diagnostics(plan.diagnostics);
    return plan;
}

std::string raytracing_capability_canonical_request(
    const RayTracingCapabilityRequest& request) {
    Json result{
        {"schema", bounded_text(request.schema)},
        {"backend", backend_json(request.backend)},
        {"policy", policy_json(request.policy)},
        {"budget", budget_json(request.budget)},
        {"queues", queue_requirements_json(request.queues)},
    };
    Json geometries = Json::array();
    const auto geometry_indices = sorted_indices(
        request.geometries,
        [](const RayTracingGeometryInput& left, const RayTracingGeometryInput& right) {
            if (left.geometry_id != right.geometry_id)
                return left.geometry_id < right.geometry_id;
            return left.revision < right.revision;
        });
    const auto geometry_count = std::min(
        geometry_indices.size(), raytracing_capability_max_evidence_items);
    for (std::size_t index = 0U; index < geometry_count; ++index)
        geometries.push_back(geometry_json(request.geometries[geometry_indices[index]]));
    Json instances = Json::array();
    const auto instance_indices = sorted_indices(
        request.instances,
        [](const RayTracingInstanceInput& left, const RayTracingInstanceInput& right) {
            if (left.instance_id != right.instance_id)
                return left.instance_id < right.instance_id;
            return left.geometry_id < right.geometry_id;
        });
    const auto instance_count = std::min(
        instance_indices.size(), raytracing_capability_max_evidence_items);
    for (std::size_t index = 0U; index < instance_count; ++index)
        instances.push_back(instance_json(request.instances[instance_indices[index]]));
    result["geometries"] = std::move(geometries);
    result["instances"] = std::move(instances);
    result["geometriesTruncated"] = geometry_indices.size() > geometry_count;
    result["instancesTruncated"] = instance_indices.size() > instance_count;
    return result.dump();
}

std::string raytracing_capability_canonical_evidence(
    const RayTracingCapabilityPlan& plan) {
    bool blas_truncated = false;
    bool instances_truncated = false;
    Json blas = bounded_plan_array(plan.blas, blas_truncated);
    Json instances = bounded_instance_array(plan.instances, instances_truncated);
    Json diagnostics = Json::array();
    const auto diagnostic_count = std::min(
        plan.diagnostics.size(), raytracing_capability_max_diagnostics);
    for (std::size_t index = 0U; index < diagnostic_count; ++index)
        diagnostics.push_back(diagnostic_json(plan.diagnostics[index]));
    Json result{
        {"schema", bounded_text(plan.schema)},
        {"contract", "capability-and-build-policy-only"},
        {"backend", raytracing_backend_name(plan.backend)},
        {"backendFacts", backend_json(plan.backend_facts)},
        {"state", raytracing_support_state_name(plan.state)},
        {"valid", plan.valid},
        {"supported", plan.supported},
        {"unsupportedReason",
         raytracing_unsupported_reason_name(plan.unsupported_reason)},
        {"policy", policy_json(plan.policy)},
        {"budget", budget_json(plan.budget)},
        {"budgetReport", budget_report_json(plan.budget_report)},
        {"queues", queue_plan_json(plan.queues)},
        {"rasterFallback", fallback_json(plan.raster_fallback)},
        {"blas", std::move(blas)},
        {"blasTruncated", blas_truncated},
        {"instances", std::move(instances)},
        {"instancesTruncated", instances_truncated},
        {"tlas", tlas_json(plan.tlas)},
        {"diagnostics", std::move(diagnostics)},
    };
    return result.dump();
}

std::string raytracing_capability_fingerprint(
    const RayTracingCapabilityPlan& plan) {
    return hex_u64(fnv1a(raytracing_capability_canonical_evidence(plan)));
}

} // namespace noemancer
