#include "engine/gltf_mesh.hpp"
#include "engine/content_hash.hpp"
#include "engine/image_decoder.hpp"

#if __has_include(<fastgltf/core.hpp>) && __has_include(<fastgltf/tools.hpp>)
#define NOEMANCER_HAS_FASTGLTF 1
#include <fastgltf/core.hpp>
#include <fastgltf/tools.hpp>
#else
#define NOEMANCER_HAS_FASTGLTF 0
#endif

#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstring>
#include <fstream>
#include <functional>
#include <limits>
#include <optional>
#include <span>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>

namespace noemancer {
namespace {

using Json = nlohmann::json;

struct Mat4 final { std::array<float, 16> value{}; };

struct AccessorView final {
    const std::byte* data{};
    std::size_t count{};
    std::size_t stride{};
    std::uint32_t component_type{};
    std::size_t components{};
    bool normalized{};
};

struct BoundedFileRead final {
    bool valid{};
    std::string code;
    std::string detail;
    std::vector<std::byte> storage;
};

BoundedFileRead read_bounded_file(const std::filesystem::path& path,
                                  const std::uint64_t maximum_bytes) {
    BoundedFileRead result;
    std::error_code error;
    const auto status=std::filesystem::status(path,error);
    if(error||!std::filesystem::is_regular_file(status)){
        result.code="gltf.dependency-unavailable";
        result.detail="Source or dependency is not an available regular file.";
        return result;
    }
    const auto size=std::filesystem::file_size(path,error);
    if(error){
        result.code="gltf.dependency-stat-failed";
        result.detail="Source or dependency size could not be read.";
        return result;
    }
    if(size>maximum_bytes||size>std::numeric_limits<std::size_t>::max()||
       size>static_cast<std::uint64_t>(std::numeric_limits<std::streamsize>::max())){
        result.code="gltf.dependency-budget-exceeded";
        result.detail="Source or dependency exceeds its immutable snapshot byte budget.";
        return result;
    }
    std::ifstream input(path,std::ios::binary);
    if(!input){
        result.code="gltf.dependency-unavailable";
        result.detail="Source or dependency could not be opened.";
        return result;
    }
    result.storage.resize(static_cast<std::size_t>(size));
    if(!result.storage.empty())input.read(reinterpret_cast<char*>(result.storage.data()),
        static_cast<std::streamsize>(result.storage.size()));
    if(!input||static_cast<std::size_t>(input.gcount())!=result.storage.size()){
        result.storage.clear();result.code="gltf.dependency-read-failed";
        result.detail="Source or dependency changed or could not be read completely.";
        return result;
    }
    const auto size_after=std::filesystem::file_size(path,error);
    if(error||size_after!=size){
        result.storage.clear();result.code="gltf.dependency-changed";
        result.detail="Source or dependency changed while its snapshot was captured.";
        return result;
    }
    result.valid=true;result.code="ok";return result;
}

std::optional<std::string> decode_uri_path(const std::string_view uri){
    if(uri.empty()||uri.starts_with("//")||uri.find_first_of("?#\\")!=std::string_view::npos)
        return std::nullopt;
    const auto hex=[](const char value)->int{
        if(value>='0'&&value<='9')return value-'0';
        if(value>='a'&&value<='f')return value-'a'+10;
        if(value>='A'&&value<='F')return value-'A'+10;
        return -1;
    };
    std::string decoded;decoded.reserve(uri.size());
    for(std::size_t index=0;index<uri.size();++index){
        if(uri[index]!='%'){
            const auto byte=static_cast<unsigned char>(uri[index]);
            if(byte<0x20U||byte==0x7fU)return std::nullopt;
            decoded.push_back(uri[index]);continue;
        }
        if(index+2U>=uri.size())return std::nullopt;
        const int high=hex(uri[index+1U]),low=hex(uri[index+2U]);
        if(high<0||low<0)return std::nullopt;
        const auto byte=static_cast<unsigned char>((high<<4)|low);
        if(byte==0U||byte<0x20U||byte==0x7fU||byte=='\\')return std::nullopt;
        decoded.push_back(static_cast<char>(byte));index+=2U;
    }
    return decoded;
}

std::optional<std::filesystem::path> normalized_dependency_path(const std::string_view uri){
    const auto decoded=decode_uri_path(uri);if(!decoded)return std::nullopt;
    const std::filesystem::path candidate(*decoded);
    if(candidate.empty()||candidate.is_absolute()||candidate.has_root_name()||
       candidate.has_root_directory())return std::nullopt;
    const auto normalized=candidate.lexically_normal();
    if(normalized.empty()||normalized==".")return std::nullopt;
    for(const auto& part:normalized)if(part=="..")return std::nullopt;
    if(normalized.generic_string().find(':')!=std::string::npos)return std::nullopt;
    return normalized;
}

bool path_is_within(const std::filesystem::path& root,
                    const std::filesystem::path& candidate){
    auto root_part=root.begin(),candidate_part=candidate.begin();
    for(;root_part!=root.end();++root_part,++candidate_part)
        if(candidate_part==candidate.end()||*root_part!=*candidate_part)return false;
    return true;
}

std::uint32_t read_u32(const std::span<const std::byte> bytes, const std::size_t offset) {
    if (offset + 4U > bytes.size()) return 0U;
    const auto* data = reinterpret_cast<const unsigned char*>(bytes.data() + offset);
    return static_cast<std::uint32_t>(data[0]) |
        (static_cast<std::uint32_t>(data[1]) << 8U) |
        (static_cast<std::uint32_t>(data[2]) << 16U) |
        (static_cast<std::uint32_t>(data[3]) << 24U);
}

Mat4 identity() {
    Mat4 result{};
    result.value[0] = result.value[5] = result.value[10] = result.value[15] = 1.0F;
    return result;
}

Mat4 multiply(const Mat4& left, const Mat4& right) {
    Mat4 result{};
    for (int column = 0; column < 4; ++column)
        for (int row = 0; row < 4; ++row)
            for (int k = 0; k < 4; ++k)
                result.value[static_cast<std::size_t>(column * 4 + row)] +=
                    left.value[static_cast<std::size_t>(k * 4 + row)] *
                    right.value[static_cast<std::size_t>(column * 4 + k)];
    return result;
}

Mat4 node_transform(const Json& node) {
    if (node.contains("matrix") && node.at("matrix").is_array() && node.at("matrix").size() == 16U) {
        Mat4 result{};
        for (std::size_t index = 0; index < 16U; ++index) result.value[index] = node.at("matrix")[index].get<float>();
        return result;
    }
    const auto translation = node.value("translation", std::vector<float>{0.0F, 0.0F, 0.0F});
    const auto rotation = node.value("rotation", std::vector<float>{0.0F, 0.0F, 0.0F, 1.0F});
    const auto scale = node.value("scale", std::vector<float>{1.0F, 1.0F, 1.0F});
    if (translation.size() != 3U || rotation.size() != 4U || scale.size() != 3U) return identity();
    const float x = rotation[0], y = rotation[1], z = rotation[2], w = rotation[3];
    Mat4 result = identity();
    result.value[0] = (1.0F - 2.0F * (y * y + z * z)) * scale[0];
    result.value[1] = (2.0F * (x * y + z * w)) * scale[0];
    result.value[2] = (2.0F * (x * z - y * w)) * scale[0];
    result.value[4] = (2.0F * (x * y - z * w)) * scale[1];
    result.value[5] = (1.0F - 2.0F * (x * x + z * z)) * scale[1];
    result.value[6] = (2.0F * (y * z + x * w)) * scale[1];
    result.value[8] = (2.0F * (x * z + y * w)) * scale[2];
    result.value[9] = (2.0F * (y * z - x * w)) * scale[2];
    result.value[10] = (1.0F - 2.0F * (x * x + y * y)) * scale[2];
    result.value[12] = translation[0]; result.value[13] = translation[1]; result.value[14] = translation[2];
    return result;
}

std::array<float, 3> transform_point(const Mat4& matrix, const std::array<float, 3>& value) {
    return {
        matrix.value[0] * value[0] + matrix.value[4] * value[1] + matrix.value[8] * value[2] + matrix.value[12],
        matrix.value[1] * value[0] + matrix.value[5] * value[1] + matrix.value[9] * value[2] + matrix.value[13],
        matrix.value[2] * value[0] + matrix.value[6] * value[1] + matrix.value[10] * value[2] + matrix.value[14]
    };
}

std::array<float, 3> normalize(const std::array<float, 3>& value) {
    const float length = std::sqrt(value[0] * value[0] + value[1] * value[1] + value[2] * value[2]);
    if (length <= std::numeric_limits<float>::epsilon()) return {0.0F, 1.0F, 0.0F};
    return {value[0] / length, value[1] / length, value[2] / length};
}

std::array<float, 3> transform_normal(const Mat4& matrix, const std::array<float, 3>& normal) {
    const float a = matrix.value[0], b = matrix.value[4], c = matrix.value[8];
    const float d = matrix.value[1], e = matrix.value[5], f = matrix.value[9];
    const float g = matrix.value[2], h = matrix.value[6], i = matrix.value[10];
    const float determinant = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g);
    if (std::abs(determinant) <= std::numeric_limits<float>::epsilon()) return normalize(normal);
    const float inverse = 1.0F / determinant;
    return normalize({
        ((e * i - f * h) * normal[0] + (f * g - d * i) * normal[1] + (d * h - e * g) * normal[2]) * inverse,
        ((c * h - b * i) * normal[0] + (a * i - c * g) * normal[1] + (b * g - a * h) * normal[2]) * inverse,
        ((b * f - c * e) * normal[0] + (c * d - a * f) * normal[1] + (a * e - b * d) * normal[2]) * inverse
    });
}

std::size_t component_size(const std::uint32_t type) {
    switch (type) {
    case 5120U: case 5121U: return 1U;
    case 5122U: case 5123U: return 2U;
    case 5125U: case 5126U: return 4U;
    default: return 0U;
    }
}

std::size_t type_components(const std::string& type) {
    if (type == "SCALAR") return 1U;
    if (type == "VEC2") return 2U;
    if (type == "VEC3") return 3U;
    if (type == "VEC4") return 4U;
    if (type == "MAT4") return 16U;
    return 0U;
}

bool make_accessor(const Json& document, const std::span<const std::byte> binary,
                   const std::size_t accessor_index, AccessorView& result) {
    const auto& accessors = document.at("accessors");
    const auto& views = document.at("bufferViews");
    if (accessor_index >= accessors.size()) return false;
    const auto& accessor = accessors[accessor_index];
    if (!accessor.contains("bufferView") || accessor.contains("sparse")) return false;
    const auto view_index = accessor.at("bufferView").get<std::size_t>();
    if (view_index >= views.size()) return false;
    const auto& view = views[view_index];
    if (view.value("buffer", 0U) != 0U) return false;
    result.component_type = accessor.value("componentType", 0U);
    result.components = type_components(accessor.value("type", std::string{}));
    result.count = accessor.value("count", 0U);
    result.normalized = accessor.value("normalized", false);
    const std::size_t packed = component_size(result.component_type) * result.components;
    result.stride = view.value("byteStride", packed);
    const std::size_t offset = view.value("byteOffset", 0U) + accessor.value("byteOffset", 0U);
    if (packed == 0U || result.stride < packed || offset > binary.size() ||
        (result.count > 0U && offset + (result.count - 1U) * result.stride + packed > binary.size())) return false;
    result.data = binary.data() + offset;
    return true;
}

std::span<const std::byte> buffer_view_bytes(const Json& document, const std::span<const std::byte> binary,
                                             const std::size_t view_index) {
    const auto& views = document.at("bufferViews");
    if (view_index >= views.size()) return {};
    const auto& view = views[view_index];
    if (view.value("buffer", 0U) != 0U) return {};
    const std::size_t offset = view.value("byteOffset", 0U);
    const std::size_t length = view.value("byteLength", 0U);
    if (length == 0U || offset > binary.size() || length > binary.size() - offset) return {};
    return binary.subspan(offset, length);
}

float read_float_component(const AccessorView& view, const std::size_t element, const std::size_t component) {
    const std::byte* source = view.data + element * view.stride + component * component_size(view.component_type);
    switch (view.component_type) {
    case 5120U: {
        std::int8_t value{}; std::memcpy(&value, source, sizeof(value));
        return view.normalized ? std::max(static_cast<float>(value) / 127.0F, -1.0F) : static_cast<float>(value);
    }
    case 5121U: {
        std::uint8_t value{}; std::memcpy(&value, source, sizeof(value));
        return view.normalized ? static_cast<float>(value) / 255.0F : static_cast<float>(value);
    }
    case 5122U: {
        std::int16_t value{}; std::memcpy(&value, source, sizeof(value));
        return view.normalized ? std::max(static_cast<float>(value) / 32767.0F, -1.0F) : static_cast<float>(value);
    }
    case 5123U: {
        std::uint16_t value{}; std::memcpy(&value, source, sizeof(value));
        return view.normalized ? static_cast<float>(value) / 65535.0F : static_cast<float>(value);
    }
    case 5125U: {
        std::uint32_t value{}; std::memcpy(&value, source, sizeof(value)); return static_cast<float>(value);
    }
    case 5126U: {
        float value{}; std::memcpy(&value, source, sizeof(value)); return value;
    }
    default: return 0.0F;
    }
}

std::uint32_t read_index(const AccessorView& view, const std::size_t element) {
    const std::byte* source = view.data + element * view.stride;
    if (view.component_type == 5121U) { std::uint8_t value{}; std::memcpy(&value, source, sizeof(value)); return value; }
    if (view.component_type == 5123U) { std::uint16_t value{}; std::memcpy(&value, source, sizeof(value)); return value; }
    if (view.component_type == 5125U) { std::uint32_t value{}; std::memcpy(&value, source, sizeof(value)); return value; }
    return std::numeric_limits<std::uint32_t>::max();
}

std::uint32_t read_unsigned_component(const AccessorView& view, const std::size_t element, const std::size_t component) {
    const std::byte* source = view.data + element * view.stride + component * component_size(view.component_type);
    if (view.component_type == 5121U) { std::uint8_t value{}; std::memcpy(&value, source, sizeof(value)); return value; }
    if (view.component_type == 5123U) { std::uint16_t value{}; std::memcpy(&value, source, sizeof(value)); return value; }
    return std::numeric_limits<std::uint32_t>::max();
}

void generate_normals(std::vector<GltfDecodedVertex>& vertices, const std::vector<std::uint32_t>& indices,
                      const std::size_t vertex_begin, const std::size_t index_begin) {
    for (std::size_t index = vertex_begin; index < vertices.size(); ++index) vertices[index].normal = {};
    for (std::size_t index = index_begin; index + 2U < indices.size(); index += 3U) {
        auto& a = vertices[indices[index]]; auto& b = vertices[indices[index + 1U]]; auto& c = vertices[indices[index + 2U]];
        const std::array<float, 3> ab{b.position[0] - a.position[0], b.position[1] - a.position[1], b.position[2] - a.position[2]};
        const std::array<float, 3> ac{c.position[0] - a.position[0], c.position[1] - a.position[1], c.position[2] - a.position[2]};
        const std::array<float, 3> face{ab[1] * ac[2] - ab[2] * ac[1], ab[2] * ac[0] - ab[0] * ac[2], ab[0] * ac[1] - ab[1] * ac[0]};
        for (auto* vertex : {&a, &b, &c}) for (std::size_t axis = 0; axis < 3U; ++axis) vertex->normal[axis] += face[axis];
    }
    for (std::size_t index = vertex_begin; index < vertices.size(); ++index) vertices[index].normal = normalize(vertices[index].normal);
}

void generate_tangents(std::vector<GltfDecodedVertex>& vertices, const std::vector<std::uint32_t>& indices,
                       const std::size_t vertex_begin, const std::size_t index_begin) {
    std::vector<std::array<float, 3>> tangent_sum(vertices.size() - vertex_begin);
    std::vector<std::array<float, 3>> bitangent_sum(vertices.size() - vertex_begin);
    for (std::size_t index = index_begin; index + 2U < indices.size(); index += 3U) {
        const auto ia = indices[index], ib = indices[index + 1U], ic = indices[index + 2U];
        if (ia < vertex_begin || ib < vertex_begin || ic < vertex_begin ||
            ia >= vertices.size() || ib >= vertices.size() || ic >= vertices.size()) continue;
        const auto& a = vertices[ia]; const auto& b = vertices[ib]; const auto& c = vertices[ic];
        const std::array<float, 3> edge1{b.position[0]-a.position[0], b.position[1]-a.position[1], b.position[2]-a.position[2]};
        const std::array<float, 3> edge2{c.position[0]-a.position[0], c.position[1]-a.position[1], c.position[2]-a.position[2]};
        const float du1=b.texcoord[0]-a.texcoord[0], dv1=b.texcoord[1]-a.texcoord[1];
        const float du2=c.texcoord[0]-a.texcoord[0], dv2=c.texcoord[1]-a.texcoord[1];
        const float determinant=du1*dv2-du2*dv1;
        if (std::abs(determinant) <= std::numeric_limits<float>::epsilon()) continue;
        const float inverse=1.0F/determinant;
        const std::array<float,3> tangent{(edge1[0]*dv2-edge2[0]*dv1)*inverse,
            (edge1[1]*dv2-edge2[1]*dv1)*inverse,(edge1[2]*dv2-edge2[2]*dv1)*inverse};
        const std::array<float,3> bitangent{(edge2[0]*du1-edge1[0]*du2)*inverse,
            (edge2[1]*du1-edge1[1]*du2)*inverse,(edge2[2]*du1-edge1[2]*du2)*inverse};
        for (const auto vertex : {ia,ib,ic}) for (std::size_t axis=0;axis<3U;++axis) {
            tangent_sum[vertex-vertex_begin][axis]+=tangent[axis];
            bitangent_sum[vertex-vertex_begin][axis]+=bitangent[axis];
        }
    }
    for (std::size_t index=vertex_begin;index<vertices.size();++index) {
        const auto& normal=vertices[index].normal;
        auto tangent=tangent_sum[index-vertex_begin];
        const float projection=normal[0]*tangent[0]+normal[1]*tangent[1]+normal[2]*tangent[2];
        for (std::size_t axis=0;axis<3U;++axis) tangent[axis]-=normal[axis]*projection;
        tangent=normalize(tangent);
        const auto& bitangent=bitangent_sum[index-vertex_begin];
        const std::array<float,3> cross_value{normal[1]*tangent[2]-normal[2]*tangent[1],
            normal[2]*tangent[0]-normal[0]*tangent[2],normal[0]*tangent[1]-normal[1]*tangent[0]};
        const float handedness=cross_value[0]*bitangent[0]+cross_value[1]*bitangent[1]+cross_value[2]*bitangent[2] < 0.0F ? -1.0F : 1.0F;
        vertices[index].tangent={tangent[0],tangent[1],tangent[2],handedness};
    }
}

} // namespace

