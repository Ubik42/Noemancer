#include "engine/simulation_runtime.hpp"

#include <cmath>
#include <iostream>
#include <vector>

int main() {
    using namespace noemancer;
    PhysicsRuntime physics;
    std::vector<PhysicsBodyState> bodies{
        {.entity_id = "dynamic", .motion_type = PhysicsMotionType::dynamic_body, .position_y = 1.0F,
         .half_x = 0.5F, .half_y = 0.5F, .half_z = 0.5F, .gravity_factor = 1.0F, .linear_damping = 0.0F},
        {.entity_id = "ground", .motion_type = PhysicsMotionType::static_body, .position_y = 0.0F,
         .half_x = 5.0F, .half_y = 0.05F, .half_z = 5.0F, .gravity_factor = 0.0F}
    };
    for (int step = 0; step < 120; ++step) physics.step(bodies, 1.0F / 60.0F);
    if (std::abs(bodies[0].position_y - 0.55F) > 0.03F || physics.contacts().empty()) {
        std::cerr << "Dynamic body did not settle on the static collider: y=" << bodies[0].position_y
                  << " contacts=" << physics.contacts().size() << '\n';
        return 1;
    }
    const auto ray_hit = physics.ray_cast(0.0F, 3.0F, 0.0F, 0.0F, -5.0F, 0.0F);
    if (!ray_hit.hit || ray_hit.entity_id != "dynamic" || ray_hit.fraction <= 0.0F || ray_hit.fraction >= 1.0F) {
        std::cerr << "Jolt ray cast did not resolve the nearest stable entity\n";
        return 4;
    }
    const auto sweep_hit=physics.sphere_sweep(0.0F,3.0F,0.0F,0.0F,-5.0F,0.0F,0.25F);
    if(!sweep_hit.hit||sweep_hit.entity_id!="dynamic"||sweep_hit.fraction<=0.0F||sweep_hit.fraction>=ray_hit.fraction||
       !std::isfinite(sweep_hit.normal_y)) {
        std::cerr << "Jolt sphere sweep did not resolve an earlier stable contact\n";
        return 9;
    }
    const auto ignored_sweep=physics.sphere_sweep(0.0F,3.0F,0.0F,0.0F,-5.0F,0.0F,0.25F,"dynamic");
    if(!ignored_sweep.hit||ignored_sweep.entity_id!="ground") {
        std::cerr << "Jolt sphere sweep did not honor the ignored body filter\n";
        return 10;
    }
    bodies[0].position_x = 2.0F;
    bodies[0].position_y = 2.0F;
    bodies[0].velocity_x = bodies[0].velocity_y = bodies[0].velocity_z = 0.0F;
    physics.step(bodies, 1.0F / 60.0F);
    const auto teleported_hit = physics.ray_cast(2.0F, 3.0F, 0.0F, 0.0F, -5.0F, 0.0F);
    if (std::abs(bodies[0].position_x - 2.0F) > 0.001F || !teleported_hit.hit || teleported_hit.entity_id != "dynamic") {
        std::cerr << "External ECS transform did not synchronize back into Jolt\n";
        return 5;
    }
    PhysicsRuntime rotated_physics;
    std::vector<PhysicsBodyState> rotated{{.entity_id="rotated",.motion_type=PhysicsMotionType::static_body,
        .position_y=1.0F,.rotation_y=0.38268343F,.rotation_w=0.92387953F,.half_x=1.0F,.half_y=0.25F,.half_z=0.5F,.gravity_factor=0.0F}};
    rotated_physics.step(rotated,0.0F);
    if(std::abs(rotated[0].rotation_y-0.38268343F)>0.0001F||std::abs(rotated[0].rotation_w-0.92387953F)>0.0001F) {
        std::cerr<<"Jolt body rotation did not round-trip through the physics bridge\n";return 13;
    }

    PhysicsRuntime sphere_physics;
    std::vector<PhysicsBodyState> sphere_bodies{
        {.entity_id="sphere", .motion_type=PhysicsMotionType::dynamic_body, .shape_type=PhysicsShapeType::sphere,
         .position_y=2.0F, .radius=0.75F, .gravity_factor=1.0F, .linear_damping=0.0F, .restitution=0.1F},
        {.entity_id="sphere-ground", .motion_type=PhysicsMotionType::static_body, .position_y=0.0F,
         .half_x=5.0F, .half_y=0.05F, .half_z=5.0F, .gravity_factor=0.0F}
    };
    for (int step=0;step<180;++step) sphere_physics.step(sphere_bodies,1.0F/60.0F);
    const auto sphere_hit=sphere_physics.ray_cast(0.0F,3.0F,0.0F,0.0F,-5.0F,0.0F);
    if (std::abs(sphere_bodies[0].position_y-0.80F)>0.04F || !sphere_hit.hit || sphere_hit.entity_id!="sphere") {
        std::cerr << "Jolt sphere collider did not settle or ray-cast correctly: y=" << sphere_bodies[0].position_y << '\n';
        return 7;
    }

    PhysicsRuntime capsule_physics;
    std::vector<PhysicsBodyState> capsule_bodies{
        {.entity_id="capsule", .motion_type=PhysicsMotionType::dynamic_body, .shape_type=PhysicsShapeType::capsule,
         .position_y=2.0F, .radius=0.35F, .half_height=0.65F, .gravity_factor=1.0F, .linear_damping=0.0F},
        {.entity_id="capsule-ground", .motion_type=PhysicsMotionType::static_body, .position_y=0.0F,
         .half_x=5.0F, .half_y=0.05F, .half_z=5.0F, .gravity_factor=0.0F}
    };
    for (int step=0;step<180;++step) capsule_physics.step(capsule_bodies,1.0F/60.0F);
    const auto capsule_hit=capsule_physics.ray_cast(0.0F,3.0F,0.0F,0.0F,-5.0F,0.0F);
    if (std::abs(capsule_bodies[0].position_y-1.05F)>0.06F || !capsule_hit.hit || capsule_hit.entity_id!="capsule") {
        std::cerr << "Jolt capsule collider did not settle or ray-cast correctly: y=" << capsule_bodies[0].position_y << '\n';
        return 7;
    }

    // CharacterMotor2D bodies use the explicit 2D constraint at the physics
    // bridge. It must keep depth and all tilt axes locked without changing
    // the default six-DOF behavior of ordinary 3D bodies.
    PhysicsRuntime constrained_physics;
    const auto tilt_angle = 0.45F;
    std::vector<PhysicsBodyState> constrained_bodies{
        {.entity_id="constrained-2d", .motion_type=PhysicsMotionType::dynamic_body,
         .position_y=2.0F, .position_z=1.0F,
         .rotation_x=std::sin(tilt_angle * 0.5F), .rotation_w=std::cos(tilt_angle * 0.5F),
         .velocity_z=3.0F,
         .half_x=0.6F, .half_y=0.45F, .half_z=0.25F, .gravity_factor=1.0F,
         .linear_damping=0.0F, .constrain_to_2d=true},
        {.entity_id="constrained-ground", .motion_type=PhysicsMotionType::static_body,
         .position_y=0.0F, .half_x=5.0F, .half_y=0.05F, .half_z=5.0F,
         .gravity_factor=0.0F}
    };
    for (int step = 0; step < 120; ++step) constrained_physics.step(constrained_bodies, 1.0F / 60.0F);
    if (std::abs(constrained_bodies[0].position_z - 1.0F) > 0.001F ||
        std::abs(constrained_bodies[0].rotation_x - std::sin(tilt_angle * 0.5F)) > 0.001F ||
        std::abs(constrained_bodies[0].rotation_y) > 0.001F || std::abs(constrained_bodies[0].rotation_z) > 0.001F) {
        std::cerr << "2D constrained body leaked depth or tilt: z=" << constrained_bodies[0].position_z
                  << " rotation=(" << constrained_bodies[0].rotation_x << "," << constrained_bodies[0].rotation_y
                  << "," << constrained_bodies[0].rotation_z << ")\n";
        return 20;
    }

    PhysicsRuntime three_d_physics;
    std::vector<PhysicsBodyState> three_d_bodies{
        {.entity_id="ordinary-3d", .motion_type=PhysicsMotionType::dynamic_body,
         .position_y=2.0F, .position_z=1.0F,
         .rotation_x=std::sin(tilt_angle * 0.5F), .rotation_w=std::cos(tilt_angle * 0.5F),
         .velocity_z=3.0F,
         .half_x=0.6F, .half_y=0.45F, .half_z=0.25F, .gravity_factor=0.0F,
         .linear_damping=0.0F},
    };
    three_d_physics.step(three_d_bodies, 1.0F / 60.0F);
    if (three_d_bodies[0].position_z <= 1.0F || std::abs(three_d_bodies[0].rotation_x - std::sin(tilt_angle * 0.5F)) > 0.001F) {
        std::cerr << "Ordinary 3D rigid body unexpectedly inherited the 2D constraint\n";
        return 21;
    }

    PhysicsRuntime convex_physics;
    const std::vector<std::array<float,3>> hull{{{-0.5F,-0.5F,-0.5F}},{{0.5F,-0.5F,-0.5F}},{{-0.5F,0.5F,-0.5F}},{{0.5F,0.5F,-0.5F}},
        {{-0.5F,-0.5F,0.5F}},{{0.5F,-0.5F,0.5F}},{{-0.5F,0.5F,0.5F}},{{0.5F,0.5F,0.5F}}};
    std::vector<PhysicsBodyState> convex_bodies{
        {.entity_id="convex",.motion_type=PhysicsMotionType::dynamic_body,.shape_type=PhysicsShapeType::convex_hull,
         .position_y=2.0F,.convex_points=hull,.gravity_factor=1.0F,.linear_damping=0.0F},
        {.entity_id="convex-ground",.motion_type=PhysicsMotionType::static_body,.position_y=0.0F,
         .half_x=5.0F,.half_y=0.05F,.half_z=5.0F,.gravity_factor=0.0F}
    };
    for(int step=0;step<180;++step) convex_physics.step(convex_bodies,1.0F/60.0F);
    const auto convex_hit=convex_physics.ray_cast(0.0F,3.0F,0.0F,0.0F,-5.0F,0.0F);
    if(std::abs(convex_bodies[0].position_y-0.55F)>0.08F||!convex_hit.hit||convex_hit.entity_id!="convex") {
        std::cerr<<"Jolt convex hull did not settle or ray-cast correctly: y="<<convex_bodies[0].position_y<<'\n'; return 8;
    }

    PhysicsRuntime one_way_up_physics;
    std::vector<PhysicsBodyState> one_way_up{
        {.entity_id="actor",.motion_type=PhysicsMotionType::dynamic_body,.shape_type=PhysicsShapeType::sphere,
         .position_y=0.0F,.velocity_y=4.0F,.half_y=0.3F,.radius=0.3F,.gravity_factor=0.0F,.linear_damping=0.0F},
        {.entity_id="one-way",.motion_type=PhysicsMotionType::static_body,.position_y=1.0F,
         .half_x=3.0F,.half_y=0.1F,.half_z=1.0F,.gravity_factor=0.0F,.one_way=true}
    };
    for(int step=0;step<30;++step)one_way_up_physics.step(one_way_up,1.0F/60.0F);
    if(one_way_up[0].position_y<1.5F){std::cerr<<"One-way platform blocked ascent from below: y="<<one_way_up[0].position_y<<'\n';return 11;}
    PhysicsRuntime one_way_down_physics;
    std::vector<PhysicsBodyState> one_way_down{
        {.entity_id="actor",.motion_type=PhysicsMotionType::dynamic_body,.shape_type=PhysicsShapeType::sphere,
         .position_y=2.0F,.velocity_y=-2.0F,.half_y=0.3F,.radius=0.3F,.gravity_factor=0.0F,.linear_damping=0.0F},
        {.entity_id="one-way",.motion_type=PhysicsMotionType::static_body,.position_y=1.0F,
         .half_x=3.0F,.half_y=0.1F,.half_z=1.0F,.gravity_factor=0.0F,.one_way=true}
    };
    for(int step=0;step<60;++step)one_way_down_physics.step(one_way_down,1.0F/60.0F);
    if(std::abs(one_way_down[0].position_y-1.4F)>0.08F){std::cerr<<"One-way platform did not support descent from above: y="<<one_way_down[0].position_y<<'\n';return 12;}

    PhysicsRuntime trigger_physics;
    std::vector<PhysicsBodyState> trigger_bodies{
        {.entity_id="trigger-actor",.motion_type=PhysicsMotionType::dynamic_body,.shape_type=PhysicsShapeType::sphere,
         .position_x=-2.0F,.velocity_x=2.0F,.radius=0.25F,.gravity_factor=0.0F,.linear_damping=0.0F},
        {.entity_id="trigger-volume",.motion_type=PhysicsMotionType::static_body,.position_x=0.0F,
         .half_x=0.5F,.half_y=1.0F,.half_z=1.0F,.gravity_factor=0.0F,.is_trigger=true}
    };
    bool observed_trigger=false;
    for(int step=0;step<120;++step){
        trigger_physics.step(trigger_bodies,1.0F/60.0F);
        observed_trigger=observed_trigger||std::ranges::any_of(trigger_physics.contacts(),[](const auto& contact){return contact.is_trigger;});
    }
    if(!observed_trigger||trigger_bodies[0].position_x<1.8F){
        std::cerr<<"Jolt sensor did not report a trigger or incorrectly blocked motion: x="<<trigger_bodies[0].position_x<<'\n';return 14;
    }

    // The plain-data bridge must preserve angular velocity and the authored
    // CCD/sleep policy while still stepping a regular dynamic body.
    PhysicsRuntime angular_physics;
    std::vector<PhysicsBodyState> angular_bodies{
        {.entity_id="angular", .motion_type=PhysicsMotionType::dynamic_body,
         .position_y=2.0F, .gravity_factor=0.0F, .linear_damping=0.0F,
         .angular_velocity_x=1.5F, .angular_velocity_y=-0.5F,
         .angular_velocity_z=0.25F, .angular_damping=0.0F,
         .continuous_collision=true, .allow_sleeping=false},
    };
    angular_physics.step(angular_bodies, 0.0F);
    if (std::abs(angular_bodies[0].angular_velocity_x - 1.5F) > 0.02F ||
        std::abs(angular_bodies[0].angular_velocity_y + 0.5F) > 0.02F ||
        std::abs(angular_bodies[0].angular_velocity_z - 0.25F) > 0.02F) {
        std::cerr << "Jolt angular velocity did not round-trip through the body bridge\n";
        return 22;
    }
    angular_physics.step(angular_bodies, 1.0F / 60.0F);
    if (!std::isfinite(angular_bodies[0].position_x) ||
        !std::isfinite(angular_bodies[0].angular_velocity_x) ||
        !std::isfinite(angular_bodies[0].angular_velocity_y) ||
        !std::isfinite(angular_bodies[0].angular_velocity_z)) {
        std::cerr << "Jolt CCD/sleep policy produced a non-finite dynamic step\n";
        return 23;
    }

    PhysicsRuntime impulse_physics;
    std::vector<PhysicsBodyState> impulse_bodies{
        {.entity_id="impulse", .motion_type=PhysicsMotionType::dynamic_body,
         .position_y=2.0F, .gravity_factor=0.0F, .linear_damping=0.0F,
         .angular_damping=0.0F, .allow_sleeping=false},
        {.entity_id="impulse-static", .motion_type=PhysicsMotionType::static_body,
         .position_y=-2.0F, .gravity_factor=0.0F},
    };
    impulse_physics.step(impulse_bodies, 0.0F);
    if (!impulse_physics.apply_force("impulse", 60.0F, 0.0F, 0.0F) ||
        !impulse_physics.apply_impulse("impulse", 2.0F, 0.0F, 0.0F) ||
        !impulse_physics.apply_angular_impulse("impulse", 0.0F, 0.0F, 1.0F)) {
        std::cerr << "Jolt dynamic force/impulse command unexpectedly failed\n";
        return 24;
    }
    impulse_physics.step(impulse_bodies, 1.0F / 60.0F);
    if (impulse_bodies[0].velocity_x <= 2.0F ||
        std::abs(impulse_bodies[0].angular_velocity_z) <= 0.1F) {
        std::cerr << "Jolt force/impulse commands did not change dynamic velocity: linear="
                  << impulse_bodies[0].velocity_x << " angular="
                  << impulse_bodies[0].angular_velocity_z << '\n';
        return 25;
    }
    if (impulse_physics.apply_force("missing", 1.0F, 0.0F, 0.0F) ||
        impulse_physics.apply_impulse("impulse-static", 1.0F, 0.0F, 0.0F) ||
        impulse_physics.apply_angular_impulse("missing", 0.0F, 1.0F, 0.0F)) {
        std::cerr << "Jolt force/impulse command accepted an unknown or static body\n";
        return 26;
    }

    // Collision categories are evaluated by the Jolt pair/broadphase filters,
    // while query filters apply the same symmetric layer/mask rule to native
    // ray, shape-cast and overlap queries. The default filter remains category
    // 1 against all categories for backwards-compatible scenes.
    PhysicsRuntime query_physics;
    std::vector<PhysicsBodyState> query_bodies{
        {.entity_id="layer-two", .motion_type=PhysicsMotionType::static_body,
         .position_x=0.0F, .half_x=0.5F, .half_y=0.5F, .half_z=0.5F, .gravity_factor=0.0F},
        {.entity_id="sweep-box", .motion_type=PhysicsMotionType::static_body,
         .position_x=3.0F, .half_x=0.5F, .half_y=0.5F, .half_z=0.5F, .gravity_factor=0.0F},
        {.entity_id="sweep-capsule", .motion_type=PhysicsMotionType::static_body,
         .shape_type=PhysicsShapeType::capsule, .position_x=6.0F, .radius=0.35F,
         .half_height=0.65F, .gravity_factor=0.0F},
        {.entity_id="overlap-sphere", .motion_type=PhysicsMotionType::static_body,
         .shape_type=PhysicsShapeType::sphere, .position_y=3.0F, .radius=0.75F, .gravity_factor=0.0F},
        {.entity_id="overlap-capsule", .motion_type=PhysicsMotionType::static_body,
         .shape_type=PhysicsShapeType::capsule, .position_y=-3.0F, .radius=0.4F,
         .half_height=0.6F, .gravity_factor=0.0F},
    };
    query_bodies[0].collision_layer = 2U;
    query_bodies[0].collision_mask = 2U;
    query_physics.step(query_bodies, 0.0F);
    if (query_physics.ray_cast(-2.0F, 0.0F, 0.0F, 1.5F, 0.0F, 0.0F).hit) {
        std::cerr << "Default query filter unexpectedly hit a category-2-only body\n";
        return 27;
    }
    const PhysicsQueryFilter layer_two_filter{.layer=2U, .mask=2U};
    const auto filtered_ray = query_physics.ray_cast(-2.0F, 0.0F, 0.0F,
        5.0F, 0.0F, 0.0F, layer_two_filter);
    if (!filtered_ray.hit || filtered_ray.entity_id != "layer-two") {
        std::cerr << "Layer/mask query filter did not expose the matching body\n";
        return 28;
    }
    const auto box_hit = query_physics.box_sweep(-1.0F, 0.0F, 0.0F,
        6.0F, 0.0F, 0.0F, 0.25F, 0.25F, 0.25F);
    if (!box_hit.hit || box_hit.entity_id != "sweep-box" ||
        box_hit.fraction <= 0.0F || box_hit.fraction >= 1.0F ||
        !std::isfinite(box_hit.normal_x) || !std::isfinite(box_hit.penetration_depth)) {
        std::cerr << "Jolt box sweep did not return a stable hit payload\n";
        return 29;
    }
    const auto capsule_sweep_hit = query_physics.capsule_sweep(4.0F, 0.0F, 0.0F,
        4.0F, 0.0F, 0.0F, 0.2F, 0.4F);
    if (!capsule_sweep_hit.hit || capsule_sweep_hit.entity_id != "sweep-capsule" ||
        capsule_sweep_hit.fraction <= 0.0F || capsule_sweep_hit.fraction >= 1.0F ||
        !std::isfinite(capsule_sweep_hit.normal_x)) {
        std::cerr << "Jolt capsule sweep did not return the stable entity id\n";
        return 30;
    }
    const auto sphere_overlaps = query_physics.overlap_sphere(0.0F, 3.0F, 0.0F, 1.0F);
    if (sphere_overlaps.hits.empty() || sphere_overlaps.hits.front().entity_id != "overlap-sphere" ||
        sphere_overlaps.hits.size() > physics_query_maximum_overlap_hits ||
        !std::isfinite(sphere_overlaps.hits.front().penetration_depth)) {
        std::cerr << "Sphere overlap did not return a bounded stable hit\n";
        return 31;
    }
    const auto capsule_overlaps = query_physics.overlap_capsule(0.0F, -3.0F, 0.0F, 0.5F, 0.5F);
    if (capsule_overlaps.hits.empty() || capsule_overlaps.hits.front().entity_id != "overlap-capsule") {
        std::cerr << "Capsule overlap did not resolve its target\n";
        return 32;
    }
    std::array<std::string_view, 1> ignored_overlap{"overlap-sphere"};
    const PhysicsQueryFilter ignored_filter{.ignored_entity_ids=ignored_overlap};
    const auto box_overlaps = query_physics.overlap_box(0.0F, 0.0F, 0.0F,
        1.0F, 1.0F, 1.0F, ignored_filter, 1U);
    if (std::ranges::any_of(box_overlaps.hits, [](const PhysicsOverlapHit& hit) {
            return hit.entity_id == "overlap-sphere";
        }) || box_overlaps.hits.size() > 1U) {
        std::cerr << "Box overlap did not honor ignored ids or the result bound\n";
        return 33;
    }

    AnimationRuntime animation;
    auto time = animation.advance_time(0.0F, 0.5F, 1.0F, true, true, "asset.animation.test-bob");
    if (std::abs(time - 0.5F) > 0.0001F || std::abs(animation.sample_translation_y("asset.animation.test-bob", time) - 0.22F) > 0.0001F) {
        std::cerr << "Keyframe animation sampling is incorrect\n";
        return 2;
    }
    time = animation.advance_time(1.75F, 0.5F, 1.0F, true, true, "asset.animation.test-bob");
    if (std::abs(time - 0.25F) > 0.0001F) {
        std::cerr << "Looping animation cursor is incorrect\n";
        return 3;
    }
    const auto pose_start = animation.sample_skeletal_pose("asset.animation.test-bob", 0.0F);
    const auto pose_peak = animation.sample_skeletal_pose("asset.animation.test-bob", 0.5F);
    if (!pose_start.valid || !pose_peak.valid || pose_start.skinning_matrices.size() != 2U ||
        pose_peak.skinning_matrices.size() != 2U ||
        std::abs(pose_start.skinning_matrices[1][1] - pose_peak.skinning_matrices[1][1]) < 0.01F) {
        std::cerr << "ozz skeletal pose did not sample the two-joint clip\n";
        return 6;
    }

    noemancer::AnimationPoseExecutionRequest layered_request{
        .base_clip_asset = "asset.animation.test-bob",
        .base_time = 0.0F,
        .layers = {
            {.id = "upper-override", .clip_asset = "asset.animation.test-bob", .time = 0.0F,
             .secondary_clip_asset = "asset.animation.test-bob", .secondary_time = 0.5F, .secondary_weight = 1.0F,
             .mode = noemancer::AnimationPoseLayerMode::override_layer, .weight = 1.0F,
             .mask_id = "upper", .joint_weights = {1.0F, 0.5F}},
            {.id = "upper-additive", .clip_asset = "asset.animation.test-bob", .time = 0.5F,
             .mode = noemancer::AnimationPoseLayerMode::additive, .weight = 0.5F,
             .mask_id = "upper", .joint_weights = {1.0F, 0.5F}}},
        .masks = {{.id = "upper", .include_descendants = false,
                   .joints = {{.name = "upper", .weight = 1.0F}}}}};
    const auto layered = animation.sample_layered_skeletal_pose(layered_request);
    const auto layered_repeat = animation.sample_layered_skeletal_pose(layered_request);
    if (!layered.success || !layered_repeat.success || layered.code != "ok" ||
        layered.pose.skinning_matrices.size() != 2U || layered.pose.joints.size() != 2U ||
        std::abs(layered.pose.skinning_matrices[0][0] - 1.0F) > 0.0001F ||
        std::abs(layered.pose.skinning_matrices[0][1]) > 0.0001F ||
        std::abs(layered.pose.skinning_matrices[1][1] - layered_repeat.pose.skinning_matrices[1][1]) > 0.000001F ||
        std::abs(layered.pose.skinning_matrices[1][1] - 0.104F) > 0.02F) {
        std::cerr << "Local-space masked override/additive layering was not deterministic\n";
        return 15;
    }
    const auto two_source_base = animation.sample_layered_skeletal_pose({
        .base_clip_asset = "asset.animation.test-bob", .base_time = 0.0F,
        .base_secondary_clip_asset = "asset.animation.test-bob", .base_secondary_time = 0.5F,
        .base_secondary_weight = 0.5F});
    if (!two_source_base.success || std::abs(two_source_base.pose.skinning_matrices[1][1]) > 0.002F) {
        std::cerr << "Base Blend 1D sources were not combined in local space\n";
        return 18;
    }
    const auto missing_layer = animation.sample_layered_skeletal_pose({
        .base_clip_asset = "asset.animation.test-bob",
        .layers = {{.id = "missing", .clip_asset = "asset.animation.not-registered"}}});
    if (missing_layer.success || missing_layer.code != "animation.pose.layer-clip-unavailable" ||
        missing_layer.detail.find("missing") == std::string::npos) {
        std::cerr << "Layer execution did not expose a stable missing-clip diagnostic\n";
        return 16;
    }
    const auto bad_mask = animation.sample_layered_skeletal_pose({
        .base_clip_asset = "asset.animation.test-bob",
        .layers = {{.id = "bad-mask", .clip_asset = "asset.animation.test-bob", .mask_id = "missing-mask"}}});
    if (bad_mask.success || bad_mask.code != "animation.pose.mask-not-found") {
        std::cerr << "Layer execution did not expose a stable missing-mask diagnostic\n";
        return 17;
    }
    const auto bad_weights = animation.sample_layered_skeletal_pose({
        .base_clip_asset = "asset.animation.test-bob",
        .layers = {{.id = "bad-weights", .clip_asset = "asset.animation.test-bob", .joint_weights = {1.0F}}}});
    if (bad_weights.success || bad_weights.code != "animation.pose.joint-weight-count") {
        std::cerr << "Layer execution did not expose a stable per-joint weight diagnostic\n";
        return 19;
    }
    return 0;
}
