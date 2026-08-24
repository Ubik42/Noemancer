#include "runtime/live_editor_transport.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <exception>
#include <fstream>
#include <limits>
#include <mutex>
#include <optional>
#include <random>
#include <sstream>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <aclapi.h>
#include <bcrypt.h>
#endif

namespace noemancer {
namespace {

using Json = nlohmann::json;

std::string json_string_member(const Json& value, const std::string_view key,
                               const std::string_view fallback = {}) {
    if (!value.is_object()) return std::string(fallback);
    const auto iterator = value.find(std::string(key));
    if (iterator == value.end() || !iterator->is_string()) return std::string(fallback);
    return iterator->get<std::string>();
}

bool json_bool_member(const Json& value, const std::string_view key, const bool fallback = false) {
    if (!value.is_object()) return fallback;
    const auto iterator = value.find(std::string(key));
    return iterator != value.end() && iterator->is_boolean() ? iterator->get<bool>() : fallback;
}

constexpr std::size_t minimum_request_bytes = 256U;
constexpr std::size_t maximum_request_bytes = 1024U * 1024U;
constexpr std::size_t minimum_response_bytes = 256U;
constexpr std::size_t maximum_response_bytes = 4U * 1024U * 1024U;
constexpr std::size_t maximum_connections = 32U;
constexpr std::size_t maximum_queue_entries = 1024U;
constexpr std::uint32_t minimum_timeout_milliseconds = 10U;
constexpr std::uint32_t maximum_timeout_milliseconds = 60000U;
constexpr std::size_t maximum_credential_bytes = 4096U;
constexpr std::size_t maximum_identifier_bytes = 128U;

LiveEditorTransportLimits normalize_limits(LiveEditorTransportLimits value) {
    value.max_connections = std::clamp(value.max_connections, std::size_t{1U}, maximum_connections);
    value.max_request_bytes = std::clamp(value.max_request_bytes, minimum_request_bytes, maximum_request_bytes);
    value.max_response_bytes = std::clamp(value.max_response_bytes, minimum_response_bytes, maximum_response_bytes);
    value.max_queued_requests = std::clamp(value.max_queued_requests, std::size_t{1U}, maximum_queue_entries);
    value.max_pending_requests = std::clamp(value.max_pending_requests, std::size_t{1U}, maximum_queue_entries);
    value.handshake_timeout_milliseconds = std::clamp(
        value.handshake_timeout_milliseconds, minimum_timeout_milliseconds, maximum_timeout_milliseconds);
    value.request_timeout_milliseconds = std::clamp(
        value.request_timeout_milliseconds, minimum_timeout_milliseconds, maximum_timeout_milliseconds);
    return value;
}

LiveEditorTransportOperation operation_failure(std::string code, std::string detail) {
    return {false, std::move(code), std::move(detail)};
}

LiveEditorTransportOperation operation_ok(std::string detail) {
    return {true, "ok", std::move(detail)};
}

std::string trim_credential(std::string value) {
    while (!value.empty() && (value.back() == '\r' || value.back() == '\n' || value.back() == ' ' ||
                              value.back() == '\t')) {
        value.pop_back();
    }
    std::size_t first = 0U;
    while (first < value.size() && (value[first] == ' ' || value[first] == '\t')) ++first;
    if (first != 0U) value.erase(0U, first);
    return value;
}

bool valid_credential(std::string_view token) {
    if (token.empty() || token.size() > maximum_credential_bytes) return false;
    return std::ranges::all_of(token, [](const unsigned char character) {
        return character >= 0x21U && character != 0x7fU && character != '\r' && character != '\n';
    });
}

bool constant_time_equal(const std::string_view left, const std::string_view right) noexcept {
    const auto size = std::max(left.size(), right.size());
    std::size_t difference = left.size() ^ right.size();
    for (std::size_t index = 0U; index < size; ++index) {
        const auto left_byte = index < left.size() ? static_cast<unsigned char>(left[index]) : 0U;
        const auto right_byte = index < right.size() ? static_cast<unsigned char>(right[index]) : 0U;
        difference |= left_byte ^ right_byte;
    }
    return difference == 0U;
}

std::optional<std::string> read_credential(const LiveEditorTransportDescriptor& descriptor) {
    if (!descriptor.auth_token.empty()) {
        if (!valid_credential(descriptor.auth_token)) return std::nullopt;
        return descriptor.auth_token;
    }
    if (descriptor.credential_file.empty()) return std::nullopt;
    std::error_code size_error;
    if (!std::filesystem::is_regular_file(descriptor.credential_file, size_error) || size_error ||
        std::filesystem::file_size(descriptor.credential_file, size_error) > maximum_credential_bytes || size_error) {
        return std::nullopt;
    }
    std::ifstream stream(descriptor.credential_file, std::ios::binary);
    if (!stream) return std::nullopt;
    std::string token{std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>{}};
    token = trim_credential(std::move(token));
    if (!valid_credential(token)) return std::nullopt;
    return token;
}

#ifdef _WIN32

std::wstring utf8_to_wide(const std::string_view text) {
    if (text.empty()) return {};
    const auto required = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (required <= 0) return {};
    std::wstring result(static_cast<std::size_t>(required), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()),
            result.data(), required) != required) {
        return {};
    }
    return result;
}

bool valid_endpoint(const std::string_view endpoint) {
    return endpoint.starts_with("\\\\.\\pipe\\") && endpoint.size() <= 240U &&
        endpoint.find_first_of("\r\n") == std::string_view::npos;
}

