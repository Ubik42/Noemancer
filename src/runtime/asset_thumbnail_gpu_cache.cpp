#include "runtime/asset_thumbnail_gpu_cache.hpp"

#include "engine/image_decoder.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <limits>
#include <optional>
#include <string>
#include <system_error>
#include <unordered_set>
#include <utility>

namespace noemancer {
namespace {

using Json = nlohmann::json;

constexpr std::size_t kMaxAssetIdBytes = 256U;
constexpr std::size_t kMaxDiagnosticBytes = 256U;
constexpr std::size_t kDefaultEncodedBytes = 16U * 1024U * 1024U;
constexpr std::size_t kDefaultDecodedBytes = 64U * 1024U * 1024U;
constexpr std::size_t kDefaultResidentBytes = 256U * 1024U * 1024U;
constexpr std::size_t kDefaultResidentTextures = 512U;
constexpr std::size_t kDefaultBatchRequests = 256U;

std::string bounded_text(const std::string_view value) {
    if (value.size() <= kMaxDiagnosticBytes) return std::string(value);
    return std::string(value.substr(0U, kMaxDiagnosticBytes));
}

std::string bounded_json(const Json& value, const std::size_t max_bytes) {
    const auto budget = max_bytes == 0U ? 16U * 1024U : max_bytes;
    const auto encoded = value.dump();
    if (encoded.size() <= budget) return encoded;
    const Json minimal = {
        {"schema", "noemancer.asset-thumbnail-gpu-status/0.2"},
        {"initialized", false},
        {"code", "thumbnail-gpu.observation-byte-budget"},
        {"minimumRequiredBytes", encoded.size()}
    };
    const auto compact = minimal.dump();
    if (compact.size() <= budget) return compact;
    return R"({"schema":"noemancer.asset-thumbnail-gpu-status/0.2","initialized":false,"code":"thumbnail-gpu.observation-byte-budget"})";
}

std::uint8_t png_byte(const std::span<const std::byte> bytes, const std::size_t index) {
    return std::to_integer<std::uint8_t>(bytes[index]);
}

std::uint32_t read_be_u32(const std::span<const std::byte> bytes, const std::size_t offset) {
    return (static_cast<std::uint32_t>(png_byte(bytes, offset)) << 24U) |
        (static_cast<std::uint32_t>(png_byte(bytes, offset + 1U)) << 16U) |
        (static_cast<std::uint32_t>(png_byte(bytes, offset + 2U)) << 8U) |
        static_cast<std::uint32_t>(png_byte(bytes, offset + 3U));
}

bool png_header_dimensions(const std::span<const std::byte> encoded,
                           std::uint32_t& width, std::uint32_t& height,
                           std::string& code, std::string& detail) {
    constexpr std::array<std::uint8_t, 8> signature{
        0x89U, 0x50U, 0x4EU, 0x47U, 0x0DU, 0x0AU, 0x1AU, 0x0AU};
    if (encoded.size() < 24U) {
        code = "thumbnail-gpu.png-header-invalid";
        detail = "The PNG artifact is smaller than its required header.";
        return false;
    }
    for (std::size_t index = 0U; index < signature.size(); ++index) {
        if (png_byte(encoded, index) != signature[index]) {
            code = "thumbnail-gpu.png-header-invalid";
            detail = "The thumbnail artifact is not a PNG image.";
            return false;
        }
    }
    if (read_be_u32(encoded, 8U) != 13U ||
        png_byte(encoded, 12U) != static_cast<std::uint8_t>('I') ||
        png_byte(encoded, 13U) != static_cast<std::uint8_t>('H') ||
        png_byte(encoded, 14U) != static_cast<std::uint8_t>('D') ||
        png_byte(encoded, 15U) != static_cast<std::uint8_t>('R')) {
        code = "thumbnail-gpu.png-header-invalid";
        detail = "The PNG artifact does not contain a valid IHDR chunk.";
        return false;
    }
    width = read_be_u32(encoded, 16U);
    height = read_be_u32(encoded, 20U);
    if (width == 0U || height == 0U) {
        code = "thumbnail-gpu.png-dimensions-invalid";
        detail = "The PNG artifact has empty dimensions.";
        return false;
    }
    return true;
}

template <typename Fingerprint>
bool compute_source_fingerprint(const std::filesystem::path& path,
                        Fingerprint& fingerprint,
                        std::string& code, std::string& detail) {
    if (path.empty()) {
        code = "thumbnail-gpu.artifact-path-required";
        detail = "A local PNG artifact path is required.";
        return false;
    }
    std::error_code error;
    const auto status = std::filesystem::status(path, error);
    if (error || !std::filesystem::is_regular_file(status)) {
        code = "thumbnail-gpu.source-not-file";
        detail = "The thumbnail artifact is unavailable as a regular file.";
        return false;
    }
    const auto bytes = std::filesystem::file_size(path, error);
    if (error) {
        code = "thumbnail-gpu.source-stat-failed";
        detail = "The thumbnail artifact metadata could not be read.";
        return false;
    }
    const auto modified = std::filesystem::last_write_time(path, error);
    if (error) {
        code = "thumbnail-gpu.source-stat-failed";
        detail = "The thumbnail artifact timestamp could not be read.";
        return false;
    }
    fingerprint.path = path.lexically_normal();
    fingerprint.modified = modified;
    fingerprint.bytes = bytes;
    return true;
}

template <typename Fingerprint>
bool source_fingerprints_match(const Fingerprint& left,const Fingerprint& right) {
    return left.path == right.path && left.modified == right.modified && left.bytes == right.bytes;
}

bool read_png(const std::filesystem::path& path, const std::size_t max_bytes,
              std::vector<std::byte>& bytes, std::string& code, std::string& detail) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input) {
        code = "thumbnail-gpu.source-open-failed";
        detail = "The thumbnail artifact could not be opened.";
        return false;
    }
    const auto end = input.tellg();
    if (end <= 0) {
        code = "thumbnail-gpu.source-empty";
        detail = "The thumbnail artifact is empty.";
        return false;
    }
    const auto size = static_cast<std::uintmax_t>(end);
    if (size > max_bytes || size > static_cast<std::uintmax_t>(std::numeric_limits<std::size_t>::max()) ||
        size > static_cast<std::uintmax_t>(std::numeric_limits<std::streamsize>::max())) {
        code = "thumbnail-gpu.source-too-large";
        detail = "The thumbnail artifact exceeds the bounded upload budget.";
        return false;
    }
    bytes.resize(static_cast<std::size_t>(size));
    input.seekg(0, std::ios::beg);
    input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(size));
    if (!input || input.gcount() != static_cast<std::streamsize>(size)) {
        bytes.clear();
        code = "thumbnail-gpu.source-read-failed";
        detail = "The thumbnail artifact could not be read completely.";
        return false;
    }
    return true;
}

