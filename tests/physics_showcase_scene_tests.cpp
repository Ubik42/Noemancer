#include "engine/physics_showcase_scene.hpp"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <set>
#include <string>
#include <string_view>

namespace {

using namespace noemancer;

constexpr std::string_view root_id = "entity.physics-showcase-root";

bool check(const bool condition, const std::string_view message) {
    if (!condition)
        std::cerr << "physics_showcase_scene_tests: " << message << '\n';
    return condition;
}

template <typename Velocity>
bool has_nonzero_angular_velocity(const Velocity& velocity) {
    if constexpr (requires { velocity.angular; }) {
        return velocity.angular.x != 0.0 || velocity.angular.y != 0.0 ||
            velocity.angular.z != 0.0;
    }
    return false;
}

template <typename RigidBody>
bool has_continuous_collision(const RigidBody& body) {
    if constexpr (requires { body.continuous_collision; })
        return body.continuous_collision;
    return false;
}

template <typename RigidBody>
bool is_sleeping_allowed(const RigidBody& body) {
    if constexpr (requires { body.allow_sleeping; })
        return body.allow_sleeping;
    return true;
}

bool has_advanced_body_fields() {
    return requires(SceneRigidBody& body) {
        body.angular_damping = 0.0;
        body.continuous_collision = false;
        body.allow_sleeping = true;
    } && requires(SceneVelocity& velocity) {
        velocity.angular = SceneVector3{};
    };
}

bool test_constraint_gallery_contract() {
    const auto constraints = make_physics_showcase_constraints();
    if (!check(constraints.size() == 5U,
               "constraint gallery must contain exactly five teaching fixtures"))
        return false;

    std::set<std::string> ids;
    std::set<PhysicsConstraintType> types;
    std::set<std::string> referenced_bodies;
    for (const auto& constraint : constraints) {
        if (!check(!constraint.id.empty() && ids.insert(constraint.id).second,
                   "constraint gallery IDs are not unique"))
            return false;
        if (!check(constraint.id.starts_with("constraint.physics-showcase."),
                   "constraint gallery ID is outside its stable namespace"))
            return false;
        if (!check(!constraint.body_a.empty() && !constraint.body_b.empty() &&
                       constraint.body_a != constraint.body_b,
                   "constraint gallery contains an invalid body pair"))
            return false;
        const auto validation = validate_physics_constraint_spec(constraint);
        if (!check(validation.success,
                   "constraint gallery contains a spec rejected by the public validator"))
            return false;
        types.insert(constraint.type);
        referenced_bodies.insert(constraint.body_a);
        referenced_bodies.insert(constraint.body_b);
    }
    if (!check(types.size() == 5U &&
                   types.contains(PhysicsConstraintType::fixed) &&
                   types.contains(PhysicsConstraintType::distance) &&
                   types.contains(PhysicsConstraintType::hinge) &&
                   types.contains(PhysicsConstraintType::slider) &&
                   types.contains(PhysicsConstraintType::spring),
               "constraint gallery does not cover all first-class constraint types"))
        return false;
    if (!check(referenced_bodies.size() == 10U,
               "constraint gallery body references are not the expected stable pairs"))
        return false;

    const auto document = make_physics_showcase_scene_document();
    auto assert_document_constraints = [&](const auto& candidate) {
        using Candidate = std::decay_t<decltype(candidate)>;
        if constexpr (requires(const Candidate& value) { value.physics_constraints; }) {
            if (!check(candidate.physics_constraints.size() == constraints.size(),
                       "SceneDocument did not persist the five showcase constraints"))
                return false;
            for (const auto& actual : candidate.physics_constraints) {
                const auto expected = std::find_if(
                    constraints.begin(), constraints.end(), [&](const auto& value) {
                        return value.id == actual.id;
                    });
                if (!check(expected != constraints.end() &&
                               actual.body_a == expected->body_a &&
                               actual.body_b == expected->body_b &&
                               actual.type == expected->type,
                           "SceneDocument constraint identity changed during authoring/roundtrip"))
                    return false;
            }
        }
        return true;
    };
    if (!assert_document_constraints(document)) return false;

    const auto canonical = SceneDocumentCodec::write_canonical_json(document);
    const auto parsed = SceneDocumentCodec::parse_json(
        canonical, "roundtrip://physics-showcase-constraints.scene.json");
    if (!check(parsed && parsed.document.has_value(),
               "constraint gallery canonical JSON did not parse"))
        return false;
    if (!assert_document_constraints(*parsed.document)) return false;
    if (!check(SceneDocumentCodec::write_canonical_json(*parsed.document) == canonical,
               "constraint gallery canonical JSON was not deterministic after roundtrip"))
        return false;
    return true;
}

PhysicsBodyState showcase_body(const char* id,
                               const PhysicsMotionType motion_type,
                               const PhysicsShapeType shape_type,
                               const float x,
                               const float y,
                               const float z) {
    PhysicsBodyState body;
    body.entity_id = id;
    body.motion_type = motion_type;
    body.shape_type = shape_type;
    body.position_x = x;
    body.position_y = y;
    body.position_z = z;
    body.gravity_factor = 0.0F;
    body.linear_damping = 0.12F;
    body.angular_damping = 0.12F;
    body.friction = 0.55F;
    body.mass = 1.0F;
    if (shape_type == PhysicsShapeType::sphere) {
        body.radius = 0.42F;
    } else {
        body.half_x = 0.55F;
        body.half_y = 0.55F;
        body.half_z = 0.55F;
    }
    return body;
}

bool test_constraint_gallery_headless_stability() {
    const auto constraints = make_physics_showcase_constraints();
    PhysicsConstraintRuntime runtime(32U, 8U);
    const std::array<PhysicsBodyState, 10U> bodies{
        showcase_body("entity.physics-showcase-fixed-anchor",
                      PhysicsMotionType::static_body, PhysicsShapeType::box,
                      -13.5F, 1.45F, -8.5F),
        showcase_body("entity.physics-showcase-fixed-payload",
                      PhysicsMotionType::dynamic_body, PhysicsShapeType::box,
                      -11.8F, 1.45F, -8.5F),
        showcase_body("entity.physics-showcase-distance-a",
                      PhysicsMotionType::dynamic_body, PhysicsShapeType::sphere,
                      -7.5F, 3.15F, -8.5F),
        showcase_body("entity.physics-showcase-distance-b",
                      PhysicsMotionType::dynamic_body, PhysicsShapeType::sphere,
                      -5.5F, 3.15F, -8.5F),
        showcase_body("entity.physics-showcase-hinge-post",
                      PhysicsMotionType::static_body, PhysicsShapeType::box,
                      0.0F, 2.0F, -8.5F),
        showcase_body("entity.physics-showcase-hinge-door",
                      PhysicsMotionType::dynamic_body, PhysicsShapeType::box,
                      1.5F, 2.0F, -8.5F),
        showcase_body("entity.physics-showcase-slider-rail",
                      PhysicsMotionType::static_body, PhysicsShapeType::box,
                      4.5F, 4.1F, -8.5F),
        showcase_body("entity.physics-showcase-slider-carriage",
                      PhysicsMotionType::dynamic_body, PhysicsShapeType::box,
                      6.8F, 4.1F, -8.5F),
        showcase_body("entity.physics-showcase-spring-a",
                      PhysicsMotionType::dynamic_body, PhysicsShapeType::sphere,
                      10.0F, 2.8F, -8.5F),
        showcase_body("entity.physics-showcase-spring-b",
                      PhysicsMotionType::dynamic_body, PhysicsShapeType::sphere,
                      12.5F, 2.8F, -8.5F),
    };
    for (const auto& body : bodies) {
        if (!check(runtime.register_body(body).success,
                   "constraint gallery body failed to register in headless runtime"))
            return false;
    }
    for (const auto& constraint : constraints) {
        if (!check(runtime.create_constraint(constraint).success,
                   "constraint gallery fixture failed to materialize in Jolt"))
            return false;
    }

    for (std::uint32_t frame = 0U; frame < 120U; ++frame) {
        if (!check(runtime.step(1.0F / 60.0F).success,
                   "constraint gallery did not complete its 120-frame headless step"))
            return false;
        for (const auto& body : bodies) {
            const auto state = runtime.body_state(body.entity_id);
            if (!check(state.has_value() && std::isfinite(state->position_x) &&
                           std::isfinite(state->position_y) &&
                           std::isfinite(state->position_z) &&
                           std::abs(state->position_x) < 100.0F &&
                           std::abs(state->position_y) < 100.0F &&
                           std::abs(state->position_z) < 100.0F,
                       "constraint gallery body became unstable during headless stepping"))
                return false;
        }
    }

    const auto observations = runtime.observe_constraints();
    if (!check(observations.size() == constraints.size(),
               "constraint gallery headless observations changed cardinality"))
        return false;
    for (const auto& observation : observations) {
        if (!check(observation.backend_created &&
                       std::isfinite(observation.measured_value) &&
                       std::isfinite(observation.error),
                   "constraint gallery produced an invalid headless observation"))
            return false;
    }

    const auto distance_a = runtime.body_state("entity.physics-showcase-distance-a");
    const auto distance_b = runtime.body_state("entity.physics-showcase-distance-b");
    const auto spring_a = runtime.body_state("entity.physics-showcase-spring-a");
    const auto spring_b = runtime.body_state("entity.physics-showcase-spring-b");
    if (!check(distance_a && distance_b && spring_a && spring_b,
               "constraint gallery dynamic body states were not observable"))
        return false;
    const auto pair_distance = [](const PhysicsBodyState& first,
                                  const PhysicsBodyState& second) {
        const auto dx = first.position_x - second.position_x;
        const auto dy = first.position_y - second.position_y;
        const auto dz = first.position_z - second.position_z;
        return std::sqrt(dx * dx + dy * dy + dz * dz);
    };
    if (!check(pair_distance(*distance_a, *distance_b) >= 1.45F &&
                   pair_distance(*distance_a, *distance_b) <= 2.55F &&
                   pair_distance(*spring_a, *spring_b) >= 2.30F &&
                   pair_distance(*spring_a, *spring_b) <= 2.70F,
               "distance/spring gallery pairs left their authored stable range"))
        return false;
    return true;
}

bool test_showcase_document_contract() {
    const auto document = make_physics_showcase_scene_document();
    if (!check(document.schema == "noemancer.scene/0.1" &&
                   document.scene_guid == physics_showcase_scene_guid &&
                   document.source_uri ==
                       "generated://scenes/physics-showcase.scene.json",
               "showcase document identity is not stable"))
        return false;
    if (!check(document.entities.size() >= 30U,
               "showcase scene does not contain enough entities"))
        return false;

    const auto errors = SceneDocumentCodec::validate(document);
    if (!check(errors.empty(), errors.empty() ? "" : errors.front().message))
        return false;

    const auto canonical = SceneDocumentCodec::write_canonical_json(document);
    const auto parsed = SceneDocumentCodec::parse_json(
        canonical, "roundtrip://physics-showcase.scene.json");
    if (!check(parsed && parsed.document.has_value(),
               "showcase canonical JSON did not parse"))
        return false;
    if (!check(SceneDocumentCodec::write_canonical_json(*parsed.document) ==
                   canonical,
               "showcase canonical JSON was not deterministic after roundtrip"))
        return false;
    if (!check(SceneDocumentCodec::validate(*parsed.document).empty(),
               "roundtripped showcase document failed validation"))
        return false;

    std::set<std::string> ids;
    std::set<double> restitution_values;
    std::set<double> friction_values;
    std::size_t root_count = 0U;
    std::size_t static_body_count = 0U;
    std::size_t dynamic_body_count = 0U;
    std::size_t kinematic_body_count = 0U;
    std::size_t box_count = 0U;
    std::size_t sphere_count = 0U;
    std::size_t capsule_count = 0U;
    std::size_t convex_count = 0U;
    std::size_t trigger_count = 0U;
    std::size_t visible_mesh_count = 0U;
    std::size_t pbr_count = 0U;
    std::size_t camera_count = 0U;
    std::size_t directional_light_count = 0U;
    std::size_t local_light_count = 0U;
    std::size_t velocity_count = 0U;
    std::size_t angular_velocity_count = 0U;
    std::size_t ccd_count = 0U;
    std::size_t non_sleeping_count = 0U;
    bool kinematic_platform = false;
    bool has_low_friction = false;
    bool has_high_friction = false;
    bool has_no_game_specific_scripts = true;

    for (const auto& entity : document.entities) {
        if (!check(!entity.guid.empty() && ids.insert(entity.guid).second,
                   "showcase entity IDs are not unique"))
            return false;
        if (entity.parent_guid == root_id)
            ++root_count;
        if (entity.mesh_renderer && entity.mesh_renderer->visible)
            ++visible_mesh_count;
        if (entity.pbr_material)
            ++pbr_count;
        if (entity.camera)
            ++camera_count;
        if (entity.directional_light)
            ++directional_light_count;
        if (entity.local_light)
            ++local_light_count;
        if (entity.velocity) {
            ++velocity_count;
            if (has_nonzero_angular_velocity(*entity.velocity))
                ++angular_velocity_count;
        }
        if (entity.rigid_body) {
            if (entity.rigid_body->motion_type == "static")
                ++static_body_count;
            else if (entity.rigid_body->motion_type == "dynamic")
                ++dynamic_body_count;
            else if (entity.rigid_body->motion_type == "kinematic")
                ++kinematic_body_count;
            if (has_continuous_collision(*entity.rigid_body))
                ++ccd_count;
            if (!is_sleeping_allowed(*entity.rigid_body))
                ++non_sleeping_count;
        }
        if (entity.box_collider) {
            ++box_count;
            restitution_values.insert(entity.box_collider->restitution);
            friction_values.insert(entity.box_collider->friction);
            has_low_friction |= entity.box_collider->friction < 0.2;
            has_high_friction |= entity.box_collider->friction > 0.8;
            trigger_count += entity.box_collider->is_trigger ? 1U : 0U;
        }
        if (entity.sphere_collider) {
            ++sphere_count;
            restitution_values.insert(entity.sphere_collider->restitution);
            friction_values.insert(entity.sphere_collider->friction);
            trigger_count += entity.sphere_collider->is_trigger ? 1U : 0U;
        }
        if (entity.capsule_collider) {
            ++capsule_count;
            restitution_values.insert(entity.capsule_collider->restitution);
            friction_values.insert(entity.capsule_collider->friction);
            trigger_count += entity.capsule_collider->is_trigger ? 1U : 0U;
        }
        if (entity.convex_hull_collider) {
            ++convex_count;
            restitution_values.insert(entity.convex_hull_collider->restitution);
            friction_values.insert(entity.convex_hull_collider->friction);
            trigger_count += entity.convex_hull_collider->is_trigger ? 1U : 0U;
        }
        if (entity.platform_2d && entity.rigid_body &&
            entity.rigid_body->motion_type == "kinematic" &&
            entity.platform_2d->motion_distance > 0.0)
            kinematic_platform = true;
        if (entity.managed_script || entity.character_motor_2d ||
            entity.animation_player)
            has_no_game_specific_scripts = false;
    }

    if (!check(root_count >= 1U && static_body_count >= 7U &&
                   dynamic_body_count >= 25U && kinematic_body_count >= 1U,
               "showcase body categories are incomplete"))
        return false;
    if (!check(box_count >= 30U && sphere_count >= 5U && capsule_count >= 2U &&
                   convex_count >= 1U,
               "showcase collider shape coverage is incomplete"))
        return false;
    if (!check(trigger_count >= 1U && kinematic_platform &&
                   camera_count == 1U && directional_light_count == 1U &&
                   local_light_count >= 1U,
               "showcase camera/light/trigger/platform coverage is incomplete"))
        return false;
    if (!check(visible_mesh_count >= 35U && pbr_count >= visible_mesh_count,
               "visible showcase bodies do not have PBR materials"))
        return false;
    if (!check(restitution_values.size() >= 5U && friction_values.size() >= 5U &&
                   has_low_friction && has_high_friction,
               "showcase does not provide varied friction/restitution samples"))
        return false;
    if (!check(has_no_game_specific_scripts,
               "showcase unexpectedly depends on a project-specific gameplay script"))
        return false;

    // The scene remains source-compatible with the pre-advanced document ABI;
    // when this batch's richer fields are present, assert that the fixture is
    // actually exercising them rather than merely compiling against them.
    if (has_advanced_body_fields() &&
        !check(velocity_count > 0U && angular_velocity_count > 0U &&
                   ccd_count >= 1U && non_sleeping_count >= 1U,
               "advanced angular/CCD/sleeping fixture facts are missing"))
        return false;
    return true;
}

} // namespace

int main() {
    if (!test_constraint_gallery_contract()) return 1;
    if (!test_constraint_gallery_headless_stability()) return 1;
    if (!test_showcase_document_contract()) return 1;
    std::cout << "physics_showcase_scene_tests: ok\n";
    return 0;
}
