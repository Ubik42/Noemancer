#include "engine/physics_constraint_debug.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace noemancer {

namespace {

constexpr float kPi = 3.14159265358979323846F;
constexpr float kTwoPi = 2.0F * kPi;
constexpr float kEpsilon = 1.0e-5F;
constexpr float kMaximumCoordinate = 1.0e6F;
constexpr float kMaximumExtent = 1.0e4F;
constexpr std::size_t kHardMaximumConstraints = 4096U;
constexpr std::size_t kHardMaximumBodyPoses = 16384U;
constexpr std::size_t kHardMaximumPrimitives = 65536U;
constexpr std::size_t kHardMaximumDiagnostics = 2048U;
constexpr std::uint32_t kHardMaximumArcSegments = 128U;
constexpr std::uint32_t kHardMaximumSpringCoils = 64U;
constexpr std::size_t kMaximumIdentifierBytes = 96U;

bool finite_bounded(const float value, const float maximum = kMaximumCoordinate) noexcept {
    return std::isfinite(value) && std::abs(value) <= maximum;
}

bool finite_vec(const PhysicsConstraintVec3& value,
                const float maximum = kMaximumCoordinate) noexcept {
    return finite_bounded(value.x, maximum) && finite_bounded(value.y, maximum) &&
           finite_bounded(value.z, maximum);
}

PhysicsConstraintVec3 operator+(const PhysicsConstraintVec3 left,
                               const PhysicsConstraintVec3 right) noexcept {
    return {left.x + right.x, left.y + right.y, left.z + right.z};
}

PhysicsConstraintVec3 operator-(const PhysicsConstraintVec3 left,
                               const PhysicsConstraintVec3 right) noexcept {
    return {left.x - right.x, left.y - right.y, left.z - right.z};
}

PhysicsConstraintVec3 operator*(const PhysicsConstraintVec3 value,
                               const float scale) noexcept {
    return {value.x * scale, value.y * scale, value.z * scale};
}

float dot(const PhysicsConstraintVec3 left, const PhysicsConstraintVec3 right) noexcept {
    return left.x * right.x + left.y * right.y + left.z * right.z;
}

PhysicsConstraintVec3 cross(const PhysicsConstraintVec3 left,
                            const PhysicsConstraintVec3 right) noexcept {
    return {left.y * right.z - left.z * right.y,
            left.z * right.x - left.x * right.z,
            left.x * right.y - left.y * right.x};
}

float length_squared(const PhysicsConstraintVec3 value) noexcept {
    return dot(value, value);
}

float length(const PhysicsConstraintVec3 value) noexcept {
    const auto squared = length_squared(value);
    return squared > 0.0F && std::isfinite(squared) ? std::sqrt(squared) : 0.0F;
}

PhysicsConstraintVec3 normalized(const PhysicsConstraintVec3 value,
                                 const PhysicsConstraintVec3 fallback) noexcept {
    const auto magnitude = length(value);
    if (magnitude <= kEpsilon || !std::isfinite(magnitude)) return fallback;
    return value * (1.0F / magnitude);
}

PhysicsConstraintVec3 perpendicular(const PhysicsConstraintVec3 axis) noexcept {
    const auto candidate = std::abs(axis.x) < 0.8F
                               ? PhysicsConstraintVec3{1.0F, 0.0F, 0.0F}
                               : PhysicsConstraintVec3{0.0F, 1.0F, 0.0F};
    return normalized(cross(axis, candidate), {0.0F, 0.0F, 1.0F});
}

PhysicsConstraintVec3 rotate_in_plane(const PhysicsConstraintVec3 center,
                                      const PhysicsConstraintVec3 axis,
                                      const PhysicsConstraintVec3 radial,
                                      const float radius,
                                      const float angle) noexcept {
    const auto tangent = normalized(cross(axis, radial), perpendicular(axis));
    return center + radial * (std::cos(angle) * radius) + tangent * (std::sin(angle) * radius);
}

bool valid_identifier(const std::string_view value) noexcept {
    if (value.empty() || value.size() > kMaximumIdentifierBytes) return false;
    for (const auto byte : value) {
        const auto character = static_cast<unsigned char>(byte);
        if (character < 0x20U || character == 0x7fU || character == '/' || character == '\\') return false;
    }
    return true;
}

std::string bounded_text(const std::string_view value) {
    constexpr std::size_t maximum = kMaximumIdentifierBytes;
    return std::string(value.substr(0U, std::min(value.size(), maximum)));
}

std::string debug_id(const std::string_view constraint_id, const std::string_view suffix) {
    std::string result;
    result.reserve(32U + constraint_id.size() + suffix.size());
    result.append("physics.constraint.debug/");
    result.append(constraint_id);
    result.push_back('/');
    result.append(suffix);
    return result;
}

PhysicsConstraintDebugColor color_for(const PhysicsConstraintType type) noexcept {
    switch (type) {
    case PhysicsConstraintType::fixed:
        return {0.28F, 0.72F, 1.0F, 1.0F};
    case PhysicsConstraintType::distance:
        return {0.20F, 0.88F, 0.56F, 1.0F};
    case PhysicsConstraintType::hinge:
        return {1.0F, 0.66F, 0.20F, 1.0F};
    case PhysicsConstraintType::slider:
        return {0.72F, 0.48F, 1.0F, 1.0F};
    case PhysicsConstraintType::spring:
        return {1.0F, 0.36F, 0.58F, 1.0F};
    }
    return {1.0F, 0.20F, 0.16F, 1.0F};
}

PhysicsConstraintDebugStyle style_for(const PhysicsConstraintSpec& spec,
                                      const bool selected) noexcept {
    PhysicsConstraintDebugStyle style;
    style.color = color_for(spec.type);
    style.line_width = selected ? 3.0F : 2.0F;
    style.opacity = spec.enabled ? 1.0F : 0.32F;
    style.selected = selected;
    style.disabled = !spec.enabled;
    style.dashed = !spec.enabled;
    return style;
}

PhysicsConstraintDebugStyle diagnostic_style() noexcept {
    PhysicsConstraintDebugStyle style;
    style.color = {1.0F, 0.18F, 0.12F, 1.0F};
    style.line_width = 2.5F;
    style.opacity = 1.0F;
    style.dashed = true;
    style.diagnostic = true;
    return style;
}

struct BodyEntry final {
    const PhysicsConstraintDebugBodyPose* pose{};
    std::size_t ordinal{};
    bool valid_position{};
};

struct ConstraintEntry final {
    const PhysicsConstraintSpec* spec{};
    std::size_t ordinal{};
};

struct Builder final {
    const PhysicsConstraintDebugBuildInput& input;
    PhysicsConstraintDebugGeometry output;
    PhysicsConstraintDebugLimits limits;
    std::vector<BodyEntry> bodies;
    bool primitive_cap_reported{};

