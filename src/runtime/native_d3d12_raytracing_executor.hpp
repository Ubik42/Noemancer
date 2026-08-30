#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace noemancer {

// This Runtime adapter owns one short-lived, hardware-only D3D12 execution
// probe.  The receipt is deliberately plain data: no COM pointer, GPU virtual
// address, descriptor handle or other native object escapes the adapter.
// A successful receipt proves one bounded BLAS/TLAS + SBT + TraceRays/readback
// execution and synchronization; it does not claim RTGI or a production
// renderer path.
inline constexpr std::string_view native_d3d12_raytracing_executor_schema =
    "noemancer.native-d3d12-raytracing-executor/0.2";
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
    root_signature = 20U,
    state_object = 21U,
    shader_table = 22U,
    output_resource = 23U,
    trace_dispatch = 24U,
    readback = 25U,
    timestamp_query = 26U,

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
    RootSignature = root_signature,
    StateObject = state_object,
    ShaderTable = shader_table,
    OutputResource = output_resource,
    TraceDispatch = trace_dispatch,
    Readback = readback,
    TimestampQuery = timestamp_query,
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
    bool blas_prebuild_completed{};
    bool blas_build_submitted{};
    bool blas_build_completed{};
    bool tlas_prebuild_completed{};
    bool tlas_build_submitted{};
    bool tlas_build_completed{};
    bool synchronization_completed{};
    // Scope guardrails are explicit in the receipt: this adapter builds one
    // BLAS/TLAS and executes one bounded 1x1 ray dispatch.  It does not claim
    // a production renderer, RTGI, denoising or a long-lived GPU context.
    bool build_only{true};
    bool compaction_executed{};
    bool trace_dispatch_issued{};
    bool shader_table_prepared{};
    bool shader_table_uploaded{};
    bool trace_dispatch_submitted{};
    bool trace_dispatch_completed{};
    bool root_signature_created{};
    bool state_object_created{};
    bool output_resource_created{};
    bool output_readback_completed{};
    std::uint32_t shader_table_record_count{};
    std::uint32_t shader_table_record_bytes{};
    std::uint64_t shader_table_bytes{};
    std::uint32_t output_width{};
    std::uint32_t output_height{};
    std::uint32_t output_pixel_stride_bytes{};
    std::uint64_t output_bytes{};
    std::uint64_t output_readback_bytes{};
    std::uint32_t output_sentinel{};
    std::uint32_t output_hit{};
    std::uint32_t output_pixel_x{};
    std::uint32_t output_pixel_y{};
    std::uint64_t output_hash{};
    bool timestamp_query_created{};
    bool timestamp_queries_issued{};
    bool timestamp_data_resolved{};
    bool gpu_timestamps_valid{};
    std::uint64_t gpu_timestamp_frequency_hz{};
    std::uint64_t gpu_timestamp_ticks_begin{};
    std::uint64_t gpu_timestamp_ticks_end{};
    std::uint64_t gpu_timestamp_ticks_delta{};
    std::uint64_t gpu_timestamp_duration_ns{};
    std::uint64_t gpu_timestamp_readback_bytes{};
};

[[nodiscard]] NativeD3D12RayTracingReceipt
run_native_d3d12_raytracing_executor(
    const NativeD3D12RayTracingExecutorOptions& options = {});

} // namespace noemancer
