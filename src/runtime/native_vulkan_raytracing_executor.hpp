#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace noemancer {

inline constexpr std::string_view native_vulkan_raytracing_executor_schema =
    "noemancer.native-vulkan-raytracing-executor/0.1";
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
};

[[nodiscard]] std::string_view native_vulkan_raytracing_execution_state_name(
    NativeVulkanRayTracingExecutionState state) noexcept;
[[nodiscard]] std::string_view native_vulkan_raytracing_failure_stage_name(
    NativeVulkanRayTracingFailureStage stage) noexcept;

// A bounded receipt for one real, temporary Vulkan BLAS/TLAS execution.  No
// VkInstance/VkDevice/VkBuffer/acceleration-structure handle crosses this API;
// all native objects are destroyed before the function returns.
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
    bool blas_built{};
    bool tlas_built{};
    bool submitted{};
    bool fence_signaled{};
    bool resources_released{};
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
    std::uint64_t total_allocated_bytes{};
};

[[nodiscard]] NativeVulkanRayTracingExecutionReceipt
execute_native_vulkan_raytracing_blas_tlas();

} // namespace noemancer
