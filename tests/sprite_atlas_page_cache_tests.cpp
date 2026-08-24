#include "engine/sprite_atlas_page_cache.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace {

using noemancer::SpriteAtlasPageCacheReceipt;
using noemancer::SpriteAtlasPageCacheRequest;
using noemancer::SpriteAtlasPageProduced;

bool require(const bool condition, const char* message) {
    if (!condition) std::cerr << message << '\n';
    return condition;
}

std::filesystem::path unique_root() {
    const auto ticks = static_cast<std::uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch().count());
    const auto thread = std::hash<std::thread::id>{}(std::this_thread::get_id());
    return std::filesystem::temp_directory_path() /
        ("noemancer-sprite-page-cache-tests-" + std::to_string(ticks) + "-" + std::to_string(thread));
}

std::vector<std::byte> fixture_payload() {
    constexpr std::array<std::uint8_t, 12> identifier{
        0xABU, 0x4BU, 0x54U, 0x58U, 0x20U, 0x32U,
        0x30U, 0xBBU, 0x0DU, 0x0AU, 0x1AU, 0x0AU};
    std::vector<std::byte> payload;
    payload.reserve(identifier.size() + 32U);
    for (const auto value : identifier) payload.push_back(static_cast<std::byte>(value));
    for (std::uint8_t value = 0U; value < 32U; ++value) payload.push_back(static_cast<std::byte>(value));
    return payload;
}

SpriteAtlasPageCacheRequest base_request(const std::filesystem::path& root) {
    SpriteAtlasPageCacheRequest request;
    request.cache_root = root;
    request.source_page_fingerprint = "sha256:source-page-a";
    request.page_layout_fingerprint = "sha256:layout-a";
    request.cook_recipe_fingerprint = "sha256:recipe-v1";
    request.profile_fingerprint = "sha256:profile-windows-debug";
    request.compression = "basis-lz";
    request.worker_identity = "workers=1";
    request.limits.max_entries = 8U;
    request.limits.max_entry_bytes = 4096U;
    request.limits.max_total_bytes = 16U * 1024U;
    return request;
}

bool is_successful_cache_miss(const SpriteAtlasPageCacheReceipt& receipt) {
    return receipt.success && receipt.cache_miss && !receipt.cache_hit;
}

} // namespace

