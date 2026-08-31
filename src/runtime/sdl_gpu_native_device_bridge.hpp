#pragma once

#include <cstdint>
#include <string>

struct SDL_GPUDevice;

namespace noemancer {

enum class SdlGpuNativeBackend : std::uint8_t {
    unavailable,
    d3d12,
    vulkan,
};

// Runtime-private borrowed handles. They are intentionally absent from every
// JSON/Agent receipt and become invalid when the owning SDL_GPUDevice dies.
// Contexts that retain COM handles must AddRef; Vulkan contexts must never
// destroy borrowed objects.
struct SdlGpuNativeDeviceHandles final {
    SdlGpuNativeBackend backend{SdlGpuNativeBackend::unavailable};
    void* instance{};
    void* physical_device{};
    void* device{};
    void* queue{};
    std::uint32_t queue_family{};
    bool complete{};
};

// Safe semantic projection. No native address crosses this boundary.
struct SdlGpuNativeDeviceObservation final {
    std::string backend{"unavailable"};
    std::string code{"sdl-gpu-native-device.not-inspected"};
    std::string detail;
    bool properties_available{};
    bool device_borrowed{};
    bool queue_borrowed{};
    bool same_device_candidate{};
};

struct SdlGpuNativeDeviceBridgeResult final {
    SdlGpuNativeDeviceHandles handles;
    SdlGpuNativeDeviceObservation observation;
};

[[nodiscard]] SdlGpuNativeDeviceBridgeResult inspect_sdl_gpu_native_device(
    SDL_GPUDevice* device) noexcept;

} // namespace noemancer