GltfBinaryContainer read_glb_container(const std::filesystem::path& path) {
    GltfBinaryContainer result;
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input) {
        result.code = "gltf.source-unavailable";
        result.detail = "GLB source file could not be opened.";
        return result;
    }
    const auto stream_size = input.tellg();
    if (stream_size < 12) {
        result.code = "gltf.invalid-header";
        result.detail = "GLB header is truncated.";
        return result;
    }
    result.source_bytes = static_cast<std::uint64_t>(stream_size);
    result.storage.resize(static_cast<std::size_t>(stream_size));
    input.seekg(0);
    input.read(reinterpret_cast<char*>(result.storage.data()), stream_size);
    if (!input || input.gcount() != stream_size) {
        result.code = "gltf.source-read-failed";
        result.detail = "GLB source could not be read completely.";
        return result;
    }
    const std::span<const std::byte> container(result.storage);
    const auto declared_length = read_u32(container, 8);
    if (read_u32(container, 0) != 0x46546c67U || read_u32(container, 4) != 2U ||
        declared_length != result.storage.size()) {
        result.code = "gltf.invalid-header";
        result.detail = "Only complete GLB 2 containers are supported.";
        return result;
    }
    result.version = 2U;

    constexpr std::uint32_t json_chunk_type = 0x4e4f534aU;
    constexpr std::uint32_t binary_chunk_type = 0x004e4942U;
    bool found_json = false;
    bool found_binary = false;
    std::size_t cursor = 12U;
    while (cursor < result.storage.size()) {
        if (result.storage.size() - cursor < 8U) {
            result.code = "gltf.invalid-chunk-header";
            result.detail = "GLB chunk header is truncated.";
            return result;
        }
        const auto length = read_u32(container, cursor);
        const auto type = read_u32(container, cursor + 4U);
        cursor += 8U;
        if (length > result.storage.size() - cursor) {
            result.code = type == json_chunk_type ? "gltf.truncated-json" : "gltf.missing-binary-chunk";
            result.detail = "GLB chunk payload is truncated.";
            return result;
        }
        if (!found_json && type != json_chunk_type) {
            result.code = "gltf.missing-json-chunk";
            result.detail = "First GLB chunk must contain JSON.";
            return result;
        }
        const auto payload = container.subspan(cursor, length);
        if (type == json_chunk_type) {
            if (found_json) {
                result.code = "gltf.duplicate-json-chunk";
                result.detail = "GLB contains more than one JSON chunk.";
                return result;
            }
            if (length == 0U) {
                result.code = "gltf.invalid-json-length";
                result.detail = "GLB JSON chunk must not be empty.";
                return result;
            }
            result.json_offset = cursor;
            result.json_size = length;
            found_json = true;
        } else if (type == binary_chunk_type) {
            if (found_binary) {
                result.code = "gltf.duplicate-binary-chunk";
                result.detail = "GLB contains more than one binary chunk.";
                return result;
            }
            result.binary_offset = cursor;
            result.binary_size = length;
            result.has_binary_chunk = true;
            found_binary = true;
        }
        cursor += length;
    }
    if (!found_json) {
        result.code = "gltf.missing-json-chunk";
        result.detail = "GLB JSON chunk is missing.";
        return result;
    }
    result.valid = true;
    result.code = "ok";
    result.detail = found_binary
        ? "GLB 2 container validated and split into JSON and binary payloads."
        : "GLB 2 container validated with a JSON payload and no BIN chunk.";
    return result;
}

