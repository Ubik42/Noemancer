#include "editor/project_settings_input_map_panel.hpp"

#include <iostream>
#include <string>
#include <vector>

namespace {

noemancer::ProjectSettingsInputMapSnapshot snapshot(const std::uint64_t revision = 11U) {
    return {.project_id = "project.panel-test",
            .project_name = "Panel Test",
            .revision = revision,
            .actions = {
                {"gameplay.jump", noemancer::InputActionKind::button,
                 {{"keyboard.space", 1.0F, 0.0F}}},
                {"gameplay.move.x", noemancer::InputActionKind::axis_1d,
                 {{"keyboard.a", -1.0F, 0.0F}, {"keyboard.d", 1.0F, 0.0F}}}}};
}

} // namespace

int main() {
    using namespace noemancer;
    ProjectSettingsInputMapPanel panel(snapshot());
    const auto initial = panel.state();
    if (initial.selected_action_id != "gameplay.jump" || initial.selected_binding_id.empty() ||
        initial.has_pending_request) {
        std::cerr << "Panel did not establish stable initial selection.\n";
        return 1;
    }

    panel.set_action_draft("gameplay.pause", InputActionKind::button);
    panel.set_add_binding_draft("keyboard.escape", 1.0F, 0.0F);
    if (!panel.request_add_action()) return 2;
    const auto add_action = panel.consume_request();
    if (!add_action || add_action->kind != ProjectSettingsInputMapPanelRequestKind::add_action ||
        !add_action->intent || add_action->intent->base_revision != 11U ||
        add_action->intent->action_id != "gameplay.pause" ||
        add_action->intent->source != "keyboard.escape" || panel.consume_request()) {
        std::cerr << "Add-action request was not revision-bound or consumable.\n";
        return 3;
    }

    panel.set_add_binding_draft("keyboard.enter", 1.0F, 0.15F);
    if (!panel.request_add_binding()) return 4;
    const auto add_binding = panel.consume_request();
    if (!add_binding || add_binding->kind != ProjectSettingsInputMapPanelRequestKind::add_binding ||
        !add_binding->intent || add_binding->intent->source != "keyboard.enter" ||
        add_binding->intent->dead_zone != 0.15F) {
        std::cerr << "Add-binding request did not preserve the draft.\n";
        return 5;
    }

    const auto binding_id = panel.state().selected_binding_id;
    if (!panel.set_binding_scale("gameplay.jump", binding_id, -1.0F) ||
        !panel.set_binding_dead_zone("gameplay.jump", binding_id, 0.2F) ||
        !panel.set_binding_draft("gameplay.jump", binding_id, "keyboard.j", -1.0F, 0.2F) ||
        !panel.request_rebind_binding()) return 6;
    const auto rebind = panel.consume_request();
    if (!rebind || rebind->kind != ProjectSettingsInputMapPanelRequestKind::rebind_binding ||
        !rebind->intent || rebind->intent->binding_id != binding_id ||
        rebind->intent->source != "keyboard.j" || rebind->base_revision != 11U) {
        std::cerr << "Rebind request did not preserve stable binding identity.\n";
        return 7;
    }

    if (!panel.begin_rebind_capture()) return 8;
    const auto begin_capture = panel.consume_request();
    if (!begin_capture || begin_capture->kind != ProjectSettingsInputMapPanelRequestKind::begin_capture ||
        begin_capture->capture_request_id == 0U || begin_capture->action_id != "gameplay.jump") return 9;
    panel.set_capture_observation({.state = ProjectSettingsInputMapCaptureState::captured,
                                   .request_id = begin_capture->capture_request_id,
                                   .source = "keyboard.k", .device = "keyboard", .value = 1.0F});
    const auto captured_rebind = panel.consume_request();
    if (!captured_rebind || captured_rebind->kind != ProjectSettingsInputMapPanelRequestKind::rebind_binding ||
        !captured_rebind->intent || captured_rebind->intent->source != "keyboard.k" ||
        !panel.state().capture_action_id.empty()) {
        std::cerr << "Captured input did not produce a rebind request.\n";
        return 10;
    }

    if (!panel.begin_rebind_capture()) return 11;
    const auto begin_cancel = panel.consume_request();
    if (!begin_cancel) return 12;
    if (!panel.request_cancel_capture()) return 13;
    const auto cancel = panel.consume_request();
    if (!cancel || cancel->kind != ProjectSettingsInputMapPanelRequestKind::cancel_capture ||
        cancel->capture_request_id != begin_cancel->capture_request_id ||
        !panel.state().capture_action_id.empty()) {
        std::cerr << "Capture cancellation did not produce a stable request.\n";
        return 14;
    }

    if (!panel.begin_rebind_capture()) return 15;
    static_cast<void>(panel.consume_request());
    panel.set_snapshot(snapshot(12U));
    if (!panel.state().capture_action_id.empty() ||
        panel.state().capture.state != ProjectSettingsInputMapCaptureState::cancelled) {
        std::cerr << "Revision change did not cancel stale capture state.\n";
        return 16;
    }
    ProjectSettingsInputMapPanel stale(snapshot());
    stale.set_action_draft("gameplay.pause", InputActionKind::button);
    stale.set_add_binding_draft("keyboard.escape", 1.0F, 0.0F);
    if (!stale.request_add_action()) return 17;
    stale.set_snapshot(snapshot(12U));
    if (stale.state().has_pending_request || stale.state().last_error.empty()) {
        std::cerr << "Revision change did not reject a stale pending request.\n";
        return 18;
    }
    if (!panel.select_binding("gameplay.move.x", panel.view_model().actions().at(1).bindings.at(1).id) ||
        panel.state().selected_action_id != "gameplay.move.x") return 19;
    return 0;
}
