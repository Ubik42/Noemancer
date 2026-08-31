#include "engine/physics_constraint_debug.hpp"

#include <cmath>
#include <cstddef>
#include <iostream>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace noemancer;

bool expect(const bool condition, const char* message) {
    if (condition) return true;
    std::cerr << message << '\n';
    return false;
}

PhysicsConstraintSpec make_spec(const char* id,
                               const PhysicsConstraintType type,
                               const char* body_a = "body-a",
                               const char* body_b = "body-b") {
    PhysicsConstraintSpec spec;
    spec.id = id;
    spec.type = type;
    spec.body_a = body_a;
    spec.body_b = body_b;
    spec.frame.anchor_a = {0.0F, 0.0F, 0.0F};
    spec.frame.anchor_b = {2.0F, 0.0F, 0.0F};
    spec.frame.primary_axis_a = {0.0F, 1.0F, 0.0F};
    spec.frame.secondary_axis_a = {1.0F, 0.0F, 0.0F};
    spec.frame.primary_axis_b = {0.0F, 1.0F, 0.0F};
    spec.frame.secondary_axis_b = {1.0F, 0.0F, 0.0F};
    switch (type) {
    case PhysicsConstraintType::fixed:
        break;
    case PhysicsConstraintType::distance:
        spec.lower_limit = 1.0F;
        spec.upper_limit = 3.0F;
        break;
    case PhysicsConstraintType::hinge:
        spec.lower_limit = -1.57079632679F;
        spec.upper_limit = 1.57079632679F;
        break;
    case PhysicsConstraintType::slider:
        spec.lower_limit = -2.0F;
        spec.upper_limit = 2.0F;
        break;
    case PhysicsConstraintType::spring:
        spec.rest_length = 2.0F;
        spec.spring_frequency_hz = 3.0F;
        spec.spring_damping_ratio = 0.7F;
        break;
    }
    return spec;
}

PhysicsConstraintDebugBodyPose body(const char* id, const float x) {
    return {id, {x, 0.0F, 0.0F}};
}

bool finite_vec(const PhysicsConstraintVec3& value) {
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

bool same_geometry(const PhysicsConstraintDebugGeometry& left,
                   const PhysicsConstraintDebugGeometry& right) {
    if (left.primitives.size() != right.primitives.size() || left.diagnostics.size() != right.diagnostics.size() ||
        left.constraints_considered != right.constraints_considered ||
        left.constraints_truncated != right.constraints_truncated ||
        left.body_poses_truncated != right.body_poses_truncated ||
        left.primitives_truncated != right.primitives_truncated) {
        return false;
    }
    for (std::size_t index = 0U; index < left.primitives.size(); ++index) {
        const auto& a = left.primitives[index];
        const auto& b = right.primitives[index];
        if (a.id != b.id || a.constraint_id != b.constraint_id || a.kind != b.kind || a.role != b.role ||
            a.style.selected != b.style.selected || a.style.disabled != b.style.disabled ||
            a.style.dashed != b.style.dashed || a.style.diagnostic != b.style.diagnostic ||
            a.start.x != b.start.x || a.start.y != b.start.y || a.start.z != b.start.z ||
            a.end.x != b.end.x || a.end.y != b.end.y || a.end.z != b.end.z ||
            a.center.x != b.center.x || a.center.y != b.center.y || a.center.z != b.center.z ||
            a.axis.x != b.axis.x || a.axis.y != b.axis.y || a.axis.z != b.axis.z ||
            a.radial.x != b.radial.x || a.radial.y != b.radial.y || a.radial.z != b.radial.z ||
            a.radius != b.radius || a.angle_start_radians != b.angle_start_radians ||
            a.angle_end_radians != b.angle_end_radians || a.segments != b.segments) {
            return false;
        }
    }
    for (std::size_t index = 0U; index < left.diagnostics.size(); ++index) {
        const auto& a = left.diagnostics[index];
        const auto& b = right.diagnostics[index];
        if (a.id != b.id || a.constraint_id != b.constraint_id || a.body_id != b.body_id || a.code != b.code ||
            a.detail != b.detail) {
            return false;
        }
    }
    return true;
}

const PhysicsConstraintDebugPrimitive* find_primitive(const PhysicsConstraintDebugGeometry& geometry,
                                                       const char* constraint_id,
                                                       const PhysicsConstraintDebugPrimitiveRole role) {
    for (const auto& primitive : geometry.primitives) {
        if (primitive.constraint_id == constraint_id && primitive.role == role) return &primitive;
    }
    return nullptr;
}

bool has_diagnostic(const PhysicsConstraintDebugGeometry& geometry,
                    const PhysicsConstraintDebugDiagnosticCode code,
                    const std::string_view body_id = {}) {
    for (const auto& diagnostic : geometry.diagnostics) {
        if (diagnostic.code == code && (body_id.empty() || diagnostic.body_id == body_id)) return true;
    }
    return false;
}

} // namespace

