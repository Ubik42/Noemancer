#include "engine/character_motor_2d.hpp"
#include "engine/scene_document.hpp"
#include "engine/world.hpp"

#include <nlohmann/json.hpp>

#include <cmath>
#include <iostream>

int main() {
    using namespace noemancer;
    const CharacterMotor2DConfig config;
    CharacterMotor2DState state;
    state.grounded = true;
    state.coyote_remaining = config.coyote_time_seconds;
    auto moved = update_character_motor_2d(config, state, {.move=1.0F,.ground_hit=true,.ground_normal_y=1.0F,.ground_entity_id="ground"}, 0.0F, 0.0F, 1.0F / 60.0F);
    if (moved.velocity_x <= 0.0F || moved.velocity_x > config.maximum_speed || moved.state.decision != "ground-move") {
        std::cerr << "Ground acceleration decision is invalid\n"; return 1;
    }
    auto jumped = update_character_motor_2d(config, moved.state, {.move=1.0F,.jump_held=true,.ground_hit=true,.ground_normal_y=1.0F,.ground_entity_id="ground"},
        moved.velocity_x, 0.0F, 1.0F / 60.0F);
    if (jumped.velocity_y != config.jump_speed || jumped.state.grounded || jumped.state.jump_count != 1 || jumped.state.decision != "jump") {
        std::cerr << "Buffered ground jump is invalid\n"; return 2;
    }
    auto coyote = jumped.state;
    coyote.jump_was_held = false;
    coyote.coyote_remaining = 0.05F;
    auto coyote_jump = update_character_motor_2d(config, coyote, {.jump_held=true,.ground_normal_y=1.0F},
        0.0F, -1.0F, 1.0F / 60.0F);
    if (coyote_jump.velocity_y != config.jump_speed || coyote_jump.state.reason != "coyote-window") {
        std::cerr << "Coyote jump window is invalid\n"; return 3;
    }
    auto cut=update_character_motor_2d(config,jumped.state,{.move=1.0F,.jump_held=false,.ground_normal_y=1.0F},
        jumped.velocity_x,jumped.velocity_y,1.0F/60.0F);
    if(cut.state.decision!="jump-cut"||cut.velocity_y>=jumped.velocity_y||cut.velocity_y<=0.0F) {
        std::cerr<<"Variable jump release did not cut ascent\n";return 8;
    }

    const auto parsed = SceneDocumentCodec::parse_json(R"({"schema":"noemancer.scene/0.1","sceneGuid":"scene.motor","name":"Motor","entities":[{"guid":"camera","name":"Camera","parent":null,"components":{"Transform":{"position":[0,2,12]},"Camera":{"target":[0,0,0],"verticalFovDegrees":45,"nearClip":0.1,"farClip":100,"primary":true,"projection":"orthographic","orthographicHeight":10},"CameraFollow2D":{"targetEntityId":"player","positionOffset":[0,2,12],"deadZone":[0,0,0],"lookAheadDistance":1,"smoothing":20,"minimumCenter":[-10,-10,-10],"maximumCenter":[10,10,10]}}},{"guid":"ground","name":"Ground","parent":null,"components":{"Transform":{"position":[0,-0.5,0]},"RigidBody":{"motionType":"static","mass":1,"gravityFactor":0,"linearDamping":0},"BoxCollider":{"halfExtents":[8,0.5,1],"friction":0.9,"restitution":0}}},{"guid":"player","name":"Player","parent":null,"components":{"Transform":{"position":[0,0.96,0]},"Velocity":{"linear":[0,0,0]},"RigidBody":{"motionType":"dynamic","mass":1,"gravityFactor":1,"linearDamping":0},"CapsuleCollider":{"radius":0.42,"halfHeight":0.5,"friction":0.2,"restitution":0},"CharacterMotor2D":{"maximumSpeed":7,"groundAcceleration":55,"airAcceleration":24,"groundDeceleration":70,"jumpSpeed":10.5,"maximumFallSpeed":24,"coyoteTimeSeconds":0.1,"jumpBufferSeconds":0.12,"groundProbeDistance":0.18,"minimumGroundNormalY":0.65,"jumpReleaseVelocityFactor":0.45}}}]})");
    if (!parsed || !parsed.document->entities[2].character_motor_2d ||!parsed.document->entities[0].camera_follow_2d||
        SceneDocumentCodec::write_canonical_json(*parsed.document).find("CameraFollow2D") == std::string::npos) {
        std::cerr << "CharacterMotor2D scene contract did not round-trip\n"; return 4;
    }
    World world;
    if (!world.load_scene(*parsed.document).success) return 5;
    world.tick(1.0F / 60.0F);
    static_cast<void>(world.inject_input_json("keyboard.d", 1.0F));
    world.tick(1.0F / 60.0F);
    auto observation = nlohmann::json::parse(world.character_motor_2d_observation_json("player"));
    if (observation.at("motors").size() != 1 || observation.at("motors").at(0).at("velocity").at("x").get<float>() <= 0.0F ||
        observation.at("motors").at(0).at("decision").at("kind") != "ground-move") {
        std::cerr << "World input did not drive CharacterMotor2D through Jolt: " << observation.dump() << "\n"; return 6;
    }
    static_cast<void>(world.inject_input_json("keyboard.space", 1.0F));
    world.tick(1.0F / 60.0F);
    observation = nlohmann::json::parse(world.character_motor_2d_observation_json("player"));
    if (observation.at("motors").at(0).at("decision").at("kind") != "jump" ||
        observation.at("motors").at(0).at("velocity").at("y").get<float>() <= 0.0F ||
        observation.at("motors").at(0).at("counters").at("jumps") != 1) {
        std::cerr << "Jump decision was not applied or observed\n"; return 7;
    }
    const auto camera=nlohmann::json::parse(world.camera_follow_2d_observation_json("camera"));
    if(camera.at("follows").size()!=1||camera.at("follows").at(0).at("decision")!="follow"||
        camera.at("follows").at(0).at("cameraTarget").at("x").get<float>()<=0.0F) {
        std::cerr<<"CameraFollow2D did not follow the moving target\n";return 9;
    }
    return 0;
}
