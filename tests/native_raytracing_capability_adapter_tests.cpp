#include "runtime/native_raytracing_capability_adapter.hpp"

#include <iostream>
#include <string_view>

namespace {

using namespace noemancer;

bool check(const bool condition, const std::string_view message) {
    if (!condition) std::cerr << "native_raytracing_capability_adapter_tests: " << message << '\n';
    return condition;
}

bool validate_result(const NativeRayTracingCapability& result,
                     const NativeRayTracingBackend expected_backend) {
    if (!check(result.schema == native_raytracing_capability_schema,
               "capability schema drifted")) return false;
    if (!check(result.backend == native_raytracing_backend_name(expected_backend),
               "backend identity is not stable")) return false;
    if (!check(result.code.size() <= native_raytracing_capability_max_text_bytes &&
                   result.detail.size() <= native_raytracing_capability_max_text_bytes &&
                   result.device_name.size() <= native_raytracing_capability_max_text_bytes,
               "probe text exceeded the bounded plain-data contract")) return false;
    if (!check(result.supported_device_count <= result.device_count,
               "supported device count exceeded enumerated device count")) return false;
    if (!check(!result.code.empty() && !result.detail.empty(),
               "probe did not return a diagnostic code and detail")) return false;
    if (!check(result.state == NativeRayTracingProbeState::unavailable ||
                   result.loader_available,
               "a non-unavailable result did not establish loader availability")) return false;
    if (result.state == NativeRayTracingProbeState::supported) {
        if (!check(result.device_query_completed && result.feature_query_completed &&
                       result.supported_device_count > 0U,
                   "supported result omitted complete device/feature evidence")) return false;
        if (expected_backend == NativeRayTracingBackend::d3d12) {
            if (!check(result.native_device_created && result.ray_tracing_tier > 0U,
                       "D3D12 supported result omitted OPTIONS5 tier evidence")) return false;
        } else if (!check(result.acceleration_structure_extension &&
                              result.ray_tracing_pipeline_extension &&
                              result.deferred_host_operations_extension &&
                              result.buffer_device_address_extension &&
                              result.acceleration_structure_feature &&
                              result.ray_tracing_pipeline_feature &&
                              result.buffer_device_address_feature,
                          "Vulkan supported result omitted required RT extension/feature evidence"))
            return false;
    }
    return true;
}

bool test_vocabulary_and_platform_probes() {
    if (!check(native_raytracing_backend_name(NativeRayTracingBackend::d3d12) == "d3d12" &&
                   native_raytracing_backend_name(NativeRayTracingBackend::vulkan) == "vulkan",
               "backend vocabulary drifted")) return false;
    if (!check(native_raytracing_probe_state_name(NativeRayTracingProbeState::unavailable) ==
                   "unavailable" &&
                   native_raytracing_probe_state_name(NativeRayTracingProbeState::unsupported) ==
                   "unsupported" &&
                   native_raytracing_probe_state_name(NativeRayTracingProbeState::supported) ==
                   "supported" &&
                   native_raytracing_probe_state_name(NativeRayTracingProbeState::query_failed) ==
                   "query-failed",
               "probe-state vocabulary drifted")) return false;

    const auto d3d12 = probe_d3d12_raytracing_capability();
    const auto d3d12_dispatch = probe_native_raytracing_capability(NativeRayTracingBackend::d3d12);
    if (!validate_result(d3d12, NativeRayTracingBackend::d3d12) ||
        !validate_result(d3d12_dispatch, NativeRayTracingBackend::d3d12)) return false;
    if (!check(d3d12.backend == d3d12_dispatch.backend &&
                   d3d12.state == d3d12_dispatch.state,
               "D3D12 dispatch and direct probe disagree")) return false;

    const auto vulkan = probe_vulkan_raytracing_capability();
    const auto vulkan_dispatch = probe_native_raytracing_capability(NativeRayTracingBackend::vulkan);
    if (!validate_result(vulkan, NativeRayTracingBackend::vulkan) ||
        !validate_result(vulkan_dispatch, NativeRayTracingBackend::vulkan)) return false;
    return check(vulkan.backend == vulkan_dispatch.backend &&
                     vulkan.state == vulkan_dispatch.state,
                 "Vulkan dispatch and direct probe disagree");
}

bool test_supported_state_is_not_fabricated() {
    const auto d3d12 = probe_d3d12_raytracing_capability();
    if (d3d12.state == NativeRayTracingProbeState::supported &&
        !check(d3d12.ray_tracing_tier > 0U && d3d12.native_device_created,
               "D3D12 supported state was not backed by OPTIONS5/device evidence")) return false;
    const auto vulkan = probe_vulkan_raytracing_capability();
    if (vulkan.state == NativeRayTracingProbeState::supported &&
        !check(vulkan.acceleration_structure_extension &&
                   vulkan.ray_tracing_pipeline_extension &&
                   vulkan.deferred_host_operations_extension &&
                   vulkan.buffer_device_address_extension,
               "Vulkan supported state was not backed by extension evidence")) return false;
    return true;
}

} // namespace

int main() {
    if (!test_vocabulary_and_platform_probes()) return 1;
    if (!test_supported_state_is_not_fabricated()) return 2;
    std::cout << "native_raytracing_capability_adapter_tests: ok\n";
    return 0;
}