GltfSourceSnapshot read_gltf_source_snapshot(const std::filesystem::path& path,
                                             const GltfSourceSnapshotLimits& limits){
    GltfSourceSnapshot result;
    if(limits.maximum_document_bytes==0U||limits.maximum_dependency_bytes==0U||
       limits.maximum_total_bytes==0U||limits.maximum_dependencies==0U){
        result.code="gltf.invalid-snapshot-limits";
        result.detail="glTF snapshot limits must all be positive.";return result;
    }
    std::error_code error;
    const auto source_path=std::filesystem::weakly_canonical(path,error);
    std::string extension=source_path.extension().string();
    std::ranges::transform(extension,extension.begin(),[](const unsigned char value){
        return static_cast<char>(std::tolower(value));
    });
    if(error||extension!=".gltf"){
        result.code="gltf.unsupported-container";
        result.detail="External resource snapshots require a JSON .gltf source.";return result;
    }
    const auto source_root=source_path.parent_path();
    auto source=read_bounded_file(source_path,limits.maximum_document_bytes);
    if(!source.valid){result.code=source.code;result.detail=source.detail;return result;}
    if(source.storage.size()>limits.maximum_total_bytes){
        result.code="gltf.total-budget-exceeded";
        result.detail="glTF document exceeds the total immutable snapshot budget.";return result;
    }
    result.source_path=source_path;result.source_bytes=source.storage.size();
    result.total_bytes=source.storage.size();result.storage=std::move(source.storage);
    const auto source_hash=sha256_bytes(result.storage);
    if(!source_hash.success){result.code=source_hash.code;result.detail=source_hash.detail;return result;}
    result.content_hash=source_hash.value;

    Json document;
    try{
        document=Json::parse(std::string(reinterpret_cast<const char*>(result.storage.data()),
                                         result.storage.size()));
    }catch(const std::exception& exception){
        result.code="gltf.invalid-json";result.detail=exception.what();return result;
    }
    if(!document.is_object()){
        result.code="gltf.invalid-json";result.detail="glTF document root must be an object.";
        return result;
    }

    std::unordered_map<std::string,std::size_t> lookup;
    const auto collect=[&](const Json& entries,const std::string_view kind)->bool{
        if(!entries.is_array()){
            result.code="gltf.invalid-json";
            result.detail="glTF buffer and image collections must be arrays.";return false;
        }
        for(const auto& entry:entries){
            if(!entry.is_object()){
                result.code="gltf.invalid-json";
                result.detail="glTF buffer and image entries must be objects.";return false;
            }
            if(!entry.contains("uri"))continue;
            if(!entry.at("uri").is_string()){
                result.code="gltf.invalid-uri";
                result.detail="glTF dependency URI must be a string.";return false;
            }
            const auto uri=entry.at("uri").get<std::string>();
            // fastgltf owns data URI decoding. Its encoded payload is already
            // bounded by maximum_document_bytes and is not an external file.
            if(uri.starts_with("data:"))continue;
            const auto relative=normalized_dependency_path(uri);
            if(!relative){
                result.code="gltf.external-uri-unsafe";
                result.detail="External glTF URIs must be normalized relative file paths.";
                return false;
            }
            const auto normalized=relative->generic_string();
            if(const auto found=lookup.find(normalized);found!=lookup.end()){
                if(result.dependencies[found->second].kind!=kind)
                    result.dependencies[found->second].kind="shared";
                continue;
            }
            if(result.dependencies.size()>=limits.maximum_dependencies){
                result.code="gltf.dependency-count-exceeded";
                result.detail="glTF external dependency count exceeds the snapshot budget.";
                return false;
            }
            error.clear();
            const auto dependency_path=std::filesystem::weakly_canonical(source_root/ *relative,error);
            if(error||!path_is_within(source_root,dependency_path)){
                result.code="gltf.external-uri-outside-root";
                result.detail="External glTF dependency resolves outside the source directory.";
                return false;
            }
            auto bytes=read_bounded_file(dependency_path,limits.maximum_dependency_bytes);
            if(!bytes.valid){
                result.code=bytes.code;result.detail=bytes.detail+" URI: "+uri;return false;
            }
            if(result.total_bytes>limits.maximum_total_bytes||
               bytes.storage.size()>limits.maximum_total_bytes-result.total_bytes){
                result.code="gltf.total-budget-exceeded";
                result.detail="External glTF dependencies exceed the total snapshot budget.";
                return false;
            }
            const auto hash=sha256_bytes(bytes.storage);
            if(!hash.success){result.code=hash.code;result.detail=hash.detail;return false;}
            GltfExternalResourceSnapshot dependency;
            dependency.uri=uri;dependency.normalized_relative_path=normalized;
            dependency.kind=std::string(kind);dependency.content_hash=hash.value;
            dependency.source_bytes=bytes.storage.size();dependency.storage=std::move(bytes.storage);
            result.total_bytes+=dependency.source_bytes;
            lookup.emplace(normalized,result.dependencies.size());
            result.dependencies.push_back(std::move(dependency));
        }
        return true;
    };
    if(!collect(document.value("buffers",Json::array()),"buffer")||
       !collect(document.value("images",Json::array()),"image")){
        result.dependencies.clear();return result;
    }
    result.valid=true;result.code="ok";
    result.detail="JSON glTF document and external dependencies captured as immutable bounded snapshots.";
    return result;
}

GltfDependencyVerification verify_gltf_source_snapshot(
    const GltfSourceSnapshot& snapshot,const GltfSourceSnapshotLimits& limits){
    GltfDependencyVerification result;
    const auto changed=[&](const std::string& path,const std::string& detail){
        result.unchanged=false;result.code="gltf.dependency-changed";
        result.normalized_relative_path=path;result.detail=detail;
    };
    if(!snapshot.valid||snapshot.source_path.empty()||snapshot.content_hash.empty()){
        result.code="gltf.invalid-snapshot";
        result.detail="A valid immutable glTF source snapshot is required.";return result;
    }
    const auto source=sha256_file(snapshot.source_path,static_cast<std::size_t>(
        std::min<std::uint64_t>(limits.maximum_document_bytes,
                                std::numeric_limits<std::size_t>::max())));
    if(!source.success||source.value!=snapshot.content_hash||source.bytes!=snapshot.source_bytes){
        changed("<document>","glTF source document no longer matches its immutable snapshot.");
        return result;
    }
    if(source.bytes>limits.maximum_total_bytes){
        changed("<document>","Current glTF source exceeds the total snapshot budget.");return result;
    }
    const auto root=snapshot.source_path.parent_path();
    std::uint64_t total=source.bytes;
    for(const auto& dependency:snapshot.dependencies){
        const auto relative=normalized_dependency_path(dependency.normalized_relative_path);
        if(!relative){
            changed(dependency.normalized_relative_path,
                    "glTF snapshot contains an invalid normalized path.");return result;
        }
        std::error_code error;
        const auto path=std::filesystem::weakly_canonical(root/ *relative,error);
        if(error||!path_is_within(root,path)){
            changed(dependency.normalized_relative_path,
                    "glTF dependency no longer resolves beneath the source directory.");
            return result;
        }
        const auto hash=sha256_file(path,static_cast<std::size_t>(
            std::min<std::uint64_t>(limits.maximum_dependency_bytes,
                                    std::numeric_limits<std::size_t>::max())));
        if(!hash.success||hash.value!=dependency.content_hash||hash.bytes!=dependency.source_bytes){
            changed(dependency.normalized_relative_path,
                    "glTF dependency no longer matches its immutable snapshot.");return result;
        }
        if(total>limits.maximum_total_bytes||hash.bytes>limits.maximum_total_bytes-total){
            changed(dependency.normalized_relative_path,
                    "Current glTF dependency set exceeds the total snapshot budget.");return result;
        }
        total+=hash.bytes;
    }
    if(total!=snapshot.total_bytes){
        changed("<dependency-set>","glTF dependency byte total no longer matches its snapshot.");
        return result;
    }
    result.unchanged=true;result.code="ok";
    result.detail="glTF source and dependencies still match the immutable snapshot.";
    return result;
}

void compute_decoded_scene_bounds(DecodedSceneAsset& asset) {
    for (auto& primitive:asset.primitives) {
        std::array<float,3> minimum{std::numeric_limits<float>::max(),std::numeric_limits<float>::max(),std::numeric_limits<float>::max()};
        std::array<float,3> maximum{-std::numeric_limits<float>::max(),-std::numeric_limits<float>::max(),-std::numeric_limits<float>::max()};
        const auto end=std::min<std::size_t>(asset.indices.size(),static_cast<std::size_t>(primitive.first_index)+primitive.index_count);
        bool has_vertex=false;
        for (std::size_t index=primitive.first_index;index<end;++index) {
            const auto vertex_index=asset.indices[index]; if (vertex_index>=asset.vertices.size()) continue;
            has_vertex=true;
            for (std::size_t axis=0;axis<3U;++axis) {
                minimum[axis]=std::min(minimum[axis],asset.vertices[vertex_index].position[axis]);
                maximum[axis]=std::max(maximum[axis],asset.vertices[vertex_index].position[axis]);
            }
        }
        primitive.bounds_center={}; primitive.bounds_radius=0.0F;
        if (!has_vertex) continue;
        for (std::size_t axis=0;axis<3U;++axis) primitive.bounds_center[axis]=(minimum[axis]+maximum[axis])*0.5F;
        for (std::size_t index=primitive.first_index;index<end;++index) {
            const auto vertex_index=asset.indices[index]; if (vertex_index>=asset.vertices.size()) continue;
            float distance_squared{};
            for (std::size_t axis=0;axis<3U;++axis) {
                const float delta=asset.vertices[vertex_index].position[axis]-primitive.bounds_center[axis]; distance_squared+=delta*delta;
            }
            primitive.bounds_radius=std::max(primitive.bounds_radius,std::sqrt(distance_squared));
        }
        if (primitive.skin>=0) primitive.bounds_radius*=1.5F;
    }
}

