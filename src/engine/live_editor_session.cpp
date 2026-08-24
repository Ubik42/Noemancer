#include "engine/live_editor_session.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <optional>
#include <system_error>
#include <utility>

#if defined(_WIN32)
#define NOMINMAX
#include <Windows.h>
#else
#include <unistd.h>
#endif

namespace noemancer {
namespace {

using Json = nlohmann::json;

constexpr std::size_t max_session_id_bytes = 96U;
constexpr std::size_t max_process_identity_bytes = 192U;
constexpr std::size_t max_project_id_bytes = 192U;
constexpr std::size_t max_project_name_bytes = 192U;
constexpr std::size_t max_project_root_bytes = 2048U;
constexpr std::size_t max_endpoint_bytes = 512U;
constexpr std::size_t max_credential_file_bytes = 128U;
constexpr std::size_t max_credential_bytes = 64U * 1024U;
constexpr std::uint64_t default_stale_after_milliseconds = 120'000U;
constexpr std::uint64_t max_stale_after_milliseconds = 7ULL * 24ULL * 60ULL * 60ULL * 1000ULL;
constexpr std::size_t default_max_discovered_sessions = 128U;
constexpr std::size_t default_max_scan_entries = 256U;

std::uint64_t unix_milliseconds_now() noexcept {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
}

std::optional<std::string> environment_value(const char* name) {
#if defined(_WIN32)
    char* value = nullptr;
    std::size_t length = 0U;
    if (_dupenv_s(&value, &length, name) != 0 || value == nullptr) return std::nullopt;
    std::string result(value, length > 0U ? length - 1U : 0U);
    std::free(value);
    return result;
#else
    const auto* value = std::getenv(name);
    if (value == nullptr) return std::nullopt;
    return std::string(value);
#endif
}

std::filesystem::path default_session_root() {
    if (const auto local_app_data = environment_value("LOCALAPPDATA");
        local_app_data && !local_app_data->empty()) {
        return std::filesystem::path(*local_app_data) / "Noemancer" / "Sessions";
    }
#if !defined(_WIN32)
    if (const auto state_home = environment_value("XDG_STATE_HOME");
        state_home && !state_home->empty()) {
        return std::filesystem::path(*state_home) / "Noemancer" / "Sessions";
    }
#endif
    return std::filesystem::temp_directory_path() / "Noemancer" / "Sessions";
}

bool valid_text(const std::string_view value, const std::size_t limit,
    const bool required = true) noexcept {
    if (required && value.empty()) return false;
    if (value.size() > limit) return false;
    for (const unsigned char character : value) {
        if (character < 0x20U || character == 0x7fU) return false;
    }
    return true;
}

bool valid_component(const std::string_view value, const std::size_t limit) noexcept {
    if (!valid_text(value, limit) || value == "." || value == "..") return false;
    for (const unsigned char character : value) {
        if (std::isalnum(character) != 0 || character == '-' || character == '_' || character == '.') continue;
        return false;
    }
    return true;
}

bool path_is_within(const std::filesystem::path& root,
    const std::filesystem::path& candidate) {
    const auto relative = candidate.lexically_relative(root);
    if (relative.empty() || relative.is_absolute()) return false;
    for (const auto& part : relative) {
        if (part == std::filesystem::path("..")) return false;
    }
    return true;
}

std::optional<std::filesystem::path> root_child(
    const std::filesystem::path& root, const std::string_view filename) {
    if (!valid_component(filename, max_credential_file_bytes) &&
        !(filename.size() > 5U && filename.ends_with(".json") &&
          valid_component(filename.substr(0U, filename.size() - 5U), max_session_id_bytes))) {
        return std::nullopt;
    }
    const auto candidate = (root / std::filesystem::path(std::string(filename))).lexically_normal();
    return path_is_within(root, candidate) ? std::optional<std::filesystem::path>{candidate} : std::nullopt;
}

void append_diagnostic(std::vector<LiveEditorSessionDiagnostic>& diagnostics,
    const std::string_view code, const std::string_view detail,
    const std::filesystem::path& path) {
    if (diagnostics.size() >= LiveEditorSessionStore::max_diagnostics()) return;
    diagnostics.push_back({std::string(code), std::string(detail), path.generic_string()});
}

std::uint64_t json_unsigned(const Json& value, const std::string_view key,
    bool& valid) {
    if (!value.contains(key)) {
        valid = false;
        return 0U;
    }
    const auto& item = value.at(key);
    if (item.is_number_unsigned()) return item.get<std::uint64_t>();
    if (item.is_number_integer()) {
        const auto signed_value = item.get<std::int64_t>();
        if (signed_value >= 0) return static_cast<std::uint64_t>(signed_value);
    }
    valid = false;
    return 0U;
}

std::optional<LiveEditorSessionDescriptor> parse_descriptor(
    const Json& value, std::string& code, std::string& detail) {
    if (!value.is_object()) {
        code = "live-editor-session.descriptor-not-object";
        detail = "The session descriptor root must be a JSON object.";
        return std::nullopt;
    }
    if (value.size() > 32U) {
        code = "live-editor-session.field-limit";
        detail = "The session descriptor contains too many fields.";
        return std::nullopt;
    }
    for (const auto key : {"token", "authToken", "accessToken", "secret"}) {
        if (value.contains(key)) {
            code = "live-editor-session.secret-in-descriptor";
            detail = "Bearer credentials must remain in the referenced sidecar.";
            return std::nullopt;
        }
    }
    LiveEditorSessionDescriptor descriptor;
    const auto read_string = [&](const std::string_view key, std::string& output,
        const std::size_t limit, const bool required) {
        if (!value.contains(key)) return !required;
        const auto& item = value.at(key);
        if (!item.is_string()) return false;
        output = item.get<std::string>();
        return valid_text(output, limit, required);
    };
    if (!read_string("schemaVersion", descriptor.schema_version, 96U, true) ||
        descriptor.schema_version != "noemancer.live-editor-session/0.1") {
        code = "live-editor-session.schema-invalid";
        detail = "The descriptor schemaVersion is unsupported.";
        return std::nullopt;
    }
    bool numbers_valid = true;
    descriptor.version = static_cast<std::uint32_t>(json_unsigned(value, "version", numbers_valid));
    descriptor.process_id = json_unsigned(value, "processId", numbers_valid);
    descriptor.created_unix_milliseconds = json_unsigned(value, "createdUnixMilliseconds", numbers_valid);
    descriptor.heartbeat_unix_milliseconds = json_unsigned(value, "heartbeatUnixMilliseconds", numbers_valid);
    descriptor.revision = json_unsigned(value, "revision", numbers_valid);
    if (!numbers_valid || descriptor.version != 1U || descriptor.process_id == 0U ||
        descriptor.created_unix_milliseconds == 0U || descriptor.heartbeat_unix_milliseconds == 0U ||
        descriptor.revision == 0U || descriptor.heartbeat_unix_milliseconds < descriptor.created_unix_milliseconds) {
        code = "live-editor-session.numeric-field-invalid";
        detail = "The descriptor has an invalid version, process, timestamp or revision.";
        return std::nullopt;
    }
    if (!read_string("sessionId", descriptor.session_id, max_session_id_bytes, true) ||
        !valid_component(descriptor.session_id, max_session_id_bytes) ||
        !read_string("processIdentity", descriptor.process_identity, max_process_identity_bytes, true) ||
        !read_string("projectId", descriptor.project_id, max_project_id_bytes, true) ||
        !read_string("projectName", descriptor.project_name, max_project_name_bytes, false) ||
        !read_string("projectRoot", descriptor.project_root, max_project_root_bytes, false) ||
        !read_string("endpoint", descriptor.endpoint, max_endpoint_bytes, true) ||
        !read_string("credentialFile", descriptor.credential_file, max_credential_file_bytes, false) ||
        (!descriptor.credential_file.empty() && !valid_component(descriptor.credential_file, max_credential_file_bytes))) {
        code = "live-editor-session.string-field-invalid";
        detail = "The descriptor contains an invalid or oversized string field.";
        return std::nullopt;
    }
    if (!value.contains("capabilities") || !value.at("capabilities").is_array() ||
        value.at("capabilities").size() > LiveEditorSessionStore::max_capabilities()) {
        code = "live-editor-session.capability-limit";
        detail = "The descriptor capabilities array is missing or exceeds its bound.";
        return std::nullopt;
    }
    for (const auto& item : value.at("capabilities")) {
        if (!item.is_string() || !valid_component(item.get<std::string>(), LiveEditorSessionStore::max_capability_bytes())) {
            code = "live-editor-session.capability-invalid";
            detail = "Every capability must be a bounded identifier.";
            return std::nullopt;
        }
        descriptor.capabilities.push_back(item.get<std::string>());
    }
    return descriptor;
}

bool credential_file_is_valid(const std::filesystem::path& root,
    const LiveEditorSessionDescriptor& descriptor, std::string& detail) {
    if (descriptor.credential_file.empty()) return true;
    const auto credential_path = root_child(root, descriptor.credential_file);
    if (!credential_path) {
        detail = "The credentialFile must be a root-relative safe filename.";
        return false;
    }
    std::error_code error;
    const auto status = std::filesystem::symlink_status(*credential_path, error);
    if (error || !std::filesystem::is_regular_file(status)) {
        detail = "The referenced credential sidecar is missing or not a regular file.";
        return false;
    }
    const auto bytes = std::filesystem::file_size(*credential_path, error);
    if (error || bytes > max_credential_bytes) {
        detail = "The referenced credential sidecar exceeds its byte bound.";
        return false;
    }
    return true;
}

std::string descriptor_json_text(const LiveEditorSessionDescriptor& descriptor) {
    return Json{
        {"schemaVersion", descriptor.schema_version},
        {"version", descriptor.version},
        {"sessionId", descriptor.session_id},
        {"processId", descriptor.process_id},
        {"processIdentity", descriptor.process_identity},
        {"projectId", descriptor.project_id},
        {"projectName", descriptor.project_name},
        {"projectRoot", descriptor.project_root},
        {"endpoint", descriptor.endpoint},
        {"credentialFile", descriptor.credential_file},
        {"capabilities", descriptor.capabilities},
        {"createdUnixMilliseconds", descriptor.created_unix_milliseconds},
        {"heartbeatUnixMilliseconds", descriptor.heartbeat_unix_milliseconds},
        {"revision", descriptor.revision}}.dump(2) + "\n";
}

bool read_descriptor_file(const std::filesystem::path& path,
    const std::size_t max_bytes, LiveEditorSessionDescriptor& descriptor,
    std::string& code, std::string& detail) {
    std::error_code error;
    const auto status = std::filesystem::symlink_status(path, error);
    if (error || !std::filesystem::is_regular_file(status)) {
        code = "live-editor-session.not-regular-file";
        detail = "The descriptor is not a regular file.";
        return false;
    }
    const auto bytes = std::filesystem::file_size(path, error);
    if (error || bytes > max_bytes) {
        code = "live-editor-session.descriptor-too-large";
        detail = "The descriptor exceeds the configured byte limit.";
        return false;
    }
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        code = "live-editor-session.read-failed";
        detail = "The descriptor could not be opened.";
        return false;
    }
    const std::string source{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    const auto parsed = Json::parse(source, nullptr, false);
    if (parsed.is_discarded()) {
        code = "live-editor-session.json-invalid";
        detail = "The descriptor is not valid JSON.";
        return false;
    }
    const auto decoded = parse_descriptor(parsed, code, detail);
    if (!decoded) return false;
    descriptor = *decoded;
    return true;
}

bool atomic_write(const std::filesystem::path& root,
    const std::filesystem::path& destination, const std::string_view content,
    const bool replace_existing, std::string& error) {
    if (content.size() > LiveEditorSessionStore::max_descriptor_bytes()) {
        error = "The descriptor exceeds the byte limit.";
        return false;
    }
    std::error_code filesystem_error;
    std::filesystem::create_directories(root, filesystem_error);
    if (filesystem_error) {
        error = filesystem_error.message();
        return false;
    }
    static std::atomic<std::uint64_t> sequence{0U};
    const auto temporary = root / ("." + destination.filename().string() + ".tmp-" +
        std::to_string(LiveEditorSessionStore::current_process_id()) + "-" +
        std::to_string(++sequence));
    if (!path_is_within(root, temporary)) {
        error = "Temporary path escaped the session root.";
        return false;
    }
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output) {
            error = "The temporary descriptor could not be opened.";
            return false;
        }
        output.write(content.data(), static_cast<std::streamsize>(content.size()));
        output.flush();
        if (!output) {
            error = "The temporary descriptor could not be written.";
            std::error_code ignored;
            std::filesystem::remove(temporary, ignored);
            return false;
        }
    }
#if defined(_WIN32)
    const auto flags = static_cast<DWORD>(MOVEFILE_WRITE_THROUGH |
        (replace_existing ? MOVEFILE_REPLACE_EXISTING : 0U));
    if (MoveFileExW(temporary.c_str(), destination.c_str(), flags) != 0) return true;
    error = "Atomic descriptor replacement failed with Win32 error " + std::to_string(GetLastError());
#else
    std::filesystem::rename(temporary, destination, filesystem_error);
    if (!filesystem_error) return true;
    error = filesystem_error.message();
#endif
    std::error_code ignored;
    std::filesystem::remove(temporary, ignored);
    return false;
}

