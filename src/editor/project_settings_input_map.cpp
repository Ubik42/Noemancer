#include "editor/project_settings_input_map.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <map>
#include <sstream>
#include <string>
#include <utility>

namespace noemancer {
namespace {

using Json = nlohmann::json;

constexpr std::size_t maximum_input_actions = 64U;
constexpr std::size_t maximum_input_bindings = 16U;

bool safe_input_identifier(const std::string_view value) {
    if (value.empty() || value.size() > 96U) return false;
    for (const auto character : value) {
        const auto byte = static_cast<unsigned char>(character);
        if (!std::isalnum(byte) && character != '.' && character != '-' && character != '_') return false;
    }
    return true;
}

bool known_action_kind(const InputActionKind kind) {
    return kind == InputActionKind::button || kind == InputActionKind::axis_1d;
}

std::string stable_component(const std::string_view value) {
    std::string result;
    result.reserve(value.size() + 4U);
    constexpr char hex[] = "0123456789ABCDEF";
    for (const auto character : value) {
        const auto byte = static_cast<unsigned char>(character);
        if (std::isalnum(byte) || character == '.' || character == '-' || character == '_') {
            result.push_back(character);
        } else {
            result.push_back('_');
            result.push_back(hex[(byte >> 4U) & 0x0FU]);
            result.push_back(hex[byte & 0x0FU]);
        }
    }
    if (result.empty()) result = "empty";
    return result;
}

std::string fingerprint(const Json& value) {
    std::uint64_t hash = 14695981039346656037ULL;
    for (const auto byte : value.dump()) {
        hash ^= static_cast<unsigned char>(byte);
        hash *= 1099511628211ULL;
    }
    std::ostringstream output;
    output << "fnv1a64:" << std::hex << std::setfill('0') << std::setw(16) << hash;
    return output.str();
}

Json make_node(const std::string_view id, const std::string_view parent_id,
               const std::string_view role, const std::string_view label, const bool editable) {
    Json node{{"id", std::string(id)},
              {"parentId", parent_id.empty() ? Json(nullptr) : Json(std::string(parent_id))},
              {"role", std::string(role)}, {"label", std::string(label)},
              {"state", {{"visible", true}, {"enabled", true}, {"editable", editable}}},
              {"actions", Json::array()}};
    return node;
}

Json diagnostic_json(const InputMapConflictDiagnostic& diagnostic) {
    Json value{{"id", diagnostic.id}, {"code", diagnostic.code}, {"severity", diagnostic.severity},
               {"detail", diagnostic.detail}, {"source", diagnostic.source},
               {"actionId", diagnostic.action_id}, {"bindingId", diagnostic.binding_id}};
    if (!diagnostic.related_action_id.empty()) value["relatedActionId"] = diagnostic.related_action_id;
    if (!diagnostic.related_binding_id.empty()) value["relatedBindingId"] = diagnostic.related_binding_id;
    return value;
}

InputMapIntentResult failure(std::string code, std::string detail) {
    return {.success = false, .code = std::move(code), .detail = std::move(detail), .intent = std::nullopt};
}

InputMapIntentResult success(InputMapEditIntent intent, std::string detail) {
    return {.success = true, .code = "ok", .detail = std::move(detail), .intent = std::move(intent)};
}

std::string intent_prefix(const InputMapIntentKind kind) {
    return "editor.project-settings.input-map.intent." + input_map_intent_kind_name(kind);
}

std::string intent_id(const InputMapIntentKind kind, const std::string_view action_id,
                     const std::string_view binding_id = {}, const std::string_view source = {}) {
    std::string result = intent_prefix(kind);
    result += ".";
    result += stable_component(action_id);
    if (!binding_id.empty()) {
        result += ".";
        result += stable_component(binding_id);
    }
    if (!source.empty()) {
        result += ".";
        result += stable_component(source);
    }
    return result;
}

std::string action_diagnostic_id(const std::string_view action_id, const std::size_t index,
                                 const std::string_view code) {
    return "editor.project-settings.input-map.diagnostic." + std::string(code) + "." +
        stable_component(action_id) + "." + std::to_string(index);
}

std::string binding_diagnostic_id(const std::string_view binding_id, const std::size_t index,
                                  const std::string_view code) {
    return "editor.project-settings.input-map.diagnostic." + std::string(code) + "." +
        stable_component(binding_id) + "." + std::to_string(index);
}

} // namespace

std::string input_map_action_kind_name(const InputActionKind kind) {
    if (kind == InputActionKind::button) return "button";
    if (kind == InputActionKind::axis_1d) return "axis1d";
    return "unknown";
}

std::string input_map_intent_kind_name(const InputMapIntentKind kind) {
    switch (kind) {
    case InputMapIntentKind::add_action: return "add-action";
    case InputMapIntentKind::remove_action: return "remove-action";
    case InputMapIntentKind::add_binding: return "add-binding";
    case InputMapIntentKind::remove_binding: return "remove-binding";
    case InputMapIntentKind::rebind_binding: return "rebind-binding";
    }
    return "unknown";
}

std::string editor_project_settings_document_id(const std::string_view project_id) {
    return "editor.project-settings." + stable_component(project_id);
}

std::string editor_project_settings_action_node_id(const std::string_view action_id) {
    return std::string(editor_project_settings_input_map_node_id) + ".action." + stable_component(action_id);
}

std::string editor_project_settings_binding_id(const std::string_view action_id,
                                                const std::string_view source) {
    return editor_project_settings_action_node_id(action_id) + ".binding." + stable_component(source);
}

std::string editor_project_settings_binding_node_id(const std::string_view action_id,
                                                     const std::string_view source) {
    return editor_project_settings_binding_id(action_id, source);
}

std::string input_map_intent_json(const InputMapEditIntent& intent) {
    return Json{{"schemaVersion", "noemancer.editor-input-map-intent/0.1"},
                {"intentId", intent.intent_id}, {"kind", input_map_intent_kind_name(intent.kind)},
                {"baseRevision", intent.base_revision}, {"actionId", intent.action_id},
                {"bindingId", intent.binding_id}, {"actionKind", input_map_action_kind_name(intent.action_kind)},
                {"previousSource", intent.previous_source}, {"source", intent.source},
                {"scale", intent.scale}, {"deadZone", intent.dead_zone}}.dump();
}

ProjectSettingsInputMapViewModel::ProjectSettingsInputMapViewModel(ProjectSettingsInputMapSnapshot snapshot)
    : snapshot_(std::move(snapshot)) {
    if (snapshot_.revision == 0U) snapshot_.revision = 1U;
    if (snapshot_.project_id.empty()) snapshot_.project_id = "project.unknown";
    if (snapshot_.project_name.empty()) snapshot_.project_name = "Project Settings";
    rebuild();
}

ProjectSettingsInputMapViewModel::ProjectSettingsInputMapViewModel(
    std::string project_id, std::string project_name, const std::uint64_t revision,
    const std::span<const InputActionDefinition> actions)
    : ProjectSettingsInputMapViewModel(ProjectSettingsInputMapSnapshot{
          .project_id = std::move(project_id),
          .project_name = std::move(project_name),
          .revision = revision,
          .actions = std::vector<InputActionDefinition>(actions.begin(), actions.end())}) {}

const ProjectSettingsInputMapSnapshot& ProjectSettingsInputMapViewModel::snapshot() const noexcept {
    return snapshot_;
}

const std::vector<InputMapActionView>& ProjectSettingsInputMapViewModel::actions() const noexcept {
    return actions_;
}

const std::vector<InputMapConflictDiagnostic>& ProjectSettingsInputMapViewModel::diagnostics() const noexcept {
    return diagnostics_;
}

bool ProjectSettingsInputMapViewModel::valid() const noexcept {
    for (const auto& diagnostic : diagnostics_)
        if (diagnostic.severity == "error") return false;
    return true;
}

void ProjectSettingsInputMapViewModel::rebuild() {
    actions_.clear();
    diagnostics_.clear();
    actions_.reserve(snapshot_.actions.size());

    std::map<std::string, std::size_t> action_occurrences;
    std::map<std::string, std::pair<std::size_t, std::size_t>> first_binding_by_source;
    std::map<std::string, std::vector<std::pair<std::size_t, std::size_t>>> bindings_by_source;

    const auto add_action_diagnostic = [&](const std::size_t action_index, const std::string_view code,
                                           const std::string_view detail, const std::string_view severity) {
        auto& action = actions_.at(action_index);
        const auto id = action_diagnostic_id(action.id, action_index, code);
        diagnostics_.push_back({.id = id,
                                 .code = std::string(code),
                                 .severity = std::string(severity),
                                 .detail = std::string(detail),
                                 .source = {},
                                 .action_id = action.id,
                                 .binding_id = {},
                                 .related_action_id = {},
                                 .related_binding_id = {}});
        action.diagnostic_ids.push_back(id);
    };

    for (std::size_t action_index = 0; action_index < snapshot_.actions.size(); ++action_index) {
        const auto& definition = snapshot_.actions.at(action_index);
        auto& occurrence = action_occurrences[definition.id];
        InputMapActionView action{.id = definition.id,
                                  .node_id = editor_project_settings_action_node_id(definition.id),
                                  .kind = definition.kind,
                                  .bindings = {},
                                  .diagnostic_ids = {}};
        const auto duplicate_action = occurrence > 0U;
        if (duplicate_action) action.node_id += ".duplicate." + std::to_string(occurrence);
        ++occurrence;
        actions_.push_back(std::move(action));
        const auto current_action_index = actions_.size() - 1U;
        if (duplicate_action)
            add_action_diagnostic(current_action_index, "action-id-duplicate",
                                  "Input action IDs must be unique.", "error");

        if (!safe_input_identifier(definition.id)) {
            add_action_diagnostic(current_action_index, definition.id.empty() ? "action-id-missing" : "action-id-invalid",
                                  "Input action ID must be a non-empty stable identifier.", "error");
        }
        if (!known_action_kind(definition.kind)) {
            add_action_diagnostic(current_action_index, "action-kind-invalid",
                                  "Input action kind must be button or axis1d.", "error");
        }
        if (definition.bindings.empty()) {
            add_action_diagnostic(current_action_index, "bindings-empty",
                                  "An input action needs at least one binding.", "error");
        }
        if (definition.bindings.size() > maximum_input_bindings) {
            add_action_diagnostic(current_action_index, "bindings-too-many",
                                  "An input action cannot contain more than 16 bindings.", "error");
        }

        std::map<std::string, std::size_t> source_occurrences;
        for (std::size_t binding_index = 0; binding_index < definition.bindings.size(); ++binding_index) {
            const auto& definition_binding = definition.bindings.at(binding_index);
            auto& source_occurrence = source_occurrences[definition_binding.source];
            auto binding_id = editor_project_settings_binding_id(definition.id, definition_binding.source);
            if (source_occurrence > 0U)
                binding_id += ".duplicate." + std::to_string(source_occurrence);
            ++source_occurrence;
            actions_.at(current_action_index).bindings.push_back({
                .id = binding_id,
                .node_id = binding_id,
                .source = definition_binding.source,
                .scale = definition_binding.scale,
                .dead_zone = definition_binding.dead_zone,
                .has_conflict = false,
                .diagnostic_ids = {}});
            const auto current_binding_index = actions_.at(current_action_index).bindings.size() - 1U;
            auto& binding = actions_.at(current_action_index).bindings.at(current_binding_index);

            const auto add_binding_diagnostic = [&](const std::string_view code, const std::string_view detail,
                                                     const std::string_view severity) {
                const auto id = binding_diagnostic_id(binding.id, binding_index, code);
                diagnostics_.push_back({.id = id,
                                         .code = std::string(code),
                                         .severity = std::string(severity),
                                         .detail = std::string(detail),
                                         .source = binding.source,
                                         .action_id = actions_.at(current_action_index).id,
                                         .binding_id = binding.id,
                                         .related_action_id = {},
                                         .related_binding_id = {}});
                binding.diagnostic_ids.push_back(id);
                actions_.at(current_action_index).diagnostic_ids.push_back(id);
            };

            if (!safe_input_identifier(binding.source)) {
                add_binding_diagnostic(binding.source.empty() ? "binding-source-missing" : "binding-source-invalid",
                                       "Binding source must be a non-empty stable identifier.", "error");
            }
            if (!std::isfinite(binding.scale) || binding.scale == 0.0F || std::abs(binding.scale) > 4.0F) {
                add_binding_diagnostic("binding-scale-invalid",
                                       "Binding scale must be finite, non-zero and within -4..4.", "error");
            }
            if (!std::isfinite(binding.dead_zone) || binding.dead_zone < 0.0F || binding.dead_zone >= 1.0F) {
                add_binding_diagnostic("binding-dead-zone-invalid",
                                       "Binding deadZone must be finite and within [0,1).", "error");
            }
            if (source_occurrence > 1U) {
                const auto first = std::ranges::find_if(actions_.at(current_action_index).bindings,
                    [&](const auto& candidate) { return candidate.source == binding.source && candidate.id != binding.id; });
                const auto id = binding_diagnostic_id(binding.id, binding_index, "binding-duplicate");
                diagnostics_.push_back({.id = id,
                                         .code = "binding-duplicate",
                                         .severity = "error",
                                         .detail = "A source can only be bound once within an input action.",
                                         .source = binding.source,
                                         .action_id = actions_.at(current_action_index).id,
                                         .binding_id = binding.id,
                                         .related_action_id = actions_.at(current_action_index).id,
                                         .related_binding_id = first == actions_.at(current_action_index).bindings.end() ?
                                             std::string{} : first->id});
                binding.diagnostic_ids.push_back(id);
                actions_.at(current_action_index).diagnostic_ids.push_back(id);
            }
            if (!binding.source.empty()) {
                const auto source_key = binding.source;
                if (!first_binding_by_source.contains(source_key))
                    first_binding_by_source.emplace(source_key, std::make_pair(current_action_index, current_binding_index));
                bindings_by_source[source_key].emplace_back(current_action_index, current_binding_index);
            }
        }
    }

    std::size_t conflict_sequence{};
    for (const auto& [source, references] : bindings_by_source) {
        if (references.size() < 2U) continue;
        for (std::size_t reference_index = 1; reference_index < references.size(); ++reference_index) {
            const auto [action_index, binding_index] = references.at(reference_index);
            const auto [related_action_index, related_binding_index] = references.at(reference_index - 1U);
            auto& binding = actions_.at(action_index).bindings.at(binding_index);
            const auto& related_binding = actions_.at(related_action_index).bindings.at(related_binding_index);
            const auto id = "editor.project-settings.input-map.diagnostic.binding-conflict." +
                stable_component(source) + "." + std::to_string(conflict_sequence++);
            const auto severity = action_index == related_action_index ? "error" : "warning";
            diagnostics_.push_back({.id = id,
                                     .code = "binding-conflict",
                                     .severity = severity,
                                     .detail = "Input source '" + source + "' is used by more than one action.",
                                     .source = source,
                                     .action_id = actions_.at(action_index).id,
                                     .binding_id = binding.id,
                                     .related_action_id = actions_.at(related_action_index).id,
                                     .related_binding_id = related_binding.id});
            binding.has_conflict = true;
            binding.diagnostic_ids.push_back(id);
            actions_.at(action_index).diagnostic_ids.push_back(id);
            actions_.at(related_action_index).bindings.at(related_binding_index).has_conflict = true;
            actions_.at(related_action_index).bindings.at(related_binding_index).diagnostic_ids.push_back(id);
            actions_.at(related_action_index).diagnostic_ids.push_back(id);
        }
    }

    if (snapshot_.actions.size() > maximum_input_actions) {
        // This is a document-level diagnostic; attach it to the input-map root
        // through the first action when possible, while preserving every row.
        const auto id = "editor.project-settings.input-map.diagnostic.actions-too-many";
        diagnostics_.push_back({.id = id,
                                 .code = "actions-too-many",
                                 .severity = "error",
                                 .detail = "A project cannot contain more than 64 input actions.",
                                 .source = {},
                                 .action_id = {},
                                 .binding_id = {},
                                 .related_action_id = {},
                                 .related_binding_id = {}});
        if (!actions_.empty()) actions_.front().diagnostic_ids.push_back(id);
    }
}

std::string ProjectSettingsInputMapViewModel::semantic_ui_document_json(const std::string_view locale) const {
    Json nodes = Json::array();
    auto root = make_node(editor_project_settings_node_id, {}, "project-settings", snapshot_.project_name, false);
    root["source"] = {{"projectId", snapshot_.project_id}, {"revision", snapshot_.revision}};
    root["fingerprint"] = fingerprint(root);
    nodes.push_back(std::move(root));

    auto input_map = make_node(editor_project_settings_input_map_node_id, editor_project_settings_node_id,
                               "input-map", "Input Map", true);
    input_map["value"] = {{"actionCount", actions_.size()}, {"diagnosticCount", diagnostics_.size()}};
    input_map["binding"] = {{"kind", "editor-input-map"}, {"projectId", snapshot_.project_id},
                              {"revision", snapshot_.revision}};
    input_map["actions"].push_back({{"id", std::string(editor_project_settings_input_map_add_action_id)},
                                     {"mode", "plan-apply-receipt"}, {"revisionBound", true}, {"undoable", true}});
    input_map["fingerprint"] = fingerprint(input_map);
    nodes.push_back(std::move(input_map));

    Json input_map_actions = Json::array();
    for (const auto& action : actions_) {
        auto action_node = make_node(action.node_id, editor_project_settings_input_map_node_id,
                                     "input-action", action.id.empty() ? "Unnamed Action" : action.id, true);
        action_node["value"] = {{"id", action.id}, {"kind", input_map_action_kind_name(action.kind)},
                                 {"bindingCount", action.bindings.size()}};
        action_node["binding"] = {{"kind", "editor-input-action"}, {"projectId", snapshot_.project_id},
                                   {"actionId", action.id}, {"revision", snapshot_.revision}};
        action_node["actions"].push_back({{"id", std::string(editor_project_settings_input_map_remove_action_id)},
                                           {"mode", "plan-apply-receipt"}, {"revisionBound", true}, {"undoable", true}});
        action_node["actions"].push_back({{"id", std::string(editor_project_settings_input_map_add_binding_id)},
                                           {"mode", "plan-apply-receipt"}, {"revisionBound", true}, {"undoable", true}});
        if (!action.diagnostic_ids.empty()) action_node["diagnosticIds"] = action.diagnostic_ids;
        action_node["fingerprint"] = fingerprint(action_node);
        nodes.push_back(std::move(action_node));

        for (const auto& binding : action.bindings) {
            auto binding_node = make_node(binding.node_id, action.node_id, "input-binding",
                                          binding.source.empty() ? "Unassigned Input" : binding.source, true);
            binding_node["value"] = {{"source", binding.source}, {"scale", binding.scale},
                                      {"deadZone", binding.dead_zone}};
            binding_node["binding"] = {{"kind", "editor-input-binding"}, {"projectId", snapshot_.project_id},
                                        {"actionId", action.id}, {"bindingId", binding.id},
                                        {"revision", snapshot_.revision}};
            binding_node["actions"].push_back({{"id", std::string(editor_project_settings_input_map_remove_binding_id)},
                                                {"mode", "plan-apply-receipt"}, {"revisionBound", true}, {"undoable", true}});
            binding_node["actions"].push_back({{"id", std::string(editor_project_settings_input_map_rebind_id)},
                                                {"mode", "plan-apply-receipt"}, {"revisionBound", true}, {"undoable", true}});
            if (binding.has_conflict) binding_node["state"]["error"] = "Input source conflicts with another binding.";
            if (!binding.diagnostic_ids.empty()) binding_node["diagnosticIds"] = binding.diagnostic_ids;
            binding_node["fingerprint"] = fingerprint(binding_node);
            nodes.push_back(std::move(binding_node));
        }

        Json action_value{{"id", action.id}, {"nodeId", action.node_id},
                          {"kind", input_map_action_kind_name(action.kind)}, {"bindings", Json::array()},
                          {"diagnosticIds", action.diagnostic_ids}};
        for (const auto& binding : action.bindings)
            action_value["bindings"].push_back({{"id", binding.id}, {"nodeId", binding.node_id},
                                                 {"source", binding.source}, {"scale", binding.scale},
                                                 {"deadZone", binding.dead_zone},
                                                 {"hasConflict", binding.has_conflict},
                                                 {"diagnosticIds", binding.diagnostic_ids}});
        input_map_actions.push_back(std::move(action_value));
    }

    Json diagnostic_values = Json::array();
    for (const auto& diagnostic : diagnostics_) diagnostic_values.push_back(diagnostic_json(diagnostic));
    const auto document_valid = valid();
    Json document{{"schemaVersion", std::string(editor_project_settings_input_map_ui_schema)},
                  {"valid", document_valid}, {"code", document_valid ? "ok" : "editor.input-map.invalid"},
                  {"documentId", editor_project_settings_document_id(snapshot_.project_id)},
                  {"surface", "editor"}, {"kind", "project-settings"}, {"revision", snapshot_.revision},
                  {"locale", std::string(locale)}, {"roots", Json::array({std::string(editor_project_settings_node_id)})},
                  {"capabilities", {{"semanticQuery", true}, {"transactionActions", true},
                                    {"projectSettings", true}, {"inputMapAuthoring", true},
                                    {"conflictDiagnostics", true}, {"layoutEvidence", false}}},
                  {"settings", {{"projectId", snapshot_.project_id}, {"name", snapshot_.project_name}}},
                  {"inputMap", {{"nodeId", std::string(editor_project_settings_input_map_node_id)},
                                 {"actions", std::move(input_map_actions)}}},
                  {"authoring", {{"schemaVersion", std::string(editor_project_settings_input_map_schema)},
                                  {"revisionBound", true}, {"dispatch", "editor-domain-plan-apply-receipt"},
                                  {"maximumActions", maximum_input_actions},
                                  {"maximumBindingsPerAction", maximum_input_bindings},
                                  {"operations", Json::array({std::string(editor_project_settings_input_map_add_action_id),
                                                               std::string(editor_project_settings_input_map_remove_action_id),
                                                               std::string(editor_project_settings_input_map_add_binding_id),
                                                               std::string(editor_project_settings_input_map_remove_binding_id),
                                                               std::string(editor_project_settings_input_map_rebind_id)})}}},
                  {"diagnostics", std::move(diagnostic_values)}, {"nodes", std::move(nodes)}};
    document["validation"] = {{"schemaVersion", "noemancer.ui-validation/0.1"}, {"valid", document_valid},
                               {"code", document_valid ? "ok" : "ui.invalid-document"},
                               {"nodeCount", document.at("nodes").size()}};
    return document.dump();
}

std::string ProjectSettingsInputMapViewModel::authoring_json(const std::string_view locale) const {
    auto document = Json::parse(semantic_ui_document_json(locale), nullptr, false);
    if (document.is_discarded() || !document.is_object()) return "{}";
    document["schemaVersion"] = std::string(editor_project_settings_input_map_schema);
    document["documentSchema"] = std::string(editor_project_settings_input_map_ui_schema);
    return document.dump();
}

InputMapIntentResult ProjectSettingsInputMapViewModel::add_action_intent(
    const std::string_view action_id, const InputActionKind kind, const std::string_view initial_source,
    const float scale, const float dead_zone) const {
    if (!safe_input_identifier(action_id))
        return failure("editor.input-map.invalid-action-id", "Action id must be a non-empty stable identifier.");
    if (!known_action_kind(kind))
        return failure("editor.input-map.invalid-action-kind", "Action kind must be button or axis1d.");
    for (const auto& action : actions_)
        if (action.id == action_id)
            return failure("editor.input-map.action-already-exists", "An input action with this id already exists.");
    if (actions_.size() >= maximum_input_actions)
        return failure("editor.input-map.actions-limit", "The project already contains 64 input actions.");
    if (!initial_source.empty() && !safe_input_identifier(initial_source))
        return failure("editor.input-map.invalid-binding-source", "Initial binding source must use a stable identifier.");
    if (!std::isfinite(scale) || scale == 0.0F || std::abs(scale) > 4.0F)
        return failure("editor.input-map.invalid-binding-scale", "Initial binding scale must be finite, non-zero and within -4..4.");
    if (!std::isfinite(dead_zone) || dead_zone < 0.0F || dead_zone >= 1.0F)
        return failure("editor.input-map.invalid-binding-dead-zone", "Initial binding deadZone must be within [0,1).");

    InputMapEditIntent intent{.kind = InputMapIntentKind::add_action,
                              .intent_id = intent_id(InputMapIntentKind::add_action, action_id),
                              .base_revision = snapshot_.revision,
                              .action_id = std::string(action_id),
                              .binding_id = {},
                              .action_kind = kind,
                              .source = std::string(initial_source),
                              .scale = scale,
                              .dead_zone = dead_zone};
    return success(std::move(intent), "Input action add intent is ready for plan/apply.");
}

InputMapIntentResult ProjectSettingsInputMapViewModel::remove_action_intent(
    const std::string_view action_id) const {
    const auto found = std::ranges::find(actions_, action_id, &InputMapActionView::id);
    if (found == actions_.end())
        return failure("editor.input-map.action-not-found", "The requested input action does not exist.");
    if (std::ranges::count(actions_, action_id, &InputMapActionView::id) > 1)
        return failure("editor.input-map.action-id-ambiguous", "The requested action id is duplicated in the document.");

    InputMapEditIntent intent{.kind = InputMapIntentKind::remove_action,
                              .intent_id = intent_id(InputMapIntentKind::remove_action, action_id),
                              .base_revision = snapshot_.revision,
                              .action_id = std::string(action_id),
                              .binding_id = {},
                              .action_kind = found->kind,
                              .source = {},
                              .scale = 1.0F,
                              .dead_zone = 0.0F};
    return success(std::move(intent), "Input action remove intent is ready for plan/apply.");
}

InputMapIntentResult ProjectSettingsInputMapViewModel::add_binding_intent(
    const std::string_view action_id, const std::string_view source, const float scale,
    const float dead_zone) const {
    const auto action = std::ranges::find(actions_, action_id, &InputMapActionView::id);
    if (action == actions_.end())
        return failure("editor.input-map.action-not-found", "The requested input action does not exist.");
    if (!safe_input_identifier(source))
        return failure("editor.input-map.invalid-binding-source", "Binding source must be a non-empty stable identifier.");
    if (!std::isfinite(scale) || scale == 0.0F || std::abs(scale) > 4.0F)
        return failure("editor.input-map.invalid-binding-scale", "Binding scale must be finite, non-zero and within -4..4.");
    if (!std::isfinite(dead_zone) || dead_zone < 0.0F || dead_zone >= 1.0F)
        return failure("editor.input-map.invalid-binding-dead-zone", "Binding deadZone must be finite and within [0,1). ");
    if (action->bindings.size() >= maximum_input_bindings)
        return failure("editor.input-map.bindings-limit", "The action already contains 16 bindings.");
    for (const auto& binding : action->bindings)
        if (binding.source == source)
            return failure("editor.input-map.binding-already-exists", "This source is already bound to the action.");

    InputMapEditIntent intent{.kind = InputMapIntentKind::add_binding,
                              .intent_id = intent_id(InputMapIntentKind::add_binding, action_id, {}, source),
                              .base_revision = snapshot_.revision,
                              .action_id = std::string(action_id),
                              .binding_id = editor_project_settings_binding_id(action_id, source),
                              .action_kind = action->kind,
                              .source = std::string(source),
                              .scale = scale,
                              .dead_zone = dead_zone};
    return success(std::move(intent), "Input binding add intent is ready for plan/apply.");
}

InputMapIntentResult ProjectSettingsInputMapViewModel::remove_binding_intent(
    const std::string_view action_id, const std::string_view binding_id) const {
    const auto action = std::ranges::find(actions_, action_id, &InputMapActionView::id);
    if (action == actions_.end())
        return failure("editor.input-map.action-not-found", "The requested input action does not exist.");
    const auto binding = std::ranges::find(action->bindings, binding_id, &InputMapBindingView::id);
    if (binding == action->bindings.end())
        return failure("editor.input-map.binding-not-found", "The requested input binding does not exist.");

    InputMapEditIntent intent{.kind = InputMapIntentKind::remove_binding,
                              .intent_id = intent_id(InputMapIntentKind::remove_binding, action_id, binding_id),
                              .base_revision = snapshot_.revision,
                              .action_id = std::string(action_id),
                              .binding_id = std::string(binding_id),
                              .action_kind = action->kind,
                              .previous_source = binding->source,
                              .source = binding->source,
                              .scale = binding->scale,
                              .dead_zone = binding->dead_zone};
    return success(std::move(intent), "Input binding remove intent is ready for plan/apply.");
}

InputMapIntentResult ProjectSettingsInputMapViewModel::rebind_binding_intent(
    const std::string_view action_id, const std::string_view binding_id, const std::string_view source,
    const float scale, const float dead_zone) const {
    const auto action = std::ranges::find(actions_, action_id, &InputMapActionView::id);
    if (action == actions_.end())
        return failure("editor.input-map.action-not-found", "The requested input action does not exist.");
    const auto binding = std::ranges::find(action->bindings, binding_id, &InputMapBindingView::id);
    if (binding == action->bindings.end())
        return failure("editor.input-map.binding-not-found", "The requested input binding does not exist.");
    if (!safe_input_identifier(source))
        return failure("editor.input-map.invalid-binding-source", "Binding source must be a non-empty stable identifier.");
    if (!std::isfinite(scale) || scale == 0.0F || std::abs(scale) > 4.0F)
        return failure("editor.input-map.invalid-binding-scale", "Binding scale must be finite, non-zero and within -4..4.");
    if (!std::isfinite(dead_zone) || dead_zone < 0.0F || dead_zone >= 1.0F)
        return failure("editor.input-map.invalid-binding-dead-zone", "Binding deadZone must be finite and within [0,1). ");
    for (const auto& candidate : action->bindings)
        if (&candidate != &*binding && candidate.source == source)
            return failure("editor.input-map.binding-already-exists", "This source is already bound to the action.");

    InputMapEditIntent intent{.kind = InputMapIntentKind::rebind_binding,
                              .intent_id = intent_id(InputMapIntentKind::rebind_binding, action_id, binding_id, source),
                              .base_revision = snapshot_.revision,
                              .action_id = std::string(action_id),
                              .binding_id = std::string(binding_id),
                              .action_kind = action->kind,
                              .previous_source = binding->source,
                              .source = std::string(source),
                              .scale = scale,
                              .dead_zone = dead_zone};
    return success(std::move(intent), "Input binding rebind intent is ready for plan/apply.");
}

} // namespace noemancer
