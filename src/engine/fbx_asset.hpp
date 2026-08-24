#pragma once

#include "engine/gltf_mesh.hpp"

#include <filesystem>

namespace noemancer {

[[nodiscard]] DecodedSceneAsset decode_fbx_asset(const std::filesystem::path& path);

} // namespace noemancer
