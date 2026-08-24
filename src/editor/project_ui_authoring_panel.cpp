#include "editor/project_ui_authoring_panel.hpp"

#include <imgui.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <functional>
#include <iomanip>
#include <initializer_list>
#include <limits>
#include <map>
#include <ranges>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace noemancer {
namespace {

using Json = nlohmann::json;

constexpr std::string_view ui_document_schema = "noemancer.ui-document/0.1";
constexpr std::size_t maximum_nodes = 4096U;
constexpr std::size_t maximum_depth = 64U;
constexpr std::size_t maximum_text_bytes = 4096U;
constexpr std::size_t maximum_binding_bytes = 64U * 1024U;

std::string string_member(const Json& object, const std::string_view name,
                          const std::string_view fallback = {}) {
    if (!object.is_object()) return std::string(fallback);
    const auto found = object.find(name);
    return found != object.end() && found->is_string() ? found->get<std::string>() : std::string(fallback);
}

std::string capped_string(std::string value, const std::size_t maximum) {
    if (value.size() > maximum) value.resize(maximum);
    return value;
}

ProjectUiAuthoringNodeKind kind_from_string(std::string_view value) noexcept {
    if (value == "container" || value == "panel" || value == "group" || value == "hud" ||
        value == "window" || value == "root")
        return ProjectUiAuthoringNodeKind::container;
    if (value == "text" || value == "label" || value == "heading") return ProjectUiAuthoringNodeKind::text;
    if (value == "button" || value == "action" || value == "link") return ProjectUiAuthoringNodeKind::button;
    if (value == "property" || value == "field" || value == "input" || value == "slider" || value == "meter")
        return ProjectUiAuthoringNodeKind::property;
    return ProjectUiAuthoringNodeKind::unknown;
}

std::string action_id_from_node(const Json& node) {
    const auto explicit_id = string_member(node, "actionId");
    if (!explicit_id.empty()) return explicit_id;
    const auto actions = node.find("actions");
    if (actions == node.end() || !actions->is_array()) return {};
    for (const auto& action : *actions) {
        const auto id = string_member(action, "id");
        if (!id.empty()) return id;
    }
    return {};
}

std::string binding_json_from_node(const Json& node) {
    // Action-level bindings describe the invocation contract.  They take
    // precedence over node.binding, which may instead carry a displayed
    // property value (for example a world-property inspector field).
    const auto actions = node.find("actions");
    if (actions != node.end() && actions->is_array()) {
        for (const auto& action : *actions) {
            const auto action_binding = action.find("binding");
            if (action_binding == action.end() || action_binding->is_null()) continue;
            if (action_binding->is_string()) return action_binding->get<std::string>();
            return action_binding->dump();
        }
    }
    const auto binding = node.find("binding");
    if (binding == node.end() || binding->is_null()) return "{}";
    if (binding->is_string()) return binding->get<std::string>();
    return binding->dump();
}

ProjectUiAuthoringJsonKind json_kind(const Json& value) noexcept {
    if (value.is_object()) return ProjectUiAuthoringJsonKind::object;
    if (value.is_array()) return ProjectUiAuthoringJsonKind::array;
    if (value.is_null()) return ProjectUiAuthoringJsonKind::scalar;
    return ProjectUiAuthoringJsonKind::scalar;
}

std::string json_member_from_node(const Json& node, const std::string_view name,
                                  const std::string_view fallback, bool* present = nullptr) {
    const auto found = node.find(name);
    if (found == node.end()) {
        if (present != nullptr) *present = false;
        return std::string(fallback);
    }
    if (present != nullptr) *present = true;
    return found->dump();
}

std::string first_string_member(const Json& object,
                                const std::initializer_list<std::string_view> names) {
    if (!object.is_object()) return {};
    for (const auto name : names) {
        const auto value = string_member(object, name);
        if (!value.empty()) return value;
    }
    return {};
}

const Json* first_json_member(const Json& object,
                              const std::initializer_list<std::string_view> names) {
    if (!object.is_object()) return nullptr;
    for (const auto name : names) {
        const auto found = object.find(name);
        if (found != object.end()) return &*found;
    }
    return nullptr;
}

std::string json_kind_name(const ProjectUiAuthoringJsonKind kind) noexcept {
    switch (kind) {
    case ProjectUiAuthoringJsonKind::absent: return "absent";
    case ProjectUiAuthoringJsonKind::object: return "object";
    case ProjectUiAuthoringJsonKind::array: return "array";
    case ProjectUiAuthoringJsonKind::scalar: return "scalar";
    case ProjectUiAuthoringJsonKind::invalid: return "invalid";
    }
    return "invalid";
}

bool has_field_error(const std::map<std::string, std::string>& errors,
                     const std::string_view key) {
    return errors.contains(std::string(key));
}

std::string hash_payload(std::string_view value) {
    std::uint64_t hash = 14695981039346656037ULL;
    for (const auto byte : value) {
        hash ^= static_cast<unsigned char>(byte);
        hash *= 1099511628211ULL;
    }
    std::ostringstream output;
    output << std::hex << std::setfill('0') << std::setw(16) << hash;
    return output.str();
}

std::string json_or_string(const Json& value) {
    return value.is_string() ? value.get<std::string>() : value.dump();
}

void add_diagnostic(ProjectUiAuthoringView& view, std::string code, std::string path,
                    std::string detail, std::string node_id = {}) {
    view.diagnostics.push_back({std::move(code), std::move(path), std::move(node_id), std::move(detail)});
}

Json field_state_json(const ProjectUiAuthoringFieldState& state) {
    return {{"field", project_ui_authoring_field_kind_name(state.field)}, {"valid", state.valid},
            {"dirty", state.dirty}, {"disabled", state.disabled}, {"pending", state.pending},
            {"conflict", state.conflict}, {"error", state.error}};
}

bool draw_text_input(const char* label, std::string& value, const std::size_t capacity = 512U) {
    std::vector<char> buffer(std::max<std::size_t>(capacity, value.size() + 1U));
    std::ranges::copy(value, buffer.begin());
    buffer[value.size()] = '\0';
    if (!ImGui::InputText(label, buffer.data(), buffer.size())) return false;
    value = buffer.data();
    return true;
}

bool draw_multiline_text(const char* label, std::string& value, const ImVec2 size = {0.0F, 76.0F},
                         const std::size_t capacity = 4096U) {
    std::vector<char> buffer(std::max<std::size_t>(capacity, value.size() + 1U));
    std::ranges::copy(value, buffer.begin());
    buffer[value.size()] = '\0';
    if (!ImGui::InputTextMultiline(label, buffer.data(), buffer.size(), size)) return false;
    value = buffer.data();
    return true;
}

void draw_field_status(const ProjectUiAuthoringFieldState& status) {
    const auto field = project_ui_authoring_field_kind_name(status.field);
    if (!status.valid) {
        ImGui::SameLine();
        ImGui::TextColored({1.0F, 0.42F, 0.34F, 1.0F}, "error");
    } else if (status.conflict) {
        ImGui::SameLine();
        ImGui::TextColored({1.0F, 0.72F, 0.24F, 1.0F}, "conflict");
    } else if (status.pending) {
        ImGui::SameLine();
        ImGui::TextColored({0.42F, 0.78F, 1.0F, 1.0F}, "pending");
    } else if (status.dirty) {
        ImGui::SameLine();
        ImGui::TextColored({0.45F, 0.88F, 0.62F, 1.0F}, "dirty");
    }
    if (!status.error.empty()) {
        ImGui::TextColored({1.0F, 0.42F, 0.34F, 1.0F}, "%s: %s", field, status.error.c_str());
    }
}

void draw_readonly_json_summary(const char* label, const std::string& json) {
    const auto parsed = Json::parse(json, nullptr, false);
    if (parsed.is_discarded()) {
        ImGui::TextColored({1.0F, 0.42F, 0.34F, 1.0F}, "%s: invalid JSON", label);
        return;
    }
    if (!parsed.is_object()) {
        ImGui::TextDisabled("%s: %s", label, parsed.type_name());
        return;
    }
    ImGui::TextDisabled("%s: %zu keys", label, parsed.size());
    for (const auto& [key, value] : parsed.items()) {
        const auto preview = value.is_string() ? value.get<std::string>() : value.dump();
        const auto clipped = preview.size() > 96U ? preview.substr(0U, 93U) + "..." : preview;
        ImGui::BulletText("%s = %s", key.c_str(), clipped.c_str());
    }
}

} // namespace

const char* project_ui_authoring_node_kind_name(const ProjectUiAuthoringNodeKind kind) noexcept {
    switch (kind) {
    case ProjectUiAuthoringNodeKind::unknown: return "unknown";
    case ProjectUiAuthoringNodeKind::container: return "container";
    case ProjectUiAuthoringNodeKind::text: return "text";
    case ProjectUiAuthoringNodeKind::button: return "button";
    case ProjectUiAuthoringNodeKind::property: return "property";
    }
    return "unknown";
}

const char* project_ui_authoring_field_kind_name(const ProjectUiAuthoringFieldKind kind) noexcept {
    switch (kind) {
    case ProjectUiAuthoringFieldKind::label: return "label";
    case ProjectUiAuthoringFieldKind::role: return "role";
    case ProjectUiAuthoringFieldKind::parent: return "parent";
    case ProjectUiAuthoringFieldKind::action_id: return "actionId";
    case ProjectUiAuthoringFieldKind::binding: return "binding";
    case ProjectUiAuthoringFieldKind::state: return "state";
    case ProjectUiAuthoringFieldKind::presentation: return "presentation";
    case ProjectUiAuthoringFieldKind::value: return "value";
    case ProjectUiAuthoringFieldKind::component_ref: return "componentRef";
    case ProjectUiAuthoringFieldKind::component_declaration: return "componentDeclaration";
    case ProjectUiAuthoringFieldKind::design_tokens: return "designTokens";
    }
    return "unknown";
}

const char* project_ui_authoring_request_kind_name(const ProjectUiAuthoringPanelRequestKind kind) noexcept {
    switch (kind) {
    case ProjectUiAuthoringPanelRequestKind::add_node: return "add-node";
    case ProjectUiAuthoringPanelRequestKind::remove_node: return "remove-node";
    case ProjectUiAuthoringPanelRequestKind::update_node: return "update-node";
    case ProjectUiAuthoringPanelRequestKind::reparent_node: return "reparent-node";
    case ProjectUiAuthoringPanelRequestKind::undo: return "undo";
    case ProjectUiAuthoringPanelRequestKind::redo: return "redo";
    case ProjectUiAuthoringPanelRequestKind::update_design_tokens: return "update-design-tokens";
    case ProjectUiAuthoringPanelRequestKind::add_component_declaration: return "add-component-declaration";
    case ProjectUiAuthoringPanelRequestKind::update_component_declaration: return "update-component-declaration";
    case ProjectUiAuthoringPanelRequestKind::remove_component_declaration: return "remove-component-declaration";
    }
    return "unknown";
}