    explicit Builder(const PhysicsConstraintDebugBuildInput& source) : input(source), limits(source.limits) {
        limits.maximum_constraints = std::min(limits.maximum_constraints, kHardMaximumConstraints);
        limits.maximum_body_poses = std::min(limits.maximum_body_poses, kHardMaximumBodyPoses);
        limits.maximum_primitives = std::min(limits.maximum_primitives, kHardMaximumPrimitives);
        limits.maximum_diagnostics = std::min(limits.maximum_diagnostics, kHardMaximumDiagnostics);
        limits.maximum_arc_segments = std::clamp(limits.maximum_arc_segments, 4U, kHardMaximumArcSegments);
        limits.maximum_spring_coils = std::clamp(limits.maximum_spring_coils, 1U, kHardMaximumSpringCoils);
        limits.axis_length = finite_bounded(limits.axis_length, kMaximumExtent) && limits.axis_length > kEpsilon
                                 ? limits.axis_length
                                 : 0.5F;
        limits.marker_radius = finite_bounded(limits.marker_radius, kMaximumExtent) && limits.marker_radius > kEpsilon
                                   ? limits.marker_radius
                                   : 0.08F;
        limits.marker_radius = std::min(limits.marker_radius,
                                        std::max(limits.axis_length * 0.5F, kEpsilon));
        output.primitives.reserve(std::min(limits.maximum_primitives, std::size_t{256U}));
        output.diagnostics.reserve(std::min(limits.maximum_diagnostics, std::size_t{32U}));
    }

