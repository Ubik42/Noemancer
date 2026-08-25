#include "engine/image_decoder.hpp"
#include "runtime/asset_thumbnail_gpu_cache.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <span>
#include <string_view>

namespace {

bool write_png(const std::filesystem::path& path,
               const std::span<const std::uint8_t> rgba8) {
    const auto encoded = noemancer::encode_png_rgba8(2U, 1U, rgba8);
    if (!encoded.valid) return false;
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(reinterpret_cast<const char*>(encoded.bytes.data()),
                 static_cast<std::streamsize>(encoded.bytes.size()));
    return output.good();
}

bool upload_and_finish(noemancer::AssetThumbnailGpuCache& cache,
                       SDL_GPUDevice* device,
                       const std::string_view asset_id,
                       const std::filesystem::path& path,
                       const bool commit) {
    auto* command = SDL_AcquireGPUCommandBuffer(device);
    if (command == nullptr) return false;
    const noemancer::AssetThumbnailGpuCache::Request request{
        .asset_id = std::string(asset_id), .artifact_path = path};
    const auto sync = cache.sync(command, std::span(&request, 1U));
    if (!sync.success || sync.uploaded_count != 1U) {
        SDL_CancelGPUCommandBuffer(command);
        cache.rollback_uploads();
        return false;
    }
    if (commit) {
        if (!SDL_SubmitGPUCommandBuffer(command)) {
            cache.rollback_uploads();
            return false;
        }
        cache.commit_uploads();
    } else {
        if (!SDL_CancelGPUCommandBuffer(command)) return false;
        cache.rollback_uploads();
    }
    return true;
}

} // namespace

int main() {
    const auto root = std::filesystem::temp_directory_path() /
        "noemancer-asset-thumbnail-cpu-snapshot-tests";
    std::error_code error;
    std::filesystem::create_directories(root, error);
    if (error) return 1;
    const auto red_path = root / "red.png";
    const auto green_path = root / "green.png";
    constexpr std::array<std::uint8_t, 8U> red{
        255U, 0U, 0U, 255U, 255U, 0U, 0U, 255U};
    constexpr std::array<std::uint8_t, 8U> green{
        0U, 255U, 0U, 255U, 0U, 255U, 0U, 255U};
    if (!write_png(red_path, red) || !write_png(green_path, green)) return 2;

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        std::cerr << "SKIP: SDL video initialization unavailable: " << SDL_GetError() << '\n';
        return 0;
    }
    auto* device = SDL_CreateGPUDevice(
        SDL_GPU_SHADERFORMAT_SPIRV | SDL_GPU_SHADERFORMAT_DXIL, false, nullptr);
    if (device == nullptr) {
        std::cerr << "SKIP: SDL GPU device unavailable: " << SDL_GetError() << '\n';
        SDL_Quit();
        return 0;
    }

    noemancer::AssetThumbnailGpuCache::Limits limits;
    limits.max_cpu_snapshot_count = 1U;
    limits.max_cpu_snapshot_bytes = 8U;
    limits.max_resident_textures = 8U;
    noemancer::AssetThumbnailGpuCache cache(device, limits);

    if (!upload_and_finish(cache, device, "asset.red", red_path, true)) return 3;
    const auto first = cache.cpu_snapshot("asset.red", 8U);
    if (!first.success || first.width != 2U || first.height != 1U ||
        !std::ranges::equal(first.rgba8, red) ||
        first.source_fingerprint.empty() || first.generation == 0U ||
        cache.cpu_snapshot_count() != 1U || cache.cpu_snapshot_bytes() != 8U) return 4;
    const auto first_generation = first.generation;
    const auto first_fingerprint = first.source_fingerprint;

    const auto bounded = cache.cpu_snapshot("asset.red", 7U);
    if (bounded.success || bounded.code != "thumbnail-cpu.byte-budget-exceeded" ||
        !bounded.rgba8.empty()) return 5;

    {
        auto* command = SDL_AcquireGPUCommandBuffer(device);
        if (command == nullptr) return 11;
        const noemancer::AssetThumbnailGpuCache::Request request{
            .asset_id = "asset.red", .artifact_path = red_path};
        const auto sync = cache.sync(command, std::span(&request, 1U));
        if (!sync.success || sync.cache_hit_count != 1U || sync.uploaded_count != 0U ||
            !SDL_CancelGPUCommandBuffer(command)) return 12;
        cache.rollback_uploads();
        const auto hit = cache.cpu_snapshot("asset.red", 8U);
        if (!hit.success || hit.generation != first_generation ||
            hit.source_fingerprint != first_fingerprint) return 13;
    }

    const auto invalid_path = root / "invalid.png";
    {
        std::ofstream invalid(invalid_path, std::ios::binary | std::ios::trunc);
        invalid << "not a png";
    }
    {
        auto* command = SDL_AcquireGPUCommandBuffer(device);
        if (command == nullptr) return 14;
        const noemancer::AssetThumbnailGpuCache::Request request{
            .asset_id = "asset.red", .artifact_path = invalid_path};
        const auto sync = cache.sync(command, std::span(&request, 1U));
        if (sync.success || sync.failed_count != 1U || !SDL_CancelGPUCommandBuffer(command)) return 15;
        cache.rollback_uploads();
        const auto after_failure = cache.cpu_snapshot("asset.red", 8U);
        if (!after_failure.success || !std::ranges::equal(after_failure.rgba8, red) ||
            after_failure.generation != first_generation ||
            after_failure.source_fingerprint != first_fingerprint) return 16;
    }

    if (!upload_and_finish(cache, device, "asset.red", green_path, false)) return 6;
    const auto rolled_back = cache.cpu_snapshot("asset.red", 8U);
    if (!rolled_back.success || !std::ranges::equal(rolled_back.rgba8, red) ||
        rolled_back.generation != first_generation ||
        rolled_back.source_fingerprint != first_fingerprint) return 7;

    if (!upload_and_finish(cache, device, "asset.green", green_path, true)) return 8;
    const auto evicted = cache.cpu_snapshot("asset.red", 8U);
    const auto second = cache.cpu_snapshot("asset.green", 8U);
    if (evicted.success || evicted.code != "thumbnail-cpu.not-resident" ||
        !second.success || !std::ranges::equal(second.rgba8, green) ||
        second.generation <= first_generation ||
        second.source_fingerprint.empty() || cache.cpu_snapshot_count() != 1U ||
        cache.cpu_snapshot_bytes() != 8U) return 9;

    // A GPU-resident thumbnail whose bounded CPU snapshot was evicted must be
    // rehydrated on demand; otherwise revisiting an older Asset Browser page
    // would render a permanent blank preview behind a GPU cache hit.
    if (!upload_and_finish(cache, device, "asset.red", red_path, true)) return 17;
    const auto rehydrated = cache.cpu_snapshot("asset.red", 8U);
    if (!rehydrated.success || !std::ranges::equal(rehydrated.rgba8, red) ||
        rehydrated.generation <= second.generation || cache.cpu_snapshot_count() != 1U)
        return 18;

    cache.shutdown();
    if (cache.cpu_snapshot_count() != 0U || cache.cpu_snapshot_bytes() != 0U) return 10;
    SDL_DestroyGPUDevice(device);
    SDL_Quit();
    std::filesystem::remove_all(root, error);
    return 0;
}