#if NOEMANCER_HAS_FASTGLTF

Mat4 fastgltf_matrix(const fastgltf::math::fmat4x4& source) {
    Mat4 result{};
    for (std::size_t column = 0; column < 4U; ++column)
        for (std::size_t row = 0; row < 4U; ++row)
            result.value[column * 4U + row] = source.col(column)[row];
    return result;
}

Mat4 fastgltf_node_transform(const fastgltf::Node& node) {
    return std::visit([&](const auto& transform) {
        using Transform = std::decay_t<decltype(transform)>;
        if constexpr (std::is_same_v<Transform, fastgltf::TRS>) {
            Mat4 result = identity();
            const auto& translation = transform.translation;
            const auto& rotation = transform.rotation;
            const auto& scale = transform.scale;
            const float x = rotation.x();
            const float y = rotation.y();
            const float z = rotation.z();
            const float w = rotation.w();
            result.value[0] = (1.0F - 2.0F * (y * y + z * z)) * scale.x();
            result.value[1] = (2.0F * (x * y + z * w)) * scale.x();
            result.value[2] = (2.0F * (x * z - y * w)) * scale.x();
            result.value[4] = (2.0F * (x * y - z * w)) * scale.y();
            result.value[5] = (1.0F - 2.0F * (x * x + z * z)) * scale.y();
            result.value[6] = (2.0F * (y * z + x * w)) * scale.y();
            result.value[8] = (2.0F * (x * z + y * w)) * scale.z();
            result.value[9] = (2.0F * (y * z - x * w)) * scale.z();
            result.value[10] = (1.0F - 2.0F * (x * x + y * y)) * scale.z();
            result.value[12] = translation.x();
            result.value[13] = translation.y();
            result.value[14] = translation.z();
            return result;
        } else {
            return fastgltf_matrix(transform);
        }
    }, node.transform);
}

bool fastgltf_accessor_is(const fastgltf::Asset& asset, const std::size_t index,
                          const fastgltf::AccessorType type, const std::size_t count = 0U) {
    if (index >= asset.accessors.size()) return false;
    const auto& accessor = asset.accessors[index];
    return accessor.type == type && accessor.bufferViewIndex.has_value() && (count == 0U || accessor.count == count);
}

std::array<float, 3> fastgltf_vec3(const fastgltf::math::fvec3& value) {
    return {value.x(), value.y(), value.z()};
}

std::array<float, 4> fastgltf_vec4(const fastgltf::math::fvec4& value) {
    return {value.x(), value.y(), value.z(), value.w()};
}

std::span<const std::byte> fastgltf_image_bytes(const fastgltf::Asset& asset,
                                                const fastgltf::Image& image,
                                                fastgltf::MimeType& mime_type) {
    std::span<const std::byte> result;
    std::visit([&](const auto& source) {
        using Source = std::decay_t<decltype(source)>;
        if constexpr (std::is_same_v<Source, fastgltf::sources::BufferView>) {
            mime_type = source.mimeType;
            const fastgltf::DefaultBufferDataAdapter adapter{};
            const auto data = adapter(asset, source.bufferViewIndex);
            result = {data.data(), data.size()};
        } else if constexpr (std::is_same_v<Source, fastgltf::sources::Array>) {
            mime_type = source.mimeType;
            result = {source.bytes.data(), source.bytes.size_bytes()};
        } else if constexpr (std::is_same_v<Source, fastgltf::sources::Vector>) {
            mime_type = source.mimeType;
            result = {source.bytes.data(), source.bytes.size()};
        } else if constexpr (std::is_same_v<Source, fastgltf::sources::ByteView>) {
            mime_type = source.mimeType;
            result = {source.bytes.data(), source.bytes.size()};
        }
    }, image.data);
    return result;
}

void fastgltf_decode_images(const fastgltf::Asset& asset, GltfMeshData& result) {
    result.images.resize(asset.images.size());
    for (std::size_t image_index = 0; image_index < asset.images.size(); ++image_index) {
        auto& decoded_image = result.images[image_index];
        fastgltf::MimeType mime_type = fastgltf::MimeType::None;
        const auto encoded = fastgltf_image_bytes(asset, asset.images[image_index], mime_type);
        decoded_image.mime_type = std::string(fastgltf::getMimeTypeString(mime_type));
        if (mime_type != fastgltf::MimeType::PNG) {
            decoded_image.code = encoded.empty() ? "image.external-uri-unsupported" : "image.unsupported-mime";
            continue;
        }
        const auto png = decode_png_rgba8(encoded);
        decoded_image.valid = png.valid;
        decoded_image.code = png.code;
        decoded_image.width = png.width;
        decoded_image.height = png.height;
        decoded_image.rgba8 = png.rgba8;
    }
}

std::string fastgltf_animation_path(const fastgltf::AnimationPath path) {
    switch (path) {
    case fastgltf::AnimationPath::Translation: return "translation";
    case fastgltf::AnimationPath::Rotation: return "rotation";
    case fastgltf::AnimationPath::Scale: return "scale";
    case fastgltf::AnimationPath::Weights: return "weights";
    }
    return {};
}

std::string fastgltf_interpolation(const fastgltf::AnimationInterpolation interpolation) {
    switch (interpolation) {
    case fastgltf::AnimationInterpolation::Linear: return "LINEAR";
    case fastgltf::AnimationInterpolation::Step: return "STEP";
    case fastgltf::AnimationInterpolation::CubicSpline: return "CUBICSPLINE";
    }
    return {};
}

int fastgltf_texture_image(const fastgltf::Asset& asset, const std::vector<GltfDecodedImage>& images,
                           const fastgltf::Optional<fastgltf::TextureInfo>& texture_info) {
    if (!texture_info.has_value() || texture_info->texCoordIndex != 0U ||
        texture_info->textureIndex >= asset.textures.size()) return -1;
    const auto& texture = asset.textures[texture_info->textureIndex];
    if (!texture.imageIndex.has_value() || *texture.imageIndex >= images.size()) return -1;
    const auto image_index = *texture.imageIndex;
    return images[image_index].valid ? static_cast<int>(image_index) : -1;
}

int fastgltf_texture_image(const fastgltf::Asset& asset, const std::vector<GltfDecodedImage>& images,
                           const fastgltf::Optional<fastgltf::NormalTextureInfo>& texture_info) {
    if (!texture_info.has_value() || texture_info->texCoordIndex != 0U ||
        texture_info->textureIndex >= asset.textures.size()) return -1;
    const auto& texture = asset.textures[texture_info->textureIndex];
    if (!texture.imageIndex.has_value() || *texture.imageIndex >= images.size()) return -1;
    const auto image_index = *texture.imageIndex;
    return images[image_index].valid ? static_cast<int>(image_index) : -1;
}

int fastgltf_texture_image(const fastgltf::Asset& asset, const std::vector<GltfDecodedImage>& images,
                           const fastgltf::Optional<fastgltf::OcclusionTextureInfo>& texture_info) {
    if (!texture_info.has_value() || texture_info->texCoordIndex != 0U ||
        texture_info->textureIndex >= asset.textures.size()) return -1;
    const auto& texture = asset.textures[texture_info->textureIndex];
    if (!texture.imageIndex.has_value() || *texture.imageIndex >= images.size()) return -1;
    const auto image_index = *texture.imageIndex;
    return images[image_index].valid ? static_cast<int>(image_index) : -1;
}

void fastgltf_decode_material(const fastgltf::Asset& asset, const fastgltf::Primitive& primitive,
                              const std::vector<GltfDecodedImage>& images, GltfDecodedPrimitive& decoded) {
    if (!primitive.materialIndex.has_value() || *primitive.materialIndex >= asset.materials.size()) return;
    const auto& material = asset.materials[*primitive.materialIndex];
    for (std::size_t component = 0; component < 4U; ++component)
        decoded.base_color[component] = material.pbrData.baseColorFactor[component];
    decoded.metallic = material.pbrData.metallicFactor;
    decoded.roughness = material.pbrData.roughnessFactor;
    decoded.unlit = material.unlit;
    decoded.double_sided = material.doubleSided;
    decoded.alpha_cutoff = material.alphaCutoff;
    decoded.emissive_factor = {material.emissiveFactor[0], material.emissiveFactor[1], material.emissiveFactor[2]};
    switch (material.alphaMode) {
    case fastgltf::AlphaMode::Opaque: decoded.alpha_mode = "OPAQUE"; break;
    case fastgltf::AlphaMode::Mask: decoded.alpha_mode = "MASK"; break;
    case fastgltf::AlphaMode::Blend: decoded.alpha_mode = "BLEND"; break;
    }
    decoded.base_color_image = fastgltf_texture_image(asset, images, material.pbrData.baseColorTexture);
    decoded.metallic_roughness_image = fastgltf_texture_image(asset, images, material.pbrData.metallicRoughnessTexture);
    decoded.normal_image = fastgltf_texture_image(asset, images, material.normalTexture);
    decoded.occlusion_image = fastgltf_texture_image(asset, images, material.occlusionTexture);
    decoded.emissive_image = fastgltf_texture_image(asset, images, material.emissiveTexture);
    if (material.normalTexture.has_value()) decoded.normal_scale = material.normalTexture->scale;
    if (material.occlusionTexture.has_value()) decoded.occlusion_strength = material.occlusionTexture->strength;
}

class ScopedGltfSnapshotDirectory final {
public:
    explicit ScopedGltfSnapshotDirectory(const GltfSourceSnapshot& snapshot) {
        static std::atomic_uint64_t next_directory{1U};
        std::string identity = snapshot.content_hash;
        if (const auto separator = identity.find(':'); separator != std::string::npos)
            identity.erase(0U, separator + 1U);
        if (identity.empty()) identity = "unhashed";

        std::error_code error;
        const auto temporary_root = std::filesystem::temp_directory_path(error);
        if (error) {
            code_ = "gltf.snapshot-staging-unavailable";
            detail_ = "The temporary directory for the immutable glTF snapshot is unavailable.";
            return;
        }
        for (std::size_t attempt = 0; attempt < 64U; ++attempt) {
            const auto sequence = next_directory.fetch_add(1U, std::memory_order_relaxed);
            path_ = temporary_root / ("noemancer-gltf-snapshot-" + identity + "-" + std::to_string(sequence));
            error.clear();
            if (std::filesystem::create_directory(path_, error)) break;
            path_.clear();
        }
        if (path_.empty()) {
            code_ = "gltf.snapshot-staging-unavailable";
            detail_ = "A unique immutable glTF snapshot directory could not be created.";
            return;
        }
        document_path_ = path_ / "source.gltf";
        if (!write_file(document_path_, snapshot.storage)) return;
        for (const auto& dependency : snapshot.dependencies) {
            const auto dependency_path = path_ / std::filesystem::path(dependency.normalized_relative_path);
            error.clear();
            std::filesystem::create_directories(dependency_path.parent_path(), error);
            if (error) {
                code_ = "gltf.snapshot-staging-failed";
                detail_ = "An external glTF dependency directory could not be staged.";
                return;
            }
            if (!write_file(dependency_path, dependency.storage)) return;
        }
        valid_ = true;
        code_ = "ok";
    }

