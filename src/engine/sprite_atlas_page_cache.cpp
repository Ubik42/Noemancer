#include "engine/sprite_atlas_page_cache.hpp"

#include "engine/content_hash.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <limits>
#include <sstream>
#include <span>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace noemancer {
namespace {

using Json = nlohmann::json;

constexpr std::array<std::uint8_t, 8> kRecordMagic{
    static_cast<std::uint8_t>('N'), static_cast<std::uint8_t>('M'),
    static_cast<std::uint8_t>('S'), static_cast<std::uint8_t>('P'),
    static_cast<std::uint8_t>('C'), static_cast<std::uint8_t>('0'),
    static_cast<std::uint8_t>('0'), static_cast<std::uint8_t>('1')};
constexpr std::uint32_t kRecordVersion = 1U;
constexpr std::string_view kCacheNamespace = "sprite-atlas-pages-v1";
constexpr std::string_view kRecordExtension = ".spc";
constexpr std::size_t kMaximumCacheKeyBytes = 256U;
constexpr std::size_t kFixedRecordHeaderBytes = kRecordMagic.size() +
    sizeof(std::uint32_t) + sizeof(std::uint32_t) + sizeof(std::uint64_t) + sizeof(std::uint32_t);
constexpr std::array<std::uint8_t, 12> kKtx2Identifier{
    0xABU, 0x4BU, 0x54U, 0x58U, 0x20U, 0x32U,
    0x30U, 0xBBU, 0x0DU, 0x0AU, 0x1AU, 0x0AU};

std::atomic<std::uint64_t> temporary_counter{};

struct CacheReadResult final {
    bool hit{};
    bool owned{};
    bool corrupt{};
    std::string code;
    std::string detail;
    std::vector<std::byte> payload;
    std::string payload_fingerprint;
};

struct CacheStats final {
    bool success{};
    std::string code;
    std::string detail;
    std::size_t entries{};
    std::size_t bytes{};
};

struct CommitResult final {
    bool committed{};
    bool existing_won{};
    std::string code;
    std::string detail;
};

std::span<const std::byte> as_bytes(const std::string& value) {
    return std::span<const std::byte>(reinterpret_cast<const std::byte*>(value.data()), value.size());
}

std::span<const std::byte> as_bytes(const std::vector<std::byte>& value) {
    return std::span<const std::byte>(value.data(), value.size());
}

std::string path_text(const std::filesystem::path& path) {
    return path.generic_string();
}

bool is_ktx2_payload(const std::span<const std::byte> payload) {
    if (payload.size() < kKtx2Identifier.size()) return false;
    for (std::size_t index = 0U; index < kKtx2Identifier.size(); ++index) {
        if (std::to_integer<std::uint8_t>(payload[index]) != kKtx2Identifier[index]) return false;
    }
    return true;
}

bool valid_identity_component(const std::string_view value, const std::size_t maximum) {
    if (value.empty() || value.size() > maximum) return false;
    if (value.find("..") != std::string_view::npos) return false;
    for (const auto character : value) {
        const auto byte = static_cast<unsigned char>(character);
        if (byte < 0x20U || byte == 0x7fU || character == '/' || character == '\\') return false;
    }
    return true;
}

bool valid_limits(const SpriteAtlasPageCacheLimits& limits) {
    return limits.max_entries > 0U && limits.max_entry_bytes >= kFixedRecordHeaderBytes &&
        limits.max_total_bytes > 0U && limits.max_identity_component_bytes > 0U;
}

std::string identity_material(const SpriteAtlasPageCacheRequest& request) {
    std::string material;
    const auto append = [&material](const std::string_view label, const std::string_view value) {
        material += label;
        material += std::to_string(value.size());
        material.push_back(':');
        material.append(value);
        material.push_back('\n');
    };
    append("source-page=", request.source_page_fingerprint);
    append("page-layout=", request.page_layout_fingerprint);
    append("cook-recipe=", request.cook_recipe_fingerprint);
    append("profile=", request.profile_fingerprint);
    append("compression=", request.compression);
    append("worker=", request.worker_identity);
    return material;
}

std::string hex_without_prefix(std::string value) {
    constexpr std::string_view prefix = "sha256:";
    if (value.starts_with(prefix)) value.erase(0U, prefix.size());
    return value;
}

std::string temporary_token() {
    const auto sequence = temporary_counter.fetch_add(1U, std::memory_order_relaxed);
    const auto clock = static_cast<std::uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch().count());
    const auto thread_id = std::hash<std::thread::id>{}(std::this_thread::get_id());
    std::ostringstream stream;
    stream << std::hex << clock << '-' << thread_id << '-' << sequence;
    return stream.str();
}