    void add_diagnostic(const PhysicsConstraintDebugDiagnosticCode code,
                        const std::string_view constraint_id,
                        const std::string_view body_id,
                        const std::string_view detail,
                        const std::size_t ordinal = 0U) {
        if (output.diagnostics.size() >= limits.maximum_diagnostics) {
            output.diagnostics_truncated = true;
            return;
        }
        PhysicsConstraintDebugDiagnostic diagnostic;
        diagnostic.constraint_id = bounded_text(constraint_id);
        diagnostic.body_id = bounded_text(body_id);
        diagnostic.code = code;
        diagnostic.detail = std::string(detail);
        diagnostic.id = "physics.constraint.debug/diagnostic/";
        diagnostic.id.append(physics_constraint_debug_diagnostic_code_name(code));
        if (!diagnostic.constraint_id.empty()) {
            diagnostic.id.push_back('/');
            diagnostic.id.append(diagnostic.constraint_id);
        }
        if (!diagnostic.body_id.empty()) {
            diagnostic.id.push_back('/');
            diagnostic.id.append(diagnostic.body_id);
        }
        if (ordinal != 0U) {
            diagnostic.id.push_back('/');
            diagnostic.id.append(std::to_string(ordinal));
        }
        output.diagnostics.push_back(std::move(diagnostic));
    }

    bool append(PhysicsConstraintDebugPrimitive primitive) {
        if (output.primitives.size() >= limits.maximum_primitives) {
            output.primitives_truncated = true;
            if (!primitive_cap_reported) {
                primitive_cap_reported = true;
                add_diagnostic(PhysicsConstraintDebugDiagnosticCode::primitives_truncated, {}, {},
                               "The debug primitive budget was reached; remaining geometry was omitted.");
            }
            return false;
        }
        output.primitives.push_back(std::move(primitive));
        return true;
    }

    PhysicsConstraintDebugPrimitive base(const PhysicsConstraintSpec& spec,
                                         const PhysicsConstraintDebugPrimitiveKind kind,
                                         const PhysicsConstraintDebugPrimitiveRole role,
                                         const std::string_view suffix) const {
        PhysicsConstraintDebugPrimitive primitive;
        primitive.id = debug_id(spec.id, suffix);
        primitive.constraint_id = bounded_text(spec.id);
        primitive.kind = kind;
        primitive.role = role;
        primitive.style = style_for(spec, spec.id == input.selected_constraint_id);
        return primitive;
    }

    void add_line(const PhysicsConstraintSpec& spec,
                  const PhysicsConstraintDebugPrimitiveRole role,
                  const std::string_view suffix,
                  const PhysicsConstraintVec3 start,
                  const PhysicsConstraintVec3 end,
                  const bool dashed = false) {
        PhysicsConstraintDebugPrimitive primitive = base(spec, PhysicsConstraintDebugPrimitiveKind::line, role, suffix);
        primitive.start = start;
        primitive.end = end;
        primitive.style.dashed = primitive.style.dashed || dashed;
        append(std::move(primitive));
    }

    void add_marker(const PhysicsConstraintSpec& spec,
                    const PhysicsConstraintDebugPrimitiveRole role,
                    const std::string_view suffix,
                    const PhysicsConstraintVec3 center,
                    const float scale = 1.0F) {
        PhysicsConstraintDebugPrimitive primitive = base(spec, PhysicsConstraintDebugPrimitiveKind::marker, role, suffix);
        primitive.center = center;
        primitive.radius = limits.marker_radius * std::clamp(scale, 0.25F, 8.0F);
        primitive.segments = 12U;
        append(std::move(primitive));
    }

    void add_diagnostic_marker(const PhysicsConstraintSpec& spec,
                               const std::string_view suffix,
                               const PhysicsConstraintVec3 center) {
        PhysicsConstraintDebugPrimitive primitive = base(spec, PhysicsConstraintDebugPrimitiveKind::marker,
                                                         PhysicsConstraintDebugPrimitiveRole::diagnostic, suffix);
        primitive.center = center;
        primitive.radius = limits.marker_radius * 1.75F;
        primitive.segments = 8U;
        primitive.style = diagnostic_style();
        primitive.style.selected = spec.id == input.selected_constraint_id;
        append(std::move(primitive));
    }

