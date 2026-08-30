#include "engine/physics_showcase_scene.hpp"

#include <array>
#include <cmath>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

namespace noemancer {
namespace {

constexpr std::string_view root_id = "entity.physics-showcase-root";

SceneEntityDocument make_visible_entity(
    const std::string& id,
    const std::string& name,
    const SceneVector3 position,
    const SceneVector3 scale,
    const SceneVector3 color,
    const double metallic,
    const double roughness,
    const SceneVector3 rotation = {},
    const std::string_view mesh_asset = "asset.primitive.cube",
    const bool casts_shadows = true,
    const bool receives_shadows = true) {
    return SceneEntityDocument{
        .guid = id,
        .name = name,
        .parent_guid = std::string(root_id),
        .transform = SceneTransform{position, scale, rotation},
        .mesh_renderer = SceneMeshRenderer{
            std::string(mesh_asset), true, casts_shadows, receives_shadows},
        .pbr_material = ScenePbrMaterial{color, metallic, roughness}};
}

// These helpers keep this fixture source-compatible with the short-lived
// SceneDocument ABI while consuming the richer angular/CCD authoring fields
// as soon as the physics-authoring lane adds them.  They are intentionally
// compile-time member checks, not runtime reflection or hidden registration.
template <typename Velocity>
void set_angular_velocity(Velocity& velocity, const SceneVector3 angular) {
    if constexpr (requires(Velocity& value) { value.angular = angular; })
        velocity.angular = angular;
}

template <typename RigidBody>
void set_advanced_body_options(
    RigidBody& body,
    const double angular_damping,
    const bool continuous_collision,
    const bool allow_sleeping) {
    if constexpr (requires(RigidBody& value) {
                      value.angular_damping = angular_damping;
                  })
        body.angular_damping = angular_damping;
    if constexpr (requires(RigidBody& value) {
                      value.continuous_collision = continuous_collision;
                  })
        body.continuous_collision = continuous_collision;
    if constexpr (requires(RigidBody& value) {
                      value.allow_sleeping = allow_sleeping;
                  })
        body.allow_sleeping = allow_sleeping;
}

SceneEntityDocument make_box_body(
    const std::string& id,
    const std::string& name,
    const SceneVector3 position,
    const SceneVector3 half_extents,
    const std::string_view motion_type,
    const double mass,
    const double friction,
    const double restitution,
    const SceneVector3 color,
    const double metallic = 0.05,
    const double roughness = 0.48,
    const SceneVector3 rotation = {},
    const SceneVector3 linear_velocity = {},
    const SceneVector3 angular_velocity = {},
    const bool trigger = false,
    const bool casts_shadows = true,
    const bool receives_shadows = true,
    const double angular_damping = 0.05,
    const bool continuous_collision = false,
    const bool allow_sleeping = true) {
    auto entity = make_visible_entity(
        id, name, position, half_extents, color, metallic, roughness,
        rotation);
    entity.rigid_body = SceneRigidBody{
        std::string(motion_type), mass,
        motion_type == "static" ? 0.0 : 1.0,
        motion_type == "static" ? 0.0 : 0.035};
    set_advanced_body_options(
        *entity.rigid_body, angular_damping, continuous_collision,
        allow_sleeping);
    entity.box_collider = SceneBoxCollider{
        half_extents, friction, restitution, trigger};
    if (linear_velocity.x != 0.0 || linear_velocity.y != 0.0 ||
        linear_velocity.z != 0.0 || angular_velocity.x != 0.0 ||
        angular_velocity.y != 0.0 || angular_velocity.z != 0.0) {
        SceneVelocity velocity{linear_velocity};
        set_angular_velocity(velocity, angular_velocity);
        entity.velocity = velocity;
    }
    entity.mesh_renderer->casts_shadows = casts_shadows;
    entity.mesh_renderer->receives_shadows = receives_shadows;
    return entity;
}

SceneEntityDocument make_sphere_body(
    const std::string& id,
    const std::string& name,
    const SceneVector3 position,
    const double radius,
    const std::string_view motion_type,
    const double mass,
    const double friction,
    const double restitution,
    const SceneVector3 color,
    const SceneVector3 linear_velocity = {},
    const SceneVector3 angular_velocity = {},
    const bool trigger = false,
    const bool continuous_collision = false,
    const bool allow_sleeping = true) {
    auto entity = make_visible_entity(
        id, name, position, {radius, radius, radius}, color, 0.18, 0.28,
        {}, "asset.primitive.sphere");
    entity.rigid_body = SceneRigidBody{
        std::string(motion_type), mass,
        motion_type == "static" ? 0.0 : 1.0,
        motion_type == "static" ? 0.0 : 0.025};
    set_advanced_body_options(
        *entity.rigid_body, 0.04, continuous_collision, allow_sleeping);
    entity.sphere_collider = SceneSphereCollider{
        radius, friction, restitution, trigger};
    if (linear_velocity.x != 0.0 || linear_velocity.y != 0.0 ||
        linear_velocity.z != 0.0 || angular_velocity.x != 0.0 ||
        angular_velocity.y != 0.0 || angular_velocity.z != 0.0) {
        SceneVelocity velocity{linear_velocity};
        set_angular_velocity(velocity, angular_velocity);
        entity.velocity = velocity;
    }
    return entity;
}

SceneEntityDocument make_capsule_body(
    const std::string& id,
    const std::string& name,
    const SceneVector3 position,
    const double radius,
    const double half_height,
    const SceneVector3 color) {
    auto entity = make_visible_entity(
        id, name, position,
        {radius, radius + half_height, radius}, color, 0.05, 0.36,
        {}, "asset.primitive.sphere");
    entity.rigid_body = SceneRigidBody{"dynamic", 1.4, 1.0, 0.04};
    set_advanced_body_options(*entity.rigid_body, 0.06, false, true);
    entity.capsule_collider = SceneCapsuleCollider{
        radius, half_height, 0.45, 0.08, false};
    return entity;
}

SceneEntityDocument make_convex_body(
    const std::string& id,
    const std::string& name,
    const SceneVector3 position) {
    auto entity = make_visible_entity(
        id, name, position, {1.0, 1.0, 1.0}, {0.92, 0.42, 0.12}, 0.22,
        0.34);
    entity.rigid_body = SceneRigidBody{"dynamic", 2.0, 1.0, 0.04};
    set_advanced_body_options(*entity.rigid_body, 0.08, false, true);
    entity.convex_hull_collider = SceneConvexHullCollider{
        {{-0.9, -0.55, -0.65}, {0.9, -0.55, -0.65},
         {-0.9, 0.55, -0.65}, {0.9, 0.55, -0.65},
         {-0.55, -0.40, 0.75}, {0.55, -0.40, 0.75},
         {-0.55, 0.40, 0.75}, {0.55, 0.40, 0.75}},
        0.62, 0.12, false};
    return entity;
}

} // namespace

SceneDocument make_physics_showcase_scene_document() {
    SceneDocument document{
        .scene_guid = std::string(physics_showcase_scene_guid),
        .name = "Physics Showcase Scene",
        .source_uri = "generated://scenes/physics-showcase.scene.json"};
    document.entities.reserve(48U);

    document.entities.push_back(SceneEntityDocument{
        .guid = std::string(root_id),
        .name = "Physics Showcase Root"});
    document.entities.push_back(SceneEntityDocument{
        .guid = "entity.physics-showcase-camera",
        .name = "Physics Showcase Camera",
        .parent_guid = std::string(root_id),
        .transform = SceneTransform{{18.0, 13.0, 27.0}},
        .camera = SceneCamera{{0.0, 3.0, 0.0}, 50.0, 0.1, 120.0, true}});
    document.entities.push_back(SceneEntityDocument{
        .guid = "entity.physics-showcase-sun",
        .name = "Physics Showcase Directional Light",
        .parent_guid = std::string(root_id),
        .directional_light = SceneDirectionalLight{
            {-0.48, -1.0, -0.32}, {1.0, 0.92, 0.80}, 1.15, 0.22, true}});
    document.entities.push_back(SceneEntityDocument{
        .guid = "entity.physics-showcase-fill",
        .name = "Physics Showcase Cool Fill",
        .parent_guid = std::string(root_id),
        .transform = SceneTransform{{-8.0, 9.0, 8.0}},
        .local_light = SceneLocalLight{
            "point", {0.34, 0.58, 1.0}, 950.0, 24.0,
            {0.0, -1.0, 0.0}, 25.0, 35.0, 0.15, true}});

    // Containment geometry keeps the dynamic demonstrations in one readable
    // camera volume and makes the scene useful as an editor manipulation test.
    document.entities.push_back(make_box_body(
        "entity.physics-showcase-ground", "Static Ground", {0.0, -0.55, 0.0},
        {18.0, 0.55, 14.0}, "static", 1.0, 0.82, 0.02,
        {0.16, 0.20, 0.26}, 0.08, 0.78, {}, {}, {}, false, false, true));
    document.entities.push_back(make_box_body(
        "entity.physics-showcase-wall-left", "Static Side Wall Left",
        {-18.0, 4.0, 0.0}, {0.45, 4.5, 14.0}, "static", 1.0, 0.70, 0.0,
        {0.20, 0.24, 0.30}, 0.05, 0.72));
    document.entities.push_back(make_box_body(
        "entity.physics-showcase-wall-right", "Static Side Wall Right",
        {18.0, 4.0, 0.0}, {0.45, 4.5, 14.0}, "static", 1.0, 0.70, 0.0,
        {0.20, 0.24, 0.30}, 0.05, 0.72));
    document.entities.push_back(make_box_body(
        "entity.physics-showcase-wall-back", "Static Back Wall",
        {0.0, 4.0, -14.0}, {18.0, 4.5, 0.45}, "static", 1.0, 0.70, 0.0,
        {0.14, 0.18, 0.24}, 0.05, 0.76));

    // Three ramps intentionally differ in friction and slope so contact
    // response can be compared side-by-side in the editor and observation UI.
    document.entities.push_back(make_box_body(
        "entity.physics-showcase-ramp-low-friction", "Ramp Low Friction",
        {-11.0, 0.55, -2.5}, {3.0, 0.35, 2.0}, "static", 1.0, 0.12, 0.02,
        {0.16, 0.52, 0.82}, 0.15, 0.34, {0.0, 0.0, -12.0}));
    document.entities.push_back(make_box_body(
        "entity.physics-showcase-ramp-medium-friction", "Ramp Medium Friction",
        {-3.8, 0.55, -2.5}, {3.0, 0.35, 2.0}, "static", 1.0, 0.50, 0.02,
        {0.32, 0.72, 0.48}, 0.10, 0.40, {0.0, 0.0, 12.0}));
    document.entities.push_back(make_box_body(
        "entity.physics-showcase-ramp-high-friction", "Ramp High Friction",
        {3.4, 0.55, -2.5}, {3.0, 0.35, 2.0}, "static", 1.0, 0.92, 0.02,
        {0.78, 0.34, 0.20}, 0.12, 0.38, {0.0, 0.0, -18.0}));

    // Two five-box towers exercise stacking, broad-phase updates, and contact
    // stabilization without tying the fixture to a game-specific mechanic.
    for (std::uint32_t column = 0U; column < 2U; ++column) {
        for (std::uint32_t row = 0U; row < 5U; ++row) {
            const auto id = "entity.physics-showcase-stack-" +
                std::to_string(column) + "-" + std::to_string(row);
            const auto x = column == 0U ? -5.9 : -1.9;
            const auto z = column == 0U ? 1.5 : 0.9;
            const auto color = column == 0U
                ? SceneVector3{0.86, 0.36 + row * 0.05, 0.16}
                : SceneVector3{0.22, 0.48 + row * 0.06, 0.88};
            document.entities.push_back(make_box_body(
                id, "Stacked Box " + std::to_string(column) + "/" +
                    std::to_string(row),
                {x, 0.62 + static_cast<double>(row) * 1.28, z},
                {0.58, 0.58, 0.58}, "dynamic", 1.0 + row * 0.08,
                0.58, 0.04, color, 0.10, 0.42, {}, {},
                {0.0, row % 2U == 0U ? 0.08 : -0.08, 0.0}));
        }
    }

    // A domino chain uses thin, upright boxes with slightly different initial
    // lean and angular velocity, making rotational integration easy to see.
    for (std::uint32_t index = 0U; index < 12U; ++index) {
        const auto id = "entity.physics-showcase-domino-" +
            std::to_string(index);
        const auto x = 4.5 + static_cast<double>(index) * 1.02;
        const auto z = 1.8 + std::sin(static_cast<double>(index) * 0.52) * 0.55;
        const auto tilt = index % 3U == 0U ? -4.0 :
            (index % 3U == 1U ? 0.0 : 4.0);
        document.entities.push_back(make_box_body(
            id, "Domino " + std::to_string(index), {x, 1.02, z},
            {0.16, 1.02, 0.52}, "dynamic", 0.42, 0.74, 0.06,
            {0.92, 0.68, 0.18}, 0.12, 0.30, {0.0, 0.0, tilt}, {},
            {0.0, 0.0, index == 0U ? -0.40 : 0.0}, false, true, true,
            0.035, false, true));
    }

    // Four restitution samples are placed in one row for a clean bounce
    // comparison: damped, rubber, lively, and nearly elastic.
    constexpr std::array<double, 4U> restitutions{{0.05, 0.35, 0.70, 0.95}};
    constexpr std::array<SceneVector3, 4U> ball_colors{{
        {0.72, 0.75, 0.80}, {0.24, 0.78, 0.42},
        {0.95, 0.50, 0.16}, {0.76, 0.24, 0.92}}};
    for (std::uint32_t index = 0U; index < restitutions.size(); ++index) {
        document.entities.push_back(make_sphere_body(
            "entity.physics-showcase-bounce-ball-" + std::to_string(index),
            "Bounce Ball e=" + std::to_string(restitutions[index]),
            {-11.0 + static_cast<double>(index) * 2.05, 6.3, 5.1}, 0.68,
            "dynamic", 0.9, 0.42, restitutions[index], ball_colors[index],
            {}, {0.0, 0.3 * static_cast<double>(index), 0.0}));
    }

    // This projectile is the explicit CCD fixture.  The linear speed is high
    // enough to cross thin geometry in a single coarse step; the advanced
    // body option is consumed when available in the authoring contract.
    document.entities.push_back(make_sphere_body(
        "entity.physics-showcase-ccd-projectile", "High Speed CCD Projectile",
        {-13.5, 1.0, 7.2}, 0.32, "dynamic", 0.55, 0.36, 0.25,
        {1.0, 0.10, 0.08}, {30.0, 0.0, 0.0}, {0.0, 8.0, 0.0}, false,
        true, false));

    document.entities.push_back(make_box_body(
        "entity.physics-showcase-kinematic-platform", "Kinematic Moving Platform",
        {8.0, 2.2, -4.5}, {2.6, 0.28, 1.6}, "kinematic", 1.0, 0.70, 0.02,
        {0.18, 0.62, 0.88}, 0.18, 0.30));
    document.entities.back().platform_2d = ScenePlatform2D{
        "solid", {1.0, 0.0, 0.0}, 4.0, 3.4, 0.0};

    auto trigger = make_box_body(
        "entity.physics-showcase-trigger", "Trigger Sensor Volume",
        {8.0, 1.35, 4.8}, {2.0, 1.35, 1.4}, "static", 1.0, 0.0, 0.0,
        {0.04, 0.32, 0.46}, 0.0, 0.32, {}, {}, {}, true, false, false);
    trigger.pbr_material->emissive_color = {0.04, 0.52, 0.92};
    trigger.pbr_material->emissive_intensity = 2.0;
    document.entities.push_back(std::move(trigger));

    document.entities.push_back(make_capsule_body(
        "entity.physics-showcase-capsule-a", "Capsule Tester A",
        {12.0, 3.2, -0.4}, 0.42, 0.78, {0.24, 0.82, 0.72}));
    document.entities.push_back(make_capsule_body(
        "entity.physics-showcase-capsule-b", "Capsule Tester B",
        {14.0, 4.8, -0.4}, 0.42, 0.78, {0.42, 0.66, 0.96}));
    document.entities.push_back(make_convex_body(
        "entity.physics-showcase-convex", "Convex Hull Tester", {11.2, 2.0, 3.0}));

    return document;
}

} // namespace noemancer
