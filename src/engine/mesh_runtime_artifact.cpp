#include "engine/mesh_runtime_artifact.hpp"

#include "engine/content_hash.hpp"
#include "engine/ktx2_cook_adapter.hpp"

#include <meshoptimizer.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <optional>
#include <ranges>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>

namespace noemancer {
namespace {

using Json = nlohmann::json;

constexpr std::array<std::byte, 8> artifact_magic{
    std::byte{'N'}, std::byte{'M'}, std::byte{'M'}, std::byte{'E'},
    std::byte{'S'}, std::byte{'H'}, std::byte{'0'}, std::byte{'2'}};
constexpr std::array<std::byte, 8> geometry_magic{
    std::byte{'N'}, std::byte{'M'}, std::byte{'M'}, std::byte{'S'},
    std::byte{'H'}, std::byte{'0'}, std::byte{'0'}, std::byte{'1'}};
constexpr std::uint32_t artifact_version = 2U;
constexpr std::uint32_t artifact_endian = 0x01020304U;
constexpr std::size_t header_bytes = 48U;
constexpr std::size_t maximum_payload_bytes = 512U * 1024U * 1024U;
constexpr std::size_t maximum_manifest_bytes = 4U * 1024U * 1024U;
constexpr std::size_t maximum_vertices = 10U * 1024U * 1024U;
constexpr std::size_t maximum_indices = 30U * 1024U * 1024U;
constexpr std::size_t maximum_primitives = 65536U;
constexpr std::size_t maximum_images = 2048U;
constexpr std::size_t maximum_lods = 8U;
constexpr std::size_t maximum_text_bytes = 4096U;
constexpr std::uint32_t maximum_image_dimension = 32768U;
constexpr std::size_t maximum_decoded_image_bytes = 256U * 1024U * 1024U;

struct RuntimeVertex final {
    std::array<float, 3> position{};
    std::array<float, 3> normal{};
    std::array<float, 2> texcoord{};
    std::array<float, 4> tangent{};
    std::array<std::uint16_t, 4> joints{};
    std::array<float, 4> weights{};
};
static_assert(std::is_trivially_copyable_v<RuntimeVertex>);
static_assert(sizeof(RuntimeVertex) == 72U);

void append_u32(std::vector<std::byte>& output, const std::uint32_t value) {
    for (std::uint32_t shift = 0U; shift < 32U; shift += 8U)
        output.push_back(static_cast<std::byte>((value >> shift) & 0xffU));
}

void append_u64(std::vector<std::byte>& output, const std::uint64_t value) {
    for (std::uint32_t shift = 0U; shift < 64U; shift += 8U)
        output.push_back(static_cast<std::byte>((value >> shift) & 0xffU));
}

std::optional<std::uint32_t> read_u32(const std::span<const std::byte> input, const std::size_t offset) {
    if (offset > input.size() || input.size() - offset < 4U) return std::nullopt;
    std::uint32_t value{};
    for (std::uint32_t index = 0U; index < 4U; ++index)
        value |= static_cast<std::uint32_t>(input[offset + index]) << (index * 8U);
    return value;
}

std::optional<std::uint64_t> read_u64(const std::span<const std::byte> input, const std::size_t offset) {
    if (offset > input.size() || input.size() - offset < 8U) return std::nullopt;
    std::uint64_t value{};
    for (std::uint32_t index = 0U; index < 8U; ++index)
        value |= static_cast<std::uint64_t>(input[offset + index]) << (index * 8U);
    return value;
}

bool checked_add(std::size_t& total, const std::size_t value) {
    if (value > std::numeric_limits<std::size_t>::max() - total) return false;
    total += value;
    return true;
}

std::uint64_t fnv1a(const std::span<const std::byte> bytes) {
    std::uint64_t value = 14695981039346656037ULL;
    for (const auto byte : bytes) {
        value ^= std::to_integer<std::uint8_t>(byte);
        value *= 1099511628211ULL;
    }
    return value;
}

CookMeshInput mesh_input(const GltfMeshData& mesh, const GltfDecodedPrimitive& primitive) {
    std::vector<RuntimeVertex> vertices;
    vertices.reserve(std::min<std::size_t>(mesh.vertices.size(), primitive.index_count));
    std::unordered_map<std::uint32_t, std::uint32_t> remap;
    remap.reserve(vertices.capacity());
    CookMeshInput result;
    if (primitive.first_index <= mesh.indices.size() && primitive.index_count <=
        mesh.indices.size() - primitive.first_index) {
        result.indices.reserve(primitive.index_count);
        for (std::size_t offset = 0U; offset < primitive.index_count; ++offset) {
            const auto source_index = mesh.indices[primitive.first_index + offset];
            if (source_index >= mesh.vertices.size()) {
                result.indices.push_back(source_index);
                continue;
            }
            const auto [position, inserted] = remap.try_emplace(source_index,
                static_cast<std::uint32_t>(vertices.size()));
            if (inserted) {
                const auto& source = mesh.vertices[source_index];
                vertices.push_back({source.position, source.normal, source.texcoord, source.tangent,
                    source.joints, source.weights});
            }
            result.indices.push_back(position->second);
        }
    }
    const auto bytes = std::as_bytes(std::span(vertices));
    result.vertices.assign(bytes.begin(), bytes.end());
    result.vertex_stride = sizeof(RuntimeVertex);
    result.position_offset = offsetof(RuntimeVertex, position);
    result.vertex_layout = "position:float3@0;normal:float3@12;texcoord:float2@24;"
        "tangent:float4@32;joints:uint16x4@48;weights:float4@56";
    return result;
}

bool image_uses_srgb(const GltfMeshData& mesh, const std::size_t image) {
    return std::ranges::any_of(mesh.primitives, [image](const GltfDecodedPrimitive& primitive) {
        return primitive.base_color_image == static_cast<int>(image) ||
            primitive.emissive_image == static_cast<int>(image);
    });
}

bool image_is_referenced(const GltfMeshData& mesh, const std::size_t image) {
    return std::ranges::any_of(mesh.primitives, [image](const GltfDecodedPrimitive& primitive) {
        const auto value = static_cast<int>(image);
        return primitive.base_color_image == value || primitive.normal_image == value ||
            primitive.metallic_roughness_image == value || primitive.occlusion_image == value ||
            primitive.emissive_image == value;
    });
}

Json primitive_json(const GltfDecodedPrimitive& primitive, const std::uint32_t first_index,
                    const std::uint32_t index_count, const std::size_t geometry_offset,
                    const std::size_t geometry_bytes, const std::string& geometry_hash) {
    return {{"firstIndex", first_index}, {"indexCount", index_count},
        {"geometryOffset", geometry_offset}, {"geometryBytes", geometry_bytes},
        {"geometrySha256", geometry_hash},
        {"baseColor", primitive.base_color}, {"metallic", primitive.metallic},
        {"roughness", primitive.roughness}, {"unlit", primitive.unlit},
        {"baseColorImage", primitive.base_color_image}, {"normalImage", primitive.normal_image},
        {"metallicRoughnessImage", primitive.metallic_roughness_image},
        {"occlusionImage", primitive.occlusion_image}, {"emissiveImage", primitive.emissive_image},
        {"emissiveFactor", primitive.emissive_factor}, {"normalScale", primitive.normal_scale},
        {"occlusionStrength", primitive.occlusion_strength}, {"alphaCutoff", primitive.alpha_cutoff},
        {"alphaMode", primitive.alpha_mode}, {"doubleSided", primitive.double_sided},
        {"nodeName", primitive.node_name}, {"meshName", primitive.mesh_name}, {"skin", primitive.skin},
        {"boundsCenter", primitive.bounds_center}, {"boundsRadius", primitive.bounds_radius}};
}

bool finite_array(const Json& value, const std::size_t count) {
    if (!value.is_array() || value.size() != count) return false;
    for (const auto& item : value)
        if (!item.is_number() || !std::isfinite(item.get<float>())) return false;
    return true;
}

bool valid_image_index(const int value, const std::size_t image_count) {
    return value >= -1 && (value < 0 || static_cast<std::size_t>(value) < image_count);
}

bool ktx2_preflight(const std::span<const std::byte> bytes, const std::uint32_t expected_width,
                    const std::uint32_t expected_height) {
    constexpr std::array<std::byte, 12> identifier{std::byte{0xab}, std::byte{'K'}, std::byte{'T'},
        std::byte{'X'}, std::byte{' '}, std::byte{'2'}, std::byte{'0'}, std::byte{0xbb},
        std::byte{0x0d}, std::byte{0x0a}, std::byte{0x1a}, std::byte{0x0a}};
    if (bytes.size() < 80U || !std::equal(identifier.begin(), identifier.end(), bytes.begin())) return false;
    const auto width = read_u32(bytes, 20U);
    const auto height = read_u32(bytes, 24U);
    const auto depth = read_u32(bytes, 28U);
    const auto layers = read_u32(bytes, 32U);
    const auto faces = read_u32(bytes, 36U);
    const auto levels = read_u32(bytes, 40U);
    if (!width || !height || !depth || !layers || !faces || !levels || *width != expected_width ||
        *height != expected_height || *width == 0U || *height == 0U || *width > maximum_image_dimension ||
        *height > maximum_image_dimension || *depth != 0U || *layers != 0U || *faces != 1U ||
        *levels == 0U || *levels > 32U || *levels > (bytes.size() - 80U) / 24U) return false;
    const auto decoded_bytes = static_cast<std::uint64_t>(*width) * *height * 4U;
    if (decoded_bytes > maximum_decoded_image_bytes) return false;
    for (std::size_t level = 0U; level < *levels; ++level) {
        const auto offset = read_u64(bytes, 80U + level * 24U);
        const auto length = read_u64(bytes, 88U + level * 24U);
        const auto uncompressed = read_u64(bytes, 96U + level * 24U);
        if (!offset || !length || !uncompressed || *length == 0U ||
            *offset > bytes.size() || *length > bytes.size() - static_cast<std::size_t>(*offset)) return false;
    }
    return true;
}

bool parse_primitive(const Json& value, const std::size_t vertex_index_count,
                     const std::size_t image_count, GltfDecodedPrimitive& output) {
    if (!value.is_object() || !finite_array(value.value("baseColor", Json{}), 4U) ||
        !finite_array(value.value("emissiveFactor", Json{}), 3U) ||
        !finite_array(value.value("boundsCenter", Json{}), 3U)) return false;
    output.first_index = value.at("firstIndex").get<std::uint32_t>();
    output.index_count = value.at("indexCount").get<std::uint32_t>();
    if (output.index_count == 0U || output.index_count % 3U != 0U ||
        output.first_index > vertex_index_count || output.index_count > vertex_index_count - output.first_index)
        return false;
    output.base_color = value.at("baseColor").get<std::array<float, 4>>();
    output.metallic = value.at("metallic").get<float>();
    output.roughness = value.at("roughness").get<float>();
    output.unlit = value.at("unlit").get<bool>();
    output.base_color_image = value.at("baseColorImage").get<int>();
    output.normal_image = value.at("normalImage").get<int>();
    output.metallic_roughness_image = value.at("metallicRoughnessImage").get<int>();
    output.occlusion_image = value.at("occlusionImage").get<int>();
    output.emissive_image = value.at("emissiveImage").get<int>();
    if (!valid_image_index(output.base_color_image, image_count) ||
        !valid_image_index(output.normal_image, image_count) ||
        !valid_image_index(output.metallic_roughness_image, image_count) ||
        !valid_image_index(output.occlusion_image, image_count) ||
        !valid_image_index(output.emissive_image, image_count)) return false;
    output.emissive_factor = value.at("emissiveFactor").get<std::array<float, 3>>();
    output.normal_scale = value.at("normalScale").get<float>();
    output.occlusion_strength = value.at("occlusionStrength").get<float>();
    output.alpha_cutoff = value.at("alphaCutoff").get<float>();
    output.alpha_mode = value.at("alphaMode").get<std::string>();
    output.double_sided = value.at("doubleSided").get<bool>();
    output.node_name = value.at("nodeName").get<std::string>();
    output.mesh_name = value.at("meshName").get<std::string>();
    output.skin = value.at("skin").get<int>();
    output.bounds_center = value.at("boundsCenter").get<std::array<float, 3>>();
    output.bounds_radius = value.at("boundsRadius").get<float>();
    return std::isfinite(output.metallic) && std::isfinite(output.roughness) &&
        std::isfinite(output.normal_scale) && std::isfinite(output.occlusion_strength) &&
        std::isfinite(output.alpha_cutoff) && std::isfinite(output.bounds_radius) && output.bounds_radius >= 0.0F &&
        (output.alpha_mode == "OPAQUE" || output.alpha_mode == "MASK" || output.alpha_mode == "BLEND") &&
        output.node_name.size() <= maximum_text_bytes && output.mesh_name.size() <= maximum_text_bytes;
}

struct DecodedGeometry final {
    bool valid{};
    std::string code;
    std::size_t lod_count{};
    std::vector<GltfDecodedVertex> vertices;
    std::vector<std::uint32_t> indices;
};

DecodedGeometry decode_geometry(const std::span<const std::byte> bytes) {
    DecodedGeometry result;
    if (bytes.size() < 40U || !std::equal(geometry_magic.begin(), geometry_magic.end(), bytes.begin())) {
        result.code = "mesh.artifact-geometry-header-invalid";
        return result;
    }
    const auto version = read_u32(bytes, 8U);
    const auto stride = read_u32(bytes, 12U);
    const auto source_vertices = read_u32(bytes, 16U);
    const auto source_indices = read_u32(bytes, 20U);
    const auto lod_count = read_u32(bytes, 24U);
    if (!version || !stride || !source_vertices || !source_indices || !lod_count || *version != 1U ||
        *stride != sizeof(RuntimeVertex) || *source_vertices == 0U || *source_vertices > maximum_vertices ||
        *source_indices == 0U || *source_indices > maximum_indices || *source_indices % 3U != 0U ||
        *lod_count == 0U || *lod_count > maximum_lods) {
        result.code = "mesh.artifact-geometry-range-invalid";
        return result;
    }
    std::size_t cursor = 28U;
    std::span<const std::byte> first_vertices;
    std::span<const std::byte> first_indices;
    std::uint32_t first_vertex_count{};
    std::uint32_t first_index_count{};
    for (std::size_t lod = 0U; lod < *lod_count; ++lod) {
        if (cursor > bytes.size() || bytes.size() - cursor < 32U) {
            result.code = "mesh.artifact-geometry-truncated";
            return result;
        }
        const auto ratio_bits = read_u32(bytes, cursor);
        const auto error_bits = read_u32(bytes, cursor + 4U);
        const auto vertex_count = read_u32(bytes, cursor + 8U);
        const auto index_count = read_u32(bytes, cursor + 12U);
        const auto encoded_vertex_bytes = read_u64(bytes, cursor + 16U);
        if (!ratio_bits || !error_bits || !vertex_count || !index_count || !encoded_vertex_bytes ||
            !std::isfinite(std::bit_cast<float>(*ratio_bits)) || !std::isfinite(std::bit_cast<float>(*error_bits)) ||
            *vertex_count == 0U || *vertex_count > maximum_vertices || *index_count == 0U ||
            *index_count > maximum_indices || *index_count % 3U != 0U) {
            result.code = "mesh.artifact-geometry-range-invalid";
            return result;
        }
        cursor += 24U;
        if (*encoded_vertex_bytes > bytes.size() - cursor) {
            result.code = "mesh.artifact-geometry-truncated";
            return result;
        }
        const auto encoded_vertices = bytes.subspan(cursor, static_cast<std::size_t>(*encoded_vertex_bytes));
        cursor += static_cast<std::size_t>(*encoded_vertex_bytes);
        const auto encoded_index_bytes = read_u64(bytes, cursor);
        if (!encoded_index_bytes) {
            result.code = "mesh.artifact-geometry-truncated";
            return result;
        }
        cursor += 8U;
        if (*encoded_index_bytes > bytes.size() - cursor) {
            result.code = "mesh.artifact-geometry-truncated";
            return result;
        }
        const auto encoded_indices = bytes.subspan(cursor, static_cast<std::size_t>(*encoded_index_bytes));
        cursor += static_cast<std::size_t>(*encoded_index_bytes);
        if (lod == 0U) {
            first_vertices = encoded_vertices;
            first_indices = encoded_indices;
            first_vertex_count = *vertex_count;
            first_index_count = *index_count;
        }
    }
    if (cursor > bytes.size() || bytes.size() - cursor != sizeof(std::uint64_t)) {
        result.code = "mesh.artifact-geometry-size-invalid";
        return result;
    }
    const auto stored_hash = read_u64(bytes, cursor);
    if (!stored_hash || *stored_hash != fnv1a(bytes.first(cursor))) {
        result.code = "mesh.artifact-geometry-hash-mismatch";
        return result;
    }
    std::vector<RuntimeVertex> runtime_vertices(first_vertex_count);
    std::vector<std::uint32_t> indices(first_index_count);
    if (meshopt_decodeVertexBuffer(runtime_vertices.data(), runtime_vertices.size(), sizeof(RuntimeVertex),
            reinterpret_cast<const unsigned char*>(first_vertices.data()), first_vertices.size()) != 0 ||
        meshopt_decodeIndexBuffer(indices.data(), indices.size(), sizeof(std::uint32_t),
            reinterpret_cast<const unsigned char*>(first_indices.data()), first_indices.size()) != 0) {
        result.code = "mesh.artifact-geometry-decode-failed";
        return result;
    }
    if (std::ranges::any_of(indices, [first_vertex_count](const std::uint32_t index) {
            return index >= first_vertex_count;
        })) {
        result.code = "mesh.artifact-index-range-invalid";
        return result;
    }
    result.vertices.reserve(runtime_vertices.size());
    for (const auto& vertex : runtime_vertices) {
        const auto finite = [](const auto& values) {
            return std::ranges::all_of(values, [](const float value) { return std::isfinite(value); });
        };
        if (!finite(vertex.position) || !finite(vertex.normal) || !finite(vertex.texcoord) ||
            !finite(vertex.tangent) || !finite(vertex.weights)) {
            result.code = "mesh.artifact-vertex-non-finite";
            return result;
        }
        const auto weight_sum = std::accumulate(vertex.weights.begin(), vertex.weights.end(), 0.0F);
        if (std::ranges::any_of(vertex.weights, [](const float weight) { return weight < 0.0F; }) ||
            weight_sum <= 0.0F || std::ranges::any_of(vertex.joints,
                [](const std::uint16_t joint) { return joint >= 64U; })) {
            result.code = "mesh.artifact-skinning-invalid";
            return result;
        }
        result.vertices.push_back({vertex.position, vertex.normal, vertex.texcoord, vertex.tangent,
            vertex.joints, vertex.weights});
    }
    result.indices = std::move(indices);
    result.lod_count = *lod_count;
    result.valid = true;
    result.code = "ok";
    return result;
}

} // namespace

MeshRuntimeArtifactCookResult cook_mesh_runtime_artifact(
    const CookSource& source, const GltfMeshData& mesh, const CookPlatformProfile& profile,
    const MeshCookSettings& settings) {
    MeshRuntimeArtifactCookResult result;
    result.asset_id = source.asset_id;
    result.source_hash = source.source_hash;
    if (!mesh.valid || source.asset_id.empty() || !source.source_hash.starts_with("sha256:") ||
        mesh.vertices.empty() || mesh.vertices.size() > maximum_vertices || mesh.indices.empty() ||
        mesh.indices.size() > maximum_indices || mesh.primitives.empty() ||
        mesh.primitives.size() > maximum_primitives || mesh.images.size() > maximum_images) {
        result.code = "mesh.artifact-input-invalid";
        result.detail = "Decoded mesh data exceeds the cooked runtime artifact contract.";
        return result;
    }
    std::vector<std::byte> geometry_bytes;
    Json primitives = Json::array();
    std::uint32_t first_index{};
    std::size_t common_lod_count{};
    for (const auto& primitive : mesh.primitives) {
        if (primitive.index_count == 0U || primitive.index_count % 3U != 0U ||
            primitive.first_index > mesh.indices.size() ||
            primitive.index_count > mesh.indices.size() - primitive.first_index) {
            result.code = "mesh.artifact-primitive-invalid";
            result.detail = "A source primitive does not identify a bounded triangle range.";
            return result;
        }
        const auto geometry = execute_mesh_cook(source, mesh_input(mesh, primitive), profile, settings);
        if (!geometry.valid || geometry.payload.empty() || geometry.lods.empty()) {
            result.code = geometry.code.empty() ? "mesh.artifact-geometry-cook-failed" : geometry.code;
            result.detail = geometry.detail;
            return result;
        }
        if (common_lod_count == 0U) common_lod_count = geometry.lods.size();
        if (geometry.lods.size() != common_lod_count ||
            geometry.lods.front().index_count > std::numeric_limits<std::uint32_t>::max() - first_index) {
            result.code = "mesh.artifact-geometry-layout-invalid";
            result.detail = "Per-primitive cooked geometry has an inconsistent LOD or index layout.";
            return result;
        }
        const auto geometry_hash = sha256_bytes(geometry.payload);
        if (!geometry_hash.success) {
            result.code = geometry_hash.code;
            result.detail = geometry_hash.detail;
            return result;
        }
        if (geometry_bytes.size() > maximum_payload_bytes ||
            geometry.payload.size() > maximum_payload_bytes - geometry_bytes.size()) {
            result.code = "mesh.artifact-too-large";
            result.detail = "Per-primitive geometry exceeds the Runtime artifact byte budget.";
            return result;
        }
        const auto offset = geometry_bytes.size();
        geometry_bytes.insert(geometry_bytes.end(), geometry.payload.begin(), geometry.payload.end());
        primitives.push_back(primitive_json(primitive, first_index, geometry.lods.front().index_count,
            offset, geometry.payload.size(), geometry_hash.value));
        first_index += geometry.lods.front().index_count;
    }

    std::vector<std::byte> texture_bytes;
    Json images = Json::array();
    for (std::size_t index = 0U; index < mesh.images.size(); ++index) {
        const auto& image = mesh.images[index];
        const auto referenced = image_is_referenced(mesh, index);
        if (!image.valid) {
            if (referenced) {
                result.code = "mesh.artifact-referenced-image-invalid";
                result.detail = "A material references an image that did not decode during Cook.";
                return result;
            }
            images.push_back({{"imageIndex", index}, {"available", false}, {"offset", texture_bytes.size()},
                {"bytes", 0U}, {"width", 0U}, {"height", 0U}, {"sha256", ""}});
            continue;
        }
        const auto decoded_image_bytes = static_cast<std::uint64_t>(image.width) * image.height * 4U;
        if (image.width == 0U || image.height == 0U || image.width > maximum_image_dimension ||
            image.height > maximum_image_dimension || decoded_image_bytes > maximum_decoded_image_bytes ||
            image.rgba8.size() != decoded_image_bytes) {
            result.code = "mesh.artifact-image-shape-invalid";
            result.detail = "A decoded material image has an invalid RGBA8 shape.";
            return result;
        }
        TextureCookInput input{.width = image.width, .height = image.height};
        input.rgba8.assign(reinterpret_cast<const std::byte*>(image.rgba8.data()),
            reinterpret_cast<const std::byte*>(image.rgba8.data() + image.rgba8.size()));
        const bool srgb = image_uses_srgb(mesh, index);
        TextureCookSettings texture_settings;
        texture_settings.semantic = srgb ? TextureSemantic::base_color : TextureSemantic::data;
        texture_settings.alpha_mode = TextureAlphaMode::blend;
        texture_settings.srgb = srgb;
        texture_settings.generate_mipmaps = true;
        texture_settings.streaming = true;
        const auto texture = execute_texture_cook(source, input, profile, texture_settings,
            srgb ? TextureCookCompression::basis_lz : TextureCookCompression::uastc);
        if (!texture.valid || texture.payload.empty()) {
            result.code = texture.code.empty() ? "mesh.artifact-texture-cook-failed" : texture.code;
            result.detail = texture.detail;
            return result;
        }
        const auto hash = sha256_bytes(texture.payload);
        if (!hash.success) {
            result.code = hash.code;
            result.detail = hash.detail;
            return result;
        }
        const auto offset = texture_bytes.size();
        if (texture_bytes.size() > maximum_payload_bytes ||
            texture.payload.size() > maximum_payload_bytes - texture_bytes.size()) {
            result.code = "mesh.artifact-too-large";
            result.detail = "Embedded KTX2 images exceed the Runtime artifact byte budget.";
            return result;
        }
        texture_bytes.insert(texture_bytes.end(), texture.payload.begin(), texture.payload.end());
        images.push_back({{"imageIndex", index}, {"available", true}, {"offset", offset},
            {"bytes", texture.payload.size()}, {"width", texture.width}, {"height", texture.height},
            {"levelCount", texture.level_count}, {"srgb", srgb}, {"sha256", hash.value}});
    }

    const auto geometry_hash = sha256_bytes(geometry_bytes);
    const auto texture_hash = sha256_bytes(texture_bytes);
    if (!geometry_hash.success || !texture_hash.success) {
        result.code = "mesh.artifact-section-hash-failed";
        result.detail = "A cooked mesh section could not be hashed.";
        return result;
    }
    const Json manifest{{"schema", result.schema_version}, {"assetId", result.asset_id},
        {"sourceHash", result.source_hash}, {"backend", "meshoptimizer+KTX-Software"},
        {"endianness", "little"}, {"vertexLayout", "noemancer.pbr-skinned/0.1"},
        {"lodCount", common_lod_count}, {"primitiveCount", primitives.size()},
        {"imageCount", images.size()}, {"primitives", std::move(primitives)}, {"images", std::move(images)},
        {"sections", {{"geometry", {{"format", "meshopt/NMMSH001"},
            {"bytes", geometry_bytes.size()}, {"sha256", geometry_hash.value},
            {"partitioning", "per-primitive"}}},
            {"textures", {{"format", "concatenated-ktx2"}, {"bytes", texture_bytes.size()},
                {"sha256", texture_hash.value}}}}},
        {"runtimeContract", {{"sourceDecode", false}, {"materialTable", true},
            {"embeddedTextures", "ktx2"}, {"selectedLod", 0}}}};
    const auto manifest_text = manifest.dump();
    if (manifest_text.empty() || manifest_text.size() > maximum_manifest_bytes ||
        manifest_text.size() > std::numeric_limits<std::uint32_t>::max()) {
        result.code = "mesh.artifact-manifest-too-large";
        result.detail = "Cooked mesh manifest exceeds its byte budget.";
        return result;
    }
    result.payload.reserve(header_bytes + manifest_text.size() + geometry_bytes.size() + texture_bytes.size());
    result.payload.insert(result.payload.end(), artifact_magic.begin(), artifact_magic.end());
    append_u32(result.payload, artifact_version);
    append_u32(result.payload, artifact_endian);
    append_u32(result.payload, static_cast<std::uint32_t>(manifest_text.size()));
    append_u32(result.payload, static_cast<std::uint32_t>(common_lod_count));
    append_u32(result.payload, static_cast<std::uint32_t>(mesh.primitives.size()));
    append_u32(result.payload, static_cast<std::uint32_t>(mesh.images.size()));
    append_u64(result.payload, geometry_bytes.size());
    append_u64(result.payload, texture_bytes.size());
    result.payload.insert(result.payload.end(), reinterpret_cast<const std::byte*>(manifest_text.data()),
        reinterpret_cast<const std::byte*>(manifest_text.data() + manifest_text.size()));
    result.payload.insert(result.payload.end(), geometry_bytes.begin(), geometry_bytes.end());
    result.payload.insert(result.payload.end(), texture_bytes.begin(), texture_bytes.end());
    if (result.payload.size() > maximum_payload_bytes) {
        result.payload.clear();
        result.code = "mesh.artifact-too-large";
        result.detail = "Cooked mesh payload exceeds the Runtime byte budget.";
        return result;
    }
    const auto payload_hash = sha256_bytes(result.payload);
    if (!payload_hash.success) {
        result.payload.clear();
        result.code = payload_hash.code;
        result.detail = payload_hash.detail;
        return result;
    }
    result.payload_hash = payload_hash.value;
    result.lod_count = common_lod_count;
    result.primitive_count = mesh.primitives.size();
    result.image_count = mesh.images.size();
    result.success = true;
    result.code = "ok";
    result.detail = "Mesh geometry, material primitives and KTX2 images were cooked into one runtime envelope.";
    return result;
}

MeshRuntimeArtifactLoadResult load_mesh_runtime_artifact(
    const std::span<const std::byte> payload, const std::string_view expected_asset_id,
    const std::string_view expected_source_hash, const std::string_view expected_payload_hash) {
    MeshRuntimeArtifactLoadResult result;
    result.code = "mesh.artifact-header-invalid";
    try {
        if (payload.size() < header_bytes || payload.size() > maximum_payload_bytes ||
            !std::equal(artifact_magic.begin(), artifact_magic.end(), payload.begin())) {
            result.detail = "Cooked mesh magic or total byte count is invalid.";
            return result;
        }
        const auto version = read_u32(payload, 8U);
        const auto endian = read_u32(payload, 12U);
        const auto manifest_bytes = read_u32(payload, 16U);
        const auto lod_count = read_u32(payload, 20U);
        const auto primitive_count = read_u32(payload, 24U);
        const auto image_count = read_u32(payload, 28U);
        const auto geometry_bytes = read_u64(payload, 32U);
        const auto texture_bytes = read_u64(payload, 40U);
        if (!version || !endian || !manifest_bytes || !lod_count || !primitive_count || !image_count ||
            !geometry_bytes || !texture_bytes || *version != artifact_version || *endian != artifact_endian ||
            *manifest_bytes == 0U || *manifest_bytes > maximum_manifest_bytes || *lod_count == 0U ||
            *lod_count > maximum_lods || *primitive_count == 0U || *primitive_count > maximum_primitives ||
            *image_count > maximum_images || *geometry_bytes == 0U || *geometry_bytes > maximum_payload_bytes ||
            *texture_bytes > maximum_payload_bytes) {
            result.code = "mesh.artifact-range-invalid";
            result.detail = "Cooked mesh section counts exceed the Runtime contract.";
            return result;
        }
        std::size_t exact_size = header_bytes;
        if (!checked_add(exact_size, *manifest_bytes) || !checked_add(exact_size, *geometry_bytes) ||
            !checked_add(exact_size, *texture_bytes) || exact_size != payload.size()) {
            result.code = "mesh.artifact-range-invalid";
            result.detail = "Cooked mesh sections do not exactly cover the payload.";
            return result;
        }
        const auto payload_identity = sha256_bytes(payload);
        if (!payload_identity.success) {
            result.code = payload_identity.code;
            result.detail = payload_identity.detail;
            return result;
        }
        result.payload_hash = payload_identity.value;
        if (!expected_payload_hash.empty() && expected_payload_hash != result.payload_hash) {
            result.code = "mesh.artifact-hash-mismatch";
            result.detail = "Cooked mesh payload hash does not match the Registry identity.";
            return result;
        }
        const auto manifest_begin = header_bytes;
        const auto geometry_begin = manifest_begin + *manifest_bytes;
        const auto texture_begin = geometry_begin + static_cast<std::size_t>(*geometry_bytes);
        const auto manifest = Json::parse(std::string(reinterpret_cast<const char*>(payload.data() + manifest_begin),
            *manifest_bytes), nullptr, false);
        if (!manifest.is_object() || manifest.value("schema", std::string{}) != result.schema_version ||
            manifest.value("backend", std::string{}) != "meshoptimizer+KTX-Software" ||
            manifest.value("endianness", std::string{}) != "little" ||
            manifest.value("vertexLayout", std::string{}) != "noemancer.pbr-skinned/0.1") {
            result.code = "mesh.artifact-manifest-invalid";
            result.detail = "Cooked mesh manifest identity or backend is invalid.";
            return result;
        }
        result.asset_id = manifest.value("assetId", std::string{});
        result.source_hash = manifest.value("sourceHash", std::string{});
        if (result.asset_id.empty() || result.source_hash.size() != 71U ||
            !result.source_hash.starts_with("sha256:") || manifest.value("lodCount", 0U) != *lod_count ||
            manifest.value("primitiveCount", 0U) != *primitive_count ||
            manifest.value("imageCount", 0U) != *image_count ||
            (!expected_asset_id.empty() && expected_asset_id != result.asset_id) ||
            (!expected_source_hash.empty() && expected_source_hash != result.source_hash)) {
            result.code = "mesh.artifact-identity-mismatch";
            result.detail = "Cooked mesh asset/source identity or declared counts are invalid.";
            return result;
        }
        if (!manifest.contains("sections") || !manifest.at("sections").is_object() ||
            !manifest.contains("primitives") || !manifest.at("primitives").is_array() ||
            !manifest.contains("images") || !manifest.at("images").is_array() ||
            manifest.at("primitives").size() != *primitive_count || manifest.at("images").size() != *image_count) {
            result.code = "mesh.artifact-manifest-invalid";
            result.detail = "Cooked mesh material or image tables are invalid.";
            return result;
        }
        const auto geometry = payload.subspan(geometry_begin, static_cast<std::size_t>(*geometry_bytes));
        const auto textures = payload.subspan(texture_begin, static_cast<std::size_t>(*texture_bytes));
        const auto section_matches = [&](const char* name, const std::span<const std::byte> bytes) {
            const auto& sections = manifest.at("sections");
            if (!sections.contains(name) || !sections.at(name).is_object()) return false;
            const auto hash = sha256_bytes(bytes);
            return hash.success && sections.at(name).value("bytes", std::size_t{}) == bytes.size() &&
                sections.at(name).value("sha256", std::string{}) == hash.value;
        };
        if (!section_matches("geometry", geometry) || !section_matches("textures", textures)) {
            result.code = "mesh.artifact-section-hash-mismatch";
            result.detail = "A cooked mesh section failed its integrity contract.";
            return result;
        }
        result.mesh.primitives.reserve(*primitive_count);
        std::size_t expected_geometry_offset{};
        for (const auto& value : manifest.at("primitives")) {
            if (!value.is_object() ||
                value.value("geometryOffset", std::size_t(-1)) != expected_geometry_offset) {
                result.code = "mesh.artifact-geometry-table-invalid";
                result.detail = "Cooked primitive geometry offsets are not canonical and contiguous.";
                return result;
            }
            const auto bytes = value.value("geometryBytes", std::size_t{});
            if (expected_geometry_offset > geometry.size() || bytes == 0U ||
                bytes > geometry.size() - expected_geometry_offset) {
                result.code = "mesh.artifact-geometry-table-invalid";
                result.detail = "A cooked primitive geometry range exceeds the geometry section.";
                return result;
            }
            const auto encoded = geometry.subspan(expected_geometry_offset, bytes);
            const auto encoded_hash = sha256_bytes(encoded);
            if (!encoded_hash.success || value.value("geometrySha256", std::string{}) != encoded_hash.value) {
                result.code = "mesh.artifact-geometry-hash-mismatch";
                result.detail = "A cooked primitive geometry range failed its identity contract.";
                return result;
            }
            auto decoded_geometry = decode_geometry(encoded);
            if (!decoded_geometry.valid || decoded_geometry.lod_count != *lod_count ||
                decoded_geometry.vertices.size() > maximum_vertices - result.mesh.vertices.size() ||
                decoded_geometry.indices.size() > maximum_indices - result.mesh.indices.size() ||
                result.mesh.vertices.size() > std::numeric_limits<std::uint32_t>::max()) {
                result.code = decoded_geometry.valid ? "mesh.artifact-geometry-count-limit" : decoded_geometry.code;
                result.detail = "Cooked per-primitive geometry could not be decoded safely.";
                return result;
            }
            const auto base_vertex = static_cast<std::uint32_t>(result.mesh.vertices.size());
            const auto first_index = result.mesh.indices.size();
            result.mesh.vertices.insert(result.mesh.vertices.end(), decoded_geometry.vertices.begin(),
                decoded_geometry.vertices.end());
            for (const auto index : decoded_geometry.indices) {
                if (index > std::numeric_limits<std::uint32_t>::max() - base_vertex) {
                    result.code = "mesh.artifact-index-range-invalid";
                    result.detail = "A cooked primitive index overflows the combined mesh address space.";
                    return result;
                }
                result.mesh.indices.push_back(index + base_vertex);
            }
            GltfDecodedPrimitive primitive;
            if (!parse_primitive(value, result.mesh.indices.size(), *image_count, primitive)) {
                result.code = "mesh.artifact-primitive-invalid";
                result.detail = "A cooked material primitive is invalid or out of range.";
                return result;
            }
            if (primitive.first_index != first_index || primitive.index_count != decoded_geometry.indices.size()) {
                result.code = "mesh.artifact-primitive-geometry-mismatch";
                result.detail = "A material primitive does not exactly identify its decoded geometry range.";
                return result;
            }
            result.mesh.primitives.push_back(std::move(primitive));
            expected_geometry_offset += bytes;
        }
        if (expected_geometry_offset != geometry.size()) {
            result.code = "mesh.artifact-geometry-table-invalid";
            result.detail = "Cooked primitive records do not exactly consume the geometry section.";
            return result;
        }
        result.mesh.images.resize(*image_count);
        std::size_t expected_offset{};
        for (std::size_t index = 0U; index < *image_count; ++index) {
            const auto& value = manifest.at("images").at(index);
            if (!value.is_object() || value.value("imageIndex", std::size_t(-1)) != index ||
                value.value("offset", std::size_t(-1)) != expected_offset) {
                result.code = "mesh.artifact-image-table-invalid";
                result.detail = "Cooked image section offsets are not canonical and contiguous.";
                return result;
            }
            const auto available = value.value("available", false);
            const auto bytes = value.value("bytes", std::size_t{});
            if (expected_offset > textures.size() || bytes > textures.size() - expected_offset) {
                result.code = "mesh.artifact-image-table-invalid";
                result.detail = "A cooked image section exceeds the texture payload.";
                return result;
            }
            auto& image = result.mesh.images[index];
            if (available) {
                if (bytes == 0U) {
                    result.code = "mesh.artifact-image-table-invalid";
                    result.detail = "An available cooked image has no KTX2 payload.";
                    return result;
                }
                const auto encoded = textures.subspan(expected_offset, bytes);
                const auto hash = sha256_bytes(encoded);
                const auto declared_width = value.value("width", 0U);
                const auto declared_height = value.value("height", 0U);
                if (!hash.success || value.value("sha256", std::string{}) != hash.value ||
                    !ktx2_preflight(encoded, declared_width, declared_height)) {
                    result.code = "mesh.artifact-image-invalid";
                    result.detail = "A cooked KTX2 image failed identity or bounded header validation.";
                    return result;
                }
                const auto decoded = decode_ktx2_rgba8(encoded);
                if (!decoded.valid ||
                    decoded.width != value.value("width", 0U) || decoded.height != value.value("height", 0U)) {
                    result.code = "mesh.artifact-image-invalid";
                    result.detail = "A cooked KTX2 image failed identity or decode validation.";
                    return result;
                }
                image.valid = true;
                image.code = "ok";
                image.mime_type = "image/ktx2-cooked";
                image.width = decoded.width;
                image.height = decoded.height;
                image.rgba8.assign(reinterpret_cast<const std::uint8_t*>(decoded.rgba8.data()),
                    reinterpret_cast<const std::uint8_t*>(decoded.rgba8.data() + decoded.rgba8.size()));
            } else if (bytes != 0U) {
                result.code = "mesh.artifact-image-table-invalid";
                result.detail = "An unavailable cooked image unexpectedly owns payload bytes.";
                return result;
            }
            expected_offset += bytes;
        }
        if (expected_offset != textures.size()) {
            result.code = "mesh.artifact-image-table-invalid";
            result.detail = "Cooked image records do not exactly consume the texture section.";
            return result;
        }
        result.mesh.valid = true;
        result.mesh.code = "ok";
        result.mesh.detail = "Loaded from noemancer.mesh-runtime-artifact/0.2 without source decode.";
        result.lod_count = *lod_count;
        result.success = true;
        result.code = "ok";
        result.detail = "Cooked mesh geometry, material table and KTX2 images loaded successfully.";
        return result;
    } catch (const std::exception&) {
        result.code = "mesh.artifact-manifest-invalid";
        result.detail = "Cooked mesh manifest types or values are invalid.";
        return result;
    }
}

} // namespace noemancer
