#include "runtime/native_raytracing_execution_adapter.hpp"

#include <algorithm>
#include <limits>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace noemancer {
namespace {

constexpr std::string_view d3d12_adapter_identity =
    "native-d3d12-raytracing-execution-adapter";
constexpr std::string_view vulkan_adapter_identity =
    "native-vulkan-raytracing-execution-adapter";

constexpr std::string_view code_backend_unavailable = "rt.backend-unavailable";
constexpr std::string_view code_unsupported = "rt.unsupported";
constexpr std::string_view code_execution_failed = "rt.execution-failed";
constexpr std::string_view code_native_state_invalid = "rt.native-state-invalid";
constexpr std::string_view code_build_only = "rt.build-only-incomplete";
constexpr std::string_view code_trace_incomplete = "rt.trace-incomplete";
constexpr std::string_view code_sbt_missing = "rt.sbt-missing";
constexpr std::string_view code_output_missing = "rt.output-missing";
constexpr std::string_view code_trace_not_dispatched = "rt.trace-not-dispatched";
constexpr std::string_view code_readback_missing = "rt.readback-not-observed";
constexpr std::string_view code_trace_sync_missing =
    "rt.trace-synchronization-incomplete";
constexpr std::string_view code_gpu_timestamp_missing = "rt.gpu-timestamp-missing";
constexpr std::string_view code_compaction_incomplete = "rt.compaction-incomplete";

std::string bounded_text(const std::string_view value) {
    return std::string(value.substr(
        0U, native_raytracing_execution_adapter_max_text_bytes));
}

bool valid_text(const std::string_view value) noexcept {
    if (value.empty() ||
        value.size() > native_raytracing_execution_adapter_max_text_bytes)
        return false;
    for (const auto character : value) {
        const auto byte = static_cast<unsigned char>(character);
        if (byte < 0x20U || byte == 0x7fU) return false;
    }
    return true;
}

void append_error_code(RayTracingExecutionReceipt& receipt,
                       const std::string_view code) {
    const auto bounded = bounded_text(code);
    if (!valid_text(bounded)) return;
    if (std::ranges::find(receipt.error_codes, bounded) !=
        receipt.error_codes.end())
        return;
    if (receipt.error_codes.size() < raytracing_execution_max_error_codes)
        receipt.error_codes.push_back(bounded);
}

void append_native_error_code(RayTracingExecutionReceipt& receipt,
                              const std::string_view native_code) {
    // Native executors already use stable, bounded diagnostic codes.  Keep a
    // valid copy as a secondary detail, but never let a native diagnostic be
    // the only engine-level vocabulary.
    const auto bounded = bounded_text(native_code);
    if (valid_text(bounded)) append_error_code(receipt, bounded);
}

void sort_error_codes(RayTracingExecutionReceipt& receipt) {
    std::sort(receipt.error_codes.begin(), receipt.error_codes.end());
}

std::uint64_t add_resource_bytes(const std::uint64_t left,
                                 const std::uint64_t right) noexcept {
    if (left > std::numeric_limits<std::uint64_t>::max() - right)
        return std::numeric_limits<std::uint64_t>::max();
    return left + right;
}

bool has_nonzero_build_resources(const RayTracingExecutionReceipt& receipt) {
    return receipt.resources.blas_count > 0U &&
        receipt.resources.tlas_count > 0U &&
        receipt.resources.blas_result_bytes > 0U &&
        receipt.resources.tlas_result_bytes > 0U &&
        receipt.resources.scratch_bytes > 0U;
}

bool compaction_is_complete(const RayTracingExecutionCompactionFacts& facts) {
    if (!facts.requested) return !facts.committed;
    return facts.size_query_completed && facts.copy_completed && facts.committed &&
        facts.compacted_bytes > 0U;
}

bool has_compaction_evidence(
    const RayTracingExecutionCompactionFacts& facts) noexcept {
    return facts.requested || facts.size_query_completed || facts.copy_completed ||
        facts.committed || facts.compacted_bytes != 0U;
}

bool has_sync_evidence(
    const RayTracingExecutionSynchronizationFacts& facts) noexcept {
    return facts.acceleration_structure_barrier_submitted ||
        facts.build_to_trace_sync_submitted || facts.barrier_validation_passed ||
        facts.queue_ownership_transfer_required ||
        facts.queue_ownership_transfer_completed;
}

bool has_trace_evidence(
    const NativeRayTracingExecutionTraceEvidence& evidence) noexcept {
    // build_only=true with every other field empty is the intentional default
    // value.  This lets the no-argument adapter consume facts directly from a
    // native 0.2 receipt, while a caller can still provide an explicit future
    // extension by setting any proof field (or clearing build_only).
    return !evidence.build_only || evidence.blas_flags_observed ||
        evidence.tlas_flags_observed || evidence.blas_allow_update ||
        evidence.tlas_allow_update || evidence.blas_allow_compaction ||
        evidence.tlas_allow_compaction || evidence.blas_prefer_fast_trace ||
        evidence.tlas_prefer_fast_trace || evidence.blas_prefer_fast_build ||
        evidence.tlas_prefer_fast_build || evidence.blas_build_count != 0U ||
        evidence.tlas_build_count != 0U || evidence.blas_update_count != 0U ||
        evidence.tlas_update_count != 0U || has_compaction_evidence(evidence.compaction) ||
        evidence.sbt_ready || evidence.sbt_bytes != 0U || evidence.output_ready ||
        evidence.output_bytes != 0U || evidence.trace_submitted ||
        evidence.trace_completed || evidence.readback_completed ||
        evidence.gpu_timestamps_valid || has_sync_evidence(evidence.synchronization) ||
        evidence.allocations_completed || evidence.resource_addresses_valid ||
        evidence.ray_tracing_pipeline_supported;
}

NativeRayTracingExecutionTraceEvidence d3d12_native_trace_evidence(
    const NativeD3D12RayTracingReceipt& native_receipt) {
    NativeRayTracingExecutionTraceEvidence result;
    result.build_only = native_receipt.build_only;
    result.blas_flags_observed = native_receipt.blas_flags_observed;
    result.tlas_flags_observed = native_receipt.tlas_flags_observed;
    result.blas_allow_update = native_receipt.blas_allow_update;
    result.tlas_allow_update = native_receipt.tlas_allow_update;
    result.blas_allow_compaction = native_receipt.blas_allow_compaction;
    result.tlas_allow_compaction = native_receipt.tlas_allow_compaction;
    result.blas_prefer_fast_trace = native_receipt.blas_prefer_fast_trace;
    result.tlas_prefer_fast_trace = native_receipt.tlas_prefer_fast_trace;
    result.blas_prefer_fast_build = native_receipt.blas_prefer_fast_build;
    result.tlas_prefer_fast_build = native_receipt.tlas_prefer_fast_build;
    result.blas_build_count = native_receipt.blas_build_count;
    result.tlas_build_count = native_receipt.tlas_build_count;
    result.blas_update_count = native_receipt.blas_update_count;
    result.tlas_update_count = native_receipt.tlas_update_count;
    result.compaction.requested = native_receipt.compaction_executed;
    result.sbt_ready = native_receipt.shader_table_prepared &&
        native_receipt.shader_table_uploaded &&
        native_receipt.shader_table_record_count > 0U &&
        native_receipt.shader_table_record_bytes > 0U &&
        native_receipt.shader_table_bytes > 0U;
    result.sbt_bytes = native_receipt.shader_table_bytes;
    result.output_ready = native_receipt.output_resource_created &&
        native_receipt.output_width > 0U && native_receipt.output_height > 0U &&
        native_receipt.output_pixel_stride_bytes > 0U &&
        native_receipt.output_bytes > 0U;
    result.output_bytes = native_receipt.output_bytes;
    // "issued" only proves command recording.  Submission and completion are
    // separate gates so a partially recorded command list cannot become ready.
    result.trace_submitted = native_receipt.trace_dispatch_submitted;
    result.trace_completed = native_receipt.trace_dispatch_completed;
    result.readback_completed = native_receipt.output_readback_completed &&
        native_receipt.output_bytes > 0U &&
        native_receipt.output_readback_bytes >= native_receipt.output_bytes;
    // The native 0.2 executor validates the timestamp query/readback contract
    // before setting this bit.  Keep it as a fact, rather than manufacturing a
    // timestamp from CPU time or from command-recording markers.
    result.gpu_timestamps_valid = native_receipt.gpu_timestamps_valid;
    if (native_receipt.synchronization_completed) {
        result.synchronization.acceleration_structure_barrier_submitted = true;
        result.synchronization.barrier_validation_passed = true;
        result.synchronization.build_to_trace_sync_submitted =
            result.trace_submitted;
    }
    result.allocations_completed =
        native_receipt.blas_build_completed && native_receipt.tlas_build_completed &&
        native_receipt.blas_result_bytes > 0U &&
        native_receipt.tlas_result_bytes > 0U &&
        native_receipt.blas_scratch_bytes > 0U &&
        native_receipt.tlas_scratch_bytes > 0U;
    result.resource_addresses_valid = result.allocations_completed;
    result.ray_tracing_pipeline_supported =
        native_receipt.root_signature_created &&
        native_receipt.state_object_created;
    return result;
}

NativeRayTracingExecutionTraceEvidence vulkan_native_trace_evidence(
    const NativeVulkanRayTracingExecutionReceipt& native_receipt) {
    NativeRayTracingExecutionTraceEvidence result;
    result.build_only = native_receipt.build_only;
    result.blas_flags_observed = native_receipt.blas_flags_observed;
    result.tlas_flags_observed = native_receipt.tlas_flags_observed;
    result.blas_allow_update = native_receipt.blas_allow_update;
    result.tlas_allow_update = native_receipt.tlas_allow_update;
    result.blas_allow_compaction = native_receipt.blas_allow_compaction;
    result.tlas_allow_compaction = native_receipt.tlas_allow_compaction;
    result.blas_prefer_fast_trace = native_receipt.blas_prefer_fast_trace;
    result.tlas_prefer_fast_trace = native_receipt.tlas_prefer_fast_trace;
    result.blas_prefer_fast_build = native_receipt.blas_prefer_fast_build;
    result.tlas_prefer_fast_build = native_receipt.tlas_prefer_fast_build;
    result.blas_build_count = native_receipt.blas_build_count;
    result.tlas_build_count = native_receipt.tlas_build_count;
    result.blas_update_count = native_receipt.blas_update_count;
    result.tlas_update_count = native_receipt.tlas_update_count;
    result.compaction.requested = native_receipt.compaction_executed;
    result.sbt_ready = native_receipt.sbt_built &&
        native_receipt.shader_table_prepared &&
        native_receipt.shader_table_uploaded &&
        native_receipt.shader_table_record_count > 0U &&
        native_receipt.shader_table_record_bytes > 0U &&
        native_receipt.sbt_buffer_bytes > 0U &&
        native_receipt.shader_table_bytes > 0U;
    result.sbt_bytes = native_receipt.shader_table_bytes;
    result.output_ready = native_receipt.output_resource_created &&
        native_receipt.output_width > 0U && native_receipt.output_height > 0U &&
        native_receipt.output_pixel_stride_bytes > 0U &&
        native_receipt.output_buffer_bytes > 0U && native_receipt.output_bytes > 0U;
    result.output_bytes = native_receipt.output_bytes;
    result.trace_submitted = native_receipt.trace_dispatch_submitted;
    result.trace_completed = native_receipt.trace_dispatch_completed;
    result.readback_completed = native_receipt.output_readback_completed &&
        native_receipt.output_bytes > 0U &&
        native_receipt.output_readback_bytes >= native_receipt.output_bytes;
    result.gpu_timestamps_valid = native_receipt.gpu_timestamps_valid;
    if (native_receipt.submitted && native_receipt.fence_signaled) {
        result.synchronization.acceleration_structure_barrier_submitted = true;
        result.synchronization.barrier_validation_passed = true;
        result.synchronization.build_to_trace_sync_submitted =
            native_receipt.trace_dispatch_issued;
    }
    result.allocations_completed = native_receipt.fence_signaled &&
        native_receipt.blas_result_bytes > 0U &&
        native_receipt.tlas_result_bytes > 0U &&
        native_receipt.blas_scratch_bytes > 0U &&
        native_receipt.tlas_scratch_bytes > 0U;
    result.resource_addresses_valid = result.allocations_completed;
    result.ray_tracing_pipeline_supported = native_receipt.pipeline_created;
    return result;
}

NativeRayTracingExecutionTraceEvidence merge_trace_evidence(
    const NativeRayTracingExecutionTraceEvidence& native_evidence,
    const NativeRayTracingExecutionTraceEvidence& supplied_evidence) {
    if (!has_trace_evidence(supplied_evidence)) return native_evidence;

    NativeRayTracingExecutionTraceEvidence result = native_evidence;
    // A native build-only marker is authoritative.  An extension evidence
    // object may prove additional facts, but cannot rewrite a receipt that
    // explicitly says it never dispatched a trace.
    result.build_only = native_evidence.build_only || supplied_evidence.build_only;
    result.blas_flags_observed |= supplied_evidence.blas_flags_observed;
    result.tlas_flags_observed |= supplied_evidence.tlas_flags_observed;
    result.blas_allow_update |= supplied_evidence.blas_allow_update;
    result.tlas_allow_update |= supplied_evidence.tlas_allow_update;
    result.blas_allow_compaction |= supplied_evidence.blas_allow_compaction;
    result.tlas_allow_compaction |= supplied_evidence.tlas_allow_compaction;
    result.blas_prefer_fast_trace |= supplied_evidence.blas_prefer_fast_trace;
    result.tlas_prefer_fast_trace |= supplied_evidence.tlas_prefer_fast_trace;
    result.blas_prefer_fast_build |= supplied_evidence.blas_prefer_fast_build;
    result.tlas_prefer_fast_build |= supplied_evidence.tlas_prefer_fast_build;
    result.blas_build_count = std::max(result.blas_build_count,
                                       supplied_evidence.blas_build_count);
    result.tlas_build_count = std::max(result.tlas_build_count,
                                       supplied_evidence.tlas_build_count);
    result.blas_update_count = std::max(result.blas_update_count,
                                        supplied_evidence.blas_update_count);
    result.tlas_update_count = std::max(result.tlas_update_count,
                                        supplied_evidence.tlas_update_count);
    if (has_compaction_evidence(supplied_evidence.compaction))
        result.compaction = supplied_evidence.compaction;
    result.sbt_ready |= supplied_evidence.sbt_ready;
    if (supplied_evidence.sbt_bytes != 0U)
        result.sbt_bytes = supplied_evidence.sbt_bytes;
    result.output_ready |= supplied_evidence.output_ready;
    if (supplied_evidence.output_bytes != 0U)
        result.output_bytes = supplied_evidence.output_bytes;
    result.trace_submitted |= supplied_evidence.trace_submitted;
    result.trace_completed |= supplied_evidence.trace_completed;
    result.readback_completed |= supplied_evidence.readback_completed;
    result.gpu_timestamps_valid |= supplied_evidence.gpu_timestamps_valid;
    result.synchronization.acceleration_structure_barrier_submitted |=
        supplied_evidence.synchronization.acceleration_structure_barrier_submitted;
    result.synchronization.build_to_trace_sync_submitted |=
        supplied_evidence.synchronization.build_to_trace_sync_submitted;
    result.synchronization.barrier_validation_passed |=
        supplied_evidence.synchronization.barrier_validation_passed;
    result.synchronization.queue_ownership_transfer_required |=
        supplied_evidence.synchronization.queue_ownership_transfer_required;
    result.synchronization.queue_ownership_transfer_completed |=
        supplied_evidence.synchronization.queue_ownership_transfer_completed;
    result.allocations_completed |= supplied_evidence.allocations_completed;
    result.resource_addresses_valid |= supplied_evidence.resource_addresses_valid;
    result.ray_tracing_pipeline_supported |=
        supplied_evidence.ray_tracing_pipeline_supported;
    return result;
}

void append_missing_trace_codes(
    RayTracingExecutionReceipt& receipt,
    const NativeRayTracingExecutionTraceEvidence& evidence) {
    if (!evidence.sbt_ready || evidence.sbt_bytes == 0U)
        append_error_code(receipt, code_sbt_missing);
    if (!evidence.output_ready || evidence.output_bytes == 0U)
        append_error_code(receipt, code_output_missing);
    if (!evidence.trace_submitted || !evidence.trace_completed)
        append_error_code(receipt, code_trace_not_dispatched);
    if (!evidence.readback_completed)
        append_error_code(receipt, code_readback_missing);
    if (!evidence.synchronization.acceleration_structure_barrier_submitted ||
        !evidence.synchronization.build_to_trace_sync_submitted ||
        !evidence.synchronization.barrier_validation_passed ||
        (evidence.synchronization.queue_ownership_transfer_required &&
         !evidence.synchronization.queue_ownership_transfer_completed))
        append_error_code(receipt, code_trace_sync_missing);
    if (!evidence.gpu_timestamps_valid)
        append_error_code(receipt, code_gpu_timestamp_missing);
    if (!compaction_is_complete(evidence.compaction))
        append_error_code(receipt, code_compaction_incomplete);
}

void set_fallback(RayTracingExecutionReceipt& receipt,
                  const std::string_view reason) {
    receipt.state = RayTracingExecutionState::fallback;
    receipt.fallback_active = true;
    receipt.fallback_reason = bounded_text(reason);
}

void set_unsupported(RayTracingExecutionReceipt& receipt) {
    receipt.state = RayTracingExecutionState::unsupported;
    receipt.fallback_active = false;
    receipt.fallback_reason.clear();
}

void set_error(RayTracingExecutionReceipt& receipt,
               const std::string_view reason) {
    receipt.state = RayTracingExecutionState::error;
    receipt.fallback_active = true;
    receipt.fallback_reason = bounded_text(reason);
}

RayTracingExecutionBuildFacts d3d12_build_facts(
    const bool submitted, const bool completed, const bool flags_observed,
    const bool allow_update, const bool allow_compaction,
    const bool prefer_fast_trace, const bool prefer_fast_build,
    const std::uint32_t build_count, const std::uint32_t update_count) {
    return RayTracingExecutionBuildFacts{
        .submitted = submitted,
        .completed = completed,
        .flags_observed = flags_observed,
        .allow_update = allow_update,
        .allow_compaction = allow_compaction,
        .prefer_fast_trace = prefer_fast_trace,
        .prefer_fast_build = prefer_fast_build,
        .build_count = build_count,
        .update_count = update_count,
    };
}

RayTracingExecutionBuildFacts vulkan_build_facts(
    const bool submitted, const bool completed, const bool flags_observed,
    const bool allow_update, const bool allow_compaction,
    const bool prefer_fast_trace, const bool prefer_fast_build,
    const std::uint32_t build_count, const std::uint32_t update_count) {
    return d3d12_build_facts(submitted, completed, flags_observed, allow_update,
                             allow_compaction, prefer_fast_trace,
                             prefer_fast_build, build_count, update_count);
}

bool trace_evidence_ready(const RayTracingExecutionReceipt& receipt,
                          const NativeRayTracingExecutionTraceEvidence& evidence,
                          const bool native_build_only) {
    // Readback is intentionally an adapter-only gate for the current engine
    // schema: dispatch completion is not pixel evidence.  Once the engine
    // receipt grows an explicit readback field, this check remains the single
    // place that must be updated alongside the projection.
    return !native_build_only && !evidence.build_only &&
        receipt.device_supported && receipt.acceleration_structure_supported &&
        receipt.ray_tracing_pipeline_supported &&
        receipt.blas.submitted && receipt.blas.completed &&
        receipt.blas.flags_observed && receipt.tlas.submitted &&
        receipt.tlas.completed && receipt.tlas.flags_observed &&
        has_nonzero_build_resources(receipt) &&
        receipt.resources.allocations_completed &&
        receipt.resources.resource_addresses_valid && evidence.sbt_ready &&
        evidence.sbt_bytes > 0U && evidence.output_ready &&
        evidence.output_bytes > 0U && evidence.trace_submitted &&
        evidence.trace_completed && evidence.readback_completed &&
        evidence.synchronization.acceleration_structure_barrier_submitted &&
        evidence.synchronization.build_to_trace_sync_submitted &&
        evidence.synchronization.barrier_validation_passed &&
        (!evidence.synchronization.queue_ownership_transfer_required ||
         evidence.synchronization.queue_ownership_transfer_completed) &&
        evidence.gpu_timestamps_valid && compaction_is_complete(evidence.compaction);
}

void finish_projection(RayTracingExecutionReceipt& receipt,
                       const NativeRayTracingExecutionTraceEvidence& evidence,
                       const bool native_build_only,
                       const bool native_execution_completed) {
    const bool ready = native_execution_completed &&
        trace_evidence_ready(receipt, evidence, native_build_only);
    if (ready) {
        receipt.state = RayTracingExecutionState::ready;
        receipt.fallback_active = false;
        receipt.fallback_reason.clear();
        receipt.error_codes.clear();
        sort_error_codes(receipt);
        return;
    }

    // The native executor completed only an AS build today.  Keep this as a
    // valid fallback receipt, with a machine-readable list of every missing
    // production trace proof, rather than projecting it to ready.
    if (native_execution_completed) {
        if (native_build_only || evidence.build_only) {
            set_fallback(receipt, code_build_only);
            append_error_code(receipt, code_build_only);
        } else {
            set_fallback(receipt, code_trace_incomplete);
            append_error_code(receipt, code_trace_incomplete);
        }
        append_missing_trace_codes(receipt, evidence);
    }
    sort_error_codes(receipt);
}

void map_d3d12_capabilities(
    const NativeD3D12RayTracingReceipt& native_receipt,
    RayTracingExecutionReceipt& result) {
    const bool hardware_rt = native_receipt.hardware_device_created &&
        native_receipt.hardware_raytracing_device_found &&
        native_receipt.raytracing_tier > 0U;
    result.device_supported = hardware_rt;
    result.acceleration_structure_supported = hardware_rt;
    result.ray_tracing_pipeline_supported = hardware_rt;
    result.native_handles_exposed = native_receipt.native_handle_exposed;
}

void map_vulkan_capabilities(
    const NativeVulkanRayTracingExecutionReceipt& native_receipt,
    const NativeRayTracingExecutionTraceEvidence& evidence,
    RayTracingExecutionReceipt& result) {
    const bool device = native_receipt.device_created &&
        native_receipt.feature_chain_enabled;
    const bool acceleration_structure = device &&
        (native_receipt.blas_built || native_receipt.tlas_built ||
         native_receipt.geometry_count > 0U || native_receipt.instance_count > 0U);
    result.device_supported = device;
    result.acceleration_structure_supported = acceleration_structure;
    // Pipeline support is a native 0.2 fact now.  Keep the evidence argument
    // as an additive extension for a backend that has not yet bumped its
    // receipt, but never infer pipeline support from AS capability alone.
    result.ray_tracing_pipeline_supported = native_receipt.pipeline_created ||
        evidence.ray_tracing_pipeline_supported;
    result.native_handles_exposed = false;
}

RayTracingExecutionResourceFacts d3d12_resources(
    const NativeD3D12RayTracingReceipt& native_receipt,
    const NativeRayTracingExecutionTraceEvidence& evidence) {
    const bool has_blas = native_receipt.blas_prebuild_completed ||
        native_receipt.blas_build_submitted || native_receipt.blas_build_completed;
    const bool has_tlas = native_receipt.tlas_prebuild_completed ||
        native_receipt.tlas_build_submitted || native_receipt.tlas_build_completed;
    const bool build_completed =
        native_receipt.blas_build_completed && native_receipt.tlas_build_completed;
    const bool allocated = build_completed && native_receipt.blas_result_bytes > 0U &&
        native_receipt.tlas_result_bytes > 0U && native_receipt.blas_scratch_bytes > 0U &&
        native_receipt.tlas_scratch_bytes > 0U;
    return RayTracingExecutionResourceFacts{
        .blas_count = has_blas ? 1U : 0U,
        .tlas_count = has_tlas ? 1U : 0U,
        .blas_result_bytes = native_receipt.blas_result_bytes,
        .tlas_result_bytes = native_receipt.tlas_result_bytes,
        .scratch_bytes = add_resource_bytes(native_receipt.blas_scratch_bytes,
                                             native_receipt.tlas_scratch_bytes),
        .sbt_bytes = evidence.sbt_bytes,
        .output_bytes = evidence.output_bytes,
        .allocations_completed = allocated || evidence.allocations_completed,
        .resource_addresses_valid = allocated || evidence.resource_addresses_valid,
    };
}

RayTracingExecutionResourceFacts vulkan_resources(
    const NativeVulkanRayTracingExecutionReceipt& native_receipt,
    const NativeRayTracingExecutionTraceEvidence& evidence) {
    const bool has_blas = native_receipt.blas_built ||
        native_receipt.geometry_count > 0U;
    const bool has_tlas = native_receipt.tlas_built ||
        native_receipt.instance_count > 0U;
    const bool build_completed = native_receipt.blas_built &&
        native_receipt.tlas_built && native_receipt.fence_signaled;
    const bool allocated = build_completed && native_receipt.blas_result_bytes > 0U &&
        native_receipt.tlas_result_bytes > 0U && native_receipt.blas_scratch_bytes > 0U &&
        native_receipt.tlas_scratch_bytes > 0U;
    return RayTracingExecutionResourceFacts{
        .blas_count = has_blas ? 1U : 0U,
        .tlas_count = has_tlas ? 1U : 0U,
        .blas_result_bytes = native_receipt.blas_result_bytes,
        .tlas_result_bytes = native_receipt.tlas_result_bytes,
        .scratch_bytes = add_resource_bytes(native_receipt.blas_scratch_bytes,
                                             native_receipt.tlas_scratch_bytes),
        .sbt_bytes = evidence.sbt_bytes,
        .output_bytes = evidence.output_bytes,
        .allocations_completed = allocated || evidence.allocations_completed,
        .resource_addresses_valid = allocated || evidence.resource_addresses_valid,
    };
}

RayTracingExecutionReceipt base_receipt(
    const RayTracingExecutionBackend backend, const std::string_view identity) {
    RayTracingExecutionReceipt result;
    result.backend = backend;
    result.adapter_identity = std::string(identity);
    return result;
}

} // namespace

