#pragma once

#include "engine/scene_document.hpp"

#include <string>
#include <string_view>

namespace noemancer {

inline constexpr std::string_view commercial_raster_reference_scene_id =
    "noemancer.commercial-raster-reference/1.8";
inline constexpr std::string_view commercial_raster_reference_scene_guid =
    "scene.commercial-raster-reference-v1-8";
inline constexpr std::string_view commercial_raster_reference_name =
    "Commercial Raster Reference v1.8";
inline constexpr std::string_view commercial_raster_reference_source_uri =
    "generated://scenes/commercial-raster-reference-v1-8.scene.json";

[[nodiscard]] SceneDocument make_commercial_raster_reference_scene_document();
[[nodiscard]] std::string commercial_raster_reference_contract_json();

} // namespace noemancer
