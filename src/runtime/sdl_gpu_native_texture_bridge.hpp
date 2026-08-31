#pragma once

#include <SDL3/SDL_gpu.h>

#include <cstdint>
#include <string>

namespace noemancer {

// One non-cycling SDL_GPU texture whose backend resource is borrowed by the
// Runtime-native interop adapter. The native pointer is never serialized.
struct SdlGpuNativeTextureExport final {
    SDL_GPUTexture* texture{};
    SDL_PropertiesID properties{};
    void* native_resource{};
    void* native_view{};
    std::string backend;
    std::string format;
    std::string code{"sdl-gpu-native-texture.not-created"};
    std::uint32_t width{};
    std::uint32_t height{};
    std::uint64_t generation{};
    bool ready{};
};

[[nodiscard]] SdlGpuNativeTextureExport create_sdl_gpu_native_rt_texture(
    SDL_GPUDevice* device, std::uint32_t width, std::uint32_t height,
    std::uint64_t generation) noexcept;
void release_sdl_gpu_native_rt_texture(
    SDL_GPUDevice* device, SdlGpuNativeTextureExport& value) noexcept;

} // namespace noemancer