LiveEditorSessionWriteReceipt failure_receipt(
    const std::string_view code, const std::string_view detail,
    const std::filesystem::path& path = {}) {
    return {false, false, false, false, std::string(code), std::string(detail),
        path.generic_string(), 0U};
}

} // namespace

LiveEditorSessionStore::LiveEditorSessionStore(LiveEditorSessionStoreOptions options)
    : options_(std::move(options)) {
    if (options_.stale_after_milliseconds == 0U ||
        options_.stale_after_milliseconds > max_stale_after_milliseconds) {
        options_.stale_after_milliseconds = default_stale_after_milliseconds;
    }
    if (options_.max_discovered_sessions == 0U) {
        options_.max_discovered_sessions = default_max_discovered_sessions;
    }
    options_.max_discovered_sessions = std::min(options_.max_discovered_sessions,
        LiveEditorSessionStore::max_scan_entries());
    if (options_.max_scan_entries == 0U) options_.max_scan_entries = default_max_scan_entries;
    options_.max_scan_entries = std::min(options_.max_scan_entries,
        LiveEditorSessionStore::max_scan_entries());
    root_ = options_.root_override.empty() ? default_session_root() : options_.root_override;
    std::error_code error;
    root_ = std::filesystem::absolute(root_, error).lexically_normal();
    if (error || root_.empty()) root_ = default_session_root().lexically_normal();
}

