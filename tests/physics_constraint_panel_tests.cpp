#include "editor/physics_constraint_panel.hpp"

#include <nlohmann/json.hpp>

#include <cmath>
#include <cstdint>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

namespace {

using noemancer::PhysicsConstraintEntityOption;
using noemancer::PhysicsConstraintPanel;
using noemancer::PhysicsConstraintPanelRequestKind;
using noemancer::PhysicsConstraintPanelSnapshot;
using noemancer::PhysicsConstraintSpec;
using noemancer::PhysicsConstraintType;

PhysicsConstraintSpec valid_constraint(std::string id,
                                       std::string body_a = "body.alpha",
                                       std::string body_b = "body.beta") {
    PhysicsConstraintSpec result;
    result.id = std::move(id);
    result.type = PhysicsConstraintType::distance;
    result.body_a = std::move(body_a);
    result.body_b = std::move(body_b);
    result.lower_limit = 0.0F;
    result.upper_limit = 2.0F;
    result.rest_length = 1.0F;
    result.spring_damping_ratio = 1.0F;
    return result;
}

PhysicsConstraintPanelSnapshot snapshot(std::uint64_t revision = 17U) {
    PhysicsConstraintPanelSnapshot result;
    result.world_revision = revision;
    result.manager_id = "world.physics.main";
    // Deliberately unsorted: the panel owns deterministic scene ordering.
    result.rigid_bodies = {{"body.beta", "摆锤"}, {"body.alpha", "基座"}};
    result.constraints = {valid_constraint("joint.z"), valid_constraint("joint.a")};
    return result;
}

int fail(const char* message, const int code) {
    std::cerr << "physics_constraint_panel_tests: " << message << '\n';
    return code;
}

bool has_code(const PhysicsConstraintPanel& panel, const std::string_view code) {
    for (const auto& diagnostic : panel.validation().diagnostics) {
        if (diagnostic.code == code) return true;
    }
    return false;
}

} // namespace

