#include "editor/project_settings_input_map_panel.hpp"

#include <imgui.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <utility>

namespace noemancer {
namespace {

const char* capture_state_name(const ProjectSettingsInputMapCaptureState state) noexcept {
    switch (state) {
    case ProjectSettingsInputMapCaptureState::idle: return "idle";
    case ProjectSettingsInputMapCaptureState::armed: return "armed";
    case ProjectSettingsInputMapCaptureState::captured: return "captured";
    case ProjectSettingsInputMapCaptureState::cancelled: return "cancelled";
    }
    return "unknown";
}

const char* panel_request_kind_name(const ProjectSettingsInputMapPanelRequestKind kind) noexcept {
    switch (kind) {
    case ProjectSettingsInputMapPanelRequestKind::add_action: return "add-action";
    case ProjectSettingsInputMapPanelRequestKind::remove_action: return "remove-action";
    case ProjectSettingsInputMapPanelRequestKind::add_binding: return "add-binding";
    case ProjectSettingsInputMapPanelRequestKind::remove_binding: return "remove-binding";
    case ProjectSettingsInputMapPanelRequestKind::rebind_binding: return "rebind-binding";
    case ProjectSettingsInputMapPanelRequestKind::begin_capture: return "begin-capture";
    case ProjectSettingsInputMapPanelRequestKind::cancel_capture: return "cancel-capture";
    }
    return "unknown";
}

bool same_binding(const ProjectSettingsInputMapBindingDraft& draft, const std::string_view action_id,
                  const std::string_view binding_id) {
    return draft.action_id == action_id && draft.binding_id == binding_id;
}

} // namespace

ProjectSettingsInputMapPanel::ProjectSettingsInputMapPanel(ProjectSettingsInputMapSnapshot snapshot)
    : view_model_(std::move(snapshot)) {
    retain_selection();
    sync_edit_buffers();
}

const ProjectSettingsInputMapViewModel& ProjectSettingsInputMapPanel::view_model() const noexcept {
    return view_model_;
}

ProjectSettingsInputMapPanelState ProjectSettingsInputMapPanel::state() const {
    ProjectSettingsInputMapPanelState result{
        .selected_action_id = selected_action_id_,
        .selected_binding_id = selected_binding_id_,
        .action_draft_id = action_draft_id_,
        .action_draft_kind = action_draft_kind_,
        .add_binding_source = add_binding_source_,
        .add_binding_scale = add_binding_scale_,
        .add_binding_dead_zone = add_binding_dead_zone_,
        .binding_drafts = binding_drafts_,
        .capture = capture_,
        .capture_action_id = capture_action_id_,
        .capture_binding_id = capture_binding_id_,
        .last_error = last_error_,
        .has_pending_request = pending_request_.has_value()};
    std::ranges::sort(result.binding_drafts, [](const auto& left, const auto& right) {
        return left.action_id != right.action_id ? left.action_id < right.action_id :
            left.binding_id < right.binding_id;
    });
    return result;
}

std::optional<ProjectSettingsInputMapPanelRequest> ProjectSettingsInputMapPanel::consume_request() {
    if (!pending_request_) return std::nullopt;
    auto result = std::move(pending_request_);
    pending_request_.reset();
    return result;
}

void ProjectSettingsInputMapPanel::set_snapshot(ProjectSettingsInputMapSnapshot snapshot) {
    const auto previous_revision = view_model_.snapshot().revision;
    view_model_ = ProjectSettingsInputMapViewModel(std::move(snapshot));
    retain_selection();
    sync_edit_buffers();
    if (view_model_.snapshot().revision != previous_revision) {
        if (pending_request_ && pending_request_->base_revision != view_model_.snapshot().revision) {
            pending_request_.reset();
            last_error_ = "The pending Input Map request became stale after the project revision changed.";
        }
        if (!capture_action_id_.empty()) {
            capture_ = {.state = ProjectSettingsInputMapCaptureState::cancelled,
                        .request_id = capture_request_id_, .source = {}, .device = {}, .value = {}};
            capture_action_id_.clear();
            capture_binding_id_.clear();
            capture_request_id_ = 0U;
        }
    }
}

void ProjectSettingsInputMapPanel::set_capture_observation(
    ProjectSettingsInputMapCaptureObservation observation) {
    capture_ = std::move(observation);
    if (capture_action_id_.empty() || capture_.request_id != capture_request_id_) return;
    if (capture_.state == ProjectSettingsInputMapCaptureState::cancelled) {
        capture_action_id_.clear();
        capture_binding_id_.clear();
        capture_request_id_ = 0U;
        return;
    }
    if (capture_.state != ProjectSettingsInputMapCaptureState::captured) return;
    if (capture_.source.empty()) {
        last_error_ = "Captured input source is empty.";
        return;
    }
    const auto* draft = binding_draft(capture_action_id_, capture_binding_id_);
    const auto target_action = std::ranges::find(view_model_.actions(), capture_action_id_, &InputMapActionView::id);
    const auto* binding = target_action == view_model_.actions().end() ? nullptr : [&]() -> const InputMapBindingView* {
        const auto target_binding = std::ranges::find(target_action->bindings, capture_binding_id_,
                                                       &InputMapBindingView::id);
        return target_binding == target_action->bindings.end() ? nullptr : &*target_binding;
    }();
    const auto scale = draft != nullptr ? draft->scale : binding != nullptr ? binding->scale : 1.0F;
    const auto dead_zone = draft != nullptr ? draft->dead_zone : binding != nullptr ? binding->dead_zone : 0.0F;
    auto result = view_model_.rebind_binding_intent(capture_action_id_, capture_binding_id_, capture_.source,
                                                     scale, dead_zone);
    if (!result || !result.intent || !queue_edit(std::move(result),
            ProjectSettingsInputMapPanelRequestKind::rebind_binding)) return;
    capture_action_id_.clear();
    capture_binding_id_.clear();
    capture_request_id_ = 0U;
    capture_.state = ProjectSettingsInputMapCaptureState::idle;
}

bool ProjectSettingsInputMapPanel::select_action(const std::string_view action_id) {
    const auto found = std::ranges::find(view_model_.actions(), action_id, &InputMapActionView::id);
    if (found == view_model_.actions().end()) {
        last_error_ = "The requested input action does not exist.";
        return false;
    }
    selected_action_id_ = found->id;
    selected_binding_id_ = found->bindings.empty() ? std::string{} : found->bindings.front().id;
    sync_edit_buffers();
    return true;
}

bool ProjectSettingsInputMapPanel::select_binding(const std::string_view action_id,
                                                  const std::string_view binding_id) {
    const auto action = std::ranges::find(view_model_.actions(), action_id, &InputMapActionView::id);
    if (action == view_model_.actions().end()) {
        last_error_ = "The requested input action does not exist.";
        return false;
    }
    const auto binding = std::ranges::find(action->bindings, binding_id, &InputMapBindingView::id);
    if (binding == action->bindings.end()) {
        last_error_ = "The requested input binding does not exist.";
        return false;
    }
    selected_action_id_ = action->id;
    selected_binding_id_ = binding->id;
    sync_edit_buffers();
    return true;
}

void ProjectSettingsInputMapPanel::set_action_draft(std::string action_id, const InputActionKind kind) {
    action_draft_id_ = std::move(action_id);
    action_draft_kind_ = kind;
    sync_edit_buffers();
}

void ProjectSettingsInputMapPanel::set_add_binding_draft(std::string source, const float scale,
                                                         const float dead_zone) {
    add_binding_source_ = std::move(source);
    add_binding_scale_ = scale;
    add_binding_dead_zone_ = dead_zone;
    sync_edit_buffers();
}

bool ProjectSettingsInputMapPanel::set_binding_draft(const std::string_view action_id,
                                                     const std::string_view binding_id,
                                                     std::string source, const float scale,
                                                     const float dead_zone) {
    if (std::ranges::find(view_model_.actions(), action_id, &InputMapActionView::id) == view_model_.actions().end()) {
        last_error_ = "The requested input action does not exist.";
        return false;
    }
    const auto action = std::ranges::find(view_model_.actions(), action_id, &InputMapActionView::id);
    if (!binding_id.empty() && std::ranges::find(action->bindings, binding_id, &InputMapBindingView::id) == action->bindings.end()) {
        last_error_ = "The requested input binding does not exist.";
        return false;
    }
    if (binding_id.empty()) {
        set_add_binding_draft(std::move(source), scale, dead_zone);
        return true;
    }
    auto* draft = mutable_binding_draft(action_id, binding_id);
    if (draft == nullptr) return false;
    draft->source = std::move(source);
    draft->scale = scale;
    draft->dead_zone = dead_zone;
    sync_edit_buffers();
    return true;
}

bool ProjectSettingsInputMapPanel::set_binding_scale(const std::string_view action_id,
                                                     const std::string_view binding_id,
                                                     const float scale) {
    auto* draft = mutable_binding_draft(action_id, binding_id);
    if (draft == nullptr) return false;
    draft->scale = scale;
    return true;
}

bool ProjectSettingsInputMapPanel::set_binding_dead_zone(const std::string_view action_id,
                                                         const std::string_view binding_id,
                                                         const float dead_zone) {
    auto* draft = mutable_binding_draft(action_id, binding_id);
    if (draft == nullptr) return false;
    draft->dead_zone = dead_zone;
    return true;
}

bool ProjectSettingsInputMapPanel::request_add_action() {
    if(add_binding_source_.empty()) {last_error_="A new input action needs an initial binding source.";return false;}
    return queue_edit(view_model_.add_action_intent(action_draft_id_,action_draft_kind_,add_binding_source_,
                          add_binding_scale_,add_binding_dead_zone_),
                      ProjectSettingsInputMapPanelRequestKind::add_action);
}

bool ProjectSettingsInputMapPanel::request_remove_action() {
    return queue_edit(view_model_.remove_action_intent(selected_action_id_),
                      ProjectSettingsInputMapPanelRequestKind::remove_action);
}

bool ProjectSettingsInputMapPanel::request_add_binding() {
    if (selected_action_id_.empty()) {
        last_error_ = "Select an input action before adding a binding.";
        return false;
    }
    return queue_edit(view_model_.add_binding_intent(selected_action_id_, add_binding_source_,
                                                      add_binding_scale_, add_binding_dead_zone_),
                      ProjectSettingsInputMapPanelRequestKind::add_binding);
}

bool ProjectSettingsInputMapPanel::request_remove_binding() {
    return queue_edit(view_model_.remove_binding_intent(selected_action_id_, selected_binding_id_),
                      ProjectSettingsInputMapPanelRequestKind::remove_binding);
}

bool ProjectSettingsInputMapPanel::request_rebind_binding() {
    const auto* binding = selected_binding();
    if (binding == nullptr) {
        last_error_ = "Select an input binding before rebinding it.";
        return false;
    }
    const auto* draft = binding_draft(selected_action_id_, selected_binding_id_);
    const auto source = draft != nullptr ? draft->source : binding->source;
    const auto scale = draft != nullptr ? draft->scale : binding->scale;
    const auto dead_zone = draft != nullptr ? draft->dead_zone : binding->dead_zone;
    return queue_edit(view_model_.rebind_binding_intent(selected_action_id_, selected_binding_id_, source,
                                                         scale, dead_zone),
                      ProjectSettingsInputMapPanelRequestKind::rebind_binding);
}

bool ProjectSettingsInputMapPanel::begin_rebind_capture() {
    const auto* binding = selected_binding();
    if (binding == nullptr) {
        last_error_ = "Select an input binding before capturing a replacement.";
        return false;
    }
    if (pending_request_) {
        last_error_ = "Consume the pending Input Map request before starting capture.";
        return false;
    }
    const auto request_number = capture_request_id(selected_action_id_, selected_binding_id_);
    ProjectSettingsInputMapPanelRequest request{
        .kind = ProjectSettingsInputMapPanelRequestKind::begin_capture,
        .request_id = std::string(editor_project_settings_input_map_rebind_id) + ".capture." +
            selected_action_id_ + "." + selected_binding_id_,
        .base_revision = view_model_.snapshot().revision,
        .capture_request_id = request_number,
        .action_id = selected_action_id_,
        .binding_id = selected_binding_id_,
        .source = {}, .scale = binding->scale, .dead_zone = binding->dead_zone,
        .action_kind = selected_action()->kind, .intent = std::nullopt};
    if (!queue_request(std::move(request))) return false;
    capture_action_id_ = selected_action_id_;
    capture_binding_id_ = selected_binding_id_;
    capture_request_id_ = request_number;
    capture_ = {.state = ProjectSettingsInputMapCaptureState::armed,
                .request_id = request_number, .source = {}, .device = {}, .value = {}};
    return true;
}

bool ProjectSettingsInputMapPanel::request_cancel_capture() {
    if (capture_action_id_.empty()) {
        last_error_ = "No Input Map capture is active.";
        return false;
    }
    ProjectSettingsInputMapPanelRequest request{
        .kind = ProjectSettingsInputMapPanelRequestKind::cancel_capture,
        .request_id = std::string(editor_project_settings_input_map_rebind_id) + ".cancel." +
            std::to_string(capture_request_id_),
        .base_revision = view_model_.snapshot().revision,
        .capture_request_id = capture_request_id_,
        .action_id = capture_action_id_,
        .binding_id = capture_binding_id_,
        .source = {}, .scale = 1.0F, .dead_zone = {},
        .action_kind = InputActionKind::button, .intent = std::nullopt};
    if (!queue_request(std::move(request))) return false;
    capture_.state = ProjectSettingsInputMapCaptureState::cancelled;
    capture_action_id_.clear();
    capture_binding_id_.clear();
    capture_request_id_ = 0U;
    return true;
}

const InputMapActionView* ProjectSettingsInputMapPanel::selected_action() const noexcept {
    const auto found = std::ranges::find(view_model_.actions(), selected_action_id_, &InputMapActionView::id);
    return found == view_model_.actions().end() ? nullptr : &*found;
}

const InputMapBindingView* ProjectSettingsInputMapPanel::selected_binding() const noexcept {
    const auto* action = selected_action();
    if (action == nullptr) return nullptr;
    const auto found = std::ranges::find(action->bindings, selected_binding_id_, &InputMapBindingView::id);
    return found == action->bindings.end() ? nullptr : &*found;
}

ProjectSettingsInputMapBindingDraft* ProjectSettingsInputMapPanel::mutable_binding_draft(
    const std::string_view action_id, const std::string_view binding_id) {
    const auto found = std::ranges::find_if(binding_drafts_, [&](const auto& draft) {
        return same_binding(draft, action_id, binding_id);
    });
    if (found != binding_drafts_.end()) return &*found;
    const auto action = std::ranges::find(view_model_.actions(), action_id, &InputMapActionView::id);
    if (action == view_model_.actions().end()) {
        last_error_ = "The requested input action does not exist.";
        return nullptr;
    }
    const auto binding = std::ranges::find(action->bindings, binding_id, &InputMapBindingView::id);
    if (binding == action->bindings.end()) {
        last_error_ = "The requested input binding does not exist.";
        return nullptr;
    }
    binding_drafts_.push_back({std::string(action_id), std::string(binding_id), binding->source,
                               binding->scale, binding->dead_zone});
    return &binding_drafts_.back();
}

const ProjectSettingsInputMapBindingDraft* ProjectSettingsInputMapPanel::binding_draft(
    const std::string_view action_id, const std::string_view binding_id) const noexcept {
    const auto found = std::ranges::find_if(binding_drafts_, [&](const auto& draft) {
        return same_binding(draft, action_id, binding_id);
    });
    return found == binding_drafts_.end() ? nullptr : &*found;
}

bool ProjectSettingsInputMapPanel::queue_request(ProjectSettingsInputMapPanelRequest request) {
    if (pending_request_) {
        last_error_ = "Consume the pending Input Map request before issuing another request.";
        return false;
    }
    pending_request_ = std::move(request);
    last_error_.clear();
    return true;
}

bool ProjectSettingsInputMapPanel::queue_edit(InputMapIntentResult result,
                                              const ProjectSettingsInputMapPanelRequestKind kind) {
    if (!result || !result.intent) {
        last_error_ = result.detail;
        return false;
    }
    const auto& intent = *result.intent;
    ProjectSettingsInputMapPanelRequest request{
        .kind = kind,
        .request_id = intent.intent_id,
        .base_revision = intent.base_revision,
        .capture_request_id = {},
        .action_id = intent.action_id,
        .binding_id = intent.binding_id,
        .source = intent.source,
        .scale = intent.scale,
        .dead_zone = intent.dead_zone,
        .action_kind = intent.action_kind,
        .intent = std::move(result.intent)};
    return queue_request(std::move(request));
}

std::uint64_t ProjectSettingsInputMapPanel::capture_request_id(const std::string_view action_id,
                                                               const std::string_view binding_id) const noexcept {
    std::uint64_t hash = 14695981039346656037ULL ^ view_model_.snapshot().revision;
    for (const auto value : action_id) {
        hash ^= static_cast<unsigned char>(value);
        hash *= 1099511628211ULL;
    }
    hash ^= 0xFFU;
    hash *= 1099511628211ULL;
    for (const auto value : binding_id) {
        hash ^= static_cast<unsigned char>(value);
        hash *= 1099511628211ULL;
    }
    return hash == 0U ? 1U : hash;
}

void ProjectSettingsInputMapPanel::retain_selection() {
    const auto action = selected_action();
    if (action == nullptr) {
        selected_action_id_ = view_model_.actions().empty() ? std::string{} : view_model_.actions().front().id;
    }
    const auto* selected = selected_action();
    if (selected == nullptr) {
        selected_binding_id_.clear();
    } else if (std::ranges::find(selected->bindings, selected_binding_id_, &InputMapBindingView::id) == selected->bindings.end()) {
        selected_binding_id_ = selected->bindings.empty() ? std::string{} : selected->bindings.front().id;
    }
    std::erase_if(binding_drafts_, [&](const auto& draft) {
        const auto draft_action = std::ranges::find(view_model_.actions(), draft.action_id, &InputMapActionView::id);
        if (draft_action == view_model_.actions().end()) return true;
        return !draft.binding_id.empty() &&
            std::ranges::find(draft_action->bindings, draft.binding_id, &InputMapBindingView::id) == draft_action->bindings.end();
    });
}

void ProjectSettingsInputMapPanel::sync_edit_buffers() {
    // The ImGui-facing text controls are backed directly by these strings;
    // non-ImGui callers need no separate buffer or frame lifecycle.
}

void ProjectSettingsInputMapPanel::render() {
    ImGui::SetNextWindowSize({680.0F, 720.0F}, ImGuiCond_FirstUseEver);
    ImGui::Begin("Project Settings / Input Map");
    ImGui::Text("Revision %llu  |  %zu actions  |  %zu diagnostics",
                static_cast<unsigned long long>(view_model_.snapshot().revision), view_model_.actions().size(),
                view_model_.diagnostics().size());

    ImGui::SeparatorText("Add Action");
    std::array<char, 128> action_buffer{};
    std::snprintf(action_buffer.data(), action_buffer.size(), "%s", action_draft_id_.c_str());
    if (ImGui::InputText("Action ID", action_buffer.data(), action_buffer.size()))
        action_draft_id_ = action_buffer.data();
    const auto kind_label = action_draft_kind_ == InputActionKind::axis_1d ? "Axis 1D" : "Button";
    if (ImGui::BeginCombo("Action Kind", kind_label)) {
        if (ImGui::Selectable("Button", action_draft_kind_ == InputActionKind::button))
            action_draft_kind_ = InputActionKind::button;
        if (ImGui::Selectable("Axis 1D", action_draft_kind_ == InputActionKind::axis_1d))
            action_draft_kind_ = InputActionKind::axis_1d;
        ImGui::EndCombo();
    }
    std::array<char,128> initial_source_buffer{};
    std::snprintf(initial_source_buffer.data(),initial_source_buffer.size(),"%s",add_binding_source_.c_str());
    if(ImGui::InputText("Initial / new source",initial_source_buffer.data(),initial_source_buffer.size()))
        add_binding_source_=initial_source_buffer.data();
    ImGui::InputFloat("Initial / new scale",&add_binding_scale_,0.1F,1.0F,"%.3f");
    ImGui::InputFloat("Initial / new dead-zone",&add_binding_dead_zone_,0.05F,0.25F,"%.3f");
    if (ImGui::SmallButton("Add Action")) static_cast<void>(request_add_action());

    ImGui::SeparatorText("Bindings");
    for (const auto& action : view_model_.actions()) {
        ImGui::PushID(action.node_id.c_str());
        const auto open = ImGui::TreeNodeEx(action.node_id.c_str(), ImGuiTreeNodeFlags_SpanAvailWidth,
                                            "%s (%s)", action.id.c_str(), input_map_action_kind_name(action.kind).c_str());
        ImGui::SameLine();
        if (ImGui::SmallButton("Select")) static_cast<void>(select_action(action.id));
        ImGui::SameLine();
        if (ImGui::SmallButton("Remove Action")) {
            static_cast<void>(select_action(action.id));
            static_cast<void>(request_remove_action());
        }
        if (!action.diagnostic_ids.empty()) {
            ImGui::SameLine();
            ImGui::TextColored({1.0F, 0.55F, 0.28F, 1.0F}, "diagnostics: %zu", action.diagnostic_ids.size());
        }
        if (open) {
            if (selected_action_id_ == action.id) {
                std::array<char, 128> source_buffer{};
                std::snprintf(source_buffer.data(), source_buffer.size(), "%s", add_binding_source_.c_str());
                if (ImGui::InputText("New source", source_buffer.data(), source_buffer.size()))
                    add_binding_source_ = source_buffer.data();
                ImGui::InputFloat("New scale", &add_binding_scale_, 0.1F, 1.0F, "%.3f");
                ImGui::InputFloat("New dead-zone", &add_binding_dead_zone_, 0.05F, 0.25F, "%.3f");
                if (ImGui::SmallButton("Add Binding")) static_cast<void>(request_add_binding());
            }
            for (const auto& binding : action.bindings) {
                ImGui::PushID(binding.node_id.c_str());
                const bool selected = selected_action_id_ == action.id && selected_binding_id_ == binding.id;
                if (ImGui::Selectable(binding.source.empty() ? "Unassigned Input" : binding.source.c_str(), selected))
                    static_cast<void>(select_binding(action.id, binding.id));
                if (binding.has_conflict) {
                    ImGui::SameLine();
                    ImGui::TextColored({1.0F, 0.55F, 0.28F, 1.0F}, "conflict");
                }
                if (selected) {
                    auto* draft = mutable_binding_draft(action.id, binding.id);
                    if (draft != nullptr) {
                        ImGui::InputFloat("Scale", &draft->scale, 0.1F, 1.0F, "%.3f");
                        ImGui::InputFloat("Dead-zone", &draft->dead_zone, 0.05F, 0.25F, "%.3f");
                        if (ImGui::SmallButton("Rebind / Capture")) static_cast<void>(begin_rebind_capture());
                        ImGui::SameLine();
                        if (ImGui::SmallButton("Rebind Draft")) static_cast<void>(request_rebind_binding());
                        ImGui::SameLine();
                        if (ImGui::SmallButton("Remove Binding")) static_cast<void>(request_remove_binding());
                    }
                }
                ImGui::PopID();
            }
            ImGui::TreePop();
        }
        ImGui::PopID();
    }

    if (!capture_action_id_.empty()) {
        ImGui::SeparatorText("Input Capture");
        ImGui::Text("Capture %s  (%s)", capture_action_id_.c_str(), capture_state_name(capture_.state));
        if (!capture_.source.empty()) ImGui::Text("Observed: %s", capture_.source.c_str());
        if (ImGui::SmallButton("Cancel Capture")) static_cast<void>(request_cancel_capture());
    }
    if (pending_request_) {
        ImGui::Separator();
        ImGui::TextDisabled("Pending request: %s  (revision %llu)",
                           panel_request_kind_name(pending_request_->kind),
                           static_cast<unsigned long long>(pending_request_->base_revision));
    }
    if (!last_error_.empty()) ImGui::TextColored({1.0F, 0.35F, 0.30F, 1.0F}, "%s", last_error_.c_str());
    ImGui::End();
}

} // namespace noemancer
