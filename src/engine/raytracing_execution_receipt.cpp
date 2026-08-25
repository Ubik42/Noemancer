#include "engine/raytracing_execution_receipt.hpp"

#include <algorithm>
#include <iomanip>
#include <limits>
#include <nlohmann/json.hpp>
#include <sstream>
#include <utility>

namespace noemancer {
namespace {

using Json = nlohmann::json;

std::string bounded_text(const std::string_view value) {
    return std::string(value.substr(0U, raytracing_execution_max_text_bytes));
}

bool valid_text(const std::string_view value) noexcept {
    if (value.empty() || value.size() > raytracing_execution_max_text_bytes)
        return false;
    for (const auto character : value) {
        const auto byte = static_cast<unsigned char>(character);
        if (byte < 0x20U || byte == 0x7FU)
            return false;
    }
    return true;
}

void add_diagnostic(std::vector<RayTracingExecutionDiagnostic>& diagnostics,
                    std::string_view code, std::string_view path,
                    std::string_view message) {
    if (diagnostics.size() >= raytracing_execution_max_diagnostics)
        return;
    diagnostics.push_back(RayTracingExecutionDiagnostic{
        .code = bounded_text(code),
        .path = bounded_text(path),
        .message = bounded_text(message),
    });
}

void sort_diagnostics(std::vector<RayTracingExecutionDiagnostic>& diagnostics) {
    std::sort(diagnostics.begin(), diagnostics.end(),
              [](const RayTracingExecutionDiagnostic& left,
                 const RayTracingExecutionDiagnostic& right) {
                  if (left.code != right.code)
                      return left.code < right.code;
                  if (left.path != right.path)
                      return left.path < right.path;
                  return left.message < right.message;
              });
}

void validate_build_facts(
    const RayTracingExecutionBuildFacts& facts, const std::string_view path,
    std::vector<RayTracingExecutionDiagnostic>& diagnostics,
    const bool require_success) {
    if (facts.build_count > raytracing_execution_max_as_count)
        add_diagnostic(diagnostics, "build-count-overflow", path,
                       "BLAS/TLAS build count exceeds the bounded contract");
    if (facts.update_count > raytracing_execution_max_as_count)
        add_diagnostic(diagnostics, "update-count-overflow", path,
                       "BLAS/TLAS update count exceeds the bounded contract");
    if (facts.update_count > facts.build_count)
        add_diagnostic(diagnostics, "update-count-invalid", path,
                       "update count cannot exceed build count");
    if (facts.update_count > 0U && !facts.allow_update)
        add_diagnostic(diagnostics, "update-flag-missing", path,
                       "observed update work requires the allow-update flag");

    if (!require_success)
        return;
    if (!facts.submitted)
        add_diagnostic(diagnostics, "build-not-submitted", path,
                       "native BLAS/TLAS build was not submitted");
    if (!facts.completed)
        add_diagnostic(diagnostics, "build-not-completed", path,
                       "native BLAS/TLAS build did not complete");
    if (!facts.flags_observed)
        add_diagnostic(diagnostics, "build-flags-missing", path,
                       "native BLAS/TLAS build flags were not observed");
    if (facts.build_count == 0U)
        add_diagnostic(diagnostics, "build-count-zero", path,
                       "a successful receipt needs at least one BLAS/TLAS build");
}

void validate_resource_bytes(
    const RayTracingExecutionResourceFacts& resources,
    std::vector<RayTracingExecutionDiagnostic>& diagnostics,
    const bool require_success) {
    const auto validate_count = [&diagnostics](const std::uint32_t value,
                                                const std::string_view path) {
        if (value > raytracing_execution_max_as_count)
            add_diagnostic(diagnostics, "resource-count-overflow", path,
                           "resource count exceeds the bounded contract");
    };
    const auto validate_bytes = [&diagnostics](const std::uint64_t value,
                                               const std::string_view path) {
        if (value > raytracing_execution_max_resource_bytes)
            add_diagnostic(diagnostics, "resource-bytes-overflow", path,
                           "resource bytes exceed the bounded contract");
    };
    validate_count(resources.blas_count, "resources.blasCount");
    validate_count(resources.tlas_count, "resources.tlasCount");
    validate_bytes(resources.blas_result_bytes, "resources.blasResultBytes");
    validate_bytes(resources.tlas_result_bytes, "resources.tlasResultBytes");
    validate_bytes(resources.scratch_bytes, "resources.scratchBytes");
    validate_bytes(resources.sbt_bytes, "resources.sbtBytes");
    validate_bytes(resources.output_bytes, "resources.outputBytes");

    std::uint64_t total = 0U;
    const auto add_checked = [&diagnostics, &total](const std::uint64_t value,
                                                    const std::string_view path) {
        if (value > std::numeric_limits<std::uint64_t>::max() - total) {
            add_diagnostic(diagnostics, "resource-bytes-overflow", path,
                           "resource byte sum overflowed");
            return;
        }
        total += value;
        if (total > raytracing_execution_max_resource_bytes)
            add_diagnostic(diagnostics, "resource-budget-overflow", path,
                           "combined resource bytes exceed the bounded contract");
    };
    add_checked(resources.blas_result_bytes, "resources.blasResultBytes");
    add_checked(resources.tlas_result_bytes, "resources.tlasResultBytes");
    add_checked(resources.scratch_bytes, "resources.scratchBytes");
    add_checked(resources.sbt_bytes, "resources.sbtBytes");
    add_checked(resources.output_bytes, "resources.outputBytes");

    if (!require_success)
        return;
    if (resources.blas_count == 0U || resources.tlas_count == 0U)
        add_diagnostic(diagnostics, "resource-count-zero", "resources",
                       "a successful receipt needs BLAS and TLAS resources");
    if (resources.blas_result_bytes == 0U || resources.tlas_result_bytes == 0U ||
        resources.scratch_bytes == 0U || resources.sbt_bytes == 0U ||
        resources.output_bytes == 0U)
        add_diagnostic(diagnostics, "resource-bytes-zero", "resources",
                       "a successful receipt needs non-zero AS, scratch, SBT, and output bytes");
    if (!resources.allocations_completed)
        add_diagnostic(diagnostics, "resource-allocation-incomplete", "resources",
                       "resource allocation completion was not observed");
    if (!resources.resource_addresses_valid)
        add_diagnostic(diagnostics, "resource-address-invalid", "resources",
                       "resource address/alignment validation was not observed");
}

Json build_json(const RayTracingExecutionBuildFacts& facts) {
    return Json{
        {"submitted", facts.submitted},
        {"completed", facts.completed},
        {"flagsObserved", facts.flags_observed},
        {"allowUpdate", facts.allow_update},
        {"allowCompaction", facts.allow_compaction},
        {"preferFastTrace", facts.prefer_fast_trace},
        {"preferFastBuild", facts.prefer_fast_build},
        {"buildCount", facts.build_count},
        {"updateCount", facts.update_count},
    };
}

Json compaction_json(const RayTracingExecutionCompactionFacts& facts) {
    return Json{
        {"requested", facts.requested},
        {"sizeQueryCompleted", facts.size_query_completed},
        {"copyCompleted", facts.copy_completed},
        {"committed", facts.committed},
        {"compactedBytes", facts.compacted_bytes},
    };
}

Json synchronization_json(const RayTracingExecutionSynchronizationFacts& facts) {
    return Json{
        {"accelerationStructureBarrierSubmitted",
         facts.acceleration_structure_barrier_submitted},
        {"buildToTraceSyncSubmitted", facts.build_to_trace_sync_submitted},
        {"barrierValidationPassed", facts.barrier_validation_passed},
        {"queueOwnershipTransferRequired",
         facts.queue_ownership_transfer_required},
        {"queueOwnershipTransferCompleted",
         facts.queue_ownership_transfer_completed},
    };
}

Json resources_json(const RayTracingExecutionResourceFacts& facts) {
    return Json{
        {"blasCount", facts.blas_count},
        {"tlasCount", facts.tlas_count},
        {"blasResultBytes", facts.blas_result_bytes},
        {"tlasResultBytes", facts.tlas_result_bytes},
        {"scratchBytes", facts.scratch_bytes},
        {"sbtBytes", facts.sbt_bytes},
        {"outputBytes", facts.output_bytes},
        {"allocationsCompleted", facts.allocations_completed},
        {"resourceAddressesValid", facts.resource_addresses_valid},
    };
}

Json receipt_json(const RayTracingExecutionReceipt& receipt) {
    Json errors = Json::array();
    std::vector<std::string> sorted_errors;
    const auto error_count = std::min(receipt.error_codes.size(),
                                      raytracing_execution_max_error_codes);
    sorted_errors.reserve(error_count);
    for (std::size_t index = 0U; index < error_count; ++index)
        sorted_errors.push_back(bounded_text(receipt.error_codes[index]));
    std::sort(sorted_errors.begin(), sorted_errors.end());
    for (const auto& error : sorted_errors)
        errors.push_back(error);

    return Json{
        {"schema", bounded_text(receipt.schema)},
        {"backend", raytracing_execution_backend_name(receipt.backend)},
        {"state", raytracing_execution_state_name(receipt.state)},
        {"adapterIdentity", bounded_text(receipt.adapter_identity)},
        {"capabilities",
         Json{
             {"deviceSupported", receipt.device_supported},
             {"accelerationStructureSupported",
              receipt.acceleration_structure_supported},
             {"rayTracingPipelineSupported",
              receipt.ray_tracing_pipeline_supported},
             {"nativeHandlesExposed", receipt.native_handles_exposed},
         }},
        {"blas", build_json(receipt.blas)},
        {"tlas", build_json(receipt.tlas)},
        {"compaction", compaction_json(receipt.compaction)},
        {"synchronization", synchronization_json(receipt.synchronization)},
        {"resources", resources_json(receipt.resources)},
        {"traceSubmitted", receipt.trace_submitted},
        {"traceCompleted", receipt.trace_completed},
        {"gpuTimestampsValid", receipt.gpu_timestamps_valid},
        {"fallbackActive", receipt.fallback_active},
        {"fallbackReason", bounded_text(receipt.fallback_reason)},
        {"errorCodes", std::move(errors)},
        {"errorCodesTruncated",
         receipt.error_codes.size() > raytracing_execution_max_error_codes},
    };
}

Json diagnostic_json(const RayTracingExecutionDiagnostic& diagnostic) {
    return Json{
        {"code", bounded_text(diagnostic.code)},
        {"path", bounded_text(diagnostic.path)},
        {"message", bounded_text(diagnostic.message)},
    };
}

void append_diagnostics(std::vector<RayTracingExecutionDiagnostic>& target,
                        const std::vector<RayTracingExecutionDiagnostic>& source) {
    for (const auto& diagnostic : source) {
        if (target.size() >= raytracing_execution_max_diagnostics)
            break;
        target.push_back(diagnostic);
    }
}

std::size_t backend_index(const RayTracingExecutionBackend backend,
                          bool& valid) noexcept {
    switch (backend) {
    case RayTracingExecutionBackend::d3d12:
        valid = true;
        return 0U;
    case RayTracingExecutionBackend::vulkan:
        valid = true;
        return 1U;
    default:
        valid = false;
        return 0U;
    }
}

std::uint64_t fnv1a(const std::string_view value) noexcept {
    constexpr std::uint64_t offset = 14695981039346656037ULL;
    constexpr std::uint64_t prime = 1099511628211ULL;
    std::uint64_t hash = offset;
    for (const auto byte : value) {
        hash ^= static_cast<std::uint8_t>(byte);
        hash *= prime;
    }
    return hash;
}

std::string hex_u64(const std::uint64_t value) {
    std::ostringstream stream;
    stream << std::hex << std::setfill('0') << std::setw(16) << value;
    return stream.str();
}

} // namespace

std::string_view raytracing_execution_backend_name(
    const RayTracingExecutionBackend backend) noexcept {
    switch (backend) {
    case RayTracingExecutionBackend::d3d12:
        return "d3d12";
    case RayTracingExecutionBackend::vulkan:
        return "vulkan";
    default:
        return "invalid";
    }
}

std::string_view raytracing_execution_state_name(
    const RayTracingExecutionState state) noexcept {
    switch (state) {
    case RayTracingExecutionState::invalid:
        return "invalid";
    case RayTracingExecutionState::unsupported:
        return "unsupported";
    case RayTracingExecutionState::fallback:
        return "fallback";
    case RayTracingExecutionState::ready:
        return "ready";
    case RayTracingExecutionState::error:
        return "error";
    default:
        return "invalid";
    }
}

bool raytracing_execution_backend_valid(
    const RayTracingExecutionBackend backend) noexcept {
    bool valid = false;
    static_cast<void>(backend_index(backend, valid));
    return valid;
}

bool raytracing_execution_state_valid(
    const RayTracingExecutionState state) noexcept {
    switch (state) {
    case RayTracingExecutionState::unsupported:
    case RayTracingExecutionState::fallback:
    case RayTracingExecutionState::ready:
    case RayTracingExecutionState::error:
        return true;
    default:
        return false;
    }
}

RayTracingExecutionValidation validate_raytracing_execution_receipt(
    const RayTracingExecutionReceipt& receipt) {
    RayTracingExecutionValidation result;
    const auto fail = [&result](const std::string_view code,
                                const std::string_view path,
                                const std::string_view message) {
        add_diagnostic(result.diagnostics, code, path, message);
    };

    if (receipt.schema != raytracing_execution_receipt_schema)
        fail("schema-mismatch", "schema", "unexpected execution receipt schema");
    if (!valid_text(receipt.schema))
        fail("schema-invalid", "schema", "schema is empty, oversized, or contains controls");
    if (!raytracing_execution_backend_valid(receipt.backend))
        fail("backend-invalid", "backend", "unknown backend enum");
    if (!raytracing_execution_state_valid(receipt.state))
        fail("state-invalid", "state", "invalid execution state");
    if (!valid_text(receipt.adapter_identity))
        fail("adapter-identity-invalid", "adapterIdentity",
             "adapter identity must be a bounded non-empty string");
    if (receipt.native_handles_exposed)
        fail("native-handle-exposed", "capabilities.nativeHandlesExposed",
             "native handles cannot cross the engine-neutral receipt boundary");

    if (receipt.error_codes.size() > raytracing_execution_max_error_codes)
        fail("error-code-overflow", "errorCodes",
             "error code count exceeds the bounded contract");
    const auto error_count = std::min(receipt.error_codes.size(),
                                      raytracing_execution_max_error_codes);
    for (std::size_t index = 0U; index < error_count; ++index) {
        if (!valid_text(receipt.error_codes[index]))
            fail("error-code-invalid", "errorCodes",
                 "error code must be a bounded non-empty string");
    }
    if (receipt.fallback_reason.size() > raytracing_execution_max_text_bytes)
        fail("fallback-reason-overflow", "fallbackReason",
             "fallback reason exceeds the bounded contract");
    if (receipt.fallback_active && !valid_text(receipt.fallback_reason))
        fail("fallback-reason-missing", "fallbackReason",
             "active fallback requires a bounded reason");

    validate_build_facts(receipt.blas, "blas", result.diagnostics,
                         receipt.state == RayTracingExecutionState::ready);
    validate_build_facts(receipt.tlas, "tlas", result.diagnostics,
                         receipt.state == RayTracingExecutionState::ready);
    validate_resource_bytes(receipt.resources, result.diagnostics,
                            receipt.state == RayTracingExecutionState::ready);

    if (receipt.compaction.compacted_bytes > raytracing_execution_max_resource_bytes)
        fail("compaction-bytes-overflow", "compaction.compactedBytes",
             "compacted bytes exceed the bounded contract");
    if (receipt.compaction.committed && !receipt.compaction.requested)
        fail("compaction-without-request", "compaction.committed",
             "compaction cannot be committed without a request");
    if (receipt.compaction.requested && receipt.state == RayTracingExecutionState::ready) {
        if (!receipt.blas.allow_compaction || !receipt.tlas.allow_compaction)
            fail("compaction-flag-missing", "compaction.requested",
                 "requested compaction requires observed BLAS and TLAS compaction flags");
        if (!receipt.compaction.size_query_completed ||
            !receipt.compaction.copy_completed || !receipt.compaction.committed ||
            receipt.compaction.compacted_bytes == 0U)
            fail("compaction-incomplete", "compaction",
                 "ready compaction requires query, copy, commit, and non-zero bytes");
    }

    if (receipt.state == RayTracingExecutionState::ready) {
        if (!receipt.device_supported ||
            !receipt.acceleration_structure_supported ||
            !receipt.ray_tracing_pipeline_supported)
            fail("capability-missing", "capabilities",
                 "ready receipt requires device, acceleration structure, and pipeline support");
        if (!receipt.synchronization.acceleration_structure_barrier_submitted ||
            !receipt.synchronization.build_to_trace_sync_submitted ||
            !receipt.synchronization.barrier_validation_passed)
            fail("synchronization-incomplete", "synchronization",
                 "ready receipt requires validated AS barrier and build-to-trace synchronization");
        if (receipt.synchronization.queue_ownership_transfer_required &&
            !receipt.synchronization.queue_ownership_transfer_completed)
            fail("queue-transfer-incomplete", "synchronization",
                 "required queue ownership transfer did not complete");
        if (!receipt.trace_submitted || !receipt.trace_completed)
            fail("trace-incomplete", "trace",
                 "ready receipt requires submitted and completed trace dispatch");
        if (!receipt.gpu_timestamps_valid)
            fail("gpu-timestamp-missing", "gpuTimestampsValid",
                 "ready receipt requires a valid GPU timestamp observation");
        if (!receipt.error_codes.empty())
            fail("ready-with-errors", "errorCodes",
                 "ready receipt cannot contain backend errors");
        if (receipt.fallback_active || !receipt.fallback_reason.empty())
            fail("ready-with-fallback", "fallback",
                 "ready receipt cannot also report fallback");
    }

    if (receipt.state == RayTracingExecutionState::unsupported) {
        if (receipt.device_supported && receipt.acceleration_structure_supported &&
            receipt.ray_tracing_pipeline_supported)
            fail("unsupported-without-capability", "state",
                 "unsupported receipt must identify a missing device capability");
        if (receipt.fallback_active)
            fail("unsupported-fallback-state-mismatch", "fallbackActive",
                 "active fallback must use the fallback state");
    }
    if (receipt.state == RayTracingExecutionState::fallback &&
        !receipt.fallback_active)
        fail("fallback-state-inactive", "fallbackActive",
             "fallback state requires active fallback");
    if (receipt.state == RayTracingExecutionState::error &&
        receipt.error_codes.empty())
        fail("error-code-missing", "errorCodes",
             "error state requires an explicit backend error code");

    sort_diagnostics(result.diagnostics);
    result.valid = result.diagnostics.empty();
    result.successful = result.valid &&
                        receipt.state == RayTracingExecutionState::ready;
    return result;
}

RayTracingExecutionAggregate aggregate_raytracing_execution_receipts(
    const std::vector<RayTracingExecutionReceipt>& receipts) {
    RayTracingExecutionAggregate result;
    result.receipts[0].backend = RayTracingExecutionBackend::d3d12;
    result.receipts[1].backend = RayTracingExecutionBackend::vulkan;
    result.valid = true;

    if (receipts.size() != raytracing_execution_max_receipts) {
        result.valid = false;
        add_diagnostic(result.diagnostics, "backend-count-invalid", "receipts",
                       "exactly one D3D12 and one Vulkan receipt are required");
    }

    const auto inspected = std::min(receipts.size(),
                                    raytracing_execution_max_receipts);
    for (std::size_t input_index = 0U; input_index < inspected; ++input_index) {
        bool backend_valid = false;
        const auto slot = backend_index(receipts[input_index].backend, backend_valid);
        if (!backend_valid) {
            result.valid = false;
            add_diagnostic(result.diagnostics, "backend-invalid",
                           "receipts[" + std::to_string(input_index) + "].backend",
                           "receipt backend is not D3D12 or Vulkan");
            continue;
        }
        if (result.present[slot]) {
            result.valid = false;
            add_diagnostic(result.diagnostics, "backend-duplicate",
                           "receipts[" + std::to_string(input_index) + "].backend",
                           "each backend may appear only once");
            continue;
        }
        result.present[slot] = true;
        result.receipts[slot] = receipts[input_index];
    }

    for (std::size_t slot = 0U; slot < raytracing_execution_max_receipts; ++slot) {
        if (!result.present[slot]) {
            result.valid = false;
            add_diagnostic(result.diagnostics, "backend-missing",
                           slot == 0U ? "receipts.d3d12" : "receipts.vulkan",
                           "required backend receipt is missing");
            continue;
        }
        const auto validation =
            validate_raytracing_execution_receipt(result.receipts[slot]);
        append_diagnostics(result.diagnostics, validation.diagnostics);
        if (!validation.valid)
            result.valid = false;
    }

    bool all_successful = result.valid &&
                          result.present[0] && result.present[1];
    if (all_successful) {
        for (std::size_t slot = 0U; slot < raytracing_execution_max_receipts;
             ++slot) {
            const auto validation =
                validate_raytracing_execution_receipt(result.receipts[slot]);
            if (!validation.successful) {
                all_successful = false;
                add_diagnostic(result.diagnostics,
                               slot == 0U ? "d3d12-not-ready" : "vulkan-not-ready",
                               slot == 0U ? "receipts.d3d12.state"
                                          : "receipts.vulkan.state",
                               "backend receipt is valid but not ready");
            }
        }
    }
    result.native_rhi_ready = all_successful;
    result.blas_tlas_runtime_ready = all_successful;
    result.rtgi_ready = false;
    sort_diagnostics(result.diagnostics);
    return result;
}

std::string raytracing_execution_receipt_canonical_json(
    const RayTracingExecutionReceipt& receipt) {
    return receipt_json(receipt).dump();
}

std::string raytracing_execution_aggregate_canonical_json(
    const RayTracingExecutionAggregate& aggregate) {
    Json backends = Json::array();
    for (std::size_t slot = 0U; slot < raytracing_execution_max_receipts; ++slot) {
        const auto backend = slot == 0U ? RayTracingExecutionBackend::d3d12
                                        : RayTracingExecutionBackend::vulkan;
        backends.push_back(Json{
            {"backend", raytracing_execution_backend_name(backend)},
            {"present", aggregate.present[slot]},
            {"receipt", aggregate.present[slot]
                             ? receipt_json(aggregate.receipts[slot])
                             : Json(nullptr)},
        });
    }

    auto diagnostics = aggregate.diagnostics;
    if (diagnostics.size() > raytracing_execution_max_diagnostics)
        diagnostics.resize(raytracing_execution_max_diagnostics);
    sort_diagnostics(diagnostics);
    Json diagnostic_values = Json::array();
    for (const auto& diagnostic : diagnostics)
        diagnostic_values.push_back(diagnostic_json(diagnostic));

    return Json{
        {"schema", bounded_text(aggregate.schema)},
        {"valid", aggregate.valid},
        {"nativeRhiReady", aggregate.native_rhi_ready},
        {"blasTlasRuntimeReady", aggregate.blas_tlas_runtime_ready},
        // This batch never proves lighting/GI.  Keep the false value explicit
        // so downstream Agent tools cannot infer an RTGI claim from RT trace.
        {"rtgiReady", false},
        {"backends", std::move(backends)},
        {"diagnostics", std::move(diagnostic_values)},
        {"diagnosticsTruncated",
         aggregate.diagnostics.size() > raytracing_execution_max_diagnostics},
    }
        .dump();
}

std::string raytracing_execution_receipt_fingerprint(
    const RayTracingExecutionReceipt& receipt) {
    return hex_u64(fnv1a(raytracing_execution_receipt_canonical_json(receipt)));
}

std::string raytracing_execution_aggregate_fingerprint(
    const RayTracingExecutionAggregate& aggregate) {
    return hex_u64(fnv1a(raytracing_execution_aggregate_canonical_json(aggregate)));
}

} // namespace noemancer
