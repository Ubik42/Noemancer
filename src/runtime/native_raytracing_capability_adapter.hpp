#pragma once

#include <cstdint>
#include <cstddef>
#include <string>
#include <string_view>

namespace noemancer {

inline constexpr std::string_view native_raytracing_capability_schema =
    "noemancer.native-raytracing-capability/0.1";
inline constexpr std::size_t native_raytracing_capability_max_text_bytes = 256U;

enum class NativeRayTracingBackend : std::uint8_t {
    d3d12 = 0U,
    vulkan = 1U,

    D3D12 = d3d12,
    Vulkan = vulkan,
};

enum class NativeRayTracingProbeState : std::uint8_t {
    unavailable = 0U,
    unsupported = 1U,
    supported = 2U,
    query_failed = 3U,

    Unavailable = unavailable,
    Unsupported = unsupported,
    Supported = supported,
    QueryFailed = query_failed,
};

[[nodiscard]] std::string_view native_raytracing_backend_name(
    NativeRayTracingBackend backend) noexcept;
[[nodiscard]] std::string_view native_raytracing_probe_state_name(
    NativeRayTracingProbeState state) noexcept;

// This is intentionally plain data.  It reports a point-in-time capability
// probe only; no native device/adapter/instance handle crosses this boundary
// and no persistent GPU object is owned by the adapter.
struct NativeRayTracingCapability final {
    std::string schema{std::string(native_raytracing_capability_schema)};
    std::string backend;
    NativeRayTracingProbeState state{NativeRayTracingProbeState::unavailable};
    std::string code;
    std::string detail;
    std::string device_name;
    bool loader_available{};
    bool device_query_completed{};
    bool feature_query_completed{};
    // Vulkan extension/feature evidence.  D3D12 reports its equivalent
    // capability through ray_tracing_tier instead of these extension flags.
    bool acceleration_structure_extension{};
    bool ray_tracing_pipeline_extension{};
    bool deferred_host_operations_extension{};
    bool buffer_device_address_extension{};
    bool acceleration_structure_feature{};
    bool ray_tracing_pipeline_feature{};
    bool buffer_device_address_feature{};
    bool native_device_created{};
    std::uint32_t api_version_major{};
    std::uint32_t api_version_minor{};
    std::uint32_t api_version_patch{};
    std::uint32_t device_count{};
    std::uint32_t supported_device_count{};
    // D3D12_OPTIONS5 RaytracingTier numeric value.  Vulkan leaves this zero;
    // Vulkan support is represented by its extension and feature flags.
    std::uint32_t ray_tracing_tier{};
};

[[nodiscard]] NativeRayTracingCapability probe_native_raytracing_capability(
    NativeRayTracingBackend backend);
[[nodiscard]] NativeRayTracingCapability probe_d3d12_raytracing_capability();
[[nodiscard]] NativeRayTracingCapability probe_vulkan_raytracing_capability();

} // namespace noemancer