std::string LiveEditorSessionStore::descriptor_json(
    const LiveEditorSessionDescriptor& descriptor) {
    return descriptor_json_text(descriptor);
}

std::uint64_t LiveEditorSessionStore::current_process_id() noexcept {
#if defined(_WIN32)
    return static_cast<std::uint64_t>(GetCurrentProcessId());
#else
    return static_cast<std::uint64_t>(getpid());
#endif
}

std::string LiveEditorSessionStore::current_process_identity() {
#if defined(_WIN32)
    FILETIME created{}, exited{}, kernel{}, user{};
    if (GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user) != 0) {
        const auto creation_ticks = (static_cast<std::uint64_t>(created.dwHighDateTime) << 32U) |
            static_cast<std::uint64_t>(created.dwLowDateTime);
        return "pid-" + std::to_string(current_process_id()) + "-created-" +
            std::to_string(creation_ticks);
    }
#endif
    // The owner string is a compare-and-swap identity, not a credential.
    // Keep a process-lifetime nonce so PID reuse cannot claim an old record.
    static const auto process_nonce = unix_milliseconds_now();
    return "pid-" + std::to_string(current_process_id()) + "-started-" +
        std::to_string(process_nonce);
}

LiveEditorSessionWriteReceipt LiveEditorSessionStore::publish(
    LiveEditorSessionDescriptor descriptor) {
    if (descriptor.schema_version.empty()) descriptor.schema_version = "noemancer.live-editor-session/0.1";
    if (descriptor.version == 0U) descriptor.version = 1U;
    const auto now = options_.now_unix_milliseconds == 0U ? unix_milliseconds_now() : options_.now_unix_milliseconds;
    if (descriptor.created_unix_milliseconds == 0U) descriptor.created_unix_milliseconds = now;
    if (descriptor.heartbeat_unix_milliseconds == 0U) descriptor.heartbeat_unix_milliseconds = now;
    if (descriptor.revision == 0U) descriptor.revision = 1U;
    const auto target = root_child(root_, descriptor.session_id + ".json");
    if (!target) return failure_receipt("live-editor-session.path-invalid",
        "The sessionId does not produce a safe descriptor filename.");
    if (!valid_text(descriptor.schema_version, 96U) || descriptor.schema_version != "noemancer.live-editor-session/0.1" ||
        descriptor.version != 1U || descriptor.process_id == 0U ||
        !valid_component(descriptor.session_id, max_session_id_bytes) ||
        !valid_text(descriptor.process_identity, max_process_identity_bytes) ||
        !valid_text(descriptor.project_id, max_project_id_bytes) ||
        !valid_text(descriptor.project_name, max_project_name_bytes, false) ||
        !valid_text(descriptor.project_root, max_project_root_bytes, false) ||
        !valid_text(descriptor.endpoint, max_endpoint_bytes) ||
        (!descriptor.credential_file.empty() && !valid_component(descriptor.credential_file, max_credential_file_bytes)) ||
        descriptor.created_unix_milliseconds == 0U || descriptor.heartbeat_unix_milliseconds < descriptor.created_unix_milliseconds ||
        descriptor.revision != 1U || descriptor.capabilities.size() > max_capabilities()) {
        return failure_receipt("live-editor-session.descriptor-invalid",
            "The descriptor contains a field outside its bounded contract.", *target);
    }
    for (const auto& capability : descriptor.capabilities) {
        if (!valid_component(capability, max_capability_bytes()))
            return failure_receipt("live-editor-session.capability-invalid",
                "A capability is not a bounded identifier.", *target);
    }
    std::error_code error;
    if (std::filesystem::exists(*target, error)) {
        return failure_receipt("live-editor-session.exists",
            "The sessionId is already published.", *target);
    }
    if (error) return failure_receipt("live-editor-session.path-check-failed", error.message(), *target);
    std::string credential_detail;
    if (!credential_file_is_valid(root_, descriptor, credential_detail))
        return failure_receipt("live-editor-session.credential-invalid", credential_detail, *target);
    const auto content = descriptor_json_text(descriptor);
    std::string write_error;
    if (!atomic_write(root_, *target, content, false, write_error))
        return failure_receipt("live-editor-session.publish-failed", write_error, *target);
    return {true, true, false, descriptor.credential_file.empty(), "ok",
        "Editor session descriptor published atomically.", target->generic_string(), descriptor.revision};
}