bool valid_asset_id(const std::string_view asset_id, std::string& code, std::string& detail) {
    if (asset_id.empty()) {
        code = "thumbnail-gpu.asset-id-required";
        detail = "A stable asset ID is required for thumbnail residency.";
        return false;
    }
    if (asset_id.size() > kMaxAssetIdBytes) {
        code = "thumbnail-gpu.asset-id-too-long";
        detail = "The stable asset ID exceeds the bounded request size.";
        return false;
    }
    return true;
}

bool valid_decoded_budget(const std::uint32_t width, const std::uint32_t height,
                          const std::size_t max_decoded_bytes,
                          const std::uint32_t max_width, const std::uint32_t max_height,
                          const std::uint64_t max_pixels,
                          std::size_t& payload_bytes, std::string& code, std::string& detail) {
    const auto pixels = static_cast<std::uint64_t>(width) * static_cast<std::uint64_t>(height);
    if (width == 0U || height == 0U || width > max_width || height > max_height || pixels > max_pixels) {
        code = "thumbnail-gpu.png-dimensions-exceed-limit";
        detail = "The thumbnail dimensions exceed the bounded GPU preview budget.";
        return false;
    }
    if (pixels > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max() / 4U)) {
        code = "thumbnail-gpu.png-payload-too-large";
        detail = "The decoded RGBA8 payload exceeds the host addressable budget.";
        return false;
    }
    payload_bytes = static_cast<std::size_t>(pixels) * 4U;
    if (payload_bytes > max_decoded_bytes ||
        payload_bytes > static_cast<std::size_t>(std::numeric_limits<Uint32>::max())) {
        code = "thumbnail-gpu.png-payload-too-large";
        detail = "The decoded RGBA8 payload exceeds the bounded GPU upload budget.";
        return false;
    }
    return true;
}

} // namespace

