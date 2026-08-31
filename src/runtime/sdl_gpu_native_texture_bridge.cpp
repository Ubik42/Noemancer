#include "runtime/sdl_gpu_native_texture_bridge.hpp"

#include <string_view>

namespace noemancer {

SdlGpuNativeTextureExport create_sdl_gpu_native_rt_texture(
    SDL_GPUDevice* device, const std::uint32_t width,
    const std::uint32_t height, const std::uint64_t generation) noexcept {
    SdlGpuNativeTextureExport result;
    result.width = width;
    result.height = height;
    result.generation = generation;
    result.format = "R32G32B32A32_UINT";
    if (device == nullptr || width == 0U || height == 0U || generation == 0U) {
        result.code = "sdl-gpu-native-texture.request-invalid";
        return result;
    }
    result.backend = SDL_GetGPUDeviceDriver(device);
    if (result.backend != "direct3d12" && result.backend != "vulkan") {
        result.code = "sdl-gpu-native-texture.backend-unsupported";
        return result;
    }
    result.properties = SDL_CreateProperties();
    if (result.properties == 0U) {
        result.code = "sdl-gpu-native-texture.properties-failed";
        return result;
    }
    SDL_SetBooleanProperty(result.properties, "Noemancer.gpu.native.export", true);
    SDL_SetStringProperty(result.properties, SDL_PROP_GPU_TEXTURE_CREATE_NAME_STRING,
                          "Noemancer Native RT Output");
    SDL_GPUTextureCreateInfo info{};
    info.type = SDL_GPU_TEXTURETYPE_2D;
    info.format = SDL_GPU_TEXTUREFORMAT_R32G32B32A32_UINT;
    info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER |
        SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_WRITE;
    info.width = width;
    info.height = height;
    info.layer_count_or_depth = 1U;
    info.num_levels = 1U;
    info.sample_count = SDL_GPU_SAMPLECOUNT_1;
    info.props = result.properties;
    result.texture = SDL_CreateGPUTexture(device, &info);
    if (result.texture == nullptr) {
        result.code = "sdl-gpu-native-texture.create-failed";
        return result;
    }
    if (result.backend == "direct3d12") {
        result.native_resource = SDL_GetPointerProperty(
            result.properties, "Noemancer.gpu.native.d3d12.texture", nullptr);
    } else {
        result.native_resource = SDL_GetPointerProperty(
            result.properties, "Noemancer.gpu.native.vulkan.image", nullptr);
        result.native_view = SDL_GetPointerProperty(
            result.properties, "Noemancer.gpu.native.vulkan.image_view", nullptr);
    }
    result.ready = result.native_resource != nullptr;
    result.code = result.ready ? "sdl-gpu-native-texture.ready"
                               : "sdl-gpu-native-texture.handle-unavailable";
    return result;
}

void release_sdl_gpu_native_rt_texture(
    SDL_GPUDevice* device, SdlGpuNativeTextureExport& value) noexcept {
    if (device != nullptr && value.texture != nullptr)
        SDL_ReleaseGPUTexture(device, value.texture);
    if (value.properties != 0U) SDL_DestroyProperties(value.properties);
    value = {};
}

} // namespace noemancer
