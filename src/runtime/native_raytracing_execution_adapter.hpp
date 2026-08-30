#pragma once

#include "engine/raytracing_execution_receipt.hpp"
#include "runtime/native_d3d12_raytracing_executor.hpp"
#include "runtime/native_vulkan_raytracing_executor.hpp"

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace noemancer {

// This is a Runtime-private projection layer.  Native executor receipts are
// intentionally allowed to evolve independently from the engine contract;
// this adapter is the only place where backend-specific execution facts are
// translated into RayTracingExecutionReceipt.
inline constexpr std::string_view
    native_raytracing_execution_adapter_schema =
        "noemancer.native-raytracing-execution-adapter/0.2";
inline constexpr std::size_t
    native_raytracing_execution_adapter_max_text_bytes =
        raytracing_execution_max_text_bytes;

// Native D3D12/Vulkan 0.2 receipts now carry the facts for the bounded probe
// directly (including build flags, SBT/output, dispatch, readback and GPU
// timestamps).  The adapter consumes those facts by default.  This bounded,
// engine-neutral extension remains available for a backend that is adding a
// fact before its native receipt schema is revised; it is deliberately
// all-false/zero by default.  No native handle or third-party type belongs
// here.
struct NativeRayTracingExecutionTraceEvidence final {
    // A build-only receipt is never trace-ready, even if another field is
    // accidentally populated.  Future executors must explicitly clear this.
    bool build_only{true};

    // Build policy/flag observations required by the engine ready contract.
    bool blas_flags_observed{};
    bool tlas_flags_observed{};
    bool blas_allow_update{};
    bool tlas_allow_update{};
    bool blas_allow_compaction{};
    bool tlas_allow_compaction{};
    bool blas_prefer_fast_trace{};
    bool tlas_prefer_fast_trace{};
    bool blas_prefer_fast_build{};
    bool tlas_prefer_fast_build{};
    std::uint32_t blas_build_count{};
    std::uint32_t tlas_build_count{};
    std::uint32_t blas_update_count{};
    std::uint32_t tlas_update_count{};

    // Compaction facts are optional for the first trace slice.  If requested,
    // the engine receipt validator still requires the complete query/copy/
    // commit sequence before accepting a ready receipt.
    RayTracingExecutionCompactionFacts compaction{};

    // Trace resource and completion facts.  SBT/output bytes are copied only
    // when the corresponding resource-ready marker is true.  Readback is a
    // separate gate: dispatch completion alone is not a pixel proof.
    bool sbt_ready{};
    std::uint64_t sbt_bytes{};
    bool output_ready{};
    std::uint64_t output_bytes{};
    bool trace_submitted{};
    bool trace_completed{};
    bool readback_completed{};
    bool gpu_timestamps_valid{};

    // A future native receipt must report these individually; one generic
    // "synchronization completed" flag is not enough for trace readiness.
    RayTracingExecutionSynchronizationFacts synchronization{};

    // Build resource allocation/address proof.  Existing build probes do not
    // expose these as separate facts and therefore leave them false.
    bool allocations_completed{};
    bool resource_addresses_valid{};

    // Pipeline support is intentionally separate from AS support.  The
    // current Vulkan build probe creates BLAS/TLAS but does not create a RT
    // pipeline, so it must remain false until a pipeline receipt exists.
    bool ray_tracing_pipeline_supported{};
};

// Project one backend's native receipt.  The default evidence is empty, so the
// 0.2 fields in the native receipt are the source of truth.  The optional
// evidence argument is an explicit extension hook, not a requirement for a
// normal CLI/runtime call.
[[nodiscard]] RayTracingExecutionReceipt
adapt_native_d3d12_raytracing_receipt(
    const NativeD3D12RayTracingReceipt& native_receipt,
    const NativeRayTracingExecutionTraceEvidence& trace_evidence = {});

[[nodiscard]] RayTracingExecutionReceipt
adapt_native_vulkan_raytracing_receipt(
    const NativeVulkanRayTracingExecutionReceipt& native_receipt,
    const NativeRayTracingExecutionTraceEvidence& trace_evidence = {});

// Project and aggregate in one operation.  The returned aggregate always uses
// the engine contract's deterministic D3D12, Vulkan slot order, regardless of
// which backend executor completed first.
[[nodiscard]] RayTracingExecutionAggregate
aggregate_native_raytracing_execution_receipts(
    const NativeD3D12RayTracingReceipt& d3d12_receipt,
    const NativeVulkanRayTracingExecutionReceipt& vulkan_receipt,
    const NativeRayTracingExecutionTraceEvidence& d3d12_trace_evidence = {},
    const NativeRayTracingExecutionTraceEvidence& vulkan_trace_evidence = {});

} // namespace noemancer