    ScopedGltfSnapshotDirectory(const ScopedGltfSnapshotDirectory&) = delete;
    ScopedGltfSnapshotDirectory& operator=(const ScopedGltfSnapshotDirectory&) = delete;
    ~ScopedGltfSnapshotDirectory() {
        if (path_.empty()) return;
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }
    [[nodiscard]] bool valid() const noexcept { return valid_; }
    [[nodiscard]] const std::string& code() const noexcept { return code_; }
    [[nodiscard]] const std::string& detail() const noexcept { return detail_; }
    [[nodiscard]] const std::filesystem::path& document_path() const noexcept { return document_path_; }

private:
    bool write_file(const std::filesystem::path& destination,
                    const std::span<const std::byte> bytes) {
        if (bytes.size() > static_cast<std::size_t>(std::numeric_limits<std::streamsize>::max())) {
            code_ = "gltf.snapshot-staging-failed";
            detail_ = "An immutable glTF snapshot file exceeds the staging stream limit.";
            return false;
        }
        std::ofstream output(destination, std::ios::binary | std::ios::trunc);
        if (!output) {
            code_ = "gltf.snapshot-staging-failed";
            detail_ = "An immutable glTF snapshot file could not be created.";
            return false;
        }
        if (!bytes.empty()) output.write(reinterpret_cast<const char*>(bytes.data()),
                                         static_cast<std::streamsize>(bytes.size()));
        output.flush();
        if (!output) {
            code_ = "gltf.snapshot-staging-failed";
            detail_ = "An immutable glTF snapshot file could not be written completely.";
            return false;
        }
        return true;
    }
    bool valid_{};
    std::string code_;
    std::string detail_;
    std::filesystem::path path_;
    std::filesystem::path document_path_;
};

