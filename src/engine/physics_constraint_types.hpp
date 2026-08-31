#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace noemancer {

// Engine-owned constraint vocabulary.  This header intentionally contains no
// backend or runtime include: scene documents, scripting, the editor and the
// physics runtime can share these records without creating an include cycle.
enum class PhysicsConstraintType : std::uint8_t {
    fixed,
    distance,
    hinge,
    slider,
    spring,
};

[[nodiscard]] std::string_view physics_constraint_type_name(PhysicsConstraintType type) noexcept;

enum class PhysicsConstraintErrorCode : std::uint8_t {
    ok,
    invalid_argument,
    invalid_id,
    invalid_body,
    body_exists,
    body_not_found,
    body_in_use,
    constraint_exists,
    constraint_not_found,
    constraint_limit_reached,
    body_limit_reached,
    unsupported_configuration,
    backend_failure,
};

[[nodiscard]] std::string_view physics_constraint_error_code_name(PhysicsConstraintErrorCode code) noexcept;

struct PhysicsConstraintVec3 final {
    float x{};
    float y{};
    float z{};
};

// All frames are expressed in world space.  The primary/secondary axes form
// the reference frame used by fixed, hinge and slider constraints.  Distance
// and spring constraints only use the anchors, but still carry the complete
// frame so one declarative record can move between constraint types without a
// schema migration.
struct PhysicsConstraintFrame final {
    PhysicsConstraintVec3 anchor_a{};
    PhysicsConstraintVec3 anchor_b{};
    PhysicsConstraintVec3 primary_axis_a{0.0F, 1.0F, 0.0F};
    PhysicsConstraintVec3 secondary_axis_a{1.0F, 0.0F, 0.0F};
    PhysicsConstraintVec3 primary_axis_b{0.0F, 1.0F, 0.0F};
    PhysicsConstraintVec3 secondary_axis_b{1.0F, 0.0F, 0.0F};
};

// One plain-data record authors all first-class rigid-body constraints.
// Limits use metres for distance/slider and radians for hinge.  A spring is
// represented by a soft distance constraint in the private Jolt adapter.
struct PhysicsConstraintSpec final {
    std::string id;
    PhysicsConstraintType type{PhysicsConstraintType::fixed};
    std::string body_a;
    std::string body_b;
    PhysicsConstraintFrame frame;

    float lower_limit{};
    float upper_limit{};
    float rest_length{1.0F};
    float spring_frequency_hz{};
    float spring_damping_ratio{1.0F};
    bool enabled{true};
};

struct PhysicsConstraintResult final {
    bool success{};
    PhysicsConstraintErrorCode code{PhysicsConstraintErrorCode::invalid_argument};
    std::string detail;

    [[nodiscard]] static PhysicsConstraintResult succeeded(std::string detail = {});
    [[nodiscard]] static PhysicsConstraintResult failed(PhysicsConstraintErrorCode code,
                                                        std::string detail);
};

// Syntax and numeric validation is available to every runtime adapter without
// requiring a backend include.  Body existence and duplicate-ID checks remain
// local to the owning runtime.
[[nodiscard]] PhysicsConstraintResult validate_physics_constraint_spec(const PhysicsConstraintSpec& spec);

struct PhysicsConstraintObservation final {
    std::string id;
    PhysicsConstraintType type{PhysicsConstraintType::fixed};
    std::string body_a;
    std::string body_b;
    bool enabled{};
    bool backend_created{};
    bool backend_active{};
    float measured_value{};
    float target_value{};
    float error{};
    std::uint64_t revision{};
};

} // namespace noemancer
