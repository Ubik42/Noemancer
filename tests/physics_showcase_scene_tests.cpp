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
    if (!test_showcase_document_contract()) return 1;
    std::cout << "physics_showcase_scene_tests: ok\n";
    return 0;
}