GltfMeshData decode_gltf_mesh_fastgltf(const std::span<const std::byte> storage,
                                       const std::filesystem::path& path,
                                       const bool binary_container) {
    GltfMeshData result;
    try {
        auto input = fastgltf::GltfDataBuffer::FromBytes(storage.data(), storage.size());
        if (!input) {
            result.code = "gltf.fastgltf-buffer-failed";
            result.detail = fastgltf::getErrorMessage(input.error());
            return result;
        }
        constexpr auto extensions = fastgltf::Extensions::KHR_materials_unlit |
            fastgltf::Extensions::KHR_mesh_quantization |
            fastgltf::Extensions::KHR_texture_transform |
            fastgltf::Extensions::EXT_meshopt_compression;
        fastgltf::Parser parser(extensions);
        constexpr auto options = fastgltf::Options::DontRequireValidAssetMember |
            fastgltf::Options::GenerateMeshIndices |
            fastgltf::Options::DecomposeNodeMatrices;
        const auto json_options = options | fastgltf::Options::LoadExternalBuffers |
            fastgltf::Options::LoadExternalImages;
        auto parsed = binary_container
            ? parser.loadGltfBinary(input.get(), path.parent_path(), options, fastgltf::Category::All)
            : parser.loadGltfJson(input.get(), path.parent_path(), json_options, fastgltf::Category::All);
        if (!parsed) {
            result.code = "gltf.fastgltf-parse-failed";
            result.detail = fastgltf::getErrorMessage(parsed.error());
            return result;
        }
        auto& asset = parsed.get();
        if (asset.scenes.empty()) throw std::runtime_error("fastgltf parsed no scenes.");
        const std::size_t scene_index = asset.defaultScene.value_or(0U);
        if (scene_index >= asset.scenes.size()) throw std::runtime_error("Default scene index is out of range.");

        fastgltf_decode_images(asset, result);

        std::vector<int> node_parents(asset.nodes.size(), -1);
        for (std::size_t parent = 0; parent < asset.nodes.size(); ++parent) {
            for (const auto child : asset.nodes[parent].children) {
                if (child >= asset.nodes.size() || node_parents[child] != -1)
                    throw std::runtime_error("Node hierarchy contains an invalid or multiply-parented child.");
                node_parents[child] = static_cast<int>(parent);
            }
        }

        result.skins.reserve(asset.skins.size());
        for (std::size_t skin_index = 0; skin_index < asset.skins.size(); ++skin_index) {
            const auto& skin = asset.skins[skin_index];
            if (skin.joints.empty() || skin.joints.size() > 64U)
                throw std::runtime_error("Skin joint count must be within the current GPU palette limit of 64.");
            std::unordered_map<std::size_t, std::size_t> joint_lookup;
            for (std::size_t joint = 0; joint < skin.joints.size(); ++joint) {
                if (skin.joints[joint] >= asset.nodes.size() || !joint_lookup.emplace(skin.joints[joint], joint).second)
                    throw std::runtime_error("Skin contains an invalid or duplicate joint node.");
            }
            if (skin.inverseBindMatrices.has_value() &&
                (!fastgltf_accessor_is(asset, *skin.inverseBindMatrices, fastgltf::AccessorType::Mat4, skin.joints.size()) ||
                 asset.accessors[*skin.inverseBindMatrices].componentType != fastgltf::ComponentType::Float))
                throw std::runtime_error("Skin inverse bind matrices must be FLOAT MAT4 accessor matching joint count.");
            GltfDecodedSkin decoded_skin;
            decoded_skin.name = skin.name.empty() ? "skin-" + std::to_string(skin_index) : std::string(skin.name);
            decoded_skin.joints.reserve(skin.joints.size());
            for (std::size_t joint = 0; joint < skin.joints.size(); ++joint) {
                const auto node_index = skin.joints[joint];
                GltfDecodedJoint decoded_joint;
                decoded_joint.node_index = static_cast<std::uint32_t>(node_index);
                decoded_joint.name = asset.nodes[node_index].name.empty()
                    ? "joint-" + std::to_string(joint) : std::string(asset.nodes[node_index].name);
                decoded_joint.local_transform = fastgltf_node_transform(asset.nodes[node_index]).value;
                decoded_joint.inverse_bind_matrix = identity().value;
                if (skin.inverseBindMatrices.has_value()) {
                    const auto matrix = fastgltf::getAccessorElement<fastgltf::math::fmat4x4>(
                        asset, asset.accessors[*skin.inverseBindMatrices], joint);
                    decoded_joint.inverse_bind_matrix = fastgltf_matrix(matrix).value;
                }
                int ancestor = node_parents[node_index];
                while (ancestor >= 0) {
                    const auto found = joint_lookup.find(static_cast<std::size_t>(ancestor));
                    if (found != joint_lookup.end()) {
                        decoded_joint.parent_joint = static_cast<int>(found->second);
                        break;
                    }
                    ancestor = node_parents[static_cast<std::size_t>(ancestor)];
                }
                decoded_skin.joints.push_back(std::move(decoded_joint));
            }
            result.skins.push_back(std::move(decoded_skin));
        }

        result.animations.reserve(asset.animations.size());
        for (std::size_t animation_index = 0; animation_index < asset.animations.size(); ++animation_index) {
            const auto& animation = asset.animations[animation_index];
            GltfAnimationClip clip;
            clip.name = animation.name.empty() ? "animation-" + std::to_string(animation_index) : std::string(animation.name);
            for (const auto& channel_data : animation.channels) {
                if (!channel_data.nodeIndex.has_value() || channel_data.samplerIndex >= animation.samplers.size())
                    throw std::runtime_error("Animation channel references an invalid sampler or target.");
                const auto& sampler = animation.samplers[channel_data.samplerIndex];
                const auto channel_path = fastgltf_animation_path(channel_data.path);
                if (channel_path != "translation" && channel_path != "rotation" && channel_path != "scale")
                    throw std::runtime_error("Only translation, rotation and scale animation channels are supported.");
                const auto interpolation = fastgltf_interpolation(sampler.interpolation);
                if (interpolation != "LINEAR" && interpolation != "STEP")
                    throw std::runtime_error("Animation interpolation must currently be LINEAR or STEP.");
                if (*channel_data.nodeIndex >= asset.nodes.size() ||
                    !fastgltf_accessor_is(asset, sampler.inputAccessor, fastgltf::AccessorType::Scalar) ||
                    asset.accessors[sampler.inputAccessor].componentType != fastgltf::ComponentType::Float)
                    throw std::runtime_error("Animation sampler input accessor is invalid.");
                const auto output_type = channel_path == "rotation" ? fastgltf::AccessorType::Vec4 : fastgltf::AccessorType::Vec3;
                if (!fastgltf_accessor_is(asset, sampler.outputAccessor, output_type) ||
                    asset.accessors[sampler.outputAccessor].componentType != fastgltf::ComponentType::Float)
                    throw std::runtime_error("Animation sampler output accessor is invalid.");
                const auto& input_accessor = asset.accessors[sampler.inputAccessor];
                const auto& output_accessor = asset.accessors[sampler.outputAccessor];
                if (input_accessor.count == 0U || output_accessor.count != input_accessor.count)
                    throw std::runtime_error("Animation sampler accessors are empty or mismatched.");
                GltfAnimationChannel channel;
                channel.node_index = static_cast<std::uint32_t>(*channel_data.nodeIndex);
                channel.path = channel_path;
                channel.interpolation = interpolation;
                channel.times.reserve(input_accessor.count);
                channel.values.reserve(output_accessor.count);
                float previous = -1.0F;
                for (std::size_t key = 0; key < input_accessor.count; ++key) {
                    const float time = fastgltf::getAccessorElement<float>(asset, input_accessor, key);
                    if (!std::isfinite(time) || time < 0.0F || time <= previous)
                        throw std::runtime_error("Animation key times must be finite, non-negative and strictly increasing.");
                    previous = time;
                    channel.times.push_back(time);
                    clip.duration = std::max(clip.duration, time);
                    std::array<float, 4> value{0.0F, 0.0F, 0.0F, channel_path == "rotation" ? 1.0F : 0.0F};
                    if (channel_path == "rotation") value = fastgltf_vec4(
                        fastgltf::getAccessorElement<fastgltf::math::fvec4>(asset, output_accessor, key));
                    else {
                        const auto vector = fastgltf_vec3(
                            fastgltf::getAccessorElement<fastgltf::math::fvec3>(asset, output_accessor, key));
                        std::copy(vector.begin(), vector.end(), value.begin());
                    }
                    if (!std::ranges::all_of(value, [](const float component) { return std::isfinite(component); }))
                        throw std::runtime_error("Animation channel contains a non-finite value.");
                    channel.values.push_back(value);
                }
                clip.channels.push_back(std::move(channel));
            }
            if (clip.channels.empty() || clip.duration <= 0.0F)
                throw std::runtime_error("Animation clip has no supported channels or positive duration.");
            result.animations.push_back(std::move(clip));
        }

        std::unordered_set<std::size_t> active_nodes;
        std::function<void(std::size_t, const Mat4&)> visit;
        visit = [&](const std::size_t node_index, const Mat4& parent) {
            if (node_index >= asset.nodes.size() || active_nodes.contains(node_index))
                throw std::runtime_error("Node graph is invalid or cyclic.");
            active_nodes.insert(node_index);
            const auto& node = asset.nodes[node_index];
            const Mat4 transform = multiply(parent, fastgltf_node_transform(node));
            if (node.meshIndex.has_value()) {
                const bool skinned = node.skinIndex.has_value();
                const auto skin_index = skinned ? *node.skinIndex : result.skins.size();
                if (skinned && skin_index >= result.skins.size()) throw std::runtime_error("Mesh node references an invalid skin.");
                if (*node.meshIndex >= asset.meshes.size()) throw std::runtime_error("Mesh index is out of range.");
                const auto& mesh = asset.meshes[*node.meshIndex];
                for (const auto& primitive : mesh.primitives) {
                    if (primitive.type != fastgltf::PrimitiveType::Triangles) continue;
                    const auto position_attribute = primitive.findAttribute("POSITION");
                    if (position_attribute == primitive.attributes.end() ||
                        !fastgltf_accessor_is(asset, position_attribute->accessorIndex, fastgltf::AccessorType::Vec3))
                        throw std::runtime_error("Primitive POSITION accessor is unsupported.");
                    const auto& positions = asset.accessors[position_attribute->accessorIndex];
                    const auto normal_attribute = primitive.findAttribute("NORMAL");
                    const bool has_normals = normal_attribute != primitive.attributes.end() &&
                        fastgltf_accessor_is(asset, normal_attribute->accessorIndex, fastgltf::AccessorType::Vec3, positions.count);
                    const auto texcoord_attribute = primitive.findAttribute("TEXCOORD_0");
                    const bool has_texcoords = texcoord_attribute != primitive.attributes.end() &&
                        fastgltf_accessor_is(asset, texcoord_attribute->accessorIndex, fastgltf::AccessorType::Vec2, positions.count);
                    const auto tangent_attribute = primitive.findAttribute("TANGENT");
                    const bool has_tangents = tangent_attribute != primitive.attributes.end() &&
                        fastgltf_accessor_is(asset, tangent_attribute->accessorIndex, fastgltf::AccessorType::Vec4, positions.count) &&
                        asset.accessors[tangent_attribute->accessorIndex].componentType == fastgltf::ComponentType::Float;
                    const auto joints_attribute = primitive.findAttribute("JOINTS_0");
                    const bool has_joints = joints_attribute != primitive.attributes.end() &&
                        fastgltf_accessor_is(asset, joints_attribute->accessorIndex, fastgltf::AccessorType::Vec4, positions.count) &&
                        (asset.accessors[joints_attribute->accessorIndex].componentType == fastgltf::ComponentType::UnsignedByte ||
                         asset.accessors[joints_attribute->accessorIndex].componentType == fastgltf::ComponentType::UnsignedShort);
                    const auto weights_attribute = primitive.findAttribute("WEIGHTS_0");
                    const bool has_weights = weights_attribute != primitive.attributes.end() &&
                        fastgltf_accessor_is(asset, weights_attribute->accessorIndex, fastgltf::AccessorType::Vec4, positions.count) &&
                        (asset.accessors[weights_attribute->accessorIndex].componentType == fastgltf::ComponentType::Float ||
                         ((asset.accessors[weights_attribute->accessorIndex].componentType == fastgltf::ComponentType::UnsignedByte ||
                           asset.accessors[weights_attribute->accessorIndex].componentType == fastgltf::ComponentType::UnsignedShort) &&
                          asset.accessors[weights_attribute->accessorIndex].normalized));
                    if (skinned && (!has_joints || !has_weights))
                        throw std::runtime_error("Skinned primitive requires matching JOINTS_0 and WEIGHTS_0 accessors.");
                    if (has_joints != has_weights) throw std::runtime_error("JOINTS_0 and WEIGHTS_0 must be provided together.");
                    if (has_joints && !skinned) throw std::runtime_error("Vertex skin influences require a skin on the mesh node.");

                    const std::size_t vertex_begin = result.vertices.size();
                    const Mat4 vertex_transform = skinned ? identity() : transform;
                    for (std::size_t vertex = 0; vertex < positions.count; ++vertex) {
                        GltfDecodedVertex decoded;
                        const auto position = fastgltf_vec3(fastgltf::getAccessorElement<fastgltf::math::fvec3>(
                            asset, positions, vertex));
                        decoded.position = transform_point(vertex_transform, position);
                        if (has_normals) {
                            const auto normal = fastgltf_vec3(fastgltf::getAccessorElement<fastgltf::math::fvec3>(
                                asset, asset.accessors[normal_attribute->accessorIndex], vertex));
                            decoded.normal = transform_normal(vertex_transform, normal);
                        }
                        if (has_texcoords) {
                            const auto uv = fastgltf::getAccessorElement<fastgltf::math::fvec2>(
                                asset, asset.accessors[texcoord_attribute->accessorIndex], vertex);
                            decoded.texcoord = {uv.x(), uv.y()};
                        }
                        if (has_tangents) {
                            const auto tangent = fastgltf_vec4(fastgltf::getAccessorElement<fastgltf::math::fvec4>(
                                asset, asset.accessors[tangent_attribute->accessorIndex], vertex));
                            const auto transformed = transform_normal(vertex_transform, {tangent[0], tangent[1], tangent[2]});
                            decoded.tangent = {transformed[0], transformed[1], transformed[2], tangent[3] < 0.0F ? -1.0F : 1.0F};
                        }
                        if (has_joints) {
                            const auto component_type = asset.accessors[joints_attribute->accessorIndex].componentType;
                            std::array<std::uint32_t, 4> joints{};
                            if (component_type == fastgltf::ComponentType::UnsignedByte) {
                                const auto source = fastgltf::getAccessorElement<fastgltf::math::u8vec4>(
                                    asset, asset.accessors[joints_attribute->accessorIndex], vertex);
                                for (std::size_t influence = 0; influence < 4U; ++influence)
                                    joints[influence] = source[influence];
                            } else {
                                const auto source = fastgltf::getAccessorElement<fastgltf::math::u16vec4>(
                                    asset, asset.accessors[joints_attribute->accessorIndex], vertex);
                                for (std::size_t influence = 0; influence < 4U; ++influence)
                                    joints[influence] = source[influence];
                            }
                            const auto weights = fastgltf::getAccessorElement<fastgltf::math::fvec4>(
                                asset, asset.accessors[weights_attribute->accessorIndex], vertex);
                            float weight_sum = 0.0F;
                            for (std::size_t influence = 0; influence < 4U; ++influence) {
                                const auto joint = static_cast<std::uint32_t>(joints[influence]);
                                const float weight = weights[influence];
                                if (joint >= result.skins[skin_index].joints.size() || !std::isfinite(weight) || weight < 0.0F)
                                    throw std::runtime_error("Skin vertex influence references an invalid joint or weight.");
                                decoded.joints[influence] = static_cast<std::uint16_t>(joint);
                                decoded.weights[influence] = weight;
                                weight_sum += weight;
                            }
                            if (weight_sum <= std::numeric_limits<float>::epsilon())
                                throw std::runtime_error("Skin vertex weights must have a positive sum.");
                            for (auto& weight : decoded.weights) weight /= weight_sum;
                        }
                        result.vertices.push_back(decoded);
                    }

                    const std::size_t index_begin = result.indices.size();
                    if (primitive.indicesAccessor.has_value()) {
                        const auto& index_accessor = asset.accessors[*primitive.indicesAccessor];
                        if (index_accessor.type != fastgltf::AccessorType::Scalar || !index_accessor.bufferViewIndex.has_value())
                            throw std::runtime_error("Primitive index accessor is unsupported.");
                        for (std::size_t index = 0; index < index_accessor.count; ++index) {
                            std::uint32_t value{};
                            switch (index_accessor.componentType) {
                            case fastgltf::ComponentType::UnsignedByte:
                                value = fastgltf::getAccessorElement<std::uint8_t>(asset, index_accessor, index); break;
                            case fastgltf::ComponentType::UnsignedShort:
                                value = fastgltf::getAccessorElement<std::uint16_t>(asset, index_accessor, index); break;
                            case fastgltf::ComponentType::UnsignedInt:
                                value = fastgltf::getAccessorElement<std::uint32_t>(asset, index_accessor, index); break;
                            default: throw std::runtime_error("Primitive index accessor component type is unsupported.");
                            }
                            if (value >= positions.count) throw std::runtime_error("Primitive index exceeds vertex count.");
                            result.indices.push_back(static_cast<std::uint32_t>(vertex_begin) + value);
                        }
                    } else {
                        for (std::size_t index = 0; index < positions.count; ++index)
                            result.indices.push_back(static_cast<std::uint32_t>(vertex_begin + index));
                    }
                    if (!has_normals) generate_normals(result.vertices, result.indices, vertex_begin, index_begin);
                    if (!has_tangents && has_texcoords) generate_tangents(result.vertices, result.indices, vertex_begin, index_begin);
                    GltfDecodedPrimitive decoded_primitive;
                    decoded_primitive.first_index = static_cast<std::uint32_t>(index_begin);
                    decoded_primitive.index_count = static_cast<std::uint32_t>(result.indices.size() - index_begin);
                    decoded_primitive.node_name = node.name.empty() ? "node-" + std::to_string(node_index) : std::string(node.name);
                    decoded_primitive.mesh_name = mesh.name.empty() ? "mesh-" + std::to_string(*node.meshIndex) : std::string(mesh.name);
                    decoded_primitive.skin = skinned ? static_cast<int>(skin_index) : -1;
                    fastgltf_decode_material(asset, primitive, result.images, decoded_primitive);
                    result.primitives.push_back(std::move(decoded_primitive));
                }
            }
            for (const auto child : node.children) visit(child, transform);
            active_nodes.erase(node_index);
        };
        for (const auto root : asset.scenes[scene_index].nodeIndices) visit(root, identity());
        if (result.vertices.empty() || result.indices.empty() || result.primitives.empty())
            throw std::runtime_error("Default scene has no supported triangle primitives.");
        compute_decoded_scene_bounds(result);
        result.valid = true;
        result.code = "ok";
        result.detail = "fastgltf scene graph, triangle payloads, materials, skins and animation channels decoded.";
    } catch (const std::exception& error) {
        result.code = "gltf.fastgltf-decode-failed";
        result.detail = error.what();
        result.vertices.clear();
        result.indices.clear();
        result.primitives.clear();
        result.skins.clear();
        result.animations.clear();
    }
    return result;
}