    void add_arc(const PhysicsConstraintSpec& spec,
                 const PhysicsConstraintDebugPrimitiveRole role,
                 const std::string_view suffix,
                 const PhysicsConstraintVec3 center,
                 const PhysicsConstraintVec3 axis,
                 const PhysicsConstraintVec3 radial,
                 const float radius,
                 const float start_angle,
                 const float end_angle,
                 const std::uint32_t segments) {
        PhysicsConstraintDebugPrimitive primitive = base(spec, PhysicsConstraintDebugPrimitiveKind::arc, role, suffix);
        primitive.center = center;
        primitive.axis = normalized(axis, {0.0F, 1.0F, 0.0F});
        primitive.radial = normalized(radial, perpendicular(primitive.axis));
        primitive.radius = std::clamp(finite_bounded(radius, kMaximumExtent) ? radius : limits.axis_length,
                                      limits.marker_radius * 0.5F, kMaximumExtent);
        primitive.angle_start_radians = std::isfinite(start_angle) ? start_angle : 0.0F;
        primitive.angle_end_radians = std::isfinite(end_angle) ? end_angle : 0.0F;
        primitive.segments = std::clamp(segments, 4U, limits.maximum_arc_segments);
        append(std::move(primitive));
    }

    const BodyEntry* find_body(const std::string_view id) const noexcept {
        const auto found = std::lower_bound(
            bodies.begin(), bodies.end(), id,
            [](const BodyEntry& entry, const std::string_view value) {
                return std::string_view(entry.pose->body_id) < value;
            });
        if (found == bodies.end() || found->pose->body_id != id) return nullptr;
        return &*found;
    }

    void prepare_bodies() {
        const auto count = std::min(input.body_poses.size(), limits.maximum_body_poses);
        if (input.body_poses.size() > count) {
            output.body_poses_truncated = true;
            add_diagnostic(PhysicsConstraintDebugDiagnosticCode::body_poses_truncated, {}, {},
                           "The body pose budget was reached; later poses are unavailable to the overlay.");
        }
        bodies.reserve(count);
        for (std::size_t index = 0U; index < count; ++index) {
            const auto& pose = input.body_poses[index];
            if (!valid_identifier(pose.body_id)) {
                add_diagnostic(PhysicsConstraintDebugDiagnosticCode::invalid_body_pose, {}, pose.body_id,
                               "Body pose id is empty, too long, or contains an unsafe path character.", index + 1U);
                continue;
            }
            const auto valid_position = finite_vec(pose.position);
            if (!valid_position) {
                add_diagnostic(PhysicsConstraintDebugDiagnosticCode::invalid_body_pose, {}, pose.body_id,
                               "Body pose position must be finite and within the debug coordinate bound.", index + 1U);
            }
            bodies.push_back({&pose, index, valid_position});
        }
        std::stable_sort(bodies.begin(), bodies.end(), [](const BodyEntry& left, const BodyEntry& right) {
            if (left.pose->body_id != right.pose->body_id) return left.pose->body_id < right.pose->body_id;
            return left.ordinal < right.ordinal;
        });
        for (std::size_t index = 1U; index < bodies.size(); ++index) {
            if (bodies[index - 1U].pose->body_id == bodies[index].pose->body_id) {
                add_diagnostic(PhysicsConstraintDebugDiagnosticCode::duplicate_body_pose, {},
                               bodies[index].pose->body_id,
                               "Multiple body poses use the same stable id; the first pose is used.",
                               bodies[index].ordinal + 1U);
            }
        }
    }

    void add_common(const PhysicsConstraintSpec& spec,
                    const BodyEntry* body_a,
                    const BodyEntry* body_b) {
        const auto& frame = spec.frame;
        add_marker(spec, PhysicsConstraintDebugPrimitiveRole::anchor_a, "anchor-a", frame.anchor_a);
        add_marker(spec, PhysicsConstraintDebugPrimitiveRole::anchor_b, "anchor-b", frame.anchor_b);
        if (body_a != nullptr && body_a->valid_position) {
            add_marker(spec, PhysicsConstraintDebugPrimitiveRole::body_a, "body-a", body_a->pose->position, 0.75F);
            add_line(spec, PhysicsConstraintDebugPrimitiveRole::body_link_a, "body-link-a", body_a->pose->position,
                     frame.anchor_a, true);
        }
        if (body_b != nullptr && body_b->valid_position) {
            add_marker(spec, PhysicsConstraintDebugPrimitiveRole::body_b, "body-b", body_b->pose->position, 0.75F);
            add_line(spec, PhysicsConstraintDebugPrimitiveRole::body_link_b, "body-link-b", body_b->pose->position,
                     frame.anchor_b, true);
        }
    }