LiveEditorSessionWriteReceipt LiveEditorSessionStore::refresh(
    const std::string_view session_id, const std::string_view process_identity,
    const std::uint64_t expected_revision) {
    const auto target = root_child(root_, std::string(session_id) + ".json");
    if (!target) return failure_receipt("live-editor-session.path-invalid",
        "The sessionId does not produce a safe descriptor filename.");
    if (!valid_text(process_identity, max_process_identity_bytes) || expected_revision == 0U)
        return failure_receipt("live-editor-session.request-invalid",
            "Refresh requires a process identity and non-zero expected revision.", *target);
    LiveEditorSessionDescriptor descriptor;
    std::string code;
    std::string detail;
    if (!read_descriptor_file(*target, max_descriptor_bytes(), descriptor, code, detail))
        return failure_receipt(code == "live-editor-session.read-failed" ? code : "live-editor-session.not-found", detail, *target);
    if (descriptor.session_id != session_id || descriptor.process_identity != process_identity)
        return failure_receipt("live-editor-session.owner-mismatch",
            "The caller does not own this Editor session.", *target);
    if (descriptor.revision != expected_revision)
        return failure_receipt("live-editor-session.revision-conflict",
            "The session revision is stale.", *target);
    if (descriptor.revision == std::numeric_limits<std::uint64_t>::max())
        return failure_receipt("live-editor-session.revision-overflow",
            "The session revision cannot advance further.", *target);
    const auto now = options_.now_unix_milliseconds == 0U ? unix_milliseconds_now() : options_.now_unix_milliseconds;
    descriptor.heartbeat_unix_milliseconds = std::max(now, descriptor.created_unix_milliseconds);
    ++descriptor.revision;
    std::string write_error;
    if (!atomic_write(root_, *target, descriptor_json_text(descriptor), true, write_error))
        return failure_receipt("live-editor-session.refresh-failed", write_error, *target);
    return {true, true, false, descriptor.credential_file.empty(), "ok",
        "Editor session heartbeat refreshed atomically.", target->generic_string(), descriptor.revision};
}