RayTracingExecutionReceipt adapt_native_d3d12_raytracing_receipt(
    const NativeD3D12RayTracingReceipt& native_receipt,
    const NativeRayTracingExecutionTraceEvidence& supplied_trace_evidence) {
    const auto trace_evidence = merge_trace_evidence(
        d3d12_native_trace_evidence(native_receipt), supplied_trace_evidence);
    auto result = base_receipt(RayTracingExecutionBackend::d3d12,
                               d3d12_adapter_identity);
    map_d3d12_capabilities(native_receipt, result);
    result.blas = d3d12_build_facts(
        native_receipt.blas_build_submitted,
        native_receipt.blas_build_completed,
        trace_evidence.blas_flags_observed,
        trace_evidence.blas_allow_update,
        trace_evidence.blas_allow_compaction,
        trace_evidence.blas_prefer_fast_trace,
        trace_evidence.blas_prefer_fast_build,
        trace_evidence.blas_build_count != 0U
            ? trace_evidence.blas_build_count
            : (native_receipt.blas_build_submitted ? 1U : 0U),
        trace_evidence.blas_update_count);
    result.tlas = d3d12_build_facts(
        native_receipt.tlas_build_submitted,
        native_receipt.tlas_build_completed,
        trace_evidence.tlas_flags_observed,
        trace_evidence.tlas_allow_update,
        trace_evidence.tlas_allow_compaction,
        trace_evidence.tlas_prefer_fast_trace,
        trace_evidence.tlas_prefer_fast_build,
        trace_evidence.tlas_build_count != 0U
            ? trace_evidence.tlas_build_count
            : (native_receipt.tlas_build_submitted ? 1U : 0U),
        trace_evidence.tlas_update_count);
    result.compaction = trace_evidence.compaction;
    result.synchronization = trace_evidence.synchronization;
    // The native 0.2 receipt records one fence covering the BLAS/TLAS and
    // bounded TraceRays sequence.  The trace-side marker remains gated by the
    // native trace-submitted fact.
    if (native_receipt.synchronization_completed) {
        result.synchronization.acceleration_structure_barrier_submitted = true;
        result.synchronization.barrier_validation_passed = true;
        result.synchronization.build_to_trace_sync_submitted =
            result.synchronization.build_to_trace_sync_submitted ||
            trace_evidence.trace_submitted;
    }
    result.resources = d3d12_resources(native_receipt, trace_evidence);
    result.trace_submitted = trace_evidence.trace_submitted;
    result.trace_completed = trace_evidence.trace_completed;
    result.gpu_timestamps_valid = trace_evidence.gpu_timestamps_valid;

    switch (native_receipt.state) {
    case NativeD3D12RayTracingExecutionState::unavailable:
        set_fallback(result, code_backend_unavailable);
        append_error_code(result, code_backend_unavailable);
        append_native_error_code(result, native_receipt.code);
        break;
    case NativeD3D12RayTracingExecutionState::unsupported:
        set_unsupported(result);
        append_error_code(result, code_unsupported);
        append_native_error_code(result, native_receipt.code);
        break;
    case NativeD3D12RayTracingExecutionState::failed:
        set_error(result, code_execution_failed);
        append_error_code(result, code_execution_failed);
        append_native_error_code(result, native_receipt.code);
        break;
    case NativeD3D12RayTracingExecutionState::succeeded:
        finish_projection(result, trace_evidence,
                          native_receipt.build_only,
                          true);
        break;
    default:
        set_error(result, code_native_state_invalid);
        append_error_code(result, code_native_state_invalid);
        break;
    }
    sort_error_codes(result);
    return result;
}