std::filesystem::path cache_directory(const SpriteAtlasPageCacheRequest& request) {
    return request.cache_root / std::filesystem::path(kCacheNamespace);
}

std::filesystem::path artifact_path(const SpriteAtlasPageCacheRequest& request,
                                    const std::string_view key) {
    return cache_directory(request) / (std::string(key) + std::string(kRecordExtension));
}

SpriteAtlasPageCacheReceipt failure_receipt(
    std::string code, std::string detail, const std::string_view cache_key = {}) {
    SpriteAtlasPageCacheReceipt receipt;
    receipt.code = std::move(code);
    receipt.detail = std::move(detail);
    receipt.cache_key = std::string(cache_key);
    receipt.diagnostics.push_back(receipt.detail);
    return receipt;
}

void append_u32(std::vector<std::byte>& bytes, const std::uint32_t value) {
    for (std::uint32_t shift = 0U; shift < 32U; shift += 8U) {
        bytes.push_back(static_cast<std::byte>((value >> shift) & 0xffU));
    }
}

void append_u64(std::vector<std::byte>& bytes, const std::uint64_t value) {
    for (std::uint32_t shift = 0U; shift < 64U; shift += 8U) {
        bytes.push_back(static_cast<std::byte>((value >> shift) & 0xffULL));
    }
}

bool read_u32(const std::vector<std::byte>& bytes, std::size_t& offset, std::uint32_t& value) {
    if (offset > bytes.size() || bytes.size() - offset < sizeof(std::uint32_t)) return false;
    value = 0U;
    for (std::uint32_t shift = 0U; shift < 32U; shift += 8U) {
        value |= static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(bytes[offset++])) << shift;
    }
    return true;
}

bool read_u64(const std::vector<std::byte>& bytes, std::size_t& offset, std::uint64_t& value) {
    if (offset > bytes.size() || bytes.size() - offset < sizeof(std::uint64_t)) return false;
    value = 0U;
    for (std::uint32_t shift = 0U; shift < 64U; shift += 8U) {
        value |= static_cast<std::uint64_t>(std::to_integer<std::uint8_t>(bytes[offset++])) << shift;
    }
    return true;
}

bool read_file(const std::filesystem::path& path, const std::size_t maximum,
               std::vector<std::byte>& bytes, std::string& code, std::string& detail) {
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    if (error) {
        code = "sprite-atlas-cache.file-stat-failed";
        detail = "The cache entry size could not be determined.";
        return false;
    }
    if (size > maximum || size > static_cast<std::uintmax_t>(std::numeric_limits<std::size_t>::max())) {
        code = "sprite-atlas-cache.entry-too-large";
        detail = "The cache entry exceeds the configured single-file budget.";
        return false;
    }
    bytes.resize(static_cast<std::size_t>(size));
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        code = "sprite-atlas-cache.file-open-failed";
        detail = "The cache entry could not be opened.";
        bytes.clear();
        return false;
    }
    if (!bytes.empty()) {
        input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    }
    if (!input && !input.eof()) {
        code = "sprite-atlas-cache.file-read-failed";
        detail = "The cache entry could not be read.";
        bytes.clear();
        return false;
    }
    if (static_cast<std::size_t>(input.gcount()) != bytes.size() && !bytes.empty()) {
        code = "sprite-atlas-cache.file-changed";
        detail = "The cache entry changed while it was being read.";
        bytes.clear();
        return false;
    }
    return true;
}