LiveEditorSessionWriteReceipt LiveEditorSessionStore::revoke(
    const std::string_view session_id, const std::string_view process_identity,
    const std::uint64_t expected_revision) {
    const auto target = root_child(root_, std::string(session_id) + ".json");
    if (!target) return failure_receipt("live-editor-session.path-invalid",
        "The sessionId does not produce a safe descriptor filename.");
    if (!valid_text(process_identity, max_process_identity_bytes) || expected_revision == 0U)
        return failure_receipt("live-editor-session.request-invalid",
            "Revoke requires a process identity and non-zero expected revision.", *target);
    LiveEditorSessionDescriptor descriptor;
    std::string code;
    std::string detail;
    if (!read_descriptor_file(*target, max_descriptor_bytes(), descriptor, code, detail))
        return failure_receipt("live-editor-session.not-found", detail, *target);
    if (descriptor.session_id != session_id || descriptor.process_identity != process_identity)
        return failure_receipt("live-editor-session.owner-mismatch",
            "The caller does not own this Editor session.", *target);
    if (descriptor.revision != expected_revision)
        return failure_receipt("live-editor-session.revision-conflict",
            "The session revision is stale.", *target);
    std::error_code error;
    const bool descriptor_removed = std::filesystem::remove(*target, error);
    if (!descriptor_removed || error)
        return failure_receipt("live-editor-session.revoke-failed",
            error ? error.message() : "The session descriptor could not be removed.", *target);
    bool credential_removed = true;
    if (!descriptor.credential_file.empty()) {
        const auto credential = root_child(root_, descriptor.credential_file);
        if (!credential) {
            credential_removed = false;
        } else {
            error.clear();
            credential_removed = !std::filesystem::exists(*credential, error) ||
                std::filesystem::remove(*credential, error);
            credential_removed = credential_removed && !error;
        }
    }
    if (!credential_removed) return {false, true, true, false,
        "live-editor-session.credential-cleanup-failed",
        "The descriptor was revoked but the credential sidecar needs cleanup.",
        target->generic_string(), descriptor.revision};
    return {true, true, true, true, "ok",
        "Editor session descriptor and credential reference were revoked.",
        target->generic_string(), descriptor.revision};
}

