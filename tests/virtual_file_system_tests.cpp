#include "engine/content_hash.hpp"
#include "engine/virtual_file_system.hpp"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

namespace {

bool check(const bool condition, const std::string_view message) {
    if (condition) return true;
    std::cerr << message << '\n';
    return false;
}

struct Fixture final {
    Fixture() {
        std::error_code error;
        const auto temporary = std::filesystem::temp_directory_path(error);
        if (error) return;
        const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
        root = temporary / ("noemancer-vfs-tests-" + std::to_string(nonce));
        std::filesystem::create_directories(root, error);
        valid = !error;
    }

    ~Fixture() {
        if (!valid || root.empty() || root.filename().string().find("noemancer-vfs-tests-") != 0U) return;
        std::error_code error;
        std::filesystem::remove_all(root, error);
    }

    std::filesystem::path directory(const std::string_view name) const {
        std::error_code error;
        const auto path = root / name;
        std::filesystem::create_directories(path, error);
        return error ? std::filesystem::path{} : path;
    }

    bool write(const std::filesystem::path& directory, const std::string_view relative,
               const std::string_view bytes) const {
        std::error_code error;
        const auto path = directory / relative;
        std::filesystem::create_directories(path.parent_path(), error);
        if (error) return false;
        std::ofstream output(path, std::ios::binary);
        output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        return static_cast<bool>(output);
    }

    std::filesystem::path root;
    bool valid{};
};

std::string text(const noemancer::VfsReadResult& result) {
    return {reinterpret_cast<const char*>(result.bytes.data()), result.bytes.size()};
}

noemancer::VfsMountSpec mount_spec(
    std::string id, std::string root, std::filesystem::path source,
    const std::int32_t priority = 0,
    const noemancer::VfsMountKind kind = noemancer::VfsMountKind::directory) {
    return noemancer::VfsMountSpec{
        std::move(id), std::move(root), std::move(source), kind, priority, true};
}

} // namespace