ProjectUiAuthoringPanel::ProjectUiAuthoringPanel(ProjectUiAuthoringSnapshot snapshot)
    : snapshot_(std::move(snapshot)), add_draft_{
          .id = "ui.new-node", .parent_id = {}, .role = "container", .label = "New Node",
          .action_id = {}, .binding_json = "{}"} {
    parse_document();
    design_tokens_draft_ = view_.design_tokens_json;
    retain_selection();
}

const ProjectUiAuthoringView& ProjectUiAuthoringPanel::view() const noexcept {
    return view_;
}

ProjectUiAuthoringPanelState ProjectUiAuthoringPanel::state() const {
    ProjectUiAuthoringPanelState result{
        .revision = snapshot_.revision,
        .fingerprint = snapshot_.fingerprint,
        .selected_node_id = selected_node_id_,
        .selected_draft = {},
        .add_draft = add_draft_,
        .add_kind = add_kind_,
        .design_tokens_json = design_tokens_draft_,
        .design_tokens_status = view_.design_tokens_status,
        .can_undo = snapshot_.can_undo,
        .can_redo = snapshot_.can_redo,
        .has_pending_request = pending_request_.has_value(),
        .pending_request_id = pending_request_ ? pending_request_->request_id : std::string{},
        .last_error = last_error_};
    if (const auto found = drafts_.find(selected_node_id_); found != drafts_.end())
        result.selected_draft = found->second;
    else if (const auto* node = selected_node(); node != nullptr)
        result.selected_draft = {.id = node->id, .parent_id = node->parent_id, .role = node->role,
                                 .label = node->label, .action_id = node->action_id,
                                 .binding_json = node->binding_json, .state_json = node->state_json,
                                 .presentation_json = node->presentation_json, .value_json = node->value_json,
                                 .component_ref = node->component_ref};
    if (const auto* node = selected_node(); node != nullptr) {
        result.selected_fields.reserve(12U);
        for (const auto field : {ProjectUiAuthoringFieldKind::label, ProjectUiAuthoringFieldKind::role,
                                 ProjectUiAuthoringFieldKind::parent, ProjectUiAuthoringFieldKind::action_id,
                                 ProjectUiAuthoringFieldKind::binding, ProjectUiAuthoringFieldKind::state,
                                 ProjectUiAuthoringFieldKind::presentation, ProjectUiAuthoringFieldKind::value,
                                 ProjectUiAuthoringFieldKind::component_ref,
                                 ProjectUiAuthoringFieldKind::component_declaration})
            result.selected_fields.push_back(field_state(*node, field));
    }
    return result;
}

const std::vector<ProjectUiAuthoringDiagnostic>& ProjectUiAuthoringPanel::diagnostics() const noexcept {
    return view_.diagnostics;
}

std::string_view ProjectUiAuthoringPanel::last_error() const noexcept {
    return last_error_;
}

std::optional<ProjectUiAuthoringPanelRequest> ProjectUiAuthoringPanel::consume_request() {
    if (!pending_request_) return std::nullopt;
    auto result = std::move(pending_request_);
    pending_request_.reset();
    return result;
}

void ProjectUiAuthoringPanel::set_snapshot(ProjectUiAuthoringSnapshot snapshot) {
    const auto previous_revision = snapshot_.revision;
    const auto design_tokens_dirty = design_tokens_draft_ != view_.design_tokens_json;
    if (snapshot.revision != previous_revision) mark_dirty_conflicts(snapshot.revision);
    snapshot_ = std::move(snapshot);
    parse_document();
    if (!design_tokens_dirty) design_tokens_draft_ = view_.design_tokens_json;
    else if (snapshot_.revision != previous_revision)
        conflicted_fields_.insert(field_key({}, ProjectUiAuthoringFieldKind::design_tokens));
    if (pending_request_ && pending_request_->base_revision != snapshot_.revision) {
        mark_conflict_for_request(*pending_request_);
        pending_request_.reset();
        last_error_ = "The pending project UI request became stale after the document revision changed.";
    } else if (previous_revision != snapshot_.revision && !view_.valid) {
        last_error_ = view_.diagnostics.empty() ? "The project UI document is invalid." : view_.diagnostics.front().detail;
    }
    retain_selection();
}

bool ProjectUiAuthoringPanel::select_node(const std::string_view node_id) {
    const auto found = std::ranges::find(view_.nodes, node_id, &ProjectUiAuthoringNode::id);
    if (found == view_.nodes.end()) {
        last_error_ = "The requested project UI node does not exist.";
        return false;
    }
    selected_node_id_ = found->id;
    sync_selected_draft();
    last_error_.clear();
    return true;
}

bool ProjectUiAuthoringPanel::set_selected_draft(ProjectUiAuthoringNodeDraft draft) {
    if (selected_node_id_.empty() || draft.id != selected_node_id_) {
        last_error_ = "A selected project UI node is required before editing its draft.";
        return false;
    }
    if (draft.role.empty()) {
        last_error_ = "A project UI node role cannot be empty.";
        return false;
    }
    if (!validate_binding_json(draft.binding_json) ||
        !validate_object_json(draft.state_json, "state") ||
        !validate_object_json(draft.presentation_json, "presentation") ||
        !validate_any_json(draft.value_json, "value")) return false;
    drafts_[selected_node_id_] = std::move(draft);
    last_error_.clear();
    return true;
}

bool ProjectUiAuthoringPanel::set_label(std::string label) {
    if (selected_node_id_.empty()) {
        last_error_ = "Select a project UI node before editing its label.";
        return false;
    }
    sync_selected_draft();
    auto& draft = drafts_[selected_node_id_];
    draft.label = capped_string(std::move(label), maximum_text_bytes);
    clear_field_error(selected_node_id_, ProjectUiAuthoringFieldKind::label);
    last_error_.clear();
    return true;
}

bool ProjectUiAuthoringPanel::set_role(std::string role) {
    if (selected_node_id_.empty()) {
        last_error_ = "Select a project UI node before editing its role.";
        return false;
    }
    if (role.empty()) {
        last_error_ = "A project UI node role cannot be empty.";
        return false;
    }
    sync_selected_draft();
    drafts_[selected_node_id_].role = capped_string(std::move(role), maximum_text_bytes);
    clear_field_error(selected_node_id_, ProjectUiAuthoringFieldKind::role);
    last_error_.clear();
    return true;
}

bool ProjectUiAuthoringPanel::set_parent(std::string parent_id) {
    if (selected_node_id_.empty()) {
        last_error_ = "Select a project UI node before editing its parent.";
        return false;
    }
    sync_selected_draft();
    drafts_[selected_node_id_].parent_id = capped_string(std::move(parent_id), maximum_text_bytes);
    clear_field_error(selected_node_id_, ProjectUiAuthoringFieldKind::parent);
    last_error_.clear();
    return true;
}

bool ProjectUiAuthoringPanel::set_action_id(std::string action_id) {
    if (selected_node_id_.empty()) {
        last_error_ = "Select a project UI node before editing its action.";
        return false;
    }
    sync_selected_draft();
    drafts_[selected_node_id_].action_id = capped_string(std::move(action_id), maximum_text_bytes);
    clear_field_error(selected_node_id_, ProjectUiAuthoringFieldKind::action_id);
    last_error_.clear();
    return true;
}

bool ProjectUiAuthoringPanel::set_binding_json(std::string binding_json) {
    if (selected_node_id_.empty()) {
        last_error_ = "Select a project UI node before editing its binding.";
        return false;
    }
    if (!validate_binding_json(binding_json)) {
        set_field_error(selected_node_id_, ProjectUiAuthoringFieldKind::binding, std::string(last_error_));
        return false;
    }
    sync_selected_draft();
    drafts_[selected_node_id_].binding_json = std::move(binding_json);
    clear_field_error(selected_node_id_, ProjectUiAuthoringFieldKind::binding);
    last_error_.clear();
    return true;
}

bool ProjectUiAuthoringPanel::set_state_json(std::string state_json) {
    if (selected_node_id_.empty()) {
        last_error_ = "Select a project UI node before editing its state.";
        return false;
    }
    if (!validate_object_json(state_json, "state")) {
        set_field_error(selected_node_id_, ProjectUiAuthoringFieldKind::state, std::string(last_error_));
        return false;
    }
    sync_selected_draft();
    drafts_[selected_node_id_].state_json = std::move(state_json);
    clear_field_error(selected_node_id_, ProjectUiAuthoringFieldKind::state);
    last_error_.clear();
    return true;
}

bool ProjectUiAuthoringPanel::set_presentation_json(std::string presentation_json) {
    if (selected_node_id_.empty()) {
        last_error_ = "Select a project UI node before editing its presentation.";
        return false;
    }
    if (!validate_object_json(presentation_json, "presentation")) {
        set_field_error(selected_node_id_, ProjectUiAuthoringFieldKind::presentation, std::string(last_error_));
        return false;
    }
    sync_selected_draft();
    drafts_[selected_node_id_].presentation_json = std::move(presentation_json);
    clear_field_error(selected_node_id_, ProjectUiAuthoringFieldKind::presentation);
    last_error_.clear();
    return true;
}

bool ProjectUiAuthoringPanel::set_value_json(std::string value_json) {
    if (selected_node_id_.empty()) {
        last_error_ = "Select a project UI node before editing its value.";
        return false;
    }
    if (!validate_any_json(value_json, "value")) {
        set_field_error(selected_node_id_, ProjectUiAuthoringFieldKind::value, std::string(last_error_));
        return false;
    }
    sync_selected_draft();
    drafts_[selected_node_id_].value_json = std::move(value_json);
    clear_field_error(selected_node_id_, ProjectUiAuthoringFieldKind::value);
    last_error_.clear();
    return true;
}

bool ProjectUiAuthoringPanel::set_component_ref(std::string component_ref) {
    if (selected_node_id_.empty()) {
        last_error_ = "Select a project UI node before editing its component reference.";
        return false;
    }
    sync_selected_draft();
    drafts_[selected_node_id_].component_ref = capped_string(std::move(component_ref), maximum_text_bytes);
    clear_field_error(selected_node_id_, ProjectUiAuthoringFieldKind::component_ref);
    last_error_.clear();
    return true;
}

