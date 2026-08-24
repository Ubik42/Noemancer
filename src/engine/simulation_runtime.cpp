#include "engine/simulation_runtime.hpp"
#include "engine/content_hash.hpp"

#include <nlohmann/json.hpp>

#include <Jolt/Jolt.h>
#include <Jolt/RegisterTypes.h>
#include <Jolt/Core/Factory.h>
#include <Jolt/Core/JobSystemThreadPool.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/ConvexHullShape.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/ShapeCast.h>
#include <Jolt/Physics/Collision/CollisionCollectorImpl.h>
#include <Jolt/Physics/Body/BodyFilter.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/Shape/SubShapeIDPair.h>
#include <Jolt/Physics/PhysicsSystem.h>

#include <ozz/animation/offline/animation_builder.h>
#include <ozz/animation/offline/animation_optimizer.h>
#include <ozz/animation/offline/raw_animation.h>
#include <ozz/animation/offline/raw_skeleton.h>
#include <ozz/animation/offline/skeleton_builder.h>
#include <ozz/animation/runtime/animation.h>
#include <ozz/animation/runtime/blending_job.h>
#include <ozz/animation/runtime/local_to_model_job.h>
#include <ozz/animation/runtime/sampling_job.h>
#include <ozz/animation/runtime/skeleton.h>
#include <ozz/base/io/archive.h>
#include <ozz/base/io/stream.h>
#include <ozz/base/maths/simd_math.h>
#include <ozz/base/maths/soa_transform.h>
#include <ozz/base/span.h>

#include <algorithm>
#include <bit>
#include <cctype>
#include <cmath>
#include <cstring>
#include <functional>
#include <iomanip>
#include <limits>
#include <mutex>
#include <optional>
#include <sstream>
#include <thread>
#include <unordered_map>
#include <unordered_set>

namespace noemancer {

namespace {

namespace Layers {
constexpr JPH::ObjectLayer non_moving = 0;
constexpr JPH::ObjectLayer moving = 1;
constexpr JPH::ObjectLayer count = 2;
}

namespace BroadPhaseLayers {
constexpr JPH::BroadPhaseLayer non_moving{0};
constexpr JPH::BroadPhaseLayer moving{1};
constexpr unsigned count = 2;
}

class BroadPhaseLayerInterface final : public JPH::BroadPhaseLayerInterface {
public:
    [[nodiscard]] JPH::uint GetNumBroadPhaseLayers() const override { return BroadPhaseLayers::count; }
    [[nodiscard]] JPH::BroadPhaseLayer GetBroadPhaseLayer(const JPH::ObjectLayer layer) const override {
        return layer == Layers::non_moving ? BroadPhaseLayers::non_moving : BroadPhaseLayers::moving;
    }
};

class ObjectVsBroadPhaseFilter final : public JPH::ObjectVsBroadPhaseLayerFilter {
public:
    [[nodiscard]] bool ShouldCollide(const JPH::ObjectLayer layer, const JPH::BroadPhaseLayer broad_phase) const override {
        return layer == Layers::moving || broad_phase == BroadPhaseLayers::moving;
    }
};

class ObjectLayerPairFilter final : public JPH::ObjectLayerPairFilter {
public:
    [[nodiscard]] bool ShouldCollide(const JPH::ObjectLayer first, const JPH::ObjectLayer second) const override {
        return first == Layers::moving || second == Layers::moving;
    }
};

struct GlobalJolt final {
    GlobalJolt() {
        JPH::RegisterDefaultAllocator();
        JPH::Factory::sInstance = new JPH::Factory();
        JPH::RegisterTypes();
    }
    ~GlobalJolt() {
        JPH::UnregisterTypes();
        delete JPH::Factory::sInstance;
        JPH::Factory::sInstance = nullptr;
    }
};

GlobalJolt& global_jolt() {
    static GlobalJolt instance;
    return instance;
}

JPH::EMotionType jolt_motion_type(const PhysicsMotionType type) {
    if (type == PhysicsMotionType::static_body) return JPH::EMotionType::Static;
    if (type == PhysicsMotionType::kinematic_body) return JPH::EMotionType::Kinematic;
    return JPH::EMotionType::Dynamic;
}
}

struct PhysicsRuntime::Impl final : JPH::ContactListener {
    struct PendingContact final {
        JPH::BodyID first;
        JPH::BodyID second;
        JPH::Vec3 normal;
        float penetration{};
    };
    struct Definition final {
        PhysicsMotionType motion_type{};
        PhysicsShapeType shape_type{};
        float half_x{}, half_y{}, half_z{};
        float radius{}, half_height{};
        std::vector<std::array<float, 3>> convex_points;
        float gravity_factor{}, linear_damping{}, restitution{}, friction{}, mass{};
        bool one_way{},is_trigger{},constrain_to_2d{};
    };
    struct LastState final {
        float x{}, y{}, z{}, velocity_x{}, velocity_y{}, velocity_z{},rotation_x{},rotation_y{},rotation_z{},rotation_w{1.0F};
    };

    BroadPhaseLayerInterface broad_phase_layers;
    ObjectVsBroadPhaseFilter object_vs_broad_phase;
    ObjectLayerPairFilter object_pairs;
    std::unique_ptr<JPH::TempAllocatorImpl> temp_allocator;
    std::unique_ptr<JPH::JobSystemThreadPool> jobs;
    std::unique_ptr<JPH::PhysicsSystem> system;
    std::unordered_map<std::string, JPH::BodyID> bodies;
    std::unordered_map<std::string, Definition> definitions;
    std::unordered_map<std::string, LastState> last_states;
    std::unordered_map<JPH::uint32, std::string> entities;
    std::mutex contact_mutex;
    std::unordered_map<std::uint64_t, PendingContact> active_contacts;

    static std::uint64_t contact_key(const JPH::BodyID first, const JPH::BodyID second) {
        const auto a = first.GetIndexAndSequenceNumber();
        const auto b = second.GetIndexAndSequenceNumber();
        return (static_cast<std::uint64_t>(std::min(a, b)) << 32U) | std::max(a, b);
    }

    static Definition definition(const PhysicsBodyState& state) {
        return {state.motion_type, state.shape_type, state.half_x, state.half_y, state.half_z, state.radius, state.half_height, state.convex_points, state.gravity_factor,
            state.linear_damping, state.restitution, state.friction, state.mass, state.one_way,state.is_trigger,state.constrain_to_2d};
    }

    static bool same_definition(const Definition& left, const Definition& right) {
        return left.motion_type == right.motion_type && left.shape_type == right.shape_type &&
            left.half_x == right.half_x && left.half_y == right.half_y &&
            left.half_z == right.half_z && left.radius == right.radius && left.half_height == right.half_height &&
            left.convex_points == right.convex_points && left.gravity_factor == right.gravity_factor &&
            left.linear_damping == right.linear_damping && left.restitution == right.restitution &&
            left.friction == right.friction && left.mass == right.mass && left.one_way == right.one_way&&left.is_trigger==right.is_trigger &&
            left.constrain_to_2d == right.constrain_to_2d;
    }

    Impl() {
        static_cast<void>(global_jolt());
        temp_allocator = std::make_unique<JPH::TempAllocatorImpl>(8 * 1024 * 1024);
        const auto available_workers = std::max(1U, std::thread::hardware_concurrency()) - 1U;
        jobs = std::make_unique<JPH::JobSystemThreadPool>(JPH::cMaxPhysicsJobs, JPH::cMaxPhysicsBarriers,
            static_cast<int>(std::min(available_workers, 4U)));
        system = std::make_unique<JPH::PhysicsSystem>();
        system->Init(65536, 0, 65536, 10240, broad_phase_layers, object_vs_broad_phase, object_pairs);
        system->SetContactListener(this);
    }

    ~Impl() override {
        auto& interface = system->GetBodyInterface();
        for (const auto& [unused, id] : bodies) {
            static_cast<void>(unused);
            interface.RemoveBody(id);
            interface.DestroyBody(id);
        }
    }

    JPH::ValidateResult OnContactValidate(const JPH::Body& first, const JPH::Body& second, JPH::RVec3Arg,
                                          const JPH::CollideShapeResult&) override {
        const auto first_entity = entities.find(first.GetID().GetIndexAndSequenceNumber());
        const auto second_entity = entities.find(second.GetID().GetIndexAndSequenceNumber());
        if (first_entity == entities.end() || second_entity == entities.end()) {
            return JPH::ValidateResult::AcceptAllContactsForThisBodyPair;
        }
        const auto first_definition = definitions.find(first_entity->second);
        const auto second_definition = definitions.find(second_entity->second);
        const auto first_state = last_states.find(first_entity->second);
        const auto second_state = last_states.find(second_entity->second);
        if (first_definition == definitions.end() || second_definition == definitions.end() ||
            first_state == last_states.end() || second_state == last_states.end()) {
            return JPH::ValidateResult::AcceptAllContactsForThisBodyPair;
        }
        const Definition* platform = nullptr;
        const LastState* platform_state = nullptr;
        const Definition* actor = nullptr;
        const LastState* actor_state = nullptr;
        if (first_definition->second.one_way) {
            platform = &first_definition->second;
            platform_state = &first_state->second;
            actor = &second_definition->second;
            actor_state = &second_state->second;
        } else if (second_definition->second.one_way) {
            platform = &second_definition->second;
            platform_state = &second_state->second;
            actor = &first_definition->second;
            actor_state = &first_state->second;
        } else {
            return JPH::ValidateResult::AcceptAllContactsForThisBodyPair;
        }
        if (actor->motion_type != PhysicsMotionType::dynamic_body) {
            return JPH::ValidateResult::RejectAllContactsForThisBodyPair;
        }
        const auto platform_top = platform_state->y + platform->half_y;
        const auto actor_bottom = actor_state->y - actor->half_y;
        const auto approaching_from_above = actor_bottom >= platform_top - 0.08F &&
                                            actor_state->velocity_y <= platform_state->velocity_y + 0.05F;
        return approaching_from_above ? JPH::ValidateResult::AcceptAllContactsForThisBodyPair :
                                        JPH::ValidateResult::RejectAllContactsForThisBodyPair;
    }

    void OnContactAdded(const JPH::Body& first, const JPH::Body& second,
                        const JPH::ContactManifold& manifold, JPH::ContactSettings&) override {
        std::scoped_lock lock(contact_mutex);
        active_contacts.insert_or_assign(contact_key(first.GetID(), second.GetID()),
            PendingContact{first.GetID(), second.GetID(), manifold.mWorldSpaceNormal, manifold.mPenetrationDepth});
    }

    void OnContactPersisted(const JPH::Body& first, const JPH::Body& second,
                            const JPH::ContactManifold& manifold, JPH::ContactSettings&) override {
        std::scoped_lock lock(contact_mutex);
        active_contacts.insert_or_assign(contact_key(first.GetID(), second.GetID()),
            PendingContact{first.GetID(), second.GetID(), manifold.mWorldSpaceNormal, manifold.mPenetrationDepth});
    }