CacheReadResult read_cache_entry(const std::filesystem::path& path,
                                 const std::string_view expected_key,
                                 const SpriteAtlasPageCacheLimits& limits) {
    CacheReadResult result;
    std::error_code status_error;
    if (!std::filesystem::exists(path, status_error)) {
        result.code = status_error ? "sprite-atlas-cache.path-stat-failed" : "sprite-atlas-cache.miss";
        result.detail = status_error ? "The cache entry path could not be inspected." : "The cache entry is absent.";
        return result;
    }
    if (status_error || !std::filesystem::is_regular_file(path, status_error)) {
        result.code = "sprite-atlas-cache.path-conflict";
        result.detail = "The cache entry path is occupied by a non-regular file.";
        return result;
    }

    std::vector<std::byte> record;
    std::string read_code;
    std::string read_detail;
    if (!read_file(path, limits.max_entry_bytes, record, read_code, read_detail)) {
        // A file that starts with our magic is owned enough to quarantine; an
        // arbitrary file at our path is left untouched and reported as a
        // conflict later rather than being deleted or moved.
        std::array<std::byte, kRecordMagic.size()> prefix{};
        std::ifstream input(path, std::ios::binary);
        if (input) input.read(reinterpret_cast<char*>(prefix.data()),
                              static_cast<std::streamsize>(prefix.size()));
        result.owned = std::equal(prefix.begin(), prefix.end(), kRecordMagic.begin(), kRecordMagic.end(),
            [](const std::byte byte, const std::uint8_t expected) {
                return std::to_integer<std::uint8_t>(byte) == expected;
            });
        result.corrupt = result.owned;
        result.code = result.owned ? "sprite-atlas-cache.corrupt" : "sprite-atlas-cache.path-conflict";
        result.detail = read_detail;
        return result;
    }

    std::size_t offset = 0U;
    const bool has_magic = record.size() >= kRecordMagic.size() &&
        std::equal(kRecordMagic.begin(), kRecordMagic.end(), record.begin(),
            [](const std::uint8_t expected, const std::byte actual) {
                return expected == std::to_integer<std::uint8_t>(actual);
            });
    if (!has_magic) {
        result.code = "sprite-atlas-cache.path-conflict";
        result.detail = "The cache path is occupied by a file outside the engine cache format.";
        return result;
    }
    result.owned = true;
    offset = kRecordMagic.size();
    std::uint32_t version{};
    std::uint32_t key_size{};
    std::uint64_t payload_size{};
    std::uint32_t hash_size{};
    if (!read_u32(record, offset, version) || !read_u32(record, offset, key_size) ||
        !read_u64(record, offset, payload_size) || !read_u32(record, offset, hash_size) ||
        version != kRecordVersion || key_size > kMaximumCacheKeyBytes ||
        hash_size > 128U || payload_size > limits.max_entry_bytes) {
        result.corrupt = true;
        result.code = "sprite-atlas-cache.corrupt";
        result.detail = "The cache record header is invalid or exceeds the configured budget.";
        return result;
    }
    if (key_size > record.size() - std::min(offset, record.size()) ||
        hash_size > record.size() - std::min(offset + static_cast<std::size_t>(key_size), record.size()) ||
        payload_size > record.size() - std::min(offset + static_cast<std::size_t>(key_size) + hash_size, record.size())) {
        result.corrupt = true;
        result.code = "sprite-atlas-cache.corrupt";
        result.detail = "The cache record lengths exceed the file size.";
        return result;
    }
    const auto key_begin = offset;
    const auto stored_key = std::string(reinterpret_cast<const char*>(record.data() + key_begin), key_size);
    offset += key_size;
    const auto hash_begin = offset;
    const auto stored_hash = std::string(reinterpret_cast<const char*>(record.data() + hash_begin), hash_size);
    offset += hash_size;
    if (stored_key != expected_key || payload_size != record.size() - offset) {
        result.corrupt = true;
        result.code = "sprite-atlas-cache.corrupt";
        result.detail = "The cache record key or payload size does not match the requested page.";
        return result;
    }
    result.payload.assign(record.begin() + static_cast<std::ptrdiff_t>(offset), record.end());
    if (!is_ktx2_payload(as_bytes(result.payload))) {
        result.corrupt = true;
        result.code = "sprite-atlas-cache.payload-format";
        result.detail = "The cache payload is not a KTX2 payload.";
        result.payload.clear();
        return result;
    }
    const auto payload_hash = sha256_bytes(as_bytes(result.payload));
    if (!payload_hash.success || payload_hash.value != stored_hash) {
        result.corrupt = true;
        result.code = "sprite-atlas-cache.payload-hash";
        result.detail = "The cached KTX2 payload SHA-256 does not match its record.";
        result.payload.clear();
        return result;
    }
    result.hit = true;
    result.code = "sprite-atlas-cache.hit";
    result.detail = "The content-addressed Sprite Atlas page cache entry was reused.";
    result.payload_fingerprint = payload_hash.value;
    return result;
}

