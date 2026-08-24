#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace noemancer {

struct DapDecodeBatch final {
    std::vector<std::string> messages;
    std::optional<std::string> error;
};

class DapStreamDecoder final {
public:
    static constexpr std::size_t maximum_header_bytes=8192;
    static constexpr std::size_t maximum_message_bytes=1024*1024;

    [[nodiscard]] DapDecodeBatch feed(std::string_view bytes);
    [[nodiscard]] bool failed() const noexcept { return failed_; }
    void reset() noexcept;

private:
    std::string buffer_;
    std::optional<std::size_t> expected_body_bytes_;
    bool failed_{};
};

class DapObservationReducer final {
public:
    [[nodiscard]] bool ingest(std::string_view message_json);
    [[nodiscard]] std::string snapshot_json() const;
private:
    struct Thread final {std::uint64_t id{};std::string name;};
    struct Frame final {std::uint64_t id{};std::string name;std::string source;std::uint32_t line{};std::uint32_t column{};};
    struct Scope final {std::string name;std::uint64_t variables_reference{};bool expensive{};};
    struct Variable final {std::string name;std::string value;std::string type;std::string evaluate_name;std::uint64_t variables_reference{};};
    std::string state_{"created"};
    std::string stop_reason_;
    std::string last_error_;
    std::uint64_t thread_id_{};
    std::uint64_t message_count_{};
    bool all_threads_stopped_{};
    bool frames_truncated_{};
    bool variables_truncated_{};
    std::vector<Thread> threads_;
    std::vector<Frame> frames_;
    std::vector<Scope> scopes_;
    std::vector<Variable> variables_;
};

[[nodiscard]] std::string dap_request_frame(std::uint64_t sequence,std::string_view command,
                                            std::string_view arguments_json="{}");
[[nodiscard]] std::string dap_message_frame(std::string_view message_json);

} // namespace noemancer
