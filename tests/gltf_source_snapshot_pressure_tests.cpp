#include "engine/gltf_mesh.hpp"

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace {

using Json = nlohmann::json;

constexpr std::size_t kDependencyCount = 128U;
constexpr std::size_t kDependencyBytes = 17U;

struct Fixture final {
    std::filesystem::path root;
    std::filesystem::path document;
    std::vector<std::filesystem::path> dependencies;
    std::uint64_t document_bytes{};
};

struct ScopedFixture final {
    Fixture value;

    ~ScopedFixture() {
        std::error_code ignored;
        std::filesystem::remove_all(value.root, ignored);
    }
};

std::string dependency_uri(const std::size_t index) {
    std::ostringstream name;
    name << "buffers/blob-" << std::setw(3) << std::setfill('0') << index << ".bin";
    return name.str();
}

std::vector<std::byte> dependency_payload(const std::size_t index) {
    std::vector<std::byte> payload(kDependencyBytes);
    for (std::size_t byte = 0; byte < payload.size(); ++byte) {
        payload[byte] = std::byte{
            static_cast<unsigned char>((index * 37U + byte * 11U + 3U) & 0xffU)};
    }
    return payload;
}

bool write_bytes(const std::filesystem::path& path, const std::vector<std::byte>& bytes) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) return false;
    if (!bytes.empty()) {
        output.write(reinterpret_cast<const char*>(bytes.data()),
                     static_cast<std::streamsize>(bytes.size()));
    }
    return output.good();
}

bool make_fixture(ScopedFixture& scoped, std::string& error) {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    scoped.value.root = std::filesystem::temp_directory_path() /
        ("noemancer-gltf-source-snapshot-pressure-" + std::to_string(stamp));
    scoped.value.document = scoped.value.root / "scene.gltf";

    std::error_code filesystem_error;
    std::filesystem::create_directories(scoped.value.root / "buffers", filesystem_error);
    if (filesystem_error) {
        error = "could not create the temporary fixture directory: " + filesystem_error.message();
        return false;
    }

    Json document = Json::object();
    document["asset"] = Json{{"version", "2.0"}, {"generator", "noemancer.gltf-source-snapshot-pressure"}};
    document["buffers"] = Json::array();
    scoped.value.dependencies.reserve(kDependencyCount);
    for (std::size_t index = 0; index < kDependencyCount; ++index) {
        const auto relative_uri = dependency_uri(index);
        const auto path = scoped.value.root / std::filesystem::path(relative_uri);
        const auto payload = dependency_payload(index);
        if (!write_bytes(path, payload)) {
            error = "could not write dependency: " + path.string();
            return false;
        }
        scoped.value.dependencies.push_back(path);
        document["buffers"].push_back(Json{
            {"uri", relative_uri}, {"byteLength", payload.size()}});
    }

    const auto serialized = document.dump();
    std::vector<std::byte> document_bytes(serialized.size());
    if (!document_bytes.empty()) {
        std::memcpy(document_bytes.data(), serialized.data(), serialized.size());
    }
    if (!write_bytes(scoped.value.document, document_bytes)) {
        error = "could not write the glTF source document: " + scoped.value.document.string();
        return false;
    }
    scoped.value.document_bytes = static_cast<std::uint64_t>(serialized.size());
    return true;
}

int fail(const int code, const std::string& detail) {
    std::cerr << "gltf source snapshot pressure test failure (" << code << "): "
              << detail << '\n';
    return code;
}

} // namespace

int main() {
    ScopedFixture fixture;
    std::string fixture_error;
    if (!make_fixture(fixture, fixture_error)) return fail(1, fixture_error);

    const auto first = noemancer::read_gltf_source_snapshot(fixture.value.document);
    if (!first.valid || first.dependencies.size() != kDependencyCount) {
        return fail(2, "128 external buffers were not captured: " + first.code + " - " + first.detail);
    }
    const auto expected_total = fixture.value.document_bytes +
        static_cast<std::uint64_t>(kDependencyCount * kDependencyBytes);
    if (first.source_bytes != fixture.value.document_bytes || first.total_bytes != expected_total) {
        return fail(3, "snapshot byte accounting did not include the document and all dependencies");
    }
    if (first.dependencies.front().normalized_relative_path != "buffers/blob-000.bin" ||
        first.dependencies.back().normalized_relative_path != "buffers/blob-127.bin") {
        return fail(4, "dependency discovery order or normalized paths are not stable");
    }

    const auto second = noemancer::read_gltf_source_snapshot(fixture.value.document);
    if (!second.valid || second.dependencies.size() != kDependencyCount ||
        second.total_bytes != expected_total ||
        noemancer::gltf_source_snapshot_fingerprint(first).empty() ||
        noemancer::gltf_source_snapshot_fingerprint(first) !=
            noemancer::gltf_source_snapshot_fingerprint(second)) {
        return fail(5, "re-reading an unchanged source did not produce the same closure fingerprint");
    }
    const auto unchanged = noemancer::verify_gltf_source_snapshot(first);
    if (!unchanged.unchanged || unchanged.code != "ok") {
        return fail(6, "a freshly captured 128-dependency snapshot did not verify unchanged");
    }

    noemancer::GltfSourceSnapshotLimits dependency_limit;
    dependency_limit.maximum_dependencies = kDependencyCount - 1U;
    const auto count_rejected = noemancer::read_gltf_source_snapshot(
        fixture.value.document, dependency_limit);
    if (count_rejected.valid || count_rejected.code != "gltf.dependency-count-exceeded") {
        return fail(7, "maximum_dependencies did not reject the 128th dependency with a stable code: " +
            count_rejected.code + " - " + count_rejected.detail);
    }

    noemancer::GltfSourceSnapshotLimits total_limit;
    total_limit.maximum_total_bytes = expected_total - 1U;
    const auto total_rejected = noemancer::read_gltf_source_snapshot(
        fixture.value.document, total_limit);
    if (total_rejected.valid || total_rejected.code != "gltf.total-budget-exceeded") {
        return fail(8, "maximum_total_bytes did not reject the dependency closure with a stable code: " +
            total_rejected.code + " - " + total_rejected.detail);
    }

    const std::size_t changed_index = 73U;
    std::vector<std::byte> changed_payload(kDependencyBytes, std::byte{0xa5});
    if (!write_bytes(fixture.value.dependencies[changed_index], changed_payload)) {
        return fail(9, "could not mutate the selected dependency for verification");
    }
    const auto changed = noemancer::verify_gltf_source_snapshot(first);
    if (changed.unchanged || changed.code != "gltf.dependency-changed" ||
        changed.normalized_relative_path != "buffers/blob-073.bin") {
        return fail(10, "dependency mutation was not reported with the exact normalized path: " +
            changed.code + " / " + changed.normalized_relative_path + " - " + changed.detail);
    }

    std::cout << "gltf source snapshot pressure: captured " << kDependencyCount
              << " dependencies, " << first.total_bytes << " total bytes, stable closure fingerprint, "
              << "count/total budgets and exact mutation path verified\n";
    return 0;
}
