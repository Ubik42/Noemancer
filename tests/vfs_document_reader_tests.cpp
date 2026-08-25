#include "engine/content_hash.hpp"
#include "engine/vfs_document_reader.hpp"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>

namespace {

bool check(const bool condition, const std::string_view message) {
    if (condition) return true;
    std::cerr << message << '\n';
    return false;
}

struct Fixture final {
    Fixture() {
        std::error_code error;
        root = std::filesystem::temp_directory_path(error) /
            ("noemancer-vfs-document-reader-tests-" +
             std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        if (!error) std::filesystem::create_directories(root / "source", error);
        if (!error) std::filesystem::create_directories(root / "package", error);
        valid = !error;
    }

    ~Fixture() {
        if (!valid || root.empty() ||
            !root.filename().string().starts_with("noemancer-vfs-document-reader-tests-")) return;
        std::error_code error;
        std::filesystem::remove_all(root, error);
    }

    bool write(const std::string_view directory, const std::string_view relative,
               const std::string_view contents) const {
        std::error_code error;
        const auto path = root / directory / relative;
        std::filesystem::create_directories(path.parent_path(), error);
        if (error) return false;
        std::ofstream output(path, std::ios::binary);
        output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
        return static_cast<bool>(output);
    }

    std::filesystem::path root;
    bool valid{};
};

noemancer::VirtualFileSystem mounted_vfs(
    const std::filesystem::path& root, const noemancer::VfsMountKind kind,
    const std::string& id) {
    noemancer::VirtualFileSystem vfs({
        .max_mounts = 4U,
        .max_uri_bytes = 128U,
        .max_read_bytes = 256U * 1024U,
        .max_observation_bytes = 1024U});
    const auto receipt = vfs.mount({id, "document://", root, kind, 0, true});
    if (!receipt.success) std::cerr << "Could not mount document fixture: " << receipt.code << '\n';
    return vfs;
}

} // namespace

int main() {
    bool valid = true;
    Fixture fixture;
    if (!fixture.valid) return 2;

    constexpr std::string_view utf8_text = "Noemancer: \xe4\xbd\xa0\xe5\xa5\xbd, agent.\n";
    constexpr std::string_view json_source =
        R"({ "z" : 2, "schemaVersion" : "noemancer.test/0.1", "nested" : { "b" : true, "a" : 1 } })";
    constexpr std::string_view bad_json = R"({"schema":"noemancer.test/0.1",broken})";
    constexpr std::string_view invalid_utf8{"bad\xc0\xaf", 5U};
    const std::string large(192U * 1024U, 'x');
    for (const auto directory : {std::string_view{"source"}, std::string_view{"package"}}) {
        valid = check(fixture.write(directory, "text/readme.txt", utf8_text) &&
                          fixture.write(directory, "data/document.json", json_source) &&
                          fixture.write(directory, "data/bad.json", bad_json) &&
                          fixture.write(directory, "data/invalid.txt", invalid_utf8) &&
                          fixture.write(directory, "data/large.txt", large),
                      "Could not create document reader fixtures.") && valid;
    }

    auto source = mounted_vfs(fixture.root / "source", noemancer::VfsMountKind::directory, "source");
    const auto text = noemancer::read_vfs_document(source, {
        .uri = "document://text/readme.txt",
        .kind = noemancer::VfsDocumentKind::text,
        .byte_budget = 1024U});
    valid = check(text.success && text.code == "ok" && text.text == utf8_text &&
                      text.canonical_json.empty() && text.file.uri == "document://text/readme.txt" &&
                      text.file.mount_id == "source" && text.sha256.starts_with("sha256:"),
                  "UTF-8 text was not returned with stable VFS identity.") && valid;

    const auto json = noemancer::read_vfs_document(source, {
        .uri = "document://data/document.json",
        .kind = noemancer::VfsDocumentKind::json,
        .byte_budget = 1024U,
        .expected_schema = "noemancer.test/0.1"});
    valid = check(json.success && json.text.empty() &&
                      json.canonical_json ==
                          R"({"nested":{"a":1,"b":true},"schemaVersion":"noemancer.test/0.1","z":2})" &&
                      json.sha256.starts_with("sha256:"),
                  "JSON was not validated and canonicalized deterministically.") && valid;

    const auto mismatch = noemancer::read_vfs_document(source, {
        .uri = "document://data/document.json",
        .kind = noemancer::VfsDocumentKind::json,
        .byte_budget = 1024U,
        .expected_schema = "noemancer.other/1"});
    const auto malformed = noemancer::read_vfs_document(source, {
        .uri = "document://data/bad.json",
        .kind = noemancer::VfsDocumentKind::json,
        .byte_budget = 1024U});
    const auto bad_encoding = noemancer::read_vfs_document(source, {
        .uri = "document://data/invalid.txt",
        .kind = noemancer::VfsDocumentKind::text,
        .byte_budget = 1024U});
    valid = check(!mismatch.success && mismatch.code == "vfs.document-schema-mismatch" &&
                      mismatch.canonical_json.empty() && mismatch.sha256.starts_with("sha256:") &&
                      !malformed.success && malformed.code == "vfs.document-json-invalid" &&
                      malformed.canonical_json.empty() &&
                      !bad_encoding.success && bad_encoding.code == "vfs.document-utf8-invalid" &&
                      bad_encoding.text.empty(),
                  "Schema, malformed JSON or UTF-8 failures are not fail-closed.") && valid;

    const auto budget = noemancer::read_vfs_document(source, {
        .uri = "document://data/large.txt",
        .kind = noemancer::VfsDocumentKind::text,
        .byte_budget = 1024U});
    valid = check(!budget.success && budget.code == "vfs.read-budget-exceeded" &&
                      budget.text.empty() && budget.sha256.empty(),
                  "Document byte budget was not delegated to the VFS before publication.") && valid;

    std::atomic_uint32_t cancellation_checks{};
    const auto cancelled = noemancer::read_vfs_document(source, {
        .uri = "document://data/large.txt",
        .kind = noemancer::VfsDocumentKind::text,
        .byte_budget = 256U * 1024U,
        .cancelled = [&] { return cancellation_checks.fetch_add(1U) >= 3U; }});
    valid = check(!cancelled.success && cancelled.code == "vfs.cancelled" &&
                      cancelled.text.empty() && cancelled.canonical_json.empty(),
                  "Cancellation published partial document content.") && valid;

    const auto rejected = noemancer::read_vfs_document(source, {
        .uri = "document://data/../text/readme.txt",
        .kind = noemancer::VfsDocumentKind::text,
        .byte_budget = 1024U});
    valid = check(!rejected.success && rejected.code == "vfs.uri-invalid",
                  "Document reader bypassed canonical VFS path rejection.") && valid;

    auto package = mounted_vfs(
        fixture.root / "package", noemancer::VfsMountKind::package_directory, "package");
    const auto packaged_json = noemancer::read_vfs_document(package, {
        .uri = "document://data/document.json",
        .kind = noemancer::VfsDocumentKind::json,
        .byte_budget = 1024U,
        .expected_schema = "noemancer.test/0.1"});
    valid = check(packaged_json.success && packaged_json.canonical_json == json.canonical_json &&
                      packaged_json.sha256 == json.sha256 && packaged_json.file.uri == json.file.uri &&
                      packaged_json.file.relative_path == json.file.relative_path &&
                      packaged_json.file.mount_id == "package",
                  "Source and package mounts did not produce synonymous document content identity.") && valid;

    return valid ? 0 : 1;
}
