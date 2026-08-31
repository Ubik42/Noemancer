#pragma once

#include "engine/physics_constraint_types.hpp"
#include "engine/simulation_runtime.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace noemancer {

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

// A small, standalone Jolt-backed constraint fixture for focused backend
// tests and tools.  Production worlds use PhysicsRuntime, which materializes
// the same records in its existing body PhysicsSystem; keeping this fixture
// separate prevents test-only ownership from leaking into the World path.
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
