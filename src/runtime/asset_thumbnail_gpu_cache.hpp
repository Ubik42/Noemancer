#pragma once

#include "runtime/texture_resource_table.hpp"

#include <SDL3/SDL_gpu.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace noemancer {

// The cache owns only the GPU presentation of a thumbnail.  The asset
// registry and thumbnail job own the artifact; this class deliberately keeps
// a local-path boundary so the editor can choose where cooked previews live.
// All methods are same-thread by design: a frame calls sync() before its
// render pass, then uses texture_for() or the returned texture pointers.
class AssetThumbnailGpuCache final {
public:
    struct Limits final {
        std::size_t max_encoded_bytes{16U * 1024U * 1024U};
        std::size_t max_decoded_bytes{64U * 1024U * 1024U};
        std::uint32_t max_width{4096U};
        std::uint32_t max_height{4096U};
        std::uint64_t max_pixels{16ULL * 1024ULL * 1024ULL};
        std::size_t max_resident_bytes{256U * 1024U * 1024U};
        std::size_t max_resident_textures{512U};
        std::size_t max_batch_requests{256U};
    };

    struct Request final {
        std::string asset_id;
        std::filesystem::path artifact_path;
    };

    struct RequestResult final {
        std::string asset_id;
        bool success{};
        bool cache_hit{};
        bool uploaded{};
        bool stale{};
        SDL_GPUTexture* texture{};
        std::uint32_t width{};
        std::uint32_t height{};
        std::string code;
        std::string detail;
    };

    struct SyncResult final {
        std::string schema{"noemancer.asset-thumbnail-gpu-sync/0.1"};
        bool success{};
        bool copy_pass_used{};
        std::size_t requested_count{};
        std::size_t changed_count{};
        std::size_t uploaded_count{};
        std::size_t cache_hit_count{};
        std::size_t failed_count{};
        std::vector<RequestResult> results;
        std::string code;
        std::string detail;
    };

    explicit AssetThumbnailGpuCache(SDL_GPUDevice* device);
    AssetThumbnailGpuCache(SDL_GPUDevice* device, Limits limits);
    AssetThumbnailGpuCache(SDL_GPUDevice* device, TextureResourceTable& texture_resources);
    AssetThumbnailGpuCache(SDL_GPUDevice* device, TextureResourceTable& texture_resources, Limits limits);
    ~AssetThumbnailGpuCache();

    AssetThumbnailGpuCache(const AssetThumbnailGpuCache&) = delete;
    AssetThumbnailGpuCache& operator=(const AssetThumbnailGpuCache&) = delete;
    AssetThumbnailGpuCache(AssetThumbnailGpuCache&&) = delete;
    AssetThumbnailGpuCache& operator=(AssetThumbnailGpuCache&&) = delete;

    // Encodes uploads into the caller's command buffer.  The caller must call
    // this before beginning another copy/render/compute pass and must submit
    // the command buffer after sync() returns.  No command buffer is acquired
    // or submitted here, which keeps frame ownership explicit.
    [[nodiscard]] SyncResult sync(
        SDL_GPUCommandBuffer* command,
        std::span<const Request> requests);
    void commit_uploads();
    void rollback_uploads();

    [[nodiscard]] SDL_GPUTexture* texture_for(std::string_view asset_id) const noexcept;
    [[nodiscard]] RequestResult lookup(std::string_view asset_id) const;
    [[nodiscard]] std::string status_json(std::size_t max_bytes = 16U * 1024U) const;
    [[nodiscard]] const std::string& last_error() const noexcept { return last_error_; }
    [[nodiscard]] std::size_t resident_count() const noexcept { return entries_.size(); }
    [[nodiscard]] std::size_t resident_bytes() const noexcept { return resident_bytes_; }
    [[nodiscard]] bool initialized() const noexcept { return device_ != nullptr; }

    // Safe to call more than once.  It releases all owned textures but does
    // not release the SDL_GPUDevice, which remains owned by the application.
    void shutdown() noexcept;

private:
    struct SourceFingerprint final {
        std::filesystem::path path;
        std::filesystem::file_time_type modified{};
        std::uintmax_t bytes{};
    };

    struct Entry final {
        SourceFingerprint source;
        TextureResourceHandle handle;
        std::uint32_t width{};
        std::uint32_t height{};
        std::size_t bytes{};
        std::uint64_t last_access{};
    };

    struct PendingUpload final {
        std::size_t result_index{};
        SourceFingerprint source;
        std::uint32_t width{};
        std::uint32_t height{};
        std::vector<std::uint8_t> rgba8;
    };

    struct PendingResourceUpdate final {
        std::string asset_id;
        TextureResourceHandle handle;
        bool created{};
        SourceFingerprint previous_source;
        std::uint32_t previous_width{};
        std::uint32_t previous_height{};
        std::size_t previous_bytes{};
        std::uint64_t previous_last_access{};
    };

    [[nodiscard]] static bool source_fingerprint(
        const std::filesystem::path& path,
        SourceFingerprint& fingerprint,
        std::string& code,
        std::string& detail);
    [[nodiscard]] static bool same_source(
        const SourceFingerprint& left,
        const SourceFingerprint& right) noexcept;
    [[nodiscard]] bool ensure_capacity(
        std::size_t incoming_bytes,
        std::string_view keep_asset_id,
        const std::vector<std::string>& protected_asset_ids,
        std::string& code,
        std::string& detail);
    void release_entry(std::unordered_map<std::string, Entry>::iterator iterator) noexcept;
    void set_failure(RequestResult& result, std::string_view code, std::string_view detail,
                     const Entry* stale_entry = nullptr);
    void set_sync_failure(SyncResult& result, std::string_view code, std::string_view detail);

    SDL_GPUDevice* device_{};
    TextureResourceTable local_texture_resources_;
    TextureResourceTable* texture_resources_{&local_texture_resources_};
    Limits limits_{};
    std::unordered_map<std::string, Entry> entries_;
    std::vector<PendingResourceUpdate> pending_resource_updates_;
    std::vector<std::string> pending_evictions_;
    std::size_t resident_bytes_{};
    std::uint64_t access_serial_{};
    std::uint64_t upload_count_{};
    std::uint64_t cache_hit_count_{};
    std::uint64_t failure_count_{};
    std::string last_error_;
    std::string last_code_;
    std::string last_detail_;
};

} // namespace noemancer