    void OnContactRemoved(const JPH::SubShapeIDPair& pair) override {
        std::scoped_lock lock(contact_mutex);
        active_contacts.erase(contact_key(pair.GetBody1ID(), pair.GetBody2ID()));
    }
};

PhysicsRuntime::PhysicsRuntime() : impl_(std::make_unique<Impl>()) {}
PhysicsRuntime::~PhysicsRuntime() = default;

void PhysicsRuntime::step(std::vector<PhysicsBodyState>& states, const float delta_seconds) {
    auto& body_interface = impl_->system->GetBodyInterface();
    std::unordered_set<std::string> live;
    live.reserve(states.size());
    for (const auto& state : states) live.insert(state.entity_id);
    for (auto iterator = impl_->bodies.begin(); iterator != impl_->bodies.end();) {
        if (live.contains(iterator->first)) { ++iterator; continue; }
        {
            std::scoped_lock lock(impl_->contact_mutex);
            std::erase_if(impl_->active_contacts, [&](const auto& item) {
                return item.second.first == iterator->second || item.second.second == iterator->second;
            });
        }
        impl_->entities.erase(iterator->second.GetIndexAndSequenceNumber());
        impl_->definitions.erase(iterator->first);
        impl_->last_states.erase(iterator->first);
        body_interface.RemoveBody(iterator->second);
        body_interface.DestroyBody(iterator->second);
        iterator = impl_->bodies.erase(iterator);
    }

    for (const auto& state : states) {
        const auto body = impl_->bodies.find(state.entity_id);
        const auto definition = impl_->definitions.find(state.entity_id);
        if (body == impl_->bodies.end() || definition == impl_->definitions.end() ||
            Impl::same_definition(definition->second, Impl::definition(state))) continue;
        const auto id = body->second;
        impl_->entities.erase(id.GetIndexAndSequenceNumber());
        body_interface.RemoveBody(id);
        body_interface.DestroyBody(id);
        impl_->bodies.erase(body);
        impl_->definitions.erase(definition);
        impl_->last_states.erase(state.entity_id);
    }

    for (const auto& state : states) {
        if (impl_->bodies.contains(state.entity_id)) continue;
        JPH::RefConst<JPH::Shape> shape;
        if (state.shape_type == PhysicsShapeType::sphere) shape = new JPH::SphereShape(state.radius);
        else if (state.shape_type == PhysicsShapeType::capsule) shape = new JPH::CapsuleShape(state.half_height, state.radius);
        else if (state.shape_type == PhysicsShapeType::convex_hull) {
            JPH::Array<JPH::Vec3> points;
            points.reserve(state.convex_points.size());
            for (const auto& point : state.convex_points) points.emplace_back(point[0], point[1], point[2]);
            auto result = JPH::ConvexHullShapeSettings(points).Create();
            if (result.HasError()) continue;
            shape = result.Get();
        } else shape = new JPH::BoxShape(JPH::Vec3(state.half_x, state.half_y, state.half_z));
        const auto motion = jolt_motion_type(state.motion_type);
        const JPH::Quat initial_rotation(state.rotation_x,state.rotation_y,state.rotation_z,state.rotation_w);
        JPH::BodyCreationSettings settings(shape, JPH::RVec3(state.position_x, state.position_y, state.position_z),
            initial_rotation.Normalized(), motion, motion == JPH::EMotionType::Static ? Layers::non_moving : Layers::moving);
        settings.mFriction = state.friction;
        settings.mRestitution = state.restitution;
        settings.mGravityFactor = state.gravity_factor;
        settings.mLinearDamping = state.linear_damping;
        settings.mIsSensor = state.is_trigger;
        if (state.constrain_to_2d) {
            // The platformer plane is XY. Do not use Plane2D here: its
            // RotationZ allowance would still let a character tip/spin on
            // contact. CharacterMotor2D owns its facing through animation or
            // sprite state, so all three rotational DOFs stay locked.
            settings.mAllowedDOFs = JPH::EAllowedDOFs::TranslationX | JPH::EAllowedDOFs::TranslationY;
        }
        if (motion == JPH::EMotionType::Dynamic) {
            settings.mOverrideMassProperties = JPH::EOverrideMassProperties::CalculateInertia;
            settings.mMassPropertiesOverride.mMass = state.mass;
        }
        JPH::Body* body = body_interface.CreateBody(settings);
        if (body == nullptr) continue;
        const auto id = body->GetID();
        body_interface.AddBody(id, motion == JPH::EMotionType::Dynamic ? JPH::EActivation::Activate : JPH::EActivation::DontActivate);
        if (motion != JPH::EMotionType::Static) body_interface.SetLinearVelocity(id, JPH::Vec3(state.velocity_x, state.velocity_y, state.velocity_z));
        impl_->bodies.emplace(state.entity_id, id);
        impl_->definitions.emplace(state.entity_id, Impl::definition(state));
        impl_->last_states.emplace(state.entity_id, Impl::LastState{state.position_x, state.position_y, state.position_z,
            state.velocity_x, state.velocity_y, state.velocity_z,state.rotation_x,state.rotation_y,state.rotation_z,state.rotation_w});
        impl_->entities.emplace(id.GetIndexAndSequenceNumber(), state.entity_id);
    }

    for (const auto& state : states) {
        const auto found = impl_->bodies.find(state.entity_id);
        const auto previous = impl_->last_states.find(state.entity_id);
        if (found == impl_->bodies.end() || previous == impl_->last_states.end()) continue;
        constexpr float epsilon = 0.00001F;
        const bool transform_changed = std::abs(state.position_x - previous->second.x) > epsilon ||
            std::abs(state.position_y - previous->second.y) > epsilon || std::abs(state.position_z - previous->second.z) > epsilon ||
            std::abs(state.rotation_x-previous->second.rotation_x)>epsilon||std::abs(state.rotation_y-previous->second.rotation_y)>epsilon||
            std::abs(state.rotation_z-previous->second.rotation_z)>epsilon||std::abs(state.rotation_w-previous->second.rotation_w)>epsilon;
        const bool velocity_changed = std::abs(state.velocity_x - previous->second.velocity_x) > epsilon ||
            std::abs(state.velocity_y - previous->second.velocity_y) > epsilon ||
            std::abs(state.velocity_z - previous->second.velocity_z) > epsilon;
        if (state.motion_type == PhysicsMotionType::kinematic_body) {
            body_interface.MoveKinematic(found->second, JPH::RVec3(state.position_x, state.position_y, state.position_z),
                JPH::Quat(state.rotation_x,state.rotation_y,state.rotation_z,state.rotation_w).Normalized(), std::max(delta_seconds, 0.0001F));
        } else if (transform_changed) {
            body_interface.SetPositionAndRotation(found->second, JPH::RVec3(state.position_x, state.position_y, state.position_z),
                JPH::Quat(state.rotation_x,state.rotation_y,state.rotation_z,state.rotation_w).Normalized(), state.motion_type == PhysicsMotionType::dynamic_body ? JPH::EActivation::Activate : JPH::EActivation::DontActivate);
        }
        if (velocity_changed && state.motion_type == PhysicsMotionType::dynamic_body) {
            body_interface.SetLinearVelocity(found->second, JPH::Vec3(state.velocity_x, state.velocity_y, state.velocity_z));
        }
    }

    const auto dt = std::clamp(delta_seconds, 0.0F, 0.1F);
    if (dt > 0.0F) static_cast<void>(impl_->system->Update(dt, 1, impl_->temp_allocator.get(), impl_->jobs.get()));

    for (auto& state : states) {
        const auto found = impl_->bodies.find(state.entity_id);
        if (found == impl_->bodies.end()) continue;
        JPH::RVec3 position;
        JPH::Quat rotation;
        body_interface.GetPositionAndRotation(found->second, position, rotation);
        const auto velocity = body_interface.GetLinearVelocity(found->second);
        state.position_x = static_cast<float>(position.GetX());
        state.position_y = static_cast<float>(position.GetY());
        state.position_z = static_cast<float>(position.GetZ());
        state.rotation_x=rotation.GetX();state.rotation_y=rotation.GetY();state.rotation_z=rotation.GetZ();state.rotation_w=rotation.GetW();
        state.velocity_x = velocity.GetX();
        state.velocity_y = velocity.GetY();
        state.velocity_z = velocity.GetZ();
        impl_->last_states.insert_or_assign(state.entity_id, Impl::LastState{state.position_x, state.position_y, state.position_z,
            state.velocity_x, state.velocity_y, state.velocity_z,state.rotation_x,state.rotation_y,state.rotation_z,state.rotation_w});
    }

    contacts_.clear();
    std::scoped_lock lock(impl_->contact_mutex);
    for (const auto& [unused, contact] : impl_->active_contacts) {
        static_cast<void>(unused);
        const auto first = impl_->entities.find(contact.first.GetIndexAndSequenceNumber());
        const auto second = impl_->entities.find(contact.second.GetIndexAndSequenceNumber());
        if (first == impl_->entities.end() || second == impl_->entities.end()) continue;
        const auto first_definition=impl_->definitions.find(first->second),second_definition=impl_->definitions.find(second->second);
        const auto is_trigger=(first_definition!=impl_->definitions.end()&&first_definition->second.is_trigger)||
            (second_definition!=impl_->definitions.end()&&second_definition->second.is_trigger);
        contacts_.push_back({first->second, second->second, contact.normal.GetX(), contact.normal.GetY(), contact.normal.GetZ(), contact.penetration,is_trigger});
    }
    // Sleeping pairs may no longer generate callbacks. Project simple unrotated boxes into a
    // stable semantic contact set so observations do not depend on Jolt's wake/sleep policy.
    for (std::size_t first_index = 0; first_index < states.size(); ++first_index) {
        for (std::size_t second_index = first_index + 1; second_index < states.size(); ++second_index) {
            const auto& first = states[first_index];
            const auto& second = states[second_index];
            if (first.motion_type == PhysicsMotionType::static_body && second.motion_type == PhysicsMotionType::static_body) continue;
            constexpr float slop = 0.03F;
            const auto overlap_x = first.half_x + second.half_x - std::abs(first.position_x - second.position_x);
            const auto overlap_y = first.half_y + second.half_y - std::abs(first.position_y - second.position_y);
            const auto overlap_z = first.half_z + second.half_z - std::abs(first.position_z - second.position_z);
            if (overlap_x < -slop || overlap_y < -slop || overlap_z < -slop) continue;
            const bool already_present = std::ranges::any_of(contacts_, [&](const PhysicsContact& contact) {
                return (contact.body_a == first.entity_id && contact.body_b == second.entity_id) ||
                    (contact.body_a == second.entity_id && contact.body_b == first.entity_id);
            });
            if (!already_present) contacts_.push_back({first.entity_id, second.entity_id, 0.0F,
                first.position_y >= second.position_y ? 1.0F : -1.0F, 0.0F, std::max(0.0F, overlap_y),first.is_trigger||second.is_trigger});
        }
    }
}

PhysicsRayCastHit PhysicsRuntime::ray_cast(const float origin_x, const float origin_y, const float origin_z,
                                           const float direction_x, const float direction_y, const float direction_z) const {
    PhysicsRayCastHit result;
    JPH::RRayCast ray(JPH::RVec3(origin_x, origin_y, origin_z), JPH::Vec3(direction_x, direction_y, direction_z));
    JPH::RayCastResult hit;
    if (!impl_->system->GetNarrowPhaseQuery().CastRay(ray, hit)) return result;
    const auto entity = impl_->entities.find(hit.mBodyID.GetIndexAndSequenceNumber());
    if (entity == impl_->entities.end()) return result;
    result.hit = true;
    result.entity_id = entity->second;
    result.fraction = hit.mFraction;
    result.position_x = origin_x + direction_x * hit.mFraction;
    result.position_y = origin_y + direction_y * hit.mFraction;
    result.position_z = origin_z + direction_z * hit.mFraction;
    return result;
}

PhysicsSweepHit PhysicsRuntime::sphere_sweep(const float origin_x,const float origin_y,const float origin_z,
                                             const float direction_x,const float direction_y,const float direction_z,
                                             const float radius,const std::string_view ignored_entity_id) const {
    PhysicsSweepHit result;
    if (!std::isfinite(radius)||radius<=0.0F||radius>10.0F) return result;
    JPH::RefConst<JPH::Shape> shape=new JPH::SphereShape(radius);
    const JPH::RShapeCast cast{shape,JPH::Vec3::sOne(),
        JPH::RMat44::sTranslation(JPH::RVec3(origin_x,origin_y,origin_z)),
        JPH::Vec3(direction_x,direction_y,direction_z)};
    JPH::ShapeCastSettings settings;
    settings.mReturnDeepestPoint=true;
    JPH::ClosestHitCollisionCollector<JPH::CastShapeCollector> collector;
    JPH::BodyID ignored_body;
    if (const auto found=impl_->bodies.find(std::string(ignored_entity_id)); found!=impl_->bodies.end()) ignored_body=found->second;
    const JPH::IgnoreSingleBodyFilter body_filter(ignored_body);
    impl_->system->GetNarrowPhaseQuery().CastShape(cast,settings,JPH::RVec3::sZero(),collector,{}, {},body_filter);
    if (!collector.HadHit()) return result;
    const auto entity=impl_->entities.find(collector.mHit.mBodyID2.GetIndexAndSequenceNumber());
    if (entity==impl_->entities.end()) return result;
    const auto normal=collector.mHit.mPenetrationAxis.LengthSq()>0.000001F
        ? collector.mHit.mPenetrationAxis.Normalized() : JPH::Vec3::sAxisY();
    result.hit=true; result.entity_id=entity->second; result.fraction=collector.mHit.mFraction;
    result.position_x=collector.mHit.mContactPointOn2.GetX();
    result.position_y=collector.mHit.mContactPointOn2.GetY();
    result.position_z=collector.mHit.mContactPointOn2.GetZ();
    // Jolt reports the shape-cast penetration axis from body 2 toward the swept
    // shape. Public queries expose the contacted surface normal instead.
    result.normal_x=-normal.GetX(); result.normal_y=-normal.GetY(); result.normal_z=-normal.GetZ();
    result.penetration_depth=collector.mHit.mPenetrationDepth;
    return result;
}

struct AnimationRuntime::Impl final {
    struct CompiledAsset final {
        std::string skeleton_asset;
        std::string clip_asset;
        ozz::unique_ptr<ozz::animation::Skeleton> skeleton;
        ozz::unique_ptr<ozz::animation::Animation> animation;
        std::vector<std::size_t> skin_to_runtime;
        std::vector<ozz::math::Float4x4> inverse_bind_matrices;
        AnimationCompressionEvidence compression;
        mutable ozz::animation::SamplingJob::Context context;
        mutable std::vector<ozz::math::SoaTransform> local_transforms;
        mutable std::vector<ozz::math::Float4x4> model_transforms;
    };

    ozz::unique_ptr<ozz::animation::Skeleton> skeleton;
    ozz::unique_ptr<ozz::animation::Animation> animation;
    mutable ozz::animation::SamplingJob::Context context;
    mutable std::vector<ozz::math::SoaTransform> local_transforms;
    mutable std::vector<ozz::math::Float4x4> model_transforms;
    std::unordered_map<std::string, std::unique_ptr<CompiledAsset>> compiled_assets;