int main() {
    using namespace noemancer;

    PhysicsConstraintPanel panel(snapshot());
    const auto first_json = panel.semantic_state_json();
    if (first_json != panel.semantic_state_json() ||
        first_json.find("noemancer.physics-constraint-panel/0.1") == std::string::npos ||
        first_json.find("物理约束关系台") == std::string::npos ||
        first_json.find("scene-cartography-instrument-bench") == std::string::npos) {
        return fail("semantic relationship-bench projection was not deterministic or Chinese-first", 1);
    }
    const auto first = nlohmann::json::parse(first_json);
    if (first.at("snapshot").at("worldRevision") != 17U ||
        first.at("snapshot").at("manager") != "world.physics.main" ||
        first.at("constraints").at(0U).at("id") != "joint.a" ||
        first.at("entityChoices").at(0U).at("entityId") != "body.alpha") {
        return fail("snapshot choices were not stably sorted by engine IDs", 2);
    }

    // All five constraint kinds receive a valid, useful draft with the first
    // two available rigid bodies already connected.
    for (const auto type : {PhysicsConstraintType::fixed, PhysicsConstraintType::distance,
                            PhysicsConstraintType::hinge, PhysicsConstraintType::slider,
                            PhysicsConstraintType::spring}) {
        if (!panel.create_draft(type, "draft." + std::string(physics_constraint_type_name(type))) ||
            !panel.draft() || panel.draft()->body_a != "body.alpha" ||
            panel.draft()->body_b != "body.beta" || !panel.validation().valid) {
            return fail("a constraint type did not produce a valid body-connected draft", 3);
        }
    }

    if (!panel.create_draft(PhysicsConstraintType::spring, "spring.runtime") ||
        !panel.set_enabled(false) ||
        !panel.set_anchor_a({1.0F, 2.0F, 3.0F}) ||
        !panel.set_anchor_b({4.0F, 5.0F, 6.0F}) ||
        !panel.set_primary_axis_a({0.0F, 1.0F, 0.0F}) ||
        !panel.set_secondary_axis_a({1.0F, 0.0F, 0.0F}) ||
        !panel.set_primary_axis_b({0.0F, 1.0F, 0.0F}) ||
        !panel.set_secondary_axis_b({1.0F, 0.0F, 0.0F}) ||
        !panel.set_rest_length(2.5F) ||
        !panel.set_spring_frequency_hz(5.0F) ||
        !panel.set_spring_damping_ratio(0.7F) ||
        !panel.validation().valid) {
        return fail("frame and spring fields were not editable through the draft boundary", 4);
    }
    if (!panel.request_upsert(true)) return fail("valid dry-run upsert was rejected", 5);
    const auto upsert = panel.consume_request();
    if (!upsert || upsert->kind != PhysicsConstraintPanelRequestKind::upsert ||
        !upsert->dry_run || upsert->manager_id != "world.physics.main" ||
        upsert->base_revision != 17U || upsert->constraint_id != "spring.runtime" ||
        !upsert->constraint || upsert->constraint->rest_length != 2.5F ||
        upsert->request_id.empty() || panel.consume_request()) {
        return fail("upsert did not preserve manager, revision and complete constraint data", 6);
    }

    if (!panel.select_constraint("joint.a") || !panel.request_remove()) {
        return fail("existing constraint selection/remove request failed", 7);
    }
    const auto remove = panel.consume_request();
    if (!remove || remove->kind != PhysicsConstraintPanelRequestKind::remove ||
        remove->constraint_id != "joint.a" || remove->constraint || remove->base_revision != 17U) {
        return fail("remove request was not a plain revision-bound record", 8);
    }

    if (!panel.create_draft(PhysicsConstraintType::distance, "invalid.body") ||
        !panel.set_body_a("body.missing") || panel.validation().valid ||
        !has_code(panel, "physics-constraint.body-a.invalid-reference") ||
        panel.request_upsert()) {
        return fail("invalid body reference was not diagnosed and blocked", 9);
    }
    if (!panel.set_body_a("body.alpha") || !panel.set_body_b("body.alpha") ||
        panel.validation().valid ||
        !has_code(panel, "physics-constraint.body.self-reference")) {
        return fail("self-reference was not diagnosed", 10);
    }

    if (!panel.select_constraint("joint.z") || !panel.request_remove(true)) {
        return fail("revision-conflict setup failed", 11);
    }
    auto newer_snapshot = snapshot(18U);
    panel.set_snapshot(std::move(newer_snapshot));
    if (panel.state().has_pending_request || panel.selected_constraint_id() != "joint.z" ||
        !panel.draft() || panel.draft()->id != "joint.z" || panel.last_error().empty()) {
        return fail("snapshot revision change did not discard stale request and retain stable selection", 12);
    }

    PhysicsConstraintPanelSnapshot full_snapshot;
    full_snapshot.world_revision = 3U;
    full_snapshot.rigid_bodies = {{"body.alpha", "基座"}, {"body.beta", "摆锤"}};
    for (std::size_t index = 0U; index < physics_constraint_panel_max_constraints + 1U; ++index) {
        full_snapshot.constraints.push_back(valid_constraint("joint." + std::to_string(index)));
    }
    PhysicsConstraintPanel bounded(std::move(full_snapshot));
    if (bounded.snapshot().constraints.size() != physics_constraint_panel_max_constraints ||
        bounded.validation().valid ||
        !has_code(bounded, "physics-constraint.snapshot.constraints-truncated") ||
        bounded.semantic_state_json().find("\"sourceConstraintCount\": 1025") == std::string::npos) {
        return fail("constraint capacity was not bounded and explained", 13);
    }
    if (!bounded.create_draft(PhysicsConstraintType::distance, "brand-new") ||
        bounded.validation().valid || bounded.request_upsert()) {
        // The snapshot truncation diagnostic is intentionally an editor
        // warning/error: the author must refresh before changing a capped
        // relation set.
        return fail("a capped constraint snapshot incorrectly allowed a new upsert", 14);
    }

    PhysicsConstraintPanelSnapshot choice_snapshot;
    choice_snapshot.world_revision = 4U;
    choice_snapshot.rigid_bodies.reserve(physics_constraint_panel_max_entity_choices + 1U);
    for (std::size_t index = 0U; index < physics_constraint_panel_max_entity_choices + 1U; ++index) {
        choice_snapshot.rigid_bodies.push_back({"body." + std::to_string(index), {}});
    }
    PhysicsConstraintPanel choices(std::move(choice_snapshot));
    if (choices.snapshot().rigid_bodies.size() != physics_constraint_panel_max_entity_choices ||
        !has_code(choices, "physics-constraint.snapshot.entity-choices-truncated")) {
        return fail("entity choice capacity was not bounded", 15);
    }

    std::cout << "physics_constraint_panel_tests: ok\n";
    return 0;
}
