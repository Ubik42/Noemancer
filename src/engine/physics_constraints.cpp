#include "engine/physics_constraints.hpp"

#include <Jolt/Jolt.h>
#include <Jolt/Core/Factory.h>
#include <Jolt/Core/JobSystemThreadPool.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyLockInterface.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/ConvexHullShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Constraints/DistanceConstraint.h>
#include <Jolt/Physics/Constraints/FixedConstraint.h>
#include <Jolt/Physics/Constraints/HingeConstraint.h>
#include <Jolt/Physics/Constraints/SliderConstraint.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/RegisterTypes.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>

namespace noemancer {

namespace {

constexpr float kPi = 3.14159265358979323846F;
constexpr float kMaximumAnchor = 1.0e6F;
constexpr float kMaximumAxis = 1.0e4F;
constexpr float kMaximumLimit = 1.0e6F;
constexpr float kMaximumSpringFrequency = 120.0F;
constexpr float kMaximumDimension = 1.0e4F;
constexpr float kMinimumDimension = 1.0e-4F;
constexpr std::size_t kMaximumIdentifierBytes = 96U;

bool valid_identifier(const std::string_view value) {
    if (value.empty() || value.size() > kMaximumIdentifierBytes) return false;
    for (const auto byte : value) {
        const auto character = static_cast<unsigned char>(byte);
        if (character < 0x20U || character == 0x7fU || character == '/' || character == '\\') return false;
    }
    return true;
}

bool finite_bounded(const float value, const float maximum) {
    return std::isfinite(value) && std::abs(value) <= maximum;
}

bool finite_vec(const PhysicsConstraintVec3& value, const float maximum) {
    return finite_bounded(value.x, maximum) && finite_bounded(value.y, maximum) &&
           finite_bounded(value.z, maximum);
}

float length_squared(const PhysicsConstraintVec3& value) {
    return value.x * value.x + value.y * value.y + value.z * value.z;
}

float dot(const PhysicsConstraintVec3& left, const PhysicsConstraintVec3& right) {
    return left.x * right.x + left.y * right.y + left.z * right.z;
}

bool valid_axis_pair(const PhysicsConstraintVec3& primary, const PhysicsConstraintVec3& secondary) {
    const auto primary_length_squared = length_squared(primary);
    const auto secondary_length_squared = length_squared(secondary);
    if (primary_length_squared < 1.0e-8F || secondary_length_squared < 1.0e-8F) return false;
    const auto denominator = std::sqrt(primary_length_squared * secondary_length_squared);
    return denominator > 0.0F && std::abs(dot(primary, secondary)) / denominator <= 1.0e-3F;
}

PhysicsConstraintResult invalid_spec(std::string detail) {
    return PhysicsConstraintResult::failed(PhysicsConstraintErrorCode::invalid_argument, std::move(detail));
}

JPH::Vec3 jolt_vec(const PhysicsConstraintVec3& value) {
    return JPH::Vec3(value.x, value.y, value.z);
}

JPH::RVec3 jolt_rvec(const PhysicsConstraintVec3& value) {
    return JPH::RVec3(value.x, value.y, value.z);
}

PhysicsConstraintVec3 normalized(const PhysicsConstraintVec3& value) {
    const auto length = std::sqrt(length_squared(value));
    if (length <= 0.0F) return {};
    return {value.x / length, value.y / length, value.z / length};
}

JPH::EMotionType jolt_motion_type(const PhysicsMotionType type) {
    if (type == PhysicsMotionType::static_body) return JPH::EMotionType::Static;
    if (type == PhysicsMotionType::kinematic_body) return JPH::EMotionType::Kinematic;
    return JPH::EMotionType::Dynamic;
}

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
    [[nodiscard]] bool ShouldCollide(const JPH::ObjectLayer layer,
                                     const JPH::BroadPhaseLayer broad_phase) const override {
        return layer == Layers::moving || broad_phase == BroadPhaseLayers::moving;
    }
};

class ObjectLayerPairFilter final : public JPH::ObjectLayerPairFilter {
public:
    [[nodiscard]] bool ShouldCollide(const JPH::ObjectLayer first,
                                     const JPH::ObjectLayer second) const override {
        return first == Layers::moving || second == Layers::moving;
    }
};

// PhysicsRuntime historically owns the process-wide Jolt bootstrap.  This
// adapter also works as a standalone runtime for focused tests and tools.  If
// another Jolt owner is already active, it borrows that factory and leaves its
// lifetime untouched.
struct JoltLifetime final {
    bool owns_factory{};

    JoltLifetime() {
        JPH::RegisterDefaultAllocator();
        if (JPH::Factory::sInstance == nullptr) {
            JPH::Factory::sInstance = new JPH::Factory();
            JPH::RegisterTypes();
            owns_factory = true;
        }
    }

