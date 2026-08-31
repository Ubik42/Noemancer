#pragma once

#include "engine/physics_constraint_types.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace noemancer {

// A small renderer-neutral vocabulary for the constraint overlay.  The
// renderer decides how to tessellate these records; the engine only describes
// the semantic shape that should be visible in an editor viewport.
enum class PhysicsConstraintDebugPrimitiveKind : std::uint8_t {
    line,
    arc,
    marker,
};

enum class PhysicsConstraintDebugPrimitiveRole : std::uint8_t {
    body_a,
    body_b,
    anchor_a,
    anchor_b,
    body_link_a,
    body_link_b,
    connection,
    axis_a_primary,
    axis_a_secondary,
    axis_b_primary,
    axis_b_secondary,
    lower_limit,
    upper_limit,
    travel,
    angle_limits,
    rest_length,
    spring_coil,
    diagnostic,
};

enum class PhysicsConstraintDebugDiagnosticCode : std::uint8_t {
    invalid_constraint,
    missing_body,
    invalid_body_pose,
    duplicate_body_pose,
    constraints_truncated,
    body_poses_truncated,
    primitives_truncated,
};

struct PhysicsConstraintDebugColor final {
    float r{};
    float g{};
    float b{};
    float a{1.0F};
};

[[nodiscard]] std::string_view physics_constraint_debug_primitive_kind_name(
    PhysicsConstraintDebugPrimitiveKind kind) noexcept;
[[nodiscard]] std::string_view physics_constraint_debug_primitive_role_name(
    PhysicsConstraintDebugPrimitiveRole role) noexcept;
[[nodiscard]] std::string_view physics_constraint_debug_diagnostic_code_name(
    PhysicsConstraintDebugDiagnosticCode code) noexcept;

struct PhysicsConstraintDebugStyle final {
    PhysicsConstraintDebugColor color{};
    float line_width{1.0F};
    float opacity{1.0F};
    bool selected{};
    bool disabled{};
    bool dashed{};
    bool diagnostic{};
};

// One primitive intentionally has a fixed, inspectable layout.  For a line,
// start/end are used.  For an arc, center/axis/radial/radius and the angle
// range are used.  For a marker, center/radius are used.  Unused fields stay
// at their defaults so a JSON/Agent projection can expose one predictable
// record shape without a tagged union or native graphics handle.
struct PhysicsConstraintDebugPrimitive final {
    std::string id;
    std::string constraint_id;
    PhysicsConstraintDebugPrimitiveKind kind{PhysicsConstraintDebugPrimitiveKind::marker};
    PhysicsConstraintDebugPrimitiveRole role{PhysicsConstraintDebugPrimitiveRole::anchor_a};
    PhysicsConstraintDebugStyle style;

    PhysicsConstraintVec3 start{};
    PhysicsConstraintVec3 end{};
    PhysicsConstraintVec3 center{};
    PhysicsConstraintVec3 axis{0.0F, 1.0F, 0.0F};
    PhysicsConstraintVec3 radial{1.0F, 0.0F, 0.0F};
    float radius{};
    float angle_start_radians{};
    float angle_end_radians{};
    std::uint32_t segments{};
};

struct PhysicsConstraintDebugBodyPose final {
    // This is the stable body identity used by PhysicsConstraintSpec::body_a
    // and body_b.  The pose is already in world space; no backend transform
    // or body handle crosses this boundary.
    std::string body_id;
    PhysicsConstraintVec3 position{};
};

struct PhysicsConstraintDebugLimits final {
    std::size_t maximum_constraints{512U};
    std::size_t maximum_body_poses{4096U};
    std::size_t maximum_primitives{16384U};
    std::size_t maximum_diagnostics{256U};
    std::uint32_t maximum_arc_segments{48U};
    std::uint32_t maximum_spring_coils{24U};
    float axis_length{0.5F};
    float marker_radius{0.08F};
};

struct PhysicsConstraintDebugBuildInput final {
    std::span<const PhysicsConstraintSpec> constraints;
    std::span<const PhysicsConstraintDebugBodyPose> body_poses;
    std::string_view selected_constraint_id{};
    PhysicsConstraintDebugLimits limits;
};

struct PhysicsConstraintDebugDiagnostic final {
    std::string id;
    std::string constraint_id;
    std::string body_id;
    PhysicsConstraintDebugDiagnosticCode code{PhysicsConstraintDebugDiagnosticCode::invalid_constraint};
    std::string detail;
};

struct PhysicsConstraintDebugGeometry final {
    std::vector<PhysicsConstraintDebugPrimitive> primitives;
    std::vector<PhysicsConstraintDebugDiagnostic> diagnostics;
    std::size_t constraints_considered{};
    bool constraints_truncated{};
    bool body_poses_truncated{};
    bool primitives_truncated{};
    bool diagnostics_truncated{};
};

// Builds a deterministic overlay from an immutable constraint snapshot.  The
// function does not mutate the input, does not retain references to it, and
// never emits a non-finite coordinate.  Invalid constraints are reported as
// diagnostics; valid constraints with a missing body still produce the useful
// anchor/limit geometry so the editor can explain what needs fixing.
[[nodiscard]] PhysicsConstraintDebugGeometry build_physics_constraint_debug_geometry(
    const PhysicsConstraintDebugBuildInput& input);

} // namespace noemancer
