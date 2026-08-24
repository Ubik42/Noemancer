#pragma once

#include <SDL3/SDL_gpu.h>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace noemancer {

struct RuntimeTextureStreamLevel final {
    std::uint32_t level{};
    std::uint32_t width{};
    std::uint32_t height{};
    std::uint32_t pixels_per_row{};
    std::uint64_t offset{};
    std::uint64_t row_bytes{};
    std::uint64_t row_count{};
    std::uint64_t source_bytes{};
    [[nodiscard]] std::uint64_t copy_bytes() const noexcept { return row_bytes*row_count; }
};

// Runtime-owned execution state for one immutable KTX2 artifact. The texture is
// allocated at its final shape, while the transfer buffer remains private to
// the adapter until all authored detail mips have crossed the frame scheduler.
struct RuntimeTextureStream final {
    RuntimeTextureStream() = default;
    RuntimeTextureStream(const RuntimeTextureStream&) = delete;
    RuntimeTextureStream& operator=(const RuntimeTextureStream&) = delete;
    RuntimeTextureStream(RuntimeTextureStream&&) noexcept = default;
    RuntimeTextureStream& operator=(RuntimeTextureStream&&) noexcept = default;

    SDL_GPUTexture* texture{};
    SDL_GPUTransferBuffer* transfer{};
    SDL_GPUTextureFormat gpu_format{SDL_GPU_TEXTUREFORMAT_INVALID};
    bool valid{};
    bool native_compressed{};
    std::string asset_id;
    bool linear_semantic{};
    bool streaming_enabled{true};
    std::uint32_t authored_priority{500U};
    std::uint8_t authored_importance{1U};
    std::string code;
    std::string detail;
    std::string format;
    std::uint32_t width{};
    std::uint32_t height{};
    std::uint32_t level_count{};
    std::uint32_t tail_level_count{};
    std::uint32_t resident_mip_start{};
    std::uint32_t target_mip_start{};
    std::uint32_t maximum_mip_start{};
    std::uint32_t screen_mip_start{};
    std::uint32_t demand_age_frames{};
    std::uint32_t visibility_age_frames{};
    std::uint64_t last_used_frame{};
    bool physically_evicted{};
    std::uint32_t minimum_resident_level{};
    std::int32_t next_detail_level{-1};
    std::uint32_t uploaded_level_count{};
    std::uint64_t source_bytes{};
    std::uint64_t resident_bytes{};
    std::uint64_t full_chain_bytes{};
    std::uint64_t tail_bytes{};
    std::uint64_t staging_bytes{};
    std::uint64_t uploaded_source_bytes{};
    std::uint64_t uploaded_copy_bytes{};
    std::vector<RuntimeTextureStreamLevel> levels;

    // A physical residency rebase is recorded into a frame command buffer
    // before SDL knows whether that command buffer will be submitted. Keep
    // the old resource and committed counters alive until the owner calls
    // commit_texture_stream_transition(). This makes cancel/failure paths
    // able to restore the stream without exposing a dangling texture.
    bool transition_pending{};
    SDL_GPUTexture* transition_previous_texture{};
    std::uint32_t transition_previous_resident_mip_start{};
    std::uint32_t transition_previous_minimum_resident_level{};
    std::int32_t transition_previous_next_detail_level{-1};
    std::uint32_t transition_previous_uploaded_level_count{};
    std::uint64_t transition_previous_resident_bytes{};
    std::uint64_t transition_previous_uploaded_source_bytes{};
    std::uint64_t transition_previous_uploaded_copy_bytes{};

    [[nodiscard]] bool complete() const noexcept { return valid&&resident_mip_start==0U; }
    [[nodiscard]] bool at_target() const noexcept { return valid&&resident_mip_start==target_mip_start; }
};

struct RuntimeTextureStreamStep final {
    bool valid{};
    bool uploaded{};
    std::uint32_t level{};
    std::uint64_t source_bytes{};
    std::uint64_t copy_bytes{};
    std::uint64_t resident_bytes_before{};
    std::uint64_t resident_bytes_after{};
    SDL_GPUTexture* previous_texture{};
    SDL_GPUTexture* texture{};
    std::string code;
    std::string detail;
};

[[nodiscard]] RuntimeTextureStream create_ktx2_texture_stream(
    SDL_GPUDevice* device,
    std::span<const std::byte> payload,
    bool srgb,
    std::uint32_t initial_tail_levels = 4U);

[[nodiscard]] RuntimeTextureStreamStep record_texture_stream_detail(
    SDL_GPUDevice* device,
    SDL_GPUCommandBuffer* command,
    RuntimeTextureStream& stream);

[[nodiscard]] RuntimeTextureStreamStep record_texture_stream_rebase(
    SDL_GPUDevice* device,
    SDL_GPUCommandBuffer* command,
    RuntimeTextureStream& stream,
    std::uint32_t target_mip_start);

// The record function only stages a replacement resource and updates the
// stream's effective (current-frame) view. The caller must call exactly one
// of these after SDL_SubmitGPUCommandBuffer* succeeds or fails/cancels.
void commit_texture_stream_transition(SDL_GPUDevice* device, RuntimeTextureStream& stream);
void rollback_texture_stream_transition(SDL_GPUDevice* device, RuntimeTextureStream& stream);

void release_texture_stream(SDL_GPUDevice* device, RuntimeTextureStream& stream);

} // namespace noemancer