    ~JoltLifetime() {
        if (!owns_factory) return;
        JPH::UnregisterTypes();
        delete JPH::Factory::sInstance;
        JPH::Factory::sInstance = nullptr;
    }
};

bool valid_body_state(const PhysicsBodyState& state) {
    if (!valid_identifier(state.entity_id)) return false;
    if (!finite_bounded(state.position_x, kMaximumAnchor) ||
        !finite_bounded(state.position_y, kMaximumAnchor) ||
        !finite_bounded(state.position_z, kMaximumAnchor)) return false;
    if (!finite_bounded(state.rotation_x, 1.0F) || !finite_bounded(state.rotation_y, 1.0F) ||
        !finite_bounded(state.rotation_z, 1.0F) || !finite_bounded(state.rotation_w, 1.0F)) return false;
    const auto quaternion_length_squared = state.rotation_x * state.rotation_x + state.rotation_y * state.rotation_y +
                                           state.rotation_z * state.rotation_z + state.rotation_w * state.rotation_w;
    if (quaternion_length_squared < 1.0e-8F) return false;
    if (!finite_bounded(state.velocity_x, kMaximumLimit) || !finite_bounded(state.velocity_y, kMaximumLimit) ||
        !finite_bounded(state.velocity_z, kMaximumLimit) || !finite_bounded(state.angular_velocity_x, kMaximumLimit) ||
        !finite_bounded(state.angular_velocity_y, kMaximumLimit) ||
        !finite_bounded(state.angular_velocity_z, kMaximumLimit)) return false;
    if (!finite_bounded(state.mass, kMaximumLimit) || state.mass <= 0.0F) return false;
    if (!finite_bounded(state.gravity_factor, kMaximumLimit) || !finite_bounded(state.linear_damping, kMaximumLimit) ||
        !finite_bounded(state.angular_damping, kMaximumLimit) || !finite_bounded(state.friction, kMaximumLimit) ||
        !finite_bounded(state.restitution, kMaximumLimit)) return false;

    switch (state.shape_type) {
    case PhysicsShapeType::box:
        return finite_bounded(state.half_x, kMaximumDimension) && state.half_x >= kMinimumDimension &&
               finite_bounded(state.half_y, kMaximumDimension) && state.half_y >= kMinimumDimension &&
               finite_bounded(state.half_z, kMaximumDimension) && state.half_z >= kMinimumDimension;
    case PhysicsShapeType::sphere:
        return finite_bounded(state.radius, kMaximumDimension) && state.radius >= kMinimumDimension;
    case PhysicsShapeType::capsule:
        return finite_bounded(state.radius, kMaximumDimension) && state.radius >= kMinimumDimension &&
               finite_bounded(state.half_height, kMaximumDimension) && state.half_height >= kMinimumDimension;
    case PhysicsShapeType::convex_hull:
        if (state.convex_points.size() < 4U || state.convex_points.size() > 256U) return false;
        for (const auto& point : state.convex_points) {
            for (const auto coordinate : point) {
                if (!finite_bounded(coordinate, kMaximumDimension)) return false;
            }
        }
        return true;
    }
    return false;
}

bool same_body_definition(const PhysicsBodyState& left, const PhysicsBodyState& right) {
    return left.motion_type == right.motion_type && left.shape_type == right.shape_type &&
           left.half_x == right.half_x && left.half_y == right.half_y && left.half_z == right.half_z &&
           left.radius == right.radius && left.half_height == right.half_height &&
           left.convex_points == right.convex_points;
}

JPH::RefConst<JPH::Shape> make_shape(const PhysicsBodyState& state) {
    switch (state.shape_type) {
    case PhysicsShapeType::box:
        return new JPH::BoxShape(JPH::Vec3(state.half_x, state.half_y, state.half_z));
    case PhysicsShapeType::sphere:
        return new JPH::SphereShape(state.radius);
    case PhysicsShapeType::capsule:
        return new JPH::CapsuleShape(state.half_height, state.radius);
    case PhysicsShapeType::convex_hull: {
        JPH::Array<JPH::Vec3> points;
        points.reserve(state.convex_points.size());
        for (const auto& point : state.convex_points) {
            points.push_back(JPH::Vec3(point[0], point[1], point[2]));
        }
        const auto result = JPH::ConvexHullShapeSettings(points).Create();
        if (result.HasError()) return {};
        return result.Get();
    }
    }
    return {};
}

PhysicsConstraintVec3 body_position(const JPH::BodyInterface& body_interface, const JPH::BodyID id) {
    const auto position = body_interface.GetPosition(id);
    return {static_cast<float>(position.GetX()), static_cast<float>(position.GetY()),
            static_cast<float>(position.GetZ())};
}

JPH::Quat body_rotation(const JPH::BodyInterface& body_interface, const JPH::BodyID id) {
    return body_interface.GetRotation(id);
}

PhysicsConstraintVec3 world_anchor(const JPH::BodyInterface& body_interface, const JPH::BodyID id,
                                   const PhysicsConstraintVec3& local_anchor) {
    const auto position = body_position(body_interface, id);
    const auto rotated = body_rotation(body_interface, id) * jolt_vec(local_anchor);
    return {position.x + rotated.GetX(), position.y + rotated.GetY(), position.z + rotated.GetZ()};
}

float distance_between(const PhysicsConstraintVec3& first, const PhysicsConstraintVec3& second) {
    const auto x = first.x - second.x;
    const auto y = first.y - second.y;
    const auto z = first.z - second.z;
    return std::sqrt(x * x + y * y + z * z);
}

float range_error(const float value, const float lower, const float upper) {
    if (value < lower) return value - lower;
    if (value > upper) return value - upper;
    return 0.0F;
}

} // namespace

std::string_view physics_constraint_type_name(const PhysicsConstraintType type) noexcept {
    switch (type) {
    case PhysicsConstraintType::fixed:
        return "fixed";
    case PhysicsConstraintType::distance:
        return "distance";
    case PhysicsConstraintType::hinge:
        return "hinge";
    case PhysicsConstraintType::slider:
        return "slider";
    case PhysicsConstraintType::spring:
        return "spring";
    }
    return "unknown";
}

