#include "runtime/sdl_gpu_native_device_bridge.hpp"

#include <SDL3/SDL_gpu.h>
#include <SDL3/SDL_properties.h>

#include <limits>
#include <string_view>

namespace noemancer {
namespace {

constexpr const char* d3d12_device_property =
    "Noemancer.gpu.native.d3d12.device";
constexpr const char* d3d12_queue_property =
    "Noemancer.gpu.native.d3d12.command_queue";
constexpr const char* vulkan_instance_property =
    "Noemancer.gpu.native.vulkan.instance";
constexpr const char* vulkan_physical_device_property =
    "Noemancer.gpu.native.vulkan.physical_device";
constexpr const char* vulkan_device_property =
    "Noemancer.gpu.native.vulkan.device";
constexpr const char* vulkan_queue_property =
    "Noemancer.gpu.native.vulkan.queue";
constexpr const char* vulkan_queue_family_property =
    "Noemancer.gpu.native.vulkan.queue_family";

} // namespace

SdlGpuNativeDeviceBridgeResult inspect_sdl_gpu_native_device(
    SDL_GPUDevice* device) noexcept {
    SdlGpuNativeDeviceBridgeResult result;
    if (device == nullptr) {
        result.observation.code = "sdl-gpu-native-device.device-null";
        result.observation.detail = "The SDL_GPU device is null.";
        return result;
    }
    const auto properties = SDL_GetGPUDeviceProperties(device);
    result.observation.properties_available = properties != 0U;
    if (properties == 0U) {
        result.observation.code = "sdl-gpu-native-device.properties-unavailable";
        result.observation.detail = "SDL_GPU did not publish device properties.";
        return result;
    }
    const std::string_view backend = SDL_GetGPUDeviceDriver(device);
    if (backend == "direct3d12") {
        result.handles.backend = SdlGpuNativeBackend::d3d12;
        result.handles.device = SDL_GetPointerProperty(
            properties, d3d12_device_property, nullptr);
        result.handles.queue = SDL_GetPointerProperty(
            properties, d3d12_queue_property, nullptr);
        result.handles.complete = result.handles.device != nullptr &&
            result.handles.queue != nullptr;
        result.observation.backend = "d3d12";
    } else if (backend == "vulkan") {
        result.handles.backend = SdlGpuNativeBackend::vulkan;
        result.handles.instance = SDL_GetPointerProperty(
            properties, vulkan_instance_property, nullptr);
        result.handles.physical_device = SDL_GetPointerProperty(
            properties, vulkan_physical_device_property, nullptr);
        result.handles.device = SDL_GetPointerProperty(
            properties, vulkan_device_property, nullptr);
        result.handles.queue = SDL_GetPointerProperty(
            properties, vulkan_queue_property, nullptr);
        const auto queue_family = SDL_GetNumberProperty(
            properties, vulkan_queue_family_property, -1);
        if (queue_family >= 0 &&
            queue_family <= static_cast<Sint64>(std::numeric_limits<std::uint32_t>::max()))
            result.handles.queue_family = static_cast<std::uint32_t>(queue_family);
        result.handles.complete = result.handles.instance != nullptr &&
            result.handles.physical_device != nullptr &&
            result.handles.device != nullptr && result.handles.queue != nullptr &&
            queue_family >= 0;
        result.observation.backend = "vulkan";
    } else {
        result.observation.backend = std::string(backend);
        result.observation.code = "sdl-gpu-native-device.backend-unsupported";
        result.observation.detail =
            "Only the SDL_GPU D3D12 and Vulkan backends expose a native RT bridge.";
        return result;
    }
    result.observation.device_borrowed = result.handles.device != nullptr;
    result.observation.queue_borrowed = result.handles.queue != nullptr;
    result.observation.same_device_candidate = result.handles.complete;
    result.observation.code = result.handles.complete
        ? "sdl-gpu-native-device.ready"
        : "sdl-gpu-native-device.handles-incomplete";
    result.observation.detail = result.handles.complete
        ? "SDL_GPU published a complete borrowed native device/queue boundary."
        : "SDL_GPU native device properties were present but incomplete.";
    return result;
}

} // namespace noemancer