bool ProjectUiAuthoringPanel::set_component_declaration_json(std::string component_id,
                                                            std::string component_json) {
    if (component_id.empty()) {
        last_error_ = "A component declaration ID is required before editing its declaration.";
        return false;
    }
    if (!validate_object_json(component_json, "componentDeclaration")) {
        set_field_error("component:" + component_id, ProjectUiAuthoringFieldKind::component_declaration,
                        std::string(last_error_));
        return false;
    }
    component_drafts_[component_id] = std::move(component_json);
    clear_field_error("component:" + component_id, ProjectUiAuthoringFieldKind::component_declaration);
    last_error_.clear();
    return true;
}

bool ProjectUiAuthoringPanel::set_design_tokens_json(std::string design_tokens_json) {
    if (!validate_object_json(design_tokens_json, "designTokens")) {
        set_design_tokens_error(std::string(last_error_));
        return false;
    }
    design_tokens_draft_ = std::move(design_tokens_json);
    clear_design_tokens_error();
    last_error_.clear();
    return true;
}

void ProjectUiAuthoringPanel::set_add_node_draft(const ProjectUiAuthoringNodeKind kind, std::string id,
                                                std::string role, std::string label, std::string parent_id,
                                                std::string action_id, std::string binding_json) {
    add_kind_ = kind;
    add_draft_ = {.id = capped_string(std::move(id), maximum_text_bytes),
                  .parent_id = capped_string(std::move(parent_id), maximum_text_bytes),
                  .role = capped_string(std::move(role), maximum_text_bytes),
                  .label = capped_string(std::move(label), maximum_text_bytes),
                  .action_id = capped_string(std::move(action_id), maximum_text_bytes),
                  .binding_json = std::move(binding_json)};
    if (add_draft_.role.empty()) add_draft_.role = project_ui_authoring_node_kind_name(kind);
}

void ProjectUiAuthoringPanel::set_add_node_component_ref(std::string component_ref) {
    add_draft_.component_ref = capped_string(std::move(component_ref), maximum_text_bytes);
}

bool ProjectUiAuthoringPanel::request_add_node() {
    auto draft = add_draft_;
    if (draft.id.empty()) {
        const auto payload = std::string(project_ui_authoring_node_kind_name(add_kind_)) + ":" +
            draft.parent_id + ":" + draft.role + ":" + draft.label + ":" + std::to_string(snapshot_.revision);
        draft.id = "ui.node." + hash_payload(payload).substr(0U, 12U);
        add_draft_.id = draft.id;
    }
    if (draft.role.empty()) {
        last_error_ = "A new project UI node needs a non-empty role.";
        return false;
    }
    if (std::ranges::any_of(view_.nodes, [&](const auto& node) { return node.id == draft.id; })) {
        last_error_ = "The new project UI node ID already exists.";
        return false;
    }
    if (!draft.parent_id.empty() &&
        std::ranges::find(view_.nodes, draft.parent_id, &ProjectUiAuthoringNode::id) == view_.nodes.end()) {
        last_error_ = "The new project UI node parent does not exist.";
        return false;
    }
    if (!validate_binding_json(draft.binding_json) ||
        !validate_object_json(draft.state_json, "state") ||
        !validate_object_json(draft.presentation_json, "presentation") ||
        !validate_any_json(draft.value_json, "value")) return false;
    ProjectUiAuthoringPanelRequest request{
        .kind = ProjectUiAuthoringPanelRequestKind::add_node,
        .base_revision = snapshot_.revision,
        .node_kind = add_kind_ == ProjectUiAuthoringNodeKind::unknown ? kind_from_string(draft.role) : add_kind_,
        .node_id = std::move(draft.id),
        .parent_id = std::move(draft.parent_id),
        .role = std::move(draft.role),
        .label = std::move(draft.label),
        .action_id = std::move(draft.action_id),
        .binding_json = std::move(draft.binding_json),
        .state_json = std::move(draft.state_json),
        .presentation_json = std::move(draft.presentation_json),
        .value_json = std::move(draft.value_json),
        .component_ref = std::move(draft.component_ref)};
    if (request.node_kind == ProjectUiAuthoringNodeKind::unknown) {
        last_error_ = "A new project UI node role must map to container, text, button, or property.";
        return false;
    }
    request.request_id = make_request_id(request);
    return queue_request(std::move(request));
}

bool ProjectUiAuthoringPanel::request_remove_node() {
    const auto* node = selected_node();
    if (node == nullptr) {
        last_error_ = "Select a project UI node before removing it.";
        return false;
    }
    ProjectUiAuthoringPanelRequest request{
        .kind = ProjectUiAuthoringPanelRequestKind::remove_node,
        .base_revision = snapshot_.revision,
        .node_id = node->id};
    request.request_id = make_request_id(request);
    return queue_request(std::move(request));
}

bool ProjectUiAuthoringPanel::request_update_node() {
    const auto* node = selected_node();
    if (node == nullptr) {
        last_error_ = "Select a project UI node before updating it.";
        return false;
    }
    sync_selected_draft();
    const auto draft = drafts_.at(node->id);
    if (draft.role.empty()) {
        last_error_ = "A project UI node role cannot be empty.";
        return false;
    }
    if (!validate_binding_json(draft.binding_json) ||
        !validate_object_json(draft.state_json, "state") ||
        !validate_object_json(draft.presentation_json, "presentation") ||
        !validate_any_json(draft.value_json, "value")) return false;
    ProjectUiAuthoringPanelRequest request{
        .kind = ProjectUiAuthoringPanelRequestKind::update_node,
        .base_revision = snapshot_.revision,
        .node_kind = kind_from_string(draft.role),
        .node_id = node->id,
        .parent_id = node->parent_id,
        .role = draft.role,
        .label = draft.label,
        .action_id = draft.action_id,
        .binding_json = draft.binding_json,
        .state_json = draft.state_json,
        .presentation_json = draft.presentation_json,
        .value_json = draft.value_json,
        .component_ref = draft.component_ref};
    request.request_id = make_request_id(request);
    return queue_request(std::move(request));
}

bool ProjectUiAuthoringPanel::request_reparent_node() {
    const auto* node = selected_node();
    if (node == nullptr) {
        last_error_ = "Select a project UI node before changing its parent.";
        return false;
    }
    sync_selected_draft();
    const auto parent_id = drafts_.at(node->id).parent_id;
    if (parent_id == node->id) {
        last_error_ = "A project UI node cannot parent itself.";
        return false;
    }
    if (!parent_id.empty() &&
        std::ranges::find(view_.nodes, parent_id, &ProjectUiAuthoringNode::id) == view_.nodes.end()) {
        last_error_ = "The requested project UI parent does not exist.";
        return false;
    }
    for (auto candidate = parent_id; !candidate.empty();) {
        if (candidate == node->id) {
            last_error_ = "A project UI node cannot be reparented below its own subtree.";
            return false;
        }
        const auto found = std::ranges::find(view_.nodes, candidate, &ProjectUiAuthoringNode::id);
        if (found == view_.nodes.end()) break;
        candidate = found->parent_id;
    }
    ProjectUiAuthoringPanelRequest request{
        .kind = ProjectUiAuthoringPanelRequestKind::reparent_node,
        .base_revision = snapshot_.revision,
        .node_id = node->id,
        .parent_id = parent_id};
    request.request_id = make_request_id(request);
    return queue_request(std::move(request));
}

bool ProjectUiAuthoringPanel::request_undo() {
    if (!snapshot_.can_undo) {
        last_error_ = "The project UI authoring session has no undo step.";
        return false;
    }
    ProjectUiAuthoringPanelRequest request{
        .kind = ProjectUiAuthoringPanelRequestKind::undo,
        .base_revision = snapshot_.revision};
    request.request_id = make_request_id(request);
    return queue_request(std::move(request));
}

bool ProjectUiAuthoringPanel::request_redo() {
    if (!snapshot_.can_redo) {
        last_error_ = "The project UI authoring session has no redo step.";
        return false;
    }
    ProjectUiAuthoringPanelRequest request{
        .kind = ProjectUiAuthoringPanelRequestKind::redo,
        .base_revision = snapshot_.revision};
    request.request_id = make_request_id(request);
    return queue_request(std::move(request));
}

bool ProjectUiAuthoringPanel::request_update_design_tokens() {
    if (!validate_object_json(design_tokens_draft_, "designTokens")) return false;
    ProjectUiAuthoringPanelRequest request{
        .kind = ProjectUiAuthoringPanelRequestKind::update_design_tokens,
        .base_revision = snapshot_.revision,
        .design_tokens_json = design_tokens_draft_};
    request.request_id = make_request_id(request);
    return queue_request(std::move(request));
}

bool ProjectUiAuthoringPanel::request_add_component_declaration(std::string component_id) {
    if (component_id.empty()) {
        last_error_ = "A component declaration ID is required.";
        return false;
    }
    if (std::ranges::find(view_.components, component_id,
                          &ProjectUiAuthoringComponentDeclaration::id) != view_.components.end()) {
        last_error_ = "The component declaration ID already exists.";
        return false;
    }
    const auto draft = component_drafts_.find(component_id);
    const auto json = draft == component_drafts_.end() ? std::string("{}") : draft->second;
    if (!validate_object_json(json, "componentDeclaration")) return false;
    ProjectUiAuthoringPanelRequest request{
        .kind = ProjectUiAuthoringPanelRequestKind::add_component_declaration,
        .base_revision = snapshot_.revision,
        .component_id = std::move(component_id),
        .component_json = json};
    request.request_id = make_request_id(request);
    return queue_request(std::move(request));
}

bool ProjectUiAuthoringPanel::request_update_component_declaration(std::string component_id) {
    if (component_id.empty() && !view_.components.empty()) component_id = view_.components.front().id;
    const auto found = std::ranges::find(view_.components, component_id,
                                         &ProjectUiAuthoringComponentDeclaration::id);
    if (found == view_.components.end()) {
        last_error_ = "The requested component declaration does not exist.";
        return false;
    }
    const auto draft = component_drafts_.find(component_id);
    const auto json = draft == component_drafts_.end() ? found->component_json : draft->second;
    if (!validate_object_json(json, "componentDeclaration")) return false;
    ProjectUiAuthoringPanelRequest request{
        .kind = ProjectUiAuthoringPanelRequestKind::update_component_declaration,
        .base_revision = snapshot_.revision,
        .component_id = component_id,
        .component_json = json};
    request.request_id = make_request_id(request);
    return queue_request(std::move(request));
}