std::string_view physics_constraint_error_code_name(const PhysicsConstraintErrorCode code) noexcept {
    switch (code) {
    case PhysicsConstraintErrorCode::ok:
        return "ok";
    case PhysicsConstraintErrorCode::invalid_argument:
        return "invalid_argument";
    case PhysicsConstraintErrorCode::invalid_id:
        return "invalid_id";
    case PhysicsConstraintErrorCode::invalid_body:
        return "invalid_body";
    case PhysicsConstraintErrorCode::body_exists:
        return "body_exists";
    case PhysicsConstraintErrorCode::body_not_found:
        return "body_not_found";
    case PhysicsConstraintErrorCode::body_in_use:
        return "body_in_use";
    case PhysicsConstraintErrorCode::constraint_exists:
        return "constraint_exists";
    case PhysicsConstraintErrorCode::constraint_not_found:
        return "constraint_not_found";
    case PhysicsConstraintErrorCode::constraint_limit_reached:
        return "constraint_limit_reached";
    case PhysicsConstraintErrorCode::body_limit_reached:
        return "body_limit_reached";
    case PhysicsConstraintErrorCode::unsupported_configuration:
        return "unsupported_configuration";
    case PhysicsConstraintErrorCode::backend_failure:
        return "backend_failure";
    }
    return "unknown";
}

PhysicsConstraintResult PhysicsConstraintResult::succeeded(std::string detail) {
    return {true, PhysicsConstraintErrorCode::ok, std::move(detail)};
}

PhysicsConstraintResult PhysicsConstraintResult::failed(const PhysicsConstraintErrorCode code,
                                                        std::string detail) {
    return {false, code, std::move(detail)};
}

PhysicsConstraintResult validate_physics_constraint_spec(const PhysicsConstraintSpec& spec) {
    if (!valid_identifier(spec.id)) return invalid_spec("constraint id must be non-empty and path-safe");
    if (!valid_identifier(spec.body_a) || !valid_identifier(spec.body_b)) {
        return invalid_spec("constraint body ids must be non-empty and path-safe");
    }
    if (spec.body_a == spec.body_b) return invalid_spec("constraint cannot connect a body to itself");

    const auto& frame = spec.frame;
    if (!finite_vec(frame.anchor_a, kMaximumAnchor) || !finite_vec(frame.anchor_b, kMaximumAnchor)) {
        return invalid_spec("constraint anchors must be finite and bounded");
    }
    if (!finite_vec(frame.primary_axis_a, kMaximumAxis) || !finite_vec(frame.secondary_axis_a, kMaximumAxis) ||
        !finite_vec(frame.primary_axis_b, kMaximumAxis) || !finite_vec(frame.secondary_axis_b, kMaximumAxis)) {
        return invalid_spec("constraint axes must be finite and bounded");
    }
    if (!finite_bounded(spec.lower_limit, kMaximumLimit) || !finite_bounded(spec.upper_limit, kMaximumLimit) ||
        !finite_bounded(spec.rest_length, kMaximumLimit) ||
        !finite_bounded(spec.spring_frequency_hz, kMaximumSpringFrequency) ||
        !finite_bounded(spec.spring_damping_ratio, 2.0F)) {
        return invalid_spec("constraint limits and spring values must be finite and bounded");
    }
    if (spec.spring_frequency_hz < 0.0F || spec.spring_damping_ratio < 0.0F || spec.spring_damping_ratio > 2.0F) {
        return invalid_spec("spring frequency and damping ratio are outside the supported range");
    }

    switch (spec.type) {
    case PhysicsConstraintType::fixed:
        if (spec.lower_limit != 0.0F || spec.upper_limit != 0.0F || spec.spring_frequency_hz != 0.0F) {
            return invalid_spec("fixed constraint does not accept limits or spring settings");
        }
        if (!valid_axis_pair(frame.primary_axis_a, frame.secondary_axis_a) ||
            !valid_axis_pair(frame.primary_axis_b, frame.secondary_axis_b)) {
            return invalid_spec("fixed constraint axes must be non-zero and perpendicular");
        }
        break;
    case PhysicsConstraintType::distance:
        if (spec.lower_limit < 0.0F || spec.upper_limit < spec.lower_limit) {
            return invalid_spec("distance limits must satisfy 0 <= lower <= upper");
        }
        break;
    case PhysicsConstraintType::hinge:
        if (spec.lower_limit < -kPi || spec.upper_limit > kPi || spec.lower_limit > spec.upper_limit ||
            spec.lower_limit > 0.0F || spec.upper_limit < 0.0F) {
            return invalid_spec("hinge limits must contain zero and stay within [-pi, pi]");
        }
        if (!valid_axis_pair(frame.primary_axis_a, frame.secondary_axis_a) ||
            !valid_axis_pair(frame.primary_axis_b, frame.secondary_axis_b)) {
            return invalid_spec("hinge axes must be non-zero and perpendicular");
        }
        break;
    case PhysicsConstraintType::slider:
        if (spec.lower_limit < -kMaximumLimit || spec.upper_limit > kMaximumLimit ||
            spec.lower_limit > spec.upper_limit || spec.lower_limit > 0.0F || spec.upper_limit < 0.0F) {
            return invalid_spec("slider limits must contain zero and stay within the bounded range");
        }
        if (!valid_axis_pair(frame.primary_axis_a, frame.secondary_axis_a) ||
            !valid_axis_pair(frame.primary_axis_b, frame.secondary_axis_b)) {
            return invalid_spec("slider axes must be non-zero and perpendicular");
        }
        break;
    case PhysicsConstraintType::spring:
        if (spec.rest_length < kMinimumDimension || spec.rest_length > kMaximumLimit ||
            spec.spring_frequency_hz < 0.01F || spec.spring_frequency_hz > kMaximumSpringFrequency) {
            return invalid_spec("spring rest length or frequency is outside the supported range");
        }
        break;
    default:
        return invalid_spec("unknown constraint type");
    }

    if (spec.spring_frequency_hz > 0.0F && spec.spring_frequency_hz < 0.01F) {
        return invalid_spec("spring frequency must be zero or at least 0.01 Hz");
    }
    return PhysicsConstraintResult::succeeded();
}