int main() {
    const std::vector<PhysicsConstraintSpec> constraints{
        make_spec("constraint.spring", PhysicsConstraintType::spring),
        make_spec("constraint.fixed", PhysicsConstraintType::fixed),
        make_spec("constraint.slider", PhysicsConstraintType::slider),
        make_spec("constraint.hinge", PhysicsConstraintType::hinge),
        make_spec("constraint.distance", PhysicsConstraintType::distance),
    };
    const std::vector<PhysicsConstraintDebugBodyPose> bodies{
        body("body-b", 2.0F),
        body("body-a", 0.0F),
    };

    PhysicsConstraintDebugBuildInput input;
    input.constraints = std::span<const PhysicsConstraintSpec>(constraints);
    input.body_poses = std::span<const PhysicsConstraintDebugBodyPose>(bodies);
    input.selected_constraint_id = "constraint.distance";
    const auto geometry = build_physics_constraint_debug_geometry(input);
    if (!expect(geometry.constraints_considered == 5U && geometry.diagnostics.empty(),
                "valid five-type constraint snapshot produced an unexpected diagnostic")) return 1;
    if (!expect(find_primitive(geometry, "constraint.fixed", PhysicsConstraintDebugPrimitiveRole::connection) != nullptr,
                "fixed constraint did not produce its connection line")) return 2;
    if (!expect(find_primitive(geometry, "constraint.distance", PhysicsConstraintDebugPrimitiveRole::lower_limit) != nullptr,
                "distance constraint did not produce its lower limit")) return 3;
    if (!expect(find_primitive(geometry, "constraint.hinge", PhysicsConstraintDebugPrimitiveRole::angle_limits) != nullptr &&
                    find_primitive(geometry, "constraint.hinge", PhysicsConstraintDebugPrimitiveRole::angle_limits)->kind ==
                        PhysicsConstraintDebugPrimitiveKind::arc,
                "hinge constraint did not produce an angle arc")) return 4;
    if (!expect(find_primitive(geometry, "constraint.slider", PhysicsConstraintDebugPrimitiveRole::travel) != nullptr,
                "slider constraint did not produce a travel line")) return 5;
    if (!expect(find_primitive(geometry, "constraint.spring", PhysicsConstraintDebugPrimitiveRole::spring_coil) != nullptr &&
                    find_primitive(geometry, "constraint.spring", PhysicsConstraintDebugPrimitiveRole::spring_coil)->kind ==
                        PhysicsConstraintDebugPrimitiveKind::arc,
                "spring constraint did not produce a coil arc")) return 6;

    const auto fixed_connection = find_primitive(geometry, "constraint.fixed",
                                                 PhysicsConstraintDebugPrimitiveRole::connection);
    if (!expect(fixed_connection != nullptr && fixed_connection->style.disabled == false &&
                    fixed_connection->style.diagnostic == false,
                "fixed connection style is not the normal deterministic style")) return 7;
    const auto selected_distance = find_primitive(geometry, "constraint.distance",
                                                  PhysicsConstraintDebugPrimitiveRole::connection);
    if (!expect(selected_distance != nullptr && selected_distance->style.selected &&
                    selected_distance->style.line_width > fixed_connection->style.line_width,
                "selected constraint did not receive selected styling")) return 8;

    auto disabled_constraints = constraints;
    disabled_constraints[1].enabled = false;
    input.constraints = std::span<const PhysicsConstraintSpec>(disabled_constraints);
    const auto disabled_geometry = build_physics_constraint_debug_geometry(input);
    const auto disabled_fixed = find_primitive(disabled_geometry, "constraint.fixed",
                                               PhysicsConstraintDebugPrimitiveRole::connection);
    if (!expect(disabled_fixed != nullptr && disabled_fixed->style.disabled && disabled_fixed->style.dashed &&
                    disabled_fixed->style.opacity < 1.0F,
                "disabled constraint did not receive disabled styling")) return 9;

    input.constraints = std::span<const PhysicsConstraintSpec>(constraints);
    const auto geometry_again = build_physics_constraint_debug_geometry(input);
    if (!expect(same_geometry(geometry, geometry_again), "debug geometry was not deterministic across repeated builds")) {
        return 10;
    }

    auto missing_spec = make_spec("constraint.missing", PhysicsConstraintType::distance, "body-a", "body-missing");
    const std::vector<PhysicsConstraintSpec> missing_constraints{missing_spec};
    const std::vector<PhysicsConstraintDebugBodyPose> available_bodies{body("body-a", 0.0F)};
    input.constraints = std::span<const PhysicsConstraintSpec>(missing_constraints);
    input.body_poses = std::span<const PhysicsConstraintDebugBodyPose>(available_bodies);
    const auto missing_geometry = build_physics_constraint_debug_geometry(input);
    if (!expect(has_diagnostic(missing_geometry, PhysicsConstraintDebugDiagnosticCode::missing_body, "body-missing"),
                "missing body was not reported by the debug builder")) return 11;
    if (!expect(find_primitive(missing_geometry, "constraint.missing", PhysicsConstraintDebugPrimitiveRole::diagnostic) !=
                    nullptr,
                "missing body did not receive a diagnostic marker")) return 12;

    auto invalid_spec = make_spec("constraint.invalid", PhysicsConstraintType::spring);
    invalid_spec.frame.anchor_a.x = std::numeric_limits<float>::quiet_NaN();
    const std::vector<PhysicsConstraintSpec> invalid_constraints{invalid_spec};
    input.constraints = std::span<const PhysicsConstraintSpec>(invalid_constraints);
    input.body_poses = std::span<const PhysicsConstraintDebugBodyPose>(bodies);
    const auto invalid_geometry = build_physics_constraint_debug_geometry(input);
    if (!expect(has_diagnostic(invalid_geometry, PhysicsConstraintDebugDiagnosticCode::invalid_constraint) &&
                    invalid_geometry.primitives.empty(),
                "non-finite constraint input was not rejected before geometry emission")) return 13;

    PhysicsConstraintDebugBuildInput bounded_input;
    bounded_input.constraints = std::span<const PhysicsConstraintSpec>(constraints);
    bounded_input.body_poses = std::span<const PhysicsConstraintDebugBodyPose>(bodies);
    bounded_input.limits.maximum_constraints = 2U;
    bounded_input.limits.maximum_primitives = 4U;
    bounded_input.limits.maximum_diagnostics = 16U;
    const auto bounded_geometry = build_physics_constraint_debug_geometry(bounded_input);
    if (!expect(bounded_geometry.constraints_truncated && bounded_geometry.primitives.size() <= 4U &&
                    has_diagnostic(bounded_geometry, PhysicsConstraintDebugDiagnosticCode::constraints_truncated),
                "debug builder did not enforce the hard constraint/primitive budget")) return 14;

    for (const auto& primitive : geometry.primitives) {
        if (!expect(finite_vec(primitive.start) && finite_vec(primitive.end) && finite_vec(primitive.center) &&
                        finite_vec(primitive.axis) && finite_vec(primitive.radial) && std::isfinite(primitive.radius) &&
                        std::isfinite(primitive.angle_start_radians) && std::isfinite(primitive.angle_end_radians),
                    "debug builder emitted a non-finite primitive field")) return 15;
    }
    return 0;
}