    void add_missing_body_diagnostic(const PhysicsConstraintSpec& spec,
                                     const std::string_view body_id,
                                     const std::string_view suffix,
                                     const PhysicsConstraintVec3 anchor,
                                     const bool invalid_pose) {
        add_diagnostic(invalid_pose ? PhysicsConstraintDebugDiagnosticCode::invalid_body_pose
                                    : PhysicsConstraintDebugDiagnosticCode::missing_body,
                       spec.id, body_id,
                       invalid_pose ? "The referenced body pose is non-finite; body geometry was omitted."
                                     : "The referenced body is not present in the supplied pose lookup.");
        add_diagnostic_marker(spec, suffix, anchor);
    }

    void add_fixed(const PhysicsConstraintSpec& spec) {
        const auto axis_a = normalized(spec.frame.primary_axis_a, {0.0F, 1.0F, 0.0F});
        const auto secondary_a = normalized(spec.frame.secondary_axis_a, perpendicular(axis_a));
        const auto axis_b = normalized(spec.frame.primary_axis_b, {0.0F, 1.0F, 0.0F});
        const auto secondary_b = normalized(spec.frame.secondary_axis_b, perpendicular(axis_b));
        add_line(spec, PhysicsConstraintDebugPrimitiveRole::connection, "connection", spec.frame.anchor_a,
                 spec.frame.anchor_b);
        add_line(spec, PhysicsConstraintDebugPrimitiveRole::axis_a_primary, "axis-a-primary", spec.frame.anchor_a,
                 spec.frame.anchor_a + axis_a * limits.axis_length);
        add_line(spec, PhysicsConstraintDebugPrimitiveRole::axis_a_secondary, "axis-a-secondary", spec.frame.anchor_a,
                 spec.frame.anchor_a + secondary_a * (limits.axis_length * 0.7F));
        add_line(spec, PhysicsConstraintDebugPrimitiveRole::axis_b_primary, "axis-b-primary", spec.frame.anchor_b,
                 spec.frame.anchor_b + axis_b * limits.axis_length);
        add_line(spec, PhysicsConstraintDebugPrimitiveRole::axis_b_secondary, "axis-b-secondary", spec.frame.anchor_b,
                 spec.frame.anchor_b + secondary_b * (limits.axis_length * 0.7F));
    }

    PhysicsConstraintVec3 span_direction(const PhysicsConstraintSpec& spec,
                                         float& actual_length) const noexcept {
        const auto delta = spec.frame.anchor_b - spec.frame.anchor_a;
        actual_length = length(delta);
        if (actual_length > kEpsilon && finite_bounded(actual_length, kMaximumExtent)) return delta * (1.0F / actual_length);
        return normalized(spec.frame.primary_axis_a, {1.0F, 0.0F, 0.0F});
    }

    void add_distance(const PhysicsConstraintSpec& spec) {
        float actual_length{};
        const auto direction = span_direction(spec, actual_length);
        add_line(spec, PhysicsConstraintDebugPrimitiveRole::connection, "connection", spec.frame.anchor_a,
                 spec.frame.anchor_b);
        const auto lower = spec.frame.anchor_a + direction * std::clamp(spec.lower_limit, 0.0F, kMaximumExtent);
        const auto upper = spec.frame.anchor_a + direction * std::clamp(spec.upper_limit, 0.0F, kMaximumExtent);
        add_line(spec, PhysicsConstraintDebugPrimitiveRole::lower_limit, "lower-limit", spec.frame.anchor_a, lower, true);
        add_marker(spec, PhysicsConstraintDebugPrimitiveRole::lower_limit, "lower-limit-marker", lower, 0.75F);
        add_line(spec, PhysicsConstraintDebugPrimitiveRole::upper_limit, "upper-limit", spec.frame.anchor_a, upper, true);
        add_marker(spec, PhysicsConstraintDebugPrimitiveRole::upper_limit, "upper-limit-marker", upper, 0.75F);
        (void)actual_length;
    }