struct PhysicsConstraintRegistry::Impl final {
    std::unordered_map<std::string, PhysicsConstraintSpec> specs;
    std::size_t max_constraints;
    std::uint64_t revision{1U};

    explicit Impl(const std::size_t max_constraint_count)
        : max_constraints(std::clamp(max_constraint_count, std::size_t{1U}, std::size_t{65536U})) {}
};

PhysicsConstraintRegistry::PhysicsConstraintRegistry(const std::size_t max_constraints)
    : impl_(std::make_unique<Impl>(max_constraints)) {}

PhysicsConstraintRegistry::~PhysicsConstraintRegistry() = default;

PhysicsConstraintResult PhysicsConstraintRegistry::add(const PhysicsConstraintSpec& spec) {
    const auto validation = validate_physics_constraint_spec(spec);
    if (!validation.success) return validation;
    if (impl_->specs.contains(spec.id)) {
        return PhysicsConstraintResult::failed(PhysicsConstraintErrorCode::constraint_exists,
                                               "constraint id already exists");
    }
    if (impl_->specs.size() >= impl_->max_constraints) {
        return PhysicsConstraintResult::failed(PhysicsConstraintErrorCode::constraint_limit_reached,
                                               "constraint capacity reached");
    }
    impl_->specs.emplace(spec.id, spec);
    ++impl_->revision;
    return PhysicsConstraintResult::succeeded("constraint record added");
}

PhysicsConstraintResult PhysicsConstraintRegistry::update(const PhysicsConstraintSpec& spec) {
    const auto validation = validate_physics_constraint_spec(spec);
    if (!validation.success) return validation;
    const auto found = impl_->specs.find(spec.id);
    if (found == impl_->specs.end()) {
        return PhysicsConstraintResult::failed(PhysicsConstraintErrorCode::constraint_not_found,
                                               "constraint id was not found");
    }
    found->second = spec;
    ++impl_->revision;
    return PhysicsConstraintResult::succeeded("constraint record updated");
}

PhysicsConstraintResult PhysicsConstraintRegistry::remove(const std::string_view constraint_id) {
    const auto found = impl_->specs.find(std::string(constraint_id));
    if (found == impl_->specs.end()) {
        return PhysicsConstraintResult::failed(PhysicsConstraintErrorCode::constraint_not_found,
                                               "constraint id was not found");
    }
    impl_->specs.erase(found);
    ++impl_->revision;
    return PhysicsConstraintResult::succeeded("constraint record removed");
}

std::optional<PhysicsConstraintSpec> PhysicsConstraintRegistry::find(const std::string_view constraint_id) const {
    const auto found = impl_->specs.find(std::string(constraint_id));
    if (found == impl_->specs.end()) return std::nullopt;
    return found->second;
}

std::vector<PhysicsConstraintSpec> PhysicsConstraintRegistry::all() const {
    std::vector<std::string> ids;
    ids.reserve(impl_->specs.size());
    for (const auto& [id, unused] : impl_->specs) {
        static_cast<void>(unused);
        ids.push_back(id);
    }
    std::sort(ids.begin(), ids.end());
    std::vector<PhysicsConstraintSpec> result;
    result.reserve(ids.size());
    for (const auto& id : ids) {
        const auto found = impl_->specs.find(id);
        if (found != impl_->specs.end()) result.push_back(found->second);
    }
    return result;
}

std::uint64_t PhysicsConstraintRegistry::revision() const noexcept {
    return impl_->revision;
}

struct PhysicsConstraintRuntime::Impl final {
    struct BodyRecord final {
        PhysicsBodyState state;
        JPH::BodyID native_id;
    };

    struct ConstraintRecord final {
        PhysicsConstraintSpec spec;
        JPH::Ref<JPH::Constraint> native;
    };

    JoltLifetime jolt_lifetime;
    BroadPhaseLayerInterface broad_phase_layers;
    ObjectVsBroadPhaseFilter object_vs_broad_phase;
    ObjectLayerPairFilter object_pairs;
    std::unique_ptr<JPH::TempAllocatorImpl> temp_allocator;
    std::unique_ptr<JPH::JobSystemThreadPool> jobs;
    std::unique_ptr<JPH::PhysicsSystem> system;
    std::unordered_map<std::string, BodyRecord> bodies;
    std::unordered_map<std::string, ConstraintRecord> constraints;
    std::size_t max_bodies;
    std::size_t max_constraints;
    std::uint64_t revision{1U};

