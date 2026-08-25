#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace noemancer {

// This Runtime adapter owns one short-lived, hardware-only D3D12 execution
// probe.  The receipt is deliberately plain data: no COM pointer, GPU virtual
// address, descriptor handle or other native object escapes the adapter.
// A successful receipt proves only BLAS/TLAS construction and synchronization;
// it does not claim an SBT, ray dispatch, RTGI or a production renderer path.
inline constexpr std::string_view native_d3d12_raytracing_executor_schema =
    "noemancer.native-d3d12-raytracing-executor/0.1";
inline constexpr std::size_t native_d3d12_raytracing_executor_max_text_bytes =
    512U;
inline constexpr std::uint64_t native_d3d12_raytracing_executor_max_resource_bytes =
    1ULL << 40U;

enum class NativeD3D12RayTracingExecutionState : std::uint8_t {
    unavailable = 0U,
    unsupported = 1U,
    failed = 2U,
    succeeded = 3U,

    Unavailable = unavailable,
    Unsupported = unsupported,
    Failed = failed,
    Succeeded = succeeded,
};

enum class NativeD3D12RayTracingFailureStage : std::uint8_t {
    none = 0U,
    platform = 1U,
    loader = 2U,
    factory = 3U,
    adapter = 4U,
    device = 5U,
    feature = 6U,
    interface_query = 7U,
    command_queue = 8U,
    command_allocator = 9U,
    command_list = 10U,
    vertex_buffer = 11U,
    blas_prebuild = 12U,
    blas_resources = 13U,
    blas_build = 14U,
    tlas_prebuild = 15U,
    tlas_resources = 16U,
    tlas_build = 17U,
    synchronization = 18U,
    cleanup = 19U,

    None = none,
    Platform = platform,
    Loader = loader,
    Factory = factory,
    Adapter = adapter,
    Device = device,
    Feature = feature,
    InterfaceQuery = interface_query,
    CommandQueue = command_queue,
    CommandAllocator = command_allocator,
    CommandList = command_list,
    VertexBuffer = vertex_buffer,
    BlasPrebuild = blas_prebuild,
    BlasResources = blas_resources,
    BlasBuild = blas_build,
    TlasPrebuild = tlas_prebuild,
    TlasResources = tlas_resources,
    TlasBuild = tlas_build,
    Synchronization = synchronization,
    Cleanup = cleanup,
};

[[nodiscard]] std::string_view native_d3d12_raytracing_execution_state_name(
    NativeD3D12RayTracingExecutionState state) noexcept;
[[nodiscard]] std::string_view native_d3d12_raytracing_failure_stage_name(
    NativeD3D12RayTracingFailureStage stage) noexcept;

struct NativeD3D12RayTracingExecutorOptions final {
    // WARP is never treated as a hardware RT success.  If no hardware device
    // is usable, this optional probe records an explicit unsupported fallback.
    bool probe_warp_fallback{true};
};

struct NativeD3D12RayTracingReceipt final {
    std::string schema{std::string(native_d3d12_raytracing_executor_schema)};
    std::string backend{"d3d12"};
    NativeD3D12RayTracingExecutionState state{
        NativeD3D12RayTracingExecutionState::unavailable};
    NativeD3D12RayTracingFailureStage failure_stage{
        NativeD3D12RayTracingFailureStage::none};
    std::string code;
    std::string detail;
    std::string device_name;
    bool hardware_probe_completed{};
    bool hardware_device_created{};
    bool hardware_raytracing_device_found{};
    bool warp_fallback_attempted{};
    bool warp_raytracing_supported{};
    bool native_handle_exposed{};
    std::uint32_t hardware_adapter_count{};
    std::uint32_t hardware_raytracing_device_count{};
    std::uint32_t raytracing_tier{};
    std::uint64_t vertex_buffer_bytes{};
    std::uint64_t blas_scratch_bytes{};
    std::uint64_t blas_result_bytes{};
    std::uint64_t tlas_scratch_bytes{};
    std::uint64_t tlas_result_bytes{};
    bool blas_prebuild_completed{};
    bool blas_build_submitted{};
    bool blas_build_completed{};
    bool tlas_prebuild_completed{};
    bool tlas_build_submitted{};
    bool tlas_build_completed{};
    bool synchronization_completed{};
    // Scope guardrails are explicit in the receipt: this adapter builds only
    // one BLAS and one TLAS.  It does not compact, create an SBT or dispatch
    // rays.
    bool build_only{true};
    bool compaction_executed{};
    bool trace_dispatch_issued{};
};

[[nodiscard]] NativeD3D12RayTracingReceipt
run_native_d3d12_raytracing_executor(
    const NativeD3D12RayTracingExecutorOptions& options = {});

} // namespace noemancer
