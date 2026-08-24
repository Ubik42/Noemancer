#include "engine/world.hpp"

#include "engine/retained_ui_runtime.hpp"
#include "engine/network_replication.hpp"
#include "engine/transform_math.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cctype>
#include <cmath>
#include <iomanip>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <limits>
#include <sstream>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace noemancer {
namespace {

using Json = nlohmann::json;

bool valid_gameplay_tag(const std::string_view value) {
    return !value.empty()&&value.size()<=128&&std::ranges::all_of(value,[](const unsigned char character) {
        return (character>='a'&&character<='z')||(character>='0'&&character<='9')||character=='.'||character=='_'||character=='-';
    });
}

std::string contact_key(std::string_view first,std::string_view second) {
    if(second<first)std::swap(first,second);
    return std::string(first)+'\x1f'+std::string(second);
}

double semantic_f32(const float value) {
    char buffer[32]{};
    const auto [end, error] = std::to_chars(
        buffer,
        buffer + sizeof(buffer),
        value,
        std::chars_format::general);
    if (error != std::errc{}) {
        return static_cast<double>(value);
    }
    double result{};
    const auto parsed = std::from_chars(buffer, end, result, std::chars_format::general);
    return parsed.ec == std::errc{} ? result : static_cast<double>(value);
}

Json semantic_ref_json(
    const SemanticIdentity& identity,
    const std::uint64_t revision) {
    return {
        {"id", identity.id},
        {"path", identity.path},
        {"type", identity.type},
        {"schemaRef", identity.schema_ref},
        {"displayName", identity.display_name},
        {"revision", revision}
    };
}

Json vector_json(const SemanticVector3& value) {
    return {{"x", value.x}, {"y", value.y}, {"z", value.z}};
}

Json rotation_euler_json(const Transform& value) {
    const auto euler=euler_degrees_from_quaternion({value.rotation_x,value.rotation_y,value.rotation_z,value.rotation_w});
    return {{"x",euler.x},{"y",euler.y},{"z",euler.z}};
}

SemanticVector3 semantic_vector(const Transform value) {
    return {value.x, value.y, value.z};
}

Transform engine_transform(const SemanticVector3& value) {
    return {
        static_cast<float>(value.x),
        static_cast<float>(value.y),
        static_cast<float>(value.z)
    };
}

bool same_transform(const Transform left, const SemanticVector3& right) {
    return left.x == static_cast<float>(right.x) &&
        left.y == static_cast<float>(right.y) &&
        left.z == static_cast<float>(right.z);
}

std::string plan_hash(
    const std::string_view manager,
    const std::string_view entity_id,
    const std::uint64_t revision,
    const SemanticVector3& before,
    const SemanticVector3& after) {
    std::ostringstream source;
    source << manager << '\n' << entity_id << '\n' << revision << '\n'
           << std::setprecision(17)
           << before.x << ',' << before.y << ',' << before.z << '\n'
           << after.x << ',' << after.y << ',' << after.z;
    std::uint64_t hash = 1469598103934665603ULL;
    for (const unsigned char character : source.str()) {
        hash ^= character;
        hash *= 1099511628211ULL;
    }
    std::ostringstream output;
    output << "fnv1a64:" << std::hex << std::setfill('0') << std::setw(16) << hash;
    return output.str();
}

std::string property_plan_hash(const std::string_view manager, const std::string_view entity_id,
                               const std::string_view property, const std::uint64_t revision,
                               const std::string_view before, const std::string_view after) {
    std::ostringstream source;
    source << manager << '\n' << entity_id << '\n' << property << '\n' << revision << '\n' << before << '\n' << after;
    std::uint64_t hash = 1469598103934665603ULL;
    for (const unsigned char character : source.str()) { hash ^= character; hash *= 1099511628211ULL; }
    std::ostringstream output;
    output << "fnv1a64:" << std::hex << std::setfill('0') << std::setw(16) << hash;
    return output.str();
}

std::string next_world_operation_id() {
    static std::atomic<std::uint64_t> counter{0};
    const auto timestamp = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    std::ostringstream output;
    output << "world_op_" << std::hex << timestamp << '-' << std::dec << ++counter;
    return output.str();
}

Json delta_to_json(const SemanticDelta& delta) {
    Json result = {
        {"revisionBefore", delta.revision_before},
        {"revisionAfter", delta.revision_after},
        {"entityId", delta.entity_id},
        {"field", delta.field},
        {"manager", delta.manager},
        {"undoable", delta.undoable}
    };
    if (!delta.before_value_json.empty() && !delta.after_value_json.empty()) {
        result["before"] = Json::parse(delta.before_value_json);
        result["after"] = Json::parse(delta.after_value_json);
    } else {
        result["before"] = vector_json(delta.before);
        result["after"] = vector_json(delta.after);
    }
    return result;
}

PhysicsMotionType motion_type(const std::string_view value) {
    if (value == "static") return PhysicsMotionType::static_body;
    if (value == "kinematic") return PhysicsMotionType::kinematic_body;
    return PhysicsMotionType::dynamic_body;
}

std::string_view motion_type_name(const PhysicsMotionType value) {
    if (value == PhysicsMotionType::static_body) return "static";
    if (value == PhysicsMotionType::kinematic_body) return "kinematic";
    return "dynamic";
}

struct InspectorPropertySchema final {
    std::string component, field, property, label, value_type, control;
    Json constraints;
};

const std::vector<InspectorPropertySchema>& inspector_property_schemas() {
    static const std::vector<InspectorPropertySchema> schemas{
        {"Transform","position","engine.entity.transform.position","Position","vector3","drag",{{"unit","m"},{"step",0.05}}},
        {"Transform","rotationEulerDegrees","engine.entity.transform.rotationEulerDegrees","Rotation","vector3","drag",{{"unit","deg"},{"step",0.25}}},
        {"Transform","scale","engine.entity.transform.scale","Scale","vector3","drag",{{"unit","ratio"},{"minimumExclusive",0.0},{"step",0.05}}},
        {"Velocity","linear","engine.entity.velocity.linear","Linear Velocity","vector3","drag",{{"unit","m/s"},{"step",0.05}}},
        {"PbrMaterial","baseColor","engine.entity.material.baseColor","Base Color","color3","color",{{"minimum",0.0},{"maximum",1.0}}},
        {"PbrMaterial","metallic","engine.entity.material.metallic","Metallic","f32","slider",{{"minimum",0.0},{"maximum",1.0}}},
        {"PbrMaterial","roughness","engine.entity.material.roughness","Roughness","f32","slider",{{"minimum",0.0},{"maximum",1.0}}},
        {"PbrMaterial","emissiveColor","engine.entity.material.emissiveColor","Emissive Color","color3","color",{{"minimum",0.0}}},
        {"PbrMaterial","emissiveIntensity","engine.entity.material.emissiveIntensity","Emissive Intensity","f32","drag",{{"minimum",0.0},{"step",0.1}}},
        {"LocalLight","kind","engine.entity.localLight.kind","Type","enum","combo",{{"options",Json::array({"point","spot"})}}},
        {"LocalLight","color","engine.entity.localLight.color","Color","color3","color",{{"minimum",0.0},{"maximum",1.0}}},
        {"LocalLight","luminousPowerLumens","engine.entity.localLight.luminousPowerLumens","Power","f32","drag",{{"unit","lm"},{"minimum",0.0},{"step",10.0}}},
        {"LocalLight","rangeMeters","engine.entity.localLight.rangeMeters","Range","f32","drag",{{"unit","m"},{"minimumExclusive",0.0},{"step",0.1}}},
        {"LocalLight","direction","engine.entity.localLight.direction","Direction","vector3","drag",{{"step",0.02}}},
        {"LocalLight","innerConeDegrees","engine.entity.localLight.innerConeDegrees","Inner Cone","f32","slider",{{"unit","deg"},{"minimum",0.0},{"maximum",89.0}}},
        {"LocalLight","outerConeDegrees","engine.entity.localLight.outerConeDegrees","Outer Cone","f32","slider",{{"unit","deg"},{"minimum",0.0},{"maximum",89.0}}},
        {"LocalLight","sourceRadiusMeters","engine.entity.localLight.sourceRadiusMeters","Source Radius","f32","drag",{{"unit","m"},{"minimum",0.0},{"step",0.01}}},
        {"BoxCollider","halfExtents","engine.entity.collider.halfExtents","Half Extents","vector3","drag",{{"unit","m"},{"minimumExclusive",0.0},{"step",0.05}}},
        {"SphereCollider","radius","engine.entity.collider.radius","Radius","f32","drag",{{"unit","m"},{"minimumExclusive",0.0},{"step",0.05}}},
        {"CapsuleCollider","halfHeight","engine.entity.collider.halfHeight","Half Height","f32","drag",{{"unit","m"},{"minimumExclusive",0.0},{"step",0.05}}},
        {"Collider","friction","engine.entity.collider.friction","Friction","f32","drag",{{"minimum",0.0},{"step",0.05}}},
        {"Collider","restitution","engine.entity.collider.restitution","Restitution","f32","slider",{{"minimum",0.0},{"maximum",1.0}}},
        {"Collider","isTrigger","engine.entity.collider.isTrigger","Is Trigger","bool","checkbox",Json::object()},
        {"AnimationPlayer","clipAsset","engine.entity.animation.clipAsset","Clip","asset-id","asset",Json::object()},
        {"AnimationPlayer","playbackSpeed","engine.entity.animation.playbackSpeed","Speed","f32","drag",{{"step",0.05}}},
        {"AnimationPlayer","looping","engine.entity.animation.looping","Looping","bool","checkbox",Json::object()},
        {"AnimationPlayer","playing","engine.entity.animation.playing","Playing","bool","checkbox",Json::object()},
        {"AnimationPlayer","nextClipAsset","engine.entity.animation.nextClipAsset","Next Clip","asset-id","asset",Json::object()},
        {"AnimationPlayer","transitionDuration","engine.entity.animation.transitionDuration","Cross-fade","f32","drag",{{"unit","s"},{"minimum",0.0},{"step",0.05}}},
        {"AnimationPlayer","rootMotionMode","engine.entity.animation.rootMotionMode","Root Motion","enum","combo",{{"options",Json::array({"ignore","apply"})}}},
        {"AnimationPlayer","stateMachineAsset","engine.entity.animation.stateMachineAsset","State Machine","asset-id","asset",Json::object()},
        {"AnimationPlayer","animationGraphAsset","engine.entity.animation.animationGraphAsset","Animation Graph","asset-id","asset",Json::object()},
        {"SpriteRenderer","spriteAsset","engine.entity.sprite.spriteAsset","Sprite Asset","asset-id","asset",Json::object()},
        {"SpriteRenderer","clip","engine.entity.sprite.clip","Clip","string","text",Json::object()},
        {"SpriteRenderer","playbackSpeed","engine.entity.sprite.playbackSpeed","Speed","f32","drag",{{"minimum",0.0},{"step",0.05}}},
        {"SpriteRenderer","playing","engine.entity.sprite.playing","Playing","bool","checkbox",Json::object()},
        {"SpriteRenderer","flipX","engine.entity.sprite.flipX","Flip X","bool","checkbox",Json::object()},
        {"SpriteRenderer","flipY","engine.entity.sprite.flipY","Flip Y","bool","checkbox",Json::object()},
        {"SpriteRenderer","sortingLayer","engine.entity.sprite.sortingLayer","Sorting Layer","string","text",Json::object()},
        {"SpriteRenderer","sortingOrder","engine.entity.sprite.sortingOrder","Sorting Order","i32","drag",{{"step",1}}},
        {"SpriteRenderer","visible","engine.entity.sprite.visible","Visible","bool","checkbox",Json::object()},
        {"TilemapRenderer","tilemapAsset","engine.entity.tilemap.tilemapAsset","Tilemap Asset","asset-id","asset",Json::object()},
        {"TilemapRenderer","visible","engine.entity.tilemap.visible","Visible","bool","checkbox",Json::object()},
        {"TilemapRenderer","collisionEnabled","engine.entity.tilemap.collisionEnabled","Collision Enabled","bool","checkbox",Json::object()},
        {"ManagedScript","assemblyAsset","engine.entity.managedScript.assemblyAsset","Assembly","asset-id","text",Json::object()},
        {"ManagedScript","typeName","engine.entity.managedScript.typeName","Behaviour Type","string","text",Json::object()},
        {"ManagedScript","enabled","engine.entity.managedScript.enabled","Enabled","bool","checkbox",Json::object()},
        {"ManagedScript","properties","engine.entity.managedScript.properties","Properties","json","json",Json::object()}
    };
    return schemas;
}

std::optional<PhysicsBodyState> physics_state(const WorldEntityView& view) {
    if (!view.transform || !view.rigid_body || (!view.box_collider && !view.sphere_collider && !view.capsule_collider && !view.convex_hull_collider)) return std::nullopt;
    const auto velocity = view.velocity.value_or(Velocity{});
    PhysicsBodyState state;
    state.entity_id=view.id; state.motion_type=view.rigid_body->motion_type;
    state.position_x=view.transform->x; state.position_y=view.transform->y; state.position_z=view.transform->z;
    state.rotation_x=view.transform->rotation_x;state.rotation_y=view.transform->rotation_y;
    state.rotation_z=view.transform->rotation_z;state.rotation_w=view.transform->rotation_w;
    state.velocity_x=velocity.x; state.velocity_y=velocity.y; state.velocity_z=velocity.z;
    state.gravity_factor=view.rigid_body->gravity_factor; state.linear_damping=view.rigid_body->linear_damping;
    state.mass=view.rigid_body->mass;
    state.one_way=view.platform_2d&&view.platform_2d->collision_mode=="one-way";
    // CharacterMotor2D defines an XY platformer body. Keep this policy at the
    // semantic world bridge so every physics backend receives the same intent
    // while ordinary 3D rigid bodies remain unconstrained.
    state.constrain_to_2d=view.character_motor_2d.has_value();
    if(view.convex_hull_collider) {
        state.shape_type=PhysicsShapeType::convex_hull; state.convex_points=view.convex_hull_collider->points;
        state.restitution=view.convex_hull_collider->restitution; state.friction=view.convex_hull_collider->friction;state.is_trigger=view.convex_hull_collider->is_trigger;
    } else if (view.capsule_collider) {
        state.shape_type=PhysicsShapeType::capsule; state.radius=view.capsule_collider->radius; state.half_height=view.capsule_collider->half_height;
        state.half_x=state.half_z=state.radius; state.half_y=state.radius+state.half_height;
        state.restitution=view.capsule_collider->restitution; state.friction=view.capsule_collider->friction;state.is_trigger=view.capsule_collider->is_trigger;
    } else if (view.sphere_collider) {
        state.shape_type=PhysicsShapeType::sphere; state.radius=view.sphere_collider->radius;
        state.half_x=state.half_y=state.half_z=state.radius;
        state.restitution=view.sphere_collider->restitution; state.friction=view.sphere_collider->friction;state.is_trigger=view.sphere_collider->is_trigger;
    } else {
        state.half_x=view.box_collider->half_x; state.half_y=view.box_collider->half_y; state.half_z=view.box_collider->half_z;
        state.restitution=view.box_collider->restitution; state.friction=view.box_collider->friction;state.is_trigger=view.box_collider->is_trigger;
    }
    return state;
}

void append_tilemap_physics_bodies(const std::vector<WorldEntityView>& views,std::vector<PhysicsBodyState>& bodies) {
    for(const auto& view:views) {
        if(!view.transform||!view.tilemap_renderer||!view.tilemap_renderer->collision_enabled||!view.tilemap_asset)continue;
        const auto bake=TilemapAssetCodec::bake_colliders(view.tilemap_asset->palette,view.tilemap_asset->tilemap);
        if(!bake.success)continue;
        const auto& transform=*view.transform;
        const auto rotate=[&](const float x,const float y,const float z) {
            const auto qx=transform.rotation_x,qy=transform.rotation_y,qz=transform.rotation_z,qw=transform.rotation_w;
            const auto tx=2.0F*(qy*z-qz*y),ty=2.0F*(qz*x-qx*z),tz=2.0F*(qx*y-qy*x);
            return std::array<float,3>{x+qw*tx+(qy*tz-qz*ty),y+qw*ty+(qz*tx-qx*tz),z+qw*tz+(qx*ty-qy*tx)};
        };
        for(std::size_t index=0;index<bake.colliders.size();++index) {
            const auto& collider=bake.colliders[index];
            const auto local=rotate(collider.center_x*transform.scale_x,collider.center_y*transform.scale_y,0.0F);
            PhysicsBodyState state;
            state.entity_id=view.id+"/tile-collider/"+collider.layer_id+"/"+std::to_string(index);
            state.motion_type=PhysicsMotionType::static_body;state.shape_type=PhysicsShapeType::box;
            state.position_x=transform.x+local[0];state.position_y=transform.y+local[1];state.position_z=transform.z+local[2];
            state.rotation_x=transform.rotation_x;state.rotation_y=transform.rotation_y;
            state.rotation_z=transform.rotation_z;state.rotation_w=transform.rotation_w;
            state.half_x=collider.width*std::abs(transform.scale_x)*0.5F;
            state.half_y=collider.height*std::abs(transform.scale_y)*0.5F;
            state.half_z=std::max(0.05F,0.5F*std::abs(transform.scale_z));
            state.gravity_factor=0.0F;state.friction=0.8F;state.one_way=collider.collision=="one-way";
            bodies.push_back(std::move(state));
        }
    }
}

} // namespace

World::World() {
    world_.component<Transform>()
        .member<float>("x")
        .member<float>("y")
        .member<float>("z")
        .member<float>("scale_x")
        .member<float>("scale_y")
        .member<float>("scale_z")
        .member<float>("rotation_x")
        .member<float>("rotation_y")
        .member<float>("rotation_z")
        .member<float>("rotation_w");
    world_.component<Velocity>()
        .member<float>("x")
        .member<float>("y")
        .member<float>("z");
    world_.component<RigidBody>();
    world_.component<BoxCollider>();
    world_.component<SphereCollider>();
    world_.component<CapsuleCollider>();
    world_.component<CharacterMotor2D>();
    world_.component<Platform2D>();
    world_.component<ConvexHullCollider>();
    world_.component<AnimationPlayer>();
    world_.component<AnimationCueState>();
    world_.component<Camera>();
    world_.component<CameraFollow2D>();
    world_.component<DirectionalLight>();
    world_.component<LocalLight>();
    world_.component<MeshRenderer>();
    world_.component<SpriteRenderer>();
    world_.component<TilemapRenderer>();
    world_.component<PbrMaterial>();
    world_.component<SemanticIdentity>();

    const auto vfx_load = vfx_runtime_.load_graph_json(VfxRuntime::default_graph_json());
    if (vfx_load.success) {
        static_cast<void>(vfx_runtime_.bind_event("entity.spawned", vfx_load.graph_id));
        static_cast<void>(vfx_runtime_.bind_event("combat.hit", vfx_load.graph_id));
    }
    static_cast<void>(vfx_runtime_.load_graph_json(VfxRuntime::default_alpha_graph_json()));

}

void World::clear_scene_entities() {
    std::vector<flecs::entity_t> entities;
    world_.each<const SemanticIdentity>([&entities](const flecs::entity entity, const SemanticIdentity&) {
        entities.push_back(entity.id());
    });
    for (const auto entity : entities) {
        world_.entity(entity).destruct();
    }
    entity_ids_.clear();
    if (!gameplay_runtime_.events().empty())
        last_animation_cue_event_sequence_ = gameplay_runtime_.events().back().sequence;
}

void World::synchronize_managed_scripts() {
    std::vector<ManagedScriptInstance> scene_scripts;
    for(const auto& source:scene_document_.entities) if(source.managed_script&&source.managed_script->enabled) {
        ManagedScriptInstance instance;
        instance.id=source.managed_script->instance_id;instance.entity_id=source.guid;
        instance.assembly_asset=source.managed_script->assembly_asset;instance.type_name=source.managed_script->type_name;
        instance.properties_json=source.managed_script->properties_json;instance.scene_owned=true;
        scene_scripts.push_back(std::move(instance));
    }
    scripting_runtime_.synchronize_instances(std::move(scene_scripts));
}

SceneLoadResult World::load_scene(const SceneDocument& document) {
    return load_scene_internal(document, true);
}