    Impl(const std::size_t max_body_count, const std::size_t max_constraint_count)
        : max_bodies(std::clamp(max_body_count, std::size_t{1U}, std::size_t{65536U})),
          max_constraints(std::clamp(max_constraint_count, std::size_t{1U}, std::size_t{65536U})) {
        temp_allocator = std::make_unique<JPH::TempAllocatorImpl>(8U * 1024U * 1024U);
        const auto available_workers = std::max(1U, std::thread::hardware_concurrency()) - 1U;
        jobs = std::make_unique<JPH::JobSystemThreadPool>(JPH::cMaxPhysicsJobs, JPH::cMaxPhysicsBarriers,
                                                          static_cast<int>(std::min(available_workers, 4U)));
        system = std::make_unique<JPH::PhysicsSystem>();
        const auto body_capacity = static_cast<JPH::uint>(std::min(max_bodies, std::size_t{JPH::PhysicsSystem::cMaxBodiesLimit}));
        const auto pair_capacity = static_cast<JPH::uint>(std::min(max_bodies * 4U, std::size_t{JPH::PhysicsSystem::cMaxBodyPairsLimit}));
        const auto contact_capacity = static_cast<JPH::uint>(std::min(max_bodies * 2U, std::size_t{JPH::PhysicsSystem::cMaxContactConstraintsLimit}));
        system->Init(std::max<JPH::uint>(body_capacity, 1U), 0U, std::max<JPH::uint>(pair_capacity, 1U),
                     std::max<JPH::uint>(contact_capacity, 1U), broad_phase_layers, object_vs_broad_phase, object_pairs);
    }

    ~Impl() {
        for (auto& [unused, record] : constraints) {
            static_cast<void>(unused);
            if (record.native != nullptr) system->RemoveConstraint(record.native);
        }
        constraints.clear();
        auto& body_interface = system->GetBodyInterface();
        for (const auto& [unused, record] : bodies) {
            static_cast<void>(unused);
            if (body_interface.IsAdded(record.native_id)) body_interface.RemoveBody(record.native_id);
            body_interface.DestroyBody(record.native_id);
        }
    }

    JPH::Ref<JPH::Constraint> make_native_constraint(const PhysicsConstraintSpec& spec,
                                                     const JPH::BodyID body_a_id,
                                                     const JPH::BodyID body_b_id) {
        const auto& interface = system->GetBodyLockInterfaceNoLock();
        auto* body_a = interface.TryGetBody(body_a_id);
        auto* body_b = interface.TryGetBody(body_b_id);
        if (body_a == nullptr || body_b == nullptr) return {};

        JPH::Ref<JPH::Constraint> native;
        const auto primary_a = normalized(spec.frame.primary_axis_a);
        const auto secondary_a = normalized(spec.frame.secondary_axis_a);
        const auto primary_b = normalized(spec.frame.primary_axis_b);
        const auto secondary_b = normalized(spec.frame.secondary_axis_b);
        switch (spec.type) {
        case PhysicsConstraintType::fixed: {
            JPH::FixedConstraintSettings settings;
            settings.mSpace = JPH::EConstraintSpace::WorldSpace;
            settings.mPoint1 = jolt_rvec(spec.frame.anchor_a);
            settings.mPoint2 = jolt_rvec(spec.frame.anchor_b);
            settings.mAxisX1 = jolt_vec(primary_a);
            settings.mAxisY1 = jolt_vec(secondary_a);
            settings.mAxisX2 = jolt_vec(primary_b);
            settings.mAxisY2 = jolt_vec(secondary_b);
            settings.mEnabled = spec.enabled;
            native = settings.Create(*body_a, *body_b);
            break;
        }
        case PhysicsConstraintType::distance: {
            JPH::DistanceConstraintSettings settings;
            settings.mSpace = JPH::EConstraintSpace::WorldSpace;
            settings.mPoint1 = jolt_rvec(spec.frame.anchor_a);
            settings.mPoint2 = jolt_rvec(spec.frame.anchor_b);
            settings.mMinDistance = spec.lower_limit;
            settings.mMaxDistance = spec.upper_limit;
            settings.mEnabled = spec.enabled;
            settings.mLimitsSpringSettings.mFrequency = spec.spring_frequency_hz;
            settings.mLimitsSpringSettings.mDamping = spec.spring_damping_ratio;
            native = settings.Create(*body_a, *body_b);
            break;
        }
        case PhysicsConstraintType::hinge: {
            JPH::HingeConstraintSettings settings;
            settings.mSpace = JPH::EConstraintSpace::WorldSpace;
            settings.mPoint1 = jolt_rvec(spec.frame.anchor_a);
            settings.mPoint2 = jolt_rvec(spec.frame.anchor_b);
            settings.mHingeAxis1 = jolt_vec(primary_a);
            settings.mNormalAxis1 = jolt_vec(secondary_a);
            settings.mHingeAxis2 = jolt_vec(primary_b);
            settings.mNormalAxis2 = jolt_vec(secondary_b);
            settings.mLimitsMin = spec.lower_limit;
            settings.mLimitsMax = spec.upper_limit;
            settings.mLimitsSpringSettings.mFrequency = spec.spring_frequency_hz;
            settings.mLimitsSpringSettings.mDamping = spec.spring_damping_ratio;
            settings.mEnabled = spec.enabled;
            native = settings.Create(*body_a, *body_b);
            break;
        }
        case PhysicsConstraintType::slider: {
            JPH::SliderConstraintSettings settings;
            settings.mSpace = JPH::EConstraintSpace::WorldSpace;
            settings.mPoint1 = jolt_rvec(spec.frame.anchor_a);
            settings.mPoint2 = jolt_rvec(spec.frame.anchor_b);
            settings.mSliderAxis1 = jolt_vec(primary_a);
            settings.mNormalAxis1 = jolt_vec(secondary_a);
            settings.mSliderAxis2 = jolt_vec(primary_b);
            settings.mNormalAxis2 = jolt_vec(secondary_b);
            settings.mLimitsMin = spec.lower_limit;
            settings.mLimitsMax = spec.upper_limit;
            settings.mLimitsSpringSettings.mFrequency = spec.spring_frequency_hz;
            settings.mLimitsSpringSettings.mDamping = spec.spring_damping_ratio;
            settings.mEnabled = spec.enabled;
            native = settings.Create(*body_a, *body_b);
            break;
        }
        case PhysicsConstraintType::spring: {
            JPH::DistanceConstraintSettings settings;
            settings.mSpace = JPH::EConstraintSpace::WorldSpace;
            settings.mPoint1 = jolt_rvec(spec.frame.anchor_a);
            settings.mPoint2 = jolt_rvec(spec.frame.anchor_b);
            settings.mMinDistance = spec.rest_length;
            settings.mMaxDistance = spec.rest_length;
            settings.mLimitsSpringSettings.mFrequency = spec.spring_frequency_hz;
            settings.mLimitsSpringSettings.mDamping = spec.spring_damping_ratio;
            settings.mEnabled = spec.enabled;
            native = settings.Create(*body_a, *body_b);
            break;
        }
        }
        return native;
    }

