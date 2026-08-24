#include "editor/project_ui_authoring_panel.hpp"

#include <imgui.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <functional>
#include <iomanip>
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

void draw_text_input(const char* label, std::string& value, const std::size_t capacity = 512U) {
    std::vector<char> buffer(std::max<std::size_t>(capacity, value.size() + 1U));
    std::ranges::copy(value, buffer.begin());
    buffer[value.size()] = '\0';
    if (ImGui::InputText(label, buffer.data(), buffer.size())) value = buffer.data();
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

const char* project_ui_authoring_request_kind_name(const ProjectUiAuthoringPanelRequestKind kind) noexcept {
    switch (kind) {
    case ProjectUiAuthoringPanelRequestKind::add_node: return "add-node";
    case ProjectUiAuthoringPanelRequestKind::remove_node: return "remove-node";
    case ProjectUiAuthoringPanelRequestKind::update_node: return "update-node";
    case ProjectUiAuthoringPanelRequestKind::reparent_node: return "reparent-node";
    case ProjectUiAuthoringPanelRequestKind::undo: return "undo";
    case ProjectUiAuthoringPanelRequestKind::redo: return "redo";
    }
    return "unknown";
}

ProjectUiAuthoringPanel::ProjectUiAuthoringPanel(ProjectUiAuthoringSnapshot snapshot)
    : snapshot_(std::move(snapshot)), add_draft_{
          .id = "ui.new-node", .parent_id = {}, .role = "container", .label = "New Node",
          .action_id = {}, .binding_json = "{}"} {
    parse_document();
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
                                 .binding_json = node->binding_json};
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
    snapshot_ = std::move(snapshot);
    parse_document();
    if (pending_request_ && pending_request_->base_revision != snapshot_.revision) {
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
    if (!validate_binding_json(draft.binding_json)) return false;
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
    last_error_.clear();
    return true;
}

bool ProjectUiAuthoringPanel::set_binding_json(std::string binding_json) {
    if (selected_node_id_.empty()) {
        last_error_ = "Select a project UI node before editing its binding.";
        return false;
    }
    if (!validate_binding_json(binding_json)) return false;
    sync_selected_draft();
    drafts_[selected_node_id_].binding_json = std::move(binding_json);
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
    if (!validate_binding_json(draft.binding_json)) return false;
    ProjectUiAuthoringPanelRequest request{
        .kind = ProjectUiAuthoringPanelRequestKind::add_node,
        .base_revision = snapshot_.revision,
        .node_kind = add_kind_ == ProjectUiAuthoringNodeKind::unknown ? kind_from_string(draft.role) : add_kind_,
        .node_id = std::move(draft.id),
        .parent_id = std::move(draft.parent_id),
        .role = std::move(draft.role),
        .label = std::move(draft.label),
        .action_id = std::move(draft.action_id),
        .binding_json = std::move(draft.binding_json)};
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
    if (!validate_binding_json(draft.binding_json)) return false;
    ProjectUiAuthoringPanelRequest request{
        .kind = ProjectUiAuthoringPanelRequestKind::update_node,
        .base_revision = snapshot_.revision,
        .node_kind = kind_from_string(draft.role),
        .node_id = node->id,
        .parent_id = node->parent_id,
        .role = draft.role,
        .label = draft.label,
        .action_id = draft.action_id,
        .binding_json = draft.binding_json};
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

std::string ProjectUiAuthoringPanel::semantic_snapshot_json() const {
    Json result{{"schemaVersion", "noemancer.project-ui-authoring/0.1"},
                {"documentSchema", view_.schema_version}, {"documentId", view_.document_id},
                {"valid", view_.valid}, {"code", view_.valid ? "ok" : "ui.authoring-invalid-document"},
                {"sourceRevision", snapshot_.revision}, {"sourceFingerprint", snapshot_.fingerprint},
                {"selectedNodeId", selected_node_id_}, {"canUndo", snapshot_.can_undo},
                {"canRedo", snapshot_.can_redo}};
    result["roots"] = view_.root_ids;
    result["nodes"] = Json::array();
    for (const auto& node : view_.nodes) {
        Json binding = Json::object();
        const auto parsed_binding = Json::parse(node.binding_json, nullptr, false);
        if (!parsed_binding.is_discarded()) binding = parsed_binding;
        result["nodes"].push_back({{"id", node.id},
                                   {"parentId", node.parent_id.empty() ? Json(nullptr) : Json(node.parent_id)},
                                   {"role", node.role}, {"kind", project_ui_authoring_node_kind_name(node.kind)},
                                   {"label", node.label}, {"actionId", node.action_id}, {"binding", binding},
                                   {"bindingJson", node.binding_json}, {"bindingValid", node.binding_valid},
                                   {"depth", node.depth}, {"childCount", node.child_count},
                                   {"selected", node.id == selected_node_id_}});
    }
    result["drafts"] = Json::array();
    for (const auto& [node_id, draft] : drafts_)
        result["drafts"].push_back({{"nodeId", node_id}, {"parentId", draft.parent_id}, {"role", draft.role},
                                    {"label", draft.label}, {"actionId", draft.action_id},
                                    {"bindingJson", draft.binding_json}});
    result["addDraft"] = {{"id", add_draft_.id}, {"parentId", add_draft_.parent_id},
                           {"kind", project_ui_authoring_node_kind_name(add_kind_)},
                           {"role", add_draft_.role}, {"label", add_draft_.label},
                           {"actionId", add_draft_.action_id}, {"bindingJson", add_draft_.binding_json}};
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
    ImGui::SetNextWindowSize({620.0F, 760.0F}, ImGuiCond_FirstUseEver);
    ImGui::Begin("Project UI Authoring");
    ImGui::Text("Revision %llu | %zu nodes | %s",
                static_cast<unsigned long long>(snapshot_.revision), view_.nodes.size(),
                view_.valid ? "valid" : "invalid");
    if (!view_.diagnostics.empty()) {
        ImGui::TextColored({1.0F, 0.55F, 0.25F, 1.0F}, "%zu document diagnostics", view_.diagnostics.size());
        if (ImGui::TreeNode("Diagnostics")) {
            for (const auto& diagnostic : view_.diagnostics)
                ImGui::BulletText("%s: %s", diagnostic.code.c_str(), diagnostic.detail.c_str());
            ImGui::TreePop();
        }
    }

    ImGui::SeparatorText("Hierarchy");
    for (const auto& node : view_.nodes) {
        ImGui::PushID(node.id.c_str());
        const std::string prefix(node.depth * 2U, ' ');
        const bool selected = node.id == selected_node_id_;
        if (ImGui::Selectable((prefix + node.label + " [" + node.role + "]").c_str(), selected))
            static_cast<void>(select_node(node.id));
        ImGui::SameLine();
        ImGui::TextDisabled("%s", node.id.c_str());
        ImGui::PopID();
    }

    if (const auto* node = selected_node(); node != nullptr) {
        sync_selected_draft();
        auto& draft = drafts_.at(node->id);
        ImGui::SeparatorText("Selected Node");
        ImGui::Text("%s (%s)", node->id.c_str(), project_ui_authoring_node_kind_name(node->kind));
        draw_text_input("Label", draft.label);
        draw_text_input("Role", draft.role);
        draw_text_input("Parent ID", draft.parent_id);
        draw_text_input("Action ID", draft.action_id);
        draw_text_input("Binding JSON", draft.binding_json, 4096U);
        if (ImGui::Button("Update")) static_cast<void>(request_update_node());
        ImGui::SameLine();
        if (ImGui::Button("Reparent")) static_cast<void>(request_reparent_node());
        ImGui::SameLine();
        if (ImGui::Button("Delete Subtree")) static_cast<void>(request_remove_node());
    }

    ImGui::SeparatorText("Add Node");
    if (ImGui::BeginCombo("New Kind", project_ui_authoring_node_kind_name(add_kind_))) {
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
    draw_text_input("New ID", add_draft_.id);
    draw_text_input("New Role", add_draft_.role);
    draw_text_input("New Label", add_draft_.label);
    draw_text_input("New Parent ID", add_draft_.parent_id);
    draw_text_input("New Action ID", add_draft_.action_id);
    draw_text_input("New Binding JSON", add_draft_.binding_json, 4096U);
    if (ImGui::Button("Add Node")) static_cast<void>(request_add_node());
    ImGui::SameLine();
    if (ImGui::Button("Undo")) static_cast<void>(request_undo());
    ImGui::SameLine();
    if (ImGui::Button("Redo")) static_cast<void>(request_redo());

    if (pending_request_)
        ImGui::TextDisabled("Pending: %s (%s)", pending_request_->request_id.c_str(),
                           project_ui_authoring_request_kind_name(pending_request_->kind));
    if (!last_error_.empty()) ImGui::TextColored({1.0F, 0.35F, 0.30F, 1.0F}, "%s", last_error_.c_str());
    ImGui::End();
}

const ProjectUiAuthoringNode* ProjectUiAuthoringPanel::selected_node() const noexcept {
    const auto found = std::ranges::find(view_.nodes, selected_node_id_, &ProjectUiAuthoringNode::id);
    return found == view_.nodes.end() ? nullptr : &*found;
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
        ":" + request.action_id + ":" + request.binding_json;
    return "project-ui." + std::string(project_ui_authoring_request_kind_name(request.kind)) + "." + hash_payload(payload);
}

bool ProjectUiAuthoringPanel::validate_binding_json(const std::string_view binding_json) {
    if (binding_json.size() > maximum_binding_bytes) {
        last_error_ = "Project UI binding JSON exceeds the 64 KiB authoring limit.";
        return false;
    }
    const auto parsed = Json::parse(binding_json, nullptr, false);
    if (parsed.is_discarded()) {
        last_error_ = "Project UI binding JSON is not valid JSON.";
        return false;
    }
    return true;
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
        auto label = string_member(source_node, "label");
        if (label.empty()) {
            const auto text = source_node.find("text");
            if (text != source_node.end() && text->is_object()) label = string_member(*text, "resolved");
        }
        auto parent = string_member(source_node, "parentId");
        const auto explicit_kind = string_member(source_node, "kind");
        const auto kind = explicit_kind.empty() ? kind_from_string(role) : kind_from_string(explicit_kind);
        view_.nodes.push_back({.id = std::move(node_id), .parent_id = capped_string(std::move(parent), maximum_text_bytes),
                               .role = std::move(role), .label = capped_string(std::move(label), maximum_text_bytes),
                               .action_id = capped_string(action_id_from_node(source_node), maximum_text_bytes),
                               .binding_json = std::move(binding), .kind = kind, .depth = 0U, .child_count = 0U,
                               .binding_valid = binding_valid});
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
                       .binding_json = node->binding_json});
}

void ProjectUiAuthoringPanel::discard_stale_drafts() {
    std::erase_if(drafts_, [&](const auto& entry) {
        return std::ranges::find(view_.nodes, entry.first, &ProjectUiAuthoringNode::id) == view_.nodes.end();
    });
}

} // namespace noemancer