AssetThumbnailGpuCache::AssetThumbnailGpuCache(SDL_GPUDevice* device)
    : AssetThumbnailGpuCache(device, Limits{}) {}

AssetThumbnailGpuCache::AssetThumbnailGpuCache(SDL_GPUDevice* device, Limits limits)
    : device_(device), limits_(limits) {
    if (limits_.max_encoded_bytes == 0U) limits_.max_encoded_bytes = kDefaultEncodedBytes;
    if (limits_.max_decoded_bytes == 0U) limits_.max_decoded_bytes = kDefaultDecodedBytes;
    if (limits_.max_width == 0U) limits_.max_width = 4096U;
    if (limits_.max_height == 0U) limits_.max_height = 4096U;
    if (limits_.max_pixels == 0U) limits_.max_pixels = 16ULL * 1024ULL * 1024ULL;
    if (limits_.max_resident_bytes == 0U) limits_.max_resident_bytes = kDefaultResidentBytes;
    if (limits_.max_resident_textures == 0U) limits_.max_resident_textures = kDefaultResidentTextures;
    if (limits_.max_batch_requests == 0U) limits_.max_batch_requests = kDefaultBatchRequests;
}

AssetThumbnailGpuCache::AssetThumbnailGpuCache(
    SDL_GPUDevice* device, TextureResourceTable& texture_resources)
    : AssetThumbnailGpuCache(device, texture_resources, Limits{}) {}

AssetThumbnailGpuCache::AssetThumbnailGpuCache(
    SDL_GPUDevice* device, TextureResourceTable& texture_resources, Limits limits)
    : AssetThumbnailGpuCache(device, std::move(limits)) {
    texture_resources_ = &texture_resources;
}

AssetThumbnailGpuCache::~AssetThumbnailGpuCache() { shutdown(); }

bool AssetThumbnailGpuCache::source_fingerprint(const std::filesystem::path& path,
    SourceFingerprint& fingerprint,std::string& code,std::string& detail) {
    return compute_source_fingerprint(path,fingerprint,code,detail);
}

bool AssetThumbnailGpuCache::same_source(
    const SourceFingerprint& left,const SourceFingerprint& right) noexcept {
    return source_fingerprints_match(left,right);
}

void AssetThumbnailGpuCache::release_entry(
    const std::unordered_map<std::string, Entry>::iterator iterator) noexcept {
    if (iterator == entries_.end()) return;
    if (device_ != nullptr && texture_resources_ != nullptr)
        if (auto* texture = texture_resources_->remove(iterator->second.handle))
            SDL_ReleaseGPUTexture(device_, texture);
    if (resident_bytes_ >= iterator->second.bytes) {
        resident_bytes_ -= iterator->second.bytes;
    } else {
        resident_bytes_ = 0U;
    }
    entries_.erase(iterator);
}

bool AssetThumbnailGpuCache::ensure_capacity(
    const std::size_t incoming_bytes,
    const std::string_view keep_asset_id,
    const std::vector<std::string>& protected_asset_ids,
    std::string& code,
    std::string& detail) {
    if (incoming_bytes > limits_.max_resident_bytes) {
        code = "thumbnail-gpu.resident-budget-exceeded";
        detail = "The thumbnail texture exceeds the bounded resident GPU budget.";
        return false;
    }
    const auto existing = entries_.find(std::string(keep_asset_id));
    const auto existing_bytes = existing == entries_.end() ? 0U : existing->second.bytes;
    std::size_t effective_count = entries_.size();
    std::size_t effective_bytes = resident_bytes_;
    for (const auto& asset_id : pending_evictions_) {
        const auto pending = entries_.find(asset_id);
        if (pending == entries_.end()) continue;
        if (effective_count > 0U) --effective_count;
        effective_bytes = effective_bytes >= pending->second.bytes
            ? effective_bytes - pending->second.bytes : 0U;
    }
    auto projected_count = effective_count - (existing == entries_.end() ? 0U : 1U) + 1U;
    auto projected_bytes = effective_bytes - existing_bytes + incoming_bytes;
    const auto is_protected = [&protected_asset_ids](const std::string_view asset_id) {
        return std::ranges::find(protected_asset_ids, asset_id) != protected_asset_ids.end();
    };
    while (projected_count > limits_.max_resident_textures ||
           projected_bytes > limits_.max_resident_bytes) {
        auto victim = entries_.end();
        for (auto iterator = entries_.begin(); iterator != entries_.end(); ++iterator) {
            if (iterator->first == keep_asset_id || is_protected(iterator->first) ||
                std::ranges::find(pending_evictions_, iterator->first) != pending_evictions_.end()) continue;
            if (victim == entries_.end() || iterator->second.last_access < victim->second.last_access) {
                victim = iterator;
            }
        }
        if (victim == entries_.end()) {
            code = "thumbnail-gpu.resident-budget-exceeded";
            detail = "No evictable thumbnail texture remains within the bounded resident budget.";
            return false;
        }
        pending_evictions_.push_back(victim->first);
        if (projected_count > 0U) --projected_count;
        projected_bytes = projected_bytes >= victim->second.bytes
            ? projected_bytes - victim->second.bytes : 0U;
    }
    return true;
}

