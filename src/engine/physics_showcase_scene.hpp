#pragma once

#include "engine/scene_document.hpp"

#include <string_view>

namespace noemancer {

inline constexpr std::string_view physics_showcase_scene_guid =
    "scene.physics-showcase";

// Build the deterministic, engine-neutral physics showcase scene used by
// editor/runtime smoke tests.  The document deliberately contains ordinary
// SceneDocument entities only: a caller can load it like any authored scene,
// inspect it through the semantic state surface, or serialize it as a fixture.
[[nodiscard]] SceneDocument make_physics_showcase_scene_document();

} // namespace noemancer
