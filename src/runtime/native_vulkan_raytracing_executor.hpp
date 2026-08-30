#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace noemancer {

inline constexpr std::string_view native_vulkan_raytracing_executor_schema =
    "noemancer.native-vulkan-raytracing-executor/0.2";
inline constexpr std::size_t native_vulkan_raytracing_executor_max_text_bytes = 256U;

enum class NativeVulkanRayTracingExecutionState : std::uint8_t {
    unavailable = 0U,
    unsupported = 1U,
    completed = 2U,
    failed = 3U,

    Unavailable = unavailable,
    Unsupported = unsupported,
    Completed = completed,
    Failed = failed,
};

enum class NativeVulkanRayTracingFailureStage : std::uint8_t {
    none = 0U,
    loader = 1U,
    instance = 2U,
    physical_device = 3U,
    device = 4U,
    memory = 5U,
    buffer = 6U,
    acceleration_structure = 7U,
    command = 8U,
    submit = 9U,
    cleanup = 10U,
    shader_module = 11U,
    descriptor = 12U,
    pipeline = 13U,
    sbt = 14U,
    readback = 15U,

    None = none,
    Loader = loader,
    Instance = instance,
    PhysicalDevice = physical_device,
    Device = device,
    Memory = memory,
    Buffer = buffer,
    AccelerationStructure = acceleration_structure,
    Command = command,
    Submit = submit,
    Cleanup = cleanup,
    ShaderModule = shader_module,
    Descriptor = descriptor,
    Pipeline = pipeline,
    Sbt = sbt,
    Readback = readback,
};

[[nodiscard]] std::string_view native_vulkan_raytracing_execution_state_name(
    NativeVulkanRayTracingExecutionState state) noexcept;
[[nodiscard]] std::string_view native_vulkan_raytracing_failure_stage_name(
    NativeVulkanRayTracingFailureStage stage) noexcept;

// A bounded receipt for one real, temporary Vulkan RT execution.  No
// VkInstance/VkDevice/VkBuffer/pipeline/SBT/acceleration-structure handle
// crosses this API; all native objects are destroyed before the function
// returns.  A completed receipt proves one 1x1 ray dispatch and host
// readback, not RTGI or a production renderer path.
struct NativeVulkanRayTracingExecutionReceipt final {
    std::string schema{std::string(native_vulkan_raytracing_executor_schema)};
    NativeVulkanRayTracingExecutionState state{
        NativeVulkanRayTracingExecutionState::unavailable};
    NativeVulkanRayTracingFailureStage failure_stage{
        NativeVulkanRayTracingFailureStage::none};
    std::string code;
    std::string detail;
    std::string device_name;
    std::string driver_name;
    bool loader_available{};
    bool instance_created{};
    bool device_created{};
    bool queue_found{};
    bool feature_chain_enabled{};
    bool shader_module_created{};
    bool descriptor_set_layout_created{};
    bool pipeline_layout_created{};
    bool descriptor_set_allocated{};
    bool pipeline_created{};
    bool sbt_built{};
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
    bool blas_built{};
    bool tlas_built{};
    bool blas_build_submitted{};
    bool tlas_build_submitted{};
    bool blas_build_completed{};
    bool tlas_build_completed{};
    bool submitted{};
    bool fence_signaled{};
    bool trace_dispatch_issued{};
    bool output_readback_completed{};
    bool resources_released{};
    bool build_only{true};
    bool compaction_executed{};
    bool shader_table_prepared{};
    bool shader_table_uploaded{};
    bool trace_dispatch_submitted{};
    bool trace_dispatch_completed{};
    bool output_resource_created{};
    bool synchronization_completed{};
    bool timestamp_query_created{};
    bool timestamp_queries_issued{};
    bool timestamp_data_resolved{};
    bool gpu_timestamps_valid{};
    std::uint32_t api_version_major{};
    std::uint32_t api_version_minor{};
    std::uint32_t api_version_patch{};
    std::uint32_t vendor_id{};
    std::uint32_t device_id{};
    std::uint32_t queue_family_index{};
    std::uint32_t physical_device_count{};
    std::uint32_t geometry_count{};
    std::uint32_t instance_count{};
    std::uint64_t vertex_buffer_bytes{};
    std::uint64_t instance_buffer_bytes{};
    std::uint64_t blas_result_bytes{};
    std::uint64_t blas_scratch_bytes{};
    std::uint64_t tlas_result_bytes{};
    std::uint64_t tlas_scratch_bytes{};
    std::uint64_t sbt_buffer_bytes{};
    std::uint64_t output_buffer_bytes{};
    std::uint64_t total_allocated_bytes{};
    std::uint32_t shader_group_count{};
    std::uint32_t trace_width{};
    std::uint32_t trace_height{};
    std::uint32_t trace_depth{};
    std::uint32_t output_value{};
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
    std::uint64_t gpu_timestamp_frequency_hz{};
    std::uint64_t gpu_timestamp_ticks_begin{};
    std::uint64_t gpu_timestamp_ticks_end{};
    std::uint64_t gpu_timestamp_ticks_delta{};
    std::uint64_t gpu_timestamp_duration_ns{};
    std::uint64_t gpu_timestamp_readback_bytes{};
};

[[nodiscard]] NativeVulkanRayTracingExecutionReceipt
execute_native_vulkan_raytracing_blas_tlas();

} // namespace noemancer
