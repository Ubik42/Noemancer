#include "runtime/live_editor_transport.hpp"

#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <string>
#include <thread>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

namespace {

using Json = nlohmann::json;
using namespace std::chrono_literals;

std::filesystem::path credential_path() {
    return std::filesystem::temp_directory_path() /
        ("noemancer-live-editor-credential-" +
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".token");
}

std::string endpoint_name() {
    return noemancer::default_live_editor_endpoint(
        "transport-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
}

#ifdef _WIN32

std::wstring endpoint_wide(const std::string& endpoint) {
    return {endpoint.begin(), endpoint.end()};
}

bool raw_write_line(const HANDLE pipe, const std::string& line) {
    const auto framed = line + '\n';
    DWORD written{};
    return WriteFile(pipe, framed.data(), static_cast<DWORD>(framed.size()), &written, nullptr) != 0 &&
        written == framed.size();
}

bool raw_read_line(const HANDLE pipe, const std::chrono::milliseconds timeout, std::string& line) {
    line.clear();
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    for (;;) {
        DWORD available{};
        if (!PeekNamedPipe(pipe, nullptr, 0U, nullptr, &available, nullptr)) return false;
        if (available != 0U) {
            char character{};
            DWORD read_bytes{};
            if (!ReadFile(pipe, &character, 1U, &read_bytes, nullptr) || read_bytes == 0U) return false;
            if (character == '\n') return true;
            if (character != '\r') line.push_back(character);
            if (line.size() >= 64U * 1024U) return false;
        } else {
            if (std::chrono::steady_clock::now() >= deadline) return false;
            Sleep(1U);
        }
    }
}

#endif

bool wait_for_reply(std::future<noemancer::LiveEditorTransportReply>& future,
                    noemancer::LiveEditorTransportServer& server,
                    const noemancer::LiveEditorTransportDispatch& dispatch,
                    const std::chrono::milliseconds timeout,
                    noemancer::LiveEditorTransportReply& reply) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (future.wait_for(1ms) != std::future_status::ready && std::chrono::steady_clock::now() < deadline)
        static_cast<void>(server.pump(dispatch, 8U));
    if (future.wait_for(0ms) != std::future_status::ready) return false;
    reply = future.get();
    return true;
}

} // namespace

