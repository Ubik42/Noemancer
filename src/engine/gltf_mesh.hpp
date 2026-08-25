#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace noemancer {

struct GltfDecodedVertex final {
    std::array<float, 3> position{};
    std::array<float, 3> normal{0.0F, 1.0F, 0.0F};
    std::array<float, 2> texcoord{};
    std::array<float, 4> tangent{1.0F, 0.0F, 0.0F, 1.0F};
    std::array<std::uint16_t, 4> joints{};
    std::array<float, 4> weights{1.0F, 0.0F, 0.0F, 0.0F};
};

struct GltfDecodedPrimitive final {
    std::uint32_t first_index{};
    std::uint32_t index_count{};
    std::array<float, 4> base_color{1.0F, 1.0F, 1.0F, 1.0F};
    float metallic{1.0F};
    float roughness{1.0F};
    bool unlit{};
    int base_color_image{-1};
    int normal_image{-1};
    int metallic_roughness_image{-1};
    int occlusion_image{-1};
    int emissive_image{-1};
    std::array<float, 3> emissive_factor{};
    float normal_scale{1.0F};
    float occlusion_strength{1.0F};
    float alpha_cutoff{0.5F};
    std::string alpha_mode{"OPAQUE"};
    bool double_sided{};
    std::string node_name;
    std::string mesh_name;
    int skin{-1};
    std::array<float,3> bounds_center{};
    float bounds_radius{};
};

struct GltfDecodedJoint final {
    std::string name;
    std::uint32_t node_index{};
    int parent_joint{-1};
    std::array<float, 16> local_transform{};
    std::array<float, 16> inverse_bind_matrix{};
};

struct GltfDecodedSkin final {
    std::string name;
    std::vector<GltfDecodedJoint> joints;
};

struct GltfAnimationChannel final {
    std::uint32_t node_index{};
    std::string path;
    std::string interpolation;
    std::vector<float> times;
    std::vector<std::array<float, 4>> values;
};

struct GltfAnimationClip final {
    std::string name;
    float duration{};
    std::vector<GltfAnimationChannel> channels;
};

struct GltfDecodedImage final {
    bool valid{};
    std::string code;
    std::string mime_type;
    std::uint32_t width{};
    std::uint32_t height{};
    std::vector<std::uint8_t> rgba8;
};

// A validated GLB container is the hand-off between file I/O and the glTF
// semantic importer. Keeping this boundary in engine-owned plain data lets a
// future fastgltf adapter consume the exact same bytes without coupling
// Scene/Asset contracts to a third-party parser type.
struct GltfBinaryContainer final {
    bool valid{};
    std::string code;
    std::string detail;
    std::uint32_t version{};
    std::uint64_t source_bytes{};
    std::vector<std::byte> storage;
    std::size_t json_offset{};
    std::size_t json_size{};
    std::size_t binary_offset{};
    std::size_t binary_size{};
    bool has_binary_chunk{};

    [[nodiscard]] std::span<const std::byte> json_chunk() const noexcept {
        if (json_size == 0U || json_offset > storage.size() || json_size > storage.size() - json_offset) return {};
        return {storage.data() + json_offset, json_size};
    }

    [[nodiscard]] std::span<const std::byte> binary_chunk() const noexcept {
        if (binary_size == 0U || binary_offset > storage.size() || binary_size > storage.size() - binary_offset) return {};
        return {storage.data() + binary_offset, binary_size};
    }
};

struct GltfExternalResourceSnapshot final {
    std::string uri;
    std::string normalized_relative_path;
    std::string kind;
    std::string content_hash;
    std::uint64_t source_bytes{};
    std::vector<std::byte> storage;
};

struct GltfSourceSnapshotLimits final {
    std::uint64_t maximum_document_bytes{64ULL * 1024ULL * 1024ULL};
    std::uint64_t maximum_dependency_bytes{512ULL * 1024ULL * 1024ULL};
    std::uint64_t maximum_total_bytes{1024ULL * 1024ULL * 1024ULL};
    std::size_t maximum_dependencies{4096U};
};

// External files are discovered before fastgltf runs, normalized beneath the
// source directory, bounded, hashed and copied into engine-owned storage.
// PNG and JPEG dependencies are decoded behind the engine-owned image adapter.
struct GltfSourceSnapshot final {
    bool valid{};
    std::string code;
    std::string detail;
    std::filesystem::path source_path;
    std::string content_hash;
    std::uint64_t source_bytes{};
    std::uint64_t total_bytes{};
    std::vector<std::byte> storage;
    std::vector<GltfExternalResourceSnapshot> dependencies;
};

struct GltfDependencyVerification final {
    bool unchanged{};
    std::string code;
    std::string detail;
    std::string normalized_relative_path;
};

struct GltfMeshData final {
    bool valid{};
    std::string code;
    std::string detail;
    std::vector<GltfDecodedVertex> vertices;
    std::vector<std::uint32_t> indices;
    std::vector<GltfDecodedPrimitive> primitives;
    std::vector<GltfDecodedImage> images;
    std::vector<GltfDecodedSkin> skins;
    std::vector<GltfAnimationClip> animations;
};

// Importers normalize their source formats into this payload. Keep the alias
// while the public API migrates away from its original glTF-specific name.
using DecodedSceneAsset = GltfMeshData;

[[nodiscard]] GltfBinaryContainer read_glb_container(const std::filesystem::path& path);
[[nodiscard]] GltfSourceSnapshot read_gltf_source_snapshot(
    const std::filesystem::path& path,
    const GltfSourceSnapshotLimits& limits = {});
[[nodiscard]] GltfDependencyVerification verify_gltf_source_snapshot(
    const GltfSourceSnapshot& snapshot,
    const GltfSourceSnapshotLimits& limits = {});
[[nodiscard]] std::string gltf_source_snapshot_fingerprint(const GltfSourceSnapshot& snapshot);
[[nodiscard]] GltfMeshData decode_gltf_mesh(const GltfSourceSnapshot& snapshot);
[[nodiscard]] GltfMeshData decode_gltf_mesh(const std::filesystem::path& path);
[[nodiscard]] GltfMeshData decode_glb_mesh(const std::filesystem::path& path);
void compute_decoded_scene_bounds(DecodedSceneAsset& asset);

} // namespace noemancer
