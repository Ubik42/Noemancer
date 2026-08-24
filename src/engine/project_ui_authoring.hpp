#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace noemancer {

inline constexpr std::string_view project_ui_authoring_schema =
    "noemancer.project-ui-authoring/0.1";

enum class ProjectUiDiagnosticSeverity : std::uint8_t {
    error,
    warning
};

struct ProjectUiDiagnostic final {
    ProjectUiDiagnosticSeverity severity{ProjectUiDiagnosticSeverity::error};
    std::string code;
    std::string path;
    std::string message;
};

struct ProjectUiEditOptions final {
    std::optional<std::uint64_t> expected_revision;
    bool dry_run{};
};

// JSON values intentionally remain strings at this public boundary.  This
// keeps the authoring ABI plain-data and makes it possible for CLI/MCP,
// managed code and the GUI to share the exact same request shape without
// leaking nlohmann::json or another parser type into the engine contract.
struct ProjectUiAddNodeRequest final {
    std::string id;
    std::string parent_id;
    std::string role;
    std::string label;
    std::optional<std::size_t> sibling_index;
    std::optional<std::string> binding_json;
    std::optional<std::string> actions_json;
    std::optional<std::string> state_json;
    std::optional<std::string> presentation_json;
    std::optional<std::string> value_json;
};

struct ProjectUiUpdateNodeRequest final {
    std::string node_id;
    std::optional<std::string> label;
    std::optional<std::string> role;
    // A JSON value of "null" removes the field.  An object is required for
    // binding, state and presentation; actions must be a JSON array.
    std::optional<std::string> binding_json;
    std::optional<std::string> actions_json;
    std::optional<std::string> state_json;
    std::optional<std::string> presentation_json;
    std::optional<std::string> value_json;
};

struct ProjectUiEditReceipt final {
    bool success{};
    bool changed{};
    bool persisted{};
    std::string operation;
    std::string code;
    std::string detail;
    std::uint64_t revision{};
    std::string fingerprint;
    std::string candidate_fingerprint;
    std::string document_json;
    std::string observation_json;
    bool can_undo{};
    bool can_redo{};
    std::vector<ProjectUiDiagnostic> diagnostics;

    [[nodiscard]] explicit operator bool() const noexcept { return success; }
    [[nodiscard]] std::string to_json() const;
};

struct ProjectUiLoadResult final {
    bool success{};
    std::string code;
    std::string detail;
    std::filesystem::path document_path;
    std::uint64_t revision{};
    std::string fingerprint;
    std::vector<ProjectUiDiagnostic> diagnostics;

    [[nodiscard]] explicit operator bool() const noexcept { return success; }
};

// Engine-owned authoring authority for a source noemancer.ui-document/0.1.
//
// The session keeps the source document as plain JSON data, while the Runtime
// may project that source into a different document containing values,
// fingerprints and layout evidence.  Such runtime-derived fields are rejected
// at this boundary so an observation can never accidentally be written back
// as authored UI.  All successful mutations validate a candidate first, then
// atomically persist it, and publish the in-memory revision only after the
// filesystem commit succeeds.
class ProjectUiAuthoringSession final {
public:
    explicit ProjectUiAuthoringSession(std::string source_json = {},
                                       std::filesystem::path document_path = {},
                                       std::uint64_t revision = 1U);

    [[nodiscard]] static ProjectUiAuthoringSession from_file(
        std::filesystem::path document_path, std::uint64_t revision = 1U);
    [[nodiscard]] static ProjectUiAuthoringSession from_json(
        std::string source_json, std::filesystem::path document_path = {},
        std::uint64_t revision = 1U);

    [[nodiscard]] bool valid() const noexcept { return diagnostics_.empty(); }
    [[nodiscard]] const std::filesystem::path& document_path() const noexcept {
        return document_path_;
    }
    [[nodiscard]] std::uint64_t revision() const noexcept { return revision_; }
    [[nodiscard]] const std::string& fingerprint() const noexcept { return fingerprint_; }
    [[nodiscard]] const std::string& source_json() const noexcept { return source_json_; }
    [[nodiscard]] bool can_undo() const noexcept { return !undo_.empty(); }
    [[nodiscard]] bool can_redo() const noexcept { return !redo_.empty(); }
    [[nodiscard]] std::vector<ProjectUiDiagnostic> validate() const;
    [[nodiscard]] std::string observation_json() const;

    [[nodiscard]] ProjectUiEditReceipt add_node(
        ProjectUiAddNodeRequest request, ProjectUiEditOptions options = {});
    [[nodiscard]] ProjectUiEditReceipt remove_subtree(
        std::string_view node_id, ProjectUiEditOptions options = {});
    [[nodiscard]] ProjectUiEditReceipt update_node(
        ProjectUiUpdateNodeRequest request, ProjectUiEditOptions options = {});
    [[nodiscard]] ProjectUiEditReceipt reparent(
        std::string_view node_id, std::string_view parent_id,
        ProjectUiEditOptions options = {});
    [[nodiscard]] ProjectUiEditReceipt reparent(
        std::string_view node_id, std::string_view parent_id, std::size_t sibling_index,
        ProjectUiEditOptions options = {});
    [[nodiscard]] ProjectUiEditReceipt reorder(
        std::string_view node_id, std::size_t sibling_index,
        ProjectUiEditOptions options = {});
    [[nodiscard]] ProjectUiEditReceipt update_design_tokens(
        std::string design_tokens_json, ProjectUiEditOptions options = {});
    [[nodiscard]] ProjectUiEditReceipt undo(ProjectUiEditOptions options = {});
    [[nodiscard]] ProjectUiEditReceipt redo(ProjectUiEditOptions options = {});

private:
    struct HistoryEntry final {
        std::string before;
        std::string after;
    };

    enum class HistoryDirection : std::uint8_t {
        edit,
        undo,
        redo
    };

    [[nodiscard]] ProjectUiEditReceipt commit_candidate(
        std::string candidate_json, const ProjectUiEditOptions& options,
        std::string_view operation, HistoryDirection direction,
        std::optional<HistoryEntry> history_entry = {});
    [[nodiscard]] ProjectUiEditReceipt failure(
        std::string_view operation, std::string_view code, std::string_view detail,
        std::vector<ProjectUiDiagnostic> diagnostics = {}) const;
    [[nodiscard]] ProjectUiEditReceipt success(
        std::string_view operation, std::string_view code, std::string_view detail,
        bool changed, bool persisted, std::uint64_t revision,
        std::string candidate_json, std::string candidate_fingerprint) const;
    [[nodiscard]] ProjectUiEditReceipt invalid_request(
        std::string_view operation, std::string_view code, std::string_view detail,
        std::vector<ProjectUiDiagnostic> diagnostics) const;

    std::string source_json_;
    std::filesystem::path document_path_;
    std::uint64_t revision_{1U};
    std::string fingerprint_;
    std::vector<ProjectUiDiagnostic> diagnostics_;
    std::vector<HistoryEntry> undo_;
    std::vector<HistoryEntry> redo_;
};

using ProjectUiEditSession = ProjectUiAuthoringSession;

} // namespace noemancer
