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
        // CPU RGBA snapshots are a separate, bounded retained-UI cache. They
        // reuse successful upload decodes and never keep decoder/GPU handles.
        std::size_t max_cpu_snapshot_bytes{64U * 1024U * 1024U};
        std::size_t max_cpu_snapshot_count{256U};
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

    struct CpuSnapshotResult final {
        bool success{};
        std::string code;
        std::string detail;
        std::string asset_id;
        std::uint32_t width{};
        std::uint32_t height{};
        std::string source_fingerprint;
        std::uint64_t generation{};
        std::vector<std::uint8_t> rgba8;
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
    // Returns a copy so callers cannot outlive or mutate cache residency.
    // The byte budget is checked before allocating/publishing the copy.
    [[nodiscard]] CpuSnapshotResult cpu_snapshot(
        std::string_view asset_id,
        std::size_t byte_budget = 64U * 1024U * 1024U);
    [[nodiscard]] std::string status_json(std::size_t max_bytes = 16U * 1024U) const;
    [[nodiscard]] const std::string& last_error() const noexcept { return last_error_; }
    [[nodiscard]] std::size_t resident_count() const noexcept { return entries_.size(); }
    [[nodiscard]] std::size_t resident_bytes() const noexcept { return resident_bytes_; }
    [[nodiscard]] std::size_t cpu_snapshot_count() const noexcept { return cpu_snapshots_.size(); }
    [[nodiscard]] std::size_t cpu_snapshot_bytes() const noexcept { return cpu_snapshot_bytes_; }
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

    struct CpuSnapshotEntry final {
        std::uint32_t width{};
        std::uint32_t height{};
        std::string source_fingerprint;
        std::uint64_t generation{};
        std::uint64_t last_access{};
        std::vector<std::uint8_t> rgba8;
    };

    struct PendingCpuSnapshot final {
        std::string asset_id;
        std::uint32_t width{};
        std::uint32_t height{};
        std::string source_fingerprint;
        std::vector<std::uint8_t> rgba8;
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
    void publish_cpu_snapshot(PendingCpuSnapshot snapshot);

    SDL_GPUDevice* device_{};
    TextureResourceTable local_texture_resources_;
    TextureResourceTable* texture_resources_{&local_texture_resources_};
    Limits limits_{};
    std::unordered_map<std::string, Entry> entries_;
    std::vector<PendingResourceUpdate> pending_resource_updates_;
    std::vector<PendingCpuSnapshot> pending_cpu_snapshots_;
    std::vector<std::string> pending_evictions_;
    std::size_t resident_bytes_{};
    std::unordered_map<std::string, CpuSnapshotEntry> cpu_snapshots_;
    std::size_t cpu_snapshot_bytes_{};
    std::uint64_t cpu_snapshot_generation_{};
    std::uint64_t cpu_snapshot_access_serial_{};
    std::uint64_t access_serial_{};
    std::uint64_t upload_count_{};
    std::uint64_t cache_hit_count_{};
    std::uint64_t failure_count_{};
    std::string last_error_;
    std::string last_code_;
    std::string last_detail_;
};

} // namespace noemancer
