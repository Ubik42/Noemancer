#include "engine/physics_constraints.hpp"

#include <cmath>
#include <iostream>

namespace {

using namespace noemancer;

bool expect(const bool condition, const char* message) {
    if (condition) return true;
    std::cerr << message << '\n';
    return false;
}

PhysicsBodyState box_body(const char* id, const PhysicsMotionType motion, const float x, const float y = 0.0F) {
    PhysicsBodyState body;
    body.entity_id = id;
    body.motion_type = motion;
    body.shape_type = PhysicsShapeType::box;
    body.position_x = x;
    body.position_y = y;
    body.gravity_factor = 0.0F;
    body.half_x = 0.25F;
    body.half_y = 0.25F;
    body.half_z = 0.25F;
    return body;
}

PhysicsConstraintSpec pair_spec(const char* id, const PhysicsConstraintType type,
                                const char* body_a, const char* body_b) {
    PhysicsConstraintSpec spec;
    spec.id = id;
    spec.type = type;
    spec.body_a = body_a;
    spec.body_b = body_b;
    spec.frame.anchor_a = {0.0F, 0.0F, 0.0F};
    spec.frame.anchor_b = {0.0F, 0.0F, 0.0F};
    return spec;
}

} // namespace

int main() {
    auto invalid = pair_spec("bad", PhysicsConstraintType::hinge, "a", "b");
    invalid.lower_limit = 1.0F;
    invalid.upper_limit = 0.5F;
    if (!expect(!validate_physics_constraint_spec(invalid).success,
                "invalid hinge limits were accepted")) return 1;

    auto self = pair_spec("self", PhysicsConstraintType::fixed, "a", "a");
    if (!expect(!validate_physics_constraint_spec(self).success, "self constraint was accepted")) return 2;

    PhysicsConstraintRegistry registry(2U);
    auto registry_fixed = pair_spec("b-weld", PhysicsConstraintType::fixed, "a", "b");
    if (!expect(registry.add(registry_fixed).success, "constraint registry did not add a valid record")) return 3;
    if (!expect(!registry.add(registry_fixed).success, "constraint registry accepted a duplicate stable id")) return 4;
    registry_fixed.enabled = false;
    if (!expect(registry.update(registry_fixed).success && registry.find("b-weld").has_value() &&
                    !registry.find("b-weld")->enabled,
                "constraint registry did not update a record")) return 5;
    if (!expect(registry.remove("b-weld").success && registry.all().empty(),
                "constraint registry did not remove a record")) return 6;

    // The fixture adapter must safely borrow the same process-wide Jolt type
    // registry used by the production runtime without replacing its Factory.
    PhysicsRuntime production_runtime;
    std::vector<PhysicsBodyState> empty_production_world;
    production_runtime.step(empty_production_world, 1.0F / 60.0F);
    PhysicsConstraintRuntime runtime(16U, 16U);
    if (!expect(runtime.register_body(box_body("anchor", PhysicsMotionType::static_body, 0.0F)).success,
                "static constraint anchor was not registered")) return 7;
    auto moving_body = box_body("slider", PhysicsMotionType::dynamic_body, 3.0F);
    moving_body.velocity_x = 8.0F;
    if (!expect(runtime.register_body(moving_body).success,
                "dynamic constraint body was not registered")) return 8;

    auto fixed = pair_spec("weld", PhysicsConstraintType::fixed, "anchor", "slider");
    if (!expect(runtime.create_constraint(fixed).success, "fixed constraint was not created")) return 9;
    const auto fixed_observation = runtime.observe_constraint("weld");
    if (!expect(fixed_observation.has_value() && fixed_observation->backend_created,
                "fixed constraint observation did not expose backend state")) return 10;
    for (int step = 0; step < 30; ++step) {
        if (!expect(runtime.step(1.0F / 60.0F).success, "Jolt constraint step failed")) return 11;
    }
    const auto welded_body = runtime.body_state("slider");
    if (!expect(welded_body.has_value() && std::abs(welded_body->position_x - 3.0F) < 0.2F &&
                    std::abs(welded_body->velocity_x) < 0.2F,
                "fixed constraint did not preserve the authored relative frame")) return 12;
    if (!expect(runtime.remove_constraint("weld").success, "fixed constraint was not removed")) return 13;

    auto distance = pair_spec("distance", PhysicsConstraintType::distance, "anchor", "slider");
    distance.lower_limit = 1.0F;
    distance.upper_limit = 2.0F;
    if (!expect(runtime.create_constraint(distance).success, "distance constraint was not created")) return 14;
    auto hinge = pair_spec("hinge", PhysicsConstraintType::hinge, "anchor", "slider");
    hinge.lower_limit = -1.0F;
    hinge.upper_limit = 1.0F;
    if (!expect(runtime.create_constraint(hinge).success, "hinge constraint was not created")) return 15;
    auto slider = pair_spec("slider", PhysicsConstraintType::slider, "anchor", "slider");
    slider.lower_limit = -2.0F;
    slider.upper_limit = 2.0F;
    if (!expect(runtime.create_constraint(slider).success, "slider constraint was not created")) return 16;
    auto spring = pair_spec("spring", PhysicsConstraintType::spring, "anchor", "slider");
    spring.rest_length = 1.5F;
    spring.spring_frequency_hz = 4.0F;
    spring.spring_damping_ratio = 0.8F;
    if (!expect(runtime.create_constraint(spring).success, "spring constraint was not created")) return 17;

    const auto observations = runtime.observe_constraints();
    if (!expect(observations.size() == 4U && observations[0].id == "distance" && observations[3].id == "spring",
                "constraint observations were not returned in stable ID order")) return 18;
    if (!expect(runtime.set_constraint_enabled("spring", false).success,
                "constraint enable state could not be changed")) return 19;
    const auto disabled = runtime.observe_constraint("spring");
    if (!expect(disabled.has_value() && !disabled->enabled, "disabled constraint was still reported enabled")) return 20;
    if (!expect(!runtime.remove_body("anchor").success, "body referenced by constraints was removed")) return 21;
    for (const auto& observation : observations) {
        if (!expect(runtime.remove_constraint(observation.id).success, "constraint cleanup failed")) return 22;
    }
    if (!expect(runtime.remove_body("slider").success && runtime.remove_body("anchor").success,
                "body cleanup failed after removing constraints")) return 23;
    return 0;
}