bool ProjectUiAuthoringPanel::request_remove_component_declaration(std::string component_id) {
    if (component_id.empty() && !view_.components.empty()) component_id = view_.components.front().id;
    if (std::ranges::find(view_.components, component_id,
                          &ProjectUiAuthoringComponentDeclaration::id) == view_.components.end()) {
        last_error_ = "The requested component declaration does not exist.";
        return false;
    }
    ProjectUiAuthoringPanelRequest request{
        .kind = ProjectUiAuthoringPanelRequestKind::remove_component_declaration,
        .base_revision = snapshot_.revision,
        .component_id = std::move(component_id)};
    request.request_id = make_request_id(request);
    return queue_request(std::move(request));
}

std::string ProjectUiAuthoringPanel::semantic_snapshot_json() const {
    Json result{{"schemaVersion", "noemancer.project-ui-authoring/0.1"},
                {"documentSchema", view_.schema_version}, {"documentId", view_.document_id},
                {"valid", view_.valid}, {"code", view_.valid ? "ok" : "ui.authoring-invalid-document"},
                {"sourceRevision", snapshot_.revision}, {"sourceFingerprint", snapshot_.fingerprint},
                {"selectedNodeId", selected_node_id_}, {"canUndo", snapshot_.can_undo},
                {"canRedo", snapshot_.can_redo}};
    result["roots"] = view_.root_ids;
    const auto parsed_tokens = Json::parse(design_tokens_draft_, nullptr, false);
    result["designTokens"] = parsed_tokens.is_discarded() ? Json(nullptr) : parsed_tokens;
    result["designTokensJson"] = design_tokens_draft_;
    result["designTokensStatus"] = field_state_json(
        [&]() {
            auto status = view_.design_tokens_status;
            status.dirty = design_tokens_draft_ != view_.design_tokens_json;
            status.pending = pending_request_ && pending_request_->kind ==
                ProjectUiAuthoringPanelRequestKind::update_design_tokens;
            status.conflict = conflicted_fields_.contains(field_key({}, ProjectUiAuthoringFieldKind::design_tokens));
            status.error = design_tokens_error_;
            if (!status.error.empty()) status.valid = false;
            return status;
        }());
    result["components"] = Json::array();
    for (const auto& component : view_.components) {
        const auto draft = component_drafts_.find(component.id);
        const auto json = draft == component_drafts_.end() ? component.component_json : draft->second;
        const auto parsed = Json::parse(json, nullptr, false);
        auto status = component.status;
        status.dirty = draft != component_drafts_.end() && json != component.component_json;
        status.pending = pending_request_ && pending_request_->component_id == component.id;
        status.conflict = conflicted_fields_.contains("component:" + component.id);
        if (const auto error = field_errors_.find(field_key("component:" + component.id,
                                                             ProjectUiAuthoringFieldKind::component_declaration));
            error != field_errors_.end()) {
            status.valid = false;
            status.error = error->second;
        }
        result["components"].push_back({{"id", component.id}, {"rootNodeId", component.root_node_id},
                                         {"label", component.label}, {"component", parsed.is_discarded() ? Json(nullptr) : parsed},
                                         {"componentJson", json}, {"status", field_state_json(status)}});
    }
    result["nodes"] = Json::array();
    for (const auto& node : view_.nodes) {
        Json binding = Json::object();
        const auto parsed_binding = Json::parse(node.binding_json, nullptr, false);
        if (!parsed_binding.is_discarded()) binding = parsed_binding;
        const auto parsed_state = Json::parse(node.state_json, nullptr, false);
        const auto parsed_presentation = Json::parse(node.presentation_json, nullptr, false);
        const auto parsed_value = Json::parse(node.value_json, nullptr, false);
        Json projected{{"id", node.id},
                       {"parentId", node.parent_id.empty() ? Json(nullptr) : Json(node.parent_id)},
                       {"role", node.role}, {"kind", project_ui_authoring_node_kind_name(node.kind)},
                       {"label", node.label}, {"actionId", node.action_id}, {"binding", binding},
                       {"bindingJson", node.binding_json}, {"bindingValid", node.binding_valid},
                       {"state", parsed_state.is_discarded() ? Json(nullptr) : parsed_state},
                       {"stateJson", node.state_json}, {"stateKind", json_kind_name(node.state_kind)},
                       {"stateValid", node.state_valid},
                       {"presentation", parsed_presentation.is_discarded() ? Json(nullptr) : parsed_presentation},
                       {"presentationJson", node.presentation_json},
                       {"presentationKind", json_kind_name(node.presentation_kind)},
                       {"presentationValid", node.presentation_valid},
                       {"value", parsed_value.is_discarded() ? Json(nullptr) : parsed_value},
                       {"valueJson", node.value_json}, {"valueKind", json_kind_name(node.value_kind)},
                       {"valueValid", node.value_valid}, {"componentRef", node.component_ref},
                       {"depth", node.depth}, {"childCount", node.child_count},
                       {"selected", node.id == selected_node_id_}};
        projected["fields"] = Json::array();
        for (const auto field : {ProjectUiAuthoringFieldKind::label, ProjectUiAuthoringFieldKind::role,
                                 ProjectUiAuthoringFieldKind::parent, ProjectUiAuthoringFieldKind::action_id,
                                 ProjectUiAuthoringFieldKind::binding, ProjectUiAuthoringFieldKind::state,
                                 ProjectUiAuthoringFieldKind::presentation, ProjectUiAuthoringFieldKind::value,
                                 ProjectUiAuthoringFieldKind::component_ref,
                                 ProjectUiAuthoringFieldKind::component_declaration})
            projected["fields"].push_back(field_state_json(field_state(node, field)));
        result["nodes"].push_back(std::move(projected));
    }
    result["drafts"] = Json::array();
    for (const auto& [node_id, draft] : drafts_)
        result["drafts"].push_back({{"nodeId", node_id}, {"parentId", draft.parent_id}, {"role", draft.role},
                                    {"label", draft.label}, {"actionId", draft.action_id},
                                    {"bindingJson", draft.binding_json}, {"stateJson", draft.state_json},
                                    {"presentationJson", draft.presentation_json}, {"valueJson", draft.value_json},
                                    {"componentRef", draft.component_ref}});
    result["addDraft"] = {{"id", add_draft_.id}, {"parentId", add_draft_.parent_id},
                           {"kind", project_ui_authoring_node_kind_name(add_kind_)},
                           {"role", add_draft_.role}, {"label", add_draft_.label},
                           {"actionId", add_draft_.action_id}, {"bindingJson", add_draft_.binding_json},
                           {"stateJson", add_draft_.state_json},
                           {"presentationJson", add_draft_.presentation_json},
                           {"valueJson", add_draft_.value_json}, {"componentRef", add_draft_.component_ref}};
    result["diagnostics"] = Json::array();
    for (const auto& diagnostic : view_.diagnostics)
        result["diagnostics"].push_back({{"code", diagnostic.code}, {"path", diagnostic.path},
                                         {"nodeId", diagnostic.node_id}, {"detail", diagnostic.detail}});
    if (pending_request_) {
        result["pendingRequest"] = {{"kind", project_ui_authoring_request_kind_name(pending_request_->kind)},
                                     {"requestId", pending_request_->request_id},
                                     {"baseRevision", pending_request_->base_revision},
                                     {"nodeId", pending_request_->node_id}};
    } else {
        result["pendingRequest"] = nullptr;
    }
    if (!last_error_.empty()) result["lastError"] = last_error_;
    return result.dump();
}