int main() {
#ifndef _WIN32
    const auto descriptor = noemancer::LiveEditorTransportDescriptor{};
    noemancer::LiveEditorTransportServer server;
    const auto server_result = server.start(descriptor);
    const auto credential = noemancer::create_live_editor_credential({});
    if (server_result.success || server_result.code != "live-editor.transport-unavailable" ||
        credential.success || credential.code != "live-editor.transport-unavailable") {
        std::cerr << "Non-Windows live editor transport did not fail closed as unavailable.\n";
        return 1;
    }
    noemancer::LiveEditorTransportClient client;
    const auto client_result = client.connect(descriptor);
    if (client_result.success || client_result.code != "live-editor.transport-unavailable" ||
        client.connected()) {
        std::cerr << "Non-Windows live editor client did not report unavailable.\n";
        return 2;
    }
    return 0;
#else
    const auto path = credential_path();
    const auto credential = noemancer::create_live_editor_credential(path);
    if (!credential.success || credential.token.empty() || credential.token.size() > 4096U ||
        !std::filesystem::is_regular_file(path)) {
        std::cerr << "Credential sidecar generation failed.\n";
        return 1;
    }
    {
        std::ifstream stream(path, std::ios::binary);
        const std::string sidecar{std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>{}};
        if (sidecar != credential.token + "\n") {
            std::cerr << "Credential sidecar was not a single atomic token line.\n";
            std::filesystem::remove(path);
            return 2;
        }
    }

    const auto endpoint = endpoint_name();
    noemancer::LiveEditorTransportDescriptor descriptor;
    descriptor.endpoint = endpoint;
    descriptor.credential_file = path;
    noemancer::LiveEditorTransportLimits limits;
    limits.max_connections = 4U;
    limits.max_queued_requests = 8U;
    limits.max_pending_requests = 8U;
    limits.handshake_timeout_milliseconds = 3000U;
    limits.request_timeout_milliseconds = 500U;

    noemancer::LiveEditorTransportServer server;
    const auto started = server.start(descriptor, limits);
    if (!started.success || !server.running()) {
        std::cerr << "Named-pipe server did not start: " << started.code << '\n';
        std::filesystem::remove(path);
        return 3;
    }
    const auto server_observation = Json::parse(server.observe_json());
    if (server_observation.at("tokenPresent") != false ||
        server_observation.dump().find(credential.token) != std::string::npos) {
        std::cerr << "Server observation leaked the credential token.\n";
        static_cast<void>(server.stop());
        std::filesystem::remove(path);
        return 4;
    }

    noemancer::LiveEditorTransportDescriptor wrong_descriptor = descriptor;
    wrong_descriptor.credential_file.clear();
    wrong_descriptor.auth_token = "deliberately-wrong-token";
    noemancer::LiveEditorTransportClient wrong_client;
    const auto wrong_result = wrong_client.connect(wrong_descriptor, limits);
    if (wrong_result.success || wrong_client.connected()) {
        std::cerr << "Invalid hello token was accepted.\n";
        static_cast<void>(wrong_client.disconnect());
        static_cast<void>(server.stop());
        std::filesystem::remove(path);
        return 5;
    }

    noemancer::LiveEditorTransportClient client;
    const auto connected = client.connect(descriptor, limits);
    if (!connected.success || !client.connected()) {
        std::cerr << "Authenticated named-pipe client did not connect: " << connected.code
                  << " (" << connected.detail << ") server=" << server.observe_json() << '\n';
        static_cast<void>(server.stop());
        std::filesystem::remove(path);
        return 6;
    }

    const auto owner_thread = std::this_thread::get_id();
    std::atomic<bool> callback_on_owner{true};
    std::atomic<unsigned> callback_count{};
    const auto dispatch = [&](const noemancer::LiveEditorTransportRequest& request) {
        if (std::this_thread::get_id() != owner_thread) callback_on_owner.store(false);
        ++callback_count;
        if (request.method == "world.observe")
            return noemancer::LiveEditorTransportDispatchResult{true, R"({"worldRevision":7,"entityCount":2})", {}, {}};
        return noemancer::LiveEditorTransportDispatchResult{false, "null", "command.unknown", "Unknown test command."};
    };

    auto request_future = std::async(std::launch::async, [&client] {
        return client.request("world.observe", R"({"scope":"editor"})", 2s);
    });
    noemancer::LiveEditorTransportReply reply;
    if (!wait_for_reply(request_future, server, dispatch, 2s, reply) || !reply.transport_ok ||
        !reply.received || !reply.success || reply.request_id.empty() ||
        Json::parse(reply.result_json).at("worldRevision") != 7 || !callback_on_owner.load() || callback_count != 1U) {
        std::cerr << "Main-thread dispatch or requestId correlation failed.\n";
        static_cast<void>(client.disconnect());
        static_cast<void>(server.stop());
        std::filesystem::remove(path);
        return 7;
    }

    const auto raw_pipe = CreateFileW(endpoint_wide(endpoint).c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
        OPEN_EXISTING, 0, nullptr);
    if (raw_pipe == INVALID_HANDLE_VALUE) {
        std::cerr << "Malformed-frame regression client could not open the named pipe.\n";
        static_cast<void>(client.disconnect());
        static_cast<void>(server.stop());
        std::filesystem::remove(path);
        return 8;
    }
    DWORD raw_mode = PIPE_READMODE_BYTE;
    std::string raw_line;
    const auto raw_hello = Json{{"type", "hello"}, {"protocol", noemancer::live_editor_transport_protocol},
        {"requestId", "hello"}, {"token", credential.token}}.dump();
    const auto raw_hello_ok = SetNamedPipeHandleState(raw_pipe, &raw_mode, nullptr, nullptr) != 0 &&
        raw_write_line(raw_pipe, raw_hello) && raw_read_line(raw_pipe, 2s, raw_line) &&
        Json::parse(raw_line).value("ok", false);
    const auto raw_bad_identity = Json{{"type", "invoke"}, {"protocol", noemancer::live_editor_transport_protocol},
        {"requestId", 42}, {"method", "world.observe"}, {"arguments", Json::object()}}.dump();
    const auto raw_bad_method = Json{{"type", "invoke"}, {"protocol", noemancer::live_editor_transport_protocol},
        {"requestId", "raw-invalid-method"}, {"method", Json::array()}, {"arguments", Json::object()}}.dump();
    const auto raw_bad_frames_ok = raw_hello_ok && raw_write_line(raw_pipe, raw_bad_identity) &&
        raw_read_line(raw_pipe, 2s, raw_line) && raw_write_line(raw_pipe, raw_bad_method) &&
        raw_read_line(raw_pipe, 2s, raw_line);
    CloseHandle(raw_pipe);
    if (!raw_bad_frames_ok || callback_count != 1U) {
        std::cerr << "Malformed JSONL field types were not rejected without dispatch termination.\n";
        static_cast<void>(client.disconnect());
        static_cast<void>(server.stop());
        std::filesystem::remove(path);
        return 9;
    }

    const auto malformed_arguments = client.request("world.observe", "{", 100ms);
    if (malformed_arguments.transport_ok || malformed_arguments.error_code != "live-editor.arguments-invalid" ||
        callback_count != 1U) {
        std::cerr << "Malformed JSON arguments were not rejected before transport dispatch.\n";
        static_cast<void>(client.disconnect());
        static_cast<void>(server.stop());
        std::filesystem::remove(path);
        return 10;
    }
    const auto malformed_method = client.request({}, "{}", 100ms);
    if (malformed_method.transport_ok || malformed_method.error_code != "live-editor.method-invalid" ||
        callback_count != 1U) {
        std::cerr << "Malformed method identity was not rejected before transport dispatch.\n";
        static_cast<void>(client.disconnect());
        static_cast<void>(server.stop());
        std::filesystem::remove(path);
        return 11;
    }
    const auto oversized_arguments = client.request(
        "world.observe", "\"" + std::string(limits.max_request_bytes, 'x') + "\"", 100ms);
    if (oversized_arguments.transport_ok || oversized_arguments.error_code != "live-editor.send-failed" ||
        callback_count != 1U) {
        std::cerr << "Oversized JSONL frame was not rejected at the client byte bound.\n";
        static_cast<void>(client.disconnect());
        static_cast<void>(server.stop());
        std::filesystem::remove(path);
        return 12;
    }

    auto timeout_future = std::async(std::launch::async, [&client] {
        return client.request("world.never", "{}", 60ms);
    });
    const auto timeout_reply = timeout_future.get();
    if (!timeout_reply.transport_ok || !timeout_reply.timed_out ||
        timeout_reply.error_code != "live-editor.request-timeout") {
        std::cerr << "Bounded client request timeout was not reported.\n";
        static_cast<void>(client.disconnect());
        static_cast<void>(server.stop());
        std::filesystem::remove(path);
        return 13;
    }
    // The expired item is consumed by the main-thread pump without invoking
    // the callback. This proves timeout handling remains outside IO threads.
    static_cast<void>(server.pump(dispatch, 8U));
    if (callback_count != 1U) {
        std::cerr << "Expired request reached the main-thread callback.\n";
        static_cast<void>(client.disconnect());
        static_cast<void>(server.stop());
        std::filesystem::remove(path);
        return 14;
    }

    const auto client_observation = Json::parse(client.observe_json());
    if (client_observation.at("tokenPresent") != false ||
        client_observation.dump().find(credential.token) != std::string::npos ||
        client_observation.at("connected") != true) {
        std::cerr << "Client observation was not bounded or leaked the token.\n";
        static_cast<void>(client.disconnect());
        static_cast<void>(server.stop());
        std::filesystem::remove(path);
        return 15;
    }

    const auto disconnected = client.disconnect();
    const auto stopped = server.stop();
    std::filesystem::remove(path);
    if (!disconnected.success || !stopped.success || client.connected() || server.running()) {
        std::cerr << "Transport shutdown did not join all IO threads.\n";
        return 16;
    }
    std::cout << "Live editor named-pipe transport authenticated, correlated requestId, pumped on the owner thread, bounded timeout, sidecar credential and clean shutdown.\n";
    return 0;
#endif
}