#endif

GltfMeshData decode_gltf_mesh(const std::filesystem::path& path) {
    std::string extension = path.extension().string();
    std::ranges::transform(extension, extension.begin(), [](const unsigned char value) {
        return static_cast<char>(std::tolower(value));
    });
    if (extension == ".glb") return decode_glb_mesh(path);

    GltfMeshData result;
    if (extension != ".gltf") {
        result.code = "gltf.unsupported-container";
        result.detail = "Only GLB 2 and JSON .gltf sources are supported.";
        return result;
    }
    const auto snapshot = read_gltf_source_snapshot(path);
    if (!snapshot.valid) {
        result.code = snapshot.code;
        result.detail = snapshot.detail;
        return result;
    }
#if NOEMANCER_HAS_FASTGLTF
    // fastgltf must never resolve authoring-tree dependencies directly. The
    // source document and every approved external URI are copied from the
    // engine-owned immutable snapshot into an isolated directory first.
    ScopedGltfSnapshotDirectory staging(snapshot);
    if (!staging.valid()) {
        result.code = staging.code();
        result.detail = staging.detail();
        return result;
    }
    return decode_gltf_mesh_fastgltf(snapshot.storage, staging.document_path(), false);
#else
    result.code = "gltf.fastgltf-unavailable";
    result.detail = "JSON glTF decoding requires the private fastgltf importer dependency.";
    return result;
#endif
}

