#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

namespace noemancer {

inline constexpr std::string_view live_editor_transport_protocol =
    "noemancer.live-editor/0.1";

// The descriptor deliberately keeps the credential out of the endpoint
// identity. Production callers should provide credential_file, a private
// sidecar containing one token line. auth_token exists for in-process tests
// and is never included in observations, diagnostics or protocol replies.
struct LiveEditorTransportDescriptor final {
    std::string endpoint;
    std::filesystem::path credential_file;
    std::string auth_token;
};

struct LiveEditorTransportLimits final {
    std::size_t max_connections{4U};
    std::size_t max_request_bytes{64U * 1024U};
    std::size_t max_response_bytes{256U * 1024U};
    std::size_t max_queued_requests{128U};
    std::size_t max_pending_requests{128U};
    std::uint32_t handshake_timeout_milliseconds{1000U};
    std::uint32_t request_timeout_milliseconds{5000U};
};

struct LiveEditorTransportOperation final {
    bool success{};
    std::string code;
    std::string detail;
};

struct LiveEditorCredentialReceipt final {
    bool success{};
    std::string code;
    std::string detail;
    std::filesystem::path path;
    // The generated token is returned only to the caller that requested the
    // sidecar. It must not be logged or copied into an endpoint descriptor.
    std::string token;
};

struct LiveEditorTransportRequest final {
    std::string connection_id;
    std::string request_id;
    std::string method;
    // Canonical JSON text for the invoke.arguments value. It is kept as text
    // so World and CommandRegistry remain the owners of their JSON schemas.
    std::string arguments_json{"{}"};
};

struct LiveEditorTransportDispatchResult final {
    bool success{};
    std::string result_json{"null"};
    std::string error_code;
    std::string detail;
};

struct LiveEditorTransportReply final {
    bool transport_ok{};
    bool received{};
    bool success{};
    bool timed_out{};
    std::string request_id;
    std::string result_json{"null"};
    std::string error_code;
    std::string detail;
};

struct LiveEditorTransportStatus final {
    bool available{};
    bool running{};
    std::size_t active_connections{};
    std::size_t authenticated_connections{};
    std::size_t queued_requests{};
    std::size_t pending_requests{};
    std::uint64_t requests_received{};
    std::uint64_t responses_sent{};
    std::uint64_t rejected_requests{};
    std::string endpoint;
    std::string credential_file;
    std::string code;
    std::string detail;
};

using LiveEditorTransportDispatch = std::function<LiveEditorTransportDispatchResult(
    const LiveEditorTransportRequest&)>;

// Returns a bounded same-user named-pipe identity on Windows. The string is
// empty on non-Windows platforms, where this transport reports unavailable.
[[nodiscard]] std::string default_live_editor_endpoint(std::string_view name = "default");

// Generates a same-user credential sidecar using the platform CSPRNG and an
// atomic replacement. The token is intentionally absent from all observation
// and transport JSON. Non-Windows builds report unavailable.
[[nodiscard]] LiveEditorCredentialReceipt create_live_editor_credential(
    const std::filesystem::path& path);

class LiveEditorTransportServer final {
public:
    LiveEditorTransportServer();
    ~LiveEditorTransportServer();
    LiveEditorTransportServer(const LiveEditorTransportServer&) = delete;
    LiveEditorTransportServer& operator=(const LiveEditorTransportServer&) = delete;

    [[nodiscard]] LiveEditorTransportOperation start(
        LiveEditorTransportDescriptor descriptor,
        LiveEditorTransportLimits limits = {});
    [[nodiscard]] LiveEditorTransportOperation stop();
    [[nodiscard]] bool running() const noexcept;

    // Must be called from the World/CommandRegistry owner thread. The
    // transport IO threads only enqueue requests and write completed replies;
    // dispatch is never invoked from an IO thread.
    [[nodiscard]] std::size_t pump(
        const LiveEditorTransportDispatch& dispatch,
        std::size_t budget = 0U);

    [[nodiscard]] LiveEditorTransportStatus status() const;
    [[nodiscard]] std::string observe_json() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

class LiveEditorTransportClient final {
public:
    LiveEditorTransportClient();
    ~LiveEditorTransportClient();
    LiveEditorTransportClient(const LiveEditorTransportClient&) = delete;
    LiveEditorTransportClient& operator=(const LiveEditorTransportClient&) = delete;

    [[nodiscard]] LiveEditorTransportOperation connect(
        LiveEditorTransportDescriptor descriptor,
        LiveEditorTransportLimits limits = {});
    [[nodiscard]] LiveEditorTransportOperation disconnect();
    [[nodiscard]] bool connected() const noexcept;

    [[nodiscard]] LiveEditorTransportReply request(
        std::string_view method,
        std::string_view arguments_json = "{}",
        std::chrono::milliseconds timeout = std::chrono::milliseconds::zero());

    [[nodiscard]] LiveEditorTransportStatus status() const;
    [[nodiscard]] std::string observe_json() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace noemancer