bool quarantine_owned_entry(const std::filesystem::path& path, std::string& detail) {
    for (std::uint32_t attempt = 0U; attempt < 8U; ++attempt) {
        auto quarantine = path;
        quarantine += ".corrupt-" + temporary_token();
        std::error_code error;
        std::filesystem::rename(path, quarantine, error);
        if (!error) {
            detail = "The corrupt cache entry was preserved under " + path_text(quarantine) + ".";
            return true;
        }
        if (error == std::errc::no_such_file_or_directory) {
            detail = "The corrupt cache entry disappeared during quarantine.";
            return true;
        }
    }
    detail = "The corrupt cache entry could not be quarantined without deleting it.";
    return false;
}

CacheStats inspect_cache_directory(const std::filesystem::path& directory) {
    CacheStats stats{true, {}, {}, 0U, 0U};
    std::error_code iterator_error;
    if (!std::filesystem::exists(directory, iterator_error)) {
        if (iterator_error) {
            stats.success = false;
            stats.code = "sprite-atlas-cache.directory-stat-failed";
            stats.detail = "The cache directory could not be inspected.";
        }
        return stats;
    }
    for (std::filesystem::directory_iterator iterator(directory, iterator_error), end;
         iterator != end && !iterator_error; iterator.increment(iterator_error)) {
        const auto& entry = *iterator;
        if (!entry.is_regular_file(iterator_error) || iterator_error ||
            entry.path().extension() != kRecordExtension) {
            iterator_error.clear();
            continue;
        }
        const auto size = entry.file_size(iterator_error);
        if (iterator_error || size > static_cast<std::uintmax_t>(std::numeric_limits<std::size_t>::max() - stats.bytes)) {
            stats.success = false;
            stats.code = "sprite-atlas-cache.directory-stat-failed";
            stats.detail = "A cache entry size could not be measured safely.";
            return stats;
        }
        ++stats.entries;
        stats.bytes += static_cast<std::size_t>(size);
    }
    if (iterator_error) {
        stats.success = false;
        stats.code = "sprite-atlas-cache.directory-read-failed";
        stats.detail = "The cache directory could not be enumerated.";
    }
    return stats;
}

std::vector<std::byte> serialize_record(const std::string_view key,
                                        const std::string_view payload_hash,
                                        const std::span<const std::byte> payload) {
    std::vector<std::byte> record;
    record.reserve(kFixedRecordHeaderBytes + key.size() + payload_hash.size() + payload.size());
    for (const auto value : kRecordMagic) record.push_back(static_cast<std::byte>(value));
    append_u32(record, kRecordVersion);
    append_u32(record, static_cast<std::uint32_t>(key.size()));
    append_u64(record, static_cast<std::uint64_t>(payload.size()));
    append_u32(record, static_cast<std::uint32_t>(payload_hash.size()));
    record.insert(record.end(), reinterpret_cast<const std::byte*>(key.data()),
                  reinterpret_cast<const std::byte*>(key.data()) + key.size());
    record.insert(record.end(), reinterpret_cast<const std::byte*>(payload_hash.data()),
                  reinterpret_cast<const std::byte*>(payload_hash.data()) + payload_hash.size());
    record.insert(record.end(), payload.begin(), payload.end());
    return record;
}