GltfMeshData decode_glb_mesh(const std::filesystem::path& path) {
    GltfMeshData result;
    const auto container = read_glb_container(path);
    if (!container.valid) { result.code = container.code; result.detail = container.detail; return result; }
    if (!container.has_binary_chunk) {
        result.code = "gltf.missing-binary-chunk";
        result.detail = "GLB mesh decoding requires a BIN chunk.";
        return result;
    }
#if NOEMANCER_HAS_FASTGLTF
    // The validated container is the ownership boundary; fastgltf only receives
    // a private copy of those bytes and its third-party records never cross this
    // function's plain-data API.
    const auto fastgltf_result = decode_gltf_mesh_fastgltf(container.storage, path, true);
    if (fastgltf_result.valid) return fastgltf_result;
#endif
    try {
        const auto json_chunk = container.json_chunk();
        std::string json_text(reinterpret_cast<const char*>(json_chunk.data()), json_chunk.size());
        while (!json_text.empty() && json_text.back() == '\0') json_text.pop_back();
        const Json document = Json::parse(json_text);
        const auto binary = container.binary_chunk();
        const auto& nodes = document.at("nodes");
        const auto& meshes = document.at("meshes");
        const auto materials = document.value("materials", Json::array());
        const auto textures = document.value("textures", Json::array());
        const auto images = document.value("images", Json::array());
        const auto scenes = document.at("scenes");
        const std::size_t scene_index = document.value("scene", 0U);
        if (scene_index >= scenes.size()) throw std::runtime_error("Default scene index is out of range.");
        result.images.resize(images.size());
        for (std::size_t image_index = 0; image_index < images.size(); ++image_index) {
            const auto& image = images[image_index];
            auto& decoded_image = result.images[image_index];
            decoded_image.mime_type = image.value("mimeType", std::string{});
            if (decoded_image.mime_type != "image/png") {
                decoded_image.code = "image.unsupported-mime";
                continue;
            }
            if (!image.contains("bufferView")) {
                decoded_image.code = "image.external-uri-unsupported";
                continue;
            }
            const auto encoded = buffer_view_bytes(document, binary, image.at("bufferView").get<std::size_t>());
            const auto png = decode_png_rgba8(encoded);
            decoded_image.valid = png.valid; decoded_image.code = png.code;
            decoded_image.width = png.width; decoded_image.height = png.height; decoded_image.rgba8 = png.rgba8;
        }
        std::vector<int> node_parents(nodes.size(), -1);
        for (std::size_t parent = 0; parent < nodes.size(); ++parent) {
            for (const auto& child_json : nodes[parent].value("children", Json::array())) {
                const auto child = child_json.get<std::size_t>();
                if (child >= nodes.size() || node_parents[child] != -1)
                    throw std::runtime_error("Node hierarchy contains an invalid or multiply-parented child.");
                node_parents[child] = static_cast<int>(parent);
            }
        }
        const auto skins = document.value("skins", Json::array());
        result.skins.reserve(skins.size());
        for (std::size_t skin_index = 0; skin_index < skins.size(); ++skin_index) {
            const auto& skin = skins[skin_index];
            const auto joint_nodes = skin.value("joints", std::vector<std::uint32_t>{});
            if (joint_nodes.empty() || joint_nodes.size() > 64U)
                throw std::runtime_error("Skin joint count must be within the current GPU palette limit of 64.");
            std::unordered_map<std::uint32_t, std::size_t> joint_lookup;
            for (std::size_t joint = 0; joint < joint_nodes.size(); ++joint) {
                if (joint_nodes[joint] >= nodes.size() || !joint_lookup.emplace(joint_nodes[joint], joint).second)
                    throw std::runtime_error("Skin contains an invalid or duplicate joint node.");
            }
            AccessorView inverse_bind{};
            const bool has_inverse_bind = skin.contains("inverseBindMatrices");
            if (has_inverse_bind && (!make_accessor(document, binary, skin.at("inverseBindMatrices").get<std::size_t>(), inverse_bind) ||
                inverse_bind.component_type != 5126U || inverse_bind.components != 16U || inverse_bind.count != joint_nodes.size()))
                throw std::runtime_error("Skin inverse bind matrices must be a FLOAT MAT4 accessor matching joint count.");
            GltfDecodedSkin decoded_skin;
            decoded_skin.name = skin.value("name", "skin-" + std::to_string(skin_index));
            decoded_skin.joints.reserve(joint_nodes.size());
            for (std::size_t joint = 0; joint < joint_nodes.size(); ++joint) {
                const auto node_index = joint_nodes[joint];
                GltfDecodedJoint decoded_joint;
                decoded_joint.node_index = node_index;
                decoded_joint.name = nodes[node_index].value("name", "joint-" + std::to_string(joint));
                const auto local = node_transform(nodes[node_index]);
                decoded_joint.local_transform = local.value;
                decoded_joint.inverse_bind_matrix = identity().value;
                if (has_inverse_bind) for (std::size_t component = 0; component < 16U; ++component)
                    decoded_joint.inverse_bind_matrix[component] = read_float_component(inverse_bind, joint, component);
                int ancestor = node_parents[node_index];
                while (ancestor >= 0) {
                    const auto found = joint_lookup.find(static_cast<std::uint32_t>(ancestor));
                    if (found != joint_lookup.end()) { decoded_joint.parent_joint = static_cast<int>(found->second); break; }
                    ancestor = node_parents[static_cast<std::size_t>(ancestor)];
                }
                decoded_skin.joints.push_back(std::move(decoded_joint));
            }
            result.skins.push_back(std::move(decoded_skin));
        }
        const auto animations = document.value("animations", Json::array());
        result.animations.reserve(animations.size());
        for (std::size_t animation_index = 0; animation_index < animations.size(); ++animation_index) {
            const auto& animation = animations[animation_index];
            const auto samplers = animation.value("samplers", Json::array());
            GltfAnimationClip clip;
            clip.name = animation.value("name", "animation-" + std::to_string(animation_index));
            for (const auto& channel_json : animation.value("channels", Json::array())) {
                const auto sampler_index = channel_json.value("sampler", samplers.size());
                if (sampler_index >= samplers.size() || !channel_json.contains("target"))
                    throw std::runtime_error("Animation channel references an invalid sampler or target.");
                const auto& sampler = samplers[sampler_index];
                const auto& target = channel_json.at("target");
                const auto node_index = target.value("node", nodes.size());
                const auto channel_path = target.value("path", std::string{});
                if (node_index >= nodes.size() || (channel_path != "translation" && channel_path != "rotation" && channel_path != "scale"))
                    throw std::runtime_error("Only joint translation, rotation and scale animation channels are supported.");
                const auto interpolation = sampler.value("interpolation", std::string("LINEAR"));
                if (interpolation != "LINEAR" && interpolation != "STEP")
                    throw std::runtime_error("Animation interpolation must currently be LINEAR or STEP.");
                AccessorView input_view{}; AccessorView output_view{};
                if (!sampler.contains("input") || !sampler.contains("output") ||
                    !make_accessor(document, binary, sampler.at("input").get<std::size_t>(), input_view) ||
                    !make_accessor(document, binary, sampler.at("output").get<std::size_t>(), output_view) ||
                    input_view.component_type != 5126U || input_view.components != 1U ||
                    output_view.component_type != 5126U || output_view.count != input_view.count ||
                    output_view.components != (channel_path == "rotation" ? 4U : 3U) || input_view.count == 0U)
                    throw std::runtime_error("Animation sampler accessors are invalid or mismatched.");
                GltfAnimationChannel channel;
                channel.node_index = static_cast<std::uint32_t>(node_index);
                channel.path = channel_path;
                channel.interpolation = interpolation;
                channel.times.reserve(input_view.count); channel.values.reserve(input_view.count);
                float previous = -1.0F;
                for (std::size_t key = 0; key < input_view.count; ++key) {
                    const float time = read_float_component(input_view, key, 0U);
                    if (!std::isfinite(time) || time < 0.0F || time <= previous)
                        throw std::runtime_error("Animation key times must be finite, non-negative and strictly increasing.");
                    previous = time; channel.times.push_back(time); clip.duration = std::max(clip.duration, time);
                    std::array<float, 4> value{0.0F, 0.0F, 0.0F, channel_path == "rotation" ? 1.0F : 0.0F};
                    for (std::size_t component = 0; component < output_view.components; ++component)
                        value[component] = read_float_component(output_view, key, component);
                    if (!std::ranges::all_of(value, [](const float component) { return std::isfinite(component); }))
                        throw std::runtime_error("Animation channel contains a non-finite value.");
                    channel.values.push_back(value);
                }
                clip.channels.push_back(std::move(channel));
            }
            if (clip.channels.empty() || clip.duration <= 0.0F)
                throw std::runtime_error("Animation clip has no supported channels or positive duration.");
            result.animations.push_back(std::move(clip));
        }
        std::unordered_set<std::size_t> active_nodes;
        std::function<void(std::size_t, const Mat4&)> visit;
        visit = [&](const std::size_t node_index, const Mat4& parent) {
            if (node_index >= nodes.size() || active_nodes.contains(node_index)) throw std::runtime_error("Node graph is invalid or cyclic.");
            active_nodes.insert(node_index);
            const auto& node = nodes[node_index];
            const Mat4 transform = multiply(parent, node_transform(node));
            if (node.contains("mesh")) {
                const bool skinned = node.contains("skin");
                const auto skin_index = skinned ? node.at("skin").get<std::size_t>() : result.skins.size();
                if (skinned && skin_index >= result.skins.size()) throw std::runtime_error("Mesh node references an invalid skin.");
                const auto mesh_index = node.at("mesh").get<std::size_t>();
                if (mesh_index >= meshes.size()) throw std::runtime_error("Mesh index is out of range.");
                const auto& mesh = meshes[mesh_index];
                for (const auto& primitive : mesh.at("primitives")) {
                    if (primitive.value("mode", 4U) != 4U) continue;
                    const auto& attributes = primitive.at("attributes");
                    AccessorView positions{};
                    if (!attributes.contains("POSITION") || !make_accessor(document, binary, attributes.at("POSITION").get<std::size_t>(), positions) || positions.components != 3U)
                        throw std::runtime_error("Primitive POSITION accessor is unsupported.");
                    AccessorView normals{}; AccessorView texcoords{}; AccessorView tangents{}; AccessorView joints{}; AccessorView weights{};
                    const bool has_normals = attributes.contains("NORMAL") && make_accessor(document, binary, attributes.at("NORMAL").get<std::size_t>(), normals) && normals.components == 3U && normals.count == positions.count;
                    const bool has_texcoords = attributes.contains("TEXCOORD_0") && make_accessor(document, binary, attributes.at("TEXCOORD_0").get<std::size_t>(), texcoords) && texcoords.components == 2U && texcoords.count == positions.count;
                    const bool has_tangents = attributes.contains("TANGENT") && make_accessor(document, binary, attributes.at("TANGENT").get<std::size_t>(), tangents) &&
                        tangents.component_type == 5126U && tangents.components == 4U && tangents.count == positions.count;
                    const bool has_joints = attributes.contains("JOINTS_0") && make_accessor(document, binary, attributes.at("JOINTS_0").get<std::size_t>(), joints) &&
                        joints.components == 4U && joints.count == positions.count && (joints.component_type == 5121U || joints.component_type == 5123U);
                    const bool has_weights = attributes.contains("WEIGHTS_0") && make_accessor(document, binary, attributes.at("WEIGHTS_0").get<std::size_t>(), weights) &&
                        weights.components == 4U && weights.count == positions.count &&
                        (weights.component_type == 5126U || ((weights.component_type == 5121U || weights.component_type == 5123U) && weights.normalized));
                    if (skinned && (!has_joints || !has_weights))
                        throw std::runtime_error("Skinned primitive requires matching JOINTS_0 and WEIGHTS_0 accessors.");
                    if (has_joints != has_weights)
                        throw std::runtime_error("JOINTS_0 and WEIGHTS_0 must be provided together.");
                    if (has_joints && !skinned)
                        throw std::runtime_error("Vertex skin influences require a skin on the mesh node.");
                    const std::size_t vertex_begin = result.vertices.size();
                    for (std::size_t vertex = 0; vertex < positions.count; ++vertex) {
                        GltfDecodedVertex decoded;
                        const Mat4 vertex_transform = skinned ? identity() : transform;
                        decoded.position = transform_point(vertex_transform, {read_float_component(positions, vertex, 0), read_float_component(positions, vertex, 1), read_float_component(positions, vertex, 2)});
                        if (has_normals) decoded.normal = transform_normal(vertex_transform, {read_float_component(normals, vertex, 0), read_float_component(normals, vertex, 1), read_float_component(normals, vertex, 2)});
                        if (has_texcoords) decoded.texcoord = {read_float_component(texcoords, vertex, 0), read_float_component(texcoords, vertex, 1)};
                        if (has_tangents) {
                            const auto tangent = transform_normal(vertex_transform,
                                {read_float_component(tangents, vertex, 0), read_float_component(tangents, vertex, 1), read_float_component(tangents, vertex, 2)});
                            decoded.tangent = {tangent[0], tangent[1], tangent[2], read_float_component(tangents, vertex, 3) < 0.0F ? -1.0F : 1.0F};
                        }
                        if (has_joints) {
                            float weight_sum = 0.0F;
                            for (std::size_t influence = 0; influence < 4U; ++influence) {
                                const auto joint = read_unsigned_component(joints, vertex, influence);
                                const float weight = read_float_component(weights, vertex, influence);
                                if (joint >= result.skins[skin_index].joints.size() || !std::isfinite(weight) || weight < 0.0F)
                                    throw std::runtime_error("Skin vertex influence references an invalid joint or weight.");
                                decoded.joints[influence] = static_cast<std::uint16_t>(joint);
                                decoded.weights[influence] = weight; weight_sum += weight;
                            }
                            if (weight_sum <= std::numeric_limits<float>::epsilon())
                                throw std::runtime_error("Skin vertex weights must have a positive sum.");
                            for (auto& weight : decoded.weights) weight /= weight_sum;
                        }
                        result.vertices.push_back(decoded);
                    }
                    const std::size_t index_begin = result.indices.size();
                    if (primitive.contains("indices")) {
                        AccessorView accessor{};
                        if (!make_accessor(document, binary, primitive.at("indices").get<std::size_t>(), accessor) || accessor.components != 1U)
                            throw std::runtime_error("Primitive index accessor is unsupported.");
                        for (std::size_t index = 0; index < accessor.count; ++index) {
                            const auto value = read_index(accessor, index);
                            if (value >= positions.count) throw std::runtime_error("Primitive index exceeds vertex count.");
                            result.indices.push_back(static_cast<std::uint32_t>(vertex_begin) + value);
                        }
                    } else {
                        for (std::size_t index = 0; index < positions.count; ++index) result.indices.push_back(static_cast<std::uint32_t>(vertex_begin + index));
                    }
                    if (!has_normals) generate_normals(result.vertices, result.indices, vertex_begin, index_begin);
                    if (!has_tangents && has_texcoords) generate_tangents(result.vertices, result.indices, vertex_begin, index_begin);
                    GltfDecodedPrimitive decoded_primitive;
                    decoded_primitive.first_index = static_cast<std::uint32_t>(index_begin);
                    decoded_primitive.index_count = static_cast<std::uint32_t>(result.indices.size() - index_begin);
                    decoded_primitive.node_name = node.value("name", "node-" + std::to_string(node_index));
                    decoded_primitive.mesh_name = mesh.value("name", "mesh-" + std::to_string(mesh_index));
                    decoded_primitive.skin = skinned ? static_cast<int>(skin_index) : -1;
                    if (primitive.contains("material")) {
                        const auto material_index = primitive.at("material").get<std::size_t>();
                        if (material_index < materials.size()) {
                            const auto& material = materials[material_index];
                            const auto pbr = material.value("pbrMetallicRoughness", Json::object());
                            const auto color = pbr.value("baseColorFactor", std::vector<float>{1.0F, 1.0F, 1.0F, 1.0F});
                            if (color.size() == 4U) std::copy_n(color.begin(), 4U, decoded_primitive.base_color.begin());
                            decoded_primitive.metallic = pbr.value("metallicFactor", 1.0F);
                            decoded_primitive.roughness = pbr.value("roughnessFactor", 1.0F);
                            decoded_primitive.unlit = material.value("extensions", Json::object()).contains("KHR_materials_unlit");
                            decoded_primitive.alpha_mode = material.value("alphaMode", std::string("OPAQUE"));
                            if (decoded_primitive.alpha_mode != "OPAQUE" && decoded_primitive.alpha_mode != "MASK" && decoded_primitive.alpha_mode != "BLEND")
                                throw std::runtime_error("Material alphaMode is invalid.");
                            decoded_primitive.alpha_cutoff = material.value("alphaCutoff", 0.5F);
                            decoded_primitive.double_sided = material.value("doubleSided", false);
                            const auto emissive = material.value("emissiveFactor", std::vector<float>{0.0F, 0.0F, 0.0F});
                            if (emissive.size() == 3U) std::copy_n(emissive.begin(), 3U, decoded_primitive.emissive_factor.begin());
                            const auto texture_image = [&](const Json& texture_info) -> int {
                                if (texture_info.value("texCoord", 0U) != 0U) return -1;
                                const auto texture_index = texture_info.value("index", textures.size());
                                if (texture_index >= textures.size()) return -1;
                                const auto image_index = textures[texture_index].value("source", images.size());
                                return image_index < result.images.size() && result.images[image_index].valid ? static_cast<int>(image_index) : -1;
                            };
                            if (pbr.contains("baseColorTexture")) {
                                decoded_primitive.base_color_image = texture_image(pbr.at("baseColorTexture"));
                            }
                            if (pbr.contains("metallicRoughnessTexture"))
                                decoded_primitive.metallic_roughness_image = texture_image(pbr.at("metallicRoughnessTexture"));
                            if (material.contains("normalTexture")) {
                                decoded_primitive.normal_image = texture_image(material.at("normalTexture"));
                                decoded_primitive.normal_scale = material.at("normalTexture").value("scale", 1.0F);
                            }
                            if (material.contains("occlusionTexture")) {
                                decoded_primitive.occlusion_image = texture_image(material.at("occlusionTexture"));
                                decoded_primitive.occlusion_strength = material.at("occlusionTexture").value("strength", 1.0F);
                            }
                            if (material.contains("emissiveTexture"))
                                decoded_primitive.emissive_image = texture_image(material.at("emissiveTexture"));
                        }
                    }
                    result.primitives.push_back(std::move(decoded_primitive));
                }
            }
            for (const auto& child : node.value("children", Json::array())) visit(child.get<std::size_t>(), transform);
            active_nodes.erase(node_index);
        };
        for (const auto& root : scenes[scene_index].value("nodes", Json::array())) visit(root.get<std::size_t>(), identity());
        if (result.vertices.empty() || result.indices.empty() || result.primitives.empty()) throw std::runtime_error("Default scene has no supported triangle primitives.");
        compute_decoded_scene_bounds(result);
        result.valid = true; result.code = "ok";
        result.detail = "GLB scene graph, triangle payloads, materials, skins and animation channels decoded.";
    } catch (const std::exception& error) {
        result.code = "gltf.decode-failed"; result.detail = error.what();
        result.vertices.clear(); result.indices.clear(); result.primitives.clear(); result.skins.clear(); result.animations.clear();
    }
    return result;
}

} // namespace noemancer