std::string endpoint_name(std::string_view name) {
    std::string result;
    result.reserve(std::min(name.size(), std::size_t{96U}));
    for (const auto character : name) {
        const auto value = static_cast<unsigned char>(character);
        if ((value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z') ||
            (value >= '0' && value <= '9') || value == '-' || value == '_') {
            result.push_back(static_cast<char>(value));
        } else if (result.empty() || result.back() != '-') {
            result.push_back('-');
        }
        if (result.size() >= 96U) break;
    }
    while (!result.empty() && result.back() == '-') result.pop_back();
    if (result.empty()) result = "default";
    return "\\\\.\\pipe\\noemancer-live-editor-" + result;
}

enum class PipeReadResult : std::uint8_t {
    ok,
    eof,
    too_large,
    timeout,
    failed
};

PipeReadResult read_line_blocking(const HANDLE pipe, const std::size_t maximum_bytes, std::string& line) {
    line.clear();
    for (;;) {
        char character{};
        DWORD read_bytes{};
        if (!ReadFile(pipe, &character, 1U, &read_bytes, nullptr) || read_bytes == 0U) {
            const auto error = GetLastError();
            return error == ERROR_BROKEN_PIPE || error == ERROR_PIPE_NOT_CONNECTED || error == ERROR_HANDLE_EOF
                ? PipeReadResult::eof
                : PipeReadResult::failed;
        }
        if (character == '\n') return PipeReadResult::ok;
        if (character != '\r') line.push_back(character);
        if (line.size() >= maximum_bytes) return PipeReadResult::too_large;
    }
}

PipeReadResult read_line_timeout(const HANDLE pipe, const std::size_t maximum_bytes,
                                 const std::uint32_t timeout_milliseconds, std::string& line) {
    line.clear();
    const auto deadline = std::chrono::steady_clock::now() +
        std::chrono::milliseconds(timeout_milliseconds);
    for (;;) {
        DWORD available{};
        if (!PeekNamedPipe(pipe, nullptr, 0U, nullptr, &available, nullptr)) {
            const auto error = GetLastError();
            return error == ERROR_BROKEN_PIPE || error == ERROR_PIPE_NOT_CONNECTED || error == ERROR_HANDLE_EOF
                ? PipeReadResult::eof
                : PipeReadResult::failed;
        }
        if (available != 0U) {
            char character{};
            DWORD read_bytes{};
            if (!ReadFile(pipe, &character, 1U, &read_bytes, nullptr) || read_bytes == 0U)
                return PipeReadResult::failed;
            if (character == '\n') return PipeReadResult::ok;
            if (character != '\r') line.push_back(character);
            if (line.size() >= maximum_bytes) return PipeReadResult::too_large;
            continue;
        }
        if (std::chrono::steady_clock::now() >= deadline) return PipeReadResult::timeout;
        Sleep(1U);
    }
}

PipeReadResult read_line_until_stopped(const HANDLE pipe, const std::size_t maximum_bytes,
                                       const std::atomic<bool>& stopping, std::string& line) {
    line.clear();
    while (!stopping.load()) {
        DWORD available{};
        if (!PeekNamedPipe(pipe, nullptr, 0U, nullptr, &available, nullptr)) {
            const auto error = GetLastError();
            return error == ERROR_BROKEN_PIPE || error == ERROR_PIPE_NOT_CONNECTED || error == ERROR_HANDLE_EOF
                ? PipeReadResult::eof
                : PipeReadResult::failed;
        }
        if (available == 0U) {
            Sleep(1U);
            continue;
        }
        char character{};
        DWORD read_bytes{};
        if (!ReadFile(pipe, &character, 1U, &read_bytes, nullptr) || read_bytes == 0U)
            return PipeReadResult::failed;
        if (character == '\n') return PipeReadResult::ok;
        if (character != '\r') line.push_back(character);
        if (line.size() >= maximum_bytes) return PipeReadResult::too_large;
    }
    return PipeReadResult::eof;
}

bool write_bytes(const HANDLE pipe, std::string_view bytes) {
    while (!bytes.empty()) {
        const auto count = static_cast<DWORD>(std::min<std::size_t>(bytes.size(), 64U * 1024U));
        DWORD written{};
        if (!WriteFile(pipe, bytes.data(), count, &written, nullptr) || written == 0U) return false;
        bytes.remove_prefix(written);
    }
    return true;
}

bool write_line(const HANDLE pipe, const std::string_view line, const std::size_t maximum_bytes) {
    if (line.empty() || line.size() >= maximum_bytes) return false;
    std::string framed(line);
    framed.push_back('\n');
    return write_bytes(pipe, framed);
}

void cancel_and_close(HANDLE& pipe) {
    if (pipe == INVALID_HANDLE_VALUE) return;
    const auto handle = pipe;
    pipe = INVALID_HANDLE_VALUE;
    CancelIoEx(handle, nullptr);
    CloseHandle(handle);
}

struct PipeGuard final {
    HANDLE value{INVALID_HANDLE_VALUE};
    ~PipeGuard() { cancel_and_close(value); }
    PipeGuard(const PipeGuard&) = delete;
    PipeGuard& operator=(const PipeGuard&) = delete;
};

struct PipeSecurity final {
    SECURITY_ATTRIBUTES attributes{};
    PSECURITY_DESCRIPTOR descriptor{};
    PACL dacl{};
    std::vector<unsigned char> token_user_buffer;

    PipeSecurity() = default;

    ~PipeSecurity() {
        if (dacl != nullptr) LocalFree(dacl);
        if (descriptor != nullptr) LocalFree(descriptor);
    }

    PipeSecurity(const PipeSecurity&) = delete;
    PipeSecurity& operator=(const PipeSecurity&) = delete;

    [[nodiscard]] bool initialize() {
        HANDLE token = INVALID_HANDLE_VALUE;
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;

        DWORD required = 0U;
        static_cast<void>(GetTokenInformation(token, TokenUser, nullptr, 0U, &required));
        if (required == 0U || GetLastError() != ERROR_INSUFFICIENT_BUFFER) {
            CloseHandle(token);
            return false;
        }
        token_user_buffer.resize(required);
        if (!GetTokenInformation(token, TokenUser, token_user_buffer.data(), required, &required)) {
            CloseHandle(token);
            return false;
        }
        CloseHandle(token);

        const auto token_user = reinterpret_cast<const PTOKEN_USER>(token_user_buffer.data());
        if (!IsValidSid(token_user->User.Sid)) return false;
        EXPLICIT_ACCESSW access{};
        access.grfAccessPermissions = FILE_GENERIC_READ | FILE_GENERIC_WRITE;
        access.grfAccessMode = SET_ACCESS;
        access.grfInheritance = NO_INHERITANCE;
        access.Trustee.TrusteeForm = TRUSTEE_IS_SID;
        access.Trustee.TrusteeType = TRUSTEE_IS_USER;
        access.Trustee.ptstrName = reinterpret_cast<LPWSTR>(token_user->User.Sid);
        if (SetEntriesInAclW(1U, &access, nullptr, &dacl) != ERROR_SUCCESS || dacl == nullptr) return false;

        descriptor = static_cast<PSECURITY_DESCRIPTOR>(LocalAlloc(LPTR, SECURITY_DESCRIPTOR_MIN_LENGTH));
        if (descriptor == nullptr || !InitializeSecurityDescriptor(descriptor, SECURITY_DESCRIPTOR_REVISION) ||
            !SetSecurityDescriptorDacl(descriptor, TRUE, dacl, FALSE)) {
            return false;
        }
        attributes.nLength = sizeof(attributes);
        attributes.lpSecurityDescriptor = descriptor;
        attributes.bInheritHandle = FALSE;
        return true;
    }
};

HANDLE create_server_pipe(const std::wstring& endpoint, const LiveEditorTransportLimits& limits) {
    DWORD mode = PIPE_ACCESS_DUPLEX;
    // Named-pipe buffer sizes are transport hints, not protocol frame limits.
    // Keeping them within the Windows 64 KiB pipe-buffer range avoids
    // ERROR_INVALID_PARAMETER while framing still enforces the larger bounds.
    constexpr std::size_t maximum_pipe_buffer = 64U * 1024U;
    const auto input_buffer = static_cast<DWORD>(std::min(limits.max_request_bytes, maximum_pipe_buffer));
    const auto output_buffer = static_cast<DWORD>(std::min(limits.max_response_bytes, maximum_pipe_buffer));
    PipeSecurity security;
    if (!security.initialize()) {
        SetLastError(ERROR_ACCESS_DENIED);
        return INVALID_HANDLE_VALUE;
    }
    return CreateNamedPipeW(endpoint.c_str(), mode,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
        PIPE_UNLIMITED_INSTANCES, output_buffer, input_buffer, limits.handshake_timeout_milliseconds,
        &security.attributes);
}

std::string random_credential_token() {
    std::array<unsigned char, 32U> bytes{};
    if (BCryptGenRandom(nullptr, bytes.data(), static_cast<ULONG>(bytes.size()),
            BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) {
        return {};
    }
    static constexpr char alphabet[] = "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ-_";
    std::string token;
    token.reserve(bytes.size() * 2U);
    for (const auto byte : bytes) {
        token.push_back(alphabet[(byte >> 6U) & 0x3fU]);
        token.push_back(alphabet[byte & 0x3fU]);
    }
    return token;
}

std::atomic<std::uint64_t> credential_sequence{};

bool write_credential_atomically(const std::filesystem::path& path, const std::string_view token,
                                 std::string& detail) {
    std::error_code error;
    if (!path.parent_path().empty()) {
        std::filesystem::create_directories(path.parent_path(), error);
        if (error) {
            detail = "Credential directory creation failed.";
            return false;
        }
    }
    const auto temporary = path.parent_path() / (path.filename().string() + ".noemancer-" +
        std::to_string(++credential_sequence) + ".tmp");
    {
        std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
        if (!stream) {
            detail = "Credential sidecar could not be opened.";
            return false;
        }
        stream << token << '\n';
        stream.flush();
        if (!stream) {
            detail = "Credential sidecar could not be written.";
            return false;
        }
    }
    const auto wide_source = utf8_to_wide(temporary.generic_string());
    const auto wide_target = utf8_to_wide(path.generic_string());
    if (wide_source.empty() || wide_target.empty() ||
        MoveFileExW(wide_source.c_str(), wide_target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == 0) {
        std::filesystem::remove(temporary, error);
        detail = "Credential sidecar atomic replacement failed.";
        return false;
    }
    return true;
}

struct ServerConnection final {
    HANDLE pipe{INVALID_HANDLE_VALUE};
    mutable std::mutex handle_mutex;
    std::string id;
    std::atomic<bool> stopping{};
    std::mutex outgoing_mutex;
    std::condition_variable outgoing_wake;
    std::deque<std::string> outgoing;
    std::thread reader;
    std::thread writer;
    bool authenticated{};
    std::unordered_set<std::string> pending_ids;

    [[nodiscard]] HANDLE current_handle() const {
        std::scoped_lock lock(handle_mutex);
        return pipe;
    }

    void close_handle() {
        std::scoped_lock lock(handle_mutex);
        cancel_and_close(pipe);
    }
};

struct PendingReply final {
    std::mutex mutex;
    std::condition_variable wake;
    bool done{};
    LiveEditorTransportReply reply;
};

#endif // _WIN32

} // namespace

#ifdef _WIN32

struct LiveEditorTransportServer::Impl final {
    struct QueuedRequest final {
        LiveEditorTransportRequest request;
        std::shared_ptr<ServerConnection> connection;
        std::chrono::steady_clock::time_point deadline;
    };

    LiveEditorTransportDescriptor descriptor;
    LiveEditorTransportLimits limits{};
    std::string token;
    std::wstring endpoint;
    std::atomic<bool> stopping{true};
    std::atomic<bool> running_flag{};
    std::thread accept_thread;
    mutable std::mutex state_mutex;
    mutable std::mutex accept_mutex;
    HANDLE accept_pipe{INVALID_HANDLE_VALUE};
    std::unordered_map<std::string, std::shared_ptr<ServerConnection>> connections;
    std::deque<QueuedRequest> requests;
    std::uint64_t next_connection_id{1U};
    std::uint64_t requests_received{};
    std::uint64_t responses_sent{};
    std::atomic<std::uint64_t> rejected_requests{};
    std::string code{"live-editor.transport-stopped"};
    std::string detail{"The live editor transport is stopped."};

    void accept_loop();
    void reader_loop(const std::shared_ptr<ServerConnection>& connection);
    void writer_loop(const std::shared_ptr<ServerConnection>& connection);
    void reap_stopped();
    bool enqueue_response(const std::shared_ptr<ServerConnection>& connection, std::string line);
    bool send_error(const std::shared_ptr<ServerConnection>& connection, std::string_view request_id,
                    std::string_view error_code, std::string_view error_detail);
    std::string response_line(std::string_view request_id, const LiveEditorTransportDispatchResult& result) const;
    std::string hello_line(bool accepted, std::string_view error_code = {}, std::string_view detail = {}) const;
};

struct LiveEditorTransportClient::Impl final {
    LiveEditorTransportDescriptor descriptor;
    LiveEditorTransportLimits limits{};
    std::string token;
    std::wstring endpoint;
    std::atomic<bool> stopping{true};
    std::atomic<bool> connected_flag{};
    mutable std::mutex state_mutex;
    std::mutex write_mutex;
    HANDLE pipe{INVALID_HANDLE_VALUE};
    std::thread reader;
    std::unordered_map<std::string, std::shared_ptr<PendingReply>> pending;
    std::uint64_t next_request_id{1U};
    std::uint64_t requests_sent{};
    std::uint64_t responses_received{};
    std::atomic<std::uint64_t> rejected_requests{};
    std::string code{"live-editor.transport-disconnected"};
    std::string detail{"The live editor transport is disconnected."};

    void reader_loop();
    void fail_pending(std::string_view error_code, std::string_view error_detail);
};

#else

struct LiveEditorTransportServer::Impl final {};
struct LiveEditorTransportClient::Impl final {};

#endif

std::string default_live_editor_endpoint(const std::string_view name) {
#ifdef _WIN32
    return endpoint_name(name);
#else
    static_cast<void>(name);
    return {};
#endif
}

LiveEditorCredentialReceipt create_live_editor_credential(const std::filesystem::path& path) {
#ifdef _WIN32
    LiveEditorCredentialReceipt result;
    result.path = path;
    if (path.empty() || path.filename().empty()) {
        result.code = "live-editor.credential-path-invalid";
        result.detail = "A credential sidecar path is required.";
        return result;
    }
    const auto token = random_credential_token();
    if (!valid_credential(token)) {
        result.code = "live-editor.credential-random-failed";
        result.detail = "The platform CSPRNG did not return a credential.";
        return result;
    }
    if (!write_credential_atomically(path, token, result.detail)) {
        result.code = "live-editor.credential-write-failed";
        return result;
    }
    result.success = true;
    result.code = "ok";
    result.token = token;
    result.detail = "Credential sidecar generated atomically.";
    return result;
#else
    static_cast<void>(path);
    return {false, "live-editor.transport-unavailable", "Named-pipe live editor transport is unavailable on this platform.", {}, {}};
#endif
}

#ifdef _WIN32

std::string LiveEditorTransportServer::Impl::hello_line(const bool accepted, const std::string_view error_code,
                                                        const std::string_view detail_text) const {
    Json value{{"type", "hello"}, {"protocol", live_editor_transport_protocol}, {"requestId", "hello"},
        {"ok", accepted}};
    if (!accepted) value["error"] = {{"code", error_code}, {"detail", detail_text}};
    return value.dump();
}

std::string LiveEditorTransportServer::Impl::response_line(
    const std::string_view request_id, const LiveEditorTransportDispatchResult& result) const {
    Json value{{"type", "response"}, {"protocol", live_editor_transport_protocol},
        {"requestId", request_id}, {"ok", result.success}};
    if (result.success) {
        const auto parsed = Json::parse(result.result_json.empty() ? "null" : result.result_json, nullptr, false);
        if (parsed.is_discarded()) {
            value["ok"] = false;
            value["error"] = {{"code", "live-editor.handler-invalid-json"},
                {"detail", "Dispatch returned invalid JSON."}};
        } else {
            value["result"] = parsed;
        }
    } else {
        value["error"] = {{"code", result.error_code.empty() ? "live-editor.request-failed" : result.error_code},
            {"detail", result.detail.empty() ? "The request was rejected." : result.detail}};
    }
    auto line = value.dump();
    if (line.size() <= limits.max_response_bytes) return line;
    return Json{{"type", "response"}, {"protocol", live_editor_transport_protocol},
        {"requestId", request_id}, {"ok", false},
        {"error", {{"code", "live-editor.response-too-large"},
            {"detail", "The response exceeded its hard byte limit."}}}}.dump();
}

bool LiveEditorTransportServer::Impl::enqueue_response(
    const std::shared_ptr<ServerConnection>& connection, std::string line) {
    if (!connection || connection->stopping.load() || line.empty() || line.size() > limits.max_response_bytes) return false;
    {
        std::scoped_lock lock(connection->outgoing_mutex);
        if (connection->stopping.load() || connection->outgoing.size() >= limits.max_queued_requests) {
            connection->stopping.store(true);
            ++rejected_requests;
            connection->outgoing.clear();
            connection->outgoing_wake.notify_all();
            return false;
        }
        connection->outgoing.push_back(std::move(line));
    }
    connection->outgoing_wake.notify_one();
    return true;
}

bool LiveEditorTransportServer::Impl::send_error(
    const std::shared_ptr<ServerConnection>& connection, const std::string_view request_id,
    const std::string_view error_code, const std::string_view error_detail) {
    LiveEditorTransportDispatchResult result;
    result.success = false;
    result.error_code = std::string(error_code);
    result.detail = std::string(error_detail);
    return enqueue_response(connection, response_line(request_id, result));
}

void LiveEditorTransportServer::Impl::writer_loop(const std::shared_ptr<ServerConnection>& connection) {
    for (;;) {
        std::string line;
        {
            std::unique_lock lock(connection->outgoing_mutex);
            connection->outgoing_wake.wait(lock, [&] {
                return connection->stopping.load() || !connection->outgoing.empty();
            });
            if (connection->outgoing.empty() && connection->stopping.load()) return;
            if (connection->outgoing.empty()) continue;
            line = std::move(connection->outgoing.front());
            connection->outgoing.pop_front();
        }
        const auto pipe = connection->current_handle();
        if (pipe == INVALID_HANDLE_VALUE || !write_line(pipe, line, limits.max_response_bytes)) {
            connection->stopping.store(true);
            connection->outgoing_wake.notify_all();
            return;
        }
        std::scoped_lock lock(state_mutex);
        ++responses_sent;
    }
}

void LiveEditorTransportServer::Impl::reader_loop(const std::shared_ptr<ServerConnection>& connection) {
    const auto pipe = connection->current_handle();
    if (pipe == INVALID_HANDLE_VALUE) {
        connection->stopping.store(true);
        return;
    }
    std::string line;
    const auto hello_read = read_line_timeout(pipe, limits.max_request_bytes,
        limits.handshake_timeout_milliseconds, line);
    if (hello_read != PipeReadResult::ok) {
        if (hello_read == PipeReadResult::too_large) send_error(connection, "hello", "live-editor.request-too-large", "The hello frame exceeded its hard byte limit.");
        connection->stopping.store(true);
        connection->outgoing_wake.notify_all();
        return;
    }
    const auto hello = Json::parse(line, nullptr, false);
    const bool hello_shape = hello.is_object() && json_string_member(hello, "type") == "hello" &&
        json_string_member(hello, "protocol") == live_editor_transport_protocol &&
        json_string_member(hello, "requestId") == "hello" &&
        constant_time_equal(json_string_member(hello, "token"), token);
    if (!hello_shape) {
        static_cast<void>(write_line(pipe,
            hello_line(false, "live-editor.auth-failed", "The hello token was rejected."),
            limits.max_response_bytes));
        connection->stopping.store(true);
        connection->outgoing_wake.notify_all();
        std::scoped_lock lock(state_mutex);
        ++rejected_requests;
        return;
    }
    {
        std::scoped_lock lock(state_mutex);
        connection->authenticated = true;
    }
    if (!write_line(pipe, hello_line(true), limits.max_response_bytes)) {
        const auto error = GetLastError();
        {
            std::scoped_lock lock(state_mutex);
            code = "live-editor.hello-write-failed";
            detail = "The authentication reply could not be written (Win32 error " +
                std::to_string(error) + ").";
        }
        connection->stopping.store(true);
        connection->outgoing_wake.notify_all();
        return;
    }
    while (!stopping.load() && !connection->stopping.load()) {
        line.clear();
        const auto read = read_line_until_stopped(pipe, limits.max_request_bytes, connection->stopping, line);
        if (read != PipeReadResult::ok) {
            if (read == PipeReadResult::too_large) send_error(connection, {}, "live-editor.request-too-large", "The invoke frame exceeded its hard byte limit.");
            break;
        }
        const auto value = Json::parse(line, nullptr, false);
        if (!value.is_object() || json_string_member(value, "type") != "invoke") {
            const auto request_id = json_string_member(value, "requestId");
            send_error(connection, request_id, "live-editor.protocol-error",
                "Only authenticated invoke frames are accepted.");
            break;
        }
        const auto request_id = json_string_member(value, "requestId");
        const auto method = json_string_member(value, "method");
        const auto arguments = value.contains("arguments") ? value.at("arguments") : Json::object();
        std::uint32_t requested_timeout = limits.request_timeout_milliseconds;
        if (value.contains("timeoutMs") && value.at("timeoutMs").is_number_unsigned()) {
            requested_timeout = static_cast<std::uint32_t>(std::min<std::uint64_t>(
                value.at("timeoutMs").get<std::uint64_t>(), limits.request_timeout_milliseconds));
        }
        auto request_lifetime = std::chrono::milliseconds(requested_timeout);
        if (value.contains("deadlineUnixMs") && value.at("deadlineUnixMs").is_number_integer()) {
            const auto deadline_unix_ms = value.at("deadlineUnixMs").get<std::int64_t>();
            const auto now_unix_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
            const auto maximum_lifetime = static_cast<std::int64_t>(limits.request_timeout_milliseconds);
            const auto maximum_deadline = now_unix_ms > std::numeric_limits<std::int64_t>::max() - maximum_lifetime
                ? std::numeric_limits<std::int64_t>::max() : now_unix_ms + maximum_lifetime;
            if (deadline_unix_ms <= now_unix_ms) {
                request_lifetime = std::chrono::milliseconds::zero();
            } else if (deadline_unix_ms < maximum_deadline) {
                request_lifetime = std::min(request_lifetime,
                    std::chrono::milliseconds(deadline_unix_ms - now_unix_ms));
            }
        }
        if (request_id.empty() || request_id.size() > maximum_identifier_bytes || method.empty() ||
            method.size() > maximum_identifier_bytes || arguments.is_discarded()) {
            send_error(connection, request_id, "live-editor.request-invalid", "Invoke identity or arguments are invalid.");
            continue;
        }
        bool accepted = false;
        std::string rejection_code;
        {
            std::scoped_lock lock(state_mutex);
            if (connection->pending_ids.contains(request_id)) {
                rejection_code = "live-editor.duplicate-request";
            } else if (requests.size() >= limits.max_queued_requests) {
                rejection_code = "live-editor.queue-full";
            } else if (connection->pending_ids.size() >= limits.max_pending_requests) {
                rejection_code = "live-editor.pending-limit";
            } else {
                connection->pending_ids.insert(request_id);
                requests.push_back({
                    LiveEditorTransportRequest{connection->id, request_id, method, arguments.dump()},
                    connection,
                    std::chrono::steady_clock::now() + request_lifetime});
                ++requests_received;
                accepted = true;
            }
        }
        if (!accepted) {
            std::scoped_lock lock(state_mutex);
            ++rejected_requests;
            send_error(connection, request_id, rejection_code, "The bounded transport queue rejected the invoke.");
        }
    }
    connection->stopping.store(true);
    connection->outgoing_wake.notify_all();
}

void LiveEditorTransportServer::Impl::reap_stopped() {
    std::vector<std::shared_ptr<ServerConnection>> stopped_connections;
    {
        std::scoped_lock lock(state_mutex);
        for (auto iterator = connections.begin(); iterator != connections.end();) {
            if (!iterator->second->stopping.load()) {
                ++iterator;
                continue;
            }
            stopped_connections.push_back(iterator->second);
            iterator = connections.erase(iterator);
        }
    }
    for (const auto& connection : stopped_connections) {
        connection->close_handle();
        connection->outgoing_wake.notify_all();
        if (connection->reader.joinable()) connection->reader.join();
        if (connection->writer.joinable()) connection->writer.join();
    }
}

void LiveEditorTransportServer::Impl::accept_loop() {
    while (!stopping.load()) {
        reap_stopped();
        bool at_connection_cap = false;
        {
            std::scoped_lock lock(state_mutex);
            at_connection_cap = connections.size() >= limits.max_connections;
        }
        if (at_connection_cap) {
            // A client that cannot obtain an instance will receive a bounded
            // WaitNamedPipe timeout; no unbounded connection is admitted
            // while the hard cap is reached.
            Sleep(2U);
            continue;
        }
        const auto pipe = create_server_pipe(endpoint, limits);
        if (pipe == INVALID_HANDLE_VALUE) {
            const auto error = GetLastError();
            std::scoped_lock lock(state_mutex);
            code = "live-editor.pipe-create-failed";
            detail = "The named pipe instance could not be created (Win32 error " +
                std::to_string(error) + ").";
            running_flag.store(false);
            break;
        }
        {
            std::scoped_lock lock(accept_mutex);
            accept_pipe = pipe;
        }
        const auto connected = ConnectNamedPipe(pipe, nullptr) != 0 || GetLastError() == ERROR_PIPE_CONNECTED;
        {
            std::scoped_lock lock(accept_mutex);
            if (accept_pipe == pipe) accept_pipe = INVALID_HANDLE_VALUE;
        }
        if (!connected) {
            HANDLE close_target = pipe;
            cancel_and_close(close_target);
            if (stopping.load()) break;
            continue;
        }
        if (stopping.load()) {
            HANDLE close_target = pipe;
            cancel_and_close(close_target);
            break;
        }
        auto connection = std::make_shared<ServerConnection>();
        connection->pipe = pipe;
        {
            std::scoped_lock lock(state_mutex);
            connection->id = "connection-" + std::to_string(next_connection_id++);
            connections.emplace(connection->id, connection);
        }
        connection->reader = std::thread([this, connection] { reader_loop(connection); });
        connection->writer = std::thread([this, connection] { writer_loop(connection); });
    }
    reap_stopped();
}

void LiveEditorTransportClient::Impl::fail_pending(
    const std::string_view error_code, const std::string_view error_detail) {
    std::vector<std::shared_ptr<PendingReply>> pending_replies;
    HANDLE pipe_to_close = INVALID_HANDLE_VALUE;
    {
        std::scoped_lock lock(state_mutex);
        for (auto& [request_id, pending_reply] : pending) {
            static_cast<void>(request_id);
            pending_replies.push_back(pending_reply);
        }
        pending.clear();
        connected_flag.store(false);
        stopping.store(true);
        pipe_to_close = pipe;
        pipe = INVALID_HANDLE_VALUE;
        code = std::string(error_code);
        detail = std::string(error_detail);
    }
    cancel_and_close(pipe_to_close);
    for (const auto& pending_reply : pending_replies) {
        {
            std::scoped_lock lock(pending_reply->mutex);
            pending_reply->done = true;
            pending_reply->reply.transport_ok = false;
            pending_reply->reply.received = false;
            pending_reply->reply.error_code = std::string(error_code);
            pending_reply->reply.detail = std::string(error_detail);
        }
        pending_reply->wake.notify_all();
    }
}

void LiveEditorTransportClient::Impl::reader_loop() {
    for (;;) {
        if (stopping.load()) return;
        std::string line;
        const auto current_pipe = [&] {
            std::scoped_lock lock(state_mutex);
            return pipe;
        }();
        if (current_pipe == INVALID_HANDLE_VALUE) return;
        const auto read = read_line_until_stopped(current_pipe, limits.max_response_bytes, stopping, line);
        if (read != PipeReadResult::ok) {
            fail_pending(read == PipeReadResult::too_large ? "live-editor.response-too-large" :
                "live-editor.disconnected", read == PipeReadResult::too_large
                ? "The response exceeded its hard byte limit." : "The named pipe was disconnected.");
            return;
        }
        const auto value = Json::parse(line, nullptr, false);
        if (!value.is_object() || json_string_member(value, "type") != "response" ||
            json_string_member(value, "protocol") != live_editor_transport_protocol) {
            fail_pending("live-editor.protocol-error", "The server returned an invalid response frame.");
            return;
        }
        const auto request_id = json_string_member(value, "requestId");
        std::shared_ptr<PendingReply> pending_reply;
        {
            std::scoped_lock lock(state_mutex);
            const auto iterator = pending.find(request_id);
            if (iterator == pending.end()) continue;
            pending_reply = iterator->second;
            pending.erase(iterator);
            ++responses_received;
        }
        LiveEditorTransportReply reply;
        reply.transport_ok = true;
        reply.received = true;
        reply.request_id = request_id;
        reply.success = json_bool_member(value, "ok");
        if (reply.success) {
            const auto result = value.contains("result") ? value.at("result") : Json(nullptr);
            reply.result_json = result.dump();
        } else {
            const auto error_iterator = value.find("error");
            const auto& error = error_iterator != value.end() && error_iterator->is_object()
                ? *error_iterator : Json::object();
            reply.error_code = json_string_member(error, "code", "live-editor.request-failed");
            reply.detail = json_string_member(error, "detail", "The server rejected the request.");
        }
        {
            std::scoped_lock lock(pending_reply->mutex);
            pending_reply->reply = std::move(reply);
            pending_reply->done = true;
        }
        pending_reply->wake.notify_all();
    }
}

#endif // _WIN32

LiveEditorTransportServer::LiveEditorTransportServer() : impl_(std::make_unique<Impl>()) {}

LiveEditorTransportServer::~LiveEditorTransportServer() {
    static_cast<void>(stop());
}

LiveEditorTransportOperation LiveEditorTransportServer::start(
    LiveEditorTransportDescriptor descriptor, LiveEditorTransportLimits limits) {
#ifdef _WIN32
    if (running()) return operation_failure("live-editor.already-running", "The live editor transport is already running.");
    if (!valid_endpoint(descriptor.endpoint))
        return operation_failure("live-editor.endpoint-invalid", "A \\\\.\\pipe\\ named-pipe endpoint is required.");
    const auto credential = read_credential(descriptor);
    if (!credential) return operation_failure("live-editor.credential-unavailable", "A valid credential sidecar or test token is required.");
    impl_->descriptor = std::move(descriptor);
    impl_->limits = normalize_limits(limits);
    impl_->endpoint = utf8_to_wide(impl_->descriptor.endpoint);
    impl_->token = *credential;
    if (impl_->endpoint.empty()) return operation_failure("live-editor.endpoint-invalid", "The named-pipe endpoint is not valid UTF-8.");
    impl_->stopping.store(false);
    impl_->running_flag.store(true);
    {
        std::scoped_lock lock(impl_->state_mutex);
        impl_->code = "ok";
        impl_->detail = "Named-pipe server is accepting authenticated connections.";
    }
    impl_->accept_thread = std::thread([impl = impl_.get()] { impl->accept_loop(); });
    return operation_ok("Named-pipe server started.");
#else
    static_cast<void>(descriptor);
    static_cast<void>(limits);
    return operation_failure("live-editor.transport-unavailable", "Named-pipe live editor transport is unavailable on this platform.");
#endif
}

LiveEditorTransportOperation LiveEditorTransportServer::stop() {
#ifdef _WIN32
    const auto was_running = impl_->running_flag.exchange(false);
    if (!was_running && !impl_->accept_thread.joinable())
        return operation_ok("Named-pipe server was already stopped.");
    impl_->stopping.store(true);
    {
        std::scoped_lock lock(impl_->accept_mutex);
        cancel_and_close(impl_->accept_pipe);
    }
    std::vector<std::shared_ptr<ServerConnection>> connections;
    {
        std::scoped_lock lock(impl_->state_mutex);
        impl_->requests.clear();
        for (auto& [id, connection] : impl_->connections) {
            static_cast<void>(id);
            connection->stopping.store(true);
            connections.push_back(connection);
        }
    }
    for (const auto& connection : connections) {
        connection->close_handle();
        connection->outgoing_wake.notify_all();
    }
    if (impl_->accept_thread.joinable()) impl_->accept_thread.join();
    for (const auto& connection : connections) {
        if (connection->reader.joinable()) connection->reader.join();
        if (connection->writer.joinable()) connection->writer.join();
    }
    {
        std::scoped_lock lock(impl_->state_mutex);
        impl_->connections.clear();
        impl_->code = "live-editor.transport-stopped";
        impl_->detail = "The named-pipe server is stopped.";
    }
    return operation_ok("Named-pipe server stopped without detached IO threads.");
#else
    return operation_failure("live-editor.transport-unavailable", "Named-pipe live editor transport is unavailable on this platform.");
#endif
}

bool LiveEditorTransportServer::running() const noexcept {
#ifdef _WIN32
    return impl_->running_flag.load();
#else
    return false;
#endif
}

std::size_t LiveEditorTransportServer::pump(
    const LiveEditorTransportDispatch& dispatch, const std::size_t requested_budget) {
#ifdef _WIN32
    if (!dispatch || !running()) return 0U;
    const auto budget = requested_budget == 0U ? impl_->limits.max_queued_requests :
        std::min(requested_budget, impl_->limits.max_queued_requests);
    std::size_t processed{};
    while (processed < budget) {
        Impl::QueuedRequest queued;
        {
            std::scoped_lock lock(impl_->state_mutex);
            if (impl_->requests.empty()) break;
            queued = std::move(impl_->requests.front());
            impl_->requests.pop_front();
        }
        if (queued.connection->stopping.load()) {
            std::scoped_lock lock(impl_->state_mutex);
            queued.connection->pending_ids.erase(queued.request.request_id);
            ++processed;
            continue;
        }
        LiveEditorTransportDispatchResult result;
        if (std::chrono::steady_clock::now() >= queued.deadline) {
            result.success = false;
            result.error_code = "live-editor.request-timeout";
            result.detail = "The request expired before the main-thread pump dispatched it.";
        } else {
            try {
                result = dispatch(queued.request);
            } catch (const std::exception&) {
                result.success = false;
                result.error_code = "live-editor.handler-exception";
                result.detail = "The main-thread dispatch callback threw an exception.";
            } catch (...) {
                result.success = false;
                result.error_code = "live-editor.handler-exception";
                result.detail = "The main-thread dispatch callback threw an unknown exception.";
            }
        }
        impl_->enqueue_response(queued.connection, impl_->response_line(queued.request.request_id, result));
        {
            std::scoped_lock lock(impl_->state_mutex);
            queued.connection->pending_ids.erase(queued.request.request_id);
        }
        ++processed;
    }
    return processed;
#else
    static_cast<void>(dispatch);
    static_cast<void>(requested_budget);
    return 0U;
#endif
}

LiveEditorTransportStatus LiveEditorTransportServer::status() const {
#ifdef _WIN32
    LiveEditorTransportStatus result;
    result.available = true;
    result.running = impl_->running_flag.load();
    result.endpoint = impl_->descriptor.endpoint;
    result.credential_file = impl_->descriptor.credential_file.generic_string();
    {
        std::scoped_lock lock(impl_->state_mutex);
        result.active_connections = impl_->connections.size();
        result.authenticated_connections = std::ranges::count_if(impl_->connections,
            [](const auto& item) { return item.second->authenticated; });
        result.queued_requests = impl_->requests.size();
        for (const auto& [id, connection] : impl_->connections) {
            static_cast<void>(id);
            result.pending_requests += connection->pending_ids.size();
        }
        result.requests_received = impl_->requests_received;
        result.responses_sent = impl_->responses_sent;
        result.rejected_requests = impl_->rejected_requests.load();
        result.code = impl_->code;
        result.detail = impl_->detail;
    }
    return result;
#else
    return {false, false, 0U, 0U, 0U, 0U, 0U, 0U, 0U, {}, {},
        "live-editor.transport-unavailable", "Named-pipe live editor transport is unavailable on this platform."};
#endif
}

std::string LiveEditorTransportServer::observe_json() const {
    const auto value = status();
    return Json{{"schemaVersion", "noemancer.live-editor-transport/0.1"}, {"role", "server"},
        {"available", value.available}, {"running", value.running}, {"endpoint", value.endpoint},
        {"credentialFile", value.credential_file}, {"activeConnections", value.active_connections},
        {"authenticatedConnections", value.authenticated_connections}, {"queuedRequests", value.queued_requests},
        {"pendingRequests", value.pending_requests}, {"requestsReceived", value.requests_received},
        {"responsesSent", value.responses_sent}, {"rejectedRequests", value.rejected_requests},
        {"code", value.code}, {"detail", value.detail}, {"tokenPresent", false}}.dump();
}

LiveEditorTransportClient::LiveEditorTransportClient() : impl_(std::make_unique<Impl>()) {}

LiveEditorTransportClient::~LiveEditorTransportClient() {
    static_cast<void>(disconnect());
}

LiveEditorTransportOperation LiveEditorTransportClient::connect(
    LiveEditorTransportDescriptor descriptor, LiveEditorTransportLimits limits) {
#ifdef _WIN32
    if (connected()) return operation_failure("live-editor.already-connected", "The live editor client is already connected.");
    if (!valid_endpoint(descriptor.endpoint))
        return operation_failure("live-editor.endpoint-invalid", "A \\\\.\\pipe\\ named-pipe endpoint is required.");
    const auto credential = read_credential(descriptor);
    if (!credential) return operation_failure("live-editor.credential-unavailable", "A valid credential sidecar or test token is required.");
    impl_->descriptor = std::move(descriptor);
    impl_->limits = normalize_limits(limits);
    impl_->endpoint = utf8_to_wide(impl_->descriptor.endpoint);
    impl_->token = *credential;
    if (impl_->endpoint.empty()) return operation_failure("live-editor.endpoint-invalid", "The named-pipe endpoint is not valid UTF-8.");
    const auto connect_deadline = std::chrono::steady_clock::now() +
        std::chrono::milliseconds(impl_->limits.handshake_timeout_milliseconds);
    bool pipe_ready = false;
    while (std::chrono::steady_clock::now() < connect_deadline) {
        const auto remaining = static_cast<DWORD>(std::min<std::int64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(connect_deadline - std::chrono::steady_clock::now()).count(),
            std::numeric_limits<DWORD>::max()));
        if (WaitNamedPipeW(impl_->endpoint.c_str(), std::max<DWORD>(remaining, 1U))) {
            pipe_ready = true;
            break;
        }
        const auto error = GetLastError();
        if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PIPE_BUSY) break;
        Sleep(1U);
    }
    if (!pipe_ready)
        return operation_failure("live-editor.connect-timeout", "No live editor server accepted the bounded connection window.");
    const auto pipe = CreateFileW(impl_->endpoint.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (pipe == INVALID_HANDLE_VALUE) return operation_failure("live-editor.connect-failed", "The named-pipe connection could not be opened.");
    DWORD mode = PIPE_READMODE_BYTE;
    if (!SetNamedPipeHandleState(pipe, &mode, nullptr, nullptr)) {
        HANDLE close_target = pipe;
        cancel_and_close(close_target);
        return operation_failure("live-editor.pipe-mode-failed", "The named-pipe byte mode could not be selected.");
    }
    impl_->pipe = pipe;
    const auto hello = Json{{"type", "hello"}, {"protocol", live_editor_transport_protocol},
        {"requestId", "hello"}, {"token", impl_->token}}.dump();
    if (!write_line(pipe, hello, impl_->limits.max_request_bytes)) {
        HANDLE close_target = impl_->pipe;
        cancel_and_close(close_target);
        impl_->pipe = INVALID_HANDLE_VALUE;
        return operation_failure("live-editor.hello-send-failed", "The authentication hello could not be sent.");
    }
    std::string line;
    if (read_line_timeout(pipe, impl_->limits.max_response_bytes, impl_->limits.handshake_timeout_milliseconds, line) != PipeReadResult::ok) {
        HANDLE close_target = impl_->pipe;
        cancel_and_close(close_target);
        impl_->pipe = INVALID_HANDLE_VALUE;
        return operation_failure("live-editor.hello-timeout", "The server did not complete authentication in time.");
    }
    const auto reply = Json::parse(line, nullptr, false);
    if (!reply.is_object() || json_string_member(reply, "type") != "hello" ||
        json_string_member(reply, "protocol") != live_editor_transport_protocol || !json_bool_member(reply, "ok")) {
        HANDLE close_target = impl_->pipe;
        cancel_and_close(close_target);
        impl_->pipe = INVALID_HANDLE_VALUE;
        return operation_failure("live-editor.auth-failed", "The server rejected the authentication hello.");
    }
    impl_->stopping.store(false);
    impl_->connected_flag.store(true);
    {
        std::scoped_lock lock(impl_->state_mutex);
        impl_->code = "ok";
        impl_->detail = "Authenticated named-pipe client is connected.";
    }
    impl_->reader = std::thread([impl = impl_.get()] { impl->reader_loop(); });
    return operation_ok("Authenticated named-pipe client connected.");
#else
    static_cast<void>(descriptor);
    static_cast<void>(limits);
    return operation_failure("live-editor.transport-unavailable", "Named-pipe live editor transport is unavailable on this platform.");
#endif
}

LiveEditorTransportOperation LiveEditorTransportClient::disconnect() {
#ifdef _WIN32
    if (!impl_->connected_flag.exchange(false) && !impl_->reader.joinable())
        return operation_ok("Named-pipe client was already disconnected.");
    impl_->stopping.store(true);
    HANDLE pipe = INVALID_HANDLE_VALUE;
    {
        std::scoped_lock lock(impl_->state_mutex);
        pipe = impl_->pipe;
        impl_->pipe = INVALID_HANDLE_VALUE;
    }
    cancel_and_close(pipe);
    if (impl_->reader.joinable()) impl_->reader.join();
    impl_->fail_pending("live-editor.disconnected", "The client closed the named-pipe session.");
    {
        std::scoped_lock lock(impl_->state_mutex);
        impl_->code = "live-editor.transport-disconnected";
        impl_->detail = "The named-pipe client is disconnected.";
    }
    return operation_ok("Named-pipe client disconnected without a detached reader thread.");
#else
    return operation_failure("live-editor.transport-unavailable", "Named-pipe live editor transport is unavailable on this platform.");
#endif
}

bool LiveEditorTransportClient::connected() const noexcept {
#ifdef _WIN32
    return impl_->connected_flag.load();
#else
    return false;
#endif
}

LiveEditorTransportReply LiveEditorTransportClient::request(
    const std::string_view method, const std::string_view arguments_json,
    const std::chrono::milliseconds requested_timeout) {
    LiveEditorTransportReply result;
    if (!connected()) {
        result.error_code = "live-editor.disconnected";
        result.detail = "The client is not connected.";
        return result;
    }
#ifdef _WIN32
    if (method.empty() || method.size() > maximum_identifier_bytes) {
        result.error_code = "live-editor.method-invalid";
        result.detail = "The method name is empty or exceeds its hard byte limit.";
        return result;
    }
    const auto argument_source = arguments_json.empty() ? std::string_view{"{}"} : arguments_json;
    const auto arguments = Json::parse(argument_source, nullptr, false);
    if (arguments.is_discarded()) {
        result.error_code = "live-editor.arguments-invalid";
        result.detail = "The invoke arguments are not valid JSON.";
        return result;
    }
    const auto timeout = requested_timeout <= std::chrono::milliseconds::zero()
        ? std::chrono::milliseconds(impl_->limits.request_timeout_milliseconds)
        : std::min(requested_timeout, std::chrono::milliseconds(impl_->limits.request_timeout_milliseconds));
    const auto pending_reply = std::make_shared<PendingReply>();
    std::string request_id;
    {
        std::scoped_lock lock(impl_->state_mutex);
        if (impl_->pending.size() >= impl_->limits.max_pending_requests) {
            ++impl_->rejected_requests;
            result.error_code = "live-editor.pending-limit";
            result.detail = "The bounded pending-request limit was reached.";
            return result;
        }
        request_id = "request-" + std::to_string(impl_->next_request_id++);
        impl_->pending.emplace(request_id, pending_reply);
        ++impl_->requests_sent;
    }
    const auto deadline_unix_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch() + timeout).count();
    const auto frame = Json{{"type", "invoke"}, {"protocol", live_editor_transport_protocol},
        {"requestId", request_id}, {"method", method}, {"arguments", arguments},
        {"timeoutMs", static_cast<std::uint32_t>(timeout.count())},
        {"deadlineUnixMs", deadline_unix_ms}}.dump();
    bool sent = false;
    {
        std::scoped_lock lock(impl_->write_mutex);
        const auto pipe = [&] {
            std::scoped_lock state_lock(impl_->state_mutex);
            return impl_->pipe;
        }();
        sent = pipe != INVALID_HANDLE_VALUE && write_line(pipe, frame, impl_->limits.max_request_bytes);
    }
    if (!sent) {
        std::scoped_lock lock(impl_->state_mutex);
        impl_->pending.erase(request_id);
        ++impl_->rejected_requests;
        result.error_code = "live-editor.send-failed";
        result.detail = "The invoke frame could not be sent.";
        return result;
    }
    std::unique_lock lock(pending_reply->mutex);
    if (!pending_reply->wake.wait_for(lock, timeout, [&] { return pending_reply->done; })) {
        {
            std::scoped_lock state_lock(impl_->state_mutex);
            impl_->pending.erase(request_id);
        }
        result.transport_ok = true;
        result.request_id = request_id;
        result.timed_out = true;
        result.error_code = "live-editor.request-timeout";
        result.detail = "The bounded response timeout elapsed before a reply arrived.";
        return result;
    }
    return pending_reply->reply;
#else
    static_cast<void>(method);
    static_cast<void>(arguments_json);
    static_cast<void>(requested_timeout);
    result.error_code = "live-editor.transport-unavailable";
    result.detail = "Named-pipe live editor transport is unavailable on this platform.";
    return result;
#endif
}