RayTracingExecutionReceipt adapt_native_vulkan_raytracing_receipt(
    const NativeVulkanRayTracingExecutionReceipt& native_receipt,
    const NativeRayTracingExecutionTraceEvidence& supplied_trace_evidence) {
    const auto trace_evidence = merge_trace_evidence(
        vulkan_native_trace_evidence(native_receipt), supplied_trace_evidence);
    auto result = base_receipt(RayTracingExecutionBackend::vulkan,
                               vulkan_adapter_identity);
    map_vulkan_capabilities(native_receipt, trace_evidence, result);
    const bool submitted = native_receipt.submitted;
    const bool blas_completed = native_receipt.blas_built &&
        native_receipt.fence_signaled;
    const bool tlas_completed = native_receipt.tlas_built &&
        native_receipt.fence_signaled;
    result.blas = vulkan_build_facts(
        submitted, blas_completed, trace_evidence.blas_flags_observed,
        trace_evidence.blas_allow_update, trace_evidence.blas_allow_compaction,
        trace_evidence.blas_prefer_fast_trace,
        trace_evidence.blas_prefer_fast_build,
        trace_evidence.blas_build_count != 0U
            ? trace_evidence.blas_build_count
            : (submitted ? 1U : 0U),
        trace_evidence.blas_update_count);
    result.tlas = vulkan_build_facts(
        submitted, tlas_completed, trace_evidence.tlas_flags_observed,
        trace_evidence.tlas_allow_update, trace_evidence.tlas_allow_compaction,
        trace_evidence.tlas_prefer_fast_trace,
        trace_evidence.tlas_prefer_fast_build,
        trace_evidence.tlas_build_count != 0U
            ? trace_evidence.tlas_build_count
            : (submitted ? 1U : 0U),
        trace_evidence.tlas_update_count);
    result.compaction = trace_evidence.compaction;
    result.synchronization = trace_evidence.synchronization;
    // The Vulkan receipt does not expose a separate synchronization struct.  A
    // signaled fence proves the submitted command stream; the build-to-trace
    // marker is still gated by the native trace-submitted bit.
    if (native_receipt.submitted && native_receipt.fence_signaled) {
        result.synchronization.acceleration_structure_barrier_submitted = true;
        result.synchronization.barrier_validation_passed = true;
        result.synchronization.build_to_trace_sync_submitted =
            result.synchronization.build_to_trace_sync_submitted ||
            trace_evidence.trace_submitted;
    }
    result.resources = vulkan_resources(native_receipt, trace_evidence);
    result.trace_submitted = trace_evidence.trace_submitted;
    result.trace_completed = trace_evidence.trace_completed;
    result.gpu_timestamps_valid = trace_evidence.gpu_timestamps_valid;

    switch (native_receipt.state) {
    case NativeVulkanRayTracingExecutionState::unavailable:
        set_fallback(result, code_backend_unavailable);
        append_error_code(result, code_backend_unavailable);
        append_native_error_code(result, native_receipt.code);
        break;
    case NativeVulkanRayTracingExecutionState::unsupported:
        set_unsupported(result);
        append_error_code(result, code_unsupported);
        append_native_error_code(result, native_receipt.code);
        break;
    case NativeVulkanRayTracingExecutionState::failed:
        set_error(result, code_execution_failed);
        append_error_code(result, code_execution_failed);
        append_native_error_code(result, native_receipt.code);
        break;
    case NativeVulkanRayTracingExecutionState::completed:
        finish_projection(result, trace_evidence,
                          native_receipt.build_only,
                          true);
        break;
    default:
        set_error(result, code_native_state_invalid);
        append_error_code(result, code_native_state_invalid);
        break;
    }
    sort_error_codes(result);
    return result;
}

RayTracingExecutionAggregate aggregate_native_raytracing_execution_receipts(
    const NativeD3D12RayTracingReceipt& d3d12_receipt,
    const NativeVulkanRayTracingExecutionReceipt& vulkan_receipt,
    const NativeRayTracingExecutionTraceEvidence& d3d12_trace_evidence,
    const NativeRayTracingExecutionTraceEvidence& vulkan_trace_evidence) {
    const auto d3d12 = adapt_native_d3d12_raytracing_receipt(
        d3d12_receipt, d3d12_trace_evidence);
    const auto vulkan = adapt_native_vulkan_raytracing_receipt(
        vulkan_receipt, vulkan_trace_evidence);
    return aggregate_raytracing_execution_receipts({d3d12, vulkan});
}

} // namespace noemancer