CommitResult commit_record(const std::filesystem::path& path, const std::string_view key,
                           const std::string_view payload_hash, const std::span<const std::byte> payload,
                           const SpriteAtlasPageCacheLimits& limits) {
    const auto record = serialize_record(key, payload_hash, payload);
    if (record.size() > limits.max_entry_bytes) {
        return {false, false, "sprite-atlas-cache.entry-budget", "The serialized cache entry exceeds the single-file budget."};
    }
    auto temporary = path;
    temporary += ".tmp-" + temporary_token();
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output) {
            return {false, false, "sprite-atlas-cache.temp-open-failed", "The cache staging file could not be opened."};
        }
        output.write(reinterpret_cast<const char*>(record.data()), static_cast<std::streamsize>(record.size()));
        output.flush();
        if (!output) {
            output.close();
            std::error_code cleanup_error;
            std::filesystem::remove(temporary, cleanup_error);
            return {false, false, "sprite-atlas-cache.temp-write-failed", "The cache staging file could not be written."};
        }
    }
    std::error_code rename_error;
    std::filesystem::rename(temporary, path, rename_error);
    if (!rename_error) return {true, false, "ok", "The cache entry was committed atomically."};

    std::error_code existing_error;
    const auto existing = std::filesystem::is_regular_file(path, existing_error);
    std::error_code cleanup_error;
    std::filesystem::remove(temporary, cleanup_error);
    if (existing && !existing_error) {
        const auto other = read_cache_entry(path, key, limits);
        if (other.hit) return {false, true, "sprite-atlas-cache.race-hit", "Another process committed the same cache entry."};
    }
    return {false, false, "sprite-atlas-cache.commit-conflict",
        "The cache destination is occupied and could not be replaced without deleting an unknown file."};
}

SpriteAtlasPageCacheReceipt successful_receipt(
    const std::string_view key, const std::filesystem::path& path,
    const std::vector<std::byte>& payload, const std::string_view fingerprint,
    const bool hit, const bool rebuilt, const std::string_view detail) {
    SpriteAtlasPageCacheReceipt receipt;
    receipt.success = true;
    receipt.cache_hit = hit;
    receipt.cache_miss = !hit;
    receipt.rebuilt = rebuilt;
    receipt.code = hit ? "sprite-atlas-cache.hit" : (rebuilt ? "sprite-atlas-cache.rebuilt" : "sprite-atlas-cache.miss");
    receipt.detail = std::string(detail);
    receipt.cache_key = std::string(key);
    receipt.artifact_path = path;
    receipt.payload_fingerprint = std::string(fingerprint);
    receipt.payload_bytes = payload.size();
    receipt.payload = payload;
    receipt.diagnostics.push_back(receipt.detail);
    return receipt;
}

} // namespace

std::string sprite_atlas_page_cache_key(const SpriteAtlasPageCacheRequest& request) {
    if (request.cache_root.empty() || !valid_limits(request.limits)) return {};
    const auto maximum = request.limits.max_identity_component_bytes;
    if (!valid_identity_component(request.source_page_fingerprint, maximum) ||
        !valid_identity_component(request.page_layout_fingerprint, maximum) ||
        !valid_identity_component(request.cook_recipe_fingerprint, maximum) ||
        !valid_identity_component(request.profile_fingerprint, maximum) ||
        !valid_identity_component(request.compression, maximum) ||
        !valid_identity_component(request.worker_identity, maximum)) return {};
    const auto digest = sha256_bytes(as_bytes(identity_material(request)));
    if (!digest.success) return {};
    return "spage-" + hex_without_prefix(digest.value);
}