    std::optional<PhysicsConstraintObservation> observation(const ConstraintRecord& record) const {
        const auto body_a = bodies.find(record.spec.body_a);
        const auto body_b = bodies.find(record.spec.body_b);
        if (body_a == bodies.end() || body_b == bodies.end() || record.native == nullptr) return std::nullopt;

        const auto& interface = system->GetBodyInterface();
        const auto anchor_a = world_anchor(interface, body_a->second.native_id, record.spec.frame.anchor_a);
        const auto anchor_b = world_anchor(interface, body_b->second.native_id, record.spec.frame.anchor_b);
        const auto measured_distance = distance_between(anchor_a, anchor_b);
        PhysicsConstraintObservation result;
        result.id = record.spec.id;
        result.type = record.spec.type;
        result.body_a = record.spec.body_a;
        result.body_b = record.spec.body_b;
        result.enabled = record.native->GetEnabled();
        result.backend_created = true;
        result.backend_active = record.native->IsActive();
        result.revision = revision;

        switch (record.spec.type) {
        case PhysicsConstraintType::fixed:
            result.measured_value = measured_distance;
            result.target_value = 0.0F;
            result.error = measured_distance;
            break;
        case PhysicsConstraintType::distance:
            result.measured_value = measured_distance;
            result.target_value = (record.spec.lower_limit + record.spec.upper_limit) * 0.5F;
            result.error = range_error(measured_distance, record.spec.lower_limit, record.spec.upper_limit);
            break;
        case PhysicsConstraintType::hinge:
            result.measured_value = static_cast<const JPH::HingeConstraint*>(record.native.GetPtr())->GetCurrentAngle();
            result.target_value = 0.0F;
            result.error = range_error(result.measured_value, record.spec.lower_limit, record.spec.upper_limit);
            break;
        case PhysicsConstraintType::slider:
            result.measured_value = static_cast<const JPH::SliderConstraint*>(record.native.GetPtr())->GetCurrentPosition();
            result.target_value = 0.0F;
            result.error = range_error(result.measured_value, record.spec.lower_limit, record.spec.upper_limit);
            break;
        case PhysicsConstraintType::spring:
            result.measured_value = measured_distance;
            result.target_value = record.spec.rest_length;
            result.error = measured_distance - record.spec.rest_length;
            break;
        }
        return result;
    }
};

PhysicsConstraintRuntime::PhysicsConstraintRuntime(const std::size_t max_bodies,
                                                   const std::size_t max_constraints)
    : impl_(std::make_unique<Impl>(max_bodies, max_constraints)) {}

PhysicsConstraintRuntime::~PhysicsConstraintRuntime() = default;