LiveEditorSessionDiscovery LiveEditorSessionStore::discover() {
    LiveEditorSessionDiscovery result{true, "ok", "Editor session discovery completed.", {}, {}, 0U};
    std::error_code error;
    if (!std::filesystem::exists(root_, error)) return result;
    if (error) return {false, "live-editor-session.root-stat-failed", error.message(), {}, {}, 0U};
    if (!std::filesystem::is_directory(root_, error) || error)
        return {false, "live-editor-session.root-invalid", "The session root is not a directory.", {}, {}, 0U};
    std::vector<std::filesystem::path> candidates;
    for (std::filesystem::directory_iterator iterator(root_, error), end; iterator != end && !error; iterator.increment(error)) {
        if (candidates.size() >= options_.max_scan_entries) {
            append_diagnostic(result.diagnostics, "live-editor-session.scan-limit",
                "The session directory exceeded its entry scan bound.", root_);
            break;
        }
        const auto path = iterator->path().lexically_normal();
        if (!path_is_within(root_, path)) {
            append_diagnostic(result.diagnostics, "live-editor-session.path-outside-root",
                "A directory entry escaped the session root.", path);
            continue;
        }
        std::error_code status_error;
        const auto status = std::filesystem::symlink_status(path, status_error);
        if (status_error || std::filesystem::is_symlink(status) || !std::filesystem::is_regular_file(status)) continue;
        if (path.extension() == ".json") candidates.push_back(path);
    }
    if (error) return {false, "live-editor-session.scan-failed", error.message(), {}, result.diagnostics, result.stale_removed};
    std::ranges::sort(candidates, [](const auto& left, const auto& right) {
        return left.generic_string() < right.generic_string();
    });
    const auto now = options_.now_unix_milliseconds == 0U ? unix_milliseconds_now() : options_.now_unix_milliseconds;
    for (const auto& path : candidates) {
        const auto stem = path.stem().string();
        if (!valid_component(stem, max_session_id_bytes)) {
            append_diagnostic(result.diagnostics, "live-editor-session.filename-invalid",
                "The descriptor filename is not a safe session ID.", path);
            continue;
        }
        LiveEditorSessionDescriptor descriptor;
        std::string code;
        std::string detail;
        if (!read_descriptor_file(path, max_descriptor_bytes(), descriptor, code, detail)) {
            append_diagnostic(result.diagnostics, code, detail, path);
            continue;
        }
        if (descriptor.session_id != stem) {
            append_diagnostic(result.diagnostics, "live-editor-session.filename-mismatch",
                "The filename does not match sessionId.", path);
            continue;
        }
        const bool stale = now > descriptor.heartbeat_unix_milliseconds &&
            now - descriptor.heartbeat_unix_milliseconds > options_.stale_after_milliseconds;
        if (stale) {
            std::error_code remove_error;
            const bool removed = std::filesystem::remove(path, remove_error);
            if (removed && !remove_error) ++result.stale_removed;
            else append_diagnostic(result.diagnostics, "live-editor-session.stale-remove-failed",
                remove_error ? remove_error.message() : "The stale descriptor could not be removed.", path);
            if (!descriptor.credential_file.empty()) {
                if (const auto credential = root_child(root_, descriptor.credential_file)) {
                    remove_error.clear();
                    static_cast<void>(std::filesystem::remove(*credential, remove_error));
                    if (remove_error) append_diagnostic(result.diagnostics,
                        "live-editor-session.stale-credential-remove-failed", remove_error.message(), *credential);
                }
            }
            continue;
        }
        if (!credential_file_is_valid(root_, descriptor, detail)) {
            append_diagnostic(result.diagnostics, "live-editor-session.credential-invalid", detail, path);
            continue;
        }
        if (result.sessions.size() >= options_.max_discovered_sessions) {
            append_diagnostic(result.diagnostics, "live-editor-session.session-limit",
                "The discovered session count exceeded its bound.", path);
            continue;
        }
        result.sessions.push_back(std::move(descriptor));
    }
    std::ranges::sort(result.sessions, {}, &LiveEditorSessionDescriptor::session_id);
    return result;
}

} // namespace noemancer
