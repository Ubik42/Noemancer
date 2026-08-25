#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace noemancer {

// Engine-neutral execution facts.  This contract intentionally contains no
// D3D12/Vulkan headers, native handles, allocator objects, or Runtime types.
// A backend adapter must produce these facts only after the corresponding GPU
// operation and synchronization have been observed.
inline constexpr std::string_view raytracing_execution_receipt_schema =
    "noemancer.raytracing-execution-receipt/0.1";
inline constexpr std::size_t raytracing_execution_max_text_bytes = 256U;
inline constexpr std::size_t raytracing_execution_max_error_codes = 32U;
inline constexpr std::size_t raytracing_execution_max_diagnostics = 64U;
inline constexpr std::size_t raytracing_execution_max_receipts = 2U;
inline constexpr std::uint32_t raytracing_execution_max_as_count = 1'048'576U;
inline constexpr std::uint64_t raytracing_execution_max_resource_bytes =
    1ULL << 40U;

enum class RayTracingExecutionBackend : std::uint8_t {
    d3d12 = 0U,
    vulkan = 1U,

    D3D12 = d3d12,
    Vulkan = vulkan,
};

enum class RayTracingExecutionState : std::uint8_t {
    invalid = 0U,
    unsupported = 1U,
    fallback = 2U,
    ready = 3U,
    error = 4U,

    Invalid = invalid,
    Unsupported = unsupported,
    Fallback = fallback,
    Ready = ready,
    Error = error,
};

[[nodiscard]] std::string_view raytracing_execution_backend_name(
    RayTracingExecutionBackend backend) noexcept;
[[nodiscard]] std::string_view raytracing_execution_state_name(
    RayTracingExecutionState state) noexcept;
[[nodiscard]] bool raytracing_execution_backend_valid(
    RayTracingExecutionBackend backend) noexcept;
[[nodiscard]] bool raytracing_execution_state_valid(
    RayTracingExecutionState state) noexcept;

struct RayTracingExecutionBuildFacts final {
    // These are facts about the native command that was actually submitted,
    // not the requested policy.  A ready receipt requires all three markers.
    bool submitted{};
    bool completed{};
    bool flags_observed{};
    bool allow_update{};
    bool allow_compaction{};
    bool prefer_fast_trace{};
    bool prefer_fast_build{};
    std::uint32_t build_count{};
    std::uint32_t update_count{};
};

struct RayTracingExecutionCompactionFacts final {
    bool requested{};
    bool size_query_completed{};
    bool copy_completed{};
    bool committed{};
    std::uint64_t compacted_bytes{};
};

struct RayTracingExecutionSynchronizationFacts final {
    bool acceleration_structure_barrier_submitted{};
    bool build_to_trace_sync_submitted{};
    bool barrier_validation_passed{};
    bool queue_ownership_transfer_required{};
    bool queue_ownership_transfer_completed{};
};

struct RayTracingExecutionResourceFacts final {
    std::uint32_t blas_count{};
    std::uint32_t tlas_count{};
    std::uint64_t blas_result_bytes{};
    std::uint64_t tlas_result_bytes{};
    std::uint64_t scratch_bytes{};
    std::uint64_t sbt_bytes{};
    std::uint64_t output_bytes{};
    bool allocations_completed{};
    bool resource_addresses_valid{};
};

struct RayTracingExecutionReceipt final {
    std::string schema{std::string(raytracing_execution_receipt_schema)};
    RayTracingExecutionBackend backend{RayTracingExecutionBackend::d3d12};
    RayTracingExecutionState state{RayTracingExecutionState::invalid};
    std::string adapter_identity;

    // Capability facts are copied from the native probe, but no native handle
    // is allowed to cross this boundary.
    bool device_supported{};
    bool acceleration_structure_supported{};
    bool ray_tracing_pipeline_supported{};
    bool native_handles_exposed{};

    RayTracingExecutionBuildFacts blas;
    RayTracingExecutionBuildFacts tlas;
    RayTracingExecutionCompactionFacts compaction;
    RayTracingExecutionSynchronizationFacts synchronization;
    RayTracingExecutionResourceFacts resources;

    bool trace_submitted{};
    bool trace_completed{};
    bool gpu_timestamps_valid{};
    bool fallback_active{};
    std::string fallback_reason;
    std::vector<std::string> error_codes;
};

struct RayTracingExecutionDiagnostic final {
    std::string code;
    std::string path;
    std::string message;
};

struct RayTracingExecutionValidation final {
    bool valid{};
    bool successful{};
    std::vector<RayTracingExecutionDiagnostic> diagnostics;
};

struct RayTracingExecutionAggregate final {
    std::string schema{std::string(raytracing_execution_receipt_schema)};
    bool valid{};
    // These two flags are deliberately conservative: both D3D12 and Vulkan
    // must have independently successful receipts.  RTGI remains explicitly
    // false until a later lighting/denoising contract is accepted.
    bool native_rhi_ready{};
    bool blas_tlas_runtime_ready{};
    bool rtgi_ready{};
    std::array<bool, raytracing_execution_max_receipts> present{};
    std::array<RayTracingExecutionReceipt, raytracing_execution_max_receipts>
        receipts{}; // fixed D3D12, Vulkan order
    std::vector<RayTracingExecutionDiagnostic> diagnostics;
};

[[nodiscard]] RayTracingExecutionValidation
validate_raytracing_execution_receipt(
    const RayTracingExecutionReceipt& receipt);

[[nodiscard]] RayTracingExecutionAggregate
aggregate_raytracing_execution_receipts(
    const std::vector<RayTracingExecutionReceipt>& receipts);

[[nodiscard]] std::string raytracing_execution_receipt_canonical_json(
    const RayTracingExecutionReceipt& receipt);
[[nodiscard]] std::string raytracing_execution_aggregate_canonical_json(
    const RayTracingExecutionAggregate& aggregate);
[[nodiscard]] std::string raytracing_execution_receipt_fingerprint(
    const RayTracingExecutionReceipt& receipt);
[[nodiscard]] std::string raytracing_execution_aggregate_fingerprint(
    const RayTracingExecutionAggregate& aggregate);

} // namespace noemancer
