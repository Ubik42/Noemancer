#pragma once

#include "engine/simulation_runtime.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace noemancer {

// Engine-owned constraint vocabulary.  The Jolt adapter is deliberately kept
// out of this header so scene documents, scripting and editor code only depend
// on stable data contracts.
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

// A single plain-data record is enough to author all first-class rigid-body
// constraints.  Limits use metres for distance/slider and radians for hinge.
// For a spring, rest_length and spring_frequency_hz are required and the
// spring is implemented as a soft distance constraint by the private adapter.
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

// Syntax and numeric validation is intentionally available without a runtime
// instance.  Existence checks (body references and duplicate IDs) happen in
// PhysicsConstraintRuntime::create_constraint.
[[nodiscard]] PhysicsConstraintResult validate_physics_constraint_spec(const PhysicsConstraintSpec& spec);

// Backend-neutral ownership for the declarative records.  The main world can
// keep this registry beside its existing body authority and let a backend
// adapter materialize the records; no native physics handle is stored here.
class PhysicsConstraintRegistry final {
public:
    explicit PhysicsConstraintRegistry(std::size_t max_constraints = 1024U);
    ~PhysicsConstraintRegistry();
    PhysicsConstraintRegistry(const PhysicsConstraintRegistry&) = delete;
    PhysicsConstraintRegistry& operator=(const PhysicsConstraintRegistry&) = delete;

    [[nodiscard]] PhysicsConstraintResult add(const PhysicsConstraintSpec& spec);
    [[nodiscard]] PhysicsConstraintResult update(const PhysicsConstraintSpec& spec);
    [[nodiscard]] PhysicsConstraintResult remove(std::string_view constraint_id);
    [[nodiscard]] std::optional<PhysicsConstraintSpec> find(std::string_view constraint_id) const;
    [[nodiscard]] std::vector<PhysicsConstraintSpec> all() const;
    [[nodiscard]] std::uint64_t revision() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

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

// A small, standalone Jolt-backed constraint world.  It is also the adapter
// seam used by the main PhysicsRuntime: callers can feed it the same
// engine-owned PhysicsBodyState values without exposing Jolt handles through
// public APIs.
class PhysicsConstraintRuntime final {
public:
    explicit PhysicsConstraintRuntime(std::size_t max_bodies = 1024U,
                                      std::size_t max_constraints = 1024U);
    ~PhysicsConstraintRuntime();
    PhysicsConstraintRuntime(const PhysicsConstraintRuntime&) = delete;
    PhysicsConstraintRuntime& operator=(const PhysicsConstraintRuntime&) = delete;

    [[nodiscard]] PhysicsConstraintResult register_body(const PhysicsBodyState& state);
    [[nodiscard]] PhysicsConstraintResult update_body(const PhysicsBodyState& state);
    [[nodiscard]] PhysicsConstraintResult remove_body(std::string_view entity_id);

    [[nodiscard]] PhysicsConstraintResult create_constraint(const PhysicsConstraintSpec& spec);
    [[nodiscard]] PhysicsConstraintResult remove_constraint(std::string_view constraint_id);
    [[nodiscard]] PhysicsConstraintResult set_constraint_enabled(std::string_view constraint_id, bool enabled);

    [[nodiscard]] PhysicsConstraintResult step(float delta_seconds);

    [[nodiscard]] std::optional<PhysicsBodyState> body_state(std::string_view entity_id) const;
    [[nodiscard]] std::optional<PhysicsConstraintObservation> observe_constraint(
        std::string_view constraint_id) const;
    [[nodiscard]] std::vector<PhysicsConstraintObservation> observe_constraints() const;
    [[nodiscard]] std::uint64_t revision() const noexcept;
    [[nodiscard]] std::string_view backend_id() const noexcept { return "jolt/5.6.0"; }

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace noemancer
