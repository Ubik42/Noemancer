#pragma once

#include "engine/gameplay_runtime.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace noemancer {

inline constexpr std::string_view project_input_authoring_schema =
    "noemancer.project-input-authoring/0.1";

enum class ProjectInputDiagnosticSeverity : std::uint8_t {
    error,
    warning
};

struct ProjectInputDiagnostic final {
    ProjectInputDiagnosticSeverity severity{ProjectInputDiagnosticSeverity::error};
    std::string code;
    std::string path;
    std::string message;
};

struct ProjectInputEditOptions final {
    std::optional<std::uint64_t> expected_revision;
    bool dry_run{};
};

struct ProjectInputEditResult final {
    bool success{};
    bool changed{};
    std::string code;
    std::string detail;
    std::uint64_t revision{};
    std::vector<ProjectInputDiagnostic> diagnostics;

    [[nodiscard]] explicit operator bool() const noexcept { return success; }
};

// A mutation is deliberately expressed only in engine-owned value types. The
// add kind accepts either an action or a binding; remove removes an action when
// source is empty and a binding otherwise; remap always replaces one binding.
enum class ProjectInputMutationKind : std::uint8_t {
    add,
    remove,
    remap,
    add_action = add,
    remove_action = remove,
    remap_binding = remap
};

struct ProjectInputEditRequest final {
    ProjectInputMutationKind kind{ProjectInputMutationKind::add};
    std::optional<std::uint64_t> expected_revision;
    bool dry_run{};
    std::string action_id;
    std::string source;
    std::optional<InputActionDefinition> action;
    std::optional<InputBinding> binding;
    std::optional<InputBinding> replacement;

    [[nodiscard]] static ProjectInputEditRequest add_action(
        std::string id, InputActionKind kind, std::vector<InputBinding> bindings = {},
        ProjectInputEditOptions options = {}) {
        ProjectInputEditRequest request;
        request.kind = ProjectInputMutationKind::add;
        request.action_id = id;
        request.action = InputActionDefinition{std::move(id), kind, std::move(bindings)};
        request.expected_revision = options.expected_revision;
        request.dry_run = options.dry_run;
        return request;
    }

    [[nodiscard]] static ProjectInputEditRequest add_binding(
        std::string action_id, InputBinding value, ProjectInputEditOptions options = {}) {
        ProjectInputEditRequest request;
        request.kind = ProjectInputMutationKind::add;
        request.action_id = std::move(action_id);
        request.binding = std::move(value);
        request.expected_revision = options.expected_revision;
        request.dry_run = options.dry_run;
        return request;
    }

    [[nodiscard]] static ProjectInputEditRequest remove_action(
        std::string action_id, ProjectInputEditOptions options = {}) {
        ProjectInputEditRequest request;
        request.kind = ProjectInputMutationKind::remove;
        request.action_id = std::move(action_id);
        request.expected_revision = options.expected_revision;
        request.dry_run = options.dry_run;
        return request;
    }

    [[nodiscard]] static ProjectInputEditRequest remove_binding(
        std::string action_id, std::string source, ProjectInputEditOptions options = {}) {
        ProjectInputEditRequest request;
        request.kind = ProjectInputMutationKind::remove;
        request.action_id = std::move(action_id);
        request.source = std::move(source);
        request.expected_revision = options.expected_revision;
        request.dry_run = options.dry_run;
        return request;
    }

    [[nodiscard]] static ProjectInputEditRequest remap_binding(
        std::string action_id, std::string source, InputBinding value,
        ProjectInputEditOptions options = {}) {
        ProjectInputEditRequest request;
        request.kind = ProjectInputMutationKind::remap;
        request.action_id = std::move(action_id);
        request.source = std::move(source);
        request.replacement = std::move(value);
        request.expected_revision = options.expected_revision;
        request.dry_run = options.dry_run;
        return request;
    }
};

struct ProjectInputEditReceipt final {
    bool success{};
    bool changed{};
    bool persisted{};
    std::string code;
    std::string detail;
    std::uint64_t revision{};
    std::vector<InputActionDefinition> actions;
    std::vector<ProjectInputDiagnostic> diagnostics;

    [[nodiscard]] explicit operator bool() const noexcept { return success; }
    [[nodiscard]] const std::vector<InputActionDefinition>& canonical_actions() const noexcept {
        return actions;
    }
};

// Engine-owned authoring authority for a project's logical input action map.
// Action IDs and binding sources are stable project identifiers; SDL device
// handles and platform event types never cross this boundary.
class ProjectInputAuthoring final {
public:
    explicit ProjectInputAuthoring(
        std::vector<InputActionDefinition> definitions = default_input_action_definitions());

