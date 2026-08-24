#include "engine/gltf_inspector.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <fstream>
#include <limits>

namespace noemancer {
namespace {

using Json = nlohmann::json;

std::uint32_t read_u32_le(const std::array<unsigned char, 12>& header, const std::size_t offset) {
    return static_cast<std::uint32_t>(header[offset]) |
        (static_cast<std::uint32_t>(header[offset + 1]) << 8U) |
        (static_cast<std::uint32_t>(header[offset + 2]) << 16U) |
        (static_cast<std::uint32_t>(header[offset + 3]) << 24U);
}

std::uint32_t read_u32_le(const std::array<unsigned char, 8>& header, const std::size_t offset) {
    return static_cast<std::uint32_t>(header[offset]) |
        (static_cast<std::uint32_t>(header[offset + 1]) << 8U) |
        (static_cast<std::uint32_t>(header[offset + 2]) << 16U) |
        (static_cast<std::uint32_t>(header[offset + 3]) << 24U);
}

std::size_t array_size(const Json& document, const char* field) {
    return document.contains(field) && document.at(field).is_array() ? document.at(field).size() : 0;
}

std::vector<std::string> names(const Json& values) {
    std::vector<std::string> result;
    if (!values.is_array()) return result;
    result.reserve(values.size());
    for (std::size_t index = 0; index < values.size(); ++index) {
        result.push_back(values[index].value("name", "unnamed-" + std::to_string(index)));
    }
    return result;
}

} // namespace

GltfSummary inspect_glb(const std::filesystem::path& path) {
    GltfSummary result;
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        result.code = "gltf.source-unavailable";
        result.detail = "GLB source file could not be opened.";
        return result;
    }
    std::error_code size_error;
    result.source_bytes = std::filesystem::file_size(path, size_error);
    std::array<unsigned char, 12> header{};
    input.read(reinterpret_cast<char*>(header.data()), static_cast<std::streamsize>(header.size()));
    if (input.gcount() != static_cast<std::streamsize>(header.size()) ||
        read_u32_le(header, 0) != 0x46546c67U) {
        result.code = "gltf.invalid-header";
        result.detail = "File is not a binary glTF container.";
        return result;
    }
    result.glb_version = read_u32_le(header, 4);
    const auto declared_length = read_u32_le(header, 8);
    if (result.glb_version != 2U || declared_length != result.source_bytes) {
        result.code = "gltf.unsupported-container";
        result.detail = "Only complete GLB version 2 containers are supported.";
        return result;
    }
    std::array<unsigned char, 8> chunk_header{};
    input.read(reinterpret_cast<char*>(chunk_header.data()), static_cast<std::streamsize>(chunk_header.size()));
    if (input.gcount() != static_cast<std::streamsize>(chunk_header.size()) ||
        read_u32_le(chunk_header, 4) != 0x4e4f534aU) {
        result.code = "gltf.missing-json-chunk";
        result.detail = "First GLB chunk must contain JSON.";
        return result;
    }
    const auto json_length = read_u32_le(chunk_header, 0);
    if (json_length == 0U || static_cast<std::uint64_t>(json_length) + 20ULL > result.source_bytes) {
        result.code = "gltf.invalid-json-length";
        result.detail = "GLB JSON chunk length exceeds the container.";
        return result;
    }
    std::string json_text(json_length, '\0');
    input.read(json_text.data(), static_cast<std::streamsize>(json_text.size()));
    if (input.gcount() != static_cast<std::streamsize>(json_text.size())) {
        result.code = "gltf.truncated-json";
        result.detail = "GLB JSON chunk is truncated.";
        return result;
    }
    try {
        const auto document = Json::parse(json_text);
        result.scenes = array_size(document, "scenes");
        result.nodes = array_size(document, "nodes");
        result.meshes = array_size(document, "meshes");
        result.materials = array_size(document, "materials");
        result.textures = array_size(document, "textures");
        result.images = array_size(document, "images");
        result.skins = array_size(document, "skins");
        result.animations = array_size(document, "animations");
        result.cameras = array_size(document, "cameras");
        result.mesh_names = names(document.value("meshes", Json::array()));
        result.node_names = names(document.value("nodes", Json::array()));
        result.extensions_used = document.value("extensionsUsed", std::vector<std::string>{});

        const auto accessors = document.value("accessors", Json::array());
        double min_x = std::numeric_limits<double>::max();
        double min_y = std::numeric_limits<double>::max();
        double min_z = std::numeric_limits<double>::max();
        double max_x = std::numeric_limits<double>::lowest();
        double max_y = std::numeric_limits<double>::lowest();
        double max_z = std::numeric_limits<double>::lowest();
        bool has_bounds = false;
        for (const auto& mesh : document.value("meshes", Json::array())) {
            for (const auto& primitive : mesh.value("primitives", Json::array())) {
                ++result.primitives;
                if (primitive.contains("indices")) {
                    const auto accessor = primitive.at("indices").get<std::size_t>();
                    if (accessor < accessors.size()) result.indices += accessors[accessor].value("count", 0ULL);
                }
                const auto attributes = primitive.value("attributes", Json::object());
                if (!attributes.contains("POSITION")) continue;
                const auto accessor = attributes.at("POSITION").get<std::size_t>();
                if (accessor >= accessors.size()) continue;
                const auto& position = accessors[accessor];
                result.vertices += position.value("count", 0ULL);
                const auto minimum = position.value("min", Json::array());
                const auto maximum = position.value("max", Json::array());
                if (minimum.size() >= 3 && maximum.size() >= 3) {
                    min_x = std::min(min_x, minimum[0].get<double>());
                    min_y = std::min(min_y, minimum[1].get<double>());
                    min_z = std::min(min_z, minimum[2].get<double>());
                    max_x = std::max(max_x, maximum[0].get<double>());
                    max_y = std::max(max_y, maximum[1].get<double>());
                    max_z = std::max(max_z, maximum[2].get<double>());
                    has_bounds = true;
                }
            }
        }
        result.position_bounds = {has_bounds, min_x, min_y, min_z, max_x, max_y, max_z};
        result.valid = true;
        result.code = "ok";
        result.detail = "GLB metadata parsed without decoding binary vertex payloads.";
    } catch (const std::exception& error) {
        result.code = "gltf.invalid-json";
        result.detail = error.what();
    }
    return result;
}

std::string gltf_summary_json(const GltfSummary& summary) {
    Json bounds = nullptr;
    if (summary.position_bounds.available) {
        bounds = {
            {"min", {summary.position_bounds.min_x, summary.position_bounds.min_y, summary.position_bounds.min_z}},
            {"max", {summary.position_bounds.max_x, summary.position_bounds.max_y, summary.position_bounds.max_z}}
        };
    }
    return Json{
        {"schemaVersion", "0.1"},
        {"valid", summary.valid},
        {"code", summary.code},
        {"detail", summary.detail},
        {"container", {{"format", "glb"}, {"version", summary.glb_version}, {"sourceBytes", summary.source_bytes}}},
        {"counts", {
            {"scenes", summary.scenes}, {"nodes", summary.nodes}, {"meshes", summary.meshes},
            {"primitives", summary.primitives}, {"materials", summary.materials},
            {"textures", summary.textures}, {"images", summary.images}, {"skins", summary.skins},
            {"animations", summary.animations}, {"cameras", summary.cameras},
            {"vertices", summary.vertices}, {"indices", summary.indices}
        }},
        {"positionBounds", std::move(bounds)},
        {"extensionsUsed", summary.extensions_used},
        {"meshNames", summary.mesh_names},
        {"nodeNames", summary.node_names}
    }.dump();
}

} // namespace noemancer