PhysicsConstraintResult PhysicsConstraintRuntime::register_body(const PhysicsBodyState& state) {
    if (!valid_body_state(state)) return PhysicsConstraintResult::failed(
        PhysicsConstraintErrorCode::invalid_body, "body state is invalid or outside the bounded runtime contract");
    if (impl_->bodies.contains(state.entity_id)) {
        return PhysicsConstraintResult::failed(PhysicsConstraintErrorCode::body_exists, "body id already exists");
    }
    if (impl_->bodies.size() >= impl_->max_bodies) {
        return PhysicsConstraintResult::failed(PhysicsConstraintErrorCode::body_limit_reached, "body capacity reached");
    }

    const auto shape = make_shape(state);
    if (shape == nullptr) {
        return PhysicsConstraintResult::failed(PhysicsConstraintErrorCode::backend_failure,
                                               "Jolt could not build the requested collision shape");
    }
    const auto motion = jolt_motion_type(state.motion_type);
    const JPH::Quat rotation(state.rotation_x, state.rotation_y, state.rotation_z, state.rotation_w);
    JPH::BodyCreationSettings settings(shape, JPH::RVec3(state.position_x, state.position_y, state.position_z),
                                       rotation.Normalized(), motion,
                                       motion == JPH::EMotionType::Static ? Layers::non_moving : Layers::moving);
    settings.mLinearVelocity = JPH::Vec3(state.velocity_x, state.velocity_y, state.velocity_z);
    settings.mAngularVelocity = JPH::Vec3(state.angular_velocity_x, state.angular_velocity_y, state.angular_velocity_z);
    settings.mGravityFactor = state.gravity_factor;
    settings.mLinearDamping = state.linear_damping;
    settings.mAngularDamping = state.angular_damping;
    settings.mFriction = state.friction;
    settings.mRestitution = state.restitution;
    settings.mAllowSleeping = state.allow_sleeping;
    settings.mIsSensor = state.is_trigger;
    settings.mMotionQuality = state.continuous_collision ? JPH::EMotionQuality::LinearCast : JPH::EMotionQuality::Discrete;
    if (motion == JPH::EMotionType::Dynamic) {
        settings.mOverrideMassProperties = JPH::EOverrideMassProperties::CalculateInertia;
        settings.mMassPropertiesOverride.mMass = state.mass;
    }
    if (state.constrain_to_2d) {
        settings.mAllowedDOFs = JPH::EAllowedDOFs::TranslationX | JPH::EAllowedDOFs::TranslationY;
    }

    auto& body_interface = impl_->system->GetBodyInterface();
    const auto native_id = body_interface.CreateAndAddBody(settings,
                                                           motion == JPH::EMotionType::Dynamic ? JPH::EActivation::Activate :
                                                                                                  JPH::EActivation::DontActivate);
    if (native_id == JPH::BodyID()) {
        return PhysicsConstraintResult::failed(PhysicsConstraintErrorCode::backend_failure,
                                               "Jolt rejected body creation");
    }
    impl_->bodies.emplace(state.entity_id, Impl::BodyRecord{state, native_id});
    ++impl_->revision;
    return PhysicsConstraintResult::succeeded("body registered");
}

PhysicsConstraintResult PhysicsConstraintRuntime::update_body(const PhysicsBodyState& state) {
    if (!valid_body_state(state)) return PhysicsConstraintResult::failed(
        PhysicsConstraintErrorCode::invalid_body, "body state is invalid or outside the bounded runtime contract");
    const auto found = impl_->bodies.find(state.entity_id);
    if (found == impl_->bodies.end()) {
        return PhysicsConstraintResult::failed(PhysicsConstraintErrorCode::body_not_found, "body id was not registered");
    }
    if (!same_body_definition(found->second.state, state)) {
        return PhysicsConstraintResult::failed(PhysicsConstraintErrorCode::unsupported_configuration,
                                               "changing body shape or motion type requires re-registration");
    }
    auto& body_interface = impl_->system->GetBodyInterface();
    const auto rotation = JPH::Quat(state.rotation_x, state.rotation_y, state.rotation_z, state.rotation_w).Normalized();
    const auto activation = state.motion_type == PhysicsMotionType::dynamic_body ? JPH::EActivation::Activate :
                                                                                     JPH::EActivation::DontActivate;
    body_interface.SetPositionAndRotation(JPH::BodyID(found->second.native_id),
                                          JPH::RVec3(state.position_x, state.position_y, state.position_z), rotation,
                                          activation);
    if (state.motion_type != PhysicsMotionType::static_body) {
        body_interface.SetLinearVelocity(found->second.native_id,
                                          JPH::Vec3(state.velocity_x, state.velocity_y, state.velocity_z));
        body_interface.SetAngularVelocity(found->second.native_id,
                                           JPH::Vec3(state.angular_velocity_x, state.angular_velocity_y,
                                                      state.angular_velocity_z));
    }
    found->second.state = state;
    ++impl_->revision;
    return PhysicsConstraintResult::succeeded("body updated");
}

PhysicsConstraintResult PhysicsConstraintRuntime::remove_body(const std::string_view entity_id) {
    const auto found = impl_->bodies.find(std::string(entity_id));
    if (found == impl_->bodies.end()) {
        return PhysicsConstraintResult::failed(PhysicsConstraintErrorCode::body_not_found, "body id was not registered");
    }
    for (const auto& [unused, constraint] : impl_->constraints) {
        static_cast<void>(unused);
        if (constraint.spec.body_a == entity_id || constraint.spec.body_b == entity_id) {
            return PhysicsConstraintResult::failed(PhysicsConstraintErrorCode::body_in_use,
                                                   "remove constraints before removing a referenced body");
        }
    }
    auto& body_interface = impl_->system->GetBodyInterface();
    if (body_interface.IsAdded(found->second.native_id)) body_interface.RemoveBody(found->second.native_id);
    body_interface.DestroyBody(found->second.native_id);
    impl_->bodies.erase(found);
    ++impl_->revision;
    return PhysicsConstraintResult::succeeded("body removed");
}

