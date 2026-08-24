#pragma once

#include "engine/asset_cook_pipeline.hpp"
#include "engine/gltf_mesh.hpp"

#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace noemancer {

inline constexpr std::string_view mesh_runtime_artifact_schema =
    "noemancer.mesh-runtime-artifact/0.2";

struct MeshRuntimeArtifactCookResult final {
    bool success{};
    std::string code;
    std::string detail;
    std::string schema_version{std::string(mesh_runtime_artifact_schema)};
    std::string asset_id;
    std::string source_hash;
    std::string payload_hash;
    std::size_t lod_count{};
    std::size_t primitive_count{};
    std::size_t image_count{};
    std::vector<std::byte> payload;
};

struct MeshRuntimeArtifactLoadResult final {
    bool success{};
    std::string code;
    std::string detail;
    std::string schema_version{std::string(mesh_runtime_artifact_schema)};
    std::string asset_id;
    std::string source_hash;
    std::string payload_hash;
    std::size_t lod_count{};
    GltfMeshData mesh;
};

// Offline-only adapter. fastgltf has already produced engine-owned plain data;
// meshoptimizer and KTX-Software remain private execution details here.
[[nodiscard]] MeshRuntimeArtifactCookResult cook_mesh_runtime_artifact(
    const CookSource& source,
    const GltfMeshData& mesh,
    const CookPlatformProfile& profile,
    const MeshCookSettings& settings = {});

// Player-facing boundary. It accepts only the versioned cooked envelope and
// returns engine-owned runtime data; no source glTF parser participates.
[[nodiscard]] MeshRuntimeArtifactLoadResult load_mesh_runtime_artifact(
    std::span<const std::byte> payload,
    std::string_view expected_asset_id = {},
    std::string_view expected_source_hash = {},
    std::string_view expected_payload_hash = {});

} // namespace noemancer
