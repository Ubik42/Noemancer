#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace noemancer {

struct GltfBounds final {
    bool available{};
    double min_x{};
    double min_y{};
    double min_z{};
    double max_x{};
    double max_y{};
    double max_z{};
};

struct GltfSummary final {
    bool valid{};
    std::string code;
    std::string detail;
    std::uint32_t glb_version{};
    std::uint64_t source_bytes{};
    std::size_t scenes{};
    std::size_t nodes{};
    std::size_t meshes{};
    std::size_t primitives{};
    std::size_t materials{};
    std::size_t textures{};
    std::size_t images{};
    std::size_t skins{};
    std::size_t animations{};
    std::size_t cameras{};
    std::uint64_t vertices{};
    std::uint64_t indices{};
    GltfBounds position_bounds;
    std::vector<std::string> extensions_used;
    std::vector<std::string> mesh_names;
    std::vector<std::string> node_names;
};

[[nodiscard]] GltfSummary inspect_glb(const std::filesystem::path& path);
[[nodiscard]] std::string gltf_summary_json(const GltfSummary& summary);

} // namespace noemancer
