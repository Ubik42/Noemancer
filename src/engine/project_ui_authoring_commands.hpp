#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "engine/project_ui_authoring.hpp"

namespace noemancer {

inline constexpr std::string_view project_ui_authoring_command_schema =
    "noemancer.project-ui-authoring-command/0.1";

// The adapter is deliberately smaller than the source-authoring limit.  A
// command is one bounded intent, not a transport for an entire project.
inline constexpr std::size_t project_ui_authoring_command_max_bytes = 256U * 1024U;
inline constexpr std::size_t project_ui_authoring_command_max_string_bytes = 4096U;
inline constexpr std::size_t project_ui_authoring_command_max_json_value_bytes = 64U * 1024U;

// A thin command boundary over a live ProjectUiAuthoringSession.  It owns no
// document, history or projection state: all observations and mutations are
// delegated to the attached authority, which remains the sole source of
// truth for source JSON, revisions and receipts.
class ProjectUiAuthoringCommandService final {
public:
    explicit ProjectUiAuthoringCommandService(ProjectUiAuthoringSession& session) noexcept;

    [[nodiscard]] std::string observe_json() const;
    [[nodiscard]] std::string dispatch_json(std::string_view request_json);

    // Naming aliases make the same plain-JSON adapter convenient for CLI,
    // MCP and managed bridges without introducing protocol-specific state.
    [[nodiscard]] std::string execute_json(std::string_view request_json) {
        return dispatch_json(request_json);
    }
    [[nodiscard]] std::string invoke_json(std::string_view request_json) {
        return dispatch_json(request_json);
    }

    [[nodiscard]] ProjectUiAuthoringSession& session() noexcept { return *session_; }
    [[nodiscard]] const ProjectUiAuthoringSession& session() const noexcept { return *session_; }

private:
    ProjectUiAuthoringSession* session_{};
};

using ProjectUiAuthoringCommandAdapter = ProjectUiAuthoringCommandService;

} // namespace noemancer