SceneLoadResult World::load_scene_internal(const SceneDocument& document, const bool reset_history) {
    auto errors = SceneDocumentCodec::validate(document);
    if (!errors.empty()) {
        return SceneLoadResult{.errors = std::move(errors)};
    }

    clear_scene_entities();
    active_script_contacts_.clear();
    scene_guid_ = document.scene_guid;
    scene_name_ = document.name;
    scene_source_uri_ = document.source_uri;
    scene_document_ = document;
    if (reset_history) {
        recent_deltas_.clear();
        undo_stack_.clear();
        redo_stack_.clear();
        scene_source_baseline_.clear();
        scene_source_existed_at_load_ = false;
        const auto source = std::filesystem::path(scene_source_uri_);
        if (!scene_source_uri_.empty() && scene_source_uri_.find("://") == std::string::npos && source.is_absolute()) {
            std::ifstream stream(source, std::ios::binary);
            if (stream) {
                scene_source_baseline_.assign(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
                scene_source_existed_at_load_ = true;
            }
        }
    }
    entity_ids_.reserve(document.entities.size());
    for (std::size_t index = 0; index < document.entities.size(); ++index) {
        const auto& source = document.entities[index];
        auto entity = world_.entity().set<SemanticIdentity>({
            source.guid,
            "/world/entities/" + source.guid,
            "engine.entity",
            "schema://noemancer/entity/0.1",
            source.name,
            document.scene_guid,
            source.parent_guid,
            SourceAnchor{
                document.source_uri,
                "/entities/" + std::to_string(index)
            }
        });
        if (source.transform) {
            const auto rotation=quaternion_from_euler_degrees(source.transform->rotation_euler_degrees);
            entity.set<Transform>({
                static_cast<float>(source.transform->position.x),
                static_cast<float>(source.transform->position.y),
                static_cast<float>(source.transform->position.z),
                static_cast<float>(source.transform->scale.x),
                static_cast<float>(source.transform->scale.y),
                static_cast<float>(source.transform->scale.z),rotation.x,rotation.y,rotation.z,rotation.w
            });
        }
        if (source.velocity) {
            entity.set<Velocity>({
                static_cast<float>(source.velocity->linear.x),
                static_cast<float>(source.velocity->linear.y),
                static_cast<float>(source.velocity->linear.z)
            });
        }
        if (source.rigid_body) {
            entity.set<RigidBody>({motion_type(source.rigid_body->motion_type), static_cast<float>(source.rigid_body->mass),
                static_cast<float>(source.rigid_body->gravity_factor), static_cast<float>(source.rigid_body->linear_damping)});
        }
        if (source.box_collider) {
            entity.set<BoxCollider>({static_cast<float>(source.box_collider->half_extents.x), static_cast<float>(source.box_collider->half_extents.y),
                static_cast<float>(source.box_collider->half_extents.z), static_cast<float>(source.box_collider->friction),
                static_cast<float>(source.box_collider->restitution),source.box_collider->is_trigger});
        }
        if (source.sphere_collider) {
            entity.set<SphereCollider>({static_cast<float>(source.sphere_collider->radius),
                static_cast<float>(source.sphere_collider->friction), static_cast<float>(source.sphere_collider->restitution),source.sphere_collider->is_trigger});
        }
        if (source.capsule_collider) {
            entity.set<CapsuleCollider>({static_cast<float>(source.capsule_collider->radius), static_cast<float>(source.capsule_collider->half_height),
                static_cast<float>(source.capsule_collider->friction), static_cast<float>(source.capsule_collider->restitution),source.capsule_collider->is_trigger});
        }
        if (source.character_motor_2d) {
            const auto& motor = *source.character_motor_2d;
            entity.set<CharacterMotor2D>({CharacterMotor2DConfig{
                static_cast<float>(motor.maximum_speed), static_cast<float>(motor.ground_acceleration),
                static_cast<float>(motor.air_acceleration), static_cast<float>(motor.ground_deceleration),
                static_cast<float>(motor.jump_speed), static_cast<float>(motor.maximum_fall_speed),
                static_cast<float>(motor.coyote_time_seconds), static_cast<float>(motor.jump_buffer_seconds),
                static_cast<float>(motor.ground_probe_distance), static_cast<float>(motor.minimum_ground_normal_y),
                static_cast<float>(motor.jump_release_velocity_factor)}, {}});
        }
        if(source.platform_2d) {
            const auto& platform=*source.platform_2d; const auto origin=source.transform?source.transform->position:SceneVector3{};
            entity.set<Platform2D>({platform.collision_mode,static_cast<float>(platform.motion_axis.x),static_cast<float>(platform.motion_axis.y),
                static_cast<float>(platform.motion_axis.z),static_cast<float>(platform.motion_distance),static_cast<float>(platform.motion_period_seconds),
                static_cast<float>(platform.motion_phase),static_cast<float>(origin.x),static_cast<float>(origin.y),static_cast<float>(origin.z),0.0F});
        }
        if(source.convex_hull_collider) {
            ConvexHullCollider collider; collider.friction=static_cast<float>(source.convex_hull_collider->friction);
            collider.restitution=static_cast<float>(source.convex_hull_collider->restitution);
            collider.is_trigger=source.convex_hull_collider->is_trigger;
            for(const auto& point:source.convex_hull_collider->points) collider.points.push_back({static_cast<float>(point.x),static_cast<float>(point.y),static_cast<float>(point.z)});
            entity.set<ConvexHullCollider>(std::move(collider));
        }
        if (source.animation_player) {
            const auto base_y = source.transform ? static_cast<float>(source.transform->position.y) : 0.0F;
            AnimationPlayer player{.clip_asset=source.animation_player->clip_asset,
                .playback_speed=static_cast<float>(source.animation_player->playback_speed),.time_seconds=0.0F,.base_y=base_y,
                .looping=source.animation_player->looping,.playing=source.animation_player->playing,
                .next_clip_asset=source.animation_player->next_clip_asset,.next_time_seconds=0.0F,.next_looping=source.animation_player->looping,
                .transition_duration_seconds=static_cast<float>(source.animation_player->transition_duration_seconds),
                .transition_elapsed_seconds=0.0F,.root_motion_mode=source.animation_player->root_motion_mode,
                .state_machine_asset=source.animation_player->state_machine_asset.empty()?
                    "animation.machine.basic-locomotion":source.animation_player->state_machine_asset};
            player.animation_graph_asset=source.animation_player->animation_graph_asset;
            player.active_state.clear();configure_animation_player(player);configure_animation_graph_player(player);
            entity.set<AnimationPlayer>(std::move(player));
        }
        if (source.camera) {
            entity.set<Camera>({
                static_cast<float>(source.camera->target.x),
                static_cast<float>(source.camera->target.y),
                static_cast<float>(source.camera->target.z),
                static_cast<float>(source.camera->vertical_fov_degrees),
                static_cast<float>(source.camera->near_clip),
                static_cast<float>(source.camera->far_clip),
                source.camera->primary,
                source.camera->projection,
                static_cast<float>(source.camera->orthographic_height)
            });
        }
        if(source.camera_follow_2d) {
            const auto& follow=*source.camera_follow_2d;
            entity.set<CameraFollow2D>({follow.target_entity_id,static_cast<float>(follow.position_offset.x),static_cast<float>(follow.position_offset.y),
                static_cast<float>(follow.position_offset.z),static_cast<float>(follow.dead_zone.x),static_cast<float>(follow.dead_zone.y),
                static_cast<float>(follow.look_ahead_distance),static_cast<float>(follow.smoothing),static_cast<float>(follow.minimum_center.x),
                static_cast<float>(follow.minimum_center.y),static_cast<float>(follow.maximum_center.x),static_cast<float>(follow.maximum_center.y),
                static_cast<float>(source.camera?source.camera->target.x:0.0),static_cast<float>(source.camera?source.camera->target.y:0.0),"hold"});
        }
        if (source.directional_light) {
            entity.set<DirectionalLight>({
                static_cast<float>(source.directional_light->direction.x),
                static_cast<float>(source.directional_light->direction.y),
                static_cast<float>(source.directional_light->direction.z),
                static_cast<float>(source.directional_light->color.x),
                static_cast<float>(source.directional_light->color.y),
                static_cast<float>(source.directional_light->color.z),
                static_cast<float>(source.directional_light->intensity),
                static_cast<float>(source.directional_light->ambient_intensity),
                source.directional_light->casts_shadows
            });
        }
        if(source.local_light) {
            const auto& light=*source.local_light;
            entity.set<LocalLight>({light.kind,static_cast<float>(light.color.x),static_cast<float>(light.color.y),static_cast<float>(light.color.z),
                static_cast<float>(light.luminous_power_lumens),static_cast<float>(light.range_meters),
                static_cast<float>(light.direction.x),static_cast<float>(light.direction.y),static_cast<float>(light.direction.z),
                static_cast<float>(light.inner_cone_degrees),static_cast<float>(light.outer_cone_degrees),
                static_cast<float>(light.source_radius_meters),light.casts_shadows});
        }
        if (source.mesh_renderer) {
            entity.set<MeshRenderer>({
                source.mesh_renderer->mesh_asset,
                source.mesh_renderer->visible,
                source.mesh_renderer->casts_shadows,
                source.mesh_renderer->receives_shadows
            });
        }
        if(source.sprite_renderer) {
            const auto& renderer=*source.sprite_renderer;
            entity.set<SpriteRenderer>({SpritePlaybackState{.asset_id=renderer.sprite_asset,.clip_id=renderer.clip,
                .playing=renderer.playing},static_cast<float>(renderer.playback_speed),renderer.flip_x,renderer.flip_y,
                renderer.sorting_layer,renderer.sorting_order,renderer.visible});
        }
        if(source.tilemap_renderer)entity.set<TilemapRenderer>({source.tilemap_renderer->tilemap_asset,
            source.tilemap_renderer->visible,source.tilemap_renderer->collision_enabled});
        if (source.pbr_material) {
            entity.set<PbrMaterial>({
                static_cast<float>(source.pbr_material->base_color.x),
                static_cast<float>(source.pbr_material->base_color.y),
                static_cast<float>(source.pbr_material->base_color.z),
                static_cast<float>(source.pbr_material->metallic),
                static_cast<float>(source.pbr_material->roughness),
                source.pbr_material->base_color_texture,
                static_cast<float>(source.pbr_material->emissive_color.x),
                static_cast<float>(source.pbr_material->emissive_color.y),
                static_cast<float>(source.pbr_material->emissive_color.z),
                static_cast<float>(source.pbr_material->emissive_intensity)
            });
        }
        entity_ids_.emplace(source.guid, entity.id());
    }
    for (const auto& source : document.entities) {
        if (!source.parent_guid.empty()) {
            world_.entity(entity_ids_.at(source.guid)).child_of(world_.entity(entity_ids_.at(source.parent_guid)));
        }
    }
    synchronize_managed_scripts();
    std::vector<PhysicsBodyState> initial_bodies;
    const auto initial_views=entity_views_impl(std::nullopt,false);
    for (const auto& view : initial_views) {
        if (auto body = physics_state(view)) initial_bodies.push_back(std::move(*body));
    }
    append_tilemap_physics_bodies(initial_views,initial_bodies);
    physics_runtime_.step(initial_bodies, 0.0F);
    ++revision_;
    return SceneLoadResult{
        .success = true,
        .entity_count = document.entities.size(),
        .revision = revision_
    };
}

void World::tick(const float delta_seconds) {
    ++simulation_tick_;
    struct ScriptContactDispatch final { std::string callback; PhysicsContact contact; };
    std::vector<ScriptContactDispatch> script_contacts;
    input_runtime_.evaluate();
    gameplay_runtime_.update_from_input(input_runtime_);
    gameplay_ability_runtime_.tick(delta_seconds, gameplay_runtime_);
    consume_animation_cues(delta_seconds);
    vfx_runtime_.consume_gameplay_events(gameplay_runtime_.events());
    vfx_runtime_.tick(delta_seconds);
    auto views = entity_views_impl(std::nullopt,false);
    std::unordered_map<std::string, Transform> transforms_before;
    for (const auto& view : views) {
        if (view.transform&&(view.platform_2d||view.character_motor_2d||view.animation_player||
            view.rigid_body||view.velocity||view.camera_follow_2d))
            transforms_before.emplace(view.id, *view.transform);
    }
    const auto revision_before = revision_;
    bool platform_transforms_changed=false;
    for(const auto& view:views) {
        if(!view.platform_2d||!view.transform||view.platform_2d->motion_distance<=0.0F) continue;
        auto platform=*view.platform_2d; const auto dt=std::max(delta_seconds,0.0F); platform.elapsed_seconds+=dt;
        const auto axis_length=std::sqrt(platform.axis_x*platform.axis_x+platform.axis_y*platform.axis_y+platform.axis_z*platform.axis_z);
        const auto phase=6.283185307179586F*(platform.elapsed_seconds/platform.motion_period_seconds+platform.motion_phase);
        const auto offset=platform.motion_distance*std::sin(phase); const auto inverse_axis=axis_length>0.00001F?1.0F/axis_length:0.0F;
        auto transform=*view.transform;
        transform.x=platform.origin_x+platform.axis_x*inverse_axis*offset;
        transform.y=platform.origin_y+platform.axis_y*inverse_axis*offset;
        transform.z=platform.origin_z+platform.axis_z*inverse_axis*offset;
        const Velocity velocity{dt>0.0F?(transform.x-view.transform->x)/dt:0.0F,dt>0.0F?(transform.y-view.transform->y)/dt:0.0F,
            dt>0.0F?(transform.z-view.transform->z)/dt:0.0F};
        auto entity=world_.entity(entity_ids_.at(view.id)); entity.set<Transform>(transform); entity.set<Velocity>(velocity); entity.set<Platform2D>(platform);
        platform_transforms_changed=true;
    }
    if(platform_transforms_changed)views=entity_views_impl(std::nullopt,false);
    const auto action_value = [&](const std::string_view id) {
        const auto found = std::ranges::find(input_runtime_.actions(), id, &InputActionState::id);
        return found == input_runtime_.actions().end() ? 0.0F : found->value;
    };
    bool character_state_changed=false;
    for (const auto& view : views) {
        if (!view.character_motor_2d || !view.transform || !view.velocity || !view.capsule_collider) continue;
        const auto& config = view.character_motor_2d->config;
        const auto& collider = *view.capsule_collider;
        const auto hit = physics_runtime_.sphere_sweep(view.transform->x, view.transform->y - collider.half_height,
            view.transform->z, 0.0F, -config.ground_probe_distance, 0.0F, collider.radius * 0.92F, view.id);
        auto ground_valid=hit.hit; Velocity ground_velocity{};
        if(hit.hit) {
            const auto ground=std::ranges::find(views,hit.entity_id,&WorldEntityView::id);
            if(ground!=views.end()) {
                ground_velocity=ground->velocity.value_or(Velocity{});
                if(ground->platform_2d&&ground->platform_2d->collision_mode=="one-way"&&ground->transform&&ground->box_collider) {
                    const auto actor_bottom=view.transform->y-collider.half_height-collider.radius;
                    const auto platform_top=ground->transform->y+ground->box_collider->half_y;
                    ground_valid=actor_bottom>=platform_top-0.08F&&view.velocity->y<=ground_velocity.y+0.05F;
                }
            }
        }
        const auto move_input=action_value("gameplay.move.x");
        PhysicsSweepHit wall_hit;
        if(std::abs(move_input)>0.001F) wall_hit=physics_runtime_.sphere_sweep(view.transform->x,
            view.transform->y,view.transform->z,(move_input>0.0F?1.0F:-1.0F)*(collider.radius+0.10F),0.0F,0.0F,
            collider.radius*0.88F,view.id);
        const CharacterMotor2DInput input{.move=move_input,.jump_held=action_value("gameplay.jump")>0.5F,
            .ground_hit=ground_valid,.ground_normal_x=hit.normal_x,.ground_normal_y=hit.normal_y,.ground_entity_id=hit.entity_id,
            .wall_hit=wall_hit.hit,.wall_normal_x=wall_hit.normal_x,.wall_entity_id=wall_hit.entity_id,
            .ground_velocity_x=ground_velocity.x,.ground_velocity_y=ground_velocity.y};
        const auto result = update_character_motor_2d(config, view.character_motor_2d->state, input,
            view.velocity->x, view.velocity->y, delta_seconds);
        auto entity = world_.entity(entity_ids_.at(view.id));
        entity.set<Velocity>({result.velocity_x, result.velocity_y, view.velocity->z});
        entity.set<CharacterMotor2D>({config, result.state});
        character_state_changed=true;
        if (const auto* current_player = entity.try_get<AnimationPlayer>()) {
            auto player = *current_player;
            player.state_parameters["speed"] = std::abs(result.velocity_x);
            player.state_parameters["grounded"] = result.state.grounded ? 1.0F : 0.0F;
            if(player.graph_parameters.contains("speed"))player.graph_parameters["speed"]=std::abs(result.velocity_x);
            if(player.graph_parameters.contains("grounded"))player.graph_parameters["grounded"]=result.state.grounded?1.0F:0.0F;
            entity.set<AnimationPlayer>(std::move(player));
        }
    }
    if(character_state_changed)views=entity_views_impl(std::nullopt,false);
    for(const auto& view:views) {
        if(!view.sprite_renderer)continue;
        auto renderer=*view.sprite_renderer;
        const auto result=sprite_assets_.advance(renderer.playback,
            static_cast<double>(std::max(delta_seconds,0.0F))*renderer.playback_speed);
        if(result.success&&result.frame_changed&&!result.event.empty())
            gameplay_runtime_.emit("sprite.frame-event",view.id,view.id,Json{{"spriteAsset",renderer.playback.asset_id},
                {"clip",renderer.playback.clip_id},{"frame",result.frame_id},{"event",result.event},
                {"completedLoops",renderer.playback.completed_loops}}.dump());
        world_.entity(entity_ids_.at(view.id)).set<SpriteRenderer>(std::move(renderer));
    }
    bool animation_state_changed=false;
    for (const auto& view : views) {
        if (!view.animation_player) continue;
        auto player = *view.animation_player;
        for(const auto& [id,value]:player.graph_parameters)
            if(player.state_parameters.contains(id))player.state_parameters[id]=value;
        AnimationStateMachineEvaluation state_evaluation;
        if(player.playing)state_evaluation=animation_state_machines_.evaluate(player.state_machine_asset,player.active_state,
            player.state_parameters,view.animation_cue?view.animation_cue->cue:std::string_view{},player.state_elapsed_seconds);
        if(state_evaluation.valid&&state_evaluation.transitioned) {
            player.previous_state=player.active_state;player.active_state=state_evaluation.to;
            player.state_elapsed_seconds=0.0F;++player.state_transition_count;
            if(!state_evaluation.clip_asset.empty()&&state_evaluation.clip_asset!=player.clip_asset) {
                if(state_evaluation.duration_seconds<=0.0F) {
                    player.clip_asset=state_evaluation.clip_asset;player.time_seconds=0.0F;player.looping=state_evaluation.looping;
                    player.next_clip_asset.clear();player.next_time_seconds=0.0F;
                    player.transition_duration_seconds=0.0F;player.transition_elapsed_seconds=0.0F;
                } else {
                    player.next_clip_asset=state_evaluation.clip_asset;player.next_time_seconds=0.0F;player.next_looping=state_evaluation.looping;
                    player.transition_duration_seconds=state_evaluation.duration_seconds;player.transition_elapsed_seconds=0.0F;
                }
            }
        } else if(player.playing)player.state_elapsed_seconds+=std::max(delta_seconds,0.0F);
        auto entity = world_.entity(entity_ids_.at(view.id));
        if (!view.transform || view.rigid_body && view.rigid_body->motion_type == PhysicsMotionType::dynamic_body) {
            entity.set<AnimationPlayer>(std::move(player));
            continue;
        }
        const auto* animation_graph=animation_graphs_.find(player.animation_graph_asset);
        std::optional<AnimationPoseExecutionRequest> previous_graph_request;
        if(animation_graph!=nullptr) {
            configure_animation_graph_player(player);
            previous_graph_request=animation_graph_pose_request(player);
        }
        const auto previous_time = player.time_seconds;
        player.time_seconds = animation_runtime_.advance_time(player.time_seconds, delta_seconds, player.playback_speed,
            player.looping, player.playing, player.clip_asset);
        auto transform = *view.transform;
        RootMotionDelta motion;
        if(animation_graph==nullptr&&player.root_motion_mode=="apply")motion=animation_runtime_.root_motion_delta(
            player.clip_asset,previous_time,player.time_seconds,player.looping,player.playback_speed);
        if (!player.next_clip_asset.empty() && player.transition_duration_seconds > 0.0F) {
            const auto previous_next_time = player.next_time_seconds;
            player.next_time_seconds = animation_runtime_.advance_time(player.next_time_seconds, delta_seconds, player.playback_speed,
                player.next_looping, player.playing, player.next_clip_asset);
            if(player.playing)player.transition_elapsed_seconds = std::min(player.transition_duration_seconds,
                player.transition_elapsed_seconds + std::max(delta_seconds, 0.0F));
            RootMotionDelta target_motion;
            if(animation_graph==nullptr&&player.root_motion_mode=="apply")target_motion=animation_runtime_.root_motion_delta(
                player.next_clip_asset,previous_next_time,player.next_time_seconds,player.next_looping,player.playback_speed);
            if (motion.valid && target_motion.valid) {
                const auto weight = player.transition_elapsed_seconds / player.transition_duration_seconds;
                motion.x = motion.x * (1.0F - weight) + target_motion.x * weight;
                motion.y = motion.y * (1.0F - weight) + target_motion.y * weight;
                motion.z = motion.z * (1.0F - weight) + target_motion.z * weight;
            }
            if (player.transition_elapsed_seconds >= player.transition_duration_seconds) {
                player.clip_asset = player.next_clip_asset;
                player.time_seconds = player.next_time_seconds;player.looping=player.next_looping;
                player.next_clip_asset.clear();
                player.next_time_seconds = 0.0F;
                player.transition_duration_seconds = 0.0F;
                player.transition_elapsed_seconds = 0.0F;
            }
        }
        if (player.root_motion_mode == "apply") {
            if (motion.valid) { transform.x += motion.x; transform.y += motion.y; transform.z += motion.z; }
        } else if(animation_graph==nullptr) {
            transform.y = player.base_y + animation_runtime_.sample_translation_y(player.clip_asset, player.time_seconds);
        }
        if(animation_graph!=nullptr) {
            for(const auto& node:animation_graph->nodes)if(node.kind=="clip")
                player.graph_node_times[node.id]=animation_runtime_.advance_time(player.graph_node_times[node.id],delta_seconds,
                    player.playback_speed,node.looping,player.playing,node.clip_asset);
            if(player.playing)if(const auto request=animation_graph_pose_request(player)) {
                std::unordered_set<std::string> advanced_groups;
                for(std::size_t layer_index=0;layer_index<animation_graph->layers.size();++layer_index) {
                    const auto& group=animation_graph->layers[layer_index].sync_group;if(group.empty()||!advanced_groups.insert(group).second)continue;
                    const auto& clip=layer_index==0?request->base_clip_asset:request->layers[layer_index-1U].clip_asset;
                    const auto duration=animation_runtime_.duration(clip);if(duration<=0.0F)continue;
                    auto& phase=player.graph_sync_phases[group];phase+=std::max(delta_seconds,0.0F)*player.playback_speed/duration;
                    phase=phase-std::floor(phase);if(phase<0.0F)phase+=1.0F;
                }
            }
            const auto current_graph_request=animation_graph_pose_request(player);
            if(player.playing&&player.root_motion_mode=="apply"&&previous_graph_request&&current_graph_request&&
               previous_graph_request->base_clip_asset==current_graph_request->base_clip_asset&&
               previous_graph_request->base_looping==current_graph_request->base_looping&&
               previous_graph_request->base_secondary_clip_asset==current_graph_request->base_secondary_clip_asset&&
               previous_graph_request->base_secondary_looping==current_graph_request->base_secondary_looping&&
               std::abs(previous_graph_request->base_secondary_weight-current_graph_request->base_secondary_weight)<=0.0001F) {
                auto graph_motion=animation_runtime_.root_motion_delta(current_graph_request->base_clip_asset,
                    previous_graph_request->base_time,current_graph_request->base_time,current_graph_request->base_looping,player.playback_speed);
                if(graph_motion.valid&&!current_graph_request->base_secondary_clip_asset.empty()&&
                   previous_graph_request->base_secondary_clip_asset==current_graph_request->base_secondary_clip_asset) {
                    const auto secondary=animation_runtime_.root_motion_delta(current_graph_request->base_secondary_clip_asset,
                        previous_graph_request->base_secondary_time,current_graph_request->base_secondary_time,
                        current_graph_request->base_secondary_looping,player.playback_speed);
                    if(secondary.valid) {
                        const auto weight=std::clamp(current_graph_request->base_secondary_weight,0.0F,1.0F);
                        graph_motion.x=graph_motion.x*(1.0F-weight)+secondary.x*weight;
                        graph_motion.y=graph_motion.y*(1.0F-weight)+secondary.y*weight;
                        graph_motion.z=graph_motion.z*(1.0F-weight)+secondary.z*weight;
                    }
                }
                if(graph_motion.valid){transform.x+=graph_motion.x;transform.y+=graph_motion.y;transform.z+=graph_motion.z;}
            }
        }
        entity.set<AnimationPlayer>(player);
        entity.set<Transform>(transform);
        animation_state_changed=true;
    }

    if(animation_state_changed)views=entity_views_impl(std::nullopt,false);
    std::vector<PhysicsBodyState> bodies;
    for (const auto& view : views) {
        if (auto body = physics_state(view)) bodies.push_back(std::move(*body));
    }
    append_tilemap_physics_bodies(views,bodies);
    physics_runtime_.step(bodies, delta_seconds);
    std::unordered_map<std::string,PhysicsContact> current_contacts;
    for(const auto& contact:physics_runtime_.contacts()) {
        const auto key=contact_key(contact.body_a,contact.body_b);
        current_contacts.insert_or_assign(key,contact);
        const auto entered=!active_script_contacts_.contains(key);
        script_contacts.push_back({contact.is_trigger?(entered?"OnTriggerEnter":"OnTriggerStay"):(entered?"OnContactEnter":"OnContactStay"),contact});
        if(entered)gameplay_runtime_.emit(contact.is_trigger?"physics.trigger.enter":"physics.contact.enter",contact.body_a,contact.body_b,
            Json{{"bodyA",contact.body_a},{"bodyB",contact.body_b},{"normal",{{"x",contact.normal_x},{"y",contact.normal_y},{"z",contact.normal_z}}},
                {"penetration",contact.penetration}}.dump());
    }
    for(const auto& [key,contact]:active_script_contacts_) if(!current_contacts.contains(key)) {
        script_contacts.push_back({contact.is_trigger?"OnTriggerExit":"OnContactExit",contact});
        gameplay_runtime_.emit(contact.is_trigger?"physics.trigger.exit":"physics.contact.exit",contact.body_a,contact.body_b,
            Json{{"bodyA",contact.body_a},{"bodyB",contact.body_b}}.dump());
    }
    active_script_contacts_=std::move(current_contacts);
    bool physics_transforms_changed=false;
    for (const auto& body : bodies) {
        const auto entity_id=entity_ids_.find(body.entity_id);if(entity_id==entity_ids_.end())continue;
        auto entity = world_.entity(entity_id->second);
        const auto* current = entity.try_get<Transform>();
        entity.set<Transform>({body.position_x, body.position_y, body.position_z,
            current ? current->scale_x : 1.0F, current ? current->scale_y : 1.0F, current ? current->scale_z : 1.0F,
            body.rotation_x,body.rotation_y,body.rotation_z,body.rotation_w});
        entity.set<Velocity>({body.velocity_x, body.velocity_y, body.velocity_z});
        physics_transforms_changed=true;
    }
    bool velocity_transforms_changed=false;
    for (const auto& view : views) {
        if (!view.transform || !view.velocity || view.rigid_body) continue;
        auto transform = *view.transform;
        transform.x += view.velocity->x * delta_seconds;
        transform.y += view.velocity->y * delta_seconds;
        transform.z += view.velocity->z * delta_seconds;
        world_.entity(entity_ids_.at(view.id)).set<Transform>(transform);
        velocity_transforms_changed=true;
    }
    const auto has_camera_follow=std::ranges::any_of(views,[](const WorldEntityView& view){return view.camera_follow_2d.has_value();});
    if(has_camera_follow&&(physics_transforms_changed||velocity_transforms_changed||animation_state_changed||platform_transforms_changed))
        views=entity_views_impl(std::nullopt,false);
    for(const auto& view:views) {
        if(!view.camera_follow_2d||!view.camera||!view.transform) continue;
        const auto target=std::ranges::find(views,view.camera_follow_2d->target_entity_id,&WorldEntityView::id);
        if(target==views.end()||!target->transform) continue;
        auto follow=*view.camera_follow_2d; const auto target_velocity=target->velocity.value_or(Velocity{});
        const auto look_direction=target_velocity.x>0.05F?1.0F:target_velocity.x<-0.05F?-1.0F:0.0F;
        const auto anchor_x=target->transform->x+look_direction*follow.look_ahead_distance;
        const auto anchor_y=target->transform->y;
        auto desired_x=follow.center_x,desired_y=follow.center_y;
        if(anchor_x<follow.center_x-follow.dead_zone_x)desired_x=anchor_x+follow.dead_zone_x;
        else if(anchor_x>follow.center_x+follow.dead_zone_x)desired_x=anchor_x-follow.dead_zone_x;
        if(anchor_y<follow.center_y-follow.dead_zone_y)desired_y=anchor_y+follow.dead_zone_y;
        else if(anchor_y>follow.center_y+follow.dead_zone_y)desired_y=anchor_y-follow.dead_zone_y;
        desired_x=std::clamp(desired_x,follow.minimum_x,follow.maximum_x); desired_y=std::clamp(desired_y,follow.minimum_y,follow.maximum_y);
        const auto alpha=follow.smoothing<=0.0F?1.0F:1.0F-std::exp(-follow.smoothing*std::max(delta_seconds,0.0F));
        const auto previous_x=follow.center_x,previous_y=follow.center_y;
        follow.center_x=std::lerp(follow.center_x,desired_x,alpha); follow.center_y=std::lerp(follow.center_y,desired_y,alpha);
        follow.decision=(std::abs(follow.center_x-previous_x)+std::abs(follow.center_y-previous_y)>0.00001F)?"follow":"dead-zone-hold";
        auto transform=*view.transform; transform.x=follow.center_x+follow.offset_x;transform.y=follow.center_y+follow.offset_y;transform.z=follow.offset_z;
        auto camera=*view.camera;camera.target_x=follow.center_x;camera.target_y=follow.center_y;camera.target_z=0.0F;
        auto entity=world_.entity(entity_ids_.at(view.id));entity.set<Transform>(transform);entity.set<Camera>(camera);entity.set<CameraFollow2D>(follow);
    }
    world_.progress(delta_seconds);
    ++revision_;
    for (const auto& [entity_id,before] : transforms_before) {
        const auto found_id=entity_ids_.find(entity_id);
        if(found_id==entity_ids_.end())continue;
        const auto* current=world_.entity(found_id->second).try_get<Transform>();
        if (!current || (before.x == current->x && before.y == current->y && before.z == current->z)) {
            continue;
        }
        recent_deltas_.push_back({
            .revision_before = revision_before,
            .revision_after = revision_,
            .entity_id = entity_id,
            .field = "engine.entity.transform.position",
            .before = semantic_vector(before),
            .after = semantic_vector(*current),
            .manager = "runtime.simulation",
            .undoable = false
        });
    }
    if (recent_deltas_.size() > 256) {
        recent_deltas_.erase(recent_deltas_.begin(), recent_deltas_.begin() + (recent_deltas_.size() - 256));
    }
    if(scripting_runtime_.project_ready()) for(const auto& instance:scripting_runtime_.automatic_instances()) {
        if(!instance.scene_owned)continue;
        auto properties=Json::parse(instance.properties_json,nullptr,false);if(properties.is_discarded())properties=Json::object();
        const auto arguments=Json{{"deltaSeconds",delta_seconds},{"properties",properties}}.dump();
        if(instance.state=="attached") {
            const auto created=Json::parse(scripting_invoke_json(instance.id,"OnCreate",arguments),nullptr,false);
            if(created.is_discarded()||!created.value("success",false))continue;
        }
        for(const auto& dispatch:script_contacts) {
            const auto is_first=dispatch.contact.body_a==instance.entity_id;
            if(!is_first&&dispatch.contact.body_b!=instance.entity_id)continue;
            if(!scripting_runtime_.type_implements_callback(instance.type_name,dispatch.callback))continue;
            auto contact_arguments=Json{{"deltaSeconds",delta_seconds},{"properties",properties},
                {"contact",{{"selfId",instance.entity_id},{"otherId",is_first?dispatch.contact.body_b:dispatch.contact.body_a},
                    {"normal",{{"x",is_first?dispatch.contact.normal_x:-dispatch.contact.normal_x},
                        {"y",is_first?dispatch.contact.normal_y:-dispatch.contact.normal_y},
                        {"z",is_first?dispatch.contact.normal_z:-dispatch.contact.normal_z}}},
                    {"penetration",dispatch.contact.penetration},{"isTrigger",dispatch.contact.is_trigger}}}};
            const auto contact_result=Json::parse(scripting_invoke_json(instance.id,dispatch.callback,contact_arguments.dump()),nullptr,false);
            if(contact_result.is_discarded()||!contact_result.value("success",false))break;
        }
        if(scripting_runtime_.type_implements_callback(instance.type_name,"OnFixedUpdate")) {
            const auto fixed=Json::parse(scripting_invoke_json(instance.id,"OnFixedUpdate",arguments),nullptr,false);
            if(fixed.is_discarded()||!fixed.value("success",false))continue;
        }
        if(scripting_runtime_.type_implements_callback(instance.type_name,"OnUpdate"))
            static_cast<void>(scripting_invoke_json(instance.id,"OnUpdate",arguments));
    }
}

void World::consume_animation_cues(const float delta_seconds) {
    world_.each<AnimationCueState>([delta_seconds](flecs::entity, AnimationCueState& cue) {
        cue.age_seconds += std::max(0.0F, delta_seconds);
    });
    for (const auto& event : gameplay_runtime_.events()) {
        if (event.sequence <= last_animation_cue_event_sequence_) continue;
        last_animation_cue_event_sequence_ = event.sequence;
        const auto payload = Json::parse(event.payload_json, nullptr, false);
        if (payload.is_discarded() || !payload.is_object()) continue;
        const auto cue = payload.value("animationCue", std::string{});
        if (cue.empty()) continue;
        const auto target = entity_ids_.find(event.target);
        if (target == entity_ids_.end()) continue;
        world_.entity(target->second).set<AnimationCueState>({cue, event.type, event.sequence, 0.0F});
    }
}

std::size_t World::entity_count() const {
    std::size_t count = 0;
    world_.each<const SemanticIdentity>([&count](flecs::entity, const SemanticIdentity&) { ++count; });
    return count;
}

std::vector<AnimationCompileResult> World::register_gltf_animations(const std::string_view asset_id,
                                                                    const GltfMeshData& source) {
    std::vector<AnimationCompileResult> results;
    for (std::size_t skin = 0; skin < source.skins.size(); ++skin)
        for (std::size_t animation = 0; animation < source.animations.size(); ++animation) {
            auto compiled = animation_runtime_.compile_gltf_asset(asset_id, source, skin, animation);
            results.push_back(std::move(compiled));
        }
    return results;
}

AnimationCookedArtifactLoadResult World::register_cooked_animation(
    const std::span<const std::byte> payload, const std::string_view expected_asset_id,
    const std::string_view expected_source_hash, const std::string_view expected_payload_hash) {
    return animation_runtime_.load_cooked_animation_artifact(
        payload, expected_asset_id, expected_source_hash, expected_payload_hash);
}

void World::configure_animation_player(AnimationPlayer& player) const {
    const auto* machine=animation_state_machines_.find(player.state_machine_asset);if(!machine)return;
    const auto state=std::ranges::find(machine->states,player.active_state,&AnimationStateDefinition::id);
    if(player.active_state.empty()||state==machine->states.end())player.active_state=machine->initial_state;
    for(const auto& parameter:machine->parameters)
        if(!player.state_parameters.contains(parameter.id))player.state_parameters[parameter.id]=parameter.default_value;
    const auto initial=std::ranges::find(machine->states,player.active_state,&AnimationStateDefinition::id);
    if(initial!=machine->states.end()) {
        if(!initial->clip_asset.empty()&&initial->clip_asset!=player.clip_asset) {
            player.clip_asset=initial->clip_asset;player.time_seconds=0.0F;player.next_clip_asset.clear();
            player.next_time_seconds=0.0F;player.next_looping=true;
            player.transition_duration_seconds=0.0F;player.transition_elapsed_seconds=0.0F;
        }
        player.looping=initial->looping;
    }
}

void World::configure_animation_graph_player(AnimationPlayer& player) const {
    const auto* graph=animation_graphs_.find(player.animation_graph_asset);if(graph==nullptr)return;
    const auto state_machine_node=std::ranges::find(graph->nodes,std::string("state-machine"),&AnimationGraphNode::kind);
    if(state_machine_node!=graph->nodes.end()&&player.state_machine_asset!=state_machine_node->state_machine_asset) {
        player.state_machine_asset=state_machine_node->state_machine_asset;player.active_state.clear();player.previous_state.clear();
        player.state_elapsed_seconds=0.0F;player.state_parameters.clear();player.next_clip_asset.clear();
        player.next_time_seconds=0.0F;player.next_looping=true;player.transition_duration_seconds=0.0F;
        player.transition_elapsed_seconds=0.0F;configure_animation_player(player);
    }
    for(const auto& parameter:graph->parameters)
        if(!player.graph_parameters.contains(parameter.id))player.graph_parameters[parameter.id]=parameter.default_value;
    for(const auto& node:graph->nodes)if(node.kind=="clip"&&!player.graph_node_times.contains(node.id))
        player.graph_node_times.emplace(node.id,0.0F);
    for(const auto& group:graph->sync_groups)if(!player.graph_sync_phases.contains(group.id))
        player.graph_sync_phases.emplace(group.id,0.0F);
}

std::optional<AnimationPoseExecutionRequest> World::animation_graph_pose_request(const AnimationPlayer& player) const {
    const auto* graph=animation_graphs_.find(player.animation_graph_asset);if(graph==nullptr||graph->layers.empty())return std::nullopt;
    struct Source final {std::string first;float first_time{};bool first_looping{true};
        std::string second;float second_time{};float second_weight{};bool second_looping{true};};
    const auto find_node=[&](const std::string_view id)->const AnimationGraphNode* {
        const auto found=std::ranges::find(graph->nodes,id,&AnimationGraphNode::id);
        return found==graph->nodes.end()?nullptr:&*found;
    };
    std::function<std::optional<Source>(std::string_view,std::string_view,std::size_t)> resolve;
    resolve=[&](const std::string_view node_id,const std::string_view sync_group,const std::size_t depth)->std::optional<Source> {
        if(depth>32U)return std::nullopt;const auto* node=find_node(node_id);if(node==nullptr)return std::nullopt;
        const auto synchronized_time=[&](const std::string_view clip,const float fallback) {
            const auto phase=player.graph_sync_phases.find(std::string(sync_group));
            if(sync_group.empty()||phase==player.graph_sync_phases.end())return fallback;
            return phase->second*animation_runtime_.duration(clip);
        };
        if(node->kind=="clip") {
            const auto time=player.graph_node_times.find(node->id);
            return Source{node->clip_asset,synchronized_time(node->clip_asset,time==player.graph_node_times.end()?0.0F:time->second),node->looping};
        }
        if(node->kind=="state-machine") {
            if(node->state_machine_asset!=player.state_machine_asset||player.clip_asset.empty())return std::nullopt;
            Source source{player.clip_asset,synchronized_time(player.clip_asset,player.time_seconds),player.looping};
            if(!player.next_clip_asset.empty()) {
                source.second=player.next_clip_asset;source.second_time=synchronized_time(player.next_clip_asset,player.next_time_seconds);
                source.second_weight=player.transition_duration_seconds>0.0F?
                    std::clamp(player.transition_elapsed_seconds/player.transition_duration_seconds,0.0F,1.0F):1.0F;
                source.second_looping=player.next_looping;
            }
            return source;
        }
        const auto selection=AnimationGraphCodec::select_blend_1d(*graph,node->id,player.graph_parameters);
        if(!selection.valid)return std::nullopt;
        const auto first=resolve(selection.first_node,sync_group,depth+1U);
        if(!first||!first->second.empty())return std::nullopt;
        if(selection.first_node==selection.second_node||selection.second_weight<=0.0F)return first;
        const auto second=resolve(selection.second_node,sync_group,depth+1U);
        if(!second||!second->second.empty())return std::nullopt;
        return Source{first->first,first->first_time,first->first_looping,
            second->first,second->first_time,selection.second_weight,second->first_looping};
    };
    const auto base=resolve(graph->layers.front().root_node,graph->layers.front().sync_group,1U);if(!base)return std::nullopt;
    AnimationPoseExecutionRequest request{.base_clip_asset=base->first,.base_time=base->first_time,
        .base_looping=base->first_looping,
        .base_secondary_clip_asset=base->second,.base_secondary_time=base->second_time,
        .base_secondary_weight=base->second_weight,.base_secondary_looping=base->second_looping};
    for(std::size_t index=1;index<graph->layers.size();++index) {
        const auto& definition=graph->layers[index];const auto source=resolve(definition.root_node,definition.sync_group,1U);
        if(!source)return std::nullopt;auto weight=definition.weight;
        if(!definition.weight_parameter.empty()) {
            const auto parameter=player.graph_parameters.find(definition.weight_parameter);
            if(parameter==player.graph_parameters.end())return std::nullopt;weight*=std::clamp(parameter->second,0.0F,1.0F);
        }
        request.layers.push_back({.id=definition.id,.clip_asset=source->first,.time=source->first_time,
            .secondary_clip_asset=source->second,.secondary_time=source->second_time,.secondary_weight=source->second_weight,
            .mode=definition.mode=="additive"?AnimationPoseLayerMode::additive:AnimationPoseLayerMode::override_layer,
            .weight=weight,.mask_id=definition.mask_id});
    }
    for(const auto& mask:graph->masks) {
        AnimationPoseMask runtime_mask{.id=mask.id,.include_descendants=mask.include_descendants};
        for(const auto& joint:mask.joints)runtime_mask.joints.push_back({joint.name,joint.weight});
        request.masks.push_back(std::move(runtime_mask));
    }
    return request;
}

bool World::register_animation_state_machine(AnimationStateMachineDocument document) {
    const auto asset_id=document.asset_id;if(!animation_state_machines_.register_document(std::move(document)))return false;
    for(const auto& [id,entity_id]:entity_ids_) {
        auto entity=world_.entity(entity_id);const auto* current=entity.try_get<AnimationPlayer>();
        if(!current||current->state_machine_asset!=asset_id)continue;
        auto player=*current;player.active_state.clear();player.previous_state.clear();player.state_elapsed_seconds=0.0F;
        player.next_clip_asset.clear();player.next_time_seconds=0.0F;
        player.next_looping=true;player.transition_duration_seconds=0.0F;player.transition_elapsed_seconds=0.0F;
        player.state_parameters.clear();configure_animation_player(player);entity.set<AnimationPlayer>(std::move(player));
    }
    return true;
}

bool World::register_animation_graph(AnimationGraphDocument document) {
    const auto asset_id=document.asset_id;if(!animation_graphs_.register_document(std::move(document)))return false;
    for(const auto& [id,entity_id]:entity_ids_) {
        static_cast<void>(id);auto entity=world_.entity(entity_id);const auto* current=entity.try_get<AnimationPlayer>();
        if(current==nullptr||current->animation_graph_asset!=asset_id)continue;
        auto player=*current;player.graph_parameters.clear();player.graph_node_times.clear();player.graph_sync_phases.clear();
        configure_animation_graph_player(player);
        entity.set<AnimationPlayer>(std::move(player));
    }
    return true;
}

bool World::register_sprite_asset(SpriteAssetDocument document) {
    return sprite_assets_.register_asset(std::move(document));
}

bool World::register_tile_palette(TilePaletteDocument document){return tilemap_assets_.register_palette(std::move(document));}
bool World::register_tilemap_asset(TilemapDocument document){return tilemap_assets_.register_tilemap(std::move(document));}

std::vector<WorldEntityView> World::entity_views(const std::optional<TilemapViewQuery> tilemap_query) const {
    return entity_views_impl(tilemap_query,true);
}

std::vector<WorldEntityView> World::entity_views_impl(const std::optional<TilemapViewQuery> tilemap_query,
                                                       const bool include_skeletal_pose) const {
    std::optional<TilemapVisibilityCamera> visibility_camera;
    if(tilemap_query&&tilemap_query->viewport_width>0&&tilemap_query->viewport_height>0) {
        visibility_camera=tilemap_query->camera_override;
        if(!visibility_camera)world_.each<const Camera>([&](const flecs::entity entity,const Camera& camera) {
            const auto* transform=entity.try_get<Transform>();
            if(!visibility_camera&&camera.primary&&transform)visibility_camera=TilemapVisibilityCamera{
                {transform->x,transform->y,transform->z},{camera.target_x,camera.target_y,camera.target_z},camera.vertical_fov_degrees,
                camera.near_clip,camera.far_clip,camera.projection,camera.orthographic_height};
        });
    }
    const auto chunk_visible=[&](const Transform& transform,const ResolvedTilemapAsset& asset,const CompiledTilemapChunk& chunk) {
        if(!visibility_camera||!tilemap_query)return true;
        const auto& camera=*visibility_camera;
        const auto subtract=[](const std::array<float,3>& a,const std::array<float,3>& b) {return std::array<float,3>{a[0]-b[0],a[1]-b[1],a[2]-b[2]};};
        const auto dot=[](const std::array<float,3>& a,const std::array<float,3>& b) {return a[0]*b[0]+a[1]*b[1]+a[2]*b[2];};
        const auto normalize=[&](const std::array<float,3>& value) {const auto length=std::sqrt(dot(value,value));
            return length>1.0e-6F?std::array<float,3>{value[0]/length,value[1]/length,value[2]/length}:std::array<float,3>{};};
        const auto cross=[](const std::array<float,3>& a,const std::array<float,3>& b) {return std::array<float,3>{
            a[1]*b[2]-a[2]*b[1],a[2]*b[0]-a[0]*b[2],a[0]*b[1]-a[1]*b[0]};};
        const auto rotate=[&](const std::array<float,4>& q,const std::array<float,3>& p) {
            const std::array<float,3> u{q[0],q[1],q[2]};const float s=q[3],uv=u[0]*p[0]+u[1]*p[1]+u[2]*p[2],uu=dot(u,u);
            return std::array<float,3>{2.0F*uv*u[0]+(s*s-uu)*p[0]+2.0F*s*(u[1]*p[2]-u[2]*p[1]),
                2.0F*uv*u[1]+(s*s-uu)*p[1]+2.0F*s*(u[2]*p[0]-u[0]*p[2]),
                2.0F*uv*u[2]+(s*s-uu)*p[2]+2.0F*s*(u[0]*p[1]-u[1]*p[0])};};
        const float left=static_cast<float>(chunk.minimum_cell_x)*asset.tilemap.cell_width;
        const float right_edge=static_cast<float>(chunk.maximum_cell_x+1)*asset.tilemap.cell_width;
        const float bottom=static_cast<float>(chunk.minimum_cell_y)*asset.tilemap.cell_height;
        const float top=static_cast<float>(chunk.maximum_cell_y+1)*asset.tilemap.cell_height;
        const std::array<float,3> local_center{(left+right_edge)*0.5F*transform.scale_x,(bottom+top)*0.5F*transform.scale_y,0.0F};
        const auto rotated=rotate({transform.rotation_x,transform.rotation_y,transform.rotation_z,transform.rotation_w},local_center);
        const std::array<float,3> center{transform.x+rotated[0],transform.y+rotated[1],transform.z+rotated[2]};
        const float half_width=std::abs(right_edge-left)*std::abs(transform.scale_x)*0.5F;
        const float half_height=std::abs(top-bottom)*std::abs(transform.scale_y)*0.5F;
        const float radius=std::sqrt(half_width*half_width+half_height*half_height);
        const auto forward=normalize(subtract(camera.target,camera.position));auto view_right=normalize(cross(forward,{0.0F,1.0F,0.0F}));
        if(dot(view_right,view_right)<0.5F)view_right={1.0F,0.0F,0.0F};const auto up=normalize(cross(view_right,forward));
        if(dot(forward,forward)<0.5F||dot(up,up)<0.5F)return true;
        const auto delta=subtract(center,camera.position);const float depth=dot(delta,forward);
        bool inside=depth+radius>=camera.near_clip&&depth-radius<=camera.far_clip;
        const float aspect=static_cast<float>(tilemap_query->viewport_width)/static_cast<float>(tilemap_query->viewport_height);
        if(inside&&camera.projection=="orthographic") {const float hh=camera.orthographic_height*0.5F,hw=hh*aspect;
            inside=std::abs(dot(delta,view_right))<=hw+radius&&std::abs(dot(delta,up))<=hh+radius;
        } else if(inside) {const float tangent=std::tan(camera.vertical_fov_degrees*0.008726646259971648F);
            inside=depth+radius>0.0F&&std::abs(dot(delta,view_right))<=std::max(depth,0.0F)*tangent*aspect+radius&&
                std::abs(dot(delta,up))<=std::max(depth,0.0F)*tangent+radius;}
        return inside;
    };
    std::vector<WorldEntityView> views;
    views.reserve(entity_ids_.size());
    world_.each<const SemanticIdentity>([&](const flecs::entity entity, const SemanticIdentity& identity) {
        WorldEntityView view{
            .id = identity.id,
            .display_name = identity.display_name,
            .type = identity.type,
            .scene_guid = identity.scene_guid,
            .parent_guid = identity.parent_guid,
            .source = identity.source,
            .revision = revision_
        };
        if (const auto* transform = entity.try_get<Transform>()) view.transform = *transform;
        if (const auto* velocity = entity.try_get<Velocity>()) view.velocity = *velocity;
        if (const auto* body = entity.try_get<RigidBody>()) view.rigid_body = *body;
        if (const auto* collider = entity.try_get<BoxCollider>()) view.box_collider = *collider;
        if (const auto* collider = entity.try_get<SphereCollider>()) view.sphere_collider = *collider;
        if (const auto* collider = entity.try_get<CapsuleCollider>()) view.capsule_collider = *collider;
        if (const auto* motor = entity.try_get<CharacterMotor2D>()) view.character_motor_2d = *motor;
        if (const auto* platform = entity.try_get<Platform2D>()) view.platform_2d = *platform;
        if (const auto* collider = entity.try_get<ConvexHullCollider>()) view.convex_hull_collider = *collider;
        if (const auto* player = entity.try_get<AnimationPlayer>()) {
            view.animation_player = *player;
            if(include_skeletal_pose) {
                SkeletalPose pose;
                if(const auto request=animation_graph_pose_request(*player)) {
                    const auto executed=animation_runtime_.sample_layered_skeletal_pose(*request);
                    if(executed.success)pose=executed.pose;
                }
                if(!pose.valid&&player->animation_graph_asset.empty())pose = player->next_clip_asset.empty() ?
                    animation_runtime_.sample_skeletal_pose(player->clip_asset, player->time_seconds) :
                    animation_runtime_.sample_blended_skeletal_pose(player->clip_asset, player->time_seconds,
                        player->next_clip_asset, player->next_time_seconds,
                        player->transition_duration_seconds > 0.0F ? player->transition_elapsed_seconds / player->transition_duration_seconds : 1.0F);
                // Applied root motion belongs to the entity transform. Remove it from the sampled
                // palette so rendering does not apply the same displacement a second time.
                if (pose.valid && player->root_motion_mode == "apply" && !pose.joints.empty()) {
                    const auto root_x = pose.joints.front().model_x;
                    const auto root_y = pose.joints.front().model_y;
                    const auto root_z = pose.joints.front().model_z;
                    for (auto& matrix : pose.skinning_matrices) {
                        matrix[12] -= root_x; matrix[13] -= root_y; matrix[14] -= root_z;
                    }
                    for (auto& joint : pose.joints) {
                        joint.model_x -= root_x; joint.model_y -= root_y; joint.model_z -= root_z;
                    }
                }
                if (pose.valid) view.skeletal_pose = std::move(pose);
            }
        }
        if (const auto* cue = entity.try_get<AnimationCueState>()) view.animation_cue = *cue;
        if (const auto* camera = entity.try_get<Camera>()) view.camera = *camera;
        if (const auto* follow = entity.try_get<CameraFollow2D>()) view.camera_follow_2d = *follow;
        if (const auto* light = entity.try_get<DirectionalLight>()) view.directional_light = *light;
        if (const auto* light = entity.try_get<LocalLight>()) view.local_light = *light;
        if (const auto* renderer = entity.try_get<MeshRenderer>()) view.mesh_renderer = *renderer;
        if (const auto* renderer = entity.try_get<SpriteRenderer>()) {
            view.sprite_renderer = *renderer;
            view.sprite_frame = sprite_assets_.resolve(renderer->playback);
        }
        if(const auto* renderer=entity.try_get<TilemapRenderer>()) {
            view.tilemap_renderer=*renderer;view.tilemap_asset=tilemap_assets_.resolve(renderer->tilemap_asset);
            if(const auto* compiled=tilemap_assets_.resolve_compiled(renderer->tilemap_asset)) {
                constexpr std::size_t maximum_resolved_tilemap_cells=65536;
                const auto& map=compiled->source.tilemap;const auto& palette=compiled->source.palette;
                view.tilemap_total_cell_count=compiled->total_cell_count;view.tilemap_compiled_chunk_count=compiled->chunks.size();
                view.tilemap_compilation_revision=compiled->compilation_revision;
                const auto* tilemap_transform=entity.try_get<Transform>();
                view.tilemap_early_visibility_applied=visibility_camera.has_value()&&tilemap_transform!=nullptr;
                for(const auto& chunk:compiled->chunks) {
                    if(view.tilemap_early_visibility_applied&&(!renderer->visible||!chunk_visible(*tilemap_transform,compiled->source,chunk))) {
                        ++view.tilemap_skipped_chunk_count;view.tilemap_cells_skipped_before_resolution+=chunk.cells.size();continue;
                    }
                    ++view.tilemap_resolved_chunk_count;
                    for(const auto& cell:chunk.cells) {
                        if(view.tilemap_cells.size()>=maximum_resolved_tilemap_cells)continue;
                        auto frame=sprite_assets_.resolve_frame(palette.sprite_asset,cell.frame_id);if(!frame)continue;
                        const auto stable_id=view.id+"/tile/"+chunk.layer_id+"/"+std::to_string(cell.cell_x)+","+std::to_string(cell.cell_y);
                        view.tilemap_cells.push_back({stable_id,map.asset_id,chunk.layer_id,chunk.sorting_layer,chunk.sorting_order,
                            cell.cell_x,cell.cell_y,chunk.chunk_x,chunk.chunk_y,chunk.content_fingerprint,map.cell_width,map.cell_height,cell.tile_id,cell.autotile_group,
                            cell.autotile_mask,cell.flip_x,cell.flip_y,std::move(*frame)});
                    }
                }
                view.tilemap_cells_truncated=view.tilemap_cells.size()+view.tilemap_cells_skipped_before_resolution<view.tilemap_total_cell_count;
            }
        }
        if (const auto* material = entity.try_get<PbrMaterial>()) view.pbr_material = *material;
        views.push_back(std::move(view));
    });
    std::ranges::sort(views, {}, &WorldEntityView::id);
    return views;
}

std::string World::observe_json(const ObservationQuery& query) const {
    const auto views = entity_views();
    const std::unordered_set<std::string> requested(query.entity_ids.begin(), query.entity_ids.end());
    const std::unordered_set<std::string> fields(query.fields.begin(), query.fields.end());
    const bool all_fields = fields.empty();

    auto in_scope = [&](const WorldEntityView& view) {
        if (requested.empty() || requested.contains(view.id)) return true;
        std::string parent = view.parent_guid;
        for (std::size_t level = 0; level < query.depth && !parent.empty(); ++level) {
            if (requested.contains(parent)) return true;
            const auto ancestor = std::ranges::find(views, parent, &WorldEntityView::id);
            parent = ancestor == views.end() ? std::string{} : ancestor->parent_guid;
        }
        return false;
    };

    std::vector<const WorldEntityView*> scoped;
    for (const auto& view : views) if (in_scope(view)) scoped.push_back(&view);
    const auto cursor = std::min(query.cursor, scoped.size());
    Json entities = Json::array();
    std::size_t next_cursor = cursor;
    const auto budget = std::max<std::size_t>(query.byte_budget, 512);
    for (std::size_t index = cursor; index < scoped.size(); ++index) {
        const auto& view = *scoped[index];
        Json entity = {{"id", view.id}, {"revision", view.revision}};
        if (all_fields || fields.contains("identity")) {
            entity["displayName"] = view.display_name;
            entity["type"] = view.type;
        }
        if (all_fields || fields.contains("hierarchy")) {
            entity["parentId"] = view.parent_guid.empty() ? Json(nullptr) : Json(view.parent_guid);
        }
        if (all_fields || fields.contains("source")) {
            entity["source"] = {{"uri", view.source.uri}, {"pointer", view.source.json_pointer}};
        }
        if ((all_fields || fields.contains("transform")) && view.transform) {
            entity["transform"] = {{"position", vector_json(semantic_vector(*view.transform))}};
        }
        if ((all_fields || fields.contains("velocity")) && view.velocity) {
            entity["velocity"] = {{"linear", vector_json({view.velocity->x, view.velocity->y, view.velocity->z})}};
        }
        if ((all_fields || fields.contains("physics")) && view.rigid_body) {
            entity["rigidBody"] = {{"motionType", motion_type_name(view.rigid_body->motion_type)}, {"mass", view.rigid_body->mass},
                {"gravityFactor", view.rigid_body->gravity_factor}, {"linearDamping", view.rigid_body->linear_damping}};
        }
        if ((all_fields || fields.contains("physics")) && view.box_collider) {
            entity["boxCollider"] = {{"halfExtents", vector_json({view.box_collider->half_x, view.box_collider->half_y, view.box_collider->half_z})},
                {"friction", view.box_collider->friction}, {"restitution", view.box_collider->restitution},{"isTrigger",view.box_collider->is_trigger}};
        }
        if ((all_fields || fields.contains("physics")) && view.sphere_collider) {
            entity["sphereCollider"] = {{"radius", view.sphere_collider->radius}, {"friction", view.sphere_collider->friction},
                {"restitution", view.sphere_collider->restitution},{"isTrigger",view.sphere_collider->is_trigger}};
        }
        if ((all_fields || fields.contains("physics")) && view.capsule_collider) {
            entity["capsuleCollider"] = {{"radius", view.capsule_collider->radius}, {"halfHeight", view.capsule_collider->half_height},
                {"friction", view.capsule_collider->friction}, {"restitution", view.capsule_collider->restitution},{"isTrigger",view.capsule_collider->is_trigger}};
        }
        if ((all_fields || fields.contains("gameplay")) && view.character_motor_2d) {
            entity["characterMotor2D"] = {{"grounded", view.character_motor_2d->state.grounded},
                {"decision", view.character_motor_2d->state.decision}, {"reason", view.character_motor_2d->state.reason},
                {"groundEntityId", view.character_motor_2d->state.ground_entity_id},
                {"wallEntityId", view.character_motor_2d->state.wall_entity_id},
                {"groundNormal",Json::array({view.character_motor_2d->state.ground_normal_x,view.character_motor_2d->state.ground_normal_y})},
                {"moveInput", view.character_motor_2d->state.move_input}};
        }
        if((all_fields||fields.contains("gameplay"))&&view.platform_2d) entity["platform2D"]={{"collisionMode",view.platform_2d->collision_mode},
            {"motionDistance",view.platform_2d->motion_distance},{"motionPeriodSeconds",view.platform_2d->motion_period_seconds}};
        if((all_fields||fields.contains("physics"))&&view.convex_hull_collider) {
            Json points=Json::array(); for(const auto& point:view.convex_hull_collider->points) points.push_back(Json::array({point[0],point[1],point[2]}));
            entity["convexHullCollider"]={{"points",std::move(points)},{"pointCount",view.convex_hull_collider->points.size()},
                {"friction",view.convex_hull_collider->friction},{"restitution",view.convex_hull_collider->restitution},
                {"isTrigger",view.convex_hull_collider->is_trigger}};
        }
        if ((all_fields || fields.contains("animation")) && view.animation_player) {
            entity["animationPlayer"] = {{"clipAsset", view.animation_player->clip_asset}, {"timeSeconds", view.animation_player->time_seconds},
                {"durationSeconds", animation_runtime_.duration(view.animation_player->clip_asset)}, {"playbackSpeed", view.animation_player->playback_speed},
                {"looping", view.animation_player->looping}, {"playing", view.animation_player->playing},
                {"rootMotionMode", view.animation_player->root_motion_mode}, {"nextClipAsset", view.animation_player->next_clip_asset},
                {"transitionDurationSeconds", view.animation_player->transition_duration_seconds},
                {"transitionElapsedSeconds", view.animation_player->transition_elapsed_seconds}};
        }
        if ((all_fields || fields.contains("render")) && view.camera) {
            entity["camera"] = {
                {"target", vector_json({view.camera->target_x, view.camera->target_y, view.camera->target_z})},
                {"verticalFovDegrees", view.camera->vertical_fov_degrees},
                {"nearClip", view.camera->near_clip}, {"farClip", view.camera->far_clip},
                {"primary", view.camera->primary}, {"projection", view.camera->projection},
                {"orthographicHeight", view.camera->orthographic_height}
            };
        }
        if((all_fields||fields.contains("render"))&&view.camera_follow_2d) entity["cameraFollow2D"]={{"targetEntityId",view.camera_follow_2d->target_entity_id},
            {"center",Json::array({view.camera_follow_2d->center_x,view.camera_follow_2d->center_y})},{"decision",view.camera_follow_2d->decision}};
        if ((all_fields || fields.contains("render")) && view.directional_light) {
            entity["directionalLight"] = {
                {"direction", vector_json({view.directional_light->direction_x, view.directional_light->direction_y, view.directional_light->direction_z})},
                {"color", vector_json({view.directional_light->color_r, view.directional_light->color_g, view.directional_light->color_b})},
                {"intensity", view.directional_light->intensity},
                {"ambientIntensity", view.directional_light->ambient_intensity},
                {"castsShadows", view.directional_light->casts_shadows}
            };
        }
        if ((all_fields || fields.contains("render")) && view.mesh_renderer) {
            entity["meshRenderer"] = {
                {"meshAsset", view.mesh_renderer->mesh_asset}, {"visible", view.mesh_renderer->visible},
                {"castsShadows", view.mesh_renderer->casts_shadows}, {"receivesShadows", view.mesh_renderer->receives_shadows}
            };
        }
        if((all_fields||fields.contains("render"))&&view.tilemap_renderer)entity["tilemapRenderer"]={
            {"tilemapAsset",view.tilemap_renderer->tilemap_asset},{"visible",view.tilemap_renderer->visible},
            {"collisionEnabled",view.tilemap_renderer->collision_enabled}};
        if ((all_fields || fields.contains("render")) && view.pbr_material) {
            entity["pbrMaterial"] = {
                {"baseColor", vector_json({view.pbr_material->base_r, view.pbr_material->base_g, view.pbr_material->base_b})},
                {"metallic", view.pbr_material->metallic}, {"roughness", view.pbr_material->roughness},
                {"baseColorTexture", view.pbr_material->base_color_texture.empty() ? Json(nullptr) : Json(view.pbr_material->base_color_texture)},
                {"emissiveColor", vector_json({view.pbr_material->emissive_r, view.pbr_material->emissive_g, view.pbr_material->emissive_b})},
                {"emissiveIntensity", view.pbr_material->emissive_intensity}
            };
        }
        auto candidate = entities;
        candidate.push_back(entity);
        const Json preview = {
            {"schemaVersion", "0.1"},
            {"revision", revision_},
            {"entities", candidate}
        };
        if (preview.dump().size() > budget && !entities.empty()) break;
        entities.push_back(std::move(entity));
        next_cursor = index + 1;
    }

    const bool truncated = next_cursor < scoped.size();
    const Json result = {
        {"schemaVersion", "0.1"},
        {"revision", revision_},
        {"scope", {
            {"entityIds", query.entity_ids},
            {"fields", query.fields},
            {"depth", query.depth},
            {"byteBudget", budget}
        }},
        {"cursor", cursor},
        {"nextCursor", truncated ? Json(next_cursor) : Json(nullptr)},
        {"truncated", truncated},
        {"entities", std::move(entities)}
    };
    return result.dump();
}

std::string World::delta_json(const std::uint64_t since_revision) const {
    Json changes = Json::array();
    bool resync_required = since_revision > revision_;
    if (!recent_deltas_.empty() && since_revision < recent_deltas_.front().revision_before) {
        resync_required = true;
    } else if (recent_deltas_.empty() && since_revision < revision_) {
        resync_required = true;
    }
    if (!resync_required) {
        for (const auto& delta : recent_deltas_) {
            if (delta.revision_after > since_revision) changes.push_back(delta_to_json(delta));
        }
    }
    return Json{
        {"schemaVersion", "0.1"},
        {"fromRevision", since_revision},
        {"toRevision", revision_},
        {"resyncRequired", resync_required},
        {"changes", std::move(changes)}
    }.dump();
}

TransformChangePlan World::plan_transform_update(
    const std::string_view entity_id,
    const Transform transform,
    const std::uint64_t base_revision,
    const std::string_view manager) const {
    TransformChangePlan plan{
        .manager = std::string(manager),
        .entity_id = std::string(entity_id),
        .base_revision = base_revision,
        .after = semantic_vector(transform)
    };
    if (manager.empty()) {
        plan.code = "world.manager-required";
        plan.detail = "A stable manager identity is required.";
        return plan;
    }
    if (base_revision != revision_) {
        plan.code = "world.revision-conflict";
        plan.detail = "Base revision does not match the current World revision.";
        return plan;
    }
    const auto found = entity_ids_.find(std::string(entity_id));
    if (found == entity_ids_.end()) {
        plan.code = "world.entity-not-found";
        plan.detail = "No entity has the requested stable ID.";
        return plan;
    }
    const auto entity = world_.entity(found->second);
    const auto* before = entity.try_get<Transform>();
    if (before == nullptr) {
        plan.code = "world.component-not-found";
        plan.detail = "The entity does not have a Transform component.";
        return plan;
    }
    plan.before = semantic_vector(*before);
    plan.content_hash = plan_hash(plan.manager, plan.entity_id, plan.base_revision, plan.before, plan.after);
    plan.plan_id = "transform-plan-" + plan.content_hash.substr(plan.content_hash.find(':') + 1);
    plan.valid = true;
    plan.code = "ok";
    plan.detail = "Plan validated without changing World state.";
    return plan;
}

std::optional<std::string> World::property_value_json(const std::string_view entity_id,
                                                      const std::string_view property) const {
    const auto found = entity_ids_.find(std::string(entity_id));
    if (found == entity_ids_.end()) return std::nullopt;
    const auto entity = world_.entity(found->second);
    if (property == "engine.entity.transform.position") {
        if (const auto* value = entity.try_get<Transform>()) return vector_json({value->x, value->y, value->z}).dump();
    }
    if (property == "engine.entity.transform.scale") {
        if (const auto* value = entity.try_get<Transform>()) return vector_json({value->scale_x, value->scale_y, value->scale_z}).dump();
    }
    if (property == "engine.entity.transform.rotationEulerDegrees") {
        if (const auto* value = entity.try_get<Transform>()) return rotation_euler_json(*value).dump();
    }
    if(property=="engine.entity.velocity.linear") {
        if(const auto* value=entity.try_get<Velocity>())return vector_json({value->x,value->y,value->z}).dump();
    }
    if (const auto* value = entity.try_get<PbrMaterial>()) {
        if (property == "engine.entity.material.baseColor") return vector_json({value->base_r, value->base_g, value->base_b}).dump();
        if (property == "engine.entity.material.metallic") return Json(value->metallic).dump();
        if (property == "engine.entity.material.roughness") return Json(value->roughness).dump();
        if (property == "engine.entity.material.emissiveColor") return vector_json({value->emissive_r, value->emissive_g, value->emissive_b}).dump();
        if (property == "engine.entity.material.emissiveIntensity") return Json(value->emissive_intensity).dump();
    }
    if(const auto* value=entity.try_get<LocalLight>()) {
        if(property=="engine.entity.localLight.kind")return Json(value->kind).dump();
        if(property=="engine.entity.localLight.color")return vector_json({value->color_r,value->color_g,value->color_b}).dump();
        if(property=="engine.entity.localLight.luminousPowerLumens")return Json(value->luminous_power_lumens).dump();
        if(property=="engine.entity.localLight.rangeMeters")return Json(value->range_meters).dump();
        if(property=="engine.entity.localLight.direction")return vector_json({value->direction_x,value->direction_y,value->direction_z}).dump();
        if(property=="engine.entity.localLight.innerConeDegrees")return Json(value->inner_cone_degrees).dump();
        if(property=="engine.entity.localLight.outerConeDegrees")return Json(value->outer_cone_degrees).dump();
        if(property=="engine.entity.localLight.sourceRadiusMeters")return Json(value->source_radius_meters).dump();
    }
    if (const auto* value = entity.try_get<BoxCollider>()) {
        if (property == "engine.entity.collider.halfExtents") return vector_json({value->half_x, value->half_y, value->half_z}).dump();
        if (property == "engine.entity.collider.friction") return Json(value->friction).dump();
        if (property == "engine.entity.collider.restitution") return Json(value->restitution).dump();
        if (property == "engine.entity.collider.isTrigger") return Json(value->is_trigger).dump();
    }
    if (const auto* value = entity.try_get<SphereCollider>()) {
        if (property == "engine.entity.collider.radius") return Json(value->radius).dump();
        if (property == "engine.entity.collider.friction") return Json(value->friction).dump();
        if (property == "engine.entity.collider.restitution") return Json(value->restitution).dump();
        if (property == "engine.entity.collider.isTrigger") return Json(value->is_trigger).dump();
    }
    if (const auto* value = entity.try_get<CapsuleCollider>()) {
        if (property == "engine.entity.collider.radius") return Json(value->radius).dump();
        if (property == "engine.entity.collider.halfHeight") return Json(value->half_height).dump();
        if (property == "engine.entity.collider.friction") return Json(value->friction).dump();
        if (property == "engine.entity.collider.restitution") return Json(value->restitution).dump();
        if (property == "engine.entity.collider.isTrigger") return Json(value->is_trigger).dump();
    }
    if(const auto* value=entity.try_get<ConvexHullCollider>()) {
        if(property=="engine.entity.collider.friction") return Json(value->friction).dump();
        if(property=="engine.entity.collider.restitution") return Json(value->restitution).dump();
        if(property=="engine.entity.collider.isTrigger") return Json(value->is_trigger).dump();
    }
    if (const auto* value = entity.try_get<AnimationPlayer>()) {
        if (property == "engine.entity.animation.clipAsset") return Json(value->clip_asset).dump();
        if (property == "engine.entity.animation.playbackSpeed") return Json(value->playback_speed).dump();
        if (property == "engine.entity.animation.looping") return Json(value->looping).dump();
        if (property == "engine.entity.animation.playing") return Json(value->playing).dump();
        if (property == "engine.entity.animation.nextClipAsset") return Json(value->next_clip_asset).dump();
        if (property == "engine.entity.animation.transitionDuration") return Json(value->transition_duration_seconds).dump();
        if (property == "engine.entity.animation.rootMotionMode") return Json(value->root_motion_mode).dump();
        if (property == "engine.entity.animation.stateMachineAsset") return Json(value->state_machine_asset).dump();
        if (property == "engine.entity.animation.animationGraphAsset") return Json(value->animation_graph_asset).dump();
    }
    if(const auto* value=entity.try_get<SpriteRenderer>()) {
        if(property=="engine.entity.sprite.spriteAsset")return Json(value->playback.asset_id).dump();
        if(property=="engine.entity.sprite.clip")return Json(value->playback.clip_id).dump();
        if(property=="engine.entity.sprite.playbackSpeed")return Json(value->playback_speed).dump();
        if(property=="engine.entity.sprite.playing")return Json(value->playback.playing).dump();
        if(property=="engine.entity.sprite.flipX")return Json(value->flip_x).dump();
        if(property=="engine.entity.sprite.flipY")return Json(value->flip_y).dump();
        if(property=="engine.entity.sprite.sortingLayer")return Json(value->sorting_layer).dump();
        if(property=="engine.entity.sprite.sortingOrder")return Json(value->sorting_order).dump();
        if(property=="engine.entity.sprite.visible")return Json(value->visible).dump();
    }
    if(const auto* value=entity.try_get<TilemapRenderer>()) {
        if(property=="engine.entity.tilemap.tilemapAsset")return Json(value->tilemap_asset).dump();
        if(property=="engine.entity.tilemap.visible")return Json(value->visible).dump();
        if(property=="engine.entity.tilemap.collisionEnabled")return Json(value->collision_enabled).dump();
    }
    const auto document_entity=std::ranges::find(scene_document_.entities,entity_id,&SceneEntityDocument::guid);
    if(document_entity!=scene_document_.entities.end()&&document_entity->managed_script) {
        const auto& value=*document_entity->managed_script;
        if(property=="engine.entity.managedScript.assemblyAsset") return Json(value.assembly_asset).dump();
        if(property=="engine.entity.managedScript.typeName") return Json(value.type_name).dump();
        if(property=="engine.entity.managedScript.enabled") return Json(value.enabled).dump();
        if(property=="engine.entity.managedScript.properties") return value.properties_json;
    }
    return std::nullopt;
}

PropertyChangePlan World::plan_property_update(const std::string_view entity_id, const std::string_view property,
                                               const std::string_view value_json, const std::uint64_t base_revision,
                                               const std::string_view manager) const {
    PropertyChangePlan plan{.manager=std::string(manager), .entity_id=std::string(entity_id),
        .property=std::string(property), .base_revision=base_revision};
    if (manager.empty()) { plan.code="world.manager-required"; plan.detail="A stable manager identity is required."; return plan; }
    if (base_revision != revision_) { plan.code="world.revision-conflict"; plan.detail="Base revision does not match the current World revision."; return plan; }
    const auto before = property_value_json(entity_id, property);
    if (!before) { plan.code="world.property-not-found"; plan.detail="The entity does not expose the requested editable property."; return plan; }
    Json value;
    try { value = Json::parse(value_json); }
    catch (...) { plan.code="world.invalid-property-value"; plan.detail="Property value is not valid JSON."; return plan; }
    const auto finite_number = [&] { return value.is_number() && std::isfinite(value.get<double>()); };
    const auto finite_vector = [&] {
        return value.is_object() && value.contains("x") && value.contains("y") && value.contains("z") &&
            value.at("x").is_number() && value.at("y").is_number() && value.at("z").is_number() &&
            std::isfinite(value.at("x").get<double>()) && std::isfinite(value.at("y").get<double>()) &&
            std::isfinite(value.at("z").get<double>());
    };
    bool valid = false;
    if (property == "engine.entity.transform.position") valid = finite_vector();
    else if (property == "engine.entity.transform.rotationEulerDegrees") valid = finite_vector();
    else if(property=="engine.entity.velocity.linear")valid=finite_vector();
    else if (property == "engine.entity.transform.scale") valid = finite_vector() && value.at("x").get<double>()>0.0 &&
        value.at("y").get<double>()>0.0 && value.at("z").get<double>()>0.0;
    else if (property == "engine.entity.material.baseColor") valid = finite_vector() && value.at("x").get<double>()>=0.0 &&
        value.at("y").get<double>()>=0.0 && value.at("z").get<double>()>=0.0 && value.at("x").get<double>()<=1.0 &&
        value.at("y").get<double>()<=1.0 && value.at("z").get<double>()<=1.0;
    else if (property == "engine.entity.material.emissiveColor") valid = finite_vector() && value.at("x").get<double>()>=0.0 &&
        value.at("y").get<double>()>=0.0 && value.at("z").get<double>()>=0.0;
    else if (property == "engine.entity.collider.halfExtents") valid = finite_vector() && value.at("x").get<double>()>0.0 &&
        value.at("y").get<double>()>0.0 && value.at("z").get<double>()>0.0;
    else if (property == "engine.entity.material.metallic" || property == "engine.entity.material.roughness" ||
        property == "engine.entity.collider.restitution") valid = finite_number() && value.get<double>() >= 0.0 && value.get<double>() <= 1.0;
    else if (property == "engine.entity.material.emissiveIntensity" || property == "engine.entity.collider.friction" ||
        property == "engine.entity.animation.transitionDuration") valid = finite_number() && value.get<double>() >= 0.0;
    else if(property=="engine.entity.localLight.kind")valid=value.is_string()&&
        (value.get_ref<const std::string&>()=="point"||value.get_ref<const std::string&>()=="spot");
    else if(property=="engine.entity.localLight.color")valid=finite_vector()&&value.at("x").get<double>()>=0.0&&
        value.at("y").get<double>()>=0.0&&value.at("z").get<double>()>=0.0&&value.at("x").get<double>()<=1.0&&
        value.at("y").get<double>()<=1.0&&value.at("z").get<double>()<=1.0;
    else if(property=="engine.entity.localLight.direction")valid=finite_vector()&&
        (value.at("x").get<double>()!=0.0||value.at("y").get<double>()!=0.0||value.at("z").get<double>()!=0.0);
    else if(property=="engine.entity.localLight.luminousPowerLumens"||property=="engine.entity.localLight.sourceRadiusMeters")
        valid=finite_number()&&value.get<double>()>=0.0;
    else if(property=="engine.entity.localLight.rangeMeters")valid=finite_number()&&value.get<double>()>0.0;
    else if(property=="engine.entity.localLight.innerConeDegrees"||property=="engine.entity.localLight.outerConeDegrees")
        valid=finite_number()&&value.get<double>()>=0.0&&value.get<double>()<=89.0;
    else if (property == "engine.entity.collider.radius" || property == "engine.entity.collider.halfHeight") valid = finite_number() && value.get<double>() > 0.0;
    else if (property == "engine.entity.animation.playbackSpeed") valid = finite_number();
    else if (property == "engine.entity.animation.looping" || property == "engine.entity.animation.playing") valid = value.is_boolean();
    else if (property == "engine.entity.collider.isTrigger") valid = value.is_boolean();
    else if (property == "engine.entity.animation.clipAsset") valid = value.is_string() && !value.get_ref<const std::string&>().empty();
    else if (property == "engine.entity.animation.stateMachineAsset") valid = value.is_string() && !value.get_ref<const std::string&>().empty();
    else if (property == "engine.entity.animation.animationGraphAsset") valid = value.is_string();
    else if (property == "engine.entity.animation.nextClipAsset") valid = value.is_string();
    else if (property == "engine.entity.animation.rootMotionMode") valid = value.is_string() &&
        (value.get_ref<const std::string&>() == "ignore" || value.get_ref<const std::string&>() == "apply");
    else if(property=="engine.entity.sprite.spriteAsset"||property=="engine.entity.sprite.clip"||property=="engine.entity.sprite.sortingLayer")
        valid=value.is_string()&&!value.get_ref<const std::string&>().empty();
    else if(property=="engine.entity.sprite.playbackSpeed")valid=finite_number()&&value.get<double>()>=0.0;
    else if(property=="engine.entity.sprite.playing"||property=="engine.entity.sprite.flipX"||property=="engine.entity.sprite.flipY"||
            property=="engine.entity.sprite.visible")valid=value.is_boolean();
    else if(property=="engine.entity.sprite.sortingOrder")valid=value.is_number_integer()&&
        value.get<std::int64_t>()>=std::numeric_limits<std::int32_t>::min()&&value.get<std::int64_t>()<=std::numeric_limits<std::int32_t>::max();
    else if(property=="engine.entity.tilemap.tilemapAsset")valid=value.is_string()&&!value.get_ref<const std::string&>().empty();
    else if(property=="engine.entity.tilemap.visible"||property=="engine.entity.tilemap.collisionEnabled")valid=value.is_boolean();
    else if (property == "engine.entity.managedScript.assemblyAsset" || property == "engine.entity.managedScript.typeName")
        valid=value.is_string()&&!value.get_ref<const std::string&>().empty();
    else if (property == "engine.entity.managedScript.enabled") valid=value.is_boolean();
    else if (property == "engine.entity.managedScript.properties") valid=value.is_object();
    if(valid&&property=="engine.entity.animation.nextClipAsset"&&!value.get_ref<const std::string&>().empty()) {
        const auto duration=property_value_json(entity_id,"engine.entity.animation.transitionDuration");
        valid=duration&&Json::parse(*duration).get<double>()>0.0;
    }
    if(valid&&property=="engine.entity.animation.transitionDuration"&&value.get<double>()<=0.0) {
        const auto next_clip=property_value_json(entity_id,"engine.entity.animation.nextClipAsset");
        valid=next_clip&&Json::parse(*next_clip).get<std::string>().empty();
    }
    if(valid&&property=="engine.entity.localLight.innerConeDegrees") {
        const auto outer=property_value_json(entity_id,"engine.entity.localLight.outerConeDegrees");
        valid=outer&&value.get<double>()<=Json::parse(*outer).get<double>();
    }
    if(valid&&property=="engine.entity.localLight.outerConeDegrees") {
        const auto inner=property_value_json(entity_id,"engine.entity.localLight.innerConeDegrees");
        valid=inner&&value.get<double>()>=Json::parse(*inner).get<double>();
    }
    if (!valid) { plan.code="world.invalid-property-value"; plan.detail="Property value violates its Schema type or range."; return plan; }
    plan.before_value_json = *before;
    plan.after_value_json = value.dump();
    plan.content_hash = property_plan_hash(manager, entity_id, property, base_revision, plan.before_value_json, plan.after_value_json);
    plan.plan_id = "property-plan-" + plan.content_hash.substr(plan.content_hash.find(':') + 1);
    plan.valid=true; plan.code="ok"; plan.detail="Property plan validated without changing World state.";
    return plan;
}

ActionReceipt World::apply_transform_plan(const TransformChangePlan& plan, const bool dry_run) {
    ActionReceipt receipt{
        .dry_run = dry_run,
        .code = plan.code,
        .detail = plan.detail,
        .operation_id = next_world_operation_id(),
        .plan_id = plan.plan_id,
        .revision_before = revision_,
        .revision_after = revision_
    };
    if (!plan.valid) return receipt;
    const auto expected_hash = plan_hash(plan.manager, plan.entity_id, plan.base_revision, plan.before, plan.after);
    if (plan.content_hash != expected_hash) {
        receipt.code = "world.plan-integrity-error";
        receipt.detail = "Plan content hash does not match its fields.";
        return receipt;
    }
    if (plan.base_revision != revision_) {
        receipt.code = "world.revision-conflict";
        receipt.detail = "Plan is stale and must be regenerated.";
        return receipt;
    }
    const auto found = entity_ids_.find(plan.entity_id);
    if (found == entity_ids_.end()) {
        receipt.code = "world.entity-not-found";
        receipt.detail = "The planned entity no longer exists.";
        return receipt;
    }
    auto entity = world_.entity(found->second);
    const auto* current = entity.try_get<Transform>();
    if (current == nullptr || !same_transform(*current, plan.before)) {
        receipt.code = "world.precondition-failed";
        receipt.detail = "The Transform no longer matches the observed value.";
        return receipt;
    }
    SemanticDelta delta{
        .revision_before = revision_,
        .revision_after = dry_run ? revision_ : revision_ + 1,
        .entity_id = plan.entity_id,
        .field = "engine.entity.transform.position",
        .before = plan.before,
        .after = plan.after,
        .manager = plan.manager,
        .undoable = true
    };
    receipt.success = true;
    receipt.code = dry_run ? "world.plan-validated" : "ok";
    receipt.detail = dry_run ? "Dry run passed; World state was not changed." : "Change plan applied.";
    receipt.delta = delta;
    if (dry_run) return receipt;

    auto next_transform = engine_transform(plan.after);
    next_transform.scale_x = current->scale_x;
    next_transform.scale_y = current->scale_y;
    next_transform.scale_z = current->scale_z;
    next_transform.rotation_x=current->rotation_x;next_transform.rotation_y=current->rotation_y;
    next_transform.rotation_z=current->rotation_z;next_transform.rotation_w=current->rotation_w;
    entity.set<Transform>(next_transform);
    ++revision_;
    receipt.revision_after = revision_;
    recent_deltas_.push_back(delta);
    undo_stack_.push_back({.delta = delta});
    redo_stack_.clear();
    for (auto& document_entity : scene_document_.entities) {
        if (document_entity.guid == plan.entity_id) {
            if (!document_entity.transform) document_entity.transform = SceneTransform{};
            document_entity.transform->position = {plan.after.x, plan.after.y, plan.after.z};
            break;
        }
    }
    if (recent_deltas_.size() > 256) recent_deltas_.erase(recent_deltas_.begin());
    return receipt;
}

bool World::set_property_json(const std::string_view entity_id, const std::string_view property,
                              const std::string_view value_json, std::string& error) {
    const auto found = entity_ids_.find(std::string(entity_id));
    if (found == entity_ids_.end()) { error="Entity no longer exists."; return false; }
    const Json value = Json::parse(value_json);
    auto entity = world_.entity(found->second);
    const auto vector = [&] { return Transform{static_cast<float>(value.at("x").get<double>()),
        static_cast<float>(value.at("y").get<double>()), static_cast<float>(value.at("z").get<double>())}; };
    bool applied = false;
    if (property == "engine.entity.transform.position") {
        auto next = vector();
        if (const auto* current = entity.try_get<Transform>()) {
            next.scale_x=current->scale_x; next.scale_y=current->scale_y; next.scale_z=current->scale_z;
            next.rotation_x=current->rotation_x;next.rotation_y=current->rotation_y;next.rotation_z=current->rotation_z;next.rotation_w=current->rotation_w;
        }
        entity.set<Transform>(next); applied=true;
    }
    else if (property == "engine.entity.transform.scale") {
        const auto v=vector(); auto next=entity.get<Transform>();
        next.scale_x=v.x; next.scale_y=v.y; next.scale_z=v.z; entity.set<Transform>(next); applied=true;
    }
    else if (property == "engine.entity.transform.rotationEulerDegrees") {
        const auto v=vector();auto next=entity.get<Transform>();
        const auto rotation=quaternion_from_euler_degrees({v.x,v.y,v.z});
        next.rotation_x=rotation.x;next.rotation_y=rotation.y;next.rotation_z=rotation.z;next.rotation_w=rotation.w;
        entity.set<Transform>(next);applied=true;
    }
    else if(property=="engine.entity.velocity.linear") {
        const auto v=vector();entity.set<Velocity>({v.x,v.y,v.z});applied=true;
    }
    else if (auto* material = entity.try_get_mut<PbrMaterial>()) {
        if (property == "engine.entity.material.baseColor") { const auto v=vector(); material->base_r=v.x; material->base_g=v.y; material->base_b=v.z; applied=true; }
        else if (property == "engine.entity.material.metallic") { material->metallic=value.get<float>(); applied=true; }
        else if (property == "engine.entity.material.roughness") { material->roughness=value.get<float>(); applied=true; }
        else if (property == "engine.entity.material.emissiveColor") { const auto v=vector(); material->emissive_r=v.x; material->emissive_g=v.y; material->emissive_b=v.z; applied=true; }
        else if (property == "engine.entity.material.emissiveIntensity") { material->emissive_intensity=value.get<float>(); applied=true; }
        if (applied) entity.modified<PbrMaterial>();
    }
    if(!applied)if(auto* light=entity.try_get_mut<LocalLight>()) {
        if(property=="engine.entity.localLight.kind"){light->kind=value.get<std::string>();applied=true;}
        else if(property=="engine.entity.localLight.color"){const auto v=vector();light->color_r=v.x;light->color_g=v.y;light->color_b=v.z;applied=true;}
        else if(property=="engine.entity.localLight.luminousPowerLumens"){light->luminous_power_lumens=value.get<float>();applied=true;}
        else if(property=="engine.entity.localLight.rangeMeters"){light->range_meters=value.get<float>();applied=true;}
        else if(property=="engine.entity.localLight.direction"){const auto v=vector();light->direction_x=v.x;light->direction_y=v.y;light->direction_z=v.z;applied=true;}
        else if(property=="engine.entity.localLight.innerConeDegrees"){light->inner_cone_degrees=value.get<float>();applied=true;}
        else if(property=="engine.entity.localLight.outerConeDegrees"){light->outer_cone_degrees=value.get<float>();applied=true;}
        else if(property=="engine.entity.localLight.sourceRadiusMeters"){light->source_radius_meters=value.get<float>();applied=true;}
        if(applied)entity.modified<LocalLight>();
    }
    if (!applied) if (auto* collider = entity.try_get_mut<BoxCollider>()) {
        if (property == "engine.entity.collider.halfExtents") { const auto v=vector(); collider->half_x=v.x; collider->half_y=v.y; collider->half_z=v.z; applied=true; }
        else if (property == "engine.entity.collider.friction") { collider->friction=value.get<float>(); applied=true; }
        else if (property == "engine.entity.collider.restitution") { collider->restitution=value.get<float>(); applied=true; }
        else if (property == "engine.entity.collider.isTrigger") { collider->is_trigger=value.get<bool>(); applied=true; }
        if (applied) entity.modified<BoxCollider>();
    }
    if (!applied) if (auto* collider = entity.try_get_mut<SphereCollider>()) {
        if (property == "engine.entity.collider.radius") { collider->radius=value.get<float>(); applied=true; }
        else if (property == "engine.entity.collider.friction") { collider->friction=value.get<float>(); applied=true; }
        else if (property == "engine.entity.collider.restitution") { collider->restitution=value.get<float>(); applied=true; }
        else if (property == "engine.entity.collider.isTrigger") { collider->is_trigger=value.get<bool>(); applied=true; }
        if (applied) entity.modified<SphereCollider>();
    }
    if (!applied) if (auto* collider = entity.try_get_mut<CapsuleCollider>()) {
        if (property == "engine.entity.collider.radius") { collider->radius=value.get<float>(); applied=true; }
        else if (property == "engine.entity.collider.halfHeight") { collider->half_height=value.get<float>(); applied=true; }
        else if (property == "engine.entity.collider.friction") { collider->friction=value.get<float>(); applied=true; }
        else if (property == "engine.entity.collider.restitution") { collider->restitution=value.get<float>(); applied=true; }
        else if (property == "engine.entity.collider.isTrigger") { collider->is_trigger=value.get<bool>(); applied=true; }
        if (applied) entity.modified<CapsuleCollider>();
    }
    if(!applied) if(auto* collider=entity.try_get_mut<ConvexHullCollider>()) {
        if(property=="engine.entity.collider.friction") {collider->friction=value.get<float>(); applied=true;}
        else if(property=="engine.entity.collider.restitution") {collider->restitution=value.get<float>(); applied=true;}
        else if(property=="engine.entity.collider.isTrigger") {collider->is_trigger=value.get<bool>(); applied=true;}
        if(applied) entity.modified<ConvexHullCollider>();
    }
    if (!applied) if (auto* player = entity.try_get_mut<AnimationPlayer>()) {
        if (property == "engine.entity.animation.clipAsset") { player->clip_asset=value.get<std::string>(); player->time_seconds=0.0F; applied=true; }
        else if (property == "engine.entity.animation.playbackSpeed") { player->playback_speed=value.get<float>(); applied=true; }
        else if (property == "engine.entity.animation.looping") { player->looping=value.get<bool>(); applied=true; }
        else if (property == "engine.entity.animation.playing") { player->playing=value.get<bool>(); applied=true; }
        else if (property == "engine.entity.animation.nextClipAsset") { player->next_clip_asset=value.get<std::string>(); player->next_time_seconds=0.0F; player->transition_elapsed_seconds=0.0F; applied=true; }
        else if (property == "engine.entity.animation.transitionDuration") { player->transition_duration_seconds=value.get<float>(); applied=true; }
        else if (property == "engine.entity.animation.rootMotionMode") { player->root_motion_mode=value.get<std::string>(); applied=true; }
        else if (property == "engine.entity.animation.stateMachineAsset") {
            player->state_machine_asset=value.get<std::string>();player->active_state.clear();player->previous_state.clear();
            player->state_parameters.clear();configure_animation_player(*player);applied=true;
        }
        else if (property == "engine.entity.animation.animationGraphAsset") {
            player->animation_graph_asset=value.get<std::string>();player->graph_parameters.clear();
            player->graph_node_times.clear();player->graph_sync_phases.clear();
            configure_animation_graph_player(*player);applied=true;
        }
        if (applied) entity.modified<AnimationPlayer>();
    }
    if(!applied) if(auto* renderer=entity.try_get_mut<SpriteRenderer>()) {
        if(property=="engine.entity.sprite.spriteAsset") {renderer->playback.asset_id=value.get<std::string>();renderer->playback.frame_index=0;renderer->playback.elapsed_in_frame_ms=0.0;applied=true;}
        else if(property=="engine.entity.sprite.clip") {renderer->playback.clip_id=value.get<std::string>();renderer->playback.frame_index=0;renderer->playback.elapsed_in_frame_ms=0.0;applied=true;}
        else if(property=="engine.entity.sprite.playbackSpeed") {renderer->playback_speed=value.get<float>();applied=true;}
        else if(property=="engine.entity.sprite.playing") {renderer->playback.playing=value.get<bool>();applied=true;}
        else if(property=="engine.entity.sprite.flipX") {renderer->flip_x=value.get<bool>();applied=true;}
        else if(property=="engine.entity.sprite.flipY") {renderer->flip_y=value.get<bool>();applied=true;}
        else if(property=="engine.entity.sprite.sortingLayer") {renderer->sorting_layer=value.get<std::string>();applied=true;}
        else if(property=="engine.entity.sprite.sortingOrder") {renderer->sorting_order=value.get<std::int32_t>();applied=true;}
        else if(property=="engine.entity.sprite.visible") {renderer->visible=value.get<bool>();applied=true;}
        if(applied)entity.modified<SpriteRenderer>();
    }
    if(!applied) if(auto* renderer=entity.try_get_mut<TilemapRenderer>()) {
        if(property=="engine.entity.tilemap.tilemapAsset"){renderer->tilemap_asset=value.get<std::string>();applied=true;}
        else if(property=="engine.entity.tilemap.visible"){renderer->visible=value.get<bool>();applied=true;}
        else if(property=="engine.entity.tilemap.collisionEnabled"){renderer->collision_enabled=value.get<bool>();applied=true;}
        if(applied)entity.modified<TilemapRenderer>();
    }
    if(!applied) {
        const auto document_entity=std::ranges::find(scene_document_.entities,entity_id,&SceneEntityDocument::guid);
        if(document_entity!=scene_document_.entities.end()&&document_entity->managed_script) {
            auto& script=*document_entity->managed_script;
            if(property=="engine.entity.managedScript.assemblyAsset") {script.assembly_asset=value.get<std::string>();applied=true;}
            else if(property=="engine.entity.managedScript.typeName") {script.type_name=value.get<std::string>();applied=true;}
            else if(property=="engine.entity.managedScript.enabled") {script.enabled=value.get<bool>();applied=true;}
            else if(property=="engine.entity.managedScript.properties") {script.properties_json=value.dump();applied=true;}
            if(applied)synchronize_managed_scripts();
        }
    }
    if (!applied) { error="Property or component is no longer available."; return false; }

    for (auto& document_entity : scene_document_.entities) {
        if (document_entity.guid != entity_id) continue;
        if (property == "engine.entity.transform.position") {
            const auto v=vector(); if(!document_entity.transform) document_entity.transform=SceneTransform{};
            document_entity.transform->position={v.x,v.y,v.z};
        }
        else if (property == "engine.entity.transform.scale") {
            const auto v=vector(); if(!document_entity.transform) document_entity.transform=SceneTransform{};
            document_entity.transform->scale={v.x,v.y,v.z};
        }
        else if (property == "engine.entity.transform.rotationEulerDegrees") {
            const auto v=vector();if(!document_entity.transform) document_entity.transform=SceneTransform{};
            document_entity.transform->rotation_euler_degrees={v.x,v.y,v.z};
        }
        else if(property=="engine.entity.velocity.linear") {
            const auto v=vector();if(!document_entity.velocity)document_entity.velocity=SceneVelocity{};
            document_entity.velocity->linear={v.x,v.y,v.z};
        }
        else if (document_entity.pbr_material) {
            if (property == "engine.entity.material.baseColor") { const auto v=vector(); document_entity.pbr_material->base_color={v.x,v.y,v.z}; }
            else if (property == "engine.entity.material.metallic") document_entity.pbr_material->metallic=value.get<double>();
            else if (property == "engine.entity.material.roughness") document_entity.pbr_material->roughness=value.get<double>();
            else if (property == "engine.entity.material.emissiveColor") { const auto v=vector(); document_entity.pbr_material->emissive_color={v.x,v.y,v.z}; }
            else if (property == "engine.entity.material.emissiveIntensity") document_entity.pbr_material->emissive_intensity=value.get<double>();
        }
        if(document_entity.local_light) {
            auto& light=*document_entity.local_light;
            if(property=="engine.entity.localLight.kind")light.kind=value.get<std::string>();
            else if(property=="engine.entity.localLight.color"){const auto v=vector();light.color={v.x,v.y,v.z};}
            else if(property=="engine.entity.localLight.luminousPowerLumens")light.luminous_power_lumens=value.get<double>();
            else if(property=="engine.entity.localLight.rangeMeters")light.range_meters=value.get<double>();
            else if(property=="engine.entity.localLight.direction"){const auto v=vector();light.direction={v.x,v.y,v.z};}
            else if(property=="engine.entity.localLight.innerConeDegrees")light.inner_cone_degrees=value.get<double>();
            else if(property=="engine.entity.localLight.outerConeDegrees")light.outer_cone_degrees=value.get<double>();
            else if(property=="engine.entity.localLight.sourceRadiusMeters")light.source_radius_meters=value.get<double>();
        }
        if (document_entity.box_collider) {
            if (property == "engine.entity.collider.halfExtents") { const auto v=vector(); document_entity.box_collider->half_extents={v.x,v.y,v.z}; }
            else if (property == "engine.entity.collider.friction") document_entity.box_collider->friction=value.get<double>();
            else if (property == "engine.entity.collider.restitution") document_entity.box_collider->restitution=value.get<double>();
            else if (property == "engine.entity.collider.isTrigger") document_entity.box_collider->is_trigger=value.get<bool>();
        }
        if (document_entity.sphere_collider) {
            if (property == "engine.entity.collider.radius") document_entity.sphere_collider->radius=value.get<double>();
            else if (property == "engine.entity.collider.friction") document_entity.sphere_collider->friction=value.get<double>();
            else if (property == "engine.entity.collider.restitution") document_entity.sphere_collider->restitution=value.get<double>();
            else if (property == "engine.entity.collider.isTrigger") document_entity.sphere_collider->is_trigger=value.get<bool>();
        }
        if (document_entity.capsule_collider) {
            if (property == "engine.entity.collider.radius") document_entity.capsule_collider->radius=value.get<double>();
            else if (property == "engine.entity.collider.halfHeight") document_entity.capsule_collider->half_height=value.get<double>();
            else if (property == "engine.entity.collider.friction") document_entity.capsule_collider->friction=value.get<double>();
            else if (property == "engine.entity.collider.restitution") document_entity.capsule_collider->restitution=value.get<double>();
            else if (property == "engine.entity.collider.isTrigger") document_entity.capsule_collider->is_trigger=value.get<bool>();
        }
        if(document_entity.convex_hull_collider) {
            if(property=="engine.entity.collider.friction") document_entity.convex_hull_collider->friction=value.get<double>();
            else if(property=="engine.entity.collider.restitution") document_entity.convex_hull_collider->restitution=value.get<double>();
            else if(property=="engine.entity.collider.isTrigger") document_entity.convex_hull_collider->is_trigger=value.get<bool>();
        }
        if (document_entity.animation_player) {
            if (property == "engine.entity.animation.clipAsset") document_entity.animation_player->clip_asset=value.get<std::string>();
            else if (property == "engine.entity.animation.playbackSpeed") document_entity.animation_player->playback_speed=value.get<double>();
            else if (property == "engine.entity.animation.looping") document_entity.animation_player->looping=value.get<bool>();
            else if (property == "engine.entity.animation.playing") document_entity.animation_player->playing=value.get<bool>();
            else if (property == "engine.entity.animation.nextClipAsset") document_entity.animation_player->next_clip_asset=value.get<std::string>();
            else if (property == "engine.entity.animation.transitionDuration") document_entity.animation_player->transition_duration_seconds=value.get<double>();
            else if (property == "engine.entity.animation.rootMotionMode") document_entity.animation_player->root_motion_mode=value.get<std::string>();
            else if (property == "engine.entity.animation.stateMachineAsset") document_entity.animation_player->state_machine_asset=value.get<std::string>();
            else if (property == "engine.entity.animation.animationGraphAsset") document_entity.animation_player->animation_graph_asset=value.get<std::string>();
        }
        if(document_entity.sprite_renderer) {
            auto& renderer=*document_entity.sprite_renderer;
            if(property=="engine.entity.sprite.spriteAsset")renderer.sprite_asset=value.get<std::string>();
            else if(property=="engine.entity.sprite.clip")renderer.clip=value.get<std::string>();
            else if(property=="engine.entity.sprite.playbackSpeed")renderer.playback_speed=value.get<double>();
            else if(property=="engine.entity.sprite.playing")renderer.playing=value.get<bool>();
            else if(property=="engine.entity.sprite.flipX")renderer.flip_x=value.get<bool>();
            else if(property=="engine.entity.sprite.flipY")renderer.flip_y=value.get<bool>();
            else if(property=="engine.entity.sprite.sortingLayer")renderer.sorting_layer=value.get<std::string>();
            else if(property=="engine.entity.sprite.sortingOrder")renderer.sorting_order=value.get<std::int32_t>();
            else if(property=="engine.entity.sprite.visible")renderer.visible=value.get<bool>();
        }
        if(document_entity.tilemap_renderer) {
            auto& renderer=*document_entity.tilemap_renderer;
            if(property=="engine.entity.tilemap.tilemapAsset")renderer.tilemap_asset=value.get<std::string>();
            else if(property=="engine.entity.tilemap.visible")renderer.visible=value.get<bool>();
            else if(property=="engine.entity.tilemap.collisionEnabled")renderer.collision_enabled=value.get<bool>();
        }
        break;
    }
    return true;
}

ActionReceipt World::apply_property_plan(const PropertyChangePlan& plan, const bool dry_run) {
    ActionReceipt receipt{.dry_run=dry_run, .code=plan.code, .detail=plan.detail, .operation_id=next_world_operation_id(),
        .plan_id=plan.plan_id, .revision_before=revision_, .revision_after=revision_};
    if (!plan.valid) return receipt;
    const auto expected_hash=property_plan_hash(plan.manager,plan.entity_id,plan.property,plan.base_revision,
        plan.before_value_json,plan.after_value_json);
    if (plan.content_hash!=expected_hash) { receipt.code="world.plan-integrity-error"; receipt.detail="Plan content hash does not match its fields."; return receipt; }
    if (plan.base_revision!=revision_) { receipt.code="world.revision-conflict"; receipt.detail="Plan is stale and must be regenerated."; return receipt; }
    const auto current=property_value_json(plan.entity_id,plan.property);
    if (!current || *current!=plan.before_value_json) { receipt.code="world.precondition-failed"; receipt.detail="The property no longer matches the observed value."; return receipt; }
    SemanticDelta delta{.revision_before=revision_, .revision_after=dry_run?revision_:revision_+1,
        .entity_id=plan.entity_id,.field=plan.property,.before_value_json=plan.before_value_json,
        .after_value_json=plan.after_value_json,.manager=plan.manager,.undoable=true};
    receipt.success=true; receipt.code=dry_run?"world.plan-validated":"ok";
    receipt.detail=dry_run?"Dry run passed; World state was not changed.":"Property change plan applied."; receipt.delta=delta;
    if (dry_run) return receipt;
    std::string error;
    if (!set_property_json(plan.entity_id,plan.property,plan.after_value_json,error)) {
        receipt.success=false; receipt.code="world.property-apply-failed"; receipt.detail=std::move(error); receipt.delta.reset(); return receipt;
    }
    ++revision_; receipt.revision_after=revision_; recent_deltas_.push_back(delta); undo_stack_.push_back({.delta=delta}); redo_stack_.clear();
    if (recent_deltas_.size()>256) recent_deltas_.erase(recent_deltas_.begin());
    return receipt;
}

TransformUpdateResult World::update_transform(
    const std::string_view entity_id,
    const Transform transform,
    const std::uint64_t expected_revision) {
    const auto plan = plan_transform_update(entity_id, transform, expected_revision, "editor.inspector");
    const auto receipt = apply_transform_plan(plan, false);
    return {
        .success = receipt.success,
        .code = receipt.code,
        .detail = receipt.detail,
        .revision = receipt.revision_after
    };
}

ActionReceipt World::undo(const std::uint64_t expected_revision, const std::string_view manager) {
    ActionReceipt receipt{
        .operation_id = next_world_operation_id(),
        .revision_before = revision_,
        .revision_after = revision_
    };
    if (expected_revision != revision_) {
        receipt.code = "world.revision-conflict";
        receipt.detail = "Undo expected revision does not match the current World revision.";
        return receipt;
    }
    if (undo_stack_.empty()) {
        receipt.code = "world.undo-empty";
        receipt.detail = "There is no committed change to undo.";
        return receipt;
    }
    const auto original_entry = undo_stack_.back();
    if (original_entry.scene_before && original_entry.scene_after) {
        if (canonical_scene_json() != SceneDocumentCodec::write_canonical_json(*original_entry.scene_after)) {
            receipt.code = "world.undo-conflict";
            receipt.detail = "Current scene document diverged from the committed structural edit.";
            return receipt;
        }
        const auto loaded = load_scene_internal(*original_entry.scene_before, false);
        if (!loaded.success) {
            receipt.code = "world.undo-failed";
            receipt.detail = "The previous scene document no longer validates.";
            return receipt;
        }
        auto delta = original_entry.delta;
        delta.revision_before = receipt.revision_before;
        delta.revision_after = revision_;
        std::swap(delta.before_value_json, delta.after_value_json);
        delta.manager = std::string(manager);
        delta.undoable = false;
        recent_deltas_.push_back(delta);
        undo_stack_.pop_back();
        redo_stack_.push_back(original_entry);
        if (recent_deltas_.size() > 256) recent_deltas_.erase(recent_deltas_.begin());
        receipt.success = true;
        receipt.code = "ok";
        receipt.detail = "Last committed scene structure change was undone.";
        receipt.revision_after = revision_;
        receipt.delta = delta;
        return receipt;
    }
    const auto original = original_entry.delta;
    const auto found = entity_ids_.find(original.entity_id);
    if (found == entity_ids_.end()) {
        receipt.code = "world.entity-not-found";
        receipt.detail = "The changed entity no longer exists.";
        return receipt;
    }
    auto entity = world_.entity(found->second);
    if (!original.after_value_json.empty()) {
        const auto current=property_value_json(original.entity_id,original.field);
        if (!current || *current!=original.after_value_json) { receipt.code="world.undo-conflict"; receipt.detail="Current property diverged from the committed change."; return receipt; }
    } else {
        const auto* current = entity.try_get<Transform>();
        if (current == nullptr || !same_transform(*current, original.after)) { receipt.code="world.undo-conflict"; receipt.detail="Current Transform diverged from the committed change."; return receipt; }
    }
    const auto revision_before = revision_;
    if (!original.before_value_json.empty()) {
        std::string error;
        if (!set_property_json(original.entity_id,original.field,original.before_value_json,error)) { receipt.code="world.undo-failed"; receipt.detail=std::move(error); return receipt; }
    } else {
        auto next=engine_transform(original.before); const auto* current=entity.try_get<Transform>();
        if(current) { next.scale_x=current->scale_x; next.scale_y=current->scale_y; next.scale_z=current->scale_z;
            next.rotation_x=current->rotation_x;next.rotation_y=current->rotation_y;next.rotation_z=current->rotation_z;next.rotation_w=current->rotation_w; }
        entity.set<Transform>(next);
    }
    ++revision_;
    SemanticDelta delta{
        .revision_before = revision_before,
        .revision_after = revision_,
        .entity_id = original.entity_id,
        .field = original.field,
        .before = original.after,
        .after = original.before,
        .before_value_json = original.after_value_json,
        .after_value_json = original.before_value_json,
        .manager = std::string(manager),
        .undoable = false
    };
    recent_deltas_.push_back(delta);
    undo_stack_.pop_back();
    redo_stack_.push_back(original_entry);
    if (original.before_value_json.empty()) for (auto& document_entity : scene_document_.entities) {
        if (document_entity.guid == original.entity_id) {
            if(!document_entity.transform) document_entity.transform=SceneTransform{};
            document_entity.transform->position={original.before.x,original.before.y,original.before.z};
            break;
        }
    }
    if (recent_deltas_.size() > 256) recent_deltas_.erase(recent_deltas_.begin());
    receipt.success = true;
    receipt.code = "ok";
    receipt.detail = original.before_value_json.empty() ? "Last committed Transform change was undone." : "Last committed property change was undone.";
    receipt.revision_after = revision_;
    receipt.delta = delta;
    return receipt;
}

ActionReceipt World::redo(const std::uint64_t expected_revision, const std::string_view manager) {
    ActionReceipt receipt{
        .operation_id = next_world_operation_id(),
        .revision_before = revision_,
        .revision_after = revision_
    };
    if (expected_revision != revision_) {
        receipt.code = "world.revision-conflict";
        receipt.detail = "Redo expected revision does not match the current World revision.";
        return receipt;
    }
    if (redo_stack_.empty()) {
        receipt.code = "world.redo-empty";
        receipt.detail = "There is no reverted change to redo.";
        return receipt;
    }
    const auto original_entry = redo_stack_.back();
    if (original_entry.scene_before && original_entry.scene_after) {
        if (canonical_scene_json() != SceneDocumentCodec::write_canonical_json(*original_entry.scene_before)) {
            receipt.code = "world.redo-conflict";
            receipt.detail = "Current scene document diverged from the reverted structural edit.";
            return receipt;
        }
        const auto loaded = load_scene_internal(*original_entry.scene_after, false);
        if (!loaded.success) {
            receipt.code = "world.redo-failed";
            receipt.detail = "The edited scene document no longer validates.";
            return receipt;
        }
        auto delta = original_entry.delta;
        delta.revision_before = receipt.revision_before;
        delta.revision_after = revision_;
        delta.manager = std::string(manager);
        delta.undoable = true;
        recent_deltas_.push_back(delta);
        redo_stack_.pop_back();
        undo_stack_.push_back(original_entry);
        if (recent_deltas_.size() > 256) recent_deltas_.erase(recent_deltas_.begin());
        receipt.success = true;
        receipt.code = "ok";
        receipt.detail = "Last reverted scene structure change was reapplied.";
        receipt.revision_after = revision_;
        receipt.delta = delta;
        return receipt;
    }
    const auto original = original_entry.delta;
    const auto found = entity_ids_.find(original.entity_id);
    if (found == entity_ids_.end()) {
        receipt.code = "world.entity-not-found";
        receipt.detail = "The changed entity no longer exists.";
        return receipt;
    }
    auto entity = world_.entity(found->second);
    if (!original.before_value_json.empty()) {
        const auto current=property_value_json(original.entity_id,original.field);
        if (!current || *current!=original.before_value_json) { receipt.code="world.redo-conflict"; receipt.detail="Current property diverged from the reverted value."; return receipt; }
    } else {
        const auto* current = entity.try_get<Transform>();
        if (current == nullptr || !same_transform(*current, original.before)) { receipt.code="world.redo-conflict"; receipt.detail="Current Transform diverged from the reverted value."; return receipt; }
    }
    const auto revision_before = revision_;
    if (!original.after_value_json.empty()) {
        std::string error;
        if (!set_property_json(original.entity_id,original.field,original.after_value_json,error)) { receipt.code="world.redo-failed"; receipt.detail=std::move(error); return receipt; }
    } else {
        auto next=engine_transform(original.after); const auto* current=entity.try_get<Transform>();
        if(current) { next.scale_x=current->scale_x; next.scale_y=current->scale_y; next.scale_z=current->scale_z;
            next.rotation_x=current->rotation_x;next.rotation_y=current->rotation_y;next.rotation_z=current->rotation_z;next.rotation_w=current->rotation_w; }
        entity.set<Transform>(next);
    }
    ++revision_;
    SemanticDelta delta{
        .revision_before = revision_before,
        .revision_after = revision_,
        .entity_id = original.entity_id,
        .field = original.field,
        .before = original.before,
        .after = original.after,
        .before_value_json = original.before_value_json,
        .after_value_json = original.after_value_json,
        .manager = std::string(manager),
        .undoable = true
    };
    recent_deltas_.push_back(delta);
    redo_stack_.pop_back();
    undo_stack_.push_back(original_entry);
    if (original.after_value_json.empty()) for (auto& document_entity : scene_document_.entities) {
        if (document_entity.guid == original.entity_id) {
            if(!document_entity.transform) document_entity.transform=SceneTransform{};
            document_entity.transform->position={original.after.x,original.after.y,original.after.z};
            break;
        }
    }
    if (recent_deltas_.size() > 256) recent_deltas_.erase(recent_deltas_.begin());
    receipt.success = true;
    receipt.code = "ok";
    receipt.detail = original.after_value_json.empty() ? "Last reverted Transform change was reapplied." : "Last reverted property change was reapplied.";
    receipt.revision_after = revision_;
    receipt.delta = delta;
    return receipt;
}

bool World::can_undo() const noexcept { return !undo_stack_.empty(); }
bool World::can_redo() const noexcept { return !redo_stack_.empty(); }

std::uint64_t World::revision() const noexcept {
    return revision_;
}

std::string World::scene_source_uri() const {
    return scene_source_uri_;
}

std::string World::edit_scene_entity_json(const std::string_view operation, const std::string_view entity_id,
                                          const std::string_view new_entity_id, const std::string_view display_name,
                                          const std::string_view parent_entity_id, const std::string_view component,
                                          const bool recursive,
                                          const std::uint64_t base_revision, const std::string_view manager,
                                          const bool dry_run) {
    const auto failure = [&](const std::string_view code, const std::string_view detail) {
        return Json{{"schemaVersion","noemancer.scene-edit-receipt/0.1"},{"success",false},{"dryRun",dry_run},
            {"code",code},{"detail",detail},{"operation",operation},{"entityId",entity_id},
            {"newEntityId",new_entity_id},{"revisionBefore",revision_},{"revisionAfter",revision_}}.dump();
    };
    if (base_revision != revision_) return failure("world.revision-conflict", "Scene edit base revision is stale.");
    if (manager.empty()) return failure("scene.invalid-manager", "Scene edits require a non-empty manager identity.");

    auto candidate = scene_document_;
    const auto find_entity = [&](const std::string_view id) {
        return std::ranges::find(candidate.entities, id, &SceneEntityDocument::guid);
    };
    std::string affected_id;
    if (operation == "create") {
        if (new_entity_id.empty() || display_name.empty())
            return failure("scene.invalid-entity", "Create requires non-empty newEntityId and displayName.");
        if (find_entity(new_entity_id) != candidate.entities.end())
            return failure("scene.entity-id-conflict", "The requested stable entity ID already exists.");
        if (!parent_entity_id.empty() && find_entity(parent_entity_id) == candidate.entities.end())
            return failure("scene.parent-not-found", "The requested parent entity does not exist.");
        candidate.entities.push_back(SceneEntityDocument{.guid=std::string(new_entity_id),
            .name=std::string(display_name),.parent_guid=std::string(parent_entity_id),.transform=SceneTransform{}});
        affected_id = std::string(new_entity_id);
    } else if (operation == "duplicate") {
        const auto source = find_entity(entity_id);
        if (source == candidate.entities.end()) return failure("world.entity-not-found", "Duplicate source entity does not exist.");
        if (new_entity_id.empty() || find_entity(new_entity_id) != candidate.entities.end())
            return failure("scene.entity-id-conflict", "Duplicate requires a new, unused stable entity ID.");
        auto copy = *source;
        copy.guid = std::string(new_entity_id);
        copy.name = display_name.empty() ? source->name + " Copy" : std::string(display_name);
        if(copy.managed_script) copy.managed_script->instance_id="script."+copy.guid;
        if (!parent_entity_id.empty()) copy.parent_guid = std::string(parent_entity_id);
        candidate.entities.push_back(std::move(copy));
        affected_id = std::string(new_entity_id);
    } else if (operation == "rename") {
        auto entity = find_entity(entity_id);
        if (entity == candidate.entities.end()) return failure("world.entity-not-found", "Rename target entity does not exist.");
        if (display_name.empty()) return failure("scene.invalid-display-name", "Rename requires a non-empty displayName.");
        entity->name = std::string(display_name);
        affected_id = std::string(entity_id);
    } else if (operation == "reparent") {
        auto entity = find_entity(entity_id);
        if (entity == candidate.entities.end()) return failure("world.entity-not-found", "Reparent target entity does not exist.");
        if (entity_id == parent_entity_id) return failure("scene.parent-cycle", "An entity cannot be its own parent.");
        if (!parent_entity_id.empty() && find_entity(parent_entity_id) == candidate.entities.end())
            return failure("scene.parent-not-found", "The requested parent entity does not exist.");
        entity->parent_guid = std::string(parent_entity_id);
        affected_id = std::string(entity_id);
    } else if (operation == "delete") {
        if (find_entity(entity_id) == candidate.entities.end()) return failure("world.entity-not-found", "Delete target entity does not exist.");
        std::unordered_set<std::string> removed{std::string(entity_id)};
        bool changed = true;
        while (recursive && changed) {
            changed = false;
            for (const auto& entity : candidate.entities) {
                if (removed.contains(entity.parent_guid) && removed.insert(entity.guid).second) changed = true;
            }
        }
        if (!recursive && std::ranges::any_of(candidate.entities, [&](const SceneEntityDocument& entity) {
                return entity.parent_guid == entity_id;
            })) return failure("scene.entity-has-children", "Delete requires recursive=true while the entity owns children.");
        std::erase_if(candidate.entities, [&](const SceneEntityDocument& entity) { return removed.contains(entity.guid); });
        affected_id = std::string(entity_id);
    } else if (operation == "add-component" || operation == "remove-component") {
        auto entity = find_entity(entity_id);
        if (entity == candidate.entities.end()) return failure("world.entity-not-found", "Component edit target entity does not exist.");
        const auto collider_exists = entity->box_collider || entity->sphere_collider || entity->capsule_collider || entity->convex_hull_collider;
        const auto exists = [&] {
            if (component=="Transform") return entity->transform.has_value();
            if (component=="Velocity") return entity->velocity.has_value();
            if (component=="RigidBody") return entity->rigid_body.has_value();
            if (component=="BoxCollider") return entity->box_collider.has_value();
            if (component=="SphereCollider") return entity->sphere_collider.has_value();
            if (component=="CapsuleCollider") return entity->capsule_collider.has_value();
            if (component=="Camera") return entity->camera.has_value();
            if (component=="DirectionalLight") return entity->directional_light.has_value();
            if (component=="LocalLight") return entity->local_light.has_value();
            if (component=="MeshRenderer") return entity->mesh_renderer.has_value();
            if (component=="SpriteRenderer") return entity->sprite_renderer.has_value();
            if (component=="TilemapRenderer") return entity->tilemap_renderer.has_value();
            if (component=="PbrMaterial") return entity->pbr_material.has_value();
            if (component=="ManagedScript") return entity->managed_script.has_value();
            return false;
        }();
        if (component.empty() || (!exists && operation=="remove-component"))
            return failure("scene.component-not-found", "The requested component is not present or unsupported by this authoring slice.");
        if (exists && operation=="add-component") return failure("scene.component-already-present", "The entity already owns this component.");
        if (operation=="add-component" && (component=="BoxCollider"||component=="SphereCollider"||component=="CapsuleCollider") && collider_exists)
            return failure("scene.collider-conflict", "Remove the existing collision shape before adding another one.");
        if (operation == "add-component") {
            if (component=="Transform") entity->transform=SceneTransform{};
            else if (component=="Velocity") entity->velocity=SceneVelocity{};
            else if (component=="RigidBody") entity->rigid_body=SceneRigidBody{};
            else if (component=="BoxCollider") entity->box_collider=SceneBoxCollider{};
            else if (component=="SphereCollider") entity->sphere_collider=SceneSphereCollider{};
            else if (component=="CapsuleCollider") entity->capsule_collider=SceneCapsuleCollider{};
            else if (component=="Camera") entity->camera=SceneCamera{};
            else if (component=="DirectionalLight") entity->directional_light=SceneDirectionalLight{};
            else if (component=="LocalLight") {if(!entity->transform)entity->transform=SceneTransform{};entity->local_light=SceneLocalLight{};}
            else if (component=="MeshRenderer") entity->mesh_renderer=SceneMeshRenderer{.mesh_asset="asset.primitive.cube"};
            else if (component=="SpriteRenderer") entity->sprite_renderer=SceneSpriteRenderer{.sprite_asset="sprite.new",.clip="idle"};
            else if (component=="TilemapRenderer") {if(!entity->transform)entity->transform=SceneTransform{};entity->tilemap_renderer=SceneTilemapRenderer{.tilemap_asset="tilemap.new"};}
            else if (component=="PbrMaterial") entity->pbr_material=ScenePbrMaterial{};
            else if (component=="ManagedScript") entity->managed_script=SceneManagedScript{
                "script."+entity->guid,"project.script","Game.NewBehaviour",true,"{}"};
            else return failure("scene.unsupported-component", "This component is not available in the first editor authoring slice.");
        } else {
            if (component=="Transform") entity->transform.reset();
            else if (component=="Velocity") entity->velocity.reset();
            else if (component=="RigidBody") entity->rigid_body.reset();
            else if (component=="BoxCollider") entity->box_collider.reset();
            else if (component=="SphereCollider") entity->sphere_collider.reset();
            else if (component=="CapsuleCollider") entity->capsule_collider.reset();
            else if (component=="Camera") entity->camera.reset();
            else if (component=="DirectionalLight") entity->directional_light.reset();
            else if (component=="LocalLight") entity->local_light.reset();
            else if (component=="MeshRenderer") entity->mesh_renderer.reset();
            else if (component=="SpriteRenderer") entity->sprite_renderer.reset();
            else if (component=="TilemapRenderer") entity->tilemap_renderer.reset();
            else if (component=="PbrMaterial") entity->pbr_material.reset();
            else if (component=="ManagedScript") entity->managed_script.reset();
            else return failure("scene.unsupported-component", "This component is not available in the first editor authoring slice.");
        }
        affected_id = std::string(entity_id);
    } else {
        return failure("scene.unsupported-edit", "Supported edits are entity create/duplicate/rename/reparent/delete and component add/remove.");
    }

    const auto validation = SceneDocumentCodec::validate(candidate);
    if (!validation.empty()) {
        Json errors=Json::array();
        for (const auto& error : validation) errors.push_back({{"code",error.code},{"path",error.path},{"message",error.message}});
        return Json{{"schemaVersion","noemancer.scene-edit-receipt/0.1"},{"success",false},{"dryRun",dry_run},
            {"code","scene.edit-validation-failed"},{"detail","The edited scene document is invalid."},{"operation",operation},
            {"entityId",affected_id},{"revisionBefore",revision_},{"revisionAfter",revision_},{"errors",std::move(errors)}}.dump();
    }

    const auto before_scene = scene_document_;
    const auto before_json = canonical_scene_json();
    const auto after_json = SceneDocumentCodec::write_canonical_json(candidate);
    SemanticDelta delta{.revision_before=revision_,.revision_after=dry_run?revision_:revision_+1,
        .entity_id=affected_id,.field=component.empty()?"engine.scene.entity.structure":"engine.scene.component."+std::string(component),.before_value_json=before_json,
        .after_value_json=after_json,.manager=std::string(manager),.undoable=true};
    if (!dry_run) {
        const auto loaded = load_scene_internal(candidate, false);
        if (!loaded.success) return failure("scene.edit-apply-failed", "The validated scene could not be instantiated.");
        delta.revision_after = revision_;
        recent_deltas_.push_back(delta);
        undo_stack_.push_back({.delta=delta,.scene_before=before_scene,.scene_after=candidate});
        redo_stack_.clear();
        if (recent_deltas_.size() > 256) recent_deltas_.erase(recent_deltas_.begin());
    }
    return Json{{"schemaVersion","noemancer.scene-edit-receipt/0.1"},{"success",true},{"dryRun",dry_run},
        {"code",dry_run?"scene.edit-validated":"ok"},{"detail",dry_run?"Scene edit dry run passed.":"Scene edit committed."},
        {"operation",operation},{"entityId",affected_id},{"revisionBefore",delta.revision_before},
        {"revisionAfter",delta.revision_after},{"undoable",true}}.dump();
}

std::string World::edit_transform_json(const std::string_view entity_id, Transform transform,
                                       const std::uint64_t base_revision, const std::string_view manager,
                                       const bool dry_run) {
    const auto failure=[&](const std::string_view code,const std::string_view detail){return Json{
        {"schemaVersion","noemancer.transform-edit-receipt/0.1"},{"success",false},{"dryRun",dry_run},{"code",code},
        {"detail",detail},{"entityId",entity_id},{"revisionBefore",revision_},{"revisionAfter",revision_}}.dump();};
    if(base_revision!=revision_) return failure("world.revision-conflict","Transform edit base revision is stale.");
    if(manager.empty()) return failure("world.manager-required","A stable manager identity is required.");
    if(!std::isfinite(transform.x)||!std::isfinite(transform.y)||!std::isfinite(transform.z)||
       !std::isfinite(transform.scale_x)||!std::isfinite(transform.scale_y)||!std::isfinite(transform.scale_z)||
       transform.scale_x<=0.0F||transform.scale_y<=0.0F||transform.scale_z<=0.0F)
        return failure("world.invalid-transform","Position must be finite and scale must be finite and positive.");
    const auto rotation_length=std::sqrt(transform.rotation_x*transform.rotation_x+transform.rotation_y*transform.rotation_y+
        transform.rotation_z*transform.rotation_z+transform.rotation_w*transform.rotation_w);
    if(!std::isfinite(rotation_length)||rotation_length<0.000001F)
        return failure("world.invalid-rotation","Rotation quaternion must be finite and non-zero.");
    const auto rotation=normalized_quaternion({transform.rotation_x,transform.rotation_y,transform.rotation_z,transform.rotation_w});
    transform.rotation_x=rotation.x;transform.rotation_y=rotation.y;transform.rotation_z=rotation.z;transform.rotation_w=rotation.w;
    auto candidate=scene_document_;
    const auto found=std::ranges::find(candidate.entities,entity_id,&SceneEntityDocument::guid);
    if(found==candidate.entities.end()) return failure("world.entity-not-found","Transform edit target entity does not exist.");
    if(!found->transform) return failure("world.component-not-found","The entity does not have a Transform component.");
    found->transform->position={transform.x,transform.y,transform.z};
    found->transform->scale={transform.scale_x,transform.scale_y,transform.scale_z};
    found->transform->rotation_euler_degrees=euler_degrees_from_quaternion(rotation);
    const auto errors=SceneDocumentCodec::validate(candidate);
    if(!errors.empty()) return failure("scene.edit-validation-failed",errors.front().message);
    const auto before_scene=scene_document_;
    const auto before_json=canonical_scene_json();
    const auto after_json=SceneDocumentCodec::write_canonical_json(candidate);
    SemanticDelta delta{.revision_before=revision_,.revision_after=dry_run?revision_:revision_+1,.entity_id=std::string(entity_id),
        .field="engine.entity.transform",.before_value_json=before_json,.after_value_json=after_json,.manager=std::string(manager),.undoable=true};
    if(!dry_run) {
        const auto loaded=load_scene_internal(candidate,false);
        if(!loaded.success) return failure("scene.edit-apply-failed","The validated transform could not be instantiated.");
        delta.revision_after=revision_;recent_deltas_.push_back(delta);
        undo_stack_.push_back({.delta=delta,.scene_before=before_scene,.scene_after=candidate});redo_stack_.clear();
        if(recent_deltas_.size()>256) recent_deltas_.erase(recent_deltas_.begin());
    }
    return Json{{"schemaVersion","noemancer.transform-edit-receipt/0.1"},{"success",true},{"dryRun",dry_run},
        {"code",dry_run?"scene.edit-validated":"ok"},{"detail",dry_run?"Transform dry run passed.":"Transform committed atomically."},
        {"entityId",entity_id},{"revisionBefore",delta.revision_before},{"revisionAfter",delta.revision_after},{"undoable",true},
        {"transform",{{"position",{{"x",transform.x},{"y",transform.y},{"z",transform.z}}},
            {"rotationQuaternion",{{"x",rotation.x},{"y",rotation.y},{"z",rotation.z},{"w",rotation.w}}},
            {"rotationEulerDegrees",{{"x",found->transform->rotation_euler_degrees.x},{"y",found->transform->rotation_euler_degrees.y},{"z",found->transform->rotation_euler_degrees.z}}},
            {"scale",{{"x",transform.scale_x},{"y",transform.scale_y},{"z",transform.scale_z}}}}}}.dump();
}

std::string World::replace_scene_document_json(const std::string_view document_json,
                                               const std::uint64_t base_revision,
                                               const std::string_view manager,const bool dry_run) {
    const auto failure=[&](const std::string_view code,const std::string_view detail,const Json& errors=Json::array()) {
        return Json{{"schemaVersion","noemancer.scene-document-edit-receipt/0.1"},{"success",false},{"dryRun",dry_run},
            {"code",code},{"detail",detail},{"revisionBefore",revision_},{"revisionAfter",revision_},{"errors",errors}}.dump();};
    if(base_revision!=revision_) return failure("world.revision-conflict","Scene document edit base revision is stale.");
    if(manager.empty()) return failure("scene.invalid-manager","Scene document edits require a stable manager identity.");
    const auto parsed=SceneDocumentCodec::parse_json(document_json,scene_source_uri_);
    if(!parsed) {
        Json errors=Json::array();for(const auto& error:parsed.errors) errors.push_back({{"code",error.code},{"path",error.path},{"message",error.message}});
        return failure("scene.edit-validation-failed","The replacement scene document is invalid.",errors);
    }
    auto candidate=*parsed.document;
    if(candidate.scene_guid!=scene_document_.scene_guid)
        return failure("scene.identity-change-forbidden","An in-place editor transaction cannot replace the scene GUID.");
    candidate.source_uri=scene_source_uri_;
    const auto before_scene=scene_document_;const auto before_json=canonical_scene_json();
    const auto after_json=SceneDocumentCodec::write_canonical_json(candidate);
    if(after_json==before_json) return failure("scene.no-change","The replacement document does not change the scene.");
    SemanticDelta delta{.revision_before=revision_,.revision_after=dry_run?revision_:revision_+1,
        .entity_id=scene_guid_,.field="engine.scene.document",.before_value_json=before_json,.after_value_json=after_json,
        .manager=std::string(manager),.undoable=true};
    if(!dry_run) {
        const auto loaded=load_scene_internal(candidate,false);
        if(!loaded.success) return failure("scene.edit-apply-failed","The validated scene document could not be instantiated.");
        delta.revision_after=revision_;recent_deltas_.push_back(delta);
        undo_stack_.push_back({.delta=delta,.scene_before=before_scene,.scene_after=candidate});redo_stack_.clear();
        if(recent_deltas_.size()>256) recent_deltas_.erase(recent_deltas_.begin());
    }
    return Json{{"schemaVersion","noemancer.scene-document-edit-receipt/0.1"},{"success",true},{"dryRun",dry_run},
        {"code",dry_run?"scene.edit-validated":"ok"},{"detail",dry_run?"Scene document dry run passed.":"Scene document committed atomically."},
        {"revisionBefore",delta.revision_before},{"revisionAfter",delta.revision_after},{"undoable",true},
        {"entityCount",candidate.entities.size()}}.dump();
}

std::string World::save_scene_to_source_json() {
    const auto source = std::filesystem::path(scene_source_uri_);
    const auto failure = [&](const std::string_view code, const std::string_view detail) {
        return Json{{"schemaVersion","noemancer.scene-save-receipt/0.1"},{"success",false},{"code",code},
            {"detail",detail},{"source",scene_source_uri_},{"revision",revision_}}.dump();
    };
    if (scene_source_uri_.empty() || scene_source_uri_.find("://") != std::string::npos || !source.is_absolute())
        return failure("scene.source-not-writable", "The active scene does not have an absolute filesystem source.");
    std::ifstream current_stream(source, std::ios::binary);
    const bool source_exists = static_cast<bool>(current_stream);
    std::string current_contents;
    if (source_exists) current_contents.assign(std::istreambuf_iterator<char>(current_stream), std::istreambuf_iterator<char>());
    if (source_exists != scene_source_existed_at_load_ || (source_exists && current_contents != scene_source_baseline_))
        return failure("scene.save-conflict", "The source scene changed on disk after it was loaded; reload or reconcile it before saving.");
    const auto contents = canonical_scene_json();
    const auto temporary = source.parent_path() / (source.filename().string() + ".noemancer-save.tmp");
    const auto recovery = source.parent_path() / (source.filename().string() + ".noemancer-recovery");
    if (source_exists && current_contents != contents) {
        std::ofstream recovery_stream(recovery, std::ios::binary | std::ios::trunc);
        if (!recovery_stream) return failure("scene.recovery-open-failed", "Could not open the recovery snapshot before replacing the scene.");
        recovery_stream.write(current_contents.data(), static_cast<std::streamsize>(current_contents.size()));
        recovery_stream.flush();
        if (!recovery_stream) return failure("scene.recovery-write-failed", "Could not preserve the previous scene revision; the source was not replaced.");
    }
    std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
    if (!stream) return failure("scene.save-open-failed", "Could not open the sibling temporary scene file.");
    stream.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    stream.flush();
    if (!stream) return failure("scene.save-write-failed", "Could not write the complete canonical scene document.");
    stream.close();
    std::error_code copy_error;
    std::filesystem::copy_file(temporary, source, std::filesystem::copy_options::overwrite_existing, copy_error);
    std::error_code cleanup_error;
    std::filesystem::remove(temporary, cleanup_error);
    if (copy_error) return failure("scene.save-replace-failed", "Could not replace the source scene with the validated temporary document.");
    scene_source_baseline_ = contents;
    scene_source_existed_at_load_ = true;
    return Json{{"schemaVersion","noemancer.scene-save-receipt/0.1"},{"success",true},{"code","ok"},
        {"detail","Canonical scene document saved."},{"source",source.generic_string()},
        {"recoverySource",source_exists&&current_contents!=contents?recovery.generic_string():std::string{}},
        {"revision",revision_},{"bytes",contents.size()}}.dump();
}

std::string World::save_scene_as_source_json(const std::string_view source_path, const bool overwrite) {
    const auto source = std::filesystem::path(source_path);
    const auto failure = [&](const std::string_view code, const std::string_view detail) {
        return Json{{"schemaVersion","noemancer.scene-save-receipt/0.1"},{"success",false},{"code",code},
            {"detail",detail},{"source",source.generic_string()},{"revision",revision_}}.dump();
    };
    if (source_path.empty() || source_path.find("://") != std::string_view::npos || !source.is_absolute())
        return failure("scene.source-not-writable", "Save As requires an absolute filesystem path.");
    if (source.generic_string() == std::filesystem::path(scene_source_uri_).generic_string()) return save_scene_to_source_json();
    std::error_code exists_error;
    const bool exists = std::filesystem::exists(source,exists_error);
    if (exists_error) return failure("scene.save-stat-failed", "Could not inspect the Save As target.");
    if (exists && !overwrite) return failure("scene.save-target-exists", "The Save As target already exists; explicit overwrite permission is required.");
    auto candidate = scene_document_;
    candidate.source_uri = source.generic_string();
    const auto loaded = load_scene_internal(candidate,true);
    if (!loaded.success) return failure("scene.save-as-load-failed", "The scene could not be rebound to the new source path.");
    return save_scene_to_source_json();
}

std::string World::open_scene_from_source_json(const std::string_view source_path) {
    const auto source = std::filesystem::path(source_path);
    const auto failure = [&](const std::string_view code, const std::string_view detail, Json errors=Json::array()) {
        return Json{{"schemaVersion","noemancer.scene-open-receipt/0.1"},{"success",false},{"code",code},
            {"detail",detail},{"source",source.generic_string()},{"revision",revision_},{"errors",std::move(errors)}}.dump();
    };
    if (source_path.empty() || source_path.find("://") != std::string_view::npos || !source.is_absolute())
        return failure("scene.source-not-readable", "Open Scene requires an absolute filesystem path.");
    std::ifstream stream(source,std::ios::binary);
    if (!stream) return failure("scene.open-read-failed", "Could not read the requested scene source.");
    const std::string contents((std::istreambuf_iterator<char>(stream)),std::istreambuf_iterator<char>());
    const auto parsed = SceneDocumentCodec::parse_json(contents,source.generic_string());
    if (!parsed) {
        Json errors=Json::array();
        for (const auto& error:parsed.errors) errors.push_back({{"code",error.code},{"path",error.path},{"message",error.message}});
        return failure("scene.open-invalid", "The requested scene document is invalid.",std::move(errors));
    }
    const auto loaded = load_scene(*parsed.document);
    if (!loaded.success) return failure("scene.open-load-failed", "The validated scene could not be instantiated.");
    return Json{{"schemaVersion","noemancer.scene-open-receipt/0.1"},{"success",true},{"code","ok"},
        {"detail","Scene opened."},{"source",source.generic_string()},{"revision",revision_},{"entityCount",loaded.entity_count}}.dump();
}

std::string World::canonical_scene_json() const {
    return SceneDocumentCodec::write_canonical_json(scene_document_);
}

std::string World::runtime_authoring_scene_json() const {
    return SceneDocumentCodec::write_canonical_json(runtime_authoring_scene_document());
}

SceneDocument World::runtime_authoring_scene_document() const {
    auto snapshot=scene_document_;
    const auto views = entity_views();
    for (auto& authored : snapshot.entities) {
        const auto runtime = std::ranges::find(views, authored.guid, &WorldEntityView::id);
        if (runtime == views.end()) continue;
        if (authored.transform && runtime->transform) {
            // Position is the durable runtime-authored value today. Rotation is
            // represented as a quaternion in ECS and Euler angles on disk, so
            // it stays unchanged until a lossless conversion policy exists.
            authored.transform->position = {runtime->transform->x, runtime->transform->y, runtime->transform->z};
            authored.transform->scale = {runtime->transform->scale_x, runtime->transform->scale_y,
                                         runtime->transform->scale_z};
        }
        if(authored.velocity&&runtime->velocity)
            authored.velocity->linear={runtime->velocity->x,runtime->velocity->y,runtime->velocity->z};
    }
    return snapshot;
}

std::string World::change_plan_json(const TransformChangePlan& plan) {
    return Json{
        {"schemaVersion", "0.1"},
        {"valid", plan.valid},
        {"code", plan.code},
        {"detail", plan.detail},
        {"planId", plan.plan_id},
        {"contentHash", plan.content_hash},
        {"manager", plan.manager},
        {"baseRevision", plan.base_revision},
        {"operation", "transform.set"},
        {"entityId", plan.entity_id},
        {"before", vector_json(plan.before)},
        {"after", vector_json(plan.after)},
        {"predictedDelta", plan.valid ? Json{{"field", "engine.entity.transform.position"}, {"before", vector_json(plan.before)}, {"after", vector_json(plan.after)}} : Json(nullptr)},
        {"risk", "low"},
        {"sideEffects", Json::array({"scene.document.dirty", "render.transform.invalidated"})}
    }.dump();
}

std::string World::property_change_plan_json(const PropertyChangePlan& plan) {
    const auto before=plan.before_value_json.empty()?Json(nullptr):Json::parse(plan.before_value_json);
    const auto after=plan.after_value_json.empty()?Json(nullptr):Json::parse(plan.after_value_json);
    return Json{{"schemaVersion","0.1"},{"valid",plan.valid},{"code",plan.code},{"detail",plan.detail},
        {"planId",plan.plan_id},{"contentHash",plan.content_hash},{"manager",plan.manager},{"baseRevision",plan.base_revision},
        {"operation","property.set"},{"entityId",plan.entity_id},{"property",plan.property},{"before",before},{"after",after},
        {"predictedDelta",plan.valid?Json{{"field",plan.property},{"before",before},{"after",after}}:Json(nullptr)},
        {"risk","low"},{"sideEffects",Json::array({"scene.document.dirty","component.invalidated"})}}.dump();
}

std::string World::action_receipt_json(const ActionReceipt& receipt) {
    return Json{
        {"schemaVersion", "0.1"},
        {"success", receipt.success},
        {"dryRun", receipt.dry_run},
        {"code", receipt.code},
        {"detail", receipt.detail},
        {"operationId", receipt.operation_id},
        {"planId", receipt.plan_id},
        {"revisionBefore", receipt.revision_before},
        {"revisionAfter", receipt.revision_after},
        {"delta", receipt.delta ? delta_to_json(*receipt.delta) : Json(nullptr)},
        {"evidence", Json::array({Json{{"kind", "semantic-delta"}, {"available", receipt.delta.has_value()}}})}
    }.dump();
}

std::string World::snapshot_json() const {
    Json entities = Json::array();
    world_.each([&](flecs::entity entity) {
        const auto* transform = entity.try_get<Transform>();
        const auto* identity = entity.try_get<SemanticIdentity>();
        if (identity == nullptr) {
            return;
        }
        Json components = Json::object();
        if (transform != nullptr) {
            components["Transform"] = {
                {"schemaRef", "schema://noemancer/component/transform/0.1"},
                {"position", {
                    {"x", semantic_f32(transform->x)},
                    {"y", semantic_f32(transform->y)},
                    {"z", semantic_f32(transform->z)},
                    {"unit", "m"},
                    {"coordinateSpace", "world.right-handed.y-up"}
                }},
                {"scale", {{"x", semantic_f32(transform->scale_x)}, {"y", semantic_f32(transform->scale_y)},
                    {"z", semantic_f32(transform->scale_z)}, {"unit", "ratio"}}},
                {"rotationEulerDegrees",rotation_euler_json(*transform)},
                {"rotationQuaternion",{{"x",semantic_f32(transform->rotation_x)},{"y",semantic_f32(transform->rotation_y)},
                    {"z",semantic_f32(transform->rotation_z)},{"w",semantic_f32(transform->rotation_w)}}}
            };
        }
        const auto* velocity = entity.try_get<Velocity>();
        if (velocity != nullptr) {
            components["Velocity"] = {
                {"schemaRef", "schema://noemancer/component/velocity/0.1"},
                {"linear", {
                    {"x", semantic_f32(velocity->x)},
                    {"y", semantic_f32(velocity->y)},
                    {"z", semantic_f32(velocity->z)},
                    {"unit", "m/s"},
                    {"coordinateSpace", "world.right-handed.y-up"}
                }}
            };
        }
        if (const auto* motor = entity.try_get<CharacterMotor2D>()) {
            components["CharacterMotor2D"] = {
                {"schemaRef", "schema://noemancer/component/character-motor-2d/0.1"},
                {"config", {{"maximumSpeed", motor->config.maximum_speed},
                    {"groundAcceleration", motor->config.ground_acceleration}, {"airAcceleration", motor->config.air_acceleration},
                    {"groundDeceleration", motor->config.ground_deceleration}, {"jumpSpeed", motor->config.jump_speed},
                    {"maximumFallSpeed", motor->config.maximum_fall_speed}, {"coyoteTimeSeconds", motor->config.coyote_time_seconds},
                    {"jumpBufferSeconds", motor->config.jump_buffer_seconds}, {"groundProbeDistance", motor->config.ground_probe_distance},
                    {"minimumGroundNormalY", motor->config.minimum_ground_normal_y},{"jumpReleaseVelocityFactor",motor->config.jump_release_velocity_factor}}},
                {"state", {{"grounded", motor->state.grounded}, {"groundEntityId", motor->state.ground_entity_id},
                    {"wallEntityId",motor->state.wall_entity_id},{"groundNormal",Json::array({motor->state.ground_normal_x,motor->state.ground_normal_y})},
                    {"moveInput", motor->state.move_input}, {"coyoteRemaining", motor->state.coyote_remaining},
                    {"jumpBufferRemaining", motor->state.jump_buffer_remaining}, {"decision", motor->state.decision},
                    {"reason", motor->state.reason}, {"jumpCount", motor->state.jump_count},
                    {"landingCount", motor->state.landing_count}}}
            };
        }
        if(const auto* platform=entity.try_get<Platform2D>()) components["Platform2D"]={{"schemaRef","schema://noemancer/component/platform-2d/0.1"},
            {"collisionMode",platform->collision_mode},{"motionAxis",vector_json({platform->axis_x,platform->axis_y,platform->axis_z})},
            {"motionDistance",platform->motion_distance},{"motionPeriodSeconds",platform->motion_period_seconds},{"motionPhase",platform->motion_phase},
            {"origin",vector_json({platform->origin_x,platform->origin_y,platform->origin_z})},{"elapsedSeconds",platform->elapsed_seconds}};
        if (const auto* camera = entity.try_get<Camera>()) {
            components["Camera"] = {
                {"schemaRef", "schema://noemancer/component/camera/0.1"},
                {"target", vector_json({camera->target_x, camera->target_y, camera->target_z})},
                {"verticalFovDegrees", camera->vertical_fov_degrees},
                {"nearClip", camera->near_clip}, {"farClip", camera->far_clip}, {"primary", camera->primary},
                {"projection",camera->projection},{"orthographicHeight",camera->orthographic_height}
            };
        }
        if(const auto* follow=entity.try_get<CameraFollow2D>()) components["CameraFollow2D"]={{"schemaRef","schema://noemancer/component/camera-follow-2d/0.1"},
            {"targetEntityId",follow->target_entity_id},{"positionOffset",vector_json({follow->offset_x,follow->offset_y,follow->offset_z})},
            {"deadZone",Json::array({follow->dead_zone_x,follow->dead_zone_y})},{"lookAheadDistance",follow->look_ahead_distance},
            {"smoothing",follow->smoothing},{"center",Json::array({follow->center_x,follow->center_y})},{"decision",follow->decision}};
        if (const auto* light = entity.try_get<DirectionalLight>()) {
            components["DirectionalLight"] = {
                {"schemaRef", "schema://noemancer/component/directional-light/0.1"},
                {"direction", vector_json({light->direction_x, light->direction_y, light->direction_z})},
                {"color", vector_json({light->color_r, light->color_g, light->color_b})},
                {"intensity", light->intensity}, {"ambientIntensity", light->ambient_intensity},
                {"castsShadows", light->casts_shadows}
            };
        }
        if(const auto* light=entity.try_get<LocalLight>())components["LocalLight"]={{"schemaRef","schema://noemancer/component/local-light/0.1"},
            {"kind",light->kind},{"color",vector_json({light->color_r,light->color_g,light->color_b})},
            {"luminousPowerLumens",light->luminous_power_lumens},{"rangeMeters",light->range_meters},
            {"direction",vector_json({light->direction_x,light->direction_y,light->direction_z})},
            {"innerConeDegrees",light->inner_cone_degrees},{"outerConeDegrees",light->outer_cone_degrees},
            {"sourceRadiusMeters",light->source_radius_meters},{"castsShadows",light->casts_shadows}};
        if (const auto* renderer = entity.try_get<MeshRenderer>()) {
            components["MeshRenderer"] = {
                {"schemaRef", "schema://noemancer/component/mesh-renderer/0.1"},
                {"meshAsset", renderer->mesh_asset}, {"visible", renderer->visible},
                {"castsShadows", renderer->casts_shadows}, {"receivesShadows", renderer->receives_shadows}
            };
        }
        if(const auto* renderer=entity.try_get<SpriteRenderer>()) {
            auto playback=Json::parse(sprite_assets_.observe_json(renderer->playback));
            components["SpriteRenderer"]={{"schemaRef","schema://noemancer/component/sprite-renderer/0.1"},
                {"visible",renderer->visible},{"flipX",renderer->flip_x},{"flipY",renderer->flip_y},
                {"sortingLayer",renderer->sorting_layer},{"sortingOrder",renderer->sorting_order},
                {"playbackSpeed",renderer->playback_speed},{"playback",std::move(playback)}};
        }
        if(const auto* renderer=entity.try_get<TilemapRenderer>())components["TilemapRenderer"]={{"schemaRef","schema://noemancer/component/tilemap-renderer/0.1"},
            {"tilemapAsset",renderer->tilemap_asset},{"visible",renderer->visible},{"collisionEnabled",renderer->collision_enabled}};
        if (const auto* material = entity.try_get<PbrMaterial>()) {
            components["PbrMaterial"] = {
                {"schemaRef", "schema://noemancer/component/pbr-material/0.1"},
                {"baseColor", vector_json({material->base_r, material->base_g, material->base_b})},
                {"metallic", material->metallic}, {"roughness", material->roughness},
                {"baseColorTexture", material->base_color_texture.empty() ? Json(nullptr) : Json(material->base_color_texture)},
                {"emissiveColor", vector_json({material->emissive_r, material->emissive_g, material->emissive_b})},
                {"emissiveIntensity", material->emissive_intensity}
            };
        }
        if (const auto* body = entity.try_get<RigidBody>()) {
            components["RigidBody"] = {{"schemaRef", "schema://noemancer/component/rigid-body/0.1"},
                {"motionType", motion_type_name(body->motion_type)}, {"mass", body->mass}, {"gravityFactor", body->gravity_factor},
                {"linearDamping", body->linear_damping}};
        }
        if (const auto* collider = entity.try_get<BoxCollider>()) {
            components["BoxCollider"] = {{"schemaRef", "schema://noemancer/component/box-collider/0.1"},
                {"halfExtents", vector_json({collider->half_x, collider->half_y, collider->half_z})}, {"friction", collider->friction},
                {"restitution", collider->restitution}};
        }
        if (const auto* collider = entity.try_get<SphereCollider>()) {
            components["SphereCollider"] = {{"schemaRef", "schema://noemancer/component/sphere-collider/0.1"},
                {"radius", collider->radius}, {"friction", collider->friction}, {"restitution", collider->restitution}};
        }
        if (const auto* player = entity.try_get<AnimationPlayer>()) {
            components["AnimationPlayer"] = {{"schemaRef", "schema://noemancer/component/animation-player/0.1"},
                {"clipAsset", player->clip_asset}, {"timeSeconds", player->time_seconds},
                {"durationSeconds", animation_runtime_.duration(player->clip_asset)}, {"playbackSpeed", player->playback_speed},
                {"looping", player->looping}, {"playing", player->playing}, {"rootMotionMode", player->root_motion_mode},
                {"nextClipAsset", player->next_clip_asset}, {"transitionDurationSeconds", player->transition_duration_seconds},
                {"transitionElapsedSeconds", player->transition_elapsed_seconds},{"stateMachineAsset",player->state_machine_asset},
                {"activeState",player->active_state},{"stateParameters",player->state_parameters},
                {"animationGraphAsset",player->animation_graph_asset},{"graphParameters",player->graph_parameters}};
        }
        entities.push_back({
            {"ref", semantic_ref_json(*identity, revision_)},
            {"sceneGuid", identity->scene_guid},
            {"parentGuid", identity->parent_guid.empty() ? Json(nullptr) : Json(identity->parent_guid)},
            {"source", {
                {"uri", identity->source.uri},
                {"pointer", identity->source.json_pointer}
            }},
            {"components", std::move(components)}
        });
    });

    const Json snapshot = {
        {"schemaVersion", "0.1"},
        {"revision", revision_},
        {"scope", {
            {"id", "world.main"},
            {"path", "/world"},
            {"type", "engine.world"},
            {"sceneGuid", scene_guid_},
            {"displayName", scene_name_},
            {"sourceUri", scene_source_uri_}
        }},
        {"conventions", "schema://noemancer/semantic-conventions/core/0.1"},
        {"entities", std::move(entities)}
    };
    return snapshot.dump();
}

std::string World::render_observation_json() const {
    Json cameras = Json::array();
    Json lights = Json::array();
    Json renderables = Json::array();
    for (const auto& view : entity_views()) {
        if (view.camera && view.transform) {
            cameras.push_back({
                {"entityId", view.id}, {"primary", view.camera->primary},
                {"position", vector_json(semantic_vector(*view.transform))},
                {"target", vector_json({view.camera->target_x, view.camera->target_y, view.camera->target_z})},
                {"verticalFovDegrees", view.camera->vertical_fov_degrees},
                {"nearClip", view.camera->near_clip}, {"farClip", view.camera->far_clip},
                {"projection",view.camera->projection},{"orthographicHeight",view.camera->orthographic_height}
            });
        }
        if (view.directional_light) {
            lights.push_back({
                {"entityId", view.id},
                {"direction", vector_json({view.directional_light->direction_x, view.directional_light->direction_y, view.directional_light->direction_z})},
                {"color", vector_json({view.directional_light->color_r, view.directional_light->color_g, view.directional_light->color_b})},
                {"intensity", view.directional_light->intensity},
                {"ambientIntensity", view.directional_light->ambient_intensity},
                {"castsShadows", view.directional_light->casts_shadows}
            });
        }
        if (view.mesh_renderer && view.transform) {
            Json renderable = {
                {"entityId", view.id}, {"meshAsset", view.mesh_renderer->mesh_asset},
                {"position", vector_json(semantic_vector(*view.transform))},
                {"visible", view.mesh_renderer->visible},
                {"castsShadows", view.mesh_renderer->casts_shadows},
                {"receivesShadows", view.mesh_renderer->receives_shadows}
            };
            if (view.pbr_material) {
                renderable["material"] = {
                    {"baseColor", vector_json({view.pbr_material->base_r, view.pbr_material->base_g, view.pbr_material->base_b})},
                    {"metallic", view.pbr_material->metallic}, {"roughness", view.pbr_material->roughness},
                    {"baseColorTexture", view.pbr_material->base_color_texture.empty() ? Json(nullptr) : Json(view.pbr_material->base_color_texture)},
                    {"emissiveColor", vector_json({view.pbr_material->emissive_r, view.pbr_material->emissive_g, view.pbr_material->emissive_b})},
                    {"emissiveIntensity", view.pbr_material->emissive_intensity}
                };
            }
            renderables.push_back(std::move(renderable));
        }
    }
    return Json{
        {"schemaVersion", "0.1"}, {"revision", revision_},
        {"scope", "render.scene.main"}, {"cameras", std::move(cameras)},
        {"directionalLights", std::move(lights)}, {"renderables", std::move(renderables)}
    }.dump();
}

std::string World::physics_observation_json() const {
    Json bodies = Json::array();
    const auto views=entity_views();
    for (const auto& view : views) {
        if (!view.rigid_body || !view.transform) continue;
        Json body = {{"entityId", view.id}, {"motionType", motion_type_name(view.rigid_body->motion_type)},
            {"position", vector_json(semantic_vector(*view.transform))}, {"mass", view.rigid_body->mass},
            {"gravityFactor", view.rigid_body->gravity_factor}, {"linearDamping", view.rigid_body->linear_damping}};
        body["rotationQuaternion"]={{"x",view.transform->rotation_x},{"y",view.transform->rotation_y},
            {"z",view.transform->rotation_z},{"w",view.transform->rotation_w}};
        body["rotationEulerDegrees"]=rotation_euler_json(*view.transform);
        if (view.velocity) body["linearVelocity"] = vector_json({view.velocity->x, view.velocity->y, view.velocity->z});
        if (view.box_collider) body["boxCollider"] = {{"halfExtents", vector_json({view.box_collider->half_x, view.box_collider->half_y, view.box_collider->half_z})},
            {"friction", view.box_collider->friction}, {"restitution", view.box_collider->restitution},{"isTrigger",view.box_collider->is_trigger}};
        if (view.sphere_collider) body["sphereCollider"] = {{"radius", view.sphere_collider->radius},
            {"friction", view.sphere_collider->friction}, {"restitution", view.sphere_collider->restitution},{"isTrigger",view.sphere_collider->is_trigger}};
        if (view.capsule_collider) body["capsuleCollider"] = {{"radius", view.capsule_collider->radius},
            {"halfHeight", view.capsule_collider->half_height}, {"friction", view.capsule_collider->friction},
            {"restitution", view.capsule_collider->restitution},{"isTrigger",view.capsule_collider->is_trigger}};
        if(view.convex_hull_collider) {
            Json points=Json::array(); for(const auto& point:view.convex_hull_collider->points) points.push_back(Json::array({point[0],point[1],point[2]}));
            body["convexHullCollider"]={{"points",std::move(points)},{"pointCount",view.convex_hull_collider->points.size()},
                {"friction",view.convex_hull_collider->friction},{"restitution",view.convex_hull_collider->restitution},
                {"isTrigger",view.convex_hull_collider->is_trigger}};
        }
        bodies.push_back(std::move(body));
    }
    std::vector<PhysicsBodyState> tilemap_bodies;append_tilemap_physics_bodies(views,tilemap_bodies);
    for(const auto& body:tilemap_bodies)bodies.push_back({{"entityId",body.entity_id},{"motionType","static"},
        {"position",vector_json({body.position_x,body.position_y,body.position_z})},{"generatedBy","TilemapColliderBake"},
        {"boxCollider",{{"halfExtents",vector_json({body.half_x,body.half_y,body.half_z})},{"friction",body.friction},
            {"restitution",body.restitution},{"oneWay",body.one_way},{"isTrigger",false}}}});
    Json contacts = Json::array();
    for (const auto& contact : physics_runtime_.contacts()) contacts.push_back({{"bodyA", contact.body_a}, {"bodyB", contact.body_b},
        {"normal", vector_json({contact.normal_x, contact.normal_y, contact.normal_z})}, {"penetration", contact.penetration},
        {"isTrigger",contact.is_trigger}});
    return Json{{"schemaVersion", "noemancer.physics-observation/0.1"}, {"revision", revision_},
        {"backend", physics_runtime_.backend_id()}, {"fixedStepSeconds", 1.0 / 60.0}, {"bodies", std::move(bodies)},
        {"contacts", std::move(contacts)}}.dump();
}

std::string World::physics_ray_cast_json(const Transform origin, const Transform direction) const {
    const auto hit = physics_runtime_.ray_cast(origin.x, origin.y, origin.z, direction.x, direction.y, direction.z);
    Json result = {{"schemaVersion", "noemancer.physics-ray-cast/0.1"}, {"revision", revision_}, {"backend", physics_runtime_.backend_id()},
        {"origin", vector_json({origin.x, origin.y, origin.z})}, {"direction", vector_json({direction.x, direction.y, direction.z})},
        {"hit", hit.hit}};
    if (hit.hit) result["result"] = {{"entityId", hit.entity_id}, {"fraction", hit.fraction},
        {"position", vector_json({hit.position_x, hit.position_y, hit.position_z})}};
    else result["result"] = nullptr;
    return result.dump();
}

std::string World::physics_sphere_sweep_json(const Transform origin,const Transform direction,const float radius,
                                             const std::string_view ignored_entity_id) const {
    const auto hit=physics_runtime_.sphere_sweep(origin.x,origin.y,origin.z,direction.x,direction.y,direction.z,radius,ignored_entity_id);
    Json result={{"schemaVersion","noemancer.physics-sphere-sweep/0.1"},{"revision",revision_},
        {"backend",physics_runtime_.backend_id()},{"shape",{{"type","sphere"},{"radius",radius}}},
        {"origin",vector_json({origin.x,origin.y,origin.z})},{"direction",vector_json({direction.x,direction.y,direction.z})},
        {"ignoredEntityId",ignored_entity_id},{"hit",hit.hit}};
    if (hit.hit) result["result"]={{"entityId",hit.entity_id},{"fraction",hit.fraction},
        {"position",vector_json({hit.position_x,hit.position_y,hit.position_z})},
        {"normal",vector_json({hit.normal_x,hit.normal_y,hit.normal_z})},{"penetrationDepth",hit.penetration_depth}};
    else result["result"]=nullptr;
    return result.dump();
}

std::string World::animation_observation_json() const {
    Json players = Json::array();
    Json cues = Json::array();
    for (const auto& view : entity_views()) {
        if (view.animation_cue) cues.push_back({{"entityId", view.id}, {"cue", view.animation_cue->cue},
            {"sourceEventType", view.animation_cue->source_event_type},
            {"eventSequence", view.animation_cue->event_sequence}, {"ageSeconds", view.animation_cue->age_seconds}});
        if (!view.animation_player) continue;
        Json player = {{"entityId", view.id}, {"clipAsset", view.animation_player->clip_asset},
            {"timeSeconds", view.animation_player->time_seconds}, {"durationSeconds", animation_runtime_.duration(view.animation_player->clip_asset)},
            {"playbackSpeed", view.animation_player->playback_speed}, {"looping", view.animation_player->looping},
            {"playing", view.animation_player->playing}, {"rootMotionMode", view.animation_player->root_motion_mode},
            {"stateMachine",{{"assetId",view.animation_player->state_machine_asset},{"activeState",view.animation_player->active_state},
                {"previousState",view.animation_player->previous_state},{"stateElapsedSeconds",view.animation_player->state_elapsed_seconds},
                {"transitionCount",view.animation_player->state_transition_count},{"parameters",view.animation_player->state_parameters}}},
            {"animationGraph",{{"assetId",view.animation_player->animation_graph_asset},{"parameters",view.animation_player->graph_parameters},
                {"syncPhases",view.animation_player->graph_sync_phases}}},
            {"transition", nullptr}, {"pose", nullptr}};
        if (!view.animation_player->next_clip_asset.empty()) player["transition"] = {
            {"targetClipAsset", view.animation_player->next_clip_asset}, {"targetTimeSeconds", view.animation_player->next_time_seconds},
            {"durationSeconds", view.animation_player->transition_duration_seconds},
            {"elapsedSeconds", view.animation_player->transition_elapsed_seconds},
            {"weight", view.animation_player->transition_duration_seconds > 0.0F ?
                view.animation_player->transition_elapsed_seconds / view.animation_player->transition_duration_seconds : 1.0F}};
        if (view.skeletal_pose) player["pose"] = {{"skeletonAsset", view.skeletal_pose->skeleton_asset},
            {"jointCount", view.skeletal_pose->skinning_matrices.size()}, {"space", "mesh-from-bind-pose"},
            {"gpuPaletteLimit", SkeletalPose::maximum_joints},
            {"debugEvidence", "animation://skeleton/" + view.id}};
        players.push_back(std::move(player));
    }
    return Json{{"schemaVersion", "noemancer.animation-observation/0.2"}, {"revision", revision_},
        {"backend", animation_runtime_.backend_id()}, {"players", std::move(players)},
        {"gameplayCues", std::move(cues)}}.dump();
}

std::string World::sprite_observation_json(const std::string_view entity_id) const {
    Json items=Json::array();
    for(const auto& view:entity_views()) {
        if(!view.sprite_renderer||(!entity_id.empty()&&view.id!=entity_id))continue;
        auto playback=Json::parse(sprite_assets_.observe_json(view.sprite_renderer->playback));
        items.push_back({{"entityId",view.id},{"visible",view.sprite_renderer->visible},
            {"flipX",view.sprite_renderer->flip_x},{"flipY",view.sprite_renderer->flip_y},
            {"sortingLayer",view.sprite_renderer->sorting_layer},{"sortingOrder",view.sprite_renderer->sorting_order},
            {"playbackSpeed",view.sprite_renderer->playback_speed},{"playback",std::move(playback)}});
    }
    return Json{{"schemaVersion","noemancer.sprite-world-observation/0.1"},{"revision",revision_},
        {"filterEntityId",entity_id},{"items",std::move(items)}}.dump();
}

std::string World::set_sprite_playback_json(const std::string_view entity_id,const std::string_view clip_id,
                                             const bool playing,const float playback_speed,const bool flip_x,
                                             const bool restart) {
    const auto failure=[&](const std::string_view code) {
        return Json{{"schemaVersion","noemancer.sprite-playback-receipt/0.1"},{"success",false},{"code",code},
            {"entityId",entity_id},{"clipId",clip_id},{"revisionBefore",revision_},{"revisionAfter",revision_}}.dump();
    };
    if(clip_id.empty()||!std::isfinite(playback_speed)||playback_speed<0.0F||playback_speed>16.0F)
        return failure("sprite.invalid-playback-request");
    const auto found=entity_ids_.find(std::string(entity_id));if(found==entity_ids_.end())return failure("world.entity-not-found");
    auto entity=world_.entity(found->second);auto* renderer=entity.try_get_mut<SpriteRenderer>();
    if(renderer==nullptr)return failure("sprite.renderer-unavailable");
    const auto* asset=sprite_assets_.find(renderer->playback.asset_id);
    if(asset==nullptr)return failure("sprite.asset-not-found");
    if(std::ranges::none_of(asset->clips,[&](const SpriteClip& clip){return clip.id==clip_id;}))return failure("sprite.clip-not-found");
    const auto runtime_state=[&]() {return Json{{"playback",Json::parse(sprite_assets_.observe_json(renderer->playback))},
        {"playbackSpeed",renderer->playback_speed},{"flipX",renderer->flip_x}}.dump();};
    const auto revision_before=revision_;const auto before=runtime_state();
    const auto changed=restart||renderer->playback.clip_id!=clip_id||renderer->playback.playing!=playing||
        renderer->playback_speed!=playback_speed||renderer->flip_x!=flip_x;
    if(!changed)return Json{{"schemaVersion","noemancer.sprite-playback-receipt/0.1"},{"success",true},{"code","ok"},
        {"entityId",entity_id},{"clipId",clip_id},{"changed",false},{"revisionBefore",revision_},{"revisionAfter",revision_},
        {"state",Json::parse(before)}}.dump();
    if(restart||renderer->playback.clip_id!=clip_id) {
        renderer->playback.clip_id=std::string(clip_id);renderer->playback.frame_index=0;
        renderer->playback.elapsed_in_frame_ms=0.0;renderer->playback.completed_loops=0;renderer->playback.last_event.clear();
    }
    renderer->playback.playing=playing;renderer->playback_speed=playback_speed;renderer->flip_x=flip_x;
    entity.modified<SpriteRenderer>();const auto after=runtime_state();
    SemanticDelta delta{.revision_before=revision_before,.revision_after=revision_before+1,.entity_id=std::string(entity_id),
        .field="engine.entity.sprite.playback",.before_value_json=before,.after_value_json=after,
        .manager="runtime.sprite",.undoable=false};
    ++revision_;recent_deltas_.push_back(std::move(delta));if(recent_deltas_.size()>256U)recent_deltas_.erase(recent_deltas_.begin());
    return Json{{"schemaVersion","noemancer.sprite-playback-receipt/0.1"},{"success",true},{"code","ok"},
        {"entityId",entity_id},{"clipId",clip_id},{"changed",true},{"revisionBefore",revision_before},{"revisionAfter",revision_},
        {"state",Json::parse(after)}}.dump();
}

std::string World::animation_skeleton_json(const std::string_view entity_id, const std::size_t max_joints) const {
    const auto views = entity_views();
    const auto found = std::ranges::find(views, entity_id, &WorldEntityView::id);
    if (found == views.end() || !found->animation_player || !found->skeletal_pose) {
        return Json{{"schemaVersion", "noemancer.animation-skeleton/0.1"}, {"revision", revision_},
            {"entityId", entity_id}, {"valid", false}, {"code", "animation.pose-unavailable"}, {"joints", Json::array()}}.dump();
    }
    Json joints = Json::array();
    const auto count = std::min(max_joints, found->skeletal_pose->joints.size());
    for (std::size_t index = 0; index < count; ++index) {
        const auto& joint = found->skeletal_pose->joints[index];
        joints.push_back({{"index", index}, {"name", joint.name}, {"parent", joint.parent},
            {"modelPosition", vector_json({joint.model_x, joint.model_y, joint.model_z})}});
    }
    return Json{{"schemaVersion", "noemancer.animation-skeleton/0.1"}, {"revision", revision_},
        {"entityId", entity_id}, {"valid", true}, {"code", "ok"}, {"skeletonAsset", found->skeletal_pose->skeleton_asset},
        {"clipAsset", found->skeletal_pose->clip_asset}, {"jointCount", found->skeletal_pose->joints.size()},
        {"returnedJointCount", count}, {"truncated", count < found->skeletal_pose->joints.size()},
        {"space", "entity-local-model"}, {"joints", std::move(joints)}}.dump();
}

std::string World::animation_state_machine_json(const std::string_view entity_id) const {
    const auto views=entity_views(); const auto found=std::ranges::find(views,entity_id,&WorldEntityView::id);
    if(found==views.end()||!found->animation_player) return Json{{"schemaVersion","noemancer.animation-state-machine/0.1"},
        {"valid",false},{"code","animation.player-unavailable"},{"entityId",entity_id},{"definition",nullptr},{"instance",nullptr}}.dump();
    const auto& player=*found->animation_player;
    const auto inspection=Json::parse(animation_state_machines_.inspect_json(player.state_machine_asset),nullptr,false);
    const auto valid=inspection.is_object()&&inspection.value("valid",false);
    const auto definition=valid?inspection.value("definition",Json{}):Json(nullptr);
    return Json{{"schemaVersion","noemancer.animation-state-machine/0.1"},{"valid",valid},
        {"code",valid?"ok":"animation.machine-not-found"},{"entityId",entity_id},
        {"definition",definition},{"instance",{{"activeState",player.active_state},{"previousState",player.previous_state},
            {"stateElapsedSeconds",player.state_elapsed_seconds},{"transitionCount",player.state_transition_count},
            {"parameters",player.state_parameters},{"poseBackend",animation_runtime_.backend_id()}}}}.dump();
}

std::string World::animation_state_parameter_set_json(const std::string_view entity_id,const std::string_view parameter,const float value) {
    if(!std::isfinite(value)) return Json{{"schemaVersion","noemancer.animation-state-parameter-receipt/0.1"},
        {"success",false},{"code","animation.invalid-parameter"},{"entityId",entity_id},{"parameter",parameter},{"revision",revision_}}.dump();
    const auto found=entity_ids_.find(std::string(entity_id));
    if(found==entity_ids_.end()) return Json{{"schemaVersion","noemancer.animation-state-parameter-receipt/0.1"},
        {"success",false},{"code","world.entity-not-found"},{"entityId",entity_id},{"parameter",parameter},{"revision",revision_}}.dump();
    auto entity=world_.entity(found->second); const auto* current=entity.try_get<AnimationPlayer>();
    if(!current) return Json{{"schemaVersion","noemancer.animation-state-parameter-receipt/0.1"},{"success",false},
        {"code","animation.player-unavailable"},{"entityId",entity_id},{"parameter",parameter},{"revision",revision_}}.dump();
    auto player=*current;const auto* machine=animation_state_machines_.find(player.state_machine_asset);
    if(!machine)return Json{{"schemaVersion","noemancer.animation-state-parameter-receipt/0.1"},{"success",false},
        {"code","animation.machine-not-found"},{"entityId",entity_id},{"parameter",parameter},{"revision",revision_}}.dump();
    const auto definition=std::ranges::find(machine->parameters,parameter,&AnimationStateParameterDefinition::id);
    if(definition==machine->parameters.end())return Json{{"schemaVersion","noemancer.animation-state-parameter-receipt/0.1"},{"success",false},
        {"code","animation.invalid-parameter"},{"entityId",entity_id},{"parameter",parameter},{"revision",revision_}}.dump();
    const auto previous=player.state_parameters[std::string(parameter)];
    player.state_parameters[std::string(parameter)]=definition->type=="bool"?(value>=0.5F?1.0F:0.0F):value;
    const auto applied=player.state_parameters[std::string(parameter)];
    entity.set<AnimationPlayer>(std::move(player)); ++revision_;
    return Json{{"schemaVersion","noemancer.animation-state-parameter-receipt/0.1"},{"success",true},{"code","ok"},
        {"entityId",entity_id},{"parameter",parameter},{"before",previous},{"after",applied},{"revision",revision_}}.dump();
}

std::string World::animation_graph_json(const std::string_view entity_id) const {
    const auto views=entity_views();const auto found=std::ranges::find(views,entity_id,&WorldEntityView::id);
    if(found==views.end()||!found->animation_player)return Json{{"schemaVersion","noemancer.animation-graph-instance/0.1"},
        {"valid",false},{"code","animation.player-unavailable"},{"entityId",entity_id},{"definition",nullptr},{"instance",nullptr}}.dump();
    const auto& player=*found->animation_player;
    const auto inspection=Json::parse(animation_graphs_.inspect_json(player.animation_graph_asset),nullptr,false);
    const auto valid=inspection.is_object()&&inspection.value("valid",false);
    Json execution=nullptr;if(valid)if(const auto request=animation_graph_pose_request(player)) {
        Json layers=Json::array();for(const auto& layer:request->layers)layers.push_back({{"id",layer.id},{"clipAsset",layer.clip_asset},
            {"secondaryClipAsset",layer.secondary_clip_asset.empty()?Json(nullptr):Json(layer.secondary_clip_asset)},
            {"secondaryWeight",layer.secondary_weight},{"weight",layer.weight},{"maskId",layer.mask_id},
            {"mode",layer.mode==AnimationPoseLayerMode::additive?"additive":"override"}});
        const auto executed=animation_runtime_.sample_layered_skeletal_pose(*request);
        execution={{"valid",executed.success},{"code",executed.code},{"detail",executed.detail},
            {"baseClipAsset",request->base_clip_asset},
            {"baseSecondaryClipAsset",request->base_secondary_clip_asset.empty()?Json(nullptr):Json(request->base_secondary_clip_asset)},
            {"baseSecondaryWeight",request->base_secondary_weight},{"layers",std::move(layers)},
            {"maskCount",request->masks.size()},{"syncPhases",player.graph_sync_phases}};
    } else execution={{"valid",false},{"code","animation.graph-execution-plan-unavailable"}};
    return Json{{"schemaVersion","noemancer.animation-graph-instance/0.1"},{"valid",valid},
        {"code",valid?"ok":"animation.graph-not-found"},{"entityId",entity_id},
        {"definition",valid?inspection.value("definition",Json{}):Json(nullptr)},
        {"instance",{{"parameters",player.graph_parameters},{"poseBackend",animation_runtime_.backend_id()},
            {"execution",std::move(execution)}}}}.dump();
}

std::string World::animation_graph_parameter_set_json(const std::string_view entity_id,const std::string_view parameter,const float value) {
    const auto failure=[&](const std::string_view code){return Json{{"schemaVersion","noemancer.animation-graph-parameter-receipt/0.1"},
        {"success",false},{"code",code},{"entityId",entity_id},{"parameter",parameter},{"revision",revision_}}.dump();};
    if(!std::isfinite(value))return failure("animation.invalid-parameter");
    const auto found=entity_ids_.find(std::string(entity_id));if(found==entity_ids_.end())return failure("world.entity-not-found");
    auto entity=world_.entity(found->second);const auto* current=entity.try_get<AnimationPlayer>();
    if(current==nullptr)return failure("animation.player-unavailable");
    auto player=*current;const auto* graph=animation_graphs_.find(player.animation_graph_asset);
    if(graph==nullptr)return failure("animation.graph-not-found");
    const auto definition=std::ranges::find(graph->parameters,parameter,&AnimationGraphParameter::id);
    if(definition==graph->parameters.end())return failure("animation.invalid-parameter");
    const auto previous=player.graph_parameters[std::string(parameter)];
    player.graph_parameters[std::string(parameter)]=definition->type=="bool"?(value>=0.5F?1.0F:0.0F):value;
    const auto applied=player.graph_parameters[std::string(parameter)];
    if(player.state_parameters.contains(std::string(parameter)))player.state_parameters[std::string(parameter)]=applied;
    entity.set<AnimationPlayer>(std::move(player));++revision_;
    return Json{{"schemaVersion","noemancer.animation-graph-parameter-receipt/0.1"},{"success",true},{"code","ok"},
        {"entityId",entity_id},{"parameter",parameter},{"before",previous},{"after",applied},{"revision",revision_}}.dump();
}

std::string World::input_observation_json() const {
    return input_runtime_.observe_json();
}

bool World::configure_input_actions(const std::span<const InputActionDefinition> definitions) {
    return definitions.empty()?input_runtime_.reset_defaults():input_runtime_.configure(definitions);
}

bool World::configure_project_hud(const std::string_view project_document_json) {
    if(project_document_json.empty()){project_hud_document_json_.clear();return true;}
    const auto validation=Json::parse(semantic_ui_validation_json(project_document_json),nullptr,false);
    if(!validation.is_object()||!validation.value("valid",false))return false;
    project_hud_document_json_=project_document_json;return true;
}

std::string World::inject_input_json(const std::string_view source, const float value) {
    const auto before = input_runtime_.revision();
    const auto success = input_runtime_.set_source_value(source, value);
    if (success) {
        if (replay_recording_) recorded_inputs_.push_back({next_recorded_input_sequence_++,simulation_tick_-replay_start_tick_,std::string(source),value});
        input_runtime_.evaluate();
        gameplay_runtime_.update_from_input(input_runtime_);
        vfx_runtime_.consume_gameplay_events(gameplay_runtime_.events());
    }
    return Json{{"schemaVersion", "noemancer.input-injection-receipt/0.1"}, {"success", success},
        {"code", success ? "ok" : "input.invalid-source-value"}, {"source", source}, {"value", value},
        {"revisionBefore", before}, {"revisionAfter", input_runtime_.revision()}}.dump();
}

std::string World::audio_observation_json() const {
    return audio_runtime_.observe_json();
}

std::string World::register_audio_asset_json(const std::string_view asset_id,const std::string_view content_hash,
                                              const AudioAssetStorage storage) {
    const auto before=audio_runtime_.revision();
    const auto valid=audio_runtime_.register_asset({std::string(asset_id),std::string(content_hash),storage});
    return Json{{"schemaVersion","noemancer.audio-asset-registration/0.1"},{"valid",valid},
        {"code",valid?"ok":"audio.invalid-asset-descriptor"},
        {"detail",valid?"Audio asset registered for asynchronous runtime resolution.":"Audio asset ID and content hash are required."},
        {"assetId",asset_id},{"contentHash",content_hash},{"storage",storage==AudioAssetStorage::stream?"stream":"resident"},
        {"state",valid?"queued":"rejected"},{"revisionBefore",before},{"revisionAfter",audio_runtime_.revision()}}.dump();
}

std::string World::set_audio_bus_json(const std::string_view bus_id, const float gain, const bool muted) {
    const auto before = audio_runtime_.revision();
    const auto success = audio_runtime_.set_bus(bus_id, gain, muted);
    return Json{{"schemaVersion", "noemancer.audio-action-receipt/0.1"}, {"success", success},
        {"code", success ? "ok" : "audio.invalid-bus-update"}, {"operation", "bus.set"}, {"busId", bus_id},
        {"revisionBefore", before}, {"revisionAfter", audio_runtime_.revision()}}.dump();
}

std::string World::play_audio_json(const std::string_view asset_id, const std::string_view bus_id, const float gain,
                                   const float pitch, const bool looping) {
    const auto before = audio_runtime_.revision();
    const auto voice_id = audio_runtime_.play(std::string(asset_id), std::string(bus_id), gain, pitch, looping);
    return Json{{"schemaVersion", "noemancer.audio-action-receipt/0.1"}, {"success", voice_id != 0},
        {"code", voice_id != 0 ? "ok" : "audio.invalid-play-request"}, {"operation", "voice.play"},
        {"voiceId", voice_id}, {"revisionBefore", before}, {"revisionAfter", audio_runtime_.revision()}}.dump();
}

std::string World::set_audio_listener_json(const Transform position,const Transform forward,const Transform up) {
    const auto before=audio_runtime_.revision();
    const auto success=audio_runtime_.set_listener({position.x,position.y,position.z},{forward.x,forward.y,forward.z},{up.x,up.y,up.z});
    return Json{{"schemaVersion","noemancer.audio-action-receipt/0.1"},{"success",success},
        {"code",success?"ok":"audio.invalid-listener"},{"operation","listener.set"},
        {"revisionBefore",before},{"revisionAfter",audio_runtime_.revision()}}.dump();
}

std::string World::set_audio_voice_spatial_json(const std::uint64_t voice_id,const bool spatial,const Transform position,
                                                 const float minimum_distance,const float maximum_distance,const float rolloff) {
    const auto before=audio_runtime_.revision();
    const auto success=audio_runtime_.set_voice_spatial(voice_id,spatial,{position.x,position.y,position.z},minimum_distance,maximum_distance,rolloff);
    return Json{{"schemaVersion","noemancer.audio-action-receipt/0.1"},{"success",success},
        {"code",success?"ok":"audio.invalid-spatial-voice"},{"operation","voice.spatial.set"},{"voiceId",voice_id},
        {"revisionBefore",before},{"revisionAfter",audio_runtime_.revision()}}.dump();
}

void World::mix_audio(const std::span<float> interleaved_stereo,const std::uint32_t sample_rate) {
    audio_runtime_.mix_stereo(interleaved_stereo,sample_rate);
}

std::string World::gameplay_observation_json(const std::size_t max_events) const {
    return gameplay_runtime_.observe_json(max_events);
}

std::string World::character_motor_2d_observation_json(const std::string_view entity_id) const {
    Json motors = Json::array();
    for (const auto& view : entity_views()) {
        if (!view.character_motor_2d || (!entity_id.empty() && view.id != entity_id)) continue;
        const auto& motor = *view.character_motor_2d;
        Json value = {{"entityId", view.id}, {"displayName", view.display_name},
            {"input", {{"move", motor.state.move_input}, {"jumpHeld", motor.state.jump_was_held}}},
            {"ground", {{"grounded", motor.state.grounded},
                {"entityId", motor.state.ground_entity_id.empty() ? Json(nullptr) : Json(motor.state.ground_entity_id)},
                {"normal",Json::array({motor.state.ground_normal_x,motor.state.ground_normal_y})},
                {"coyoteRemaining", motor.state.coyote_remaining}}},
            {"wall",{{"entityId",motor.state.wall_entity_id.empty()?Json(nullptr):Json(motor.state.wall_entity_id)},
                {"normalX",motor.state.wall_normal_x}}},
            {"decision", {{"kind", motor.state.decision}, {"reason", motor.state.reason}}},
            {"counters", {{"jumps", motor.state.jump_count}, {"landings", motor.state.landing_count}}},
            {"config", {{"maximumSpeed", motor.config.maximum_speed}, {"jumpSpeed", motor.config.jump_speed},
                {"groundAcceleration", motor.config.ground_acceleration}, {"airAcceleration", motor.config.air_acceleration},
                {"groundDeceleration", motor.config.ground_deceleration}, {"maximumFallSpeed", motor.config.maximum_fall_speed},
                {"coyoteTimeSeconds", motor.config.coyote_time_seconds}, {"jumpBufferSeconds", motor.config.jump_buffer_seconds},
                {"groundProbeDistance", motor.config.ground_probe_distance}, {"minimumGroundNormalY", motor.config.minimum_ground_normal_y},
                {"jumpReleaseVelocityFactor",motor.config.jump_release_velocity_factor}}}};
        if (view.transform) value["position"] = vector_json(semantic_vector(*view.transform));
        if (view.velocity) value["velocity"] = vector_json({view.velocity->x, view.velocity->y, view.velocity->z});
        motors.push_back(std::move(value));
    }
    return Json{{"schemaVersion", "noemancer.character-motor-2d-observation/0.1"}, {"revision", revision_},
        {"coordinateSystem", "world.right-handed.y-up; movement constrained to x/y"}, {"motors", std::move(motors)}}.dump();
}

std::string World::camera_follow_2d_observation_json(const std::string_view entity_id) const {
    Json follows=Json::array();
    for(const auto& view:entity_views()) {
        if(!view.camera_follow_2d||(!entity_id.empty()&&view.id!=entity_id))continue;
        const auto& follow=*view.camera_follow_2d;
        Json value={{"entityId",view.id},{"targetEntityId",follow.target_entity_id},{"decision",follow.decision},
            {"center",Json::array({follow.center_x,follow.center_y})},{"positionOffset",Json::array({follow.offset_x,follow.offset_y,follow.offset_z})},
            {"deadZone",Json::array({follow.dead_zone_x,follow.dead_zone_y})},{"lookAheadDistance",follow.look_ahead_distance},
            {"smoothing",follow.smoothing},{"bounds",{{"minimum",Json::array({follow.minimum_x,follow.minimum_y})},
                {"maximum",Json::array({follow.maximum_x,follow.maximum_y})}}}};
        if(view.transform)value["cameraPosition"]=vector_json(semantic_vector(*view.transform));
        if(view.camera)value["cameraTarget"]=vector_json({view.camera->target_x,view.camera->target_y,view.camera->target_z});
        follows.push_back(std::move(value));
    }
    return Json{{"schemaVersion","noemancer.camera-follow-2d-observation/0.1"},{"revision",revision_},{"follows",std::move(follows)}}.dump();
}

std::string World::gameplay_ability_catalog_json() const { return gameplay_ability_runtime_.catalog_json(); }

std::string World::gameplay_effect_catalog_json() const { return gameplay_ability_runtime_.effect_catalog_json(); }

std::string World::gameplay_ability_observation_json(const std::string_view entity_id) const {
    return gameplay_ability_runtime_.observe_json(entity_id);
}

std::string World::gameplay_ability_grant_json(const std::string_view entity_id,const std::string_view ability_id) {
    const auto before=gameplay_ability_runtime_.revision();
    const auto success=entity_ids_.contains(std::string(entity_id))&&gameplay_ability_runtime_.grant(entity_id,ability_id);
    return Json{{"schemaVersion","noemancer.ability-action-receipt/0.1"},{"success",success},
        {"code",success?"ok":"gameplay.ability.invalid-grant"},{"operation","ability.grant"},
        {"entityId",entity_id},{"abilityId",ability_id},{"revisionBefore",before},{"revisionAfter",gameplay_ability_runtime_.revision()}}.dump();
}

std::string World::gameplay_ability_activate_json(const std::string_view entity_id,const std::string_view ability_id,
                                                   const std::string_view target_id) {
    if(!entity_ids_.contains(std::string(entity_id))||(!target_id.empty()&&!entity_ids_.contains(std::string(target_id))))
        return Json{{"schemaVersion","noemancer.ability-activation/0.2"},{"success",false},{"code","gameplay.ability.entity-not-found"},
            {"entityId",entity_id},{"abilityId",ability_id},{"targetId",target_id},
            {"revisionBefore",gameplay_ability_runtime_.revision()},{"revisionAfter",gameplay_ability_runtime_.revision()}}.dump();
    return gameplay_ability_runtime_.activate_json(entity_id,ability_id,target_id,gameplay_runtime_);
}

std::string World::gameplay_effect_apply_json(const std::string_view source_entity_id,
                                              const std::string_view target_entity_id,
                                              const std::string_view effect_id) {
    if (!entity_ids_.contains(std::string(source_entity_id)) || !entity_ids_.contains(std::string(target_entity_id))) {
        return Json{{"schemaVersion", "noemancer.gameplay-effect-receipt/0.1"}, {"success", false},
            {"code", "gameplay.effect.entity-not-found"}, {"effectId", effect_id},
            {"sourceEntityId", source_entity_id}, {"targetEntityId", target_entity_id},
            {"revisionBefore", gameplay_ability_runtime_.revision()},
            {"revisionAfter", gameplay_ability_runtime_.revision()}}.dump();
    }
    return gameplay_ability_runtime_.apply_effect_json(source_entity_id, target_entity_id, effect_id, gameplay_runtime_);
}

std::string World::gameplay_ability_activate_ray_json(const std::string_view entity_id,
                                                      const std::string_view ability_id,
                                                      const Transform origin,
                                                      const Transform direction) {
    if (!entity_ids_.contains(std::string(entity_id))) {
        return Json{{"schemaVersion", "noemancer.ability-ray-activation/0.1"}, {"success", false},
            {"code", "gameplay.ability.entity-not-found"}, {"entityId", entity_id},
            {"abilityId", ability_id}, {"hit", nullptr}, {"activation", nullptr}}.dump();
    }
    const auto hit = physics_runtime_.ray_cast(origin.x, origin.y, origin.z, direction.x, direction.y, direction.z);
    Json hit_json = nullptr;
    if (hit.hit) hit_json = {{"entityId", hit.entity_id}, {"fraction", hit.fraction},
        {"position", vector_json({hit.position_x, hit.position_y, hit.position_z})}};
    if (!hit.hit) {
        return Json{{"schemaVersion", "noemancer.ability-ray-activation/0.1"}, {"success", false},
            {"code", "gameplay.ability.ray-missed"}, {"entityId", entity_id}, {"abilityId", ability_id},
            {"origin", vector_json({origin.x, origin.y, origin.z})},
            {"direction", vector_json({direction.x, direction.y, direction.z})},
            {"hit", nullptr}, {"activation", nullptr}}.dump();
    }
    const auto activation = Json::parse(gameplay_ability_activate_json(entity_id, ability_id, hit.entity_id));
    return Json{{"schemaVersion", "noemancer.ability-ray-activation/0.1"},
        {"success", activation.value("success", false)}, {"code", activation.value("code", "gameplay.ability.failed")},
        {"entityId", entity_id}, {"abilityId", ability_id},
        {"origin", vector_json({origin.x, origin.y, origin.z})},
        {"direction", vector_json({direction.x, direction.y, direction.z})},
        {"hit", std::move(hit_json)}, {"activation", activation}}.dump();
}

std::string World::gameplay_ability_activate_sweep_json(const std::string_view entity_id,
                                                        const std::string_view ability_id,
                                                        const Transform origin,const Transform direction,
                                                        const float radius) {
    if (!entity_ids_.contains(std::string(entity_id)))
        return Json{{"schemaVersion","noemancer.ability-sweep-activation/0.1"},{"success",false},
            {"code","gameplay.ability.entity-not-found"},{"entityId",entity_id},{"abilityId",ability_id},
            {"shape",{{"type","sphere"},{"radius",radius}}},{"hit",nullptr},{"activation",nullptr}}.dump();
    const auto hit=physics_runtime_.sphere_sweep(origin.x,origin.y,origin.z,direction.x,direction.y,direction.z,radius,entity_id);
    Json hit_json=nullptr;
    if (hit.hit) hit_json={{"entityId",hit.entity_id},{"fraction",hit.fraction},
        {"position",vector_json({hit.position_x,hit.position_y,hit.position_z})},
        {"normal",vector_json({hit.normal_x,hit.normal_y,hit.normal_z})},{"penetrationDepth",hit.penetration_depth}};
    if (!hit.hit) return Json{{"schemaVersion","noemancer.ability-sweep-activation/0.1"},{"success",false},
        {"code","gameplay.ability.sweep-missed"},{"entityId",entity_id},{"abilityId",ability_id},
        {"shape",{{"type","sphere"},{"radius",radius}}},{"origin",vector_json({origin.x,origin.y,origin.z})},
        {"direction",vector_json({direction.x,direction.y,direction.z})},{"hit",nullptr},{"activation",nullptr}}.dump();
    const auto activation=Json::parse(gameplay_ability_activate_json(entity_id,ability_id,hit.entity_id));
    return Json{{"schemaVersion","noemancer.ability-sweep-activation/0.1"},
        {"success",activation.value("success",false)},{"code",activation.value("code","gameplay.ability.failed")},
        {"entityId",entity_id},{"abilityId",ability_id},{"shape",{{"type","sphere"},{"radius",radius}}},
        {"origin",vector_json({origin.x,origin.y,origin.z})},{"direction",vector_json({direction.x,direction.y,direction.z})},
        {"hit",std::move(hit_json)},{"activation",activation}}.dump();
}

std::string World::vfx_graph_json(const std::string_view graph_id) const {
    return vfx_runtime_.graph_json(graph_id);
}

std::string World::vfx_gpu_program_json(const std::string_view graph_id) const {
    return vfx_runtime_.gpu_program_json(graph_id);
}

std::string World::vfx_preview_json(const std::string_view graph_id, const std::uint64_t seed,
                                    const std::uint32_t steps, const float fixed_delta_seconds,
                                    const std::size_t max_particles) const {
    return vfx_runtime_.preview_json(graph_id, seed, steps, fixed_delta_seconds, max_particles);
}

std::string World::vfx_observation_json(const std::size_t max_particles) const {
    return vfx_runtime_.observe_json(max_particles);
}

std::string World::vfx_benchmark_json(const std::string_view graph_id,const std::uint32_t particle_count,
                                      const std::uint32_t steps,const float fixed_delta_seconds) const {
    return vfx_runtime_.benchmark_json(graph_id,particle_count,steps,fixed_delta_seconds);
}

std::string World::vfx_spawn_json(const std::string_view graph_id, const Transform position, const std::uint64_t seed) {
    const auto alive_before=vfx_runtime_.particle_count();
    const auto success=vfx_runtime_.spawn(graph_id,{position.x,position.y,position.z},seed,seed);
    const auto alive_after=vfx_runtime_.particle_count();
    return Json{{"schemaVersion","noemancer.vfx-spawn-receipt/0.1"},{"success",success},
        {"code",success?"ok":"vfx.graph.not-found"},{"graphId",graph_id},{"seed",seed},
        {"position",vector_json({position.x,position.y,position.z})},{"aliveBefore",alive_before},
        {"aliveAfter",alive_after},{"spawned",alive_after-alive_before}}.dump();
}

std::string World::vfx_plan_graph_patch_json(const std::string_view graph_id, const std::string_view patch_json,
                                             const std::uint64_t base_revision) const {
    return vfx_runtime_.plan_graph_patch_json(graph_id, patch_json, base_revision);
}

std::string World::vfx_apply_graph_plan_json(const std::string_view plan_json, const bool dry_run) {
    return vfx_runtime_.apply_graph_plan_json(plan_json, dry_run);
}

std::string World::vfx_undo_graph_json(const std::uint64_t expected_revision) {
    return vfx_runtime_.undo_graph_json(expected_revision);
}

std::string World::network_snapshot_preview_json(const std::uint64_t tick, const std::size_t max_entities) const {
    const NetworkReplicationRuntime server("server.preview", true);
    auto views = entity_views();
    const auto total = views.size();
    if (views.size() > max_entities) views.resize(max_entities);
    auto snapshot = Json::parse(server.capture_snapshot_json(tick, revision_, views));
    snapshot["totalWorldEntityCount"] = total;
    snapshot["truncated"] = views.size() < total;
    return snapshot.dump();
}

std::string World::network_loopback_verify_json() const {
    using NetworkJson = nlohmann::json;
    NetworkReplicationRuntime server("server.loopback", true);
    NetworkReplicationRuntime client("client.loopback", false);
    const auto baseline = server.capture_snapshot_json(100, revision_, entity_views());
    const auto baseline_receipt = NetworkJson::parse(client.apply_snapshot_json(baseline));
    auto target_views = entity_views();
    std::string predicted_net_entity;
    for (auto& view : target_views) {
        if (!view.transform) continue;
        predicted_net_entity = NetworkReplicationRuntime::stable_net_entity_id(view.id);
        view.transform->x += 0.75F;
        view.revision += 1;
        break;
    }
    const auto target = server.capture_snapshot_json(101, revision_ + 1, target_views);
    const auto delta = NetworkReplicationRuntime::delta_json(baseline, target);
    const auto delta_receipt = NetworkJson::parse(client.apply_delta_json(delta));
    NetworkReplicationRuntime reference("client.reference", false);
    const auto reference_receipt = NetworkJson::parse(reference.apply_snapshot_json(target));
    const auto client_after_delta = NetworkJson::parse(client.observe_json(0));
    const auto reference_state = NetworkJson::parse(reference.observe_json(0));
    const bool converged = baseline_receipt.value("success", false) && delta_receipt.value("success", false) &&
        reference_receipt.value("success", false) && client_after_delta.at("digest") == reference_state.at("digest");
    bool predicted = false;
    NetworkJson reconciliation = nullptr;
    if (!predicted_net_entity.empty()) {
        predicted = client.predict_position(predicted_net_entity, 1, {0.25, 0.0, 0.0});
        reconciliation = NetworkJson::parse(client.reconcile_json(target, 1));
    }
    const auto delta_document = NetworkJson::parse(delta);
    return NetworkJson{{"schemaVersion", "noemancer.network-loopback-verification/0.1"},
        {"valid", converged && predicted && reconciliation.value("success", false)},
        {"code", converged && predicted && reconciliation.value("success", false) ? "ok" : "network.loopback.failed"},
        {"serverPeerId", "server.loopback"}, {"clientPeerId", "client.loopback"},
        {"baselineTick", 100}, {"targetTick", 101}, {"snapshotEntityCount", NetworkJson::parse(target).at("entityCount")},
        {"delta", {{"added", delta_document.at("added").size()}, {"changed", delta_document.at("changed").size()},
            {"removed", delta_document.at("removed").size()}}}, {"converged", converged},
        {"predictionAccepted", predicted}, {"reconciliation", std::move(reconciliation)}}.dump();
}

std::string World::spawn_prefab_json(const std::string_view source_entity_id, const std::string_view new_entity_id,
                                     const std::string_view display_name, const Transform position) {
    const auto revision_before = revision_;
    if (new_entity_id.empty() || entity_ids_.contains(std::string(new_entity_id)))
        return Json{{"schemaVersion","noemancer.gameplay-action-receipt/0.1"},{"success",false},{"code","gameplay.entity-id-conflict"},
            {"operation","prefab.spawn"},{"revisionBefore",revision_before},{"revisionAfter",revision_}}.dump();
    const auto source = std::ranges::find(scene_document_.entities, source_entity_id, &SceneEntityDocument::guid);
    if (source == scene_document_.entities.end())
        return Json{{"schemaVersion","noemancer.gameplay-action-receipt/0.1"},{"success",false},{"code","gameplay.prefab-source-not-found"},
            {"operation","prefab.spawn"},{"revisionBefore",revision_before},{"revisionAfter",revision_}}.dump();
    auto candidate = runtime_authoring_scene_document();
    auto instance = *source;
    instance.guid = std::string(new_entity_id);
    instance.name = display_name.empty() ? source->name + " Instance" : std::string(display_name);
    instance.parent_guid.clear();
    if(instance.managed_script)instance.managed_script->instance_id="script."+instance.guid;
    if (!instance.transform) instance.transform = SceneTransform{};
    instance.transform->position = {position.x, position.y, position.z};
    candidate.entities.push_back(std::move(instance));
    const auto result = load_scene_internal(candidate,false);
    if (!result.success)
        return Json{{"schemaVersion","noemancer.gameplay-action-receipt/0.1"},{"success",false},{"code","gameplay.spawn-validation-failed"},
            {"operation","prefab.spawn"},{"revisionBefore",revision_before},{"revisionAfter",revision_}}.dump();
    gameplay_runtime_.emit("entity.spawned", std::string(source_entity_id), std::string(new_entity_id),
        Json{{"position",vector_json({position.x,position.y,position.z})}}.dump());
    return Json{{"schemaVersion","noemancer.gameplay-action-receipt/0.1"},{"success",true},{"code","ok"},{"operation","prefab.spawn"},
        {"entityId",new_entity_id},{"sourceEntityId",source_entity_id},{"revisionBefore",revision_before},{"revisionAfter",revision_}}.dump();
}

std::string World::export_prefab_json(const std::string_view entity_id) const {
    const auto scene=Json::parse(canonical_scene_json());
    const auto found=std::ranges::find_if(scene.at("entities"),[&](const Json& entity){return entity.at("guid").get<std::string>()==entity_id;});
    if(found==scene.at("entities").end()) return Json{{"schemaVersion","noemancer.prefab/0.1"},{"valid",false},
        {"code","world.entity-not-found"},{"sourceSceneGuid",scene_guid_},{"entity",nullptr}}.dump();
    auto entity=*found; entity["parent"]=nullptr;
    return Json{{"schemaVersion","noemancer.prefab/0.1"},{"valid",true},{"code","ok"},{"sourceSceneGuid",scene_guid_},
        {"sourceEntityId",entity_id},{"entity",std::move(entity)}}.dump();
}

std::string World::instantiate_prefab_json(const std::string_view prefab_json,const std::string_view new_entity_id,
                                           const std::string_view display_name,const Transform position) {
    const auto before=revision_;
    auto prefab=Json::parse(prefab_json,nullptr,false);
    if(prefab.is_discarded()||!prefab.is_object()||prefab.value("schemaVersion",std::string{})!="noemancer.prefab/0.1"||
        !prefab.value("valid",false)||!prefab.contains("entity")||!prefab.at("entity").is_object()||new_entity_id.empty()||entity_ids_.contains(std::string(new_entity_id)))
        return Json{{"schemaVersion","noemancer.gameplay-action-receipt/0.1"},{"success",false},{"code","gameplay.invalid-prefab"},
            {"operation","prefab.instantiate"},{"revisionBefore",before},{"revisionAfter",revision_}}.dump();
    auto entity=prefab.at("entity"); entity["guid"]=new_entity_id; entity["parent"]=nullptr;
    if(!display_name.empty()) entity["name"]=display_name;
    if(entity.contains("components")&&entity.at("components").is_object()&&
       entity.at("components").contains("ManagedScript")&&entity.at("components").at("ManagedScript").is_object())
        entity["components"]["ManagedScript"]["instanceId"]="script."+std::string(new_entity_id);
    entity["components"]["Transform"]["position"]=Json::array({position.x,position.y,position.z});
    const Json wrapper={{"schema","noemancer.scene/0.1"},{"sceneGuid","scene.prefab-staging"},{"name","Prefab Staging"},
        {"entities",Json::array({entity})}};
    const auto parsed=SceneDocumentCodec::parse_json(wrapper.dump(),"prefab://instantiate");
    if(!parsed) return Json{{"schemaVersion","noemancer.gameplay-action-receipt/0.1"},{"success",false},{"code","gameplay.invalid-prefab"},
        {"operation","prefab.instantiate"},{"revisionBefore",before},{"revisionAfter",revision_}}.dump();
    auto candidate=runtime_authoring_scene_document(); candidate.entities.push_back(parsed.document->entities.front());
    const auto loaded=load_scene_internal(candidate,false);
    if(loaded.success) gameplay_runtime_.emit("entity.spawned",prefab.value("sourceEntityId",std::string("prefab.document")),std::string(new_entity_id),
        Json{{"prefabSchema","noemancer.prefab/0.1"}}.dump());
    return Json{{"schemaVersion","noemancer.gameplay-action-receipt/0.1"},{"success",loaded.success},{"code",loaded.success?"ok":"gameplay.prefab-validation-failed"},
        {"operation","prefab.instantiate"},{"entityId",new_entity_id},{"revisionBefore",before},{"revisionAfter",revision_}}.dump();
}

std::string World::despawn_entity_json(const std::string_view entity_id) {
    const auto revision_before = revision_;
    auto candidate = runtime_authoring_scene_document();
    const auto found = std::ranges::find(candidate.entities, entity_id, &SceneEntityDocument::guid);
    if (found == candidate.entities.end())
        return Json{{"schemaVersion","noemancer.gameplay-action-receipt/0.1"},{"success",false},{"code","gameplay.entity-not-found"},
            {"operation","entity.despawn"},{"revisionBefore",revision_before},{"revisionAfter",revision_}}.dump();
    candidate.entities.erase(found);
    const auto result = load_scene_internal(candidate,false);
    if (!result.success)
        return Json{{"schemaVersion","noemancer.gameplay-action-receipt/0.1"},{"success",false},{"code","gameplay.entity-has-dependents"},
            {"operation","entity.despawn"},{"revisionBefore",revision_before},{"revisionAfter",revision_}}.dump();
    scripting_runtime_.release_entity_instances(entity_id);
    gameplay_ability_runtime_.forget_entity(entity_id);
    gameplay_runtime_.emit("entity.despawned", std::string(entity_id), "gameplay.world");
    return Json{{"schemaVersion","noemancer.gameplay-action-receipt/0.1"},{"success",true},{"code","ok"},{"operation","entity.despawn"},
        {"entityId",entity_id},{"revisionBefore",revision_before},{"revisionAfter",revision_}}.dump();
}

std::string World::save_capture_json() const {
    const auto document=Json::parse(runtime_authoring_scene_json());
    const auto script_state=Json::parse(scripting_runtime_.state_capture_json(),nullptr,false);
    return Json{{"schemaVersion","noemancer.save-game/0.2"},{"worldRevision",revision_},{"simulationTick",simulation_tick_},
        {"sceneGuid",scene_guid_},{"document",document},{"scriptState",script_state}}.dump();
}

std::string World::save_restore_json(const std::string_view document_json) {
    const auto before = revision_;
    auto save=Json::parse(document_json,nullptr,false);Json scene=save;Json script_state=nullptr;
    if(save.is_object()&&save.value("schemaVersion",std::string{})=="noemancer.save-game/0.2") {
        if(!save.contains("document")||!save.at("document").is_object()||!save.contains("scriptState")||!save.at("scriptState").is_object())
            return Json{{"schemaVersion","noemancer.gameplay-action-receipt/0.1"},{"success",false},{"code","gameplay.invalid-save"},
                {"operation","save.restore"},{"revisionBefore",before},{"revisionAfter",revision_}}.dump();
        scene=save.at("document");script_state=save.at("scriptState");
    }
    const auto parsed = SceneDocumentCodec::parse_json(scene.dump(), "save://agent-restore");
    if (!parsed)
        return Json{{"schemaVersion","noemancer.gameplay-action-receipt/0.1"},{"success",false},{"code","gameplay.invalid-save"},
            {"operation","save.restore"},{"revisionBefore",before},{"revisionAfter",revision_},{"errorCount",parsed.errors.size()}}.dump();
    const auto result = load_scene(*parsed.document);
    auto script_receipt=Json{{"success",true},{"code","legacy-scene-save"}};
    if(result.success&&!script_state.is_null())script_receipt=Json::parse(scripting_runtime_.state_restore_json(script_state.dump()),nullptr,false);
    const auto success=result.success&&script_receipt.value("success",false);
    if(success&&save.is_object()&&save.value("schemaVersion",std::string{})=="noemancer.save-game/0.2")
        simulation_tick_=save.value("simulationTick",simulation_tick_);
    if(success)gameplay_runtime_.emit("save.restored","gameplay.save","gameplay.world");
    return Json{{"schemaVersion","noemancer.gameplay-action-receipt/0.1"},{"success",success},
        {"code",success?"ok":result.success?"gameplay.script-state-restore-failed":"gameplay.invalid-save"},{"operation","save.restore"},
        {"scriptState",std::move(script_receipt)},{"revisionBefore",before},{"revisionAfter",revision_}}.dump();
}

std::string World::replay_start_json() {
    replay_recording_=true;recorded_inputs_.clear();next_recorded_input_sequence_=1;replay_start_tick_=simulation_tick_;
    replay_initial_save_json_=save_capture_json();
    return Json{{"schemaVersion","noemancer.replay-action-receipt/0.1"},{"success",true},{"code","ok"},{"recording",true}}.dump();
}

std::string World::replay_stop_json() {
    replay_recording_ = false;
    Json samples=Json::array();
    for(const auto& sample:recorded_inputs_)samples.push_back({{"sequence",sample.sequence},{"tick",sample.tick},{"source",sample.source},{"value",sample.value}});
    return Json{{"schemaVersion","noemancer.input-replay/0.2"},{"recording",false},{"fixedDeltaSeconds",1.0F/60.0F},
        {"initialSave",Json::parse(replay_initial_save_json_,nullptr,false)},{"sampleCount",samples.size()},{"samples",std::move(samples)}}.dump();
}

std::string World::replay_apply_json(const std::string_view replay_json) {
    Json replay=Json::parse(replay_json,nullptr,false);
    if(replay.is_discarded()||!replay.is_object()||replay.value("schemaVersion",std::string{})!="noemancer.input-replay/0.2"||
       !replay.contains("initialSave")||!replay.at("initialSave").is_object()||!replay.contains("samples")||
       !replay.at("samples").is_array()||replay.at("samples").size()>65536U)
        return Json{{"schemaVersion","noemancer.replay-action-receipt/0.1"},{"success",false},{"code","gameplay.invalid-replay"},{"appliedSamples",0}}.dump();
    const auto restored=Json::parse(save_restore_json(replay.at("initialSave").dump()),nullptr,false);
    if(!restored.value("success",false))return Json{{"schemaVersion","noemancer.replay-action-receipt/0.1"},
        {"success",false},{"code","gameplay.replay-initial-state-invalid"},{"appliedSamples",0}}.dump();
    std::vector<RecordedInput> samples;std::uint64_t maximum_tick{};
    for(const auto& sample:replay.at("samples")) {
        if(!sample.is_object()||!sample.contains("sequence")||!sample.at("sequence").is_number_unsigned()||
           !sample.contains("tick")||!sample.at("tick").is_number_unsigned()||!sample.contains("source")||
           !sample.at("source").is_string()||!sample.contains("value")||!sample.at("value").is_number())
            return Json{{"schemaVersion","noemancer.replay-action-receipt/0.1"},{"success",false},
                {"code","gameplay.invalid-replay-sample"},{"appliedSamples",0}}.dump();
        const auto tick=sample.at("tick").get<std::uint64_t>();if(tick>6000U)return Json{{"schemaVersion","noemancer.replay-action-receipt/0.1"},
            {"success",false},{"code","gameplay.replay-too-long"},{"appliedSamples",0}}.dump();
        samples.push_back({sample.at("sequence").get<std::uint64_t>(),tick,sample.at("source").get<std::string>(),sample.at("value").get<float>()});
        maximum_tick=std::max(maximum_tick,tick);
    }
    std::ranges::sort(samples,[](const RecordedInput& left,const RecordedInput& right){
        return left.tick!=right.tick?left.tick<right.tick:left.sequence<right.sequence;});
    std::size_t applied{},cursor{};
    const auto was_recording=replay_recording_; replay_recording_=false;
    const auto simulated_ticks=samples.empty()?0U:maximum_tick+1U;
    for(std::uint64_t tick_index=0;tick_index<simulated_ticks;++tick_index) {
        while(cursor<samples.size()&&samples[cursor].tick==tick_index) {
            if(input_runtime_.set_source_value(samples[cursor].source,samples[cursor].value))++applied;
            ++cursor;
        }
        tick(1.0F/60.0F);
    }
    replay_recording_=was_recording;
    gameplay_runtime_.emit("replay.applied","gameplay.replay","gameplay.world",Json{{"sampleCount",applied}}.dump());
    return Json{{"schemaVersion","noemancer.replay-action-receipt/0.1"},{"success",true},{"code","ok"},
        {"appliedSamples",applied},{"simulatedTicks",simulated_ticks}}.dump();
}

std::string World::scripting_abi_json() const { return scripting_runtime_.abi_json(); }
std::string World::scripting_observation_json() const { return scripting_runtime_.observe_json(); }
std::string World::scripting_attach_json(const std::string_view instance_id,const std::string_view entity_id,
                                         const std::string_view assembly_asset,const std::string_view type_name) {
    if(!entity_ids_.contains(std::string(entity_id))) return Json{{"schemaVersion","noemancer.script-action-receipt/0.1"},
        {"success",false},{"code","world.entity-not-found"},{"operation","instance.attach"},{"revision",revision_}}.dump();
    return scripting_runtime_.attach_json(instance_id,entity_id,assembly_asset,type_name);
}
std::string World::scripting_invoke_json(const std::string_view instance_id,const std::string_view callback,const std::string_view arguments_json) {
    const auto script_entity=scripting_runtime_.instance_entity_id(instance_id);
    Json self=nullptr;
    if(script_entity) {
        const auto views=entity_views();
        const auto found=std::ranges::find(views,*script_entity,&WorldEntityView::id);
        if(found!=views.end()) {
            self={{"id",found->id},{"name",found->display_name},{"type",found->type},{"parentId",found->parent_guid}};
            if(found->transform) self["transform"]={{"position",{{"x",found->transform->x},{"y",found->transform->y},{"z",found->transform->z}}},
                {"rotationQuaternion",{{"x",found->transform->rotation_x},{"y",found->transform->rotation_y},
                    {"z",found->transform->rotation_z},{"w",found->transform->rotation_w}}},
                {"scale",{{"x",found->transform->scale_x},{"y",found->transform->scale_y},{"z",found->transform->scale_z}}}};
            if(found->velocity) self["velocity"]={{"x",found->velocity->x},{"y",found->velocity->y},{"z",found->velocity->z}};
            if(found->character_motor_2d) {const auto& state=found->character_motor_2d->state;
                self["characterMotor2D"]={{"grounded",state.grounded},{"moveInput",state.move_input},
                    {"decision",state.decision},{"reason",state.reason},{"groundEntityId",state.ground_entity_id},
                    {"wallEntityId",state.wall_entity_id},{"jumpCount",state.jump_count},{"landingCount",state.landing_count}};}
            if(found->sprite_renderer) {const auto& playback=found->sprite_renderer->playback;
                self["spritePlayback"]={{"assetId",playback.asset_id},{"clipId",playback.clip_id},
                    {"frameIndex",playback.frame_index},{"playing",playback.playing},{"completedLoops",playback.completed_loops}};}
        }
    }
    auto input=Json::parse(input_observation_json(),nullptr,false);
    if(input.is_discarded()) input=Json::object();
    const auto script_views=entity_views();
    Json visible_entities=Json::array();
    constexpr std::size_t script_entity_limit=256;
    for(std::size_t index=0;index<std::min(script_views.size(),script_entity_limit);++index) {
        const auto& view=script_views[index];
        Json components=Json::array();
        if(view.transform)components.push_back("engine.component.Transform");
        if(view.velocity)components.push_back("engine.component.Velocity");
        if(view.rigid_body)components.push_back("engine.component.RigidBody");
        if(view.box_collider)components.push_back("engine.component.BoxCollider");
        if(view.sphere_collider)components.push_back("engine.component.SphereCollider");
        if(view.capsule_collider)components.push_back("engine.component.CapsuleCollider");
        if(view.character_motor_2d)components.push_back("engine.component.CharacterMotor2D");
        if(view.platform_2d)components.push_back("engine.component.Platform2D");
        if(view.animation_player)components.push_back("engine.component.AnimationPlayer");
        if(view.camera)components.push_back("engine.component.Camera");
        if(view.directional_light)components.push_back("engine.component.DirectionalLight");
        if(view.local_light)components.push_back("engine.component.LocalLight");
        if(view.mesh_renderer)components.push_back("engine.component.MeshRenderer");
        if(view.sprite_renderer)components.push_back("engine.component.SpriteRenderer");
        if(view.tilemap_renderer)components.push_back("engine.component.TilemapRenderer");
        if(view.pbr_material)components.push_back("engine.component.PbrMaterial");
        Json item={{"id",view.id},{"name",view.display_name},{"type",view.type},
            {"parentId",view.parent_guid.empty()?Json(nullptr):Json(view.parent_guid)},{"components",std::move(components)},
            {"tags",gameplay_ability_runtime_.tags(view.id)}};
        if(view.transform)item["position"]={{"x",view.transform->x},{"y",view.transform->y},{"z",view.transform->z}};
        visible_entities.push_back(std::move(item));
    }
    Json visible_events=Json::array();
    const auto all_events=gameplay_runtime_.events();
    const auto event_begin=all_events.size()>64U?all_events.size()-64U:0U;
    for(std::size_t index=event_begin;index<all_events.size();++index) {
        const auto& event=all_events[index];
        auto payload=Json::parse(event.payload_json,nullptr,false);if(payload.is_discarded())payload=Json::object();
        visible_events.push_back({{"sequence",event.sequence},{"type",event.type},{"source",event.source},
            {"target",event.target},{"payload",std::move(payload)}});
    }
    const auto context=Json{{"revision",revision_},{"self",std::move(self)},{"input",std::move(input)},
        {"entities",{{"schemaVersion","noemancer.script-entity-query/0.1"},{"items",std::move(visible_entities)},
            {"total",script_views.size()},{"truncated",script_views.size()>script_entity_limit}}},
        {"events",{{"schemaVersion","noemancer.script-event-query/0.1"},{"items",std::move(visible_events)},
            {"truncated",all_events.size()>64U}}}};
    auto receipt=Json::parse(scripting_runtime_.invoke_json(instance_id,callback,arguments_json,context.dump()),nullptr,false);
    if(receipt.is_discarded()||!receipt.is_object()) return Json{{"schemaVersion","noemancer.script-action-receipt/0.2"},
        {"success",false},{"code","scripting.invalid-host-receipt"},{"operation","lifecycle.invoke"}}.dump();
    if(!receipt.value("success",false)||!receipt.contains("managedResult")||!receipt.at("managedResult").is_object()) return receipt.dump();
    const auto commands=receipt.at("managedResult").value("commands",Json::array());
    Json errors=Json::array();
    std::unordered_set<std::string> reserved_spawn_ids;
    std::size_t pending_persistence_requests{};
    auto preflight_scene=scene_document_;
    if(!commands.is_array()||commands.size()>64U) errors.push_back({{"code","scripting.invalid-command-count"},{"index",0}});
    else for(std::size_t index=0;index<commands.size();++index) {
        const auto& command=commands.at(index);
        if(!command.is_object()||!command.contains("operation")||!command.at("operation").is_string()||
           !command.contains("entityId")||!command.at("entityId").is_string()||!command.contains("payload")||!command.at("payload").is_object()) {
            errors.push_back({{"code","scripting.invalid-command-shape"},{"index",index}}); continue;
        }
        const auto operation=command.at("operation").get<std::string>();
        const auto entity=command.at("entityId").get<std::string>();
        if(operation!="audio.voice.play"&&operation!="gameplay.persistence.request"&&!entity_ids_.contains(entity)) errors.push_back({{"code","world.entity-not-found"},{"index",index},{"entityId",entity}});
        else if(operation=="scene.transform.set-position") {
            const auto& payload=command.at("payload");
            if(!payload.contains("x")||!payload.at("x").is_number()||!payload.contains("y")||!payload.at("y").is_number()||
               !payload.contains("z")||!payload.at("z").is_number()) errors.push_back({{"code","scripting.invalid-position"},{"index",index}});
            else if(!world_.entity(entity_ids_.at(entity)).has<Transform>())
                errors.push_back({{"code","world.component-not-found"},{"index",index},{"component","Transform"}});
        } else if(operation=="gameplay.event.emit") {
            const auto& payload=command.at("payload");
            if(!payload.contains("eventType")||!payload.at("eventType").is_string()||payload.at("eventType").get<std::string>().empty())
                errors.push_back({{"code","scripting.invalid-event"},{"index",index}});
        } else if(operation=="scene.property.set") {
            const auto& payload=command.at("payload");
            if(!payload.contains("propertyId")||!payload.at("propertyId").is_string()||
               !payload.contains("value")) errors.push_back({{"code","scripting.invalid-property-command"},{"index",index}});
            else {
                const auto plan=plan_property_update(entity,payload.at("propertyId").get<std::string>(),payload.at("value").dump(),
                    revision_,"scripting:"+std::string(instance_id));
                if(!plan.valid)errors.push_back({{"code",plan.code},{"detail",plan.detail},{"index",index}});
            }
        } else if(operation=="sprite.playback.set") {
            const auto& payload=command.at("payload");
            const auto shape_valid=payload.contains("clipId")&&payload.at("clipId").is_string()&&
                payload.contains("playing")&&payload.at("playing").is_boolean()&&
                payload.contains("playbackSpeed")&&payload.at("playbackSpeed").is_number()&&
                payload.contains("flipX")&&payload.at("flipX").is_boolean()&&
                payload.contains("restart")&&payload.at("restart").is_boolean();
            if(!shape_valid)errors.push_back({{"code","scripting.invalid-sprite-playback"},{"index",index}});
            else {
                const auto speed=payload.at("playbackSpeed").get<float>();const auto clip=payload.at("clipId").get<std::string>();
                const auto* renderer=world_.entity(entity_ids_.at(entity)).try_get<SpriteRenderer>();
                const auto* asset=renderer?sprite_assets_.find(renderer->playback.asset_id):nullptr;
                if(renderer==nullptr)errors.push_back({{"code","sprite.renderer-unavailable"},{"index",index}});
                else if(clip.empty()||!std::isfinite(speed)||speed<0.0F||speed>16.0F)
                    errors.push_back({{"code","sprite.invalid-playback-request"},{"index",index}});
                else if(asset==nullptr)errors.push_back({{"code","sprite.asset-not-found"},{"index",index}});
                else if(std::ranges::none_of(asset->clips,[&](const SpriteClip& candidate){return candidate.id==clip;}))
                    errors.push_back({{"code","sprite.clip-not-found"},{"index",index}});
            }
        } else if(operation=="audio.voice.play") {
            const auto& payload=command.at("payload");
            const auto shape_valid=payload.contains("assetId")&&payload.at("assetId").is_string()&&
                payload.contains("busId")&&payload.at("busId").is_string()&&
                payload.contains("gain")&&payload.at("gain").is_number()&&
                payload.contains("pitch")&&payload.at("pitch").is_number()&&
                payload.contains("looping")&&payload.at("looping").is_boolean();
            if(!shape_valid)errors.push_back({{"code","scripting.invalid-audio-play"},{"index",index}});
            else {const auto gain=payload.at("gain").get<float>(),pitch=payload.at("pitch").get<float>();
                if(payload.at("assetId").get<std::string>().empty()||payload.at("busId").get<std::string>().empty()||
                   !std::isfinite(gain)||gain<0.0F||gain>4.0F||!std::isfinite(pitch)||pitch<=0.0F||pitch>4.0F)
                    errors.push_back({{"code","audio.invalid-play-request"},{"index",index}});}
        } else if(operation=="gameplay.persistence.request") {
            const auto& payload=command.at("payload");
            const auto shape_valid=payload.contains("action")&&payload.at("action").is_string()&&
                payload.contains("slotId")&&payload.at("slotId").is_string();
            if(!shape_valid)errors.push_back({{"code","scripting.invalid-persistence-request"},{"index",index}});
            else {
                const auto action=payload.at("action").get<std::string>();
                const auto slot=payload.at("slotId").get<std::string>();
                const auto known=action=="save"||action=="load"||action=="replay-start"||
                    action=="replay-stop"||action=="replay-play";
                const auto slot_required=action!="replay-start";
                const auto valid_slot=!slot.empty()&&slot.size()<=64U&&std::ranges::all_of(slot,[](const unsigned char character){
                    return (character>='a'&&character<='z')||(character>='A'&&character<='Z')||
                        (character>='0'&&character<='9')||character=='.'||character=='_'||character=='-';});
                if(!known||(slot_required&&!valid_slot)||(!slot_required&&!slot.empty()))
                    errors.push_back({{"code","gameplay.persistence-request-invalid"},{"index",index}});
                else if(persistence_requests_.size()+pending_persistence_requests>=16U)
                    errors.push_back({{"code","gameplay.persistence-queue-full"},{"index",index}});
                else ++pending_persistence_requests;
            }
        } else if(operation=="gameplay.prefab.spawn") {
            const auto& payload=command.at("payload");
            const auto shape_valid=payload.contains("newEntityId")&&payload.at("newEntityId").is_string()&&
                payload.contains("displayName")&&payload.at("displayName").is_string()&&payload.contains("x")&&payload.at("x").is_number()&&
                payload.contains("y")&&payload.at("y").is_number()&&payload.contains("z")&&payload.at("z").is_number();
            if(!shape_valid)errors.push_back({{"code","scripting.invalid-prefab-spawn"},{"index",index}});
            else {
                const auto new_id=payload.at("newEntityId").get<std::string>();
                if(new_id.empty()||entity_ids_.contains(new_id)||!reserved_spawn_ids.insert(new_id).second)
                    errors.push_back({{"code","gameplay.entity-id-conflict"},{"index",index},{"entityId",new_id}});
            }
        } else if(operation=="gameplay.entity.despawn") {
            auto candidate=preflight_scene;
            const auto removed=std::ranges::find(candidate.entities,entity,&SceneEntityDocument::guid);
            if(removed==candidate.entities.end())errors.push_back({{"code","gameplay.entity-not-found"},{"index",index},{"entityId",entity}});
            else {
                candidate.entities.erase(removed);
                if(!SceneDocumentCodec::validate(candidate).empty())
                    errors.push_back({{"code","gameplay.entity-has-dependents"},{"index",index},{"entityId",entity}});
                else preflight_scene=std::move(candidate);
            }
        } else if(operation=="gameplay.tag.set") {
            const auto& payload=command.at("payload");
            if(!payload.contains("tag")||!payload.at("tag").is_string()||
               !payload.contains("present")||!payload.at("present").is_boolean()||
               !valid_gameplay_tag(payload.at("tag").get<std::string>()))
                errors.push_back({{"code","scripting.invalid-gameplay-tag"},{"index",index}});
        } else errors.push_back({{"code","scripting.command-not-allowed"},{"index",index},{"operation",operation}});
    }
    Json applied=Json::array();
    if(errors.empty()) for(std::size_t index=0;index<commands.size();++index) {
        const auto& command=commands.at(index); const auto operation=command.at("operation").get<std::string>();
        const auto entity=command.at("entityId").get<std::string>(); const auto& payload=command.at("payload");
        if(operation=="scene.transform.set-position") {
            const auto views=entity_views(); const auto found=std::ranges::find(views,entity,&WorldEntityView::id);
            if(found==views.end()||!found->transform) {errors.push_back({{"code","world.component-not-found"},{"index",index}});break;}
            auto transform=*found->transform;transform.x=payload.at("x").get<float>();transform.y=payload.at("y").get<float>();transform.z=payload.at("z").get<float>();
            auto result=Json::parse(edit_transform_json(entity,transform,revision_,"scripting:"+std::string(instance_id),false));
            applied.push_back({{"index",index},{"operation",operation},{"receipt",std::move(result)}});
        } else if(operation=="gameplay.event.emit") {
            const auto payload_value=payload.value("payload",Json::object());
            const auto sequence=gameplay_runtime_.emit(payload.at("eventType").get<std::string>(),script_entity.value_or(std::string(instance_id)),entity,payload_value.dump());
            vfx_runtime_.consume_gameplay_events(gameplay_runtime_.events());
            applied.push_back({{"index",index},{"operation",operation},{"eventSequence",sequence}});
        } else if(operation=="scene.property.set") {
            const auto plan=plan_property_update(entity,payload.at("propertyId").get<std::string>(),payload.at("value").dump(),
                revision_,"scripting:"+std::string(instance_id));
            const auto result=Json::parse(action_receipt_json(apply_property_plan(plan,false)));
            if(!result.value("success",false)){errors.push_back({{"code",result.value("code",std::string("world.property-apply-failed"))},{"index",index}});break;}
            applied.push_back({{"index",index},{"operation",operation},{"receipt",result}});
        } else if(operation=="sprite.playback.set") {
            const auto result=Json::parse(set_sprite_playback_json(entity,payload.at("clipId").get<std::string>(),
                payload.at("playing").get<bool>(),payload.at("playbackSpeed").get<float>(),
                payload.at("flipX").get<bool>(),payload.at("restart").get<bool>()));
            if(!result.value("success",false)){errors.push_back({{"code",result.value("code",std::string("sprite.playback-failed"))},{"index",index}});break;}
            applied.push_back({{"index",index},{"operation",operation},{"receipt",result}});
        } else if(operation=="audio.voice.play") {
            const auto result=Json::parse(play_audio_json(payload.at("assetId").get<std::string>(),
                payload.at("busId").get<std::string>(),payload.at("gain").get<float>(),
                payload.at("pitch").get<float>(),payload.at("looping").get<bool>()));
            if(!result.value("success",false)){errors.push_back({{"code",result.value("code",std::string("audio.play-failed"))},{"index",index}});break;}
            applied.push_back({{"index",index},{"operation",operation},{"receipt",result}});
        } else if(operation=="gameplay.persistence.request") {
            const auto request=GameplayPersistenceRequest{next_persistence_request_sequence_++,payload.at("action").get<std::string>(),
                payload.at("slotId").get<std::string>(),script_entity.value_or(std::string(instance_id))};
            persistence_requests_.push_back(request);
            applied.push_back({{"index",index},{"operation",operation},{"requestSequence",request.sequence}});
        } else if(operation=="gameplay.prefab.spawn") {
            const auto result=Json::parse(spawn_prefab_json(entity,payload.at("newEntityId").get<std::string>(),
                payload.at("displayName").get<std::string>(),{payload.at("x").get<float>(),payload.at("y").get<float>(),payload.at("z").get<float>()}));
            if(!result.value("success",false)){errors.push_back({{"code",result.value("code",std::string("gameplay.spawn-validation-failed"))},{"index",index}});break;}
            applied.push_back({{"index",index},{"operation",operation},{"receipt",result}});
        } else if(operation=="gameplay.entity.despawn") {
            const auto result=Json::parse(despawn_entity_json(entity));
            if(!result.value("success",false)){errors.push_back({{"code",result.value("code",std::string("gameplay.despawn-failed"))},{"index",index}});break;}
            applied.push_back({{"index",index},{"operation",operation},{"receipt",result}});
        } else if(operation=="gameplay.tag.set") {
            auto result=Json::parse(gameplay_ability_runtime_.set_tag_json(entity,payload.at("tag").get<std::string>(),
                payload.at("present").get<bool>(),gameplay_runtime_));
            applied.push_back({{"index",index},{"operation",operation},{"receipt",std::move(result)}});
        }
    }
    receipt["commandApplication"]={{"success",errors.empty()},{"requested",commands.is_array()?commands.size():0U},
        {"applied",applied.size()},{"errors",std::move(errors)},{"receipts",std::move(applied)},{"worldRevision",revision_}};
    if(!receipt.at("commandApplication").at("success").get<bool>()){receipt["success"]=false;receipt["code"]="scripting.command-application-failed";}
    return receipt.dump();
}

std::vector<GameplayPersistenceRequest> World::consume_persistence_requests() {
    auto requests=std::move(persistence_requests_);persistence_requests_.clear();return requests;
}

void World::complete_persistence_request(const GameplayPersistenceRequest& request,const bool success,
                                         const std::string_view code,const std::string_view detail) {
    gameplay_runtime_.emit("gameplay.persistence.completed","runtime.persistence",request.requester_entity_id,
        Json{{"requestSequence",request.sequence},{"action",request.action},{"slotId",request.slot_id},
            {"success",success},{"code",code},{"detail",detail}}.dump());
}
std::string World::scripting_project_configure_json(const std::filesystem::path& project_root,
                                                     const std::filesystem::path& script_project) {
    return scripting_runtime_.configure_project_json(project_root,script_project);
}
std::string World::scripting_project_load_assembly_json(const std::filesystem::path& assembly,
                                                        const std::string_view configuration) {
    return scripting_runtime_.load_project_assembly_json(assembly,configuration);
}
std::string World::scripting_project_compile_json(const std::string_view configuration) {
    return scripting_runtime_.compile_project_json(configuration);
}
std::string World::scripting_project_types_json() const { return scripting_runtime_.discover_project_types_json(); }
std::string World::scripting_project_observation_json() const { return scripting_runtime_.project_observe_json(); }
std::string World::scripting_debug_attach_json() const { return scripting_runtime_.debug_attach_json(); }
std::string World::scripting_debug_session_start_json() { return scripting_runtime_.debug_session_start_json(); }
std::string World::scripting_debug_session_status_json() const { return scripting_runtime_.debug_session_status_json(); }
std::string World::scripting_debug_session_request_json(const std::string_view command,
                                                         const std::string_view arguments_json,
                                                         const std::uint32_t timeout_ms) {
    return scripting_runtime_.debug_session_request_json(command,arguments_json,timeout_ms);
}
std::string World::scripting_debug_session_events_json() { return scripting_runtime_.debug_session_events_json(); }
std::string World::scripting_debug_session_stop_json(const std::uint32_t timeout_ms) {
    return scripting_runtime_.debug_session_stop_json(timeout_ms);
}

std::string World::inspector_document_json(const std::string_view entity_id) const {
    const auto views=entity_views();
    const auto found=std::ranges::find(views,entity_id,&WorldEntityView::id);
    if(found==views.end()) return Json{{"schemaVersion","noemancer.inspector-document/0.1"},{"revision",revision_},
        {"valid",false},{"code","world.entity-not-found"},{"entity",nullptr},{"sections",Json::array()}}.dump();
    Json sections=Json::array();
    const auto node=[&](const std::string_view property,const std::string_view component_override={}) {
        const auto schema=std::ranges::find(inspector_property_schemas(),property,&InspectorPropertySchema::property);
        if(schema==inspector_property_schemas().end()) return Json::object();
        const auto value=property_value_json(entity_id,property);
        const auto component=component_override.empty()?schema->component:std::string(component_override);
        return Json{{"id","editor.inspector."+std::string(entity_id)+"."+component+"."+schema->field},
            {"role","property"},{"component",component},{"field",schema->field},{"property",property},{"label",schema->label},
            {"valueType",schema->value_type},{"control",schema->control},{"value",value?Json::parse(*value):Json(nullptr)},
            {"editable",value.has_value()},{"action","world.property.plan"},{"constraints",schema->constraints}};
    };
    Json identity=Json::array({
        {{"id","editor.inspector."+std::string(entity_id)+".identity.stable-id"},{"role","property"},{"label","Stable ID"},{"value",found->id},{"valueType","string"},{"control","label"},{"editable",false}},
        {{"id","editor.inspector."+std::string(entity_id)+".identity.source"},{"role","property"},{"label","Source"},{"value",found->source.uri},{"valueType","string"},{"control","label"},{"editable",false}}
    });
    sections.push_back({{"id","editor.inspector."+std::string(entity_id)+".section.identity"},{"role","group"},
        {"component","SemanticIdentity"},{"label","Identity"},{"defaultExpanded",false},{"properties",std::move(identity)}});
    if(found->transform) sections.push_back({{"id","editor.inspector."+std::string(entity_id)+".section.transform"},{"role","group"},
        {"component","Transform"},{"label","Transform"},{"defaultExpanded",true},{"properties",Json::array({
            node("engine.entity.transform.position"),node("engine.entity.transform.rotationEulerDegrees"),node("engine.entity.transform.scale")})}});
    if(found->pbr_material) sections.push_back({{"id","editor.inspector."+std::string(entity_id)+".section.material"},{"role","group"},
        {"component","PbrMaterial"},{"label","Material"},{"defaultExpanded",true},{"properties",Json::array({
            node("engine.entity.material.baseColor"),node("engine.entity.material.metallic"),node("engine.entity.material.roughness"),
            node("engine.entity.material.emissiveColor"),node("engine.entity.material.emissiveIntensity")})}});
    if(found->local_light)sections.push_back({{"id","editor.inspector."+std::string(entity_id)+".section.local-light"},{"role","group"},
        {"component","LocalLight"},{"label","Local Light"},{"defaultExpanded",true},{"properties",Json::array({
            node("engine.entity.localLight.kind"),node("engine.entity.localLight.color"),node("engine.entity.localLight.luminousPowerLumens"),
            node("engine.entity.localLight.rangeMeters"),node("engine.entity.localLight.direction"),node("engine.entity.localLight.innerConeDegrees"),
            node("engine.entity.localLight.outerConeDegrees"),node("engine.entity.localLight.sourceRadiusMeters")})}});
    if(found->box_collider||found->sphere_collider||found->capsule_collider||found->convex_hull_collider) {
        Json properties=Json::array();
        const auto collider_component=found->box_collider?"BoxCollider":found->sphere_collider?"SphereCollider":found->capsule_collider?"CapsuleCollider":"ConvexHullCollider";
        if(found->box_collider) properties.push_back(node("engine.entity.collider.halfExtents"));
        else if(!found->convex_hull_collider) {
            properties.push_back(node("engine.entity.collider.radius",collider_component));
            if(found->capsule_collider) properties.push_back(node("engine.entity.collider.halfHeight",collider_component));
        }
        if(found->convex_hull_collider) properties.push_back({{"id","editor.inspector."+std::string(entity_id)+".ConvexHullCollider.pointCount"},
            {"role","property"},{"component","ConvexHullCollider"},{"field","pointCount"},{"property","engine.entity.collider.pointCount"},
            {"label","Hull Points"},{"valueType","u32"},{"control","label"},{"value",found->convex_hull_collider->points.size()},{"editable",false}});
        properties.push_back(node("engine.entity.collider.friction",collider_component));
        properties.push_back(node("engine.entity.collider.restitution",collider_component));
        properties.push_back(node("engine.entity.collider.isTrigger",collider_component));
        sections.push_back({{"id","editor.inspector."+std::string(entity_id)+".section.collider"},{"role","group"},
            {"component",collider_component},{"label","Collider"},{"defaultExpanded",true},{"properties",std::move(properties)}});
    }
    if(found->animation_player) sections.push_back({{"id","editor.inspector."+std::string(entity_id)+".section.animation"},{"role","group"},
        {"component","AnimationPlayer"},{"label","Animation"},{"defaultExpanded",true},{"properties",Json::array({
            node("engine.entity.animation.clipAsset"),node("engine.entity.animation.playbackSpeed"),
            node("engine.entity.animation.looping"),node("engine.entity.animation.playing"),
            node("engine.entity.animation.nextClipAsset"),node("engine.entity.animation.transitionDuration"),
            node("engine.entity.animation.rootMotionMode"),node("engine.entity.animation.stateMachineAsset"),
            node("engine.entity.animation.animationGraphAsset")})}});
    if(found->sprite_renderer)sections.push_back({{"id","editor.inspector."+std::string(entity_id)+".section.sprite-renderer"},{"role","group"},
        {"component","SpriteRenderer"},{"label","Sprite Renderer"},{"defaultExpanded",true},{"properties",Json::array({
            node("engine.entity.sprite.spriteAsset"),node("engine.entity.sprite.clip"),node("engine.entity.sprite.playbackSpeed"),
            node("engine.entity.sprite.playing"),node("engine.entity.sprite.flipX"),node("engine.entity.sprite.flipY"),
            node("engine.entity.sprite.sortingLayer"),node("engine.entity.sprite.sortingOrder"),node("engine.entity.sprite.visible")})}});
    if(found->tilemap_renderer)sections.push_back({{"id","editor.inspector."+std::string(entity_id)+".section.tilemap-renderer"},{"role","group"},
        {"component","TilemapRenderer"},{"label","Tilemap Renderer"},{"defaultExpanded",true},{"properties",Json::array({
            node("engine.entity.tilemap.tilemapAsset"),node("engine.entity.tilemap.visible"),node("engine.entity.tilemap.collisionEnabled")})}});
    const auto document_entity=std::ranges::find(scene_document_.entities,entity_id,&SceneEntityDocument::guid);
    if(document_entity!=scene_document_.entities.end()&&document_entity->managed_script) {
        const auto& script=*document_entity->managed_script;
        auto runtime=Json::parse(scripting_runtime_.observe_json(),nullptr,false);Json state=nullptr;
        if(!runtime.is_discarded()) for(const auto& instance:runtime.value("instances",Json::array()))
            if(instance.value("id",std::string{})==script.instance_id){state=instance;break;}
        const auto property=[&](const std::string& field,const std::string& label,Json value,const std::string& value_type){return Json{
            {"id","editor.inspector."+std::string(entity_id)+".ManagedScript."+field},{"role","property"},{"component","ManagedScript"},
            {"field",field},{"property","engine.entity.managedScript."+field},{"label",label},{"valueType",value_type},
            {"control","label"},{"value",std::move(value)},{"editable",false}};};
        sections.push_back({{"id","editor.inspector."+std::string(entity_id)+".section.managed-script"},{"role","group"},
            {"component","ManagedScript"},{"label","C# Script"},{"defaultExpanded",true},{"properties",Json::array({
                property("instanceId","Instance ID",script.instance_id,"string"),node("engine.entity.managedScript.assemblyAsset"),
                node("engine.entity.managedScript.typeName"),node("engine.entity.managedScript.enabled"),
                node("engine.entity.managedScript.properties"),
                property("state","Runtime State",state,"json")})}});
    }
    return Json{{"schemaVersion","noemancer.inspector-document/0.1"},{"revision",revision_},{"valid",true},{"code","ok"},
        {"panel",{{"id","editor.panel.inspector"},{"role","inspector"},{"label","Inspector"}}},
        {"entity",{{"id",found->id},{"name",found->display_name},{"type",found->type},{"source",{{"uri",found->source.uri},{"pointer",found->source.json_pointer}}}}},
        {"sections",std::move(sections)}}.dump();
}

std::string World::semantic_ui_document_json(const std::string_view entity_id, const std::string_view locale) const {
    return semantic_ui_document_from_inspector(inspector_document_json(entity_id), locale);
}

std::string World::semantic_ui_observation_json(const std::string_view entity_id, const SemanticUiQuery& query,
                                                 const std::string_view locale) const {
    return semantic_ui_query_json(semantic_ui_document_json(entity_id, locale), query);
}

std::string World::semantic_ui_project_document_json(const std::string_view locale) const {
    if(project_hud_document_json_.empty())return Json{{"schemaVersion","noemancer.ui-document/0.1"},{"valid",false},
        {"code","ui.project-hud-not-configured"},{"documentId","project.hud.missing"},{"nodes",Json::array()}}.dump();
    return semantic_ui_project_runtime_document(project_hud_document_json_,scripting_runtime_.observe_json(),
        input_runtime_.observe_json(),gameplay_runtime_.observe_json(),locale);
}

std::string World::semantic_ui_delta_json(const std::string_view entity_id, const std::uint64_t since_revision,
                                           const SemanticUiDeltaQuery& query, const std::string_view locale) const {
    return noemancer::semantic_ui_delta_json(
        semantic_ui_document_json(entity_id, locale), delta_json(since_revision), entity_id, query);
}

std::string World::retained_ui_preview_json(const std::string_view entity_id, const std::uint32_t width,
                                            const std::uint32_t height, const float density_scale,
                                            const std::string_view locale) const {
    return noemancer::retained_ui_preview_json(
        semantic_ui_document_json(entity_id, locale), width, height, density_scale);
}

std::string World::schema_json() const {
    Json schema = {
        {"protocolVersion", "0.2"},
        {"semanticSchemaVersion", "0.1"},
        {"conventionRegistry", "noemancer.semantic-conventions.core"},
        {"components", {
            {"Transform", {
                {"schemaRef", "schema://noemancer/component/transform/0.1"},
                {"fields", {
                    {"x", "engine.entity.transform.position.x"},
                    {"y", "engine.entity.transform.position.y"},
                    {"z", "engine.entity.transform.position.z"},
                    {"rotationEulerDegrees", "engine.entity.transform.rotationEulerDegrees"},
                    {"rotationQuaternion", "engine.entity.transform.rotationQuaternion"}
                }}
            }},
            {"Velocity", {
                {"schemaRef", "schema://noemancer/component/velocity/0.1"},
                {"fields", {
                    {"x", "engine.entity.velocity.linear.x"},
                    {"y", "engine.entity.velocity.linear.y"},
                    {"z", "engine.entity.velocity.linear.z"}
                }}
            }},
            {"Camera", {{"schemaRef", "schema://noemancer/component/camera/0.1"}}},
            {"DirectionalLight", {{"schemaRef", "schema://noemancer/component/directional-light/0.1"}}},
            {"LocalLight", {{"schemaRef", "schema://noemancer/component/local-light/0.1"}}},
            {"MeshRenderer", {{"schemaRef", "schema://noemancer/component/mesh-renderer/0.1"}}},
            {"SpriteRenderer", {{"schemaRef", "schema://noemancer/component/sprite-renderer/0.1"}}},
            {"TilemapRenderer", {{"schemaRef", "schema://noemancer/component/tilemap-renderer/0.1"}}},
            {"PbrMaterial", {{"schemaRef", "schema://noemancer/component/pbr-material/0.1"}}},
            {"RigidBody", {{"schemaRef", "schema://noemancer/component/rigid-body/0.1"}}},
            {"BoxCollider", {{"schemaRef", "schema://noemancer/component/box-collider/0.1"}}},
            {"SphereCollider", {{"schemaRef", "schema://noemancer/component/sphere-collider/0.1"}}},
            {"CapsuleCollider", {{"schemaRef", "schema://noemancer/component/capsule-collider/0.1"}}},
            {"ConvexHullCollider", {{"schemaRef", "schema://noemancer/component/convex-hull-collider/0.1"}}},
            {"AnimationPlayer", {{"schemaRef", "schema://noemancer/component/animation-player/0.1"}}},
            {"ManagedScript", {{"schemaRef", "schema://noemancer/component/managed-script/0.1"},
                {"lifecycle",Json::array({"OnCreate","OnFixedUpdate","OnUpdate","OnDestroy"})},
                {"boundary","stable-id+json-value+command-buffer"}}}
        }}
    };
    for(auto& [name,component]:schema.at("components").items())component["id"]="engine.component."+name;
    schema["editableProperties"]=Json::array();
    for(const auto& property:inspector_property_schemas()) schema["editableProperties"].push_back({
        {"component",property.component},{"field",property.field},{"property",property.property},{"label",property.label},
        {"valueType",property.value_type},{"control",property.control},{"constraints",property.constraints},
        {"planAction","world.property.plan"},{"applyAction","world.change.apply"}});
    return schema.dump();
}

std::string World::managed_bindings_source() const {
    const auto schema=Json::parse(schema_json());
    auto identifier=[](const std::string_view value) {
        std::string result;
        bool capitalize=true;
        for(const unsigned char character:value) {
            if(!std::isalnum(character)) { capitalize=true; continue; }
            result.push_back(capitalize?static_cast<char>(std::toupper(character)):static_cast<char>(character));
            capitalize=false;
        }
        if(result.empty()||std::isdigit(static_cast<unsigned char>(result.front())))result.insert(result.begin(),'_');
        return result;
    };
    auto managed_type=[](const std::string_view value_type) -> std::string_view {
        if(value_type=="f32")return "float";
        if(value_type=="i32")return "int";
        if(value_type=="bool")return "bool";
        if(value_type=="asset-id")return "AssetId";
        if(value_type=="vector3")return "Float3";
        if(value_type=="color3")return "Color3";
        if(value_type=="json")return "System.Text.Json.JsonElement";
        return "string";
    };

    std::ostringstream output;
    output<<"// <auto-generated by `noemancer bindings csharp` />\n"
          <<"namespace Noemancer.Generated;\n\n"
          <<"public static class EngineSchema\n{\n"
          <<"    public const string ProtocolVersion = \""<<schema.at("protocolVersion").get<std::string>()<<"\";\n\n"
          <<"    public static class Components\n    {\n";
    for(const auto& [name,component]:schema.at("components").items())
        output<<"        public static readonly ComponentId "<<identifier(name)<<" = new(\""<<component.at("id").get<std::string>()<<"\");\n";
    output<<"    }\n\n    public static class Properties\n    {\n";

    std::map<std::string,std::map<std::string,std::pair<std::string,std::string>>> properties;
    for(const auto& property:schema.at("editableProperties")) {
        const auto component=property.at("component").get<std::string>();
        const auto field=property.at("field").get<std::string>();
        properties[component][field]={property.at("property").get<std::string>(),property.at("valueType").get<std::string>()};
    }
    bool first_component=true;
    for(const auto& [component,fields]:properties) {
        if(!first_component)output<<'\n';
        first_component=false;
        output<<"        public static class "<<identifier(component)<<"\n        {\n";
        for(const auto& [field,property]:fields) {
            auto field_name=identifier(field);
            if(property.second=="json")field_name+="Json";
            output<<"            public static readonly PropertyId<"<<managed_type(property.second)<<"> "<<field_name
                  <<" = new(\""<<property.first<<"\");\n";
        }
        output<<"        }\n";
    }
    output<<"    }\n}\n";
    return output.str();
}

} // namespace noemancer