int main() {
    bool valid = true;
    Fixture fixture;
    if (!fixture.valid) {
        std::cerr << "Could not create the VFS test fixture.\n";
        return 2;
    }

    const auto low = fixture.directory("low");
    const auto high = fixture.directory("high");
    const auto equal = fixture.directory("equal");
    const auto nested = fixture.directory("nested");
    const auto outside = fixture.directory("outside");
    const std::string large(192U * 1024U, 'z');
    valid = check(!low.empty() && !high.empty() && !equal.empty() && !nested.empty() && !outside.empty() &&
                      fixture.write(low, "tiles/hero.bin", "low-layer") &&
                      fixture.write(high, "tiles/hero.bin", "high-layer") &&
                      fixture.write(equal, "tiles/hero.bin", "equal-layer") &&
                      fixture.write(nested, "hero.bin", "nested-layer") &&
                      fixture.write(low, "range.bin", "0123456789") &&
                      fixture.write(low, "large.bin", large) && fixture.write(outside, "secret.bin", "secret"),
                  "Could not construct deterministic VFS fixtures.") && valid;

    noemancer::VirtualFileSystem vfs({
        .max_mounts = 32U,
        .max_uri_bytes = 128U,
        .max_read_bytes = 256U * 1024U,
        .max_observation_bytes = 1024U});
    const auto low_mount = vfs.mount(mount_spec("base", "asset://roots/0/", low, 0));
    valid = check(low_mount.success && low_mount.changed && low_mount.revision_before == 0U &&
                      low_mount.revision_after == 1U && vfs.revision() == 1U,
                  "Base mount did not publish a revisioned receipt.") && valid;

    auto result = vfs.read({.uri = "asset://roots/0/tiles/hero.bin", .byte_budget = 64U});
    valid = check(result.success && text(result) == "low-layer" && result.file.mount_id == "base" &&
                      result.file.relative_path == "tiles/hero.bin" && result.eof &&
                      result.sha256 == noemancer::sha256_bytes(result.bytes).value,
                  "Base directory read or SHA-256 identity failed.") && valid;

    const auto package_mount = vfs.mount(mount_spec(
        "package", "asset://roots/0", high, 20, noemancer::VfsMountKind::package_directory));
    result = vfs.read({.uri = "asset://roots/0/tiles/hero.bin", .byte_budget = 64U});
    valid = check(package_mount.success && result.success && text(result) == "high-layer" &&
                      result.file.mount_id == "package",
                  "Package-directory overlay did not obey priority.") && valid;

    const auto equal_mount = vfs.mount(mount_spec("equal", "asset://roots/0", equal, 20));
    result = vfs.read({.uri = "asset://roots/0/tiles/hero.bin", .byte_budget = 64U});
    valid = check(equal_mount.success && result.success && text(result) == "high-layer",
                  "Equal-priority overlay did not preserve stable mount order.") && valid;

    const auto nested_mount = vfs.mount(mount_spec("nested", "asset://roots/0/tiles", nested, -50));
    result = vfs.read({.uri = "asset://roots/0/tiles/hero.bin", .byte_budget = 64U});
    valid = check(nested_mount.success && result.success && text(result) == "nested-layer" &&
                      result.file.mount_id == "nested" && result.file.relative_path == "hero.bin",
                  "A more specific virtual root did not take precedence over a broad overlay.") && valid;

    const auto nested_unmount = vfs.unmount("nested");
    result = vfs.read({.uri = "asset://roots/0/tiles/hero.bin", .byte_budget = 64U});
    valid = check(nested_unmount.success && result.success && text(result) == "high-layer" &&
                      !vfs.unmount("missing").success,
                  "Unmount did not restore the next overlay or report a missing stable ID.") && valid;

    const auto range = vfs.read({
        .uri = "asset://roots/0/range.bin", .offset = 3U, .length = 4U, .byte_budget = 4U});
    const auto eof = vfs.read({
        .uri = "asset://roots/0/range.bin", .offset = 10U, .length = 0U, .byte_budget = 0U});
    const auto beyond = vfs.read({
        .uri = "asset://roots/0/range.bin", .offset = 11U, .length = 0U, .byte_budget = 8U});
    const auto past_end = vfs.read({
        .uri = "asset://roots/0/range.bin", .offset = 8U, .length = 3U, .byte_budget = 8U});
    valid = check(range.success && text(range) == "3456" && !range.eof &&
                      eof.success && eof.bytes.empty() && eof.eof &&
                      !beyond.success && beyond.code == "vfs.range-invalid" &&
                      !past_end.success && past_end.code == "vfs.range-invalid",
                  "Range and EOF contracts are not deterministic.") && valid;

    const auto budget = vfs.read({
        .uri = "asset://roots/0/range.bin", .offset = 0U, .length = 0U, .byte_budget = 9U});
    valid = check(!budget.success && budget.code == "vfs.read-budget-exceeded" && budget.bytes.empty(),
                  "The effective read budget was not enforced before allocation.") && valid;

    std::atomic_uint32_t cancellation_checks{};
    const auto cancelled = vfs.read({
        .uri = "asset://roots/0/large.bin",
        .offset = 0U,
        .length = 0U,
        .byte_budget = 256U * 1024U,
        .cancelled = [&] { return cancellation_checks.fetch_add(1U) >= 3U; }});
    valid = check(!cancelled.success && cancelled.code == "vfs.cancelled" && cancelled.bytes.empty() &&
                      cancelled.sha256.empty(),
                  "Cancellation published partial bytes or a partial hash.") && valid;

    const auto traversal = vfs.stat("asset://roots/0/../outside/secret.bin");
    const auto noncanonical = vfs.stat("Asset://roots/0/range.bin");
    const auto trailing_slash = vfs.stat("asset://roots/0/range.bin/");
    valid = check(!traversal.success && traversal.code == "vfs.uri-invalid" &&
                      !noncanonical.success && noncanonical.code == "vfs.uri-invalid" &&
                      !trailing_slash.success && trailing_slash.code == "vfs.uri-invalid",
                  "Non-canonical or traversing URIs were accepted.") && valid;

    std::error_code symlink_error;
    std::filesystem::create_directory_symlink(outside, low / "escape", symlink_error);
    if (!symlink_error) {
        const auto escaped = vfs.stat("asset://roots/0/escape/secret.bin");
        valid = check(!escaped.success && escaped.code == "vfs.path-escape",
                      "A symlink escape from the mounted source root was accepted.") && valid;
    }

    const auto observation_text = vfs.observation_json();
    const auto observation = nlohmann::json::parse(observation_text, nullptr, false);
    valid = check(!observation.is_discarded() && observation_text.size() <= vfs.limits().max_observation_bytes &&
                      observation.value("schema", "") == "noemancer.vfs-observation/0.1" &&
                      observation.value("revision", 0U) == vfs.revision() && observation.contains("mounts"),
                  "The mount observation is invalid, unbounded or not revisioned.") && valid;

    noemancer::VirtualFileSystem bounded({
        .max_mounts = 1U, .max_uri_bytes = 40U, .max_read_bytes = 4U, .max_observation_bytes = 20U});
    valid = check(bounded.mount(mount_spec("one", "tiny://", low)).success &&
                      !bounded.mount(mount_spec("two", "other://", high)).success &&
                      !bounded.stat("tiny://this-path-is-intentionally-more-than-forty-bytes-long.bin").success &&
                      bounded.read({.uri = "tiny://range.bin", .byte_budget = 64U}).code ==
                          "vfs.read-budget-exceeded" &&
                      bounded.observation_json().size() <= 20U,
                  "Mount, URI, read or observation limits were not enforced.") && valid;

    noemancer::VirtualFileSystem concurrent({
        .max_mounts = 8U, .max_uri_bytes = 128U, .max_read_bytes = 64U, .max_observation_bytes = 1024U});
    valid = check(concurrent.mount(mount_spec("concurrent-base", "data://", low)).success,
                  "Could not mount the concurrent read fixture.") && valid;
    std::atomic_bool start{};
    std::atomic_bool concurrency_ok{true};
    std::vector<std::thread> readers;
    for (std::size_t index = 0U; index < 4U; ++index) {
        readers.emplace_back([&] {
            while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
            for (std::size_t attempt = 0U; attempt < 250U; ++attempt) {
                const auto read = concurrent.read({.uri = "data://range.bin", .byte_budget = 16U});
                if (!read.success || text(read) != "0123456789") concurrency_ok.store(false);
                static_cast<void>(concurrent.observation_json());
            }
        });
    }
    start.store(true, std::memory_order_release);
    for (std::size_t attempt = 0U; attempt < 100U; ++attempt) {
        const auto id = "transient-" + std::to_string(attempt);
        if (!concurrent.mount(mount_spec(id, "temp://", high)).success || !concurrent.unmount(id).success)
            concurrency_ok.store(false);
    }
    for (auto& reader : readers) reader.join();
    valid = check(concurrency_ok.load(), "Concurrent reads and mount mutations were not safe.") && valid;

    return valid ? 0 : 1;
}