void AssetThumbnailGpuCache::set_failure(RequestResult& result, const std::string_view code,
                                         const std::string_view detail,
                                         const Entry* stale_entry) {
    result.success = false;
    result.cache_hit = false;
    result.uploaded = false;
    result.code = std::string(code);
    result.detail = bounded_text(detail);
    if (stale_entry != nullptr) {
        result.stale = true;
        result.texture = texture_resources_ == nullptr ? nullptr : texture_resources_->resolve(stale_entry->handle);
        result.width = stale_entry->width;
        result.height = stale_entry->height;
    }
    ++failure_count_;
}

void AssetThumbnailGpuCache::set_sync_failure(SyncResult& result, const std::string_view code,
                                               const std::string_view detail) {
    result.success = false;
    result.code = std::string(code);
    result.detail = bounded_text(detail);
    last_code_ = result.code;
    last_detail_ = result.detail;
    last_error_ = result.detail;
}

AssetThumbnailGpuCache::SyncResult AssetThumbnailGpuCache::sync(
    SDL_GPUCommandBuffer* command, const std::span<const Request> requests) {
    SyncResult result;
    if (!pending_resource_updates_.empty() || !pending_evictions_.empty()) {
        set_sync_failure(result, "thumbnail-gpu.submit-result-required",
                         "The previous thumbnail upload must be committed or rolled back before another sync.");
        return result;
    }
    result.requested_count = requests.size();
    if (requests.size() > limits_.max_batch_requests) {
        set_sync_failure(result, "thumbnail-gpu.batch-too-large",
                         "The thumbnail synchronization batch exceeds its bounded request budget.");
        return result;
    }
    if (requests.empty()) {
        result.success = true;
        result.code = "thumbnail-gpu.no-requests";
        result.detail = "No thumbnail synchronization was requested.";
        last_code_ = result.code;
        last_detail_ = result.detail;
        last_error_.clear();
        return result;
    }

    result.results.reserve(requests.size());
    std::unordered_set<std::string> seen_asset_ids;
    std::vector<PendingUpload> pending;
    pending.reserve(requests.size());

    for (const auto& request : requests) {
        RequestResult request_result;
        request_result.asset_id = request.asset_id.substr(0U, kMaxAssetIdBytes);
        result.results.push_back(std::move(request_result));
        auto& current = result.results.back();
        const auto existing = entries_.find(request.asset_id);
        std::string code;
        std::string detail;
        if (!valid_asset_id(request.asset_id, code, detail)) {
            set_failure(current, code, detail, existing == entries_.end() ? nullptr : &existing->second);
            ++result.failed_count;
            continue;
        }
        if (!seen_asset_ids.insert(request.asset_id).second) {
            set_failure(current, "thumbnail-gpu.duplicate-asset-id",
                        "The synchronization batch contains a duplicate stable asset ID.",
                        existing == entries_.end() ? nullptr : &existing->second);
            ++result.failed_count;
            continue;
        }

        SourceFingerprint source;
        if (!source_fingerprint(request.artifact_path, source, code, detail)) {
            set_failure(current, code, detail, existing == entries_.end() ? nullptr : &existing->second);
            ++result.failed_count;
            continue;
        }
        if (existing != entries_.end() && same_source(existing->second.source, source)) {
            current.success = true;
            current.cache_hit = true;
            current.texture = texture_resources_->resolve(existing->second.handle);
            current.width = existing->second.width;
            current.height = existing->second.height;
            current.code = "thumbnail-gpu.cache-hit";
            current.detail = "The PNG artifact fingerprint is unchanged; the GPU texture was reused.";
            existing->second.last_access = ++access_serial_;
            ++result.cache_hit_count;
            ++cache_hit_count_;
            continue;
        }

        ++result.changed_count;
        std::vector<std::byte> encoded;
        if (!read_png(request.artifact_path, limits_.max_encoded_bytes, encoded, code, detail)) {
            set_failure(current, code, detail, existing == entries_.end() ? nullptr : &existing->second);
            ++result.failed_count;
            continue;
        }
        std::uint32_t header_width{};
        std::uint32_t header_height{};
        if (!png_header_dimensions(std::span<const std::byte>(encoded.data(), encoded.size()),
                                   header_width, header_height, code, detail)) {
            set_failure(current, code, detail, existing == entries_.end() ? nullptr : &existing->second);
            ++result.failed_count;
            continue;
        }
        std::size_t payload_bytes{};
        if (!valid_decoded_budget(header_width, header_height, limits_.max_decoded_bytes,
                                  limits_.max_width, limits_.max_height, limits_.max_pixels,
                                  payload_bytes, code, detail)) {
            set_failure(current, code, detail, existing == entries_.end() ? nullptr : &existing->second);
            ++result.failed_count;
            continue;
        }
        const auto decoded = decode_png_rgba8(
            std::span<const std::byte>(encoded.data(), encoded.size()));
        if (!decoded.valid || decoded.width != header_width || decoded.height != header_height ||
            decoded.rgba8.size() != payload_bytes) {
            set_failure(current, "thumbnail-gpu.png-decode-failed",
                        "The existing PNG decoder rejected the thumbnail artifact.",
                        existing == entries_.end() ? nullptr : &existing->second);
            ++result.failed_count;
            continue;
        }
        SourceFingerprint after_read;
        if (!source_fingerprint(request.artifact_path, after_read, code, detail) ||
            !same_source(source, after_read)) {
            set_failure(current, "thumbnail-gpu.source-changed-during-read",
                        "The thumbnail artifact changed while it was being decoded.",
                        existing == entries_.end() ? nullptr : &existing->second);
            ++result.failed_count;
            continue;
        }
        pending.push_back(PendingUpload{
            .result_index = result.results.size() - 1U,
            .source = std::move(source),
            .width = decoded.width,
            .height = decoded.height,
            .rgba8 = decoded.rgba8});
    }

    if (pending.empty()) {
        result.success = result.failed_count == 0U;
        result.code = result.success ? "thumbnail-gpu.cache-synchronized" :
            "thumbnail-gpu.sync-failed";
        result.detail = result.success
            ? "All requested thumbnail textures were already resident."
            : "No changed thumbnail artifact could be uploaded.";
        last_code_ = result.code;
        last_detail_ = result.detail;
        last_error_ = result.success ? std::string{} : result.detail;
        return result;
    }
    if (device_ == nullptr) {
        for (const auto& upload : pending) {
            const auto existing = entries_.find(result.results[upload.result_index].asset_id);
            set_failure(result.results[upload.result_index], "thumbnail-gpu.device-required",
                        "A live SDL GPU device is required to upload a changed thumbnail.",
                        existing == entries_.end() ? nullptr : &existing->second);
            ++result.failed_count;
        }
        result.success = false;
        result.code = "thumbnail-gpu.device-required";
        result.detail = "A live SDL GPU device is required to upload a changed thumbnail.";
        last_code_ = result.code;
        last_detail_ = result.detail;
        last_error_ = result.detail;
        return result;
    }
    if (command == nullptr) {
        for (const auto& upload : pending) {
            const auto existing = entries_.find(result.results[upload.result_index].asset_id);
            set_failure(result.results[upload.result_index], "thumbnail-gpu.command-buffer-required",
                        "A caller-owned SDL GPU command buffer is required for thumbnail upload.",
                        existing == entries_.end() ? nullptr : &existing->second);
            ++result.failed_count;
        }
        result.success = false;
        result.code = "thumbnail-gpu.command-buffer-required";
        result.detail = "A caller-owned SDL GPU command buffer is required for thumbnail upload.";
        last_code_ = result.code;
        last_detail_ = result.detail;
        last_error_ = result.detail;
        return result;
    }

    auto* copy_pass = SDL_BeginGPUCopyPass(command);
    if (copy_pass == nullptr) {
        for (const auto& upload : pending) {
            const auto existing = entries_.find(result.results[upload.result_index].asset_id);
            set_failure(result.results[upload.result_index], "thumbnail-gpu.copy-pass-begin-failed",
                        "The SDL GPU copy pass could not be started.",
                        existing == entries_.end() ? nullptr : &existing->second);
            ++result.failed_count;
        }
        result.success = false;
        result.code = "thumbnail-gpu.copy-pass-begin-failed";
        result.detail = "The SDL GPU copy pass could not be started.";
        last_code_ = result.code;
        last_detail_ = result.detail;
        last_error_ = result.detail;
        return result;
    }
    result.copy_pass_used = true;

    std::vector<std::string> protected_asset_ids;
    protected_asset_ids.reserve(requests.size());
    for (const auto& request : requests)
        if (!request.asset_id.empty()) protected_asset_ids.push_back(request.asset_id);

    for (const auto& upload : pending) {
        auto& current = result.results[upload.result_index];
        std::string code;
        std::string detail;
        const auto payload_bytes = upload.rgba8.size();
        const auto eviction_count_before = pending_evictions_.size();
        const auto restore_eviction_plan = [&] { pending_evictions_.resize(eviction_count_before); };
        if (!ensure_capacity(payload_bytes, current.asset_id, protected_asset_ids, code, detail)) {
            restore_eviction_plan();
            const auto existing = entries_.find(current.asset_id);
            set_failure(current, code, detail, existing == entries_.end() ? nullptr : &existing->second);
            ++result.failed_count;
            continue;
        }

        SDL_GPUTextureCreateInfo texture_info{};
        texture_info.type = SDL_GPU_TEXTURETYPE_2D;
        texture_info.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
        texture_info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
        texture_info.width = upload.width;
        texture_info.height = upload.height;
        texture_info.layer_count_or_depth = 1U;
        texture_info.num_levels = 1U;
        texture_info.sample_count = SDL_GPU_SAMPLECOUNT_1;
        auto* texture = SDL_CreateGPUTexture(device_, &texture_info);
        if (texture == nullptr) {
            restore_eviction_plan();
            set_failure(current, "thumbnail-gpu.texture-create-failed",
                        "SDL could not create the thumbnail texture.",
                        entries_.contains(current.asset_id) ? &entries_.at(current.asset_id) : nullptr);
            ++result.failed_count;
            continue;
        }
        SDL_GPUTransferBufferCreateInfo transfer_info{};
        transfer_info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
        transfer_info.size = static_cast<Uint32>(payload_bytes);
        auto* transfer = SDL_CreateGPUTransferBuffer(device_, &transfer_info);
        if (transfer == nullptr) {
            restore_eviction_plan();
            SDL_ReleaseGPUTexture(device_, texture);
            set_failure(current, "thumbnail-gpu.transfer-create-failed",
                        "SDL could not create the thumbnail upload buffer.",
                        entries_.contains(current.asset_id) ? &entries_.at(current.asset_id) : nullptr);
            ++result.failed_count;
            continue;
        }
        auto* mapped = SDL_MapGPUTransferBuffer(device_, transfer, false);
        if (mapped == nullptr) {
            restore_eviction_plan();
            SDL_ReleaseGPUTransferBuffer(device_, transfer);
            SDL_ReleaseGPUTexture(device_, texture);
            set_failure(current, "thumbnail-gpu.transfer-map-failed",
                        "SDL could not map the thumbnail upload buffer.",
                        entries_.contains(current.asset_id) ? &entries_.at(current.asset_id) : nullptr);
            ++result.failed_count;
            continue;
        }
        std::memcpy(mapped, upload.rgba8.data(), payload_bytes);
        SDL_UnmapGPUTransferBuffer(device_, transfer);
        const TextureResourceMetadata metadata{upload.width, upload.height, 1U, 0U, payload_bytes};
        auto existing = entries_.find(current.asset_id);
        TextureResourceHandle handle;
        if (existing != entries_.end()) {
            handle = existing->second.handle;
            if (!texture_resources_->stage_replacement(handle, texture, metadata)) {
                restore_eviction_plan();
                SDL_ReleaseGPUTransferBuffer(device_, transfer);
                SDL_ReleaseGPUTexture(device_, texture);
                set_failure(current, "thumbnail-gpu.resource-stage-failed",
                            "The stable thumbnail resource could not stage its replacement.", &existing->second);
                ++result.failed_count;
                continue;
            }
            pending_resource_updates_.push_back(PendingResourceUpdate{
                .asset_id=current.asset_id,.handle=handle,.created=false,
                .previous_source=existing->second.source,.previous_width=existing->second.width,
                .previous_height=existing->second.height,.previous_bytes=existing->second.bytes,
                .previous_last_access=existing->second.last_access});
            resident_bytes_ = resident_bytes_ >= existing->second.bytes
                ? resident_bytes_ - existing->second.bytes : 0U;
            existing->second = Entry{.source=upload.source,.handle=handle,.width=upload.width,
                .height=upload.height,.bytes=payload_bytes,.last_access=++access_serial_};
        } else {
            handle = texture_resources_->acquire({
                .stable_id=current.asset_id,.semantic="editor-thumbnail-rgba",
                .owner="ui.editor.thumbnail",.source=upload.source.path.generic_string(),
                .residency="resident",.metadata=metadata},texture);
            if (!handle.valid()) {
                restore_eviction_plan();
                SDL_ReleaseGPUTransferBuffer(device_, transfer);
                SDL_ReleaseGPUTexture(device_, texture);
                set_failure(current, "thumbnail-gpu.resource-acquire-failed",
                            "The stable thumbnail resource could not be acquired.");
                ++result.failed_count;
                continue;
            }
            entries_.emplace(current.asset_id, Entry{.source=upload.source,.handle=handle,
                .width=upload.width,.height=upload.height,.bytes=payload_bytes,
                .last_access=++access_serial_});
            pending_resource_updates_.push_back(PendingResourceUpdate{
                .asset_id=current.asset_id,.handle=handle,.created=true});
        }
        resident_bytes_ += payload_bytes;
        SDL_GPUTextureTransferInfo source_info{};
        source_info.transfer_buffer = transfer;
        source_info.offset = 0U;
        source_info.pixels_per_row = upload.width;
        source_info.rows_per_layer = upload.height;
        SDL_GPUTextureRegion destination{};
        destination.texture = texture;
        destination.mip_level = 0U;
        destination.layer = 0U;
        destination.w = upload.width;
        destination.h = upload.height;
        destination.d = 1U;
        SDL_UploadToGPUTexture(copy_pass, &source_info, &destination, false);
        SDL_ReleaseGPUTransferBuffer(device_, transfer);
        current.success = true;
        current.uploaded = true;
        current.cache_hit = false;
        current.texture = texture_resources_->resolve(handle);
        current.width = upload.width;
        current.height = upload.height;
        current.code = "thumbnail-gpu.uploaded";
        current.detail = "The PNG artifact was decoded and uploaded to an RGBA8 GPU texture.";
        ++result.uploaded_count;
    }
    SDL_EndGPUCopyPass(copy_pass);

    result.success = result.failed_count == 0U;
    result.code = result.success ? "thumbnail-gpu.synchronized" :
        (result.uploaded_count == 0U ? "thumbnail-gpu.sync-failed" : "thumbnail-gpu.partial-failure");
    result.detail = result.success
        ? "The requested thumbnail textures are resident and ready for sampling."
        : "Some thumbnail requests could not be uploaded; successful requests remain resident.";
    last_code_ = result.code;
    last_detail_ = result.detail;
    last_error_ = result.success ? std::string{} : result.detail;
    return result;
}

