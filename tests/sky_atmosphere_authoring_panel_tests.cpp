#include "editor/sky_atmosphere_authoring/panel.hpp"

#include <nlohmann/json.hpp>

#include <array>
#include <iostream>
#include <string>

using noemancer::SkyAtmosphereAuthoringEditOptions;
using noemancer::SkyAtmosphereAuthoringPanel;
using noemancer::SkyAtmosphereAuthoringSession;
using noemancer::SkyAtmosphereQuality;

int main() {
    auto settings = noemancer::make_sky_atmosphere_settings(SkyAtmosphereQuality::medium);
    SkyAtmosphereAuthoringSession session(settings);
    if (session.revision() != 1U || !session.settings()) {
        std::cerr << "Sky atmosphere session did not initialize its snapshot\n";
        return 1;
    }
    if (session.can_undo() || session.can_redo()) {
        std::cerr << "Fresh sky atmosphere session unexpectedly has history\n";
        return 2;
    }

    auto edited = settings;
    edited.enabled = false;
    auto preview = session.apply(edited, {.expected_revision = 1U, .dry_run = true});
    if (!preview.success || !preview.dry_run || preview.committed || preview.revision != 1U ||
        !preview.changed || session.settings()->enabled != settings.enabled) {
        std::cerr << "Sky atmosphere dry-run did not remain non-mutating\n";
        return 3;
    }
    auto committed = session.apply(edited, {.expected_revision = 1U});
    if (!committed.success || !committed.committed || committed.revision != 2U ||
        !session.settings() || session.settings()->enabled || !session.can_undo()) {
        std::cerr << "Sky atmosphere commit did not publish a revisioned edit\n";
        return 4;
    }
    auto conflict = session.apply(settings, {.expected_revision = 1U});
    if (conflict.success || conflict.code != "sky-atmosphere.revision-conflict" ||
        conflict.revision != 2U) {
        std::cerr << "Sky atmosphere CAS conflict was not reported\n";
        return 5;
    }
    auto undone = session.undo({.expected_revision = 2U});
    if (!undone.success || !undone.committed || undone.revision != 3U ||
        !session.settings() || !session.settings()->enabled || !session.can_redo()) {
        std::cerr << "Sky atmosphere undo did not restore the prior state\n";
        return 6;
    }
    auto redone = session.redo({.expected_revision = 3U});
    if (!redone.success || !redone.committed || redone.revision != 4U ||
        !session.settings() || session.settings()->enabled) {
        std::cerr << "Sky atmosphere redo did not restore the edited state\n";
        return 7;
    }
    const auto receipt_json = nlohmann::json::parse(redone.to_json(), nullptr, false);
    if (receipt_json.is_discarded() || receipt_json.at("code") != "sky-atmosphere.edit.committed" ||
        receipt_json.at("settings").at("enabled") != false) {
        std::cerr << "Sky atmosphere receipt JSON is not stable\n";
        return 8;
    }

    SkyAtmosphereAuthoringPanel panel({.revision = 4U, .settings = session.settings(),
                                       .can_undo = session.can_undo(), .can_redo = session.can_redo()});
    if (!panel.validation().valid || !panel.preview().valid || !panel.set_sun_intensity(2.0F) ||
        !panel.set_sun_direction({0.0F, 1.0F, 0.0F}) ||
        !panel.set_rayleigh_scattering({5.0e-6F, 13.0e-6F, 33.0e-6F}) ||
        !panel.set_mie_scattering({0.4e-6F, 0.4e-6F, 0.4e-6F})) {
        std::cerr << "Sky atmosphere panel field edits failed\n";
        return 9;
    }
    if (!panel.request_apply(true)) {
        std::cerr << "Sky atmosphere panel did not queue a dry-run request\n";
        return 10;
    }
    const auto request = panel.consume_request();
    if (!request || !request->dry_run || request->expected_revision != 4U ||
        request->kind != noemancer::SkyAtmosphereAuthoringRequestKind::apply ||
        request->request_id.find("editor.project-settings.sky-atmosphere.apply") != 0U) {
        std::cerr << "Sky atmosphere panel request was not revision-bound\n";
        return 11;
    }
    const auto semantic = nlohmann::json::parse(panel.semantic_state_json(), nullptr, false);
    if (semantic.is_discarded() ||
        semantic.at("schema") != "noemancer.sky-atmosphere-authoring-panel/0.1" ||
        semantic.at("nodeId") != "editor.project-settings.sky-atmosphere" ||
        semantic.at("fields").size() < 6U ||
        semantic.at("fields").at(2).at("intent") != "set-atmosphere-sun-direction" ||
        semantic.at("preview").at("sunIntensity") != 2.0F) {
        std::cerr << "Sky atmosphere semantic UI projection is incomplete\n";
        return 12;
    }

    if (!panel.set_sun_direction({0.0F, 0.0F, 0.0F}) || panel.validation().valid ||
        panel.request_apply(false) || panel.last_error().empty()) {
        std::cerr << "Sky atmosphere panel accepted an invalid direction\n";
        return 13;
    }
    panel.set_snapshot(session.snapshot());
    if (panel.snapshot().revision != session.revision() || !panel.snapshot().settings ||
        !session.settings() ||
        noemancer::SkyAtmosphereSettingsCodec::write_canonical_json(*panel.snapshot().settings) !=
            noemancer::SkyAtmosphereSettingsCodec::write_canonical_json(*session.settings())) {
        std::cerr << "Sky atmosphere panel failed to refresh its snapshot\n";
        return 14;
    }
    return 0;
}