    void add_hinge(const PhysicsConstraintSpec& spec) {
        float actual_length{};
        const auto direction = span_direction(spec, actual_length);
        const auto axis = normalized(spec.frame.primary_axis_a, {0.0F, 1.0F, 0.0F});
        const auto radial = normalized(spec.frame.secondary_axis_a, perpendicular(axis));
        const auto radius = std::clamp(std::max(actual_length * 0.5F, limits.axis_length * 1.5F),
                                       limits.marker_radius * 2.0F, 4.0F * limits.axis_length);
        const auto span = std::abs(spec.upper_limit - spec.lower_limit);
        const auto segment_estimate = static_cast<std::uint32_t>(std::ceil(span * 12.0F / kPi));
        const auto segments = std::clamp(std::max(segment_estimate, 4U), 4U, limits.maximum_arc_segments);
        add_line(spec, PhysicsConstraintDebugPrimitiveRole::connection, "connection", spec.frame.anchor_a,
                 spec.frame.anchor_b);
        add_line(spec, PhysicsConstraintDebugPrimitiveRole::axis_a_primary, "hinge-axis", spec.frame.anchor_a,
                 spec.frame.anchor_a + axis * limits.axis_length);
        add_arc(spec, PhysicsConstraintDebugPrimitiveRole::angle_limits, "angle-limits", spec.frame.anchor_a, axis,
                radial, radius, spec.lower_limit, spec.upper_limit, segments);
        add_line(spec, PhysicsConstraintDebugPrimitiveRole::lower_limit, "lower-limit", spec.frame.anchor_a,
                 rotate_in_plane(spec.frame.anchor_a, axis, radial, radius, spec.lower_limit), true);
        add_line(spec, PhysicsConstraintDebugPrimitiveRole::upper_limit, "upper-limit", spec.frame.anchor_a,
                 rotate_in_plane(spec.frame.anchor_a, axis, radial, radius, spec.upper_limit), true);
        (void)direction;
    }

    void add_slider(const PhysicsConstraintSpec& spec) {
        const auto axis = normalized(spec.frame.primary_axis_a, {1.0F, 0.0F, 0.0F});
        const auto lower = spec.frame.anchor_a + axis * std::clamp(spec.lower_limit, -kMaximumExtent, kMaximumExtent);
        const auto upper = spec.frame.anchor_a + axis * std::clamp(spec.upper_limit, -kMaximumExtent, kMaximumExtent);
        add_line(spec, PhysicsConstraintDebugPrimitiveRole::connection, "connection", spec.frame.anchor_a,
                 spec.frame.anchor_b);
        add_line(spec, PhysicsConstraintDebugPrimitiveRole::travel, "travel", lower, upper);
        add_marker(spec, PhysicsConstraintDebugPrimitiveRole::lower_limit, "lower-limit-marker", lower, 0.75F);
        add_marker(spec, PhysicsConstraintDebugPrimitiveRole::upper_limit, "upper-limit-marker", upper, 0.75F);
        add_line(spec, PhysicsConstraintDebugPrimitiveRole::axis_a_primary, "slider-axis", spec.frame.anchor_a,
                 spec.frame.anchor_a + axis * limits.axis_length);
    }