void ProjectUiAuthoringPanel::render() {
    ImGui::SetNextWindowSize({1040.0F, 780.0F}, ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Project UI Authoring")) {
        ImGui::End();
        return;
    }

    // The header is intentionally a compact, machine-legible status line.  A
    // human can see the source revision and whether an edit is still local;
    // an Agent sees the same facts in semantic_snapshot_json().
    std::size_t dirty_fields{};
    std::size_t invalid_fields{};
    for (const auto& node : view_.nodes) {
        for (const auto field : {ProjectUiAuthoringFieldKind::label, ProjectUiAuthoringFieldKind::role,
                                 ProjectUiAuthoringFieldKind::parent, ProjectUiAuthoringFieldKind::action_id,
                                 ProjectUiAuthoringFieldKind::binding, ProjectUiAuthoringFieldKind::state,
                                 ProjectUiAuthoringFieldKind::presentation, ProjectUiAuthoringFieldKind::value,
                                 ProjectUiAuthoringFieldKind::component_ref}) {
            const auto status = field_state(node, field);
            dirty_fields += status.dirty ? 1U : 0U;
            invalid_fields += status.valid ? 0U : 1U;
        }
    }
    dirty_fields += design_tokens_draft_ != view_.design_tokens_json ? 1U : 0U;
    ImGui::Text("Project UI  |  revision %llu  |  %zu nodes  |  %zu components",
                static_cast<unsigned long long>(snapshot_.revision), view_.nodes.size(), view_.components.size());
    ImGui::SameLine();
    ImGui::TextColored(view_.valid ? ImVec4{0.42F, 0.86F, 0.58F, 1.0F} : ImVec4{1.0F, 0.42F, 0.34F, 1.0F},
                       "%s", view_.valid ? "valid" : "invalid");
    ImGui::SameLine();
    ImGui::TextDisabled("dirty %zu  invalid %zu  undo %s  redo %s", dirty_fields, invalid_fields,
                        snapshot_.can_undo ? "ready" : "empty", snapshot_.can_redo ? "ready" : "empty");
    ImGui::Separator();

    if (ImGui::BeginTable("##project-ui-authoring-columns", 2,
                          ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV |
                              ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("Hierarchy", ImGuiTableColumnFlags_WidthFixed, 330.0F);
        ImGui::TableSetupColumn("Inspector", ImGuiTableColumnFlags_WidthStretch);

        // Left column: the source hierarchy is the primary navigation model.
        // It is rendered as a real tree, not a string with spaces, so the same
        // parent/child relationship is visible to a person and unambiguous in
        // the semantic projection.
        ImGui::TableNextColumn();
        ImGui::SeparatorText("Node tree");
        if (ImGui::BeginChild("##project-ui-node-tree", ImVec2(0.0F, 300.0F), ImGuiChildFlags_Borders)) {
            std::unordered_map<std::string, std::vector<const ProjectUiAuthoringNode*>> children;
            std::vector<const ProjectUiAuthoringNode*> roots;
            for (const auto& node : view_.nodes) {
                if (node.parent_id.empty()) roots.push_back(&node);
                else children[node.parent_id].push_back(&node);
            }
            std::unordered_set<std::string> visited;
            const auto draw_tree_node = [&](const auto& self, const ProjectUiAuthoringNode& node) -> void {
                if (!visited.insert(node.id).second) return;
                const auto child = children.find(node.id);
                const bool has_children = child != children.end() && !child->second.empty();
                ImGui::PushID(node.id.c_str());
                ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_SpanAvailWidth;
                if (node.id == selected_node_id_) flags |= ImGuiTreeNodeFlags_Selected;
                if (!has_children) flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
                std::string title = node.label.empty() ? node.id : node.label;
                title += "  [";
                title += node.role;
                title += "]";
                const bool open = ImGui::TreeNodeEx("##node", flags, "%s", title.c_str());
                if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) static_cast<void>(select_node(node.id));
                const auto state = field_state(node, ProjectUiAuthoringFieldKind::state);
                const auto presentation = field_state(node, ProjectUiAuthoringFieldKind::presentation);
                const auto component = field_state(node, ProjectUiAuthoringFieldKind::component_ref);
                if (!state.valid || !presentation.valid || !component.valid) {
                    ImGui::SameLine();
                    ImGui::TextColored({1.0F, 0.42F, 0.34F, 1.0F}, "!");
                } else if (state.conflict || presentation.conflict || component.conflict) {
                    ImGui::SameLine();
                    ImGui::TextColored({1.0F, 0.72F, 0.24F, 1.0F}, "conflict");
                } else if (state.dirty || presentation.dirty || component.dirty) {
                    ImGui::SameLine();
                    ImGui::TextColored({0.45F, 0.88F, 0.62F, 1.0F}, "dirty");
                }
                ImGui::SameLine();
                ImGui::TextDisabled("%s", node.id.c_str());
                if (has_children && open) {
                    for (const auto* child_node : child->second) self(self, *child_node);
                    ImGui::TreePop();
                }
                ImGui::PopID();
            };
            for (const auto* node : roots) draw_tree_node(draw_tree_node, *node);
            // Invalid/orphaned sources remain inspectable rather than silently
            // disappearing from the authoring surface.
            for (const auto& node : view_.nodes)
                if (!visited.contains(node.id)) draw_tree_node(draw_tree_node, node);
            if (view_.nodes.empty()) ImGui::TextDisabled("No authored nodes.");
        }
        ImGui::EndChild();

        ImGui::SeparatorText("Create node");
        if (ImGui::BeginCombo("Kind", project_ui_authoring_node_kind_name(add_kind_))) {
            for (const auto kind : {ProjectUiAuthoringNodeKind::container, ProjectUiAuthoringNodeKind::text,
                                    ProjectUiAuthoringNodeKind::button, ProjectUiAuthoringNodeKind::property}) {
                const bool selected_kind = add_kind_ == kind;
                if (ImGui::Selectable(project_ui_authoring_node_kind_name(kind), selected_kind)) {
                    add_kind_ = kind;
                    if (add_draft_.role.empty() || kind_from_string(add_draft_.role) == ProjectUiAuthoringNodeKind::unknown)
                        add_draft_.role = project_ui_authoring_node_kind_name(kind);
                }
                if (selected_kind) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
        draw_text_input("ID", add_draft_.id);
        draw_text_input("Role", add_draft_.role);
        draw_text_input("Label", add_draft_.label);
        draw_text_input("Parent ID", add_draft_.parent_id);
        draw_text_input("Action ID", add_draft_.action_id);
        draw_multiline_text("Binding##new-node", add_draft_.binding_json, ImVec2(-1.0F, 54.0F));
        if (ImGui::BeginCombo("Component##new-node",
                              add_draft_.component_ref.empty() ? "<none>" : add_draft_.component_ref.c_str())) {
            if (ImGui::Selectable("<none>", add_draft_.component_ref.empty())) add_draft_.component_ref.clear();
            for (const auto& component : view_.components) {
                const bool selected = add_draft_.component_ref == component.id;
                const auto title = component.label.empty() ? component.id : component.id + " / " + component.label;
                if (ImGui::Selectable(title.c_str(), selected)) add_draft_.component_ref = component.id;
                if (selected) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
        if (ImGui::Button("Add node")) static_cast<void>(request_add_node());
        ImGui::SameLine();
        ImGui::BeginDisabled(!snapshot_.can_undo || pending_request_.has_value());
        if (ImGui::Button("Undo")) static_cast<void>(request_undo());
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(!snapshot_.can_redo || pending_request_.has_value());
        if (ImGui::Button("Redo")) static_cast<void>(request_redo());
        ImGui::EndDisabled();

        // Right column: a compact inspector deliberately follows the source
        // language.  JSON editors are kept under stateful sections and are
        // accompanied by parsed summaries, so arbitrary user-authored fields
        // remain lossless without turning the whole panel into a JSON dump.
        ImGui::TableNextColumn();
        const auto* node = selected_node();
        if (node == nullptr) {
            ImGui::SeparatorText("Inspector");
            ImGui::TextDisabled("Select a node from the tree to inspect it.");
        } else {
            sync_selected_draft();
            auto& draft = drafts_.at(node->id);
            ImGui::SeparatorText("Selected node");
            ImGui::Text("%s  /  %s  /  depth %zu  /  %zu children", node->id.c_str(),
                        project_ui_authoring_node_kind_name(node->kind), node->depth, node->child_count);

            const auto edit_scalar = [&](const char* caption, std::string& value,
                                         const ProjectUiAuthoringFieldKind field, const auto& setter) {
                ImGui::TextUnformatted(caption);
                draw_field_status(field_state(*node, field));
                std::string candidate = value;
                const std::string id = std::string("##selected-") + project_ui_authoring_field_kind_name(field);
                if (draw_text_input(id.c_str(), candidate)) {
                    if (!setter(candidate)) {
                        value = candidate;
                        if (!last_error_.empty()) set_field_error(node->id, field, last_error_);
                    }
                }
            };
            const auto edit_json = [&](const char* caption, std::string& value,
                                       const ProjectUiAuthoringFieldKind field, const auto& setter,
                                       const ImVec2 size = {0.0F, 72.0F}) {
                ImGui::TextUnformatted(caption);
                draw_field_status(field_state(*node, field));
                std::string candidate = value;
                const std::string id = std::string("##selected-") + project_ui_authoring_field_kind_name(field);
                if (draw_multiline_text(id.c_str(), candidate, size)) {
                    if (!setter(candidate)) {
                        value = candidate;
                        if (!last_error_.empty()) set_field_error(node->id, field, last_error_);
                    }
                }
                if (ImGui::TreeNodeEx((std::string("Parsed ") + caption).c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
                    draw_readonly_json_summary(caption, value);
                    ImGui::TreePop();
                }
            };

            if (ImGui::CollapsingHeader("Identity", ImGuiTreeNodeFlags_DefaultOpen)) {
                edit_scalar("Label", draft.label, ProjectUiAuthoringFieldKind::label,
                            [&](const std::string& value) { return set_label(value); });
                edit_scalar("Role", draft.role, ProjectUiAuthoringFieldKind::role,
                            [&](const std::string& value) { return set_role(value); });
                edit_scalar("Parent ID", draft.parent_id, ProjectUiAuthoringFieldKind::parent,
                            [&](const std::string& value) { return set_parent(value); });
                edit_scalar("Action ID", draft.action_id, ProjectUiAuthoringFieldKind::action_id,
                            [&](const std::string& value) { return set_action_id(value); });

                ImGui::TextUnformatted("Component declaration");
                draw_field_status(field_state(*node, ProjectUiAuthoringFieldKind::component_ref));
                std::string component_preview = draft.component_ref.empty() ? "<none>" : draft.component_ref;
                if (!draft.component_ref.empty()) {
                    const auto found = std::ranges::find(view_.components, draft.component_ref,
                                                         &ProjectUiAuthoringComponentDeclaration::id);
                    if (found != view_.components.end() && !found->label.empty())
                        component_preview += " / " + found->label;
                }
                if (ImGui::BeginCombo("##selected-component", component_preview.c_str())) {
                    if (ImGui::Selectable("<none>", draft.component_ref.empty()))
                        static_cast<void>(set_component_ref({}));
                    for (const auto& component : view_.components) {
                        const bool selected = draft.component_ref == component.id;
                        const auto title = component.label.empty() ? component.id : component.id + " / " + component.label;
                        if (ImGui::Selectable(title.c_str(), selected)) static_cast<void>(set_component_ref(component.id));
                        if (selected) ImGui::SetItemDefaultFocus();
                    }
                    ImGui::EndCombo();
                }
            }

            if (ImGui::CollapsingHeader("State and presentation", ImGuiTreeNodeFlags_DefaultOpen)) {
                edit_json("Binding", draft.binding_json, ProjectUiAuthoringFieldKind::binding,
                          [&](const std::string& value) { return set_binding_json(value); }, ImVec2(0.0F, 62.0F));
                edit_json("State", draft.state_json, ProjectUiAuthoringFieldKind::state,
                          [&](const std::string& value) { return set_state_json(value); });
                edit_json("Presentation", draft.presentation_json, ProjectUiAuthoringFieldKind::presentation,
                          [&](const std::string& value) { return set_presentation_json(value); });
                edit_json("Value", draft.value_json, ProjectUiAuthoringFieldKind::value,
                          [&](const std::string& value) { return set_value_json(value); }, ImVec2(0.0F, 54.0F));
            }

            ImGui::BeginDisabled(pending_request_.has_value());
            if (ImGui::Button("Commit node")) static_cast<void>(request_update_node());
            ImGui::SameLine();
            if (ImGui::Button("Reparent")) static_cast<void>(request_reparent_node());
            ImGui::SameLine();
            if (ImGui::Button("Delete subtree")) static_cast<void>(request_remove_node());
            ImGui::EndDisabled();
        }

        if (ImGui::CollapsingHeader("Component declarations", ImGuiTreeNodeFlags_DefaultOpen)) {
            if (view_.components.empty()) {
                ImGui::TextDisabled("No reusable declarations. Add one below.");
            } else {
                if (selected_component_id_.empty() ||
                    std::ranges::find(view_.components, selected_component_id_,
                                      &ProjectUiAuthoringComponentDeclaration::id) == view_.components.end())
                    selected_component_id_ = view_.components.front().id;
                const auto selected_component = std::ranges::find(view_.components, selected_component_id_,
                                                                   &ProjectUiAuthoringComponentDeclaration::id);
                const auto component_preview = selected_component == view_.components.end()
                    ? std::string("<none>")
                    : (selected_component->label.empty() ? selected_component->id
                                                          : selected_component->id + " / " + selected_component->label);
                if (ImGui::BeginCombo("Declaration", component_preview.c_str())) {
                    for (const auto& component : view_.components) {
                        const bool selected = component.id == selected_component_id_;
                        const auto title = component.label.empty() ? component.id : component.id + " / " + component.label;
                        if (ImGui::Selectable(title.c_str(), selected)) selected_component_id_ = component.id;
                        if (selected) ImGui::SetItemDefaultFocus();
                    }
                    ImGui::EndCombo();
                }
                if (selected_component != view_.components.end()) {
                    const auto draft = component_drafts_.find(selected_component_id_);
                    std::string source = draft == component_drafts_.end() ? selected_component->component_json : draft->second;
                    auto parsed = Json::parse(source, nullptr, false);
                    if (parsed.is_discarded() || !parsed.is_object()) {
                        draw_field_status({.field = ProjectUiAuthoringFieldKind::component_declaration,
                                           .valid = false, .dirty = true,
                                           .error = "Component declaration must be a JSON object."});
                    } else {
                        ImGui::Text("id: %s  |  root: %s  |  %zu authored fields", selected_component->id.c_str(),
                                    string_member(parsed, "rootNodeId", string_member(parsed, "root", "<none>")).c_str(),
                                    parsed.size());
                        const auto label_key = parsed.contains("label") ? "label" : "name";
                        std::string label = string_member(parsed, label_key);
                        ImGui::TextUnformatted("Label");
                        const std::string label_id = "##component-label-" + selected_component_id_;
                        if (draw_text_input(label_id.c_str(), label)) {
                            parsed[label_key] = label;
                            const auto candidate = parsed.dump();
                            if (!set_component_declaration_json(selected_component_id_, candidate))
                                component_drafts_[selected_component_id_] = candidate;
                        }
                        const auto root_key = parsed.contains("rootNodeId") ? "rootNodeId" : "root";
                        std::string root = string_member(parsed, root_key);
                        ImGui::TextUnformatted("Root node ID");
                        const std::string root_id = "##component-root-" + selected_component_id_;
                        if (draw_text_input(root_id.c_str(), root)) {
                            parsed[root_key] = root;
                            const auto candidate = parsed.dump();
                            if (!set_component_declaration_json(selected_component_id_, candidate))
                                component_drafts_[selected_component_id_] = candidate;
                        }
                        if (ImGui::TreeNode("Advanced declaration source")) {
                            const auto current_draft = component_drafts_.find(selected_component_id_);
                            std::string editable = current_draft == component_drafts_.end()
                                ? selected_component->component_json : current_draft->second;
                            if (draw_multiline_text(("##component-source-" + selected_component_id_).c_str(), editable,
                                                    ImVec2(0.0F, 92.0F), 8192U))
                                static_cast<void>(set_component_declaration_json(selected_component_id_, editable));
                            ImGui::TreePop();
                        }
                        const auto status = selected_component->status;
                        auto effective_status = status;
                        effective_status.dirty = component_drafts_.contains(selected_component_id_) &&
                            component_drafts_.at(selected_component_id_) != selected_component->component_json;
                        effective_status.pending = pending_request_ && pending_request_->component_id == selected_component_id_;
                        effective_status.conflict = conflicted_fields_.contains("component:" + selected_component_id_);
                        if (const auto error = field_errors_.find(field_key("component:" + selected_component_id_,
                                                                             ProjectUiAuthoringFieldKind::component_declaration));
                            error != field_errors_.end()) {
                            effective_status.valid = false;
                            effective_status.error = error->second;
                        }
                        draw_field_status(effective_status);
                        ImGui::BeginDisabled(pending_request_.has_value());
                        if (ImGui::Button("Commit declaration"))
                            static_cast<void>(request_update_component_declaration(selected_component_id_));
                        ImGui::SameLine();
                        if (ImGui::Button("Remove declaration"))
                            static_cast<void>(request_remove_component_declaration(selected_component_id_));
                        ImGui::EndDisabled();
                    }
                }
            }
            draw_text_input("New declaration ID", new_component_id_);
            if (ImGui::Button("Add declaration"))
                static_cast<void>(request_add_component_declaration(new_component_id_));
        }

        if (ImGui::CollapsingHeader("Design tokens", ImGuiTreeNodeFlags_DefaultOpen)) {
            const auto token_status = [&] {
                auto status = view_.design_tokens_status;
                status.dirty = design_tokens_draft_ != view_.design_tokens_json;
                status.pending = pending_request_ && pending_request_->kind == ProjectUiAuthoringPanelRequestKind::update_design_tokens;
                status.conflict = conflicted_fields_.contains(field_key({}, ProjectUiAuthoringFieldKind::design_tokens));
                status.error = design_tokens_error_;
                status.valid = status.error.empty() && view_.design_tokens_valid;
                return status;
            }();
            draw_field_status(token_status);
            draw_readonly_json_summary("Current token values", design_tokens_draft_);
            if (ImGui::TreeNode("Edit token source")) {
                std::string candidate = design_tokens_draft_;
                if (draw_multiline_text("##design-tokens-source", candidate, ImVec2(0.0F, 96.0F), 8192U)) {
                    if (!set_design_tokens_json(candidate)) design_tokens_draft_ = candidate;
                }
                if (ImGui::Button("Commit design tokens")) static_cast<void>(request_update_design_tokens());
                ImGui::TreePop();
            }
        }

        if (ImGui::CollapsingHeader("Diagnostics", view_.diagnostics.empty() ? 0 : ImGuiTreeNodeFlags_DefaultOpen)) {
            if (view_.diagnostics.empty()) ImGui::TextColored({0.42F, 0.86F, 0.58F, 1.0F}, "No document diagnostics.");
            for (const auto& diagnostic : view_.diagnostics) {
                ImGui::PushID(diagnostic.path.c_str());
                ImGui::TextColored({1.0F, 0.58F, 0.28F, 1.0F}, "%s", diagnostic.code.c_str());
                ImGui::SameLine();
                ImGui::TextDisabled("%s", diagnostic.path.c_str());
                ImGui::TextWrapped("%s", diagnostic.detail.c_str());
                ImGui::PopID();
            }
        }

        ImGui::SeparatorText("Commit status");
        if (pending_request_) {
            ImGui::TextColored({0.42F, 0.78F, 1.0F, 1.0F}, "Pending %s",
                               project_ui_authoring_request_kind_name(pending_request_->kind));
            ImGui::SameLine();
            ImGui::TextDisabled("request %s @ revision %llu", pending_request_->request_id.c_str(),
                                static_cast<unsigned long long>(pending_request_->base_revision));
        } else if (dirty_fields != 0U) {
            ImGui::TextColored({0.45F, 0.88F, 0.62F, 1.0F}, "%zu local draft fields; commit is explicit.", dirty_fields);
        } else {
            ImGui::TextDisabled("No local drafts. Source is synchronized at revision %llu.",
                                static_cast<unsigned long long>(snapshot_.revision));
        }
        if (!last_error_.empty()) ImGui::TextColored({1.0F, 0.35F, 0.30F, 1.0F}, "%s", last_error_.c_str());
        ImGui::EndTable();
    }
    ImGui::End();
}

const ProjectUiAuthoringNode* ProjectUiAuthoringPanel::selected_node() const noexcept {
    const auto found = std::ranges::find(view_.nodes, selected_node_id_, &ProjectUiAuthoringNode::id);
    return found == view_.nodes.end() ? nullptr : &*found;
}

std::string ProjectUiAuthoringPanel::field_key(const std::string_view node_id,
                                               const ProjectUiAuthoringFieldKind field) const {
    return std::string(node_id) + ":" + project_ui_authoring_field_kind_name(field);
}

bool ProjectUiAuthoringPanel::field_is_dirty(const ProjectUiAuthoringNode& node,
                                             const ProjectUiAuthoringFieldKind field) const {
    const auto found = drafts_.find(node.id);
    if (found == drafts_.end()) return false;
    const auto& draft = found->second;
    switch (field) {
    case ProjectUiAuthoringFieldKind::label: return draft.label != node.label;
    case ProjectUiAuthoringFieldKind::role: return draft.role != node.role;
    case ProjectUiAuthoringFieldKind::parent: return draft.parent_id != node.parent_id;
    case ProjectUiAuthoringFieldKind::action_id: return draft.action_id != node.action_id;
    case ProjectUiAuthoringFieldKind::binding: return draft.binding_json != node.binding_json;
    case ProjectUiAuthoringFieldKind::state: return draft.state_json != node.state_json;
    case ProjectUiAuthoringFieldKind::presentation: return draft.presentation_json != node.presentation_json;
    case ProjectUiAuthoringFieldKind::value: return draft.value_json != node.value_json;
    case ProjectUiAuthoringFieldKind::component_ref: return draft.component_ref != node.component_ref;
    case ProjectUiAuthoringFieldKind::component_declaration: return false;
    case ProjectUiAuthoringFieldKind::design_tokens: return false;
    }
    return false;
}

bool ProjectUiAuthoringPanel::field_is_pending(const ProjectUiAuthoringNode& node,
                                               const ProjectUiAuthoringFieldKind field) const {
    if (!pending_request_) return false;
    const auto& request = *pending_request_;
    if (request.node_id != node.id) return false;
    switch (field) {
    case ProjectUiAuthoringFieldKind::parent: return request.kind == ProjectUiAuthoringPanelRequestKind::reparent_node;
    case ProjectUiAuthoringFieldKind::component_ref:
        return request.kind == ProjectUiAuthoringPanelRequestKind::update_node;
    case ProjectUiAuthoringFieldKind::component_declaration: return false;
    case ProjectUiAuthoringFieldKind::design_tokens: return false;
    default:
        return request.kind == ProjectUiAuthoringPanelRequestKind::update_node ||
            request.kind == ProjectUiAuthoringPanelRequestKind::remove_node;
    }
}

bool ProjectUiAuthoringPanel::field_is_conflicted(const std::string_view node_id,
                                                  const ProjectUiAuthoringFieldKind field) const {
    return conflicted_fields_.contains(field_key(node_id, field)) ||
        conflicted_fields_.contains(std::string(node_id) + ":*");
}

ProjectUiAuthoringFieldState ProjectUiAuthoringPanel::field_state(
    const ProjectUiAuthoringNode& node, const ProjectUiAuthoringFieldKind field) const {
    ProjectUiAuthoringFieldState status{.field = field};
    status.disabled = node.id != selected_node_id_ ||
        (field == ProjectUiAuthoringFieldKind::parent && node.parent_id.empty()) ||
        (field == ProjectUiAuthoringFieldKind::component_ref && view_.components.empty());
    status.dirty = field_is_dirty(node, field);
    status.pending = field_is_pending(node, field);
    status.conflict = field_is_conflicted(node.id, field);
    if (field == ProjectUiAuthoringFieldKind::binding) status.valid = node.binding_valid;
    if (field == ProjectUiAuthoringFieldKind::state) status.valid = node.state_valid;
    if (field == ProjectUiAuthoringFieldKind::presentation) status.valid = node.presentation_valid;
    if (field == ProjectUiAuthoringFieldKind::value) status.valid = node.value_valid;
    const auto error = field_errors_.find(field_key(node.id, field));
    if (error != field_errors_.end()) {
        status.valid = false;
        status.error = error->second;
    }
    if (status.error.empty()) {
        for (const auto& diagnostic : view_.diagnostics) {
            if (diagnostic.node_id != node.id) continue;
            const auto field_name = project_ui_authoring_field_kind_name(field);
            if (diagnostic.path.find(field_name) == std::string::npos &&
                diagnostic.code.find(field_name) == std::string::npos) continue;
            status.valid = false;
            status.error = diagnostic.detail;
            break;
        }
    }
    return status;
}

void ProjectUiAuthoringPanel::mark_conflict_for_request(const ProjectUiAuthoringPanelRequest& request) {
    if (request.kind == ProjectUiAuthoringPanelRequestKind::update_design_tokens) {
        conflicted_fields_.insert(field_key({}, ProjectUiAuthoringFieldKind::design_tokens));
        return;
    }
    if (request.kind == ProjectUiAuthoringPanelRequestKind::add_component_declaration ||
        request.kind == ProjectUiAuthoringPanelRequestKind::update_component_declaration ||
        request.kind == ProjectUiAuthoringPanelRequestKind::remove_component_declaration) {
        conflicted_fields_.insert("component:" + request.component_id);
        return;
    }
    if (request.node_id.empty()) return;
    if (request.kind == ProjectUiAuthoringPanelRequestKind::remove_node) {
        conflicted_fields_.insert(request.node_id + ":*");
        return;
    }
    if (request.kind == ProjectUiAuthoringPanelRequestKind::reparent_node)
        conflicted_fields_.insert(field_key(request.node_id, ProjectUiAuthoringFieldKind::parent));
    else if (request.kind == ProjectUiAuthoringPanelRequestKind::update_node)
        for (const auto field : {ProjectUiAuthoringFieldKind::label, ProjectUiAuthoringFieldKind::role,
                                 ProjectUiAuthoringFieldKind::action_id, ProjectUiAuthoringFieldKind::binding,
                                 ProjectUiAuthoringFieldKind::state, ProjectUiAuthoringFieldKind::presentation,
                                 ProjectUiAuthoringFieldKind::value, ProjectUiAuthoringFieldKind::component_ref})
            conflicted_fields_.insert(field_key(request.node_id, field));
}

void ProjectUiAuthoringPanel::mark_dirty_conflicts(const std::uint64_t next_revision) {
    static_cast<void>(next_revision);
    for (const auto& node : view_.nodes)
        for (const auto field : {ProjectUiAuthoringFieldKind::label, ProjectUiAuthoringFieldKind::role,
                                 ProjectUiAuthoringFieldKind::parent, ProjectUiAuthoringFieldKind::action_id,
                                 ProjectUiAuthoringFieldKind::binding, ProjectUiAuthoringFieldKind::state,
                                 ProjectUiAuthoringFieldKind::presentation, ProjectUiAuthoringFieldKind::value,
                                 ProjectUiAuthoringFieldKind::component_ref})
            if (field_is_dirty(node, field)) conflicted_fields_.insert(field_key(node.id, field));
    for (const auto& [component_id, draft] : component_drafts_) {
        const auto found = std::ranges::find(view_.components, component_id,
                                             &ProjectUiAuthoringComponentDeclaration::id);
        if (found != view_.components.end() && draft != found->component_json)
            conflicted_fields_.insert("component:" + component_id);
    }
    if (design_tokens_draft_ != view_.design_tokens_json)
        conflicted_fields_.insert(field_key({}, ProjectUiAuthoringFieldKind::design_tokens));
}

void ProjectUiAuthoringPanel::set_field_error(const std::string_view node_id,
                                              const ProjectUiAuthoringFieldKind field,
                                              std::string error) {
    field_errors_[field_key(node_id, field)] = std::move(error);
}

void ProjectUiAuthoringPanel::clear_field_error(const std::string_view node_id,
                                                const ProjectUiAuthoringFieldKind field) {
    field_errors_.erase(field_key(node_id, field));
}

void ProjectUiAuthoringPanel::set_design_tokens_error(std::string error) {
    design_tokens_error_ = std::move(error);
}

void ProjectUiAuthoringPanel::clear_design_tokens_error() {
    design_tokens_error_.clear();
}

bool ProjectUiAuthoringPanel::queue_request(ProjectUiAuthoringPanelRequest request) {
    if (pending_request_) {
        last_error_ = "Consume the pending project UI request before issuing another request.";
        return false;
    }
    if (!view_.valid && request.kind != ProjectUiAuthoringPanelRequestKind::undo &&
        request.kind != ProjectUiAuthoringPanelRequestKind::redo) {
        last_error_ = "The project UI document must be valid before issuing an authoring request.";
        return false;
    }
    if (request.request_id.empty()) request.request_id = make_request_id(request);
    pending_request_ = std::move(request);
    last_error_.clear();
    return true;
}

std::string ProjectUiAuthoringPanel::make_request_id(const ProjectUiAuthoringPanelRequest& request) const {
    std::string payload = project_ui_authoring_request_kind_name(request.kind);
    payload += ":" + std::to_string(request.base_revision);
    payload += ":" + std::to_string(static_cast<unsigned int>(request.node_kind));
    payload += ":" + request.node_id + ":" + request.parent_id + ":" + request.role + ":" + request.label +
        ":" + request.action_id + ":" + request.binding_json + ":" + request.state_json +
        ":" + request.presentation_json + ":" + request.value_json + ":" + request.design_tokens_json +
        ":" + request.component_id + ":" + request.component_ref + ":" + request.component_json;
    return "project-ui." + std::string(project_ui_authoring_request_kind_name(request.kind)) + "." + hash_payload(payload);
}

bool ProjectUiAuthoringPanel::validate_binding_json(const std::string_view binding_json,
                                                    const std::string_view field) {
    if (binding_json.size() > maximum_binding_bytes) {
        last_error_ = "Project UI " + std::string(field) + " JSON exceeds the 64 KiB authoring limit.";
        return false;
    }
    const auto parsed = Json::parse(binding_json, nullptr, false);
    if (parsed.is_discarded()) {
        last_error_ = "Project UI " + std::string(field) + " JSON is not valid JSON.";
        return false;
    }
    return true;
}

bool ProjectUiAuthoringPanel::validate_object_json(const std::string_view json,
                                                   const std::string_view field) {
    if (!validate_binding_json(json, field)) return false;
    const auto parsed = Json::parse(json, nullptr, false);
    if (!parsed.is_object() && !parsed.is_null()) {
        last_error_ = "Project UI " + std::string(field) + " must be a JSON object or null.";
        return false;
    }
    return true;
}

bool ProjectUiAuthoringPanel::validate_any_json(const std::string_view json,
                                                const std::string_view field) {
    return validate_binding_json(json, field);
}

void ProjectUiAuthoringPanel::parse_document() {
    view_ = {};
    view_.schema_version = std::string(ui_document_schema);
    last_error_.clear();
    const auto source = Json::parse(snapshot_.document_json, nullptr, false);
    if (source.is_discarded()) {
        view_.valid = false;
        add_diagnostic(view_, "ui.document-json-parse-error", "$", "The project UI document is not valid JSON.");
        last_error_ = view_.diagnostics.front().detail;
        return;
    }
    if (!source.is_object()) {
        view_.valid = false;
        add_diagnostic(view_, "ui.document-not-object", "$", "The project UI document root must be an object.");
        last_error_ = view_.diagnostics.front().detail;
        return;
    }
    view_.schema_version = string_member(source, "schemaVersion");
    view_.document_id = string_member(source, "documentId");
    if (view_.schema_version != ui_document_schema)
        add_diagnostic(view_, "ui.invalid-schema", "/schemaVersion", "Expected noemancer.ui-document/0.1.");
    if (view_.document_id.empty())
        add_diagnostic(view_, "ui.invalid-document-id", "/documentId", "documentId must be a non-empty string.");
    const auto nodes = source.find("nodes");
    if (nodes == source.end() || !nodes->is_array()) {
        add_diagnostic(view_, "ui.invalid-nodes", "/nodes", "nodes must be an array.");
        view_.valid = false;
        last_error_ = view_.diagnostics.front().detail;
        return;
    }
    if (nodes->size() > maximum_nodes)
        add_diagnostic(view_, "ui.node-limit-exceeded", "/nodes", "The project UI authoring projection is limited to 4096 nodes.");

    if (const auto tokens = source.find("designTokens"); tokens != source.end()) {
        view_.design_tokens_json = tokens->dump();
        view_.design_tokens_kind = json_kind(*tokens);
        if (!tokens->is_object() && !tokens->is_null()) {
            view_.design_tokens_valid = false;
            add_diagnostic(view_, "ui.invalid-design-tokens", "/designTokens",
                           "designTokens must be a JSON object or null.");
        }
    } else {
        view_.design_tokens_json = "{}";
        view_.design_tokens_kind = ProjectUiAuthoringJsonKind::absent;
    }
    view_.design_tokens_status = {.field = ProjectUiAuthoringFieldKind::design_tokens,
                                  .valid = view_.design_tokens_valid};

    if (const auto component_array = source.find("components");
        component_array != source.end() && component_array->is_array()) {
        for (std::size_t index = 0U; index < component_array->size() && index < maximum_nodes; ++index) {
            const auto& component = component_array->at(index);
            const auto path = "/components/" + std::to_string(index);
            if (!component.is_object()) {
                add_diagnostic(view_, "ui.invalid-component", path,
                               "Every component declaration must be an object.");
                continue;
            }
            auto component_id = first_string_member(component, {"id", "componentId"});
            if (component_id.empty()) {
                add_diagnostic(view_, "ui.invalid-component-id", path + "/id",
                               "Every component declaration needs a non-empty id.");
                continue;
            }
            if (std::ranges::find(view_.components, component_id,
                                  &ProjectUiAuthoringComponentDeclaration::id) != view_.components.end()) {
                add_diagnostic(view_, "ui.duplicate-component-id", path + "/id",
                               "Component declaration IDs must be unique.");
                continue;
            }
            auto root_node_id = first_string_member(component, {"rootNodeId", "root"});
            auto label = first_string_member(component, {"label", "name"});
            view_.components.push_back({.id = capped_string(std::move(component_id), maximum_text_bytes),
                                        .root_node_id = capped_string(std::move(root_node_id), maximum_text_bytes),
                                        .label = capped_string(std::move(label), maximum_text_bytes),
                                        .component_json = component.dump(), .valid = true,
                                        .status = {.field = ProjectUiAuthoringFieldKind::component_declaration,
                                                   .valid = true}});
        }
    } else if (const auto invalid_components = source.find("components"); invalid_components != source.end()) {
        add_diagnostic(view_, "ui.invalid-components", "/components",
                       "components must be an array of component declarations.");
    }

    std::unordered_set<std::string> ids;
    const auto count = std::min<std::size_t>(nodes->size(), maximum_nodes);
    view_.nodes.reserve(count);
    for (std::size_t index = 0U; index < count; ++index) {
        const auto& source_node = nodes->at(index);
        const auto path = "/nodes/" + std::to_string(index);
        if (!source_node.is_object()) {
            add_diagnostic(view_, "ui.invalid-node", path, "Every project UI node must be an object.");
            continue;
        }
        auto node_id = string_member(source_node, "id");
        if (node_id.empty()) {
            add_diagnostic(view_, "ui.invalid-node-id", path + "/id", "Every project UI node needs a non-empty id.");
            continue;
        }
        node_id = capped_string(std::move(node_id), maximum_text_bytes);
        if (!ids.insert(node_id).second) {
            add_diagnostic(view_, "ui.duplicate-node-id", path + "/id", "Project UI node IDs must be unique.", node_id);
            continue;
        }
        auto role = string_member(source_node, "role");
        if (role.empty()) {
            add_diagnostic(view_, "ui.invalid-node-role", path + "/role", "Every project UI node needs a non-empty role.", node_id);
            continue;
        }
        role = capped_string(std::move(role), maximum_text_bytes);
        auto binding = binding_json_from_node(source_node);
        if (binding.size() > maximum_binding_bytes) {
            add_diagnostic(view_, "ui.binding-json-too-large", path + "/binding",
                           "Project UI binding JSON exceeds the 64 KiB authoring limit.", node_id);
            binding.resize(maximum_binding_bytes);
        }
        const auto parsed_binding = Json::parse(binding, nullptr, false);
        const auto binding_valid = !parsed_binding.is_discarded();
        if (!binding_valid)
            add_diagnostic(view_, "ui.invalid-binding-json", path + "/binding",
                           "Project UI binding must contain valid JSON.", node_id);
        bool state_present{};
        bool presentation_present{};
        bool value_present{};
        auto state = json_member_from_node(source_node, "state", "{}", &state_present);
        auto presentation = json_member_from_node(source_node, "presentation", "{}", &presentation_present);
        auto value = json_member_from_node(source_node, "value", "null", &value_present);
        const auto component_ref = string_member(source_node, "componentRef");
        if (const auto component = source_node.find("componentRef");
            component != source_node.end() && !component->is_string() && !component->is_null())
            add_diagnostic(view_, "ui.invalid-component-ref", path + "/componentRef",
                           "componentRef must be a string or null.", node_id);
        const auto parse_field = [&](std::string& json, const std::string_view name,
                                     const bool expected_object, const bool present,
                                     ProjectUiAuthoringJsonKind& kind, bool& valid) {
            if (json.size() > maximum_binding_bytes) {
                add_diagnostic(view_, "ui." + std::string(name) + "-json-too-large", path + "/" + std::string(name),
                               "Project UI field JSON exceeds the 64 KiB authoring limit.", node_id);
                json.resize(maximum_binding_bytes);
            }
            const auto parsed = Json::parse(json, nullptr, false);
            kind = present ? (parsed.is_discarded() ? ProjectUiAuthoringJsonKind::invalid : json_kind(parsed)) :
                ProjectUiAuthoringJsonKind::absent;
            valid = !parsed.is_discarded();
            if (!valid) {
                add_diagnostic(view_, "ui.invalid-" + std::string(name) + "-json", path + "/" + std::string(name),
                               "Project UI field must contain valid JSON.", node_id);
            } else if (expected_object && present && !parsed.is_object() && !parsed.is_null()) {
                add_diagnostic(view_, "ui.invalid-" + std::string(name) + "-shape", path + "/" + std::string(name),
                               "Project UI field must be a JSON object or null.", node_id);
                valid = false;
            }
        };
        ProjectUiAuthoringJsonKind state_kind{ProjectUiAuthoringJsonKind::absent};
        ProjectUiAuthoringJsonKind presentation_kind{ProjectUiAuthoringJsonKind::absent};
        ProjectUiAuthoringJsonKind value_kind{ProjectUiAuthoringJsonKind::absent};
        bool state_valid{true};
        bool presentation_valid{true};
        bool value_valid{true};
        parse_field(state, "state", true, state_present, state_kind, state_valid);
        parse_field(presentation, "presentation", true, presentation_present, presentation_kind, presentation_valid);
        parse_field(value, "value", false, value_present, value_kind, value_valid);
        auto label = string_member(source_node, "label");
        if (label.empty()) {
            const auto text = source_node.find("text");
            if (text != source_node.end() && text->is_object()) label = string_member(*text, "resolved");
        }
        auto parent = string_member(source_node, "parentId");
        const auto explicit_kind = string_member(source_node, "kind");
        const auto kind = explicit_kind.empty() ? kind_from_string(role) : kind_from_string(explicit_kind);
        ProjectUiAuthoringNode projected;
        projected.id = std::move(node_id);
        projected.parent_id = capped_string(std::move(parent), maximum_text_bytes);
        projected.role = std::move(role);
        projected.label = capped_string(std::move(label), maximum_text_bytes);
        projected.action_id = capped_string(action_id_from_node(source_node), maximum_text_bytes);
        projected.binding_json = std::move(binding);
        projected.state_json = std::move(state);
        projected.presentation_json = std::move(presentation);
        projected.value_json = std::move(value);
        projected.component_ref = capped_string(component_ref, maximum_text_bytes);
        projected.kind = kind;
        projected.binding_valid = binding_valid;
        projected.state_valid = state_valid;
        projected.presentation_valid = presentation_valid;
        projected.value_valid = value_valid;
        projected.state_kind = state_kind;
        projected.presentation_kind = presentation_kind;
        projected.value_kind = value_kind;
        view_.nodes.push_back(std::move(projected));
    }

    std::unordered_map<std::string, std::size_t> index_by_id;
    index_by_id.reserve(view_.nodes.size());
    for (std::size_t index = 0U; index < view_.nodes.size(); ++index) index_by_id.emplace(view_.nodes[index].id, index);
    for (const auto& node : view_.nodes) {
        if (node.parent_id.empty()) view_.root_ids.push_back(node.id);
        else if (!index_by_id.contains(node.parent_id))
            add_diagnostic(view_, "ui.parent-not-found", "/nodes/" + node.id + "/parentId",
                           "The project UI node parent does not exist.", node.id);
    }
    for (auto& node : view_.nodes)
        if (const auto parent = index_by_id.find(node.parent_id); !node.parent_id.empty() && parent != index_by_id.end())
            ++view_.nodes[parent->second].child_count;

    std::vector<std::uint8_t> colors(view_.nodes.size(), 0U);
    std::function<std::size_t(std::size_t)> depth_of = [&](const std::size_t index) -> std::size_t {
        if (colors[index] == 2U) return view_.nodes[index].depth;
        if (colors[index] == 1U) {
            add_diagnostic(view_, "ui.parent-cycle", "/nodes/" + view_.nodes[index].id + "/parentId",
                           "Project UI node parent relationships cannot contain a cycle.", view_.nodes[index].id);
            view_.nodes[index].depth = 0U;
            return 0U;
        }
        colors[index] = 1U;
        const auto parent = index_by_id.find(view_.nodes[index].parent_id);
        auto depth = parent == index_by_id.end() || view_.nodes[index].parent_id.empty() ? 0U : depth_of(parent->second) + 1U;
        if (depth > maximum_depth) {
            add_diagnostic(view_, "ui.depth-limit-exceeded", "/nodes/" + view_.nodes[index].id,
                           "The project UI authoring projection is limited to 64 hierarchy levels.", view_.nodes[index].id);
            depth = maximum_depth;
        }
        view_.nodes[index].depth = depth;
        colors[index] = 2U;
        return depth;
    };
    for (std::size_t index = 0U; index < view_.nodes.size(); ++index) static_cast<void>(depth_of(index));

    if (const auto valid = source.find("valid"); valid != source.end() && !valid->is_boolean())
        add_diagnostic(view_, "ui.invalid-valid-flag", "/valid", "The document valid flag must be boolean.");
    view_.valid = view_.diagnostics.empty();
    if (!view_.valid && !view_.diagnostics.empty()) last_error_ = view_.diagnostics.front().detail;
}

void ProjectUiAuthoringPanel::retain_selection() {
    discard_stale_drafts();
    if (selected_node() == nullptr)
        selected_node_id_ = view_.nodes.empty() ? std::string{} : view_.nodes.front().id;
    sync_selected_draft();
}

void ProjectUiAuthoringPanel::sync_selected_draft() {
    const auto* node = selected_node();
    if (node == nullptr) return;
    if (!drafts_.contains(node->id))
        drafts_.emplace(node->id, ProjectUiAuthoringNodeDraft{.id = node->id, .parent_id = node->parent_id,
                       .role = node->role, .label = node->label, .action_id = node->action_id,
                       .binding_json = node->binding_json, .state_json = node->state_json,
                       .presentation_json = node->presentation_json, .value_json = node->value_json,
                       .component_ref = node->component_ref});
}

void ProjectUiAuthoringPanel::discard_stale_drafts() {
    std::erase_if(drafts_, [&](const auto& entry) {
        return std::ranges::find(view_.nodes, entry.first, &ProjectUiAuthoringNode::id) == view_.nodes.end();
    });
}

} // namespace noemancer