void AssetThumbnailGpuCache::commit_uploads() {
    for (const auto& update : pending_resource_updates_) {
        if (!update.created)
            if (auto* previous = texture_resources_->commit_replacement(update.handle))
                SDL_ReleaseGPUTexture(device_, previous);
        ++upload_count_;
    }
    for (const auto& asset_id : pending_evictions_) release_entry(entries_.find(asset_id));
    pending_resource_updates_.clear();
    pending_evictions_.clear();
}

void AssetThumbnailGpuCache::rollback_uploads() {
    for (auto cursor = pending_resource_updates_.rbegin();
         cursor != pending_resource_updates_.rend(); ++cursor) {
        const auto found = entries_.find(cursor->asset_id);
        if (found == entries_.end()) continue;
        if (resident_bytes_ >= found->second.bytes) resident_bytes_ -= found->second.bytes;
        else resident_bytes_ = 0U;
        if (cursor->created) {
            if (auto* rejected = texture_resources_->remove(cursor->handle))
                SDL_ReleaseGPUTexture(device_, rejected);
            entries_.erase(found);
        } else {
            if (auto* rejected = texture_resources_->rollback_replacement(cursor->handle))
                SDL_ReleaseGPUTexture(device_, rejected);
            found->second.source = cursor->previous_source;
            found->second.width = cursor->previous_width;
            found->second.height = cursor->previous_height;
            found->second.bytes = cursor->previous_bytes;
            found->second.last_access = cursor->previous_last_access;
            resident_bytes_ += cursor->previous_bytes;
        }
    }
    pending_resource_updates_.clear();
    pending_evictions_.clear();
}

