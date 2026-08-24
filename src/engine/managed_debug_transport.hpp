#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace noemancer {

struct DapProcessExchange final {
    bool started{};
    bool timed_out{};
    int exit_code{-1};
    std::string stdout_bytes;
    std::string stderr_text;
    std::string error;
};

[[nodiscard]] DapProcessExchange run_dap_process_exchange(
    const std::filesystem::path& executable,const std::vector<std::string>& arguments,
    const std::string& stdin_bytes,std::chrono::milliseconds timeout);

enum class ManagedDebugSessionState : std::uint8_t {
    created,
    starting,
    ready,
    running,
    paused,
    terminating,
    terminated,
    failed
};

enum class DapSessionMessageKind : std::uint8_t {
    response,
    event,
    request
};

// A backend-neutral decoded DAP message. JSON is deliberately kept as text at
// this boundary so the managed transport does not impose a JSON object ABI on
// engine callers. `message_json` is the complete framed payload without the
// Content-Length header.
struct DapSessionMessage final {
    DapSessionMessageKind kind{DapSessionMessageKind::event};
    std::uint64_t sequence{};
    std::uint64_t request_sequence{};
    std::string command;
    std::string event;
    std::string body_json{"{}"};
    std::string message_json;
};

struct DapSessionReply final {
    bool sent{};
    bool received{};
    bool success{};
    bool timed_out{};
    bool process_exited{};
    std::uint64_t sequence{};
    int exit_code{-1};
    std::string command;
    std::string body_json{"{}"};
    std::string message;
    std::string response_json;
    // Stable machine-readable errors, for example dap.request-timeout,
    // dap.process-eof, dap.process-nonzero and dap.protocol-error.
    std::string error;
};

class ManagedDebugSession final {
public:
    ManagedDebugSession();
    ~ManagedDebugSession();
    ManagedDebugSession(const ManagedDebugSession&) = delete;
    ManagedDebugSession& operator=(const ManagedDebugSession&) = delete;

    [[nodiscard]] bool start(const std::filesystem::path& executable,
                             const std::vector<std::string>& arguments = {},
                             std::chrono::milliseconds startup_timeout = std::chrono::seconds(5));
    void shutdown(std::chrono::milliseconds timeout = std::chrono::seconds(2));

    [[nodiscard]] bool active() const noexcept;
    [[nodiscard]] ManagedDebugSessionState state() const noexcept;
    [[nodiscard]] std::string state_json() const;
    [[nodiscard]] std::string last_error() const;
    [[nodiscard]] int exit_code() const noexcept;

    [[nodiscard]] DapSessionReply request(std::string_view command,
                                          std::string_view arguments_json = "{}",
                                          std::chrono::milliseconds timeout = std::chrono::milliseconds::zero());
    [[nodiscard]] DapSessionReply initialize(
        std::string_view arguments_json = R"({"clientID":"noemancer","adapterID":"noemancer","pathFormat":"path"})",
        std::chrono::milliseconds timeout = std::chrono::milliseconds::zero());
    [[nodiscard]] DapSessionReply launch(std::string_view arguments_json = "{}",
                                         std::chrono::milliseconds timeout = std::chrono::milliseconds::zero());
    [[nodiscard]] DapSessionReply configuration_done(
        std::chrono::milliseconds timeout = std::chrono::milliseconds::zero());
    [[nodiscard]] DapSessionReply set_breakpoints(
        std::string_view arguments_json,
        std::chrono::milliseconds timeout = std::chrono::milliseconds::zero());
    [[nodiscard]] DapSessionReply continue_execution(
        std::uint64_t thread_id,
        std::chrono::milliseconds timeout = std::chrono::milliseconds::zero());
    [[nodiscard]] DapSessionReply pause(
        std::uint64_t thread_id,
        std::chrono::milliseconds timeout = std::chrono::milliseconds::zero());
    [[nodiscard]] DapSessionReply threads(
        std::chrono::milliseconds timeout = std::chrono::milliseconds::zero());
    [[nodiscard]] DapSessionReply stack_trace(
        std::uint64_t thread_id,
        std::chrono::milliseconds timeout = std::chrono::milliseconds::zero());
    [[nodiscard]] DapSessionReply terminate(
        std::chrono::milliseconds timeout = std::chrono::milliseconds::zero());

    // Events are delivered in arrival order. Responses are returned by
    // request() and are not duplicated in this queue.
    [[nodiscard]] std::vector<DapSessionMessage> drain_events();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

[[nodiscard]] const char* managed_debug_session_state_name(ManagedDebugSessionState state) noexcept;

} // namespace noemancer