int main() {
    const auto root = unique_root();
    std::error_code cleanup_error;
    std::filesystem::create_directories(root, cleanup_error);
    if (cleanup_error) {
        std::cerr << "Could not create the isolated cache test root.\n";
        return 1;
    }
    const auto payload = fixture_payload();
    bool valid = true;

    auto request = base_request(root);
    int producer_calls = 0;
    const auto produce = [&]() {
        ++producer_calls;
        return SpriteAtlasPageProduced{true, "ok", "fixture encoded", payload};
    };
    const auto first = noemancer::execute_sprite_atlas_page_cache(request, produce);
    valid = require(is_successful_cache_miss(first) && !first.rebuilt && producer_calls == 1,
        "Initial Sprite Atlas page Cook did not commit a cache miss.") && valid;
    valid = require(first.payload_fingerprint.starts_with("sha256:") && first.payload_bytes == payload.size(),
        "Initial Sprite Atlas page cache receipt did not expose a SHA-256 payload identity.") && valid;
    valid = require(std::filesystem::is_regular_file(first.artifact_path),
        "Initial Sprite Atlas page cache artifact was not written.") && valid;

    const auto no_encoder_on_hit = [&]() -> SpriteAtlasPageProduced {
        ++producer_calls;
        return {false, "test.encoder-called", "The producer must not run on a cache hit.", {}};
    };
    const auto hit = noemancer::execute_sprite_atlas_page_cache(request, no_encoder_on_hit);
    valid = require(hit.success && hit.cache_hit && !hit.cache_miss && !hit.rebuilt && producer_calls == 1,
        "A repeated page request did not hit the on-disk cache without calling the producer.") && valid;
    valid = require(hit.payload == payload && hit.payload_fingerprint == first.payload_fingerprint,
        "A cache hit did not preserve the KTX2 payload identity.") && valid;
    const auto receipt_json = noemancer::sprite_atlas_page_cache_receipt_json(hit);
    valid = require(receipt_json.find("noemancer.sprite-atlas-page-cache-receipt/0.1") != std::string::npos &&
        receipt_json.find("cacheHit") != std::string::npos,
        "Sprite Atlas page cache receipt JSON did not expose the stable observation schema.") && valid;

    auto different_layout = request;
    different_layout.page_layout_fingerprint = "sha256:layout-b";
    const auto different = noemancer::execute_sprite_atlas_page_cache(different_layout, produce);
    valid = require(is_successful_cache_miss(different) && producer_calls == 2 &&
        different.cache_key != first.cache_key,
        "Changing the page layout fingerprint incorrectly aliased an existing cache entry.") && valid;

    // Corrupt only a cache file owned by this test.  The cache must preserve
    // the damaged file under a quarantine name and rebuild the canonical path.
    {
        std::ifstream input(first.artifact_path, std::ios::binary);
        std::vector<char> bytes{std::istreambuf_iterator<char>(input), {}};
        if (!bytes.empty()) {
            bytes.back() = static_cast<char>(bytes.back() ^ 0x5a);
            std::ofstream output(first.artifact_path, std::ios::binary | std::ios::trunc);
            output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        } else {
            valid = require(false, "Could not read the cache artifact before corruption test.") && valid;
        }
    }
    const auto rebuilt = noemancer::execute_sprite_atlas_page_cache(request, produce);
    valid = require(rebuilt.success && rebuilt.rebuilt && rebuilt.cache_miss && !rebuilt.cache_hit &&
        producer_calls == 3 && rebuilt.payload == payload,
        "A corrupted owned page entry was not rebuilt with an explicit rebuilt receipt.") && valid;
    std::size_t quarantine_count = 0U;
    for (const auto& entry : std::filesystem::directory_iterator(root / "sprite-atlas-pages-v1")) {
        if (entry.path().filename().string().find(".corrupt-") != std::string::npos) ++quarantine_count;
    }
    valid = require(quarantine_count == 1U,
        "The corrupted page entry was deleted instead of being preserved in quarantine.") && valid;

    auto budget_root = unique_root();
    auto budget_request = base_request(budget_root);
    budget_request.limits.max_entries = 1U;
    const auto budget_first = noemancer::execute_sprite_atlas_page_cache(budget_request, produce);
    auto budget_second_request = budget_request;
    budget_second_request.source_page_fingerprint = "sha256:source-page-b";
    const auto budget_second = noemancer::execute_sprite_atlas_page_cache(budget_second_request, produce);
    valid = require(budget_first.success && !budget_second.success && budget_second.cache_miss &&
        budget_second.code == "sprite-atlas-cache.directory-budget",
        "The page cache did not enforce its hard entry budget.") && valid;
    std::filesystem::remove_all(budget_root, cleanup_error);

    auto traversal = request;
    traversal.source_page_fingerprint = "../../outside";
    int traversal_calls = 0;
    const auto traversal_result = noemancer::execute_sprite_atlas_page_cache(traversal, [&]() {
        ++traversal_calls;
        return SpriteAtlasPageProduced{true, "ok", {}, payload};
    });
    valid = require(!traversal_result.success && traversal_result.code == "sprite-atlas-cache.identity-invalid" &&
        traversal_calls == 0,
        "Path traversal in a page identity was not rejected before the producer ran.") && valid;

    auto producer_failure_request = request;
    producer_failure_request.source_page_fingerprint = "sha256:producer-failure";
    const auto producer_failure = noemancer::execute_sprite_atlas_page_cache(
        producer_failure_request, []() {
            return SpriteAtlasPageProduced{false, "test.encoder-failed", "Synthetic encoder failure.", {}};
        });
    valid = require(!producer_failure.success && producer_failure.cache_miss && !producer_failure.cache_hit &&
        producer_failure.code == "test.encoder-failed",
        "A producer failure did not produce an explicit cache miss receipt.") && valid;

    std::filesystem::remove_all(root, cleanup_error);
    if (!valid) return 2;
    std::cout << "sprite_atlas_page_cache_tests: ok\n";
    return 0;
}
