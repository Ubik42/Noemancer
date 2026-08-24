#include "engine/project_ui_authoring_commands.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <initializer_list>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>

namespace noemancer {
namespace {

using Json = nlohmann::json;

struct CommandIssue final {
    std::string code;
    std::string detail;
};

bool one_of(const std::string_view value,
            const std::initializer_list<std::string_view> candidates) {
    return std::ranges::find(candidates, value) != candidates.end();
}

std::string canonical_operation(const std::string_view operation) {
    if (one_of(operation, {"observe", "ui.observe", "project.ui.observe"})) return "observe";
    if (one_of(operation, {"add", "add-node", "ui.add-node", "project.ui.add-node"})) return "add";
    if (one_of(operation, {"update", "update-node", "ui.update-node", "project.ui.update-node"})) return "update";
    if (one_of(operation, {"remove", "remove-node", "remove-subtree", "ui.remove-node", "project.ui.remove-node"})) return "remove";
    if (one_of(operation, {"reparent", "reparent-node", "ui.reparent", "project.ui.reparent"})) return "reparent";
    if (one_of(operation, {"reorder", "reorder-node", "ui.reorder", "project.ui.reorder"})) return "reorder";
    if (one_of(operation, {"design-tokens", "update-design-tokens", "ui.design-tokens", "project.ui.design-tokens"})) return "design-tokens";
    if (one_of(operation, {"undo", "ui.undo", "project.ui.undo"})) return "undo";
    if (one_of(operation, {"redo", "ui.redo", "project.ui.redo"})) return "redo";

    // Declaration commands remain distinct from node editing: declaration
    // data is stored in the authority's components[] collection and is never
    // silently translated into an expanded node tree.
    if (one_of(operation, {"add-reusable", "add-reusable-declaration", "add-declaration", "add-component-declaration", "reusable.add",
                           "ui.reusable.add", "project.ui.reusable.add"})) return "add-reusable";
    if (one_of(operation, {"update-reusable", "update-reusable-declaration", "update-declaration", "update-component-declaration", "reusable.update",
                           "ui.reusable.update", "project.ui.reusable.update"})) return "update-reusable";
    if (one_of(operation, {"remove-reusable", "remove-reusable-declaration", "remove-declaration", "remove-component-declaration", "reusable.remove",
                           "ui.reusable.remove", "project.ui.reusable.remove"})) return "remove-reusable";
    return {};
}

std::string failure_json(const ProjectUiAuthoringSession& session,
                         std::string operation, std::string code, std::string detail) {
    ProjectUiEditReceipt receipt;
    receipt.success = false;
    receipt.changed = false;
    receipt.persisted = false;
    receipt.operation = std::move(operation);
    receipt.code = std::move(code);
    receipt.detail = std::move(detail);
    receipt.revision = session.revision();
    receipt.fingerprint = session.fingerprint();
    receipt.document_json = session.source_json();
    receipt.observation_json = session.observation_json();
    receipt.can_undo = session.can_undo();
    receipt.can_redo = session.can_redo();
    return receipt.to_json();
}

std::optional<std::string> read_string(const Json& root, const std::string_view name,
                                       const bool required, const bool allow_empty,
                                       CommandIssue& issue) {
    const auto key = std::string(name);
    if (!root.contains(key)) {
        if (!required) return std::nullopt;
        issue = {"ui.command-field-required", "The command field '" + key + "' is required."};
        return std::nullopt;
    }
    const auto& value = root.at(key);
    if (!value.is_string()) {
        issue = {"ui.command-invalid-field", "The command field '" + key + "' must be a string."};
        return std::nullopt;
    }
    const auto result = value.get<std::string>();
    if (result.size() > project_ui_authoring_command_max_string_bytes) {
        issue = {"ui.command-string-too-large", "The command field '" + key + "' is too large."};
        return std::nullopt;
    }
    if (!allow_empty && result.empty()) {
        issue = {"ui.command-invalid-field", "The command field '" + key + "' must not be empty."};
        return std::nullopt;
    }
    return result;
}

std::optional<std::string> read_first_string(const Json& root,
                                             const std::initializer_list<std::string_view> names,
                                             const bool required, const bool allow_empty,
                                             CommandIssue& issue) {
    std::string_view selected;
    for (const auto name : names) {
        if (!root.contains(std::string(name))) continue;
        if (!selected.empty()) {
            issue = {"ui.command-duplicate-field", "Provide only one alias for the command identity field."};
            return std::nullopt;
        }
        selected = name;
    }
    if (selected.empty()) {
        if (required) issue = {"ui.command-field-required", "The command identity field is required."};
        return std::nullopt;
    }
    return read_string(root, selected, true, allow_empty, issue);
}

std::optional<std::uint64_t> read_index(const Json& root, const std::string_view name,
                                        const bool required, CommandIssue& issue) {
    const auto key = std::string(name);
    if (!root.contains(key)) {
        if (!required) return std::nullopt;
        issue = {"ui.command-field-required", "The command field '" + key + "' is required."};
        return std::nullopt;
    }
    const auto& value = root.at(key);
    if (!value.is_number_unsigned()) {
        issue = {"ui.command-invalid-field", "The command field '" + key + "' must be an unsigned integer."};
        return std::nullopt;
    }
    const auto result = value.get<std::uint64_t>();
    if (result > 4096U) {
        issue = {"ui.command-limit-exceeded", "The command field '" + key + "' exceeds the bounded sibling limit."};
        return std::nullopt;
    }
    return result;
}

std::optional<bool> read_bool(const Json& root, const std::string_view name,
                              const bool default_value, CommandIssue& issue) {
    const auto key = std::string(name);
    if (!root.contains(key)) return default_value;
    const auto& value = root.at(key);
    if (!value.is_boolean()) {
        issue = {"ui.command-invalid-field", "The command field '" + key + "' must be boolean."};
        return std::nullopt;
    }
    return value.get<bool>();
}

std::optional<std::string> read_json_value(const Json& root,
                                           const std::string_view direct_name,
                                           const std::string_view json_name,
                                           const bool required, CommandIssue& issue) {
    const auto direct_key = std::string(direct_name);
    const auto json_key = std::string(json_name);
    const bool has_direct = root.contains(direct_key);
    const bool has_json = root.contains(json_key);
    if (!has_direct && !has_json) {
        if (required) issue = {"ui.command-field-required", "The command field '" + direct_key + "' is required."};
        return std::nullopt;
    }
    if (has_direct && has_json) {
        issue = {"ui.command-duplicate-field", "Use either '" + direct_key + "' or '" + json_key + "', not both."};
        return std::nullopt;
    }
    const auto& value = root.at(has_direct ? direct_key : json_key);
    std::string result;
    if (!has_direct && value.is_string()) {
        result = value.get<std::string>();
        const auto parsed = Json::parse(result, nullptr, false);
        if (parsed.is_discarded()) {
            issue = {"ui.command-invalid-json", "The command JSON field '" + json_key + "' is invalid."};
            return std::nullopt;
        }
    } else {
        result = value.dump();
    }
    if (result.size() > project_ui_authoring_command_max_json_value_bytes) {
        issue = {"ui.command-json-too-large", "The command JSON value exceeds the bounded field limit."};
        return std::nullopt;
    }
    return result;
}

std::optional<std::string> read_first_json_value(
    const Json& root,
    const std::initializer_list<std::pair<std::string_view, std::string_view>> fields,
    const bool required, CommandIssue& issue) {
    std::pair<std::string_view, std::string_view> selected{};
    bool found = false;
    for (const auto field : fields) {
        if (!root.contains(std::string(field.first)) && !root.contains(std::string(field.second))) continue;
        if (found) {
            issue = {"ui.command-duplicate-field", "Provide only one alias for the command JSON field."};
            return std::nullopt;
        }
        found = true;
        selected = field;
    }
    if (!found) {
        if (required) issue = {"ui.command-field-required", "The command JSON field is required."};
        return std::nullopt;
    }
    return read_json_value(root, selected.first, selected.second, true, issue);
}

std::optional<std::string> read_component_ref(const Json& root, CommandIssue& issue) {
    const auto direct_key = std::string("componentRef");
    const auto json_key = std::string("componentRefJson");
    const bool has_direct = root.contains(direct_key);
    const bool has_json = root.contains(json_key);
    if (!has_direct && !has_json) return std::nullopt;
    if (has_direct && has_json) {
        issue = {"ui.command-duplicate-field", "Use either 'componentRef' or 'componentRefJson', not both."};
        return std::nullopt;
    }
    const auto& value = root.at(has_direct ? direct_key : json_key);
    if (value.is_null()) return std::string("null");
    if (value.is_string()) {
        if (has_direct) {
            const auto result = value.get<std::string>();
            if (result.empty() || result.size() > project_ui_authoring_command_max_string_bytes) {
                issue = {"ui.command-invalid-field", "componentRef must be a bounded non-empty string or null."};
                return std::nullopt;
            }
            return result;
        }
        const auto parsed = Json::parse(value.get<std::string>(), nullptr, false);
        if (parsed.is_discarded() || (!parsed.is_null() && !parsed.is_string())) {
            issue = {"ui.command-invalid-json", "componentRefJson must contain a JSON string or null."};
            return std::nullopt;
        }
        if (parsed.is_null()) return std::string("null");
        const auto result = parsed.get<std::string>();
        if (result.empty() || result.size() > project_ui_authoring_command_max_string_bytes) {
            issue = {"ui.command-invalid-field", "componentRef must be a bounded non-empty string or null."};
            return std::nullopt;
        }
        return result;
    }
    issue = {"ui.command-invalid-field", "componentRef must be a string or null."};
    return std::nullopt;
}

std::optional<ProjectUiEditOptions> read_options(const Json& root, CommandIssue& issue) {
    if (!root.contains("baseRevision")) {
        issue = {"ui.base-revision-required", "Every Project UI mutation must provide baseRevision."};
        return std::nullopt;
    }
    if (!root.at("baseRevision").is_number_unsigned() || root.at("baseRevision").get<std::uint64_t>() == 0U) {
        issue = {"ui.invalid-base-revision", "baseRevision must be a positive unsigned integer."};
        return std::nullopt;
    }
    const auto dry_run = read_bool(root, "dryRun", false, issue);
    if (!dry_run) return std::nullopt;
    return ProjectUiEditOptions{root.at("baseRevision").get<std::uint64_t>(), *dry_run};
}

} // namespace

ProjectUiAuthoringCommandService::ProjectUiAuthoringCommandService(ProjectUiAuthoringSession& session) noexcept
    : session_(&session) {}

std::string ProjectUiAuthoringCommandService::observe_json() const {
    return session_->observation_json();
}

std::string ProjectUiAuthoringCommandService::dispatch_json(const std::string_view request_json) {
    if (request_json.size() > project_ui_authoring_command_max_bytes)
        return failure_json(*session_, "command", "ui.command-too-large", "The Project UI command exceeds the bounded request limit.");

    const auto root = Json::parse(request_json, nullptr, false);
    if (root.is_discarded() || !root.is_object())
        return failure_json(*session_, "command", "ui.invalid-command", "The Project UI command must be a JSON object.");

    CommandIssue issue;
    const auto raw_operation = read_string(root, "operation", true, false, issue);
    if (!raw_operation) return failure_json(*session_, "command", issue.code, issue.detail);
    const auto operation = canonical_operation(*raw_operation);
    if (operation.empty())
        return failure_json(*session_, *raw_operation, "ui.unknown-operation", "The Project UI command operation is not supported.");
    if (operation == "observe") return session_->observation_json();

    const auto options = read_options(root, issue);
    if (!options) return failure_json(*session_, operation, issue.code, issue.detail);

    try {
        if (operation == "add") {
            ProjectUiAddNodeRequest request;
            const auto id = read_string(root, "id", true, false, issue);
            const auto parent_id = read_string(root, "parentId", false, true, issue);
            const auto role = read_string(root, "role", true, false, issue);
            const auto label = read_string(root, "label", false, true, issue);
            if (!id || !role) return failure_json(*session_, operation, issue.code, issue.detail);
            request.id = *id; request.parent_id = parent_id.value_or(""); request.role = *role; request.label = label.value_or("");
            if (const auto index = read_index(root, "siblingIndex", false, issue); issue.code.empty() && index)
                request.sibling_index = static_cast<std::size_t>(*index);
            else if (!issue.code.empty()) return failure_json(*session_, operation, issue.code, issue.detail);
            request.binding_json = read_json_value(root, "binding", "bindingJson", false, issue);
            if (!issue.code.empty()) return failure_json(*session_, operation, issue.code, issue.detail);
            request.actions_json = read_json_value(root, "actions", "actionsJson", false, issue);
            if (!issue.code.empty()) return failure_json(*session_, operation, issue.code, issue.detail);
            request.state_json = read_json_value(root, "state", "stateJson", false, issue);
            if (!issue.code.empty()) return failure_json(*session_, operation, issue.code, issue.detail);
            request.presentation_json = read_json_value(root, "presentation", "presentationJson", false, issue);
            if (!issue.code.empty()) return failure_json(*session_, operation, issue.code, issue.detail);
            request.value_json = read_json_value(root, "value", "valueJson", false, issue);
            if (!issue.code.empty()) return failure_json(*session_, operation, issue.code, issue.detail);
            request.component_ref = read_component_ref(root, issue);
            if (!issue.code.empty()) return failure_json(*session_, operation, issue.code, issue.detail);
            return session_->add_node(std::move(request), *options).to_json();
        }
        if (operation == "update") {
            ProjectUiUpdateNodeRequest request;
            const auto id = read_string(root, "nodeId", true, false, issue);
            if (!id) return failure_json(*session_, operation, issue.code, issue.detail);
            request.node_id = *id;
            request.label = read_string(root, "label", false, true, issue);
            if (!issue.code.empty()) return failure_json(*session_, operation, issue.code, issue.detail);
            request.role = read_string(root, "role", false, false, issue);
            if (!issue.code.empty()) return failure_json(*session_, operation, issue.code, issue.detail);
            request.binding_json = read_json_value(root, "binding", "bindingJson", false, issue);
            if (!issue.code.empty()) return failure_json(*session_, operation, issue.code, issue.detail);
            request.actions_json = read_json_value(root, "actions", "actionsJson", false, issue);
            if (!issue.code.empty()) return failure_json(*session_, operation, issue.code, issue.detail);
            request.state_json = read_json_value(root, "state", "stateJson", false, issue);
            if (!issue.code.empty()) return failure_json(*session_, operation, issue.code, issue.detail);
            request.presentation_json = read_json_value(root, "presentation", "presentationJson", false, issue);
            if (!issue.code.empty()) return failure_json(*session_, operation, issue.code, issue.detail);
            request.value_json = read_json_value(root, "value", "valueJson", false, issue);
            if (!issue.code.empty()) return failure_json(*session_, operation, issue.code, issue.detail);
            request.component_ref = read_component_ref(root, issue);
            if (!issue.code.empty()) return failure_json(*session_, operation, issue.code, issue.detail);
            return session_->update_node(std::move(request), *options).to_json();
        }
        if (operation == "remove") {
            const auto id = read_string(root, "nodeId", true, false, issue);
            if (!id) return failure_json(*session_, operation, issue.code, issue.detail);
            return session_->remove_subtree(*id, *options).to_json();
        }
        if (operation == "reparent") {
            const auto id = read_string(root, "nodeId", true, false, issue);
            const auto parent = read_string(root, "parentId", true, true, issue);
            if (!id || !parent) return failure_json(*session_, operation, issue.code, issue.detail);
            const auto index = read_index(root, "siblingIndex", false, issue);
            if (!issue.code.empty()) return failure_json(*session_, operation, issue.code, issue.detail);
            if (index) return session_->reparent(*id, *parent, static_cast<std::size_t>(*index), *options).to_json();
            return session_->reparent(*id, *parent, *options).to_json();
        }
        if (operation == "reorder") {
            const auto id = read_string(root, "nodeId", true, false, issue);
            const auto index = read_index(root, "siblingIndex", true, issue);
            if (!id || !index) return failure_json(*session_, operation, issue.code, issue.detail);
            return session_->reorder(*id, static_cast<std::size_t>(*index), *options).to_json();
        }
        if (operation == "design-tokens") {
            const auto tokens = read_json_value(root, "designTokens", "designTokensJson", true, issue);
            if (!tokens) return failure_json(*session_, operation, issue.code, issue.detail);
            return session_->update_design_tokens(*tokens, *options).to_json();
        }
        if (operation == "undo") return session_->undo(*options).to_json();
        if (operation == "redo") return session_->redo(*options).to_json();

        if (operation == "add-reusable") {
            const auto id = read_first_string(root, {"id", "componentId", "declarationId"}, true, false, issue);
            const auto declaration = read_first_json_value(root,
                {{"declaration", "declarationJson"}, {"componentDeclaration", "componentDeclarationJson"},
                 {"component", "componentJson"}}, true, issue);
            if (!id || !declaration) return failure_json(*session_, operation, issue.code, issue.detail);
            return session_->add_declaration(ProjectUiAddDeclarationRequest{*id, *declaration}, *options).to_json();
        }
        if (operation == "update-reusable") {
            const auto id = read_first_string(root, {"id", "componentId", "declarationId"}, true, false, issue);
            const auto declaration = read_first_json_value(root,
                {{"declaration", "declarationJson"}, {"componentDeclaration", "componentDeclarationJson"},
                 {"component", "componentJson"}}, true, issue);
            if (!id || !declaration) return failure_json(*session_, operation, issue.code, issue.detail);
            return session_->update_declaration(*id, *declaration, *options).to_json();
        }
        if (operation == "remove-reusable") {
            const auto id = read_first_string(root, {"id", "componentId", "declarationId"}, true, false, issue);
            if (!id) return failure_json(*session_, operation, issue.code, issue.detail);
            return session_->remove_declaration(*id, *options).to_json();
        }

        return failure_json(*session_, operation, "ui.command-failed", "The Project UI command could not be dispatched.");
    } catch (const std::exception&) {
        return failure_json(*session_, operation, "ui.command-failed", "The Project UI command failed at the authoring authority boundary.");
    }
}

} // namespace noemancer