    void add_spring(const PhysicsConstraintSpec& spec) {
        float actual_length{};
        const auto direction = span_direction(spec, actual_length);
        const auto display_length = std::clamp(std::max({actual_length, spec.rest_length, limits.axis_length * 2.0F}),
                                               limits.marker_radius * 2.0F, kMaximumExtent);
        const auto rest_endpoint = spec.frame.anchor_a + direction * std::clamp(spec.rest_length, 0.0F, kMaximumExtent);
        add_line(spec, PhysicsConstraintDebugPrimitiveRole::connection, "connection", spec.frame.anchor_a,
                 spec.frame.anchor_b);
        PhysicsConstraintDebugPrimitive rest = base(spec, PhysicsConstraintDebugPrimitiveKind::line,
                                                    PhysicsConstraintDebugPrimitiveRole::rest_length, "rest-length");
        rest.start = spec.frame.anchor_a;
        rest.end = rest_endpoint;
        rest.style.dashed = true;
        rest.style.opacity *= 0.75F;
        append(std::move(rest));
        add_marker(spec, PhysicsConstraintDebugPrimitiveRole::rest_length, "rest-length-marker", rest_endpoint, 0.75F);

        const auto coil_denominator = std::max(spec.rest_length * 0.5F, 0.2F);
        const auto estimated_coils = static_cast<std::uint32_t>(std::ceil(display_length / coil_denominator));
        const auto coils = std::clamp(std::max(estimated_coils, 4U), 1U, limits.maximum_spring_coils);
        const auto radial = perpendicular(direction);
        const auto coil_radius = std::clamp(std::min(limits.axis_length * 0.35F,
                                                      display_length / static_cast<float>(coils * 3U)),
                                                      limits.marker_radius * 0.55F, limits.axis_length);
        const auto segments = std::clamp(12U, 4U, limits.maximum_arc_segments);
        for (std::uint32_t index = 0U; index < coils; ++index) {
            const auto fraction = (static_cast<float>(index) + 0.5F) / static_cast<float>(coils);
            const auto center = spec.frame.anchor_a + direction * (display_length * fraction);
            add_arc(spec, PhysicsConstraintDebugPrimitiveRole::spring_coil,
                    std::string("spring-coil-") + std::to_string(index), center, direction, radial, coil_radius, 0.0F,
                    kTwoPi, segments);
        }
    }

    void emit(const PhysicsConstraintSpec& spec,
              const BodyEntry* body_a,
              const BodyEntry* body_b) {
        add_common(spec, body_a, body_b);
        switch (spec.type) {
        case PhysicsConstraintType::fixed:
            add_fixed(spec);
            break;
        case PhysicsConstraintType::distance:
            add_distance(spec);
            break;
        case PhysicsConstraintType::hinge:
            add_hinge(spec);
            break;
        case PhysicsConstraintType::slider:
            add_slider(spec);
            break;
        case PhysicsConstraintType::spring:
            add_spring(spec);
            break;
        }
    }

    PhysicsConstraintDebugGeometry run() {
        prepare_bodies();
        const auto count = std::min(input.constraints.size(), limits.maximum_constraints);
        if (input.constraints.size() > count) {
            output.constraints_truncated = true;
            add_diagnostic(PhysicsConstraintDebugDiagnosticCode::constraints_truncated, {}, {},
                           "The constraint budget was reached; later records are unavailable to the overlay.");
        }

        std::vector<ConstraintEntry> constraints;
        constraints.reserve(count);
        for (std::size_t index = 0U; index < count; ++index) constraints.push_back({&input.constraints[index], index});
        std::stable_sort(constraints.begin(), constraints.end(), [](const ConstraintEntry& left, const ConstraintEntry& right) {
            if (left.spec->id != right.spec->id) return left.spec->id < right.spec->id;
            return left.ordinal < right.ordinal;
        });

        std::string previous_id;
        for (const auto& entry : constraints) {
            const auto& spec = *entry.spec;
            if (spec.id == previous_id) {
                add_diagnostic(PhysicsConstraintDebugDiagnosticCode::invalid_constraint, spec.id, {},
                               "Duplicate constraint stable id was skipped from the debug overlay.", entry.ordinal + 1U);
                continue;
            }
            previous_id = spec.id;
            ++output.constraints_considered;

            const auto validation = validate_physics_constraint_spec(spec);
            if (!validation.success) {
                add_diagnostic(PhysicsConstraintDebugDiagnosticCode::invalid_constraint, spec.id, {},
                               validation.detail.empty() ? "Constraint record is invalid." : validation.detail);
                continue;
            }

            const auto* body_a = find_body(spec.body_a);
            const auto* body_b = find_body(spec.body_b);
            if (body_a == nullptr) {
                add_missing_body_diagnostic(spec, spec.body_a, "missing-body-a", spec.frame.anchor_a, false);
            } else if (!body_a->valid_position) {
                add_missing_body_diagnostic(spec, spec.body_a, "invalid-body-a", spec.frame.anchor_a, true);
            }
            if (body_b == nullptr) {
                add_missing_body_diagnostic(spec, spec.body_b, "missing-body-b", spec.frame.anchor_b, false);
            } else if (!body_b->valid_position) {
                add_missing_body_diagnostic(spec, spec.body_b, "invalid-body-b", spec.frame.anchor_b, true);
            }
            emit(spec, body_a, body_b);
        }
        return std::move(output);
    }
};

} // namespace