LiveEditorTransportStatus LiveEditorTransportClient::status() const {
#ifdef _WIN32
    LiveEditorTransportStatus result;
    result.available = true;
    result.running = impl_->connected_flag.load();
    result.endpoint = impl_->descriptor.endpoint;
    result.credential_file = impl_->descriptor.credential_file.generic_string();
    {
        std::scoped_lock lock(impl_->state_mutex);
        result.pending_requests = impl_->pending.size();
        result.requests_received = impl_->requests_sent;
        result.responses_sent = impl_->responses_received;
        result.rejected_requests = impl_->rejected_requests.load();
        result.code = impl_->code;
        result.detail = impl_->detail;
    }
    return result;
#else
    return {false, false, 0U, 0U, 0U, 0U, 0U, 0U, 0U, {}, {},
        "live-editor.transport-unavailable", "Named-pipe live editor transport is unavailable on this platform."};
#endif
}

std::string LiveEditorTransportClient::observe_json() const {
    const auto value = status();
    return Json{{"schemaVersion", "noemancer.live-editor-transport/0.1"}, {"role", "client"},
        {"available", value.available}, {"connected", value.running}, {"endpoint", value.endpoint},
        {"credentialFile", value.credential_file}, {"pendingRequests", value.pending_requests},
        {"requestsSent", value.requests_received}, {"responsesReceived", value.responses_sent},
        {"rejectedRequests", value.rejected_requests}, {"code", value.code}, {"detail", value.detail},
        {"tokenPresent", false}}.dump();
}

} // namespace noemancer