    Impl() {
        using ozz::animation::offline::RawAnimation;
        using ozz::animation::offline::RawSkeleton;
        using ozz::math::Float3;
        using ozz::math::Quaternion;

        RawSkeleton raw_skeleton;
        raw_skeleton.roots.resize(1U);
        auto& root = raw_skeleton.roots.front();
        root.name = "root";
        root.transform.translation = Float3::zero();
        root.transform.rotation = Quaternion::identity();
        root.transform.scale = Float3::one();
        root.children.resize(1U);
        auto& upper = root.children.front();
        upper.name = "upper";
        upper.transform.translation = Float3::zero();
        upper.transform.rotation = Quaternion::identity();
        upper.transform.scale = Float3::one();
        skeleton = ozz::animation::offline::SkeletonBuilder{}(raw_skeleton);
        if (!skeleton) return;

        RawAnimation raw_animation;
        raw_animation.name = "test-bend";
        raw_animation.duration = 2.0F;
        raw_animation.tracks.resize(static_cast<std::size_t>(skeleton->num_joints()));
        auto& root_track = raw_animation.tracks[0];
        root_track.translations = {{0.0F, Float3::zero()}, {2.0F, Float3::zero()}};
        root_track.rotations = {{0.0F, Quaternion::identity()}, {2.0F, Quaternion::identity()}};
        root_track.scales = {{0.0F, Float3::one()}, {2.0F, Float3::one()}};
        auto& upper_track = raw_animation.tracks[1];
        constexpr float angle = 0.42F;
        upper_track.translations = {{0.0F, Float3::zero()}, {2.0F, Float3::zero()}};
        upper_track.rotations = {
            {0.0F, Quaternion::FromAxisAngle(Float3::z_axis(), -angle)},
            {0.5F, Quaternion::FromAxisAngle(Float3::z_axis(), angle)},
            {1.0F, Quaternion::FromAxisAngle(Float3::z_axis(), -angle)},
            {1.5F, Quaternion::FromAxisAngle(Float3::z_axis(), angle)},
            {2.0F, Quaternion::FromAxisAngle(Float3::z_axis(), -angle)}};
        upper_track.scales = {{0.0F, Float3::one()}, {2.0F, Float3::one()}};
        animation = ozz::animation::offline::AnimationBuilder{}(raw_animation);
        if (!animation) { skeleton.reset(); return; }
        context.Resize(skeleton->num_joints());
        local_transforms.resize(static_cast<std::size_t>(skeleton->num_soa_joints()));
        model_transforms.resize(static_cast<std::size_t>(skeleton->num_joints()));
    }
};

AnimationRuntime::AnimationRuntime() : impl_(std::make_unique<Impl>()) {}
AnimationRuntime::~AnimationRuntime() = default;

namespace {

ozz::math::Float4x4 ozz_matrix(const std::array<float, 16>& source) {
    ozz::math::Float4x4 result;
    for (std::size_t column = 0; column < 4U; ++column)
        result.cols[column] = ozz::math::simd_float4::LoadPtrU(source.data() + column * 4U);
    return result;
}

std::string asset_token(std::string source) {
    for (auto& character : source) {
        const auto value = static_cast<unsigned char>(character);
        character = std::isalnum(value) != 0 ? static_cast<char>(std::tolower(value)) : '-';
    }
    while (source.find("--") != std::string::npos) source.replace(source.find("--"), 2U, "-");
    if (source.empty()) source = "unnamed";
    return source;
}

struct RawAnimationKeyCounts final {
    std::size_t translations{};
    std::size_t rotations{};
    std::size_t scales{};
};

struct ArchiveMetrics final {
    std::size_t bytes{};
    std::string hash;
};

constexpr std::array<std::byte, 8> animation_artifact_magic{
    std::byte{'N'}, std::byte{'M'}, std::byte{'A'}, std::byte{'N'},
    std::byte{'I'}, std::byte{'M'}, std::byte{'0'}, std::byte{'1'}};
constexpr std::uint32_t animation_artifact_version = 1U;
constexpr std::uint32_t animation_artifact_endian = 0x01020304U;
constexpr std::size_t animation_artifact_header_bytes = 48U;
constexpr std::size_t maximum_animation_artifact_bytes = 256U * 1024U * 1024U;
constexpr std::size_t maximum_animation_manifest_bytes = 1024U * 1024U;
constexpr std::size_t maximum_animation_artifact_joints = SkeletalPose::maximum_joints;

void append_u32_le(std::vector<std::byte>& output, const std::uint32_t value) {
    for (std::uint32_t shift = 0U; shift < 32U; shift += 8U)
        output.push_back(static_cast<std::byte>((value >> shift) & 0xffU));
}

void append_u64_le(std::vector<std::byte>& output, const std::uint64_t value) {
    for (std::uint32_t shift = 0U; shift < 64U; shift += 8U)
        output.push_back(static_cast<std::byte>((value >> shift) & 0xffU));
}

std::optional<std::uint32_t> read_u32_le(const std::span<const std::byte> source, const std::size_t offset) {
    if (offset > source.size() || source.size() - offset < 4U) return std::nullopt;
    std::uint32_t value{};
    for (std::uint32_t index = 0U; index < 4U; ++index)
        value |= static_cast<std::uint32_t>(source[offset + index]) << (index * 8U);
    return value;
}

std::optional<std::uint64_t> read_u64_le(const std::span<const std::byte> source, const std::size_t offset) {
    if (offset > source.size() || source.size() - offset < 8U) return std::nullopt;
    std::uint64_t value{};
    for (std::uint32_t index = 0U; index < 8U; ++index)
        value |= static_cast<std::uint64_t>(source[offset + index]) << (index * 8U);
    return value;
}

bool checked_accumulate(std::size_t& total, const std::size_t count, const std::size_t stride = 1U) {
    if (stride != 0U && count > std::numeric_limits<std::size_t>::max() / stride) return false;
    const auto bytes = count * stride;
    if (bytes > std::numeric_limits<std::size_t>::max() - total) return false;
    total += bytes;
    return true;
}

bool has_archive_prefix(const std::span<const std::byte> bytes, const std::string_view tag,
                        const std::uint32_t version) {
    const auto prefix_bytes = 1U + tag.size() + 1U + sizeof(std::uint32_t);
    if (bytes.size() < prefix_bytes || bytes.front() != std::byte{1U}) return false;
    if (!std::equal(tag.begin(), tag.end(), reinterpret_cast<const char*>(bytes.data() + 1U)) ||
        bytes[1U + tag.size()] != std::byte{0U}) return false;
    const auto stored_version = read_u32_le(bytes, 1U + tag.size() + 1U);
    return stored_version && *stored_version == version;
}

// ozz's runtime IArchive assumes trusted bytes and uses assertions for short
// reads. Validate the exact v2 Skeleton layout before constructing IArchive so
// a self-consistent but malformed package cannot abort the Player or request an
// unbounded allocation inside the third-party loader.
bool preflight_ozz_skeleton_archive(const std::span<const std::byte> bytes,
                                    const std::size_t expected_joints) {
    constexpr std::string_view tag = "ozz-skeleton";
    constexpr std::size_t fixed_bytes = 1U + tag.size() + 1U + 3U * sizeof(std::uint32_t);
    if (!has_archive_prefix(bytes, tag, 2U) || bytes.size() < fixed_bytes) return false;
    const auto joint_count = read_u32_le(bytes, 1U + tag.size() + 1U + sizeof(std::uint32_t));
    const auto name_bytes = read_u32_le(bytes, 1U + tag.size() + 1U + 2U * sizeof(std::uint32_t));
    if (!joint_count || !name_bytes || *joint_count != expected_joints || *joint_count == 0U ||
        *joint_count > maximum_animation_artifact_joints || *name_bytes < *joint_count) return false;
    std::size_t exact_size = fixed_bytes;
    if (!checked_accumulate(exact_size, *name_bytes) ||
        !checked_accumulate(exact_size, *joint_count, sizeof(std::int16_t)) ||
        !checked_accumulate(exact_size, (*joint_count + 3U) / 4U, 160U) || exact_size != bytes.size()) return false;
    const auto names = bytes.subspan(fixed_bytes, *name_bytes);
    if (names.empty() || names.back() != std::byte{0U} ||
        std::ranges::count(names, std::byte{0U}) != static_cast<std::ptrdiff_t>(*joint_count)) return false;
    const auto parents_begin = fixed_bytes + *name_bytes;
    for (std::size_t joint = 0U; joint < *joint_count; ++joint) {
        const auto raw_parent = static_cast<std::uint16_t>(bytes[parents_begin + joint * 2U]) |
            (static_cast<std::uint16_t>(bytes[parents_begin + joint * 2U + 1U]) << 8U);
        const auto parent = static_cast<std::int16_t>(raw_parent);
        if ((joint == 0U && parent != -1) || (joint != 0U && (parent < -1 || parent >= static_cast<std::int16_t>(joint))))
            return false;
    }
    const auto rest_begin = parents_begin + *joint_count * sizeof(std::int16_t);
    for (std::size_t offset = rest_begin; offset < bytes.size(); offset += sizeof(std::uint32_t)) {
        const auto bits = read_u32_le(bytes, offset);
        if (!bits || !std::isfinite(std::bit_cast<float>(*bits))) return false;
    }
    return true;
}

// Mirrors the fixed ozz Animation v7 serialization layout. The counts are
// checked against the section length before IArchive can allocate from them.
bool preflight_ozz_animation_archive(const std::span<const std::byte> bytes,
                                     const std::size_t expected_tracks,
                                     const float expected_duration) {
    constexpr std::string_view tag = "ozz-animation";
    constexpr std::size_t scalar_count = 11U; // name + time/value + six iframe counts
    constexpr std::size_t fixed_bytes = 1U + tag.size() + 1U + sizeof(std::uint32_t) + sizeof(float) +
        sizeof(std::uint32_t) + scalar_count * sizeof(std::uint32_t);
    if (!has_archive_prefix(bytes, tag, 7U) || bytes.size() < fixed_bytes) return false;
    const auto duration_bits = read_u32_le(bytes, 1U + tag.size() + 1U + sizeof(std::uint32_t));
    const auto track_count = read_u32_le(bytes, 1U + tag.size() + 1U + sizeof(std::uint32_t) + sizeof(float));
    if (!duration_bits || !track_count || *track_count != expected_tracks || *track_count == 0U ||
        *track_count > maximum_animation_artifact_joints) return false;
    const auto duration = std::bit_cast<float>(*duration_bits);
    if (!std::isfinite(duration) || duration <= 0.0F ||
        std::abs(duration - expected_duration) > 0.000001F) return false;
    const auto counts_begin = 1U + tag.size() + 1U + sizeof(std::uint32_t) + sizeof(float) + sizeof(std::uint32_t);
    std::array<std::size_t, scalar_count> counts{};
    for (std::size_t index = 0U; index < counts.size(); ++index) {
        const auto value = read_u32_le(bytes, counts_begin + index * sizeof(std::uint32_t));
        if (!value) return false;
        counts[index] = *value;
    }
    const auto name_length = counts[0];
    const auto timepoint_count = counts[1];
    const auto translation_count = counts[2];
    const auto rotation_count = counts[3];
    const auto scale_count = counts[4];
    if (timepoint_count == 0U || translation_count < expected_tracks || rotation_count < expected_tracks ||
        scale_count < expected_tracks) return false;
    std::size_t exact_size = fixed_bytes;
    if (!checked_accumulate(exact_size, name_length) ||
        !checked_accumulate(exact_size, timepoint_count, sizeof(float))) return false;
    const auto add_controller = [&](const std::size_t key_count, const std::size_t iframe_entries,
                                    const std::size_t iframe_descriptors) {
        return checked_accumulate(exact_size, key_count, sizeof(std::uint8_t)) &&
            checked_accumulate(exact_size, key_count, sizeof(std::uint16_t)) &&
            checked_accumulate(exact_size, iframe_entries, sizeof(std::uint8_t)) &&
            checked_accumulate(exact_size, iframe_descriptors, sizeof(std::uint32_t)) &&
            checked_accumulate(exact_size, 1U, sizeof(float)) &&
            checked_accumulate(exact_size, key_count, 3U * sizeof(std::uint16_t));
    };
    if (!add_controller(translation_count, counts[5], counts[6]) ||
        !add_controller(rotation_count, counts[7], counts[8]) ||
        !add_controller(scale_count, counts[9], counts[10]) || exact_size != bytes.size()) return false;
    if (name_length != 0U && bytes[fixed_bytes + name_length - 1U] == std::byte{0U}) return false;
    return true;
}

template <typename Value>
std::vector<std::byte> archive_payload(const Value& value) {
    ozz::io::MemoryStream stream;
    ozz::io::OArchive archive(&stream, ozz::kLittleEndian);
    archive << value;
    std::vector<std::byte> payload(stream.Size());
    if (stream.Seek(0, ozz::io::Stream::kSet) != 0 ||
        stream.Read(payload.data(), payload.size()) != payload.size()) return {};
    return payload;
}

std::string compression_mode_name(const AnimationCompressionMode mode) {
    return mode == AnimationCompressionMode::ozz_hierarchical_key_reduction
        ? "ozz_hierarchical_key_reduction" : "ozz_runtime_baseline";
}

std::string fnv1a64_hash(const std::vector<std::byte>& bytes) {
    std::uint64_t hash = 1469598103934665603ULL;
    for (const auto value : bytes) {
        hash ^= static_cast<std::uint8_t>(value);
        hash *= 1099511628211ULL;
    }
    std::ostringstream output;
    output << "fnv1a64:" << std::hex << std::setfill('0') << std::setw(16) << hash;
    return output.str();
}

template <typename Value>
ArchiveMetrics archive_metrics(const Value& value) {
    ozz::io::MemoryStream stream;
    ozz::io::OArchive archive(&stream, ozz::kLittleEndian);
    archive << value;
    ArchiveMetrics result;
    result.bytes = stream.Size();
    std::vector<std::byte> payload(result.bytes);
    if (stream.Seek(0, ozz::io::Stream::kSet) != 0 || stream.Read(payload.data(), payload.size()) != payload.size()) {
        return {};
    }
    result.hash = fnv1a64_hash(payload);
    return result;
}

RawAnimationKeyCounts raw_animation_key_counts(
    const ozz::animation::offline::RawAnimation& animation) {
    RawAnimationKeyCounts result;
    for (const auto& track : animation.tracks) {
        result.translations += track.translations.size();
        result.rotations += track.rotations.size();
        result.scales += track.scales.size();
    }
    return result;
}

void append_joint_debug(SkeletalPose& pose, const ozz::animation::Skeleton& skeleton,
                        const std::vector<ozz::math::Float4x4>& model_transforms) {
    const auto names = skeleton.joint_names();
    const auto parents = skeleton.joint_parents();
    pose.joints.reserve(model_transforms.size());
    for (std::size_t index = 0; index < model_transforms.size(); ++index) {
        std::array<float, 4> translation{};
        ozz::math::StorePtrU(model_transforms[index].cols[3], translation.data());
        pose.joints.push_back({std::string(names[index]), static_cast<int>(parents[index]),
            translation[0], translation[1], translation[2]});
    }
}

} // namespace

AnimationCompileResult AnimationRuntime::compile_gltf_asset(const std::string_view asset_id, const GltfMeshData& source,
                                                             const std::size_t skin_index, const std::size_t animation_index,
                                                             const AnimationCompressionMode compression_mode) {
    AnimationCompileResult result;
    result.compression.requested_mode = compression_mode;
    result.code = "animation.compile-invalid-source";
    if (!source.valid || skin_index >= source.skins.size() || animation_index >= source.animations.size()) {
        result.detail = "Source GLB, skin index or animation index is invalid.";
        return result;
    }
    const auto& skin = source.skins[skin_index];
    const auto& clip = source.animations[animation_index];
    if (skin.joints.empty() || skin.joints.size() > SkeletalPose::maximum_joints || clip.duration <= 0.0F) {
        result.detail = "Skin joint count or clip duration exceeds the runtime contract.";
        return result;
    }
    using RawSkeleton = ozz::animation::offline::RawSkeleton;
    using RawAnimation = ozz::animation::offline::RawAnimation;
    std::vector<std::vector<std::size_t>> children(skin.joints.size());
    std::vector<std::size_t> roots;
    for (std::size_t joint = 0; joint < skin.joints.size(); ++joint) {
        const int parent = skin.joints[joint].parent_joint;
        if (parent < 0) roots.push_back(joint);
        else if (static_cast<std::size_t>(parent) < skin.joints.size() && static_cast<std::size_t>(parent) != joint)
            children[static_cast<std::size_t>(parent)].push_back(joint);
        else { result.detail = "Skin joint hierarchy contains an invalid parent."; return result; }
    }
    auto compiled = std::make_unique<Impl::CompiledAsset>();
    std::vector<std::size_t> runtime_to_skin;
    RawSkeleton raw_skeleton;
    raw_skeleton.roots.resize(roots.size());
    std::function<bool(std::size_t, RawSkeleton::Joint&)> build_joint;
    build_joint = [&](const std::size_t skin_joint, RawSkeleton::Joint& destination) {
        destination.name = skin.joints[skin_joint].name;
        if (!ozz::math::ToAffine(ozz_matrix(skin.joints[skin_joint].local_transform), &destination.transform)) return false;
        runtime_to_skin.push_back(skin_joint);
        destination.children.resize(children[skin_joint].size());
        for (std::size_t child = 0; child < children[skin_joint].size(); ++child)
            if (!build_joint(children[skin_joint][child], destination.children[child])) return false;
        return true;
    };
    for (std::size_t root = 0; root < roots.size(); ++root)
        if (!build_joint(roots[root], raw_skeleton.roots[root])) {
            result.detail = "Joint local matrix cannot be decomposed into translation, rotation and scale.";
            return result;
        }
    if (runtime_to_skin.size() != skin.joints.size()) {
        result.detail = "Skin hierarchy is cyclic or disconnected from its declared roots.";
        return result;
    }
    compiled->skin_to_runtime.resize(skin.joints.size());
    for (std::size_t runtime_joint = 0; runtime_joint < runtime_to_skin.size(); ++runtime_joint)
        compiled->skin_to_runtime[runtime_to_skin[runtime_joint]] = runtime_joint;
    compiled->skeleton = ozz::animation::offline::SkeletonBuilder{}(raw_skeleton);
    if (!compiled->skeleton) { result.detail = "ozz rejected the validated raw skeleton."; return result; }

    RawAnimation raw_animation;
    raw_animation.name = clip.name;
    raw_animation.duration = clip.duration;
    raw_animation.tracks.resize(skin.joints.size());
    for (std::size_t skin_joint = 0; skin_joint < skin.joints.size(); ++skin_joint) {
        ozz::math::Transform rest;
        if (!ozz::math::ToAffine(ozz_matrix(skin.joints[skin_joint].local_transform), &rest)) {
            result.detail = "Joint rest transform decomposition changed during animation compilation.";
            return result;
        }
        auto& track = raw_animation.tracks[compiled->skin_to_runtime[skin_joint]];
        track.translations = {{0.0F, rest.translation}, {clip.duration, rest.translation}};
        track.rotations = {{0.0F, rest.rotation}, {clip.duration, rest.rotation}};
        track.scales = {{0.0F, rest.scale}, {clip.duration, rest.scale}};
    }
    std::unordered_map<std::uint32_t, std::size_t> node_to_skin;
    for (std::size_t joint = 0; joint < skin.joints.size(); ++joint) node_to_skin.emplace(skin.joints[joint].node_index, joint);
    std::size_t accepted_channels{};
    for (const auto& channel : clip.channels) {
        const auto found = node_to_skin.find(channel.node_index);
        if (found == node_to_skin.end()) continue;
        if (channel.interpolation != "LINEAR") {
            result.code = "animation.interpolation-unsupported";
            result.detail = "ozz compilation currently accepts LINEAR channels; STEP remains preserved in decoded source data.";
            return result;
        }
        auto& track = raw_animation.tracks[compiled->skin_to_runtime[found->second]];
        if (channel.path == "translation") {
            track.translations.clear();
            for (std::size_t key = 0; key < channel.times.size(); ++key)
                track.translations.push_back({channel.times[key], {channel.values[key][0], channel.values[key][1], channel.values[key][2]}});
        } else if (channel.path == "rotation") {
            track.rotations.clear();
            for (std::size_t key = 0; key < channel.times.size(); ++key) {
                const ozz::math::Quaternion rotation{channel.values[key][0], channel.values[key][1], channel.values[key][2], channel.values[key][3]};
                track.rotations.push_back({channel.times[key], ozz::math::NormalizeSafe(rotation, ozz::math::Quaternion::identity())});
            }
        } else if (channel.path == "scale") {
            track.scales.clear();
            for (std::size_t key = 0; key < channel.times.size(); ++key)
                track.scales.push_back({channel.times[key], {channel.values[key][0], channel.values[key][1], channel.values[key][2]}});
        }
        ++accepted_channels;
    }
    if (accepted_channels == 0U || !raw_animation.Validate()) {
        result.detail = "Animation has no channels targeting the selected skin or violates ozz key constraints.";
        return result;
    }
    const auto input_keys = raw_animation_key_counts(raw_animation);
    result.compression.input_raw_resident_bytes = raw_animation.size();
    const auto input_raw_archive = archive_metrics(raw_animation);
    result.compression.input_raw_archive_bytes = input_raw_archive.bytes;
    result.compression.input_raw_archive_hash = input_raw_archive.hash;
    result.compression.input_translation_keys = input_keys.translations;
    result.compression.input_rotation_keys = input_keys.rotations;
    result.compression.input_scale_keys = input_keys.scales;
    auto baseline_animation = ozz::animation::offline::AnimationBuilder{}(raw_animation);
    if (!baseline_animation) { result.detail = "ozz rejected the validated raw animation."; return result; }
    result.compression.baseline_runtime_resident_bytes = baseline_animation->size();
    const auto baseline_archive = archive_metrics(*baseline_animation);
    result.compression.baseline_runtime_archive_bytes = baseline_archive.bytes;
    result.compression.baseline_runtime_archive_hash = baseline_archive.hash;
    const auto skeleton_archive = archive_metrics(*compiled->skeleton);
    result.compression.skeleton_archive_bytes = skeleton_archive.bytes;
    result.compression.skeleton_archive_hash = skeleton_archive.hash;

    ozz::unique_ptr<ozz::animation::Animation> selected_animation;
    if (compression_mode == AnimationCompressionMode::ozz_hierarchical_key_reduction) {
        result.compression.optimizer_attempted = true;
        ozz::animation::offline::RawAnimation optimized_animation;
        ozz::animation::offline::AnimationOptimizer optimizer;
        result.compression.hierarchical_tolerance_meters = optimizer.setting.tolerance;
        result.compression.measurement_distance_meters = optimizer.setting.distance;
        if (optimizer(raw_animation, *compiled->skeleton, &optimized_animation)) {
            const auto optimized_keys = raw_animation_key_counts(optimized_animation);
            result.compression.optimized_raw_resident_bytes = optimized_animation.size();
            const auto optimized_raw_archive = archive_metrics(optimized_animation);
            result.compression.optimized_raw_archive_bytes = optimized_raw_archive.bytes;
            result.compression.optimized_raw_archive_hash = optimized_raw_archive.hash;
            result.compression.selected_translation_keys = optimized_keys.translations;
            result.compression.selected_rotation_keys = optimized_keys.rotations;
            result.compression.selected_scale_keys = optimized_keys.scales;
            selected_animation = ozz::animation::offline::AnimationBuilder{}(optimized_animation);
            result.compression.optimizer_applied = selected_animation != nullptr;
        }
        if (!selected_animation) {
            result.compression.fallback_used = true;
            result.compression.optimized_raw_resident_bytes = result.compression.input_raw_resident_bytes;
            result.compression.optimized_raw_archive_bytes = result.compression.input_raw_archive_bytes;
            result.compression.optimized_raw_archive_hash = result.compression.input_raw_archive_hash;
            result.compression.selected_translation_keys = input_keys.translations;
            result.compression.selected_rotation_keys = input_keys.rotations;
            result.compression.selected_scale_keys = input_keys.scales;
            selected_animation = std::move(baseline_animation);
        }
    } else {
        result.compression.optimized_raw_resident_bytes = result.compression.input_raw_resident_bytes;
        result.compression.optimized_raw_archive_bytes = result.compression.input_raw_archive_bytes;
        result.compression.optimized_raw_archive_hash = result.compression.input_raw_archive_hash;
        result.compression.selected_translation_keys = input_keys.translations;
        result.compression.selected_rotation_keys = input_keys.rotations;
        result.compression.selected_scale_keys = input_keys.scales;
        selected_animation = std::move(baseline_animation);
    }
    compiled->animation = std::move(selected_animation);
    result.compression.selected_runtime_resident_bytes = compiled->animation->size();
    const auto selected_archive = archive_metrics(*compiled->animation);
    result.compression.selected_runtime_archive_bytes = selected_archive.bytes;
    result.compression.selected_runtime_archive_hash = selected_archive.hash;
    compiled->compression = result.compression;
    compiled->skeleton_asset = std::string(asset_id) + "/skin/" + asset_token(skin.name);
    compiled->clip_asset = std::string(asset_id) + "/skin/" + asset_token(skin.name) + "/animation/" + asset_token(clip.name);
    compiled->inverse_bind_matrices.reserve(skin.joints.size());
    for (const auto& joint : skin.joints) compiled->inverse_bind_matrices.push_back(ozz_matrix(joint.inverse_bind_matrix));
    compiled->context.Resize(compiled->skeleton->num_joints());
    compiled->local_transforms.resize(static_cast<std::size_t>(compiled->skeleton->num_soa_joints()));
    compiled->model_transforms.resize(static_cast<std::size_t>(compiled->skeleton->num_joints()));
    result.success = true; result.code = "ok"; result.detail = "Validated scene skin and channels compiled into ozz runtime data.";
    result.skeleton_asset = compiled->skeleton_asset; result.clip_asset = compiled->clip_asset;
    result.joint_count = skin.joints.size(); result.duration = clip.duration;
    impl_->compiled_assets.insert_or_assign(result.clip_asset, std::move(compiled));
    return result;
}

AnimationClipComparisonResult AnimationRuntime::compare_compiled_clips(
    const std::string_view reference_clip_asset, const std::string_view candidate_clip_asset,
    const std::size_t sample_count) const {
    AnimationClipComparisonResult result;
    result.code = "animation.compression-comparison-invalid";
    if (sample_count < 2U || sample_count > 4097U) {
        result.detail = "Sample count must be between 2 and 4097.";
        return result;
    }
    const auto reference_found = impl_->compiled_assets.find(std::string(reference_clip_asset));
    const auto candidate_found = impl_->compiled_assets.find(std::string(candidate_clip_asset));
    if (reference_found == impl_->compiled_assets.end() || candidate_found == impl_->compiled_assets.end()) {
        result.code = "animation.compression-comparison-clip-missing";
        result.detail = "Both reference and candidate clips must be compiled in this runtime.";
        return result;
    }
    auto& reference = *reference_found->second;
    auto& candidate = *candidate_found->second;
    if (!reference.animation || !candidate.animation || !reference.skeleton || !candidate.skeleton ||
        reference.animation->duration() <= 0.0F || candidate.animation->duration() <= 0.0F ||
        reference.skeleton->num_joints() != candidate.skeleton->num_joints() ||
        reference.skin_to_runtime.size() != candidate.skin_to_runtime.size() ||
        reference.skin_to_runtime != candidate.skin_to_runtime ||
        reference.inverse_bind_matrices.size() != candidate.inverse_bind_matrices.size() ||
        reference.compression.skeleton_archive_hash.empty() ||
        reference.compression.skeleton_archive_hash != candidate.compression.skeleton_archive_hash ||
        std::abs(reference.animation->duration() - candidate.animation->duration()) > 0.000001F) {
        result.code = "animation.compression-comparison-incompatible";
        result.detail = "Clips must use equivalent skeletons, skin mappings, and durations.";
        return result;
    }
    for (std::size_t matrix_index = 0; matrix_index < reference.inverse_bind_matrices.size(); ++matrix_index) {
        for (std::size_t column = 0; column < 4U; ++column) {
            std::array<float, 4> reference_values{};
            std::array<float, 4> candidate_values{};
            ozz::math::StorePtrU(reference.inverse_bind_matrices[matrix_index].cols[column], reference_values.data());
            ozz::math::StorePtrU(candidate.inverse_bind_matrices[matrix_index].cols[column], candidate_values.data());
            for (std::size_t row = 0; row < 4U; ++row) {
                if (std::abs(reference_values[row] - candidate_values[row]) <= 0.000001F) continue;
                result.code = "animation.compression-comparison-incompatible";
                result.detail = "Clips use different inverse-bind transforms.";
                return result;
            }
        }
    }

    struct Sample final {
        std::vector<ozz::math::SoaTransform> local;
        std::vector<ozz::math::Float4x4> model;
    };
    const auto sample = [](Impl::CompiledAsset& asset, const float time, Sample& output) {
        output.local.resize(static_cast<std::size_t>(asset.skeleton->num_soa_joints()));
        output.model.resize(static_cast<std::size_t>(asset.skeleton->num_joints()));
        ozz::animation::SamplingJob sampling;
        sampling.animation = asset.animation.get();
        sampling.context = &asset.context;
        sampling.ratio = std::clamp(time / asset.animation->duration(), 0.0F, 1.0F);
        sampling.output = ozz::make_span(output.local);
        if (!sampling.Run()) return false;
        ozz::animation::LocalToModelJob local_to_model;
        local_to_model.skeleton = asset.skeleton.get();
        local_to_model.input = ozz::make_span(output.local);
        local_to_model.output = ozz::make_span(output.model);
        return local_to_model.Run();
    };
    const auto distance3 = [](const float* left, const float* right) {
        const auto x = left[0] - right[0];
        const auto y = left[1] - right[1];
        const auto z = left[2] - right[2];
        return std::sqrt(x * x + y * y + z * z);
    };

    Sample reference_sample;
    Sample candidate_sample;
    std::array<float, 3> previous_reference_root{};
    std::array<float, 3> previous_candidate_root{};
    const auto duration_seconds = reference.animation->duration();
    const auto joint_count = static_cast<std::size_t>(reference.skeleton->num_joints());
    for (std::size_t sample_index = 0; sample_index < sample_count; ++sample_index) {
        const auto ratio = static_cast<float>(sample_index) / static_cast<float>(sample_count - 1U);
        const auto time = duration_seconds * ratio;
        if (!sample(reference, time, reference_sample) || !sample(candidate, time, candidate_sample)) {
            result.code = "animation.compression-comparison-sampling-failed";
            result.detail = "ozz rejected a deterministic comparison sample.";
            return result;
        }
        for (std::size_t soa_index = 0; soa_index < reference_sample.local.size(); ++soa_index) {
            ozz::math::SimdFloat4 reference_translations[4];
            ozz::math::SimdFloat4 candidate_translations[4];
            ozz::math::SimdFloat4 reference_rotations[4];
            ozz::math::SimdFloat4 candidate_rotations[4];
            ozz::math::SimdFloat4 reference_scales[4];
            ozz::math::SimdFloat4 candidate_scales[4];
            ozz::math::Transpose3x4(&reference_sample.local[soa_index].translation.x, reference_translations);
            ozz::math::Transpose3x4(&candidate_sample.local[soa_index].translation.x, candidate_translations);
            ozz::math::Transpose4x4(&reference_sample.local[soa_index].rotation.x, reference_rotations);
            ozz::math::Transpose4x4(&candidate_sample.local[soa_index].rotation.x, candidate_rotations);
            ozz::math::Transpose3x4(&reference_sample.local[soa_index].scale.x, reference_scales);
            ozz::math::Transpose3x4(&candidate_sample.local[soa_index].scale.x, candidate_scales);
            for (std::size_t lane = 0; lane < 4U; ++lane) {
                const auto joint = soa_index * 4U + lane;
                if (joint >= joint_count) break;
                std::array<float, 4> reference_translation{};
                std::array<float, 4> candidate_translation{};
                std::array<float, 4> reference_rotation{};
                std::array<float, 4> candidate_rotation{};
                std::array<float, 4> reference_scale{};
                std::array<float, 4> candidate_scale{};
                ozz::math::StorePtrU(reference_translations[lane], reference_translation.data());
                ozz::math::StorePtrU(candidate_translations[lane], candidate_translation.data());
                ozz::math::StorePtrU(reference_rotations[lane], reference_rotation.data());
                ozz::math::StorePtrU(candidate_rotations[lane], candidate_rotation.data());
                ozz::math::StorePtrU(reference_scales[lane], reference_scale.data());
                ozz::math::StorePtrU(candidate_scales[lane], candidate_scale.data());
                result.maximum_local_translation_error_meters = std::max(
                    result.maximum_local_translation_error_meters,
                    distance3(reference_translation.data(), candidate_translation.data()));
                const auto dot = std::abs(reference_rotation[0] * candidate_rotation[0] +
                    reference_rotation[1] * candidate_rotation[1] +
                    reference_rotation[2] * candidate_rotation[2] +
                    reference_rotation[3] * candidate_rotation[3]);
                const auto reference_length = std::sqrt(reference_rotation[0] * reference_rotation[0] +
                    reference_rotation[1] * reference_rotation[1] + reference_rotation[2] * reference_rotation[2] +
                    reference_rotation[3] * reference_rotation[3]);
                const auto candidate_length = std::sqrt(candidate_rotation[0] * candidate_rotation[0] +
                    candidate_rotation[1] * candidate_rotation[1] + candidate_rotation[2] * candidate_rotation[2] +
                    candidate_rotation[3] * candidate_rotation[3]);
                const auto normalized_dot = reference_length > 0.0F && candidate_length > 0.0F
                    ? dot / (reference_length * candidate_length) : 0.0F;
                result.maximum_local_rotation_error_degrees = std::max(
                    result.maximum_local_rotation_error_degrees,
                    2.0F * std::acos(std::clamp(normalized_dot, 0.0F, 1.0F)) * 57.29577951308232F);
                for (std::size_t axis = 0; axis < 3U; ++axis) {
                    result.maximum_local_scale_error = std::max(result.maximum_local_scale_error,
                        std::abs(reference_scale[axis] - candidate_scale[axis]));
                }
            }
        }
        for (std::size_t joint = 0; joint < joint_count; ++joint) {
            std::array<std::array<float, 4>, 4> reference_model{};
            std::array<std::array<float, 4>, 4> candidate_model{};
            for (std::size_t column = 0; column < 4U; ++column) {
                ozz::math::StorePtrU(reference_sample.model[joint].cols[column], reference_model[column].data());
                ozz::math::StorePtrU(candidate_sample.model[joint].cols[column], candidate_model[column].data());
            }
            result.maximum_model_translation_error_meters = std::max(
                result.maximum_model_translation_error_meters,
                distance3(reference_model[3].data(), candidate_model[3].data()));
            for (std::size_t axis = 0; axis < 3U; ++axis) {
                std::array<float, 3> reference_probe{};
                std::array<float, 3> candidate_probe{};
                for (std::size_t component = 0; component < 3U; ++component) {
                    reference_probe[component] = reference_model[3][component] +
                        reference_model[axis][component] * reference.compression.measurement_distance_meters;
                    candidate_probe[component] = candidate_model[3][component] +
                        candidate_model[axis][component] * candidate.compression.measurement_distance_meters;
                }
                result.maximum_model_probe_error_meters = std::max(result.maximum_model_probe_error_meters,
                    distance3(reference_probe.data(), candidate_probe.data()));
            }
        }
        for (std::size_t skin_joint = 0; skin_joint < reference.skin_to_runtime.size(); ++skin_joint) {
            const auto reference_skinning = reference_sample.model[reference.skin_to_runtime[skin_joint]] *
                reference.inverse_bind_matrices[skin_joint];
            const auto candidate_skinning = candidate_sample.model[candidate.skin_to_runtime[skin_joint]] *
                candidate.inverse_bind_matrices[skin_joint];
            for (std::size_t column = 0; column < 4U; ++column) {
                std::array<float, 4> reference_values{};
                std::array<float, 4> candidate_values{};
                ozz::math::StorePtrU(reference_skinning.cols[column], reference_values.data());
                ozz::math::StorePtrU(candidate_skinning.cols[column], candidate_values.data());
                for (std::size_t row = 0; row < 4U; ++row) {
                    result.maximum_skinning_matrix_absolute_error = std::max(
                        result.maximum_skinning_matrix_absolute_error,
                        std::abs(reference_values[row] - candidate_values[row]));
                }
            }
        }
        std::array<float, 4> reference_root{};
        std::array<float, 4> candidate_root{};
        ozz::math::StorePtrU(reference_sample.model.front().cols[3], reference_root.data());
        ozz::math::StorePtrU(candidate_sample.model.front().cols[3], candidate_root.data());
        if (sample_index > 0U) {
            const std::array<float, 3> reference_delta{
                reference_root[0] - previous_reference_root[0],
                reference_root[1] - previous_reference_root[1],
                reference_root[2] - previous_reference_root[2]};
            const std::array<float, 3> candidate_delta{
                candidate_root[0] - previous_candidate_root[0],
                candidate_root[1] - previous_candidate_root[1],
                candidate_root[2] - previous_candidate_root[2]};
            result.maximum_root_motion_delta_error_meters = std::max(
                result.maximum_root_motion_delta_error_meters,
                distance3(reference_delta.data(), candidate_delta.data()));
        }
        std::copy_n(reference_root.begin(), 3U, previous_reference_root.begin());
        std::copy_n(candidate_root.begin(), 3U, previous_candidate_root.begin());
    }
    result.success = true;
    result.code = "ok";
    result.detail = "Compiled clips matched across the fixed local/model/root-motion sample schedule.";
    result.sample_count = sample_count;
    result.joint_count = joint_count;
    return result;
}

AnimationCookedArtifactResult AnimationRuntime::cook_gltf_animation_artifact(
    const std::string_view asset_id, const std::string_view source_hash, const GltfMeshData& source,
    const std::size_t skin_index, const std::size_t animation_index,
    const AnimationCompressionMode compression_mode) {
    using Json = nlohmann::json;
    AnimationCookedArtifactResult result;
    result.code = "animation.artifact-cook-invalid";
    result.asset_id = std::string(asset_id);
    result.source_hash = std::string(source_hash);
    if (asset_id.empty() || asset_id.size() > 4096U || source_hash.size() != 71U ||
        !source_hash.starts_with("sha256:")) {
        result.detail = "Cooked animation requires a bounded asset ID and sha256 source identity.";
        return result;
    }

    AnimationRuntime compiler;
    const auto compile_id = std::string("artifact-cook/") + std::string(asset_id);
    const auto compiled = compiler.compile_gltf_asset(
        compile_id, source, skin_index, animation_index, compression_mode);
    if (!compiled.success) {
        result.code = compiled.code;
        result.detail = compiled.detail;
        return result;
    }
    const auto found = compiler.impl_->compiled_assets.find(compiled.clip_asset);
    if (found == compiler.impl_->compiled_assets.end() || !found->second->skeleton || !found->second->animation) {
        result.code = "animation.artifact-cook-compile-missing";
        result.detail = "The offline compiler did not retain its validated runtime objects.";
        return result;
    }
    const auto& runtime_asset = *found->second;
    const auto skeleton_archive = archive_payload(*runtime_asset.skeleton);
    const auto animation_archive = archive_payload(*runtime_asset.animation);
    if (skeleton_archive.empty() || animation_archive.empty()) {
        result.code = "animation.artifact-archive-write-failed";
        result.detail = "ozz runtime archives could not be materialized.";
        return result;
    }
    if (runtime_asset.skin_to_runtime.size() > maximum_animation_artifact_joints ||
        runtime_asset.inverse_bind_matrices.size() != runtime_asset.skin_to_runtime.size()) {
        result.code = "animation.artifact-joint-limit";
        result.detail = "The compiled skin exceeds the cooked artifact joint contract.";
        return result;
    }

    std::vector<std::byte> mapping_bytes;
    mapping_bytes.reserve(runtime_asset.skin_to_runtime.size() * sizeof(std::uint32_t));
    for (const auto mapping : runtime_asset.skin_to_runtime) {
        if (mapping > std::numeric_limits<std::uint32_t>::max()) {
            result.code = "animation.artifact-mapping-invalid";
            result.detail = "A skin mapping cannot be encoded as u32 little-endian.";
            return result;
        }
        append_u32_le(mapping_bytes, static_cast<std::uint32_t>(mapping));
    }
    std::vector<std::byte> inverse_bind_bytes;
    inverse_bind_bytes.reserve(runtime_asset.inverse_bind_matrices.size() * 16U * sizeof(float));
    for (const auto& matrix : runtime_asset.inverse_bind_matrices) {
        for (std::size_t column = 0U; column < 4U; ++column) {
            std::array<float, 4> values{};
            ozz::math::StorePtrU(matrix.cols[column], values.data());
            for (const auto value : values) append_u32_le(inverse_bind_bytes, std::bit_cast<std::uint32_t>(value));
        }
    }

    const auto skeleton_hash = sha256_bytes(skeleton_archive);
    const auto animation_hash = sha256_bytes(animation_archive);
    const auto mapping_hash = sha256_bytes(mapping_bytes);
    const auto inverse_bind_hash = sha256_bytes(inverse_bind_bytes);
    if (!skeleton_hash.success || !animation_hash.success || !mapping_hash.success || !inverse_bind_hash.success) {
        result.code = "animation.artifact-hash-failed";
        result.detail = "A cooked animation section could not be hashed.";
        return result;
    }
    const auto clip_asset = std::string(asset_id);
    const Json manifest = {
        {"schema", result.schema_version},
        {"assetId", result.asset_id},
        {"sourceHash", result.source_hash},
        {"backend", "ozz-animation/0.17.0"},
        {"endianness", "little"},
        {"skeletonAsset", result.asset_id + "/skeleton"},
        {"clipAsset", clip_asset},
        {"durationSeconds", runtime_asset.animation->duration()},
        {"jointCount", runtime_asset.skin_to_runtime.size()},
        {"trackCount", runtime_asset.animation->num_tracks()},
        {"compression", {
            {"mode", compression_mode_name(compression_mode)},
            {"optimizerAttempted", runtime_asset.compression.optimizer_attempted},
            {"optimizerApplied", runtime_asset.compression.optimizer_applied},
            {"fallbackUsed", runtime_asset.compression.fallback_used},
            {"toleranceMeters", runtime_asset.compression.hierarchical_tolerance_meters},
            {"measurementDistanceMeters", runtime_asset.compression.measurement_distance_meters}}},
        {"sections", {
            {"skeleton", {{"archiveTag", "ozz-skeleton"}, {"archiveVersion", 2},
                {"bytes", skeleton_archive.size()}, {"sha256", skeleton_hash.value}}},
            {"animation", {{"archiveTag", "ozz-animation"}, {"archiveVersion", 7},
                {"bytes", animation_archive.size()}, {"sha256", animation_hash.value}}},
            {"skinMapping", {{"encoding", "u32-le"}, {"count", runtime_asset.skin_to_runtime.size()},
                {"bytes", mapping_bytes.size()}, {"sha256", mapping_hash.value}}},
            {"inverseBind", {{"encoding", "float32x16-column-major-le"},
                {"count", runtime_asset.inverse_bind_matrices.size()}, {"bytes", inverse_bind_bytes.size()},
                {"sha256", inverse_bind_hash.value}}}}}
    };
    const auto manifest_text = manifest.dump();
    if (manifest_text.size() > maximum_animation_manifest_bytes ||
        manifest_text.size() > std::numeric_limits<std::uint32_t>::max()) {
        result.code = "animation.artifact-manifest-too-large";
        result.detail = "The cooked animation manifest exceeds its byte budget.";
        return result;
    }

    result.payload.reserve(animation_artifact_header_bytes + manifest_text.size() + skeleton_archive.size() +
        animation_archive.size() + mapping_bytes.size() + inverse_bind_bytes.size());
    result.payload.insert(result.payload.end(), animation_artifact_magic.begin(), animation_artifact_magic.end());
    append_u32_le(result.payload, animation_artifact_version);
    append_u32_le(result.payload, animation_artifact_endian);
    append_u32_le(result.payload, static_cast<std::uint32_t>(manifest_text.size()));
    append_u32_le(result.payload, static_cast<std::uint32_t>(runtime_asset.skin_to_runtime.size()));
    append_u32_le(result.payload, static_cast<std::uint32_t>(runtime_asset.inverse_bind_matrices.size()));
    append_u32_le(result.payload, 0U);
    append_u64_le(result.payload, static_cast<std::uint64_t>(skeleton_archive.size()));
    append_u64_le(result.payload, static_cast<std::uint64_t>(animation_archive.size()));
    result.payload.insert(result.payload.end(), reinterpret_cast<const std::byte*>(manifest_text.data()),
        reinterpret_cast<const std::byte*>(manifest_text.data() + manifest_text.size()));
    result.payload.insert(result.payload.end(), skeleton_archive.begin(), skeleton_archive.end());
    result.payload.insert(result.payload.end(), animation_archive.begin(), animation_archive.end());
    result.payload.insert(result.payload.end(), mapping_bytes.begin(), mapping_bytes.end());
    result.payload.insert(result.payload.end(), inverse_bind_bytes.begin(), inverse_bind_bytes.end());
    if (result.payload.size() > maximum_animation_artifact_bytes) {
        result.payload.clear();
        result.code = "animation.artifact-too-large";
        result.detail = "The cooked animation payload exceeds the runtime byte budget.";
        return result;
    }
    const auto payload_hash = sha256_bytes(result.payload);
    if (!payload_hash.success) {
        result.payload.clear();
        result.code = payload_hash.code;
        result.detail = payload_hash.detail;
        return result;
    }
    result.payload_hash = payload_hash.value;
    result.clip_assets = {clip_asset};
    result.joint_count = runtime_asset.skin_to_runtime.size();
    result.success = true;
    result.code = "ok";
    result.detail = "Source animation compiled into a versioned, bounded ozz runtime artifact.";
    return result;
}

AnimationCookedArtifactLoadResult AnimationRuntime::load_cooked_animation_artifact(
    const std::span<const std::byte> payload, const std::string_view expected_asset_id,
    const std::string_view expected_source_hash, const std::string_view expected_payload_hash) {
    using Json = nlohmann::json;
    AnimationCookedArtifactLoadResult result;
    result.code = "animation.artifact-header-invalid";
    try {
    if (payload.size() < animation_artifact_header_bytes || payload.size() > maximum_animation_artifact_bytes ||
        !std::equal(animation_artifact_magic.begin(), animation_artifact_magic.end(), payload.begin())) {
        result.detail = "Cooked animation magic or total byte count is invalid.";
        return result;
    }
    const auto version = read_u32_le(payload, 8U);
    const auto endian = read_u32_le(payload, 12U);
    const auto manifest_size_value = read_u32_le(payload, 16U);
    const auto mapping_count_value = read_u32_le(payload, 20U);
    const auto inverse_count_value = read_u32_le(payload, 24U);
    const auto reserved = read_u32_le(payload, 28U);
    const auto skeleton_size_value = read_u64_le(payload, 32U);
    const auto animation_size_value = read_u64_le(payload, 40U);
    if (!version || !endian || !manifest_size_value || !mapping_count_value || !inverse_count_value || !reserved ||
        !skeleton_size_value || !animation_size_value || *version != animation_artifact_version ||
        *endian != animation_artifact_endian || *reserved != 0U) {
        result.detail = "Cooked animation header version or endianness is unsupported.";
        return result;
    }
    const auto manifest_size = static_cast<std::size_t>(*manifest_size_value);
    const auto mapping_count = static_cast<std::size_t>(*mapping_count_value);
    const auto inverse_count = static_cast<std::size_t>(*inverse_count_value);
    if (manifest_size == 0U || manifest_size > maximum_animation_manifest_bytes ||
        mapping_count == 0U || mapping_count > maximum_animation_artifact_joints ||
        inverse_count != mapping_count || *skeleton_size_value > maximum_animation_artifact_bytes ||
        *animation_size_value > maximum_animation_artifact_bytes) {
        result.code = "animation.artifact-range-invalid";
        result.detail = "Cooked animation section counts exceed the runtime contract.";
        return result;
    }
    const auto skeleton_size = static_cast<std::size_t>(*skeleton_size_value);
    const auto animation_size = static_cast<std::size_t>(*animation_size_value);
    const auto mapping_bytes_size = mapping_count * sizeof(std::uint32_t);
    const auto inverse_bytes_size = inverse_count * 16U * sizeof(float);
    const auto checked_add = [](const std::size_t left, const std::size_t right) -> std::optional<std::size_t> {
        if (right > std::numeric_limits<std::size_t>::max() - left) return std::nullopt;
        return left + right;
    };
    auto total = checked_add(animation_artifact_header_bytes, manifest_size);
    if (total) total = checked_add(*total, skeleton_size);
    if (total) total = checked_add(*total, animation_size);
    if (total) total = checked_add(*total, mapping_bytes_size);
    if (total) total = checked_add(*total, inverse_bytes_size);
    if (!total || *total != payload.size()) {
        result.code = "animation.artifact-range-invalid";
        result.detail = "Cooked animation sections do not exactly cover the payload.";
        return result;
    }
    const auto payload_hash = sha256_bytes(payload);
    if (!payload_hash.success) {
        result.code = payload_hash.code;
        result.detail = payload_hash.detail;
        return result;
    }
    result.payload_hash = payload_hash.value;
    if (!expected_payload_hash.empty() && expected_payload_hash != result.payload_hash) {
        result.code = "animation.artifact-hash-mismatch";
        result.detail = "Cooked animation payload hash does not match the Registry identity.";
        return result;
    }

    const auto manifest_begin = animation_artifact_header_bytes;
    const auto skeleton_begin = manifest_begin + manifest_size;
    const auto animation_begin = skeleton_begin + skeleton_size;
    const auto mapping_begin = animation_begin + animation_size;
    const auto inverse_begin = mapping_begin + mapping_bytes_size;
    const auto manifest_text = std::string(reinterpret_cast<const char*>(payload.data() + manifest_begin), manifest_size);
    const auto manifest = Json::parse(manifest_text, nullptr, false);
    if (manifest.is_discarded() || !manifest.is_object() ||
        manifest.value("schema", std::string{}) != result.schema_version ||
        manifest.value("backend", std::string{}) != "ozz-animation/0.17.0" ||
        manifest.value("endianness", std::string{}) != "little") {
        result.code = "animation.artifact-manifest-invalid";
        result.detail = "Cooked animation manifest schema, backend or endianness is invalid.";
        return result;
    }
    result.asset_id = manifest.value("assetId", std::string{});
    result.source_hash = manifest.value("sourceHash", std::string{});
    const auto clip_asset = manifest.value("clipAsset", std::string{});
    const auto skeleton_asset = manifest.value("skeletonAsset", std::string{});
    const auto manifest_joint_count = manifest.value("jointCount", std::size_t{});
    const auto manifest_track_count = manifest.value("trackCount", std::size_t{});
    const auto manifest_duration = manifest.value("durationSeconds", 0.0F);
    if (result.asset_id.empty() || clip_asset != result.asset_id || skeleton_asset.empty() ||
        result.source_hash.size() != 71U || !result.source_hash.starts_with("sha256:") ||
        manifest_joint_count != mapping_count || manifest_track_count == 0U ||
        !std::isfinite(manifest_duration) || manifest_duration <= 0.0F) {
        result.code = "animation.artifact-manifest-invalid";
        result.detail = "Cooked animation identity, counts or duration is invalid.";
        return result;
    }
    if (!expected_asset_id.empty() && expected_asset_id != result.asset_id) {
        result.code = "animation.artifact-asset-id-mismatch";
        result.detail = "Cooked animation asset ID does not match the requested Registry asset.";
        return result;
    }
    if (!expected_source_hash.empty() && expected_source_hash != result.source_hash) {
        result.code = "animation.artifact-source-hash-mismatch";
        result.detail = "Cooked animation source identity does not match the expected Cook input.";
        return result;
    }
    if (!manifest.contains("sections") || !manifest.at("sections").is_object() ||
        !manifest.contains("compression") || !manifest.at("compression").is_object()) {
        result.code = "animation.artifact-manifest-invalid";
        result.detail = "Cooked animation section metadata is missing.";
        return result;
    }
    const auto& declared_sections=manifest.at("sections");
    if(!declared_sections.contains("skeleton")||!declared_sections.at("skeleton").is_object()||
        declared_sections.at("skeleton").value("archiveTag",std::string{})!="ozz-skeleton"||
        declared_sections.at("skeleton").value("archiveVersion",0)!=2||
        !declared_sections.contains("animation")||!declared_sections.at("animation").is_object()||
        declared_sections.at("animation").value("archiveTag",std::string{})!="ozz-animation"||
        declared_sections.at("animation").value("archiveVersion",0)!=7) {
        result.code="animation.artifact-archive-version-unsupported";
        result.detail="Cooked animation archive tag or serialization version is unsupported.";
        return result;
    }
    const auto section_hash_matches = [&](const std::string_view name, const std::span<const std::byte> bytes,
                                          const std::size_t expected_bytes) {
        const auto& sections = manifest.at("sections");
        const auto key = std::string(name);
        if (!sections.contains(key) || !sections.at(key).is_object()) return false;
        const auto& section = sections.at(key);
        const auto hash = sha256_bytes(bytes);
        return hash.success && section.value("bytes", std::size_t{}) == expected_bytes &&
            section.value("sha256", std::string{}) == hash.value;
    };
    const auto skeleton_bytes = payload.subspan(skeleton_begin, skeleton_size);
    const auto animation_bytes = payload.subspan(animation_begin, animation_size);
    const auto mapping_bytes = payload.subspan(mapping_begin, mapping_bytes_size);
    const auto inverse_bytes = payload.subspan(inverse_begin, inverse_bytes_size);
    if (!section_hash_matches("skeleton", skeleton_bytes, skeleton_size) ||
        !section_hash_matches("animation", animation_bytes, animation_size) ||
        !section_hash_matches("skinMapping", mapping_bytes, mapping_bytes_size) ||
        !section_hash_matches("inverseBind", inverse_bytes, inverse_bytes_size)) {
        result.code = "animation.artifact-section-hash-mismatch";
        result.detail = "A cooked animation section failed its integrity contract.";
        return result;
    }

    std::vector<std::size_t> skin_to_runtime;
    skin_to_runtime.reserve(mapping_count);
    std::vector<bool> mapped(mapping_count, false);
    for (std::size_t index = 0U; index < mapping_count; ++index) {
        const auto mapping = read_u32_le(mapping_bytes, index * sizeof(std::uint32_t));
        if (!mapping || *mapping >= mapping_count || mapped[*mapping]) {
            result.code = "animation.artifact-mapping-invalid";
            result.detail = "Cooked animation skin mapping is not a bounded permutation.";
            return result;
        }
        mapped[*mapping] = true;
        skin_to_runtime.push_back(*mapping);
    }
    std::vector<ozz::math::Float4x4> inverse_bind_matrices;
    inverse_bind_matrices.reserve(inverse_count);
    for (std::size_t matrix_index = 0U; matrix_index < inverse_count; ++matrix_index) {
        std::array<float, 16> values{};
        for (std::size_t value_index = 0U; value_index < values.size(); ++value_index) {
            const auto bits = read_u32_le(inverse_bytes,
                (matrix_index * values.size() + value_index) * sizeof(std::uint32_t));
            if (!bits) {
                result.code = "animation.artifact-inverse-bind-invalid";
                result.detail = "Cooked inverse-bind matrix is truncated.";
                return result;
            }
            values[value_index] = std::bit_cast<float>(*bits);
            if (!std::isfinite(values[value_index])) {
                result.code = "animation.artifact-inverse-bind-invalid";
                result.detail = "Cooked inverse-bind matrix contains a non-finite value.";
                return result;
            }
        }
        inverse_bind_matrices.push_back(ozz_matrix(values));
    }

    if (!preflight_ozz_skeleton_archive(skeleton_bytes, mapping_count) ||
        !preflight_ozz_animation_archive(animation_bytes, manifest_track_count, manifest_duration)) {
        result.code = "animation.artifact-archive-invalid";
        result.detail = "Cooked ozz archive layout, bounds or scalar values are invalid.";
        return result;
    }

    const auto load_skeleton = [](const std::span<const std::byte> bytes,
                                  ozz::unique_ptr<ozz::animation::Skeleton>& output) {
        ozz::io::MemoryStream stream;
        if (stream.Write(bytes.data(), bytes.size()) != bytes.size() || stream.Seek(0, ozz::io::Stream::kSet) != 0)
            return false;
        ozz::io::IArchive archive(&stream);
        if (!archive.TestTag<ozz::animation::Skeleton>()) return false;
        auto candidate = ozz::make_unique<ozz::animation::Skeleton>();
        archive >> *candidate;
        output = std::move(candidate);
        return true;
    };
    const auto load_animation = [](const std::span<const std::byte> bytes,
                                   ozz::unique_ptr<ozz::animation::Animation>& output) {
        ozz::io::MemoryStream stream;
        if (stream.Write(bytes.data(), bytes.size()) != bytes.size() || stream.Seek(0, ozz::io::Stream::kSet) != 0)
            return false;
        ozz::io::IArchive archive(&stream);
        if (!archive.TestTag<ozz::animation::Animation>()) return false;
        auto candidate = ozz::make_unique<ozz::animation::Animation>();
        archive >> *candidate;
        output = std::move(candidate);
        return true;
    };
    auto candidate = std::make_unique<Impl::CompiledAsset>();
    if (!load_skeleton(skeleton_bytes, candidate->skeleton) ||
        !load_animation(animation_bytes, candidate->animation) || !candidate->skeleton || !candidate->animation ||
        static_cast<std::size_t>(candidate->skeleton->num_joints()) != mapping_count ||
        static_cast<std::size_t>(candidate->animation->num_tracks()) != manifest_track_count ||
        static_cast<std::size_t>(candidate->animation->num_tracks()) != mapping_count ||
        !std::isfinite(candidate->animation->duration()) || candidate->animation->duration() <= 0.0F ||
        std::abs(candidate->animation->duration() - manifest_duration) > 0.000001F) {
        result.code = "animation.artifact-archive-invalid";
        result.detail = "Cooked ozz archive tags or decoded runtime counts are invalid.";
        return result;
    }
    candidate->skeleton_asset = skeleton_asset;
    candidate->clip_asset = clip_asset;
    candidate->skin_to_runtime = std::move(skin_to_runtime);
    candidate->inverse_bind_matrices = std::move(inverse_bind_matrices);
    candidate->compression.requested_mode = manifest.at("compression").value("mode", std::string{}) ==
            "ozz_hierarchical_key_reduction"
        ? AnimationCompressionMode::ozz_hierarchical_key_reduction
        : AnimationCompressionMode::ozz_runtime_baseline;
    candidate->compression.optimizer_attempted = manifest.at("compression").value("optimizerAttempted", false);
    candidate->compression.optimizer_applied = manifest.at("compression").value("optimizerApplied", false);
    candidate->compression.fallback_used = manifest.at("compression").value("fallbackUsed", false);
    candidate->compression.hierarchical_tolerance_meters =
        manifest.at("compression").value("toleranceMeters", 0.001F);
    candidate->compression.measurement_distance_meters =
        manifest.at("compression").value("measurementDistanceMeters", 0.1F);
    candidate->compression.skeleton_archive_bytes = skeleton_size;
    candidate->compression.skeleton_archive_hash = fnv1a64_hash(
        std::vector<std::byte>(skeleton_bytes.begin(), skeleton_bytes.end()));
    candidate->compression.selected_runtime_archive_bytes = animation_size;
    candidate->compression.selected_runtime_archive_hash = fnv1a64_hash(
        std::vector<std::byte>(animation_bytes.begin(), animation_bytes.end()));
    candidate->compression.selected_runtime_resident_bytes = candidate->animation->size();
    candidate->context.Resize(candidate->skeleton->num_joints());
    candidate->local_transforms.resize(static_cast<std::size_t>(candidate->skeleton->num_soa_joints()));
    candidate->model_transforms.resize(static_cast<std::size_t>(candidate->skeleton->num_joints()));

    // Publish only after the complete envelope, every section and both codec
    // objects have validated. A failed reload therefore preserves the prior clip.
    impl_->compiled_assets.insert_or_assign(clip_asset, std::move(candidate));
    result.clip_assets = {clip_asset};
    result.joint_count = mapping_count;
    result.success = true;
    result.code = "ok";
    result.detail = "Cooked animation loaded transactionally without source decode or offline compilation.";
    return result;
    } catch(const std::exception& error) {
        result.success=false;
        result.code="animation.artifact-manifest-invalid";
        result.detail=std::string("Cooked animation validation failed without publishing state: ")+error.what();
        return result;
    }
}

float AnimationRuntime::duration(const std::string_view clip_asset) const noexcept {
    if (clip_asset == "asset.animation.test-bob") return 2.0F;
    const auto found = impl_->compiled_assets.find(std::string(clip_asset));
    return found == impl_->compiled_assets.end() ? 0.0F : found->second->animation->duration();
}

float AnimationRuntime::advance_time(float time, const float delta_seconds, const float speed, const bool looping,
                                     const bool playing, const std::string_view clip_asset) const noexcept {
    const auto clip_duration = duration(clip_asset);
    if (!playing || clip_duration <= 0.0F) return time;
    time += std::max(0.0F, delta_seconds) * speed;
    if (looping) {
        time = std::fmod(time, clip_duration);
        if (time < 0.0F) time += clip_duration;
    } else time = std::clamp(time, 0.0F, clip_duration);
    return time;
}

float AnimationRuntime::sample_translation_y(const std::string_view clip_asset, const float time) const noexcept {
    if (clip_asset != "asset.animation.test-bob") return 0.0F;
    // Deterministic four-key linear clip: rest -> up -> rest -> down -> rest.
    constexpr AnimationKeyframe keys[]{{0.0F, 0.0F}, {0.5F, 0.22F}, {1.0F, 0.0F}, {1.5F, -0.08F}, {2.0F, 0.0F}};
    const auto t = std::clamp(time, 0.0F, 2.0F);
    for (std::size_t index = 1; index < std::size(keys); ++index) {
        if (t <= keys[index].time) {
            const auto alpha = (t - keys[index - 1].time) / (keys[index].time - keys[index - 1].time);
            return keys[index - 1].value + (keys[index].value - keys[index - 1].value) * alpha;
        }
    }
    return 0.0F;
}

SkeletalPose AnimationRuntime::sample_skeletal_pose(const std::string_view clip_asset, const float time) const {
    SkeletalPose pose;
    pose.clip_asset = std::string(clip_asset);
    const auto compiled_found = impl_->compiled_assets.find(std::string(clip_asset));
    if (compiled_found != impl_->compiled_assets.end()) {
        auto& asset = *compiled_found->second;
        pose.skeleton_asset = asset.skeleton_asset;
        const float ratio = std::clamp(time / asset.animation->duration(), 0.0F, 1.0F);
        ozz::animation::SamplingJob sampling;
        sampling.animation = asset.animation.get(); sampling.context = &asset.context; sampling.ratio = ratio;
        sampling.output = ozz::make_span(asset.local_transforms);
        if (!sampling.Run()) return pose;
        ozz::animation::LocalToModelJob local_to_model;
        local_to_model.skeleton = asset.skeleton.get(); local_to_model.input = ozz::make_span(asset.local_transforms);
        local_to_model.output = ozz::make_span(asset.model_transforms);
        if (!local_to_model.Run()) return pose;
        pose.skinning_matrices.resize(asset.skin_to_runtime.size());
        for (std::size_t skin_joint = 0; skin_joint < asset.skin_to_runtime.size(); ++skin_joint) {
            const auto skinning = asset.model_transforms[asset.skin_to_runtime[skin_joint]] * asset.inverse_bind_matrices[skin_joint];
            for (std::size_t column = 0; column < 4U; ++column)
                ozz::math::StorePtrU(skinning.cols[column], pose.skinning_matrices[skin_joint].data() + column * 4U);
        }
        append_joint_debug(pose, *asset.skeleton, asset.model_transforms);
        pose.valid = !pose.skinning_matrices.empty();
        return pose;
    }
    pose.skeleton_asset = "asset.skeleton.test-two-joint";
    if (clip_asset != "asset.animation.test-bob" || !impl_->skeleton || !impl_->animation) return pose;
    const float ratio = std::clamp(time / impl_->animation->duration(), 0.0F, 1.0F);
    ozz::animation::SamplingJob sampling;
    sampling.animation = impl_->animation.get();
    sampling.context = &impl_->context;
    sampling.ratio = ratio;
    sampling.output = ozz::make_span(impl_->local_transforms);
    if (!sampling.Run()) return pose;
    ozz::animation::LocalToModelJob local_to_model;
    local_to_model.skeleton = impl_->skeleton.get();
    local_to_model.input = ozz::make_span(impl_->local_transforms);
    local_to_model.output = ozz::make_span(impl_->model_transforms);
    if (!local_to_model.Run()) return pose;
    pose.skinning_matrices.resize(impl_->model_transforms.size());
    for (std::size_t joint = 0; joint < impl_->model_transforms.size(); ++joint) {
        for (std::size_t column = 0; column < 4U; ++column) {
            ozz::math::StorePtrU(impl_->model_transforms[joint].cols[column],
                                 pose.skinning_matrices[joint].data() + column * 4U);
        }
    }
    append_joint_debug(pose, *impl_->skeleton, impl_->model_transforms);
    pose.valid = !pose.skinning_matrices.empty() && pose.skinning_matrices.size() <= SkeletalPose::maximum_joints;
    return pose;
}

SkeletalPose AnimationRuntime::sample_blended_skeletal_pose(const std::string_view source_clip, const float source_time,
                                                            const std::string_view target_clip, const float target_time,
                                                            const float target_weight) const {
    const auto source_found = impl_->compiled_assets.find(std::string(source_clip));
    const auto target_found = impl_->compiled_assets.find(std::string(target_clip));
    if (source_found == impl_->compiled_assets.end() || target_found == impl_->compiled_assets.end() ||
        source_found->second->skeleton_asset != target_found->second->skeleton_asset ||
        source_found->second->skin_to_runtime.size() != target_found->second->skin_to_runtime.size()) {
        return target_weight >= 0.5F ? sample_skeletal_pose(target_clip, target_time) :
            sample_skeletal_pose(source_clip, source_time);
    }
    auto& source = *source_found->second;
    auto& target = *target_found->second;
    ozz::animation::SamplingJob source_sampling;
    source_sampling.animation = source.animation.get(); source_sampling.context = &source.context;
    source_sampling.ratio = std::clamp(source_time / source.animation->duration(), 0.0F, 1.0F);
    source_sampling.output = ozz::make_span(source.local_transforms);
    ozz::animation::SamplingJob target_sampling;
    target_sampling.animation = target.animation.get(); target_sampling.context = &target.context;
    target_sampling.ratio = std::clamp(target_time / target.animation->duration(), 0.0F, 1.0F);
    target_sampling.output = ozz::make_span(target.local_transforms);
    SkeletalPose pose;
    pose.clip_asset = std::string(source_clip) + " -> " + std::string(target_clip);
    pose.skeleton_asset = source.skeleton_asset;
    if (!source_sampling.Run() || !target_sampling.Run()) return pose;
    std::vector<ozz::math::SoaTransform> blended(source.local_transforms.size());
    const auto weight = std::clamp(target_weight, 0.0F, 1.0F);
    std::array<ozz::animation::BlendingJob::Layer, 2> layers{{
        {.weight = 1.0F - weight, .transform = ozz::make_span(source.local_transforms)},
        {.weight = weight, .transform = ozz::make_span(target.local_transforms)}}};
    ozz::animation::BlendingJob blending;
    blending.layers = ozz::make_span(layers);
    blending.rest_pose = source.skeleton->joint_rest_poses();
    blending.output = ozz::make_span(blended);
    if (!blending.Run()) return pose;
    std::vector<ozz::math::Float4x4> models(source.model_transforms.size());
    ozz::animation::LocalToModelJob local_to_model;
    local_to_model.skeleton = source.skeleton.get(); local_to_model.input = ozz::make_span(blended);
    local_to_model.output = ozz::make_span(models);
    if (!local_to_model.Run()) return pose;
    pose.skinning_matrices.resize(source.skin_to_runtime.size());
    for (std::size_t skin_joint = 0; skin_joint < source.skin_to_runtime.size(); ++skin_joint) {
        const auto skinning = models[source.skin_to_runtime[skin_joint]] * source.inverse_bind_matrices[skin_joint];
        for (std::size_t column = 0; column < 4U; ++column)
            ozz::math::StorePtrU(skinning.cols[column], pose.skinning_matrices[skin_joint].data() + column * 4U);
    }
    append_joint_debug(pose, *source.skeleton, models);
    pose.valid = !pose.skinning_matrices.empty();
    return pose;
}

AnimationPoseExecutionResult AnimationRuntime::sample_layered_skeletal_pose(
    const AnimationPoseExecutionRequest& request) const {
    AnimationPoseExecutionResult result;
    result.code = "animation.pose.request-invalid";

    const auto fail = [&](std::string code, std::string detail) {
        result.success = false;
        result.code = std::move(code);
        result.detail = std::move(detail);
        result.pose = {};
        return result;
    };
    if (request.base_clip_asset.empty())
        return fail("animation.pose.base-clip-missing", "A base clip asset is required.");
    if (!std::isfinite(request.base_time))
        return fail("animation.pose.time-invalid", "The base clip time must be finite.");

    struct LocalPoseSample final {
        std::string skeleton_asset;
        std::string clip_asset;
        const ozz::animation::Skeleton* skeleton{};
        std::vector<ozz::math::SoaTransform> local;
        std::vector<std::size_t> skin_to_runtime;
        std::vector<ozz::math::Float4x4> inverse_bind_matrices;
    };

    auto sample_local = [&](const std::string_view clip_asset, const float time,
                            LocalPoseSample& output, std::string& error) {
        if (!std::isfinite(time)) {
            error = "Clip time must be finite.";
            return false;
        }
        const auto compiled_found = impl_->compiled_assets.find(std::string(clip_asset));
        ozz::animation::SamplingJob sampling;
        if (compiled_found != impl_->compiled_assets.end()) {
            auto& asset = *compiled_found->second;
            if (!asset.skeleton || !asset.animation || asset.animation->duration() <= 0.0F) {
                error = "Compiled clip has no valid ozz skeleton or animation.";
                return false;
            }
            output.skeleton_asset = asset.skeleton_asset;
            output.clip_asset = asset.clip_asset;
            output.skeleton = asset.skeleton.get();
            output.local.resize(static_cast<std::size_t>(asset.skeleton->num_soa_joints()));
            output.skin_to_runtime = asset.skin_to_runtime;
            output.inverse_bind_matrices = asset.inverse_bind_matrices;
            sampling.animation = asset.animation.get();
            sampling.context = &asset.context;
        } else if (clip_asset == "asset.animation.test-bob" && impl_->skeleton && impl_->animation) {
            output.skeleton_asset = "asset.skeleton.test-two-joint";
            output.clip_asset = std::string(clip_asset);
            output.skeleton = impl_->skeleton.get();
            output.local.resize(static_cast<std::size_t>(impl_->skeleton->num_soa_joints()));
            output.skin_to_runtime.resize(static_cast<std::size_t>(impl_->skeleton->num_joints()));
            for (std::size_t joint = 0; joint < output.skin_to_runtime.size(); ++joint)
                output.skin_to_runtime[joint] = joint;
            sampling.animation = impl_->animation.get();
            sampling.context = &impl_->context;
        } else {
            error = "Clip asset is not registered in the ozz animation runtime.";
            return false;
        }
        const auto duration_seconds = sampling.animation->duration();
        sampling.ratio = std::clamp(time / duration_seconds, 0.0F, 1.0F);
        sampling.output = ozz::make_span(output.local);
        if (!sampling.Run()) {
            error = "ozz SamplingJob rejected the clip, skeleton, or output range.";
            return false;
        }
        return true;
    };

    LocalPoseSample base;
    std::string sample_error;
    if (!sample_local(request.base_clip_asset, request.base_time, base, sample_error))
        return fail("animation.pose.base-clip-unavailable", sample_error);
    if (!base.skeleton || base.skeleton->num_joints() <= 0)
        return fail("animation.pose.skeleton-invalid", "The base clip did not produce a valid skeleton.");
    const auto joint_count = static_cast<std::size_t>(base.skeleton->num_joints());
    if (joint_count > SkeletalPose::maximum_joints)
        return fail("animation.pose.joint-limit", "The layered pose exceeds the 64-joint runtime contract.");

    const auto same_skeleton = [](const LocalPoseSample& left, const LocalPoseSample& right) {
        if (!left.skeleton || !right.skeleton || left.skeleton_asset != right.skeleton_asset ||
            left.skeleton->num_joints() != right.skeleton->num_joints() || left.local.size() != right.local.size())
            return false;
        const auto left_names = left.skeleton->joint_names();
        const auto right_names = right.skeleton->joint_names();
        const auto left_parents = left.skeleton->joint_parents();
        const auto right_parents = right.skeleton->joint_parents();
        for (std::size_t joint = 0; joint < static_cast<std::size_t>(left.skeleton->num_joints()); ++joint)
            if (!left_names[joint] || !right_names[joint] || std::string_view(left_names[joint]) != right_names[joint] ||
                left_parents[joint] != right_parents[joint])
                return false;
        return true;
    };

    const auto blend_source_locals = [&](const LocalPoseSample& primary, const LocalPoseSample& secondary,
                                         const float secondary_weight,
                                         std::vector<ozz::math::SoaTransform>& output, std::string& error) {
        if (!same_skeleton(primary, secondary)) {
            error = "Primary and secondary clips do not share the same skeleton hierarchy.";
            return false;
        }
        if (secondary_weight <= 0.0F) {
            output = primary.local;
            return true;
        }
        if (secondary_weight >= 1.0F) {
            output = secondary.local;
            return true;
        }
        std::array<ozz::animation::BlendingJob::Layer, 2> layers{{
            {.weight = 1.0F - secondary_weight, .transform = ozz::make_span(primary.local)},
            {.weight = secondary_weight, .transform = ozz::make_span(secondary.local)}}};
        output.resize(primary.local.size());
        ozz::animation::BlendingJob blending;
        blending.layers = ozz::make_span(layers);
        blending.rest_pose = primary.skeleton->joint_rest_poses();
        blending.output = ozz::make_span(output);
        if (!blending.Run()) {
            error = "ozz rejected the local-space primary/secondary blend.";
            return false;
        }
        return true;
    };

    if (!request.base_secondary_clip_asset.empty()) {
        if (!std::isfinite(request.base_secondary_time) || !std::isfinite(request.base_secondary_weight) ||
            request.base_secondary_weight < 0.0F || request.base_secondary_weight > 1.0F)
            return fail("animation.pose.base-secondary-invalid", "Base secondary time and weight must be finite; weight must be in [0,1].");
        LocalPoseSample secondary;
        if (!sample_local(request.base_secondary_clip_asset, request.base_secondary_time, secondary, sample_error))
            return fail("animation.pose.base-secondary-unavailable", sample_error);
        std::vector<ozz::math::SoaTransform> blended_base;
        if (!blend_source_locals(base, secondary, request.base_secondary_weight, blended_base, sample_error))
            return fail("animation.pose.base-secondary-invalid", sample_error);
        base.local = std::move(blended_base);
    } else if (!std::isfinite(request.base_secondary_weight) || request.base_secondary_weight != 0.0F) {
        return fail("animation.pose.base-secondary-invalid", "A base secondary weight requires a secondary clip asset.");
    }

    if (base.local.size() != static_cast<std::size_t>(base.skeleton->num_soa_joints()) ||
        base.skin_to_runtime.size() != joint_count ||
        (!base.inverse_bind_matrices.empty() && base.inverse_bind_matrices.size() != joint_count))
        return fail("animation.pose.skeleton-invalid", "The base clip buffers do not match its skeleton.");

    std::unordered_map<std::string, const AnimationPoseMask*> masks;
    for (const auto& mask : request.masks) {
        if (mask.id.empty() || mask.joints.empty() || masks.contains(mask.id))
            return fail("animation.pose.mask-invalid", "Masks require a unique ID and at least one joint.");
        std::unordered_set<std::string> joint_names;
        for (const auto& joint : mask.joints) {
            if (joint.name.empty() || !joint_names.insert(joint.name).second ||
                !std::isfinite(joint.weight) || joint.weight < 0.0F || joint.weight > 1.0F)
                return fail("animation.pose.mask-invalid", "Mask joint names must be unique with weights in [0,1].");
        }
        masks.emplace(mask.id, &mask);
    }

    const auto names = base.skeleton->joint_names();
    const auto parents = base.skeleton->joint_parents();
    std::unordered_map<std::string, std::size_t> joint_indices;
    joint_indices.reserve(joint_count);
    for (std::size_t joint = 0; joint < joint_count; ++joint) {
        if (!names[joint] || !joint_indices.emplace(names[joint], joint).second)
            return fail("animation.pose.skeleton-invalid", "The base skeleton has an empty or duplicate joint name.");
    }

    const auto compatible_skeleton = [&](const LocalPoseSample& sample) {
        if (!same_skeleton(base, sample) || sample.skeleton_asset != base.skeleton_asset ||
            sample.skin_to_runtime.size() != base.skin_to_runtime.size())
            return false;
        const auto sample_names = sample.skeleton->joint_names();
        const auto sample_parents = sample.skeleton->joint_parents();
        for (std::size_t joint = 0; joint < joint_count; ++joint)
            if (!sample_names[joint] || std::string_view(sample_names[joint]) != names[joint] ||
                sample_parents[joint] != parents[joint])
                return false;
        return true;
    };

    const auto sample_composed_layer = [&](const AnimationPoseLayer& layer, LocalPoseSample& output,
                                           std::string& error) {
        if (!sample_local(layer.clip_asset, layer.time, output, error)) return false;
        if (layer.secondary_clip_asset.empty()) {
            if (!std::isfinite(layer.secondary_weight) || layer.secondary_weight != 0.0F) {
                error = "A secondary weight requires a secondary clip asset.";
                return false;
            }
            return true;
        }
        if (!std::isfinite(layer.secondary_time) || !std::isfinite(layer.secondary_weight) ||
            layer.secondary_weight < 0.0F || layer.secondary_weight > 1.0F) {
            error = "Secondary clip time and weight must be finite; weight must be in [0,1].";
            return false;
        }
        LocalPoseSample secondary;
        if (!sample_local(layer.secondary_clip_asset, layer.secondary_time, secondary, error)) return false;
        std::vector<ozz::math::SoaTransform> blended;
        if (!blend_source_locals(output, secondary, layer.secondary_weight, blended, error)) return false;
        output.local = std::move(blended);
        return true;
    };

    auto resolve_joint_weights = [&](const AnimationPoseLayer& layer, std::vector<float>& weights,
                                     bool& has_partial_weights, std::string& error) {
        has_partial_weights = !layer.joint_weights.empty() || !layer.mask_id.empty();
        if (!layer.joint_weights.empty()) {
            if (layer.joint_weights.size() != joint_count) {
                error = "Per-joint weights must contain exactly one value per runtime joint.";
                return false;
            }
            weights = layer.joint_weights;
            for (const auto weight : weights) {
                if (!std::isfinite(weight) || weight < 0.0F || weight > 1.0F) {
                    error = "Per-joint weights must be finite values in [0,1].";
                    return false;
                }
            }
        } else {
            weights.assign(joint_count, 1.0F);
        }
        if (!layer.mask_id.empty()) {
            const auto mask_found = masks.find(layer.mask_id);
            if (mask_found == masks.end()) {
                error = "Layer references an unknown pose mask: " + layer.mask_id;
                return false;
            }
            std::vector<float> mask_weights(joint_count, 0.0F);
            const auto& mask = *mask_found->second;
            for (const auto& mask_joint : mask.joints) {
                const auto source_found = joint_indices.find(mask_joint.name);
                if (source_found == joint_indices.end()) {
                    error = "Pose mask references an unknown skeleton joint: " + mask_joint.name;
                    return false;
                }
                const auto source = source_found->second;
                for (std::size_t target = 0; target < joint_count; ++target) {
                    bool selected = target == source;
                    if (!selected && mask.include_descendants) {
                        auto parent = static_cast<int>(parents[target]);
                        while (parent >= 0) {
                            if (static_cast<std::size_t>(parent) == source) {
                                selected = true;
                                break;
                            }
                            parent = static_cast<int>(parents[static_cast<std::size_t>(parent)]);
                        }
                    }
                    if (selected) mask_weights[target] = std::max(mask_weights[target], mask_joint.weight);
                }
            }
            for (std::size_t joint = 0; joint < joint_count; ++joint)
                weights[joint] *= mask_weights[joint];
        }
        return true;
    };

    const auto pack_weights = [](const std::vector<float>& weights) {
        std::vector<ozz::math::SimdFloat4> packed((weights.size() + 3U) / 4U);
        for (std::size_t soa = 0; soa < packed.size(); ++soa) {
            std::array<float, 4> lanes{};
            for (std::size_t lane = 0; lane < 4U; ++lane) {
                const auto joint = soa * 4U + lane;
                if (joint < weights.size()) lanes[lane] = weights[joint];
            }
            packed[soa] = ozz::math::simd_float4::LoadPtrU(lanes.data());
        }
        return packed;
    };

    const auto fail_layer = [&](const AnimationPoseLayer& layer, std::string code, std::string detail) {
        const auto prefix = layer.id.empty() ? std::string{} : "Layer '" + layer.id + "': ";
        return fail(std::move(code), prefix + detail);
    };

    std::vector<ozz::math::SoaTransform> working = base.local;
    for (const auto& layer : request.layers) {
        if (layer.clip_asset.empty())
            return fail_layer(layer, "animation.pose.layer-clip-missing", "A layer clip asset is required.");
        if (!std::isfinite(layer.time) || !std::isfinite(layer.weight) || layer.weight < 0.0F || layer.weight > 1.0F)
            return fail_layer(layer, "animation.pose.layer-weight-invalid", "Layer time and weight must be finite; weight must be in [0,1].");
        if (layer.mode == AnimationPoseLayerMode::additive) continue;
        if (layer.mode != AnimationPoseLayerMode::override_layer)
            return fail_layer(layer, "animation.pose.layer-mode-invalid", "Layer mode is not a supported override or additive mode.");

        LocalPoseSample target;
        if (!sample_composed_layer(layer, target, sample_error))
            return fail_layer(layer, "animation.pose.layer-clip-unavailable", sample_error);
        if (!compatible_skeleton(target))
            return fail_layer(layer, "animation.pose.skeleton-mismatch", "Layer skeleton does not match the base skeleton.");

        std::vector<float> target_weights;
        bool has_partial_weights{};
        if (!resolve_joint_weights(layer, target_weights, has_partial_weights, sample_error))
            return fail_layer(layer, sample_error.find("unknown pose mask") != std::string::npos
                                   ? "animation.pose.mask-not-found"
                                   : sample_error.find("unknown skeleton joint") != std::string::npos
                                         ? "animation.pose.mask-joint-not-found"
                                         : sample_error.find("exactly one value") != std::string::npos
                                               ? "animation.pose.joint-weight-count"
                                         : "animation.pose.joint-weight-invalid",
                sample_error);

        std::vector<float> current_weights(joint_count);
        for (std::size_t joint = 0; joint < joint_count; ++joint)
            target_weights[joint] *= layer.weight, current_weights[joint] = 1.0F - target_weights[joint];
        const auto packed_current = pack_weights(current_weights);
        const auto packed_target = pack_weights(target_weights);
        std::array<ozz::animation::BlendingJob::Layer, 2> layers{{
            {.weight = 1.0F, .transform = ozz::make_span(working), .joint_weights = ozz::make_span(packed_current)},
            {.weight = 1.0F, .transform = ozz::make_span(target.local), .joint_weights = ozz::make_span(packed_target)}}};
        std::vector<ozz::math::SoaTransform> next(working.size());
        ozz::animation::BlendingJob blending;
        blending.layers = ozz::make_span(layers);
        blending.rest_pose = base.skeleton->joint_rest_poses();
        blending.output = ozz::make_span(next);
        if (!blending.Run())
            return fail_layer(layer, "animation.pose.blend-failed", "ozz rejected the local-space override blend.");
        working = std::move(next);
        static_cast<void>(has_partial_weights);
    }

    std::vector<std::vector<ozz::math::SoaTransform>> additive_transforms;
    std::vector<std::vector<ozz::math::SimdFloat4>> additive_weights;
    std::vector<bool> additive_has_partial;
    additive_transforms.reserve(request.layers.size());
    additive_weights.reserve(request.layers.size());
    additive_has_partial.reserve(request.layers.size());
    std::vector<const AnimationPoseLayer*> additive_definitions;
    additive_definitions.reserve(request.layers.size());
    for (const auto& layer : request.layers) {
        if (layer.mode != AnimationPoseLayerMode::additive) continue;
        if (layer.clip_asset.empty())
            return fail_layer(layer, "animation.pose.layer-clip-missing", "An additive layer clip asset is required.");
        if (!std::isfinite(layer.time) || !std::isfinite(layer.weight) || layer.weight < 0.0F || layer.weight > 1.0F)
            return fail_layer(layer, "animation.pose.layer-weight-invalid", "Layer time and weight must be finite; weight must be in [0,1].");
        LocalPoseSample source;
        if (!sample_composed_layer(layer, source, sample_error))
            return fail_layer(layer, "animation.pose.layer-clip-unavailable", sample_error);
        if (!compatible_skeleton(source))
            return fail_layer(layer, "animation.pose.skeleton-mismatch", "Layer skeleton does not match the base skeleton.");
        std::vector<float> weights;
        bool has_partial_weights{};
        if (!resolve_joint_weights(layer, weights, has_partial_weights, sample_error))
            return fail_layer(layer, sample_error.find("unknown pose mask") != std::string::npos
                                   ? "animation.pose.mask-not-found"
                                   : sample_error.find("unknown skeleton joint") != std::string::npos
                                         ? "animation.pose.mask-joint-not-found"
                                         : sample_error.find("exactly one value") != std::string::npos
                                               ? "animation.pose.joint-weight-count"
                                         : "animation.pose.joint-weight-invalid",
                sample_error);

        additive_transforms.emplace_back(source.local.size());
        auto& delta = additive_transforms.back();
        const auto rest_pose = base.skeleton->joint_rest_poses();
        for (std::size_t soa = 0; soa < delta.size(); ++soa) {
            delta[soa].translation = source.local[soa].translation - rest_pose[soa].translation;
            delta[soa].rotation = ozz::math::Conjugate(rest_pose[soa].rotation) * source.local[soa].rotation;
            delta[soa].scale = source.local[soa].scale / rest_pose[soa].scale;
        }
        if (has_partial_weights) additive_weights.push_back(pack_weights(weights));
        else additive_weights.emplace_back();
        additive_has_partial.push_back(has_partial_weights);
        additive_definitions.push_back(&layer);
    }

    if (!additive_transforms.empty()) {
        std::array<ozz::animation::BlendingJob::Layer, 1> base_layers{{
            {.weight = 1.0F, .transform = ozz::make_span(working)}}};
        std::vector<ozz::animation::BlendingJob::Layer> additive_layers;
        additive_layers.reserve(additive_transforms.size());
        for (std::size_t index = 0; index < additive_transforms.size(); ++index) {
            ozz::animation::BlendingJob::Layer descriptor;
            descriptor.weight = additive_definitions[index]->weight;
            descriptor.transform = ozz::make_span(additive_transforms[index]);
            if (additive_has_partial[index]) descriptor.joint_weights = ozz::make_span(additive_weights[index]);
            additive_layers.push_back(descriptor);
        }
        std::vector<ozz::math::SoaTransform> next(working.size());
        ozz::animation::BlendingJob blending;
        blending.layers = ozz::make_span(base_layers);
        blending.additive_layers = ozz::make_span(additive_layers);
        blending.rest_pose = base.skeleton->joint_rest_poses();
        blending.output = ozz::make_span(next);
        if (!blending.Run())
            return fail("animation.pose.additive-blend-failed", "ozz rejected the local-space additive blend.");
        working = std::move(next);
    }

    std::vector<ozz::math::Float4x4> models(joint_count);
    ozz::animation::LocalToModelJob local_to_model;
    local_to_model.skeleton = base.skeleton;
    local_to_model.input = ozz::make_span(working);
    local_to_model.output = ozz::make_span(models);
    if (!local_to_model.Run())
        return fail("animation.pose.local-to-model-failed", "ozz rejected the final local-to-model conversion.");

    result.pose.skeleton_asset = base.skeleton_asset;
    result.pose.clip_asset = base.clip_asset;
    result.pose.skinning_matrices.resize(base.skin_to_runtime.size());
    for (std::size_t skin_joint = 0; skin_joint < base.skin_to_runtime.size(); ++skin_joint) {
        const auto runtime_joint = base.skin_to_runtime[skin_joint];
        if (runtime_joint >= models.size())
            return fail("animation.pose.skeleton-invalid", "The base skin-to-runtime joint mapping is invalid.");
        const auto skinning = base.inverse_bind_matrices.empty()
                                  ? models[runtime_joint]
                                  : models[runtime_joint] * base.inverse_bind_matrices[skin_joint];
        for (std::size_t column = 0; column < 4U; ++column)
            ozz::math::StorePtrU(skinning.cols[column], result.pose.skinning_matrices[skin_joint].data() + column * 4U);
    }
    append_joint_debug(result.pose, *base.skeleton, models);
    result.pose.valid = !result.pose.skinning_matrices.empty();
    result.success = result.pose.valid;
    result.code = result.success ? "ok" : "animation.pose.empty-result";
    result.detail = result.success
                        ? "Local-space override/additive layers executed with one final LocalToModelJob."
                        : "The layered pose produced no skinning matrices.";
    return result;
}

RootMotionDelta AnimationRuntime::root_motion_delta(const std::string_view clip_asset, const float previous_time,
                                                    const float current_time, const bool looping,
                                                    const float playback_speed) const {
    const auto root_at = [&](const float time) {
        const auto pose = sample_skeletal_pose(clip_asset, time);
        return pose.valid && !pose.joints.empty() ? std::optional<SkeletalPose::JointDebug>{pose.joints.front()} : std::nullopt;
    };
    const auto previous = root_at(previous_time);
    const auto current = root_at(current_time);
    if (!previous || !current) return {};
    const auto subtract = [](const SkeletalPose::JointDebug& from, const SkeletalPose::JointDebug& to) {
        return RootMotionDelta{true, to.model_x - from.model_x, to.model_y - from.model_y, to.model_z - from.model_z};
    };
    if (!looping || playback_speed >= 0.0F && current_time >= previous_time || playback_speed < 0.0F && current_time <= previous_time)
        return subtract(*previous, *current);
    const auto clip_duration = duration(clip_asset);
    const auto start = root_at(0.0F);
    const auto end = root_at(clip_duration);
    if (!start || !end) return {};
    const auto first = playback_speed >= 0.0F ? subtract(*previous, *end) : subtract(*previous, *start);
    const auto second = playback_speed >= 0.0F ? subtract(*start, *current) : subtract(*end, *current);
    return {true, first.x + second.x, first.y + second.y, first.z + second.z};
}

} // namespace noemancer
