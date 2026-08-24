#include "engine/live_editor_session.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

using noemancer::LiveEditorSessionDescriptor;
using noemancer::LiveEditorSessionStore;
using noemancer::LiveEditorSessionStoreOptions;

void require(const bool condition, const std::string_view message) {
    if (!condition) throw std::runtime_error(std::string(message));
}

LiveEditorSessionDescriptor descriptor(
    const std::string_view id, const std::string_view process,
    const std::uint64_t created = 0U, const std::string_view credential = {}) {
    return {
        .session_id = std::string(id),
        .process_id = 42U,
        .process_identity = std::string(process),
        .project_id = "game.test.project",
        .project_name = "Session Fixture",
        .project_root = "D:/fixtures/session-project",
        .endpoint = "pipe://noemancer/editor/session",
        .credential_file = std::string(credential),
        .capabilities = {"observe", "edit", "play"},
        .created_unix_milliseconds = created,
        .heartbeat_unix_milliseconds = created,
        .revision = 1U};
}

std::filesystem::path fixture_root() {
    const auto root = std::filesystem::temp_directory_path() /
        ("noemancer-live-editor-session-" + std::to_string(LiveEditorSessionStore::current_process_id()));
    std::error_code error;
    std::filesystem::remove_all(root, error);
    std::filesystem::create_directories(root, error);
    if (error) throw std::runtime_error("could not create fixture root");
    return root;
}

bool has_temporary_file(const std::filesystem::path& root) {
    std::error_code error;
    for (const auto& entry : std::filesystem::directory_iterator(root, error)) {
        if (entry.path().filename().string().find(".tmp-") != std::string::npos) return true;
    }
    return false;
}

void write_text(const std::filesystem::path& path, const std::string_view text) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    require(static_cast<bool>(output), "fixture file could not be opened");
    output << text;
    require(static_cast<bool>(output), "fixture file could not be written");
}

} // namespace

