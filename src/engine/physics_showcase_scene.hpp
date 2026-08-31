#pragma once

#include "engine/physics_constraints.hpp"
#include "engine/scene_document.hpp"

#include <string_view>
#include <vector>

namespace noemancer {

inline constexpr std::string_view physics_showcase_scene_guid =
    "scene.physics-showcase";

// Build the deterministic, engine-neutral physics showcase scene used by
// editor/runtime smoke tests.  The document deliberately contains ordinary
// SceneDocument entities only: a caller can load it like any authored scene,
// inspect it through the semantic state surface, or serialize it as a fixture.
[[nodiscard]] SceneDocument make_physics_showcase_scene_document();

// Stable, engine-owned fixture records for the constraint gallery embedded in
// the ordinary showcase scene.  Keeping the records available independently
// is useful to headless probes and lets SceneDocument adopt the constraints
// without duplicating the authoring data when its persistence field is present.
[[nodiscard]] std::vector<PhysicsConstraintSpec>
make_physics_showcase_constraints();

} // namespace noemancer