SpriteAtlasPageCacheReceipt execute_sprite_atlas_page_cache(
    const SpriteAtlasPageCacheRequest& request, const SpriteAtlasPageProducer& producer) {
    if (request.cache_root.empty()) {
        return failure_receipt("sprite-atlas-cache.root-empty", "A cache root directory is required.");
    }
    if (!valid_limits(request.limits)) {
        return failure_receipt("sprite-atlas-cache.limits-invalid", "Cache limits must be positive and internally bounded.");
    }
    const auto key = sprite_atlas_page_cache_key(request);
    if (key.empty()) {
        return failure_receipt("sprite-atlas-cache.identity-invalid",
            "Every page fingerprint, recipe, profile, compression and worker identity must be a safe bounded token.");
    }
    if (!producer) {
        auto receipt = failure_receipt("sprite-atlas-cache.producer-missing",
            "A Sprite Atlas page producer callback is required on a cache miss.", key);
        receipt.cache_miss = true;
        return receipt;
    }

    const auto directory = cache_directory(request);
    std::error_code directory_error;
    std::filesystem::create_directories(directory, directory_error);
    if (directory_error) {
        return failure_receipt("sprite-atlas-cache.directory-create-failed",
            "The Sprite Atlas page cache directory could not be created.", key);
    }
    const auto destination = artifact_path(request, key);
    auto cached = read_cache_entry(destination, key, request.limits);
    if (cached.hit) {
        auto receipt = successful_receipt(key, destination, cached.payload, cached.payload_fingerprint,
            true, false, cached.detail);
        const auto hit_stats = inspect_cache_directory(directory);
        if (hit_stats.success) {
            receipt.cache_entries = hit_stats.entries;
            receipt.cache_bytes = hit_stats.bytes;
        }
        return receipt;
    }
    bool rebuilt = false;
    if (cached.corrupt && cached.owned) {
        std::string quarantine_detail;
        if (!quarantine_owned_entry(destination, quarantine_detail)) {
            auto receipt = failure_receipt("sprite-atlas-cache.corrupt-locked", quarantine_detail, key);
            receipt.cache_miss = true;
            receipt.rebuilt = true;
            return receipt;
        }
        rebuilt = true;
    } else if (cached.code == "sprite-atlas-cache.path-conflict") {
        auto receipt = failure_receipt(cached.code, cached.detail, key);
        receipt.cache_miss = true;
        return receipt;
    }

    const auto preflight_stats = inspect_cache_directory(directory);
    if (!preflight_stats.success) {
        auto receipt = failure_receipt(preflight_stats.code, preflight_stats.detail, key);
        receipt.cache_miss = true;
        receipt.rebuilt = rebuilt;
        return receipt;
    }
    if (preflight_stats.entries >= request.limits.max_entries) {
        auto receipt = failure_receipt("sprite-atlas-cache.directory-budget",
            "The cache directory entry budget is full; no producer work was started.", key);
        receipt.cache_miss = true;
        receipt.rebuilt = rebuilt;
        return receipt;
    }
    if (preflight_stats.bytes >= request.limits.max_total_bytes) {
        auto receipt = failure_receipt("sprite-atlas-cache.total-budget",
            "The cache directory total-byte budget is full; no producer work was started.", key);
        receipt.cache_miss = true;
        receipt.rebuilt = rebuilt;
        return receipt;
    }

    SpriteAtlasPageProduced produced;
    try {
        produced = producer();
    } catch (const std::exception& exception) {
        auto receipt = failure_receipt("sprite-atlas-cache.producer-exception", exception.what(), key);
        receipt.cache_miss = true;
        receipt.rebuilt = rebuilt;
        return receipt;
    } catch (...) {
        auto receipt = failure_receipt("sprite-atlas-cache.producer-exception",
            "The Sprite Atlas page producer threw an unknown exception.", key);
        receipt.cache_miss = true;
        receipt.rebuilt = rebuilt;
        return receipt;
    }
    if (!produced.success) {
        auto receipt = failure_receipt(
            produced.code.empty() ? "sprite-atlas-cache.producer-failed" : produced.code,
            produced.detail.empty() ? "The Sprite Atlas page producer failed." : produced.detail, key);
        receipt.cache_miss = true;
        receipt.rebuilt = rebuilt;
        return receipt;
    }
    if (produced.payload.empty() || !is_ktx2_payload(as_bytes(produced.payload))) {
        auto receipt = failure_receipt("sprite-atlas-cache.payload-format",
            "The producer did not return a non-empty KTX2 payload.", key);
        receipt.cache_miss = true;
        receipt.rebuilt = rebuilt;
        return receipt;
    }
    if (produced.payload.size() > request.limits.max_entry_bytes) {
        auto receipt = failure_receipt("sprite-atlas-cache.entry-budget",
            "The producer payload exceeds the single-file budget.", key);
        receipt.cache_miss = true;
        receipt.rebuilt = rebuilt;
        return receipt;
    }
    const auto payload_hash = sha256_bytes(as_bytes(produced.payload));
    if (!payload_hash.success) {
        auto receipt = failure_receipt("sprite-atlas-cache.payload-hash-failed",
            "The producer payload could not be hashed for cache identity.", key);
        receipt.cache_miss = true;
        receipt.rebuilt = rebuilt;
        return receipt;
    }

    const auto stats = inspect_cache_directory(directory);
    if (!stats.success) {
        auto receipt = failure_receipt(stats.code, stats.detail, key);
        receipt.cache_miss = true;
        receipt.rebuilt = rebuilt;
        return receipt;
    }
    const auto record_size = kFixedRecordHeaderBytes + key.size() + payload_hash.value.size() + produced.payload.size();
    if (record_size > request.limits.max_entry_bytes) {
        auto receipt = failure_receipt("sprite-atlas-cache.entry-budget",
            "The serialized cache entry exceeds the single-file budget.", key);
        receipt.cache_miss = true;
        receipt.rebuilt = rebuilt;
        return receipt;
    }
    if (stats.entries >= request.limits.max_entries) {
        auto receipt = failure_receipt("sprite-atlas-cache.directory-budget",
            "The cache directory entry budget would be exceeded.", key);
        receipt.cache_miss = true;
        receipt.rebuilt = rebuilt;
        return receipt;
    }
    if (record_size > request.limits.max_total_bytes ||
        stats.bytes > request.limits.max_total_bytes - record_size) {
        auto receipt = failure_receipt("sprite-atlas-cache.total-budget",
            "The cache directory total-byte budget would be exceeded.", key);
        receipt.cache_miss = true;
        receipt.rebuilt = rebuilt;
        return receipt;
    }
    const auto committed = commit_record(destination, key, payload_hash.value,
        as_bytes(produced.payload), request.limits);
    if (committed.existing_won) {
        const auto raced = read_cache_entry(destination, key, request.limits);
        if (raced.hit) {
            return successful_receipt(key, destination, raced.payload, raced.payload_fingerprint,
                true, false, raced.detail);
        }
    }
    if (!committed.committed) {
        auto receipt = failure_receipt(committed.code, committed.detail, key);
        receipt.cache_miss = true;
        receipt.rebuilt = rebuilt;
        return receipt;
    }
    auto receipt = successful_receipt(key, destination, produced.payload, payload_hash.value,
        false, rebuilt, rebuilt ? "A corrupt Sprite Atlas page entry was quarantined and rebuilt."
                                : "A new Sprite Atlas page cache entry was committed.");
    const auto final_stats = inspect_cache_directory(directory);
    if (final_stats.success) {
        receipt.cache_entries = final_stats.entries;
        receipt.cache_bytes = final_stats.bytes;
    }
    return receipt;
}