int main() {
    const auto root = fixture_root();
    try {
        const LiveEditorSessionStoreOptions options{
            .root_override = root,
            .now_unix_milliseconds = 1'000U,
            .stale_after_milliseconds = 200U,
            .max_discovered_sessions = 2U,
            .max_scan_entries = 12U};
        LiveEditorSessionStore store(options);
        write_text(root / "editor-credential.sidecar", "opaque-local-secret");

        const auto published = store.publish(descriptor("editor-main", "process-main", 1'000U,
            "editor-credential.sidecar"));
        require(published.success && published.atomic && published.revision == 1U,
            "publish did not return an atomic revision-one receipt");
        require(!has_temporary_file(root), "atomic publish left a temporary descriptor");
        const auto document_path = root / "editor-main.json";
        require(std::filesystem::is_regular_file(document_path), "publish did not create descriptor");
        const auto source = [&] {
            std::ifstream input(document_path, std::ios::binary);
            return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
        }();
        const auto parsed = nlohmann::json::parse(source, nullptr, false);
        require(!parsed.is_discarded() && parsed.value("schemaVersion", "") ==
            "noemancer.live-editor-session/0.1" && parsed.value("revision", 0U) == 1U,
            "published descriptor schema or revision was not stable");
        require(source.find("opaque-local-secret") == std::string::npos,
            "credential contents leaked into the descriptor");

        auto discovery = store.discover();
        require(discovery.success && discovery.sessions.size() == 1U &&
            discovery.sessions.front().session_id == "editor-main",
            "published session was not discoverable");
        const auto refreshed = store.refresh("editor-main", "process-main", 1U);
        require(refreshed.success && refreshed.atomic && refreshed.revision == 2U,
            "refresh did not atomically advance the revision");
        require(!has_temporary_file(root), "atomic refresh left a temporary descriptor");
        const auto before_stale_refresh = [&] {
            std::ifstream input(document_path, std::ios::binary);
            return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
        }();
        const auto stale_refresh = store.refresh("editor-main", "process-main", 1U);
        require(!stale_refresh.success && stale_refresh.code == "live-editor-session.revision-conflict",
            "stale refresh was not rejected by revision CAS");
        const auto after_stale_refresh = [&] {
            std::ifstream input(document_path, std::ios::binary);
            return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
        }();
        require(before_stale_refresh == after_stale_refresh,
            "stale refresh changed the descriptor bytes");
        const auto wrong_owner = store.revoke("editor-main", "other-process", 2U);
        require(!wrong_owner.success && wrong_owner.code == "live-editor-session.owner-mismatch",
            "revoke did not enforce process ownership");
        const auto revoked = store.revoke("editor-main", "process-main", 2U);
        require(revoked.success && revoked.atomic && revoked.descriptor_removed &&
            revoked.credential_removed && !std::filesystem::exists(document_path) &&
            !std::filesystem::exists(root / "editor-credential.sidecar"),
            "revoke did not remove descriptor and credential sidecar");

        // A stale descriptor is removed together with its sidecar, while a
        // fresh descriptor remains discoverable.  Discovery uses a separate
        // clock view so this is deterministic without sleeping.
        write_text(root / "stale-credential.sidecar", "stale-secret");
        LiveEditorSessionStore old_store({.root_override = root, .now_unix_milliseconds = 1'000U,
            .stale_after_milliseconds = 200U, .max_discovered_sessions = 2U, .max_scan_entries = 12U});
        require(old_store.publish(descriptor("stale-editor", "process-stale", 1'000U,
            "stale-credential.sidecar")).success, "stale fixture publish failed");
        require(old_store.publish(descriptor("fresh-editor", "process-fresh", 1'000U)).success,
            "fresh fixture publish failed");
        LiveEditorSessionStore future_store({.root_override = root, .now_unix_milliseconds = 1'100U,
            .stale_after_milliseconds = 200U, .max_discovered_sessions = 2U, .max_scan_entries = 12U});
        const auto fresh_discovery = future_store.discover();
        require(fresh_discovery.success && fresh_discovery.sessions.size() == 2U &&
            fresh_discovery.stale_removed == 0U, "fresh sessions were incorrectly removed");
        LiveEditorSessionStore stale_store({.root_override = root, .now_unix_milliseconds = 2'000U,
            .stale_after_milliseconds = 200U, .max_discovered_sessions = 2U, .max_scan_entries = 12U});
        const auto stale_discovery = stale_store.discover();
        require(stale_discovery.success && stale_discovery.sessions.empty() &&
            stale_discovery.stale_removed == 2U &&
            !std::filesystem::exists(root / "stale-editor.json") &&
            !std::filesystem::exists(root / "stale-credential.sidecar"),
            "stale discovery did not clean bounded session records");

        // Corrupt, secret-bearing and oversized records are diagnostics, not
        // process-fatal exceptions; discovery remains bounded.
        write_text(root / "corrupt.json", "{not-json");
        auto secret_json = nlohmann::json::parse(
            LiveEditorSessionStore::descriptor_json(descriptor("secret-editor", "process-secret", 2'000U)));
        secret_json["token"] = "must-not-be-accepted";
        write_text(root / "secret-editor.json", secret_json.dump());
        write_text(root / "oversized.json", std::string(LiveEditorSessionStore::max_descriptor_bytes() + 1U, 'x'));
        const auto bounded_discovery = future_store.discover();
        require(bounded_discovery.success && bounded_discovery.sessions.empty() &&
            bounded_discovery.diagnostics.size() <= LiveEditorSessionStore::max_diagnostics(),
            "corrupt or oversized descriptors escaped bounded diagnostics");

        const auto unsafe_id = store.publish(descriptor("../escape", "process-unsafe", 3'000U));
        require(!unsafe_id.success && unsafe_id.code == "live-editor-session.path-invalid",
            "path traversal session ID was accepted");
        auto unsafe_credential = descriptor("unsafe-credential", "process-unsafe", 3'000U, "../secret");
        const auto unsafe_credential_result = store.publish(std::move(unsafe_credential));
        require(!unsafe_credential_result.success,
            "path traversal credential reference was accepted");

        // Three valid files exercise the discovery count cap and deterministic
        // ordering without requiring an unbounded scan.
        require(store.publish(descriptor("bounded-a", "process-a", 1'000U)).success,
            "bounded fixture A publish failed");
        require(store.publish(descriptor("bounded-b", "process-b", 1'000U)).success,
            "bounded fixture B publish failed");
        require(store.publish(descriptor("bounded-c", "process-c", 1'000U)).success,
            "bounded fixture C publish failed");
        const auto limited = store.discover();
        require(limited.sessions.size() <= 2U && !limited.diagnostics.empty(),
            "discovery did not enforce its session count bound");

        std::error_code cleanup_error;
        std::filesystem::remove_all(root, cleanup_error);
        if (cleanup_error) throw std::runtime_error("fixture cleanup failed");
        std::cout << "Live editor session contract passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        std::error_code cleanup_error;
        std::filesystem::remove_all(root, cleanup_error);
        return 1;
    }
}