    [[nodiscard]] const std::vector<InputActionDefinition>& actions() const noexcept {
        return actions_;
    }
    [[nodiscard]] std::uint64_t revision() const noexcept { return revision_; }

    [[nodiscard]] std::vector<ProjectInputDiagnostic> validate() const;

    [[nodiscard]] ProjectInputEditResult add_action(
        std::string id, InputActionKind kind, std::vector<InputBinding> bindings = {},
        ProjectInputEditOptions options = {});
    [[nodiscard]] ProjectInputEditResult remove_action(
        std::string_view id, ProjectInputEditOptions options = {});
    [[nodiscard]] ProjectInputEditResult add_binding(
        std::string_view action_id, InputBinding binding, ProjectInputEditOptions options = {});
    [[nodiscard]] ProjectInputEditResult remove_binding(
        std::string_view action_id, std::string_view source, ProjectInputEditOptions options = {});
    [[nodiscard]] ProjectInputEditResult remap_binding(
        std::string_view action_id, std::string_view source, InputBinding replacement,
        ProjectInputEditOptions options = {});

    // This is the exact array accepted by ProjectDocument's inputActions
    // field. The object form includes a stable schema and revision for
    // detached authoring/observation consumers.
    [[nodiscard]] std::string serialize_input_actions_json() const;
    [[nodiscard]] std::string serialize_json() const;

    // Replaces only inputActions in an existing noemancer.project/0.1 or
    // noemancer.project/0.2 manifest. The manifest is written beside the
    // source and committed with an atomic replacement; the original schema,
    // Hybrid Pixel profile and unrelated project fields are preserved.
    [[nodiscard]] ProjectInputEditResult save_project_manifest(
        const std::filesystem::path& manifest_path,
        ProjectInputEditOptions options = {}) const;

private:
    friend class ProjectInputEditSession;

    std::vector<InputActionDefinition> actions_;
    std::uint64_t revision_{1};
};

// A transaction boundary around ProjectInputAuthoring. It owns the active
// manifest path, builds a candidate copy for every request, persists that copy
// atomically, and only then publishes the new in-memory revision. This keeps
// Runtime project switching and hot-apply independent of Editor and SDL.
class ProjectInputEditSession final {
public:
    explicit ProjectInputEditSession(
        std::vector<InputActionDefinition> definitions = default_input_action_definitions(),
        std::filesystem::path manifest_path = {});
    explicit ProjectInputEditSession(ProjectInputAuthoring authoring,
                                     std::filesystem::path manifest_path = {});

    [[nodiscard]] static ProjectInputEditSession load(
        std::vector<InputActionDefinition> definitions, std::filesystem::path manifest_path) {
        return ProjectInputEditSession(std::move(definitions), std::move(manifest_path));
    }
    [[nodiscard]] static ProjectInputEditSession load(
        ProjectInputAuthoring authoring, std::filesystem::path manifest_path) {
        return ProjectInputEditSession(std::move(authoring), std::move(manifest_path));
    }

    [[nodiscard]] const std::vector<InputActionDefinition>& actions() const noexcept {
        return authoring_.actions();
    }
    [[nodiscard]] std::uint64_t revision() const noexcept { return authoring_.revision(); }
    [[nodiscard]] const std::filesystem::path& manifest_path() const noexcept {
        return manifest_path_;
    }
    [[nodiscard]] std::vector<ProjectInputDiagnostic> validate() const {
        return authoring_.validate();
    }

    [[nodiscard]] ProjectInputEditReceipt apply(const ProjectInputEditRequest& request);

    // Replace the active definition set through the same CAS/dry-run,
    // validate, atomic-save and publish boundary as a mutation request.
    [[nodiscard]] ProjectInputEditReceipt replace(
        std::vector<InputActionDefinition> definitions,
        ProjectInputEditOptions options = {});

    // Reload is an in-memory project switch/hot-apply operation. It validates
    // first and publishes the new path, definitions and source revision only
    // after validation succeeds; it never writes the old or new manifest.
    [[nodiscard]] ProjectInputEditReceipt reload(
        std::vector<InputActionDefinition> definitions,
        std::filesystem::path manifest_path = {}, std::uint64_t source_revision = 1U);

private:
    [[nodiscard]] ProjectInputEditReceipt receipt_from_result(
        const ProjectInputEditResult& result,
        std::vector<InputActionDefinition> actions = {}) const;
    [[nodiscard]] ProjectInputEditReceipt apply_candidate(
        std::vector<InputActionDefinition> candidate,
        const ProjectInputEditOptions& options);
    [[nodiscard]] ProjectInputEditReceipt publish_reload(
        std::vector<InputActionDefinition> definitions,
        std::filesystem::path manifest_path, std::uint64_t source_revision);

    ProjectInputAuthoring authoring_;
    std::filesystem::path manifest_path_;
};

} // namespace noemancer