std::string_view physics_constraint_debug_primitive_kind_name(
    const PhysicsConstraintDebugPrimitiveKind kind) noexcept {
    switch (kind) {
    case PhysicsConstraintDebugPrimitiveKind::line:
        return "line";
    case PhysicsConstraintDebugPrimitiveKind::arc:
        return "arc";
    case PhysicsConstraintDebugPrimitiveKind::marker:
        return "marker";
    }
    return "unknown";
}

std::string_view physics_constraint_debug_primitive_role_name(
    const PhysicsConstraintDebugPrimitiveRole role) noexcept {
    switch (role) {
    case PhysicsConstraintDebugPrimitiveRole::body_a:
        return "body-a";
    case PhysicsConstraintDebugPrimitiveRole::body_b:
        return "body-b";
    case PhysicsConstraintDebugPrimitiveRole::anchor_a:
        return "anchor-a";
    case PhysicsConstraintDebugPrimitiveRole::anchor_b:
        return "anchor-b";
    case PhysicsConstraintDebugPrimitiveRole::body_link_a:
        return "body-link-a";
    case PhysicsConstraintDebugPrimitiveRole::body_link_b:
        return "body-link-b";
    case PhysicsConstraintDebugPrimitiveRole::connection:
        return "connection";
    case PhysicsConstraintDebugPrimitiveRole::axis_a_primary:
        return "axis-a-primary";
    case PhysicsConstraintDebugPrimitiveRole::axis_a_secondary:
        return "axis-a-secondary";
    case PhysicsConstraintDebugPrimitiveRole::axis_b_primary:
        return "axis-b-primary";
    case PhysicsConstraintDebugPrimitiveRole::axis_b_secondary:
        return "axis-b-secondary";
    case PhysicsConstraintDebugPrimitiveRole::lower_limit:
        return "lower-limit";
    case PhysicsConstraintDebugPrimitiveRole::upper_limit:
        return "upper-limit";
    case PhysicsConstraintDebugPrimitiveRole::travel:
        return "travel";
    case PhysicsConstraintDebugPrimitiveRole::angle_limits:
        return "angle-limits";
    case PhysicsConstraintDebugPrimitiveRole::rest_length:
        return "rest-length";
    case PhysicsConstraintDebugPrimitiveRole::spring_coil:
        return "spring-coil";
    case PhysicsConstraintDebugPrimitiveRole::diagnostic:
        return "diagnostic";
    }
    return "unknown";
}

std::string_view physics_constraint_debug_diagnostic_code_name(
    const PhysicsConstraintDebugDiagnosticCode code) noexcept {
    switch (code) {
    case PhysicsConstraintDebugDiagnosticCode::invalid_constraint:
        return "invalid-constraint";
    case PhysicsConstraintDebugDiagnosticCode::missing_body:
        return "missing-body";
    case PhysicsConstraintDebugDiagnosticCode::invalid_body_pose:
        return "invalid-body-pose";
    case PhysicsConstraintDebugDiagnosticCode::duplicate_body_pose:
        return "duplicate-body-pose";
    case PhysicsConstraintDebugDiagnosticCode::constraints_truncated:
        return "constraints-truncated";
    case PhysicsConstraintDebugDiagnosticCode::body_poses_truncated:
        return "body-poses-truncated";
    case PhysicsConstraintDebugDiagnosticCode::primitives_truncated:
        return "primitives-truncated";
    }
    return "unknown";
}

PhysicsConstraintDebugGeometry build_physics_constraint_debug_geometry(
    const PhysicsConstraintDebugBuildInput& input) {
    return Builder(input).run();
}

} // namespace noemancer