PhysicsConstraintResult PhysicsConstraintRuntime::create_constraint(const PhysicsConstraintSpec& spec) {
    const auto validation = validate_physics_constraint_spec(spec);
    if (!validation.success) return validation;
    if (impl_->constraints.contains(spec.id)) {
        return PhysicsConstraintResult::failed(PhysicsConstraintErrorCode::constraint_exists,
                                               "constraint id already exists");
    }
    if (impl_->constraints.size() >= impl_->max_constraints) {
        return PhysicsConstraintResult::failed(PhysicsConstraintErrorCode::constraint_limit_reached,
                                               "constraint capacity reached");
    }
    const auto body_a = impl_->bodies.find(spec.body_a);
    const auto body_b = impl_->bodies.find(spec.body_b);
    if (body_a == impl_->bodies.end() || body_b == impl_->bodies.end()) {
        return PhysicsConstraintResult::failed(PhysicsConstraintErrorCode::body_not_found,
                                               "constraint references an unregistered body");
    }
    auto native = impl_->make_native_constraint(spec, body_a->second.native_id, body_b->second.native_id);
    if (native == nullptr) {
        return PhysicsConstraintResult::failed(PhysicsConstraintErrorCode::backend_failure,
                                               "Jolt could not create the requested constraint");
    }
    impl_->system->AddConstraint(native);
    impl_->constraints.emplace(spec.id, Impl::ConstraintRecord{spec, std::move(native)});
    ++impl_->revision;
    return PhysicsConstraintResult::succeeded("constraint created");
}

PhysicsConstraintResult PhysicsConstraintRuntime::remove_constraint(const std::string_view constraint_id) {
    const auto found = impl_->constraints.find(std::string(constraint_id));
    if (found == impl_->constraints.end()) {
        return PhysicsConstraintResult::failed(PhysicsConstraintErrorCode::constraint_not_found,
                                               "constraint id was not found");
    }
    impl_->system->RemoveConstraint(found->second.native);
    impl_->constraints.erase(found);
    ++impl_->revision;
    return PhysicsConstraintResult::succeeded("constraint removed");
}

PhysicsConstraintResult PhysicsConstraintRuntime::set_constraint_enabled(const std::string_view constraint_id,
                                                                          const bool enabled) {
    const auto found = impl_->constraints.find(std::string(constraint_id));
    if (found == impl_->constraints.end()) {
        return PhysicsConstraintResult::failed(PhysicsConstraintErrorCode::constraint_not_found,
                                               "constraint id was not found");
    }
    if (found->second.native->GetEnabled() != enabled) {
        found->second.native->SetEnabled(enabled);
        found->second.spec.enabled = enabled;
        ++impl_->revision;
    }
    return PhysicsConstraintResult::succeeded("constraint state updated");
}

PhysicsConstraintResult PhysicsConstraintRuntime::step(const float delta_seconds) {
    if (!std::isfinite(delta_seconds) || delta_seconds <= 0.0F || delta_seconds > 0.25F) {
        return PhysicsConstraintResult::failed(PhysicsConstraintErrorCode::invalid_argument,
                                               "delta_seconds must be finite and within (0, 0.25]");
    }
    const auto error = impl_->system->Update(delta_seconds, 1, impl_->temp_allocator.get(), impl_->jobs.get());
    if (error != JPH::EPhysicsUpdateError::None) {
        return PhysicsConstraintResult::failed(PhysicsConstraintErrorCode::backend_failure,
                                               "Jolt reported an error while stepping constraints");
    }
    ++impl_->revision;
    return PhysicsConstraintResult::succeeded("physics step complete");
}

std::optional<PhysicsBodyState> PhysicsConstraintRuntime::body_state(const std::string_view entity_id) const {
    const auto found = impl_->bodies.find(std::string(entity_id));
    if (found == impl_->bodies.end()) return std::nullopt;
    auto state = found->second.state;
    const auto& body_interface = impl_->system->GetBodyInterface();
    const auto position = body_interface.GetPosition(found->second.native_id);
    const auto rotation = body_interface.GetRotation(found->second.native_id);
    const auto velocity = body_interface.GetLinearVelocity(found->second.native_id);
    const auto angular_velocity = body_interface.GetAngularVelocity(found->second.native_id);
    state.position_x = static_cast<float>(position.GetX());
    state.position_y = static_cast<float>(position.GetY());
    state.position_z = static_cast<float>(position.GetZ());
    state.rotation_x = rotation.GetX();
    state.rotation_y = rotation.GetY();
    state.rotation_z = rotation.GetZ();
    state.rotation_w = rotation.GetW();
    state.velocity_x = velocity.GetX();
    state.velocity_y = velocity.GetY();
    state.velocity_z = velocity.GetZ();
    state.angular_velocity_x = angular_velocity.GetX();
    state.angular_velocity_y = angular_velocity.GetY();
    state.angular_velocity_z = angular_velocity.GetZ();
    return state;
}

std::optional<PhysicsConstraintObservation> PhysicsConstraintRuntime::observe_constraint(
    const std::string_view constraint_id) const {
    const auto found = impl_->constraints.find(std::string(constraint_id));
    if (found == impl_->constraints.end()) return std::nullopt;
    return impl_->observation(found->second);
}

std::vector<PhysicsConstraintObservation> PhysicsConstraintRuntime::observe_constraints() const {
    std::vector<std::string> ids;
    ids.reserve(impl_->constraints.size());
    for (const auto& [id, unused] : impl_->constraints) {
        static_cast<void>(unused);
        ids.push_back(id);
    }
    std::sort(ids.begin(), ids.end());
    std::vector<PhysicsConstraintObservation> result;
    result.reserve(ids.size());
    for (const auto& id : ids) {
        const auto found = impl_->constraints.find(id);
        if (found == impl_->constraints.end()) continue;
        if (const auto observation = impl_->observation(found->second)) result.push_back(*observation);
    }
    return result;
}

std::uint64_t PhysicsConstraintRuntime::revision() const noexcept {
    return impl_->revision;
}

} // namespace noemancer