SDL_GPUTexture* AssetThumbnailGpuCache::texture_for(const std::string_view asset_id) const noexcept {
    const auto found = entries_.find(std::string(asset_id));
    return found == entries_.end() || texture_resources_ == nullptr
        ? nullptr : texture_resources_->resolve(found->second.handle);
}

AssetThumbnailGpuCache::RequestResult AssetThumbnailGpuCache::lookup(
    const std::string_view asset_id) const {
    RequestResult result;
    result.asset_id = std::string(asset_id.substr(0U, kMaxAssetIdBytes));
    if (asset_id.empty()) {
        result.code = "thumbnail-gpu.asset-id-required";
        result.detail = "A stable asset ID is required for thumbnail lookup.";
        return result;
    }
    const auto found = entries_.find(std::string(asset_id));
    if (found == entries_.end()) {
        result.code = "thumbnail-gpu.not-resident";
        result.detail = "The requested thumbnail texture is not resident.";
        return result;
    }
    result.success = true;
    result.cache_hit = true;
    result.texture = texture_resources_->resolve(found->second.handle);
    result.width = found->second.width;
    result.height = found->second.height;
    result.code = "thumbnail-gpu.cache-hit";
    result.detail = "The requested thumbnail texture is resident.";
    return result;
}

std::string AssetThumbnailGpuCache::status_json(const std::size_t max_bytes) const {
    const Json status = {
        {"schema", "noemancer.asset-thumbnail-gpu-status/0.2"},
        {"initialized", device_ != nullptr},
        {"residentTextureCount", entries_.size()},
        {"residentBytes", resident_bytes_},
        {"uploadCount", upload_count_},
        {"cacheHitCount", cache_hit_count_},
        {"failureCount", failure_count_},
        {"pendingUploads", pending_resource_updates_.size()},
        {"pendingEvictions", pending_evictions_.size()},
        {"textureResources", Json::parse(texture_resources_->observe_json("ui.editor.thumbnail"))},
        {"limits", {
            {"maxEncodedBytes", limits_.max_encoded_bytes},
            {"maxDecodedBytes", limits_.max_decoded_bytes},
            {"maxWidth", limits_.max_width},
            {"maxHeight", limits_.max_height},
            {"maxPixels", limits_.max_pixels},
            {"maxResidentBytes", limits_.max_resident_bytes},
            {"maxResidentTextures", limits_.max_resident_textures},
            {"maxBatchRequests", limits_.max_batch_requests}}},
        {"lastCode", last_code_},
        {"lastDetail", last_detail_}
    };
    return bounded_json(status, max_bytes);
}

void AssetThumbnailGpuCache::shutdown() noexcept {
    rollback_uploads();
    if (device_ != nullptr && texture_resources_ != nullptr)
        for (const auto& entry : entries_)
            if (auto* texture = texture_resources_->remove(entry.second.handle))
                SDL_ReleaseGPUTexture(device_, texture);
    entries_.clear();
    resident_bytes_ = 0U;
    device_ = nullptr;
}

} // namespace noemancer