std::string sprite_atlas_page_cache_receipt_json(
    const SpriteAtlasPageCacheReceipt& receipt, const std::size_t max_bytes) {
    Json result{
        {"schema", "noemancer.sprite-atlas-page-cache-receipt/0.1"},
        {"success", receipt.success},
        {"cacheHit", receipt.cache_hit},
        {"cacheMiss", receipt.cache_miss},
        {"rebuilt", receipt.rebuilt},
        {"code", receipt.code},
        {"detail", receipt.detail},
        {"cacheKey", receipt.cache_key},
        {"artifactPath", receipt.artifact_path.empty() ? std::string{} : path_text(receipt.artifact_path)},
        {"payloadFingerprint", receipt.payload_fingerprint},
        {"payloadBytes", receipt.payload_bytes},
        {"cacheEntries", receipt.cache_entries},
        {"cacheBytes", receipt.cache_bytes},
        {"diagnostics", receipt.diagnostics}
    };
    auto serialized = result.dump();
    if (serialized.size() <= max_bytes) return serialized;
    result["detail"] = "Receipt detail was truncated to respect the observation size budget.";
    result["diagnostics"] = Json::array();
    result["truncated"] = true;
    serialized = result.dump();
    if (serialized.size() <= max_bytes) return serialized;
    return Json{
        {"schema", "noemancer.sprite-atlas-page-cache-receipt/0.1"},
        {"success", receipt.success},
        {"cacheHit", receipt.cache_hit},
        {"cacheMiss", receipt.cache_miss},
        {"rebuilt", receipt.rebuilt},
        {"code", receipt.code},
        {"cacheKey", receipt.cache_key},
        {"truncated", true}
    }.dump();
}

} // namespace noemancer
