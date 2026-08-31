#include "runtime/native_d3d12_raytracing_context.hpp"

#include <algorithm>
#include <array>
#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <ranges>
#include <span>
#include <string>
#include <utility>

#if defined(_WIN32)
#  define NOMINMAX
#  include <windows.h>
#  include <d3d12.h>
#  include <dxgi1_6.h>
#  include <wrl/client.h>
#endif

namespace noemancer {
namespace {

std::string bounded_text(const std::string_view value) {
    return std::string(value.substr(0U, native_d3d12_raytracing_context_max_text_bytes));
}

bool valid_identifier(const std::string_view value) {
    if (value.empty() || value.size() > native_d3d12_raytracing_context_max_text_bytes)
        return false;
    for (const auto byte : value) {
        const auto character = static_cast<unsigned char>(byte);
        if (character < 0x20U || character == 0x7fU || character == '/' || character == '\\')
            return false;
    }
    return true;
}

std::uint64_t hash_bytes(std::uint64_t hash, const void* data, const std::size_t size) noexcept {
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    for (std::size_t index = 0U; index < size; ++index) {
        hash ^= bytes[index];
        hash *= 1099511628211ULL;
    }
    return hash;
}

std::uint64_t hash_string(std::uint64_t hash, const std::string_view value) noexcept {
    hash = hash_bytes(hash, value.data(), value.size());
    const std::uint8_t separator = 0xffU;
    return hash_bytes(hash, &separator, sizeof(separator));
}

std::uint64_t scene_signature(const NativeD3D12RayTracingScene& scene) noexcept {
    std::uint64_t hash = 14695981039346656037ULL;
    hash = hash_string(hash, scene.scene_id);
    hash = hash_bytes(hash, &scene.revision, sizeof(scene.revision));
    hash = hash_bytes(hash, &scene.allow_update, sizeof(scene.allow_update));
    for (const auto& geometry : scene.geometries) {
        hash = hash_string(hash, geometry.geometry_id);
        hash = hash_bytes(hash, &geometry.allow_update, sizeof(geometry.allow_update));
        if (!geometry.position_xyz.empty())
            hash = hash_bytes(hash, geometry.position_xyz.data(),
                              geometry.position_xyz.size() * sizeof(float));
        if (!geometry.indices.empty())
            hash = hash_bytes(hash, geometry.indices.data(),
                              geometry.indices.size() * sizeof(std::uint32_t));
    }
    return hash == 0U ? 1U : hash;
}

// Resource identity is deliberately separated from content identity.  A
// revision or vertex edit can reuse persistent BLAS/TLAS storage when the
// geometry topology and update flags remain compatible; adding/removing a
// geometry or changing its vertex/index cardinality requires a rebuild.
std::uint64_t scene_topology_signature(const NativeD3D12RayTracingScene& scene) noexcept {
    std::uint64_t hash = 14695981039346656037ULL;
    hash = hash_string(hash, scene.scene_id);
    hash = hash_bytes(hash, &scene.allow_update, sizeof(scene.allow_update));
    for (const auto& geometry : scene.geometries) {
        hash = hash_string(hash, geometry.geometry_id);
        hash = hash_bytes(hash, &geometry.allow_update, sizeof(geometry.allow_update));
        const auto vertex_count = static_cast<std::uint64_t>(geometry.position_xyz.size() / 3U);
        const auto index_count = static_cast<std::uint64_t>(geometry.indices.size());
        hash = hash_bytes(hash, &vertex_count, sizeof(vertex_count));
        hash = hash_bytes(hash, &index_count, sizeof(index_count));
        // D3D12 AS update preserves the primitive topology.  Treat an index
        // stream edit as a rebuild boundary while allowing vertex position
        // edits to use the cheaper in-place update path.
        if (!geometry.indices.empty())
            hash = hash_bytes(hash, geometry.indices.data(),
                              geometry.indices.size() * sizeof(std::uint32_t));
    }
    return hash == 0U ? 1U : hash;
}

struct SceneValidation final {
    bool valid{};
    std::string code;
    std::string detail;
};

SceneValidation validate_scene(const NativeD3D12RayTracingScene& scene,
                               const NativeD3D12RayTracingContextOptions& options) {
    if (!valid_identifier(scene.scene_id))
        return {false, "native-d3d12.context.scene-id-invalid",
                "Scene ID must be non-empty, bounded and path-safe."};
    if (scene.geometries.empty())
        return {false, "native-d3d12.context.scene-empty",
                "A ray-tracing scene requires at least one triangle geometry."};
    const auto maximum_geometry_count = std::min<std::size_t>(
        options.max_geometry_count == 0U ? native_d3d12_raytracing_context_max_geometry_count
                                         : options.max_geometry_count,
        native_d3d12_raytracing_context_max_geometry_count);
    if (scene.geometries.size() > maximum_geometry_count)
        return {false, "native-d3d12.context.geometry-count-exceeded",
                "The scene geometry count exceeds the bounded context contract."};

    std::uint64_t total_bytes = 0U;
    std::string previous_id;
    bool first = true;
    for (const auto& geometry : scene.geometries) {
        if (!valid_identifier(geometry.geometry_id))
            return {false, "native-d3d12.context.geometry-id-invalid",
                    "Geometry IDs must be non-empty, bounded and path-safe."};
        if (!first && previous_id == geometry.geometry_id)
            return {false, "native-d3d12.context.geometry-id-duplicate",
                    "Geometry IDs must be unique within one scene."};
        previous_id = geometry.geometry_id;
        first = false;
        if (geometry.position_xyz.size() < 9U || geometry.position_xyz.size() % 3U != 0U)
            return {false, "native-d3d12.context.vertex-layout-invalid",
                    "Position input must contain at least one triangle of XYZ floats."};
        if (geometry.indices.size() < 3U || geometry.indices.size() % 3U != 0U)
            return {false, "native-d3d12.context.index-layout-invalid",
                    "Index input must contain at least one triangle of 32-bit indices."};
        const auto vertex_count = geometry.position_xyz.size() / 3U;
        if (vertex_count > native_d3d12_raytracing_context_max_vertex_count ||
            geometry.indices.size() > native_d3d12_raytracing_context_max_index_count)
            return {false, "native-d3d12.context.geometry-size-exceeded",
                    "A geometry exceeds the bounded vertex or index contract."};
        for (const auto coordinate : geometry.position_xyz) {
            if (!std::isfinite(coordinate))
                return {false, "native-d3d12.context.vertex-non-finite",
                        "Position input must contain finite coordinates."};
        }
        for (const auto index : geometry.indices) {
            if (index >= vertex_count)
                return {false, "native-d3d12.context.index-out-of-range",
                        "A triangle index refers to a vertex outside the position buffer."};
        }
        const auto vertex_bytes = static_cast<std::uint64_t>(geometry.position_xyz.size()) * sizeof(float);
        const auto index_bytes = static_cast<std::uint64_t>(geometry.indices.size()) * sizeof(std::uint32_t);
        if (vertex_bytes > std::numeric_limits<std::uint64_t>::max() - total_bytes)
            return {false, "native-d3d12.context.resource-size-overflow",
                    "Scene resource byte accounting overflowed."};
        total_bytes += vertex_bytes;
        if (index_bytes > std::numeric_limits<std::uint64_t>::max() - total_bytes)
            return {false, "native-d3d12.context.resource-size-overflow",
                    "Scene resource byte accounting overflowed."};
        total_bytes += index_bytes;
    }
    const auto maximum_resource_bytes = options.max_resource_bytes == 0U
        ? native_d3d12_raytracing_context_max_resource_bytes
        : std::min(options.max_resource_bytes, native_d3d12_raytracing_context_max_resource_bytes);
    if (total_bytes == 0U || total_bytes > maximum_resource_bytes)
        return {false, "native-d3d12.context.resource-budget-exceeded",
                "Scene geometry exceeds the bounded upload resource budget."};
    return {true, {}, {}};
}

void sort_scene(NativeD3D12RayTracingScene& scene) {
    scene.revision = scene.revision == 0U ? 1U : scene.revision;
    std::sort(scene.geometries.begin(), scene.geometries.end(),
              [](const auto& left, const auto& right) {
                  if (left.geometry_id != right.geometry_id)
                      return left.geometry_id < right.geometry_id;
                  if (left.allow_update != right.allow_update)
                      return left.allow_update < right.allow_update;
                  if (left.position_xyz != right.position_xyz)
                      return left.position_xyz < right.position_xyz;
                  return left.indices < right.indices;
              });
}

struct CameraValidation final {
    bool valid{};
    std::string code;
    std::string detail;
};

float camera_dot(const std::array<float, 3U>& left,
                 const std::array<float, 3U>& right) noexcept {
    return left[0U] * right[0U] + left[1U] * right[1U] + left[2U] * right[2U];
}

std::array<float, 3U> camera_cross(const std::array<float, 3U>& left,
                                   const std::array<float, 3U>& right) noexcept {
    return {
        left[1U] * right[2U] - left[2U] * right[1U],
        left[2U] * right[0U] - left[0U] * right[2U],
        left[0U] * right[1U] - left[1U] * right[0U],
    };
}

bool camera_vector_finite_bounded(const std::array<float, 3U>& value,
                                  const float maximum) noexcept {
    return std::all_of(value.begin(), value.end(), [maximum](const float component) {
        return std::isfinite(component) && std::abs(component) <= maximum;
    });
}

CameraValidation validate_camera(const NativeD3D12RayTracingCamera& camera) {
    constexpr float maximum_coordinate = 1.0e9F;
    constexpr float minimum_lens_value = 1.0e-6F;
    constexpr float maximum_aspect_ratio = 16.0F;
    constexpr float maximum_fov_tan_half = 100.0F;
    constexpr float basis_length_tolerance = 0.05F;
    constexpr float basis_orthogonality_tolerance = 0.05F;
    constexpr float basis_handedness_minimum = 0.90F;

    if (!camera_vector_finite_bounded(camera.position, maximum_coordinate) ||
        !camera_vector_finite_bounded(camera.right, maximum_coordinate) ||
        !camera_vector_finite_bounded(camera.up, maximum_coordinate) ||
        !camera_vector_finite_bounded(camera.forward, maximum_coordinate)) {
        return {false, "native-d3d12.context.camera-non-finite",
                "Camera position and basis components must be finite and bounded."};
    }
    if (!std::isfinite(camera.vertical_fov_tan_half) ||
        camera.vertical_fov_tan_half < minimum_lens_value ||
        camera.vertical_fov_tan_half > maximum_fov_tan_half ||
        !std::isfinite(camera.aspect_ratio) || camera.aspect_ratio < minimum_lens_value ||
        camera.aspect_ratio > maximum_aspect_ratio || !std::isfinite(camera.near_plane) ||
        !std::isfinite(camera.far_plane) || camera.near_plane < minimum_lens_value ||
        camera.far_plane <= camera.near_plane || camera.far_plane > maximum_coordinate) {
        return {false, "native-d3d12.context.camera-lens-invalid",
                "Camera tan-half-FOV, aspect ratio and near/far planes are out of bounds."};
    }
    const auto right_length = std::sqrt(camera_dot(camera.right, camera.right));
    const auto up_length = std::sqrt(camera_dot(camera.up, camera.up));
    const auto forward_length = std::sqrt(camera_dot(camera.forward, camera.forward));
    if (!std::isfinite(right_length) || !std::isfinite(up_length) ||
        !std::isfinite(forward_length) ||
        std::abs(right_length - 1.0F) > basis_length_tolerance ||
        std::abs(up_length - 1.0F) > basis_length_tolerance ||
        std::abs(forward_length - 1.0F) > basis_length_tolerance ||
        std::abs(camera_dot(camera.right, camera.up)) > basis_orthogonality_tolerance ||
        std::abs(camera_dot(camera.right, camera.forward)) > basis_orthogonality_tolerance ||
        std::abs(camera_dot(camera.up, camera.forward)) > basis_orthogonality_tolerance) {
        return {false, "native-d3d12.context.camera-basis-invalid",
                "Camera right/up/forward must be an approximately orthonormal basis."};
    }
    const auto expected_forward = camera_cross(camera.right, camera.up);
    if (camera_dot(expected_forward, camera.forward) < basis_handedness_minimum) {
        return {false, "native-d3d12.context.camera-handedness-invalid",
                "Camera basis must be right-handed: right cross up must point along forward."};
    }
    return {true, {}, {}};
}

std::uint64_t camera_signature(const NativeD3D12RayTracingCamera& camera) noexcept {
    std::uint64_t hash = 14695981039346656037ULL;
    hash = hash_bytes(hash, camera.position.data(), sizeof(camera.position));
    hash = hash_bytes(hash, camera.right.data(), sizeof(camera.right));
    hash = hash_bytes(hash, camera.up.data(), sizeof(camera.up));
    hash = hash_bytes(hash, camera.forward.data(), sizeof(camera.forward));
    hash = hash_bytes(hash, &camera.vertical_fov_tan_half,
                      sizeof(camera.vertical_fov_tan_half));
    hash = hash_bytes(hash, &camera.aspect_ratio, sizeof(camera.aspect_ratio));
    hash = hash_bytes(hash, &camera.near_plane, sizeof(camera.near_plane));
    hash = hash_bytes(hash, &camera.far_plane, sizeof(camera.far_plane));
    return hash == 0U ? 1U : hash;
}

struct ShadingValidation final {
    bool valid{};
    std::string code;
    std::string detail;
};

bool finite_bounded_array(const std::span<const float> values,
                          const float minimum,
                          const float maximum) noexcept {
    return std::all_of(values.begin(), values.end(), [minimum, maximum](const float value) {
        return std::isfinite(value) && value >= minimum && value <= maximum;
    });
}

ShadingValidation validate_lighting(const NativeD3D12RayTracingLighting& lighting) {
    constexpr float maximum_channel = 64.0F;
    constexpr float maximum_coordinate = 1.0e6F;
    constexpr float minimum_intensity = 0.0F;
    if (!finite_bounded_array(lighting.directional_direction, -maximum_coordinate,
                              maximum_coordinate) ||
        camera_dot(lighting.directional_direction, lighting.directional_direction) <
            1.0e-8F) {
        return {false, "native-d3d12.context.shading-direction-invalid",
                "The directional light vector must be finite and non-zero."};
    }
    if (!finite_bounded_array(lighting.directional_color, minimum_intensity,
                              maximum_channel) ||
        !std::isfinite(lighting.directional_intensity) ||
        lighting.directional_intensity < minimum_intensity ||
        lighting.directional_intensity > maximum_channel ||
        !finite_bounded_array(lighting.ambient_color, minimum_intensity, maximum_channel) ||
        !std::isfinite(lighting.ambient_intensity) ||
        lighting.ambient_intensity < minimum_intensity ||
        lighting.ambient_intensity > maximum_channel) {
        return {false, "native-d3d12.context.shading-light-invalid",
                "Directional and ambient light channels/intensities must be finite and bounded."};
    }
    return {true, {}, {}};
}

ShadingValidation validate_material(const NativeD3D12RayTracingMaterial& material) {
    constexpr float maximum_channel = 64.0F;
    if (!valid_identifier(material.geometry_id)) {
        return {false, "native-d3d12.context.material-id-invalid",
                "Material geometry_id must identify one path-safe scene geometry."};
    }
    if (!finite_bounded_array(material.base_color, 0.0F, maximum_channel) ||
        material.base_color[3U] > 1.0F || !std::isfinite(material.metallic) ||
        material.metallic < 0.0F || material.metallic > 1.0F ||
        !std::isfinite(material.roughness) || material.roughness < 0.02F ||
        material.roughness > 1.0F ||
        !finite_bounded_array(material.emissive, 0.0F, maximum_channel) ||
        !std::isfinite(material.emissive_intensity) ||
        material.emissive_intensity < 0.0F || material.emissive_intensity > maximum_channel) {
        return {false, "native-d3d12.context.material-values-invalid",
                "Material base color, metallic, roughness and emissive values are out of bounds."};
    }
    return {true, {}, {}};
}

NativeD3D12RayTracingMaterial default_material(std::string geometry_id) {
    NativeD3D12RayTracingMaterial material;
    material.geometry_id = std::move(geometry_id);
    return material;
}

ShadingValidation resolve_materials(
    const NativeD3D12RayTracingScene& scene,
    const NativeD3D12RayTracingContextOptions& options,
    std::vector<NativeD3D12RayTracingMaterial>& resolved) {
    const auto lighting_validation = validate_lighting(options.lighting);
    if (!lighting_validation.valid) return lighting_validation;
    resolved.clear();
    resolved.reserve(scene.geometries.size());
    if (options.materials.empty()) {
        for (const auto& geometry : scene.geometries)
            resolved.push_back(default_material(geometry.geometry_id));
        return {true, {}, {}};
    }
    if (options.materials.size() != scene.geometries.size()) {
        return {false, "native-d3d12.context.material-count-invalid",
                "A non-empty material list must contain exactly one entry per scene geometry."};
    }
    for (const auto& material : options.materials) {
        const auto validation = validate_material(material);
        if (!validation.valid) return validation;
        const auto duplicate = std::count_if(
            options.materials.begin(), options.materials.end(),
            [&material](const auto& candidate) {
                return candidate.geometry_id == material.geometry_id;
            });
        if (duplicate != 1) {
            return {false, "native-d3d12.context.material-id-duplicate",
                    "Material geometry_id values must be unique within one scene."};
        }
    }
    for (const auto& geometry : scene.geometries) {
        const auto found = std::find_if(
            options.materials.begin(), options.materials.end(),
            [&geometry](const auto& material) {
                return material.geometry_id == geometry.geometry_id;
            });
        if (found == options.materials.end()) {
            return {false, "native-d3d12.context.material-geometry-missing",
                    "Every scene geometry must have one matching material entry."};
        }
        resolved.push_back(*found);
    }
    for (const auto& material : options.materials) {
        const auto known_geometry = std::find_if(
            scene.geometries.begin(), scene.geometries.end(),
            [&material](const auto& geometry) {
                return geometry.geometry_id == material.geometry_id;
            });
        if (known_geometry == scene.geometries.end()) {
            return {false, "native-d3d12.context.material-geometry-unknown",
                    "A material entry refers to a geometry outside the retained scene."};
        }
    }
    return {true, {}, {}};
}

std::uint64_t shading_signature(
    const std::vector<NativeD3D12RayTracingMaterial>& materials,
    const NativeD3D12RayTracingLighting& lighting) noexcept {
    std::uint64_t hash = 14695981039346656037ULL;
    for (const auto& material : materials) {
        hash = hash_string(hash, material.geometry_id);
        hash = hash_bytes(hash, material.base_color.data(), sizeof(material.base_color));
        hash = hash_bytes(hash, &material.metallic, sizeof(material.metallic));
        hash = hash_bytes(hash, &material.roughness, sizeof(material.roughness));
        hash = hash_bytes(hash, material.emissive.data(), sizeof(material.emissive));
        hash = hash_bytes(hash, &material.emissive_intensity,
                          sizeof(material.emissive_intensity));
    }
    hash = hash_bytes(hash, lighting.directional_direction.data(),
                      sizeof(lighting.directional_direction));
    hash = hash_bytes(hash, lighting.directional_color.data(),
                      sizeof(lighting.directional_color));
    hash = hash_bytes(hash, &lighting.directional_intensity,
                      sizeof(lighting.directional_intensity));
    hash = hash_bytes(hash, lighting.ambient_color.data(),
                      sizeof(lighting.ambient_color));
    hash = hash_bytes(hash, &lighting.ambient_intensity,
                      sizeof(lighting.ambient_intensity));
    return hash == 0U ? 1U : hash;
}

#if defined(_WIN32)

using Microsoft::WRL::ComPtr;
using CreateFactoryFn = HRESULT(WINAPI*)(UINT, REFIID, void**);
using CreateDeviceFn = HRESULT(WINAPI*)(IUnknown*, D3D_FEATURE_LEVEL, REFIID, void**);
using SerializeRootSignatureFn = HRESULT(WINAPI*)(
    const D3D12_ROOT_SIGNATURE_DESC*, D3D_ROOT_SIGNATURE_VERSION, ID3DBlob**, ID3DBlob**);

// This is the same pinned lib_6_3 DXIL probe used by the short-lived executor.
// Keeping one known-good artifact here makes the persistent context usable
// without a machine-local compiler or a build-directory dependency.  It writes
// a deterministic 16-byte UAV result and is deliberately not an RTGI shader.
constexpr std::string_view kDxrProbeDxilBase64 =
    "RFhCQ/QoGcU0CaFax3Iw2oDaZrABAAAAMBUAAAYAAAA4AAAASAAAAHQAAABEAgAA4AoAAPwKAABTRkkwCAAAAAAAAAAAAAAAVkVSUyQAAAABAAkAAAAAABoVAAAUAAAA"
    "MGQzZWU2YjUAMS45LjAuNTQwMgBSREFUyAEAABAAAAAEAAAAGAAAALgAAAAIAQAAtAEAAAEAAACYAAAAAGdTY2VuZQBnT3V0cHV0AAE/UmF5R2VuQEBZQVhYWgBSYXlH"
    "ZW4AAT9NaXNzQEBZQVhVUGF5bG9hZEBAQFoATWlzcwABP0Nsb3Nlc3RIaXRAQFlBWFVQYXlsb2FkQEBVQnVpbHRJblRyaWFuZ2xlSW50ZXJzZWN0aW9uQXR0cmlidXRl"
    "c0BAQFoAQ2xvc2VzdEhpdAAAAAADAAAASAAAAAIAAAAgAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAABAAAACwAAAAAAAAAAAAAAAAAAAAAAAAAIAAAA"
    "AAAAAAQAAACkAAAAAwAAADQAAAAQAAAAIAAAAAAAAAD/////BwAAAAAAAAAAAAAAAAAAAAAAAACAAAAAYwAHAAAAAAD/////JwAAAD8AAAD//////////wsAAAAEAAAA"
    "AAAAAAAAAAAAAAAAAAgAAGMACwAAAAAA/////0QAAACKAAAA//////////8KAAAABAAAAAgAAAAAAAAAAAAAAAAEAABjAAoAAAAAAP////8CAAAADAAAAAIAAAAAAAAA"
    "AQAAAFNUQVSUCAAAYwAGACUCAABEWElMAwEAABAAAAB8CAAAQkPA3iEMAAAcAgAAC4IgAAIAAAATAAAAB4EjkUHIBEkGEDI5kgGEDCUFCBkeBItigBhFAkKSC0LEEDIU"
    "OAgYSwoyYohIkBQgQ0aIpQAZMkLkSA6QESPEUEFRgYzhg+WKBDFGBlEYAAAMAAAAG4jg/////wdA2mAIAZAAywZjEIAFoDYYxP////8PgARQG4zi/////wdAAioAAAAA"
    "SRgAAAQAAAATgmCCIAQTBmEIJgTEhKAAiSAAAD4AAAAyIogJIGSFBBMjpIQEEyPjhKGQFBJMjIwLhMRMEJjBHAEYnBlIU0QJk78C2BQBAtIYmiAQCxEBE+I07BRRwkRF"
    "BAoACk6TpogSJn+FN2witGGICEnaqKIgIhQANIwAlKAg4xxpiihh8lMAWxxgQAFASBGKhJQZgGEEgTk2kKaIEiZ/o5BlEps2QoDGWAixmYhIIoQJcRptmiIkoCZCQkFD"
    "ThmK5CFojgApAwBINBWEARiGYRiGqjIwAEMXSfcMlz9hDyH5IdAMC4GCrCwFoAEAAACABNBWogLQAAAAAIZhGIZhmIS6MmhAQF8ZNGCgcCBgjiCYIwAFABMUcsCHdGCH"
    "NmiHeWgDcsCHDa9QDm3QDnpQDm0AD3owB3KgB3MgB22QDnGgB3MgB22QDnigB3MgB22QDnFgB3owB3LQBukwB3KgB3MgB22QDnZAB3pgB3TQBuYQB3agB3MgB21gDnMg"
    "B3owB3LQBuZgB3SgB3ZAB23gDnigB3FgB3owB3KgB3ZABzoPRJAhI0VEADYAYD4A4CGPAQRAAAAAAAAAAABDHgUIAAEAAAAAAAAAhjwQEAADAAAAAAAAAAx5JiAACAAA"
    "AAAAAAAY8kxAABAAAAAAAAAAMOSpgAAgAAAAAAAAAGDIcwEBQAAAAAAAAADAkGcDAiAAAAAAAAAAgCFPBwRAAAAAAAAAAABDng8IgAIAAAAAAAAAhjxhAARAAQAAAAAA"
    "AABZIA4AAAAyHpgYGRFMkIwJJkfGBEMCSqAMRgCKoUAKoSzKoRQKoiRKowiKokSKi8oCIXIEgJwZAEJmAAAAAHkYAACjAAAAGgNMkEYCE8Q0IMMbQ4GTS7MLoytLAYlx"
    "yXGBcamhgZEBQYEhmykrsxGrqWkhS5Ojy0vZEAQTBKCZIADOBmEgJgjAs0EwDA5saWITBADaMCAJMUEQABpnU2NlbmUTBCCaIADSBsFwNiTGwhjG0BjPhgCaIBABj7On"
    "Ojq4OroJAjBNEMjA27AY0mQYA1VVFbAhsDYQ0QUAEwQEDDigpdFNEABqggBUG4xEI4yN2yA43QTB+SYIgEVGLEwub6zMjU4ubWxuggBcEwQAmyAA2QYkCQPC2MRgDMhg"
    "gwAGZbChMDLvM4MJwiFsADYMQxqkwYZADSYIy7BhIIM0SIMNghq0wQQhIjYMRhqkwYZBDdoADjYcAxqsARu4wRvEAYEJQhl0GwSDDjYUwBwAWB2wFAJ+htje5srm6JDS"
    "6ICAsoKwqqDC8tjewsiAgKqE6tLY6JLcqOTSwtzO2MqS3OjK5ObKxujS3tyC6Ojk0sTq6MrmgICAtCYIgLYhMDYgYIAHibOBAZcHG4o3uAMA0ANeAT9NaXNzQEBZQVhV"
    "UGF5bG9hZEBAQFobDDCgEofLgw0FG/ABAPQBn4AfqbC8ozI3IKCsICwsrQ0EGGxcHmwo0OAPAAAUaJixvYXRzU0QgI1Fmtsc3dwEAeBIpLnRzTGhK8P7mqN7kyvbgIjC"
    "KJBCKZjCcApOFTY2uzaXNLIyN7opQVCFDM/FrkxuLu3NbUpANCHDc7ELY7Mrk5sSGHXI8Fzm0MLIyuSa3sjK2KYESRkyPBe5srm3OrmxsrkpwVWJDM+FLg+uLMjN7Y0u"
    "jC7tzW1uimAGcVCHDM+lzI1OLg/qLc2Nbm4KUQd60Aeg0IUMz2Xsrc6NrkxubkpwCgAAAHkYAABMAAAAMwiAHMThHGYUAT2IQziEw4xCgAd5eAdzmHEM5gAP7RAO9IAO"
    "MwxCHsLBHc6hHGYwBT2IQziEgxvMAz3IQz2MAz3MeIx0cAd7CAd5SIdwcAd6cAN2eIdwIIcZzBEO7JAO4TAPbjAP4/AO8FAOMxDEHd4hHNghHcJhHmYwiTu8gzvQQzm0"
    "Azy8gzyEAzvM8BR2YAd7aAc3aIdyaAc3gIdwkIdwYAd2KAd2+AV2eId3gIdfCIdxGIdymId5mIEs7vAO7uAO9cAO7DADYsihHOShHMyhHOShHNxhHMohHMSBHcphBtaQ"
    "QznIQzmYQznIQzm4wziUQziIAzuUwy+8gzz8gjvUAzuww4zIIQd8cANyEIdzcAN7CAd5YIdwyId3qAd6mIE85IAPbkAP5dAO8AAAAHEgAABiAAAARVAKgd+Q/Z6X53Rk"
    "mg4EZoPYKjScZ7/DZCCwKqyn2fSkmypPh91ndjnpppfl8/KYnn67g3S6PC2u08tzIBCorYEr8Gum53MgMBvEVqHhPPsdJgOBQG0JPIGfNJw/lt1AYDaIxWorYAwCv/Oz"
    "TofX6UDgrCq9CvP0cpBMlpfnc2HdbC7LgcBgAbhB4HeejsvuMhA4q0rDebo8PE67z8HxuMwuy8P09Ns9pcvrY3pdXgYCg8YwB8PlO48vRAQwESHQDAvxOVGJBL40RZQw"
    "+Su8YROhDUNESNJGFQUR2cIfDJfvPL4QEcBEhEAzLMTnRCUS+NIUUcLkrwA2RYCANIYmCMRCRMCEOA07RZQwURFhBWAwXL7z+AMiPcAkHCuASR3CEI2EOI3kI7dtBttw"
    "+c7jD4j0AJNwrAAmic1AXD5y23bgDJfvPP7gTLdf3LYlYMPlO48fAdZGFQURsZMTET5y26bQDZfvPP4UAQKxApgvTRElTH4KYIsDDIbwDJfvPD7VABHmF7cNAAAAAAAA"
    "SEFTSBQAAAAAAAAARpqdrLctKlKfd2uJ0Rk2S0RYSUwsCgAAYwAGAIsCAABEWElMAwEAABAAAAAUCgAAQkPA3iEMAACCAgAAC4IgAAIAAAATAAAAB4EjkUHIBEkGEDI5"
    "kgGEDCUFCBkeBItigBhFAkKSC0LEEDIUOAgYSwoyYohIkBQgQ0aIpQAZMkLkSA6QESPEUEFRgYzhg+WKBDFGBlEYAAANAAAAG4jg/////wdA2mAIAZAAywZjEIAFoDYY"
    "xP////8PgARQG4zi/////wdAAqoNhAEBZwAAAEkYAAAFAAAAE4JggiAEEwZhCCYExISgmBAYAACJIAAAPwAAADIiiAkgZIUEEyOkhAQTI+OEoZAUEkyMjAuExEwQnMEc"
    "ARicGUhTRAmTvwLYFAEC0hiaIBALEQET4jTsFFHCREUECgAKTpOmiBImf4U3bCK0YYgISdqooiAiFAA0jACUoCDjHGmKKGHyUwBbHGBAAUBIEYqElBmAYQSBOTaQpogS"
    "Jn+jkGUSmzZCgMZYCLGZiEgihAlxGm2aIiSgJkJCQUNOGYrkIWiOACkDAEg0FYQBGIZhGIaqMjAAQxdJ9wyXP2EPIfkh0AwLgYKsLAWgAQAAAIAE0FaiAtAAAAAAhmEY"
    "hmGYhLoyaEBAXxk0YKBwIGCOIJgjAAUCAAAAABMUcsCHdGCHNmiHeWgDcsCHDa9QDm3QDnpQDm0AD3owB3KgB3MgB22QDnGgB3MgB22QDnigB3MgB22QDnFgB3owB3LQ"
    "BukwB3KgB3MgB22QDnZAB3pgB3TQBuYQB3agB3MgB21gDnMgB3owB3LQBuZgB3SgB3ZAB23gDnigB3FgB3owB3KgB3ZABzoPRJAhI0VEADYAYD4A4CGPAQBAAAAAAAAA"
    "AABDHgUAAAEAAAAAAAAAhjwQAAADAAAAAAAAAAx5JiAACAAAAAAAAAAY8kxAABAAAAAAAAAAMOSpgAAgAAAAAAAAAGDIcwEBQAAAAAAAAADAkGcDAiAAAAAAAAAAgCFP"
    "BwRAAAAAAAAAAABDng8IgAIAAAAAAAAAhjxhAARAAQAAAAAAAABZIAsAAAAyHpgUGRFMkIwJJkfGBEMCSqAMSqIYRgAKpBDKoggKoijKoRSIHAGgskAAAHkYAACCAAAA"
    "GgNMkEYCE8Q0IMMbQ4GTS7MLoytLAYlxyXGBcamhgZEBQYEhmykrsxGrqWkhS5Ojy0vZEAQTBKCZIADOBmEgJgjAs0EYDA5saWITBADaMCAJMUEAogmCANA4mxorcyub"
    "IADSBAGYNgjLsyFZmGZZBmeBNgTRBIEIeJw91dHB1dFNEABqgkAG2oZlmahlGSrLsoANwbWBkDAAmCAcwgZgwzBs24aAmyAswwQBqDYM37ZtEDgwmCBExIZh2bYNAwcG"
    "Y7DhGLTOCwMxIAMCE4Qy2DYIyxlsKAAzADI0YCkE/Ayxvc2VzdEhpdEBAWUFYVVBheWxvYWRAQFVCdWlsdEluVHJpYW5nbGVJbnRlcnNlY3Rpb25BdHRyaWJ1dGVzQEB"
    "AWlNEABrggBcEwQAmyAA2YZg2YCsARskTxusgRu8wYZCDNQAAOCAV8BPU9rcHBBQVhBWFVRYHttbGBkQEJDWBmMNquRxgzfYUHhyAABzwCfgRyos76jMDQgoKwgLS2sD"
    "sQZt4AZvsKHQ6gAA7KAKG5tdm0saWZkb3ZQgqEKG52JXJjeX9uY2JSCakOG52IWx2ZXJTQmMOmR4LnNoYWRlck1vZGVsU4KkDBmei1zZ3Fud3FjZ3JQAq0SG50KXB1cW"
    "5Ob2RhdGl/bmNjclIIM6ZHguZW50cnlQb2ludHNTCDSAgzmwAwAAAHkYAABMAAAAMwiAHMThHGYUAT2IQziEw4xCgAd5eAdzmHEM5gAP7RAO9IAOMwxCHsLBHc6hHGYw"
    "BT2IQziEgxvMAz3IQz2MAz3MeIx0cAd7CAd5SIdwcAd6cAN2eIdwIIcZzBEO7JAO4TAPbjAP4/AO8FAOMxDEHd4hHNghHcJhHmYwiTu8gzvQQzm0Azy8gzyEAzvM8BR2"
    "YAd7aAc3aIdyaAc3gIdwkIdwYAd2KAd2+AV2eId3gIdfCIdxGIdymId5mIEs7vAO7uAO9cAO7DADYsihHOShHMyhHOShHNxhHMohHMSBHcphBtaQQznIQzmYQznIQzm4"
    "wziUQziIAzuUwy+8gzz8gjvUAzuww4zIIQd8cANyEIdzcAN7CAd5YIdwyId3qAd6mIE85IAPbkAP5dAO8AAAAHEgAABiAAAARVAKgd+Q/Z6X53Rkmg4EZoPYKjScZ7/D"
    "ZCCwKqyn2fSkmypPh91ndjnpppfl8/KYnn67g3S6PC2u08tzIBCorYEr8Gum53MgMBvEVqHhPPsdJgOBQG0JPIGfNJw/lt1AYDaIxWorYAwCv/OzTofX6UDgrCq9CvP0"
    "cpBMlpfnc2HdbC7LgcBgAbhB4HeejsvuMhA4q0rDebo8PE67z8HxuMwuy8P09Ns9pcvrY3pdXgYCg8YwB8PlO48vRAQwESHQDAvxOVGJBL40RZQw+Su8YROhDUNESNJG"
    "FQUR2cIfDJfvPL4QEcBEhEAzLMTnRCUS+NIUUcLkrwA2RYCANIYmCMRCRMCEOA07RZQwURFhBWAwXL7z+AMiPcAkHCuASR3CEI2EOI3kI7dtBttw+c7jD4j0AJNwrAAm"
    "ic1AXD5y23bgDJfvPP7gTLdf3LYlYMPlO48fAdZGFQURsZMTET5y26bQDZfvPP4UAQKxApgvTRElTH4KYIsDDIbwDJfvPD7VABHmF7cNAABhIAAAbgAAABMEQSwQAAAA"
    "GQAAAATMABSwQGEKlKhAkQqUW8mUrkD5D5Q4uSKpQpk2K1MnFAZJIwAlQMwYAQiCIP4LYwQgCIL4NwIwRgCCIAiCwhgBCIIgCA5jBO9Mmmg3RgCCIMyGwRgBCIIgCAZj"
    "BCAIgvAHAAA0B8GgORjGTAQCNKQwYmAAIAgGExxczoiBAYAgGExxgDkjBgYAgmAwzQEGjRgYAAiCwUQHGXQEU0cwZYICHxMW+JzB1BlMGSHQxwiBPiZE8jFBko8JGnxM"
    "2OBjWRCfEYMFAEEwqEDBDIaAGwJuxMAAQBAMLlAwg8CCQj4mEPIZMTAAEASDDxTcANvIcO3BHgwbEAEfEMCIAWWAIBh0o6AGAh+kAR+AAh+EQRh8HjEUHUYBQIYbgj4I"
    "g+kGNUiDYMTAAEAQDMKAFOaAGzFwABAEgw0V5iAAhToQ6qAO6gAN/GDEwABAEAzCoBTooBsxcAAQBIMtFegg+AM7KOzADuwgDf5gxMAAQBAMwsAU6sAbMXAAEASDTRXq"
    "IOiDO+ju4A7uQA1AYcTAAEAQDMLgFOzgGzFwABAEg20V7CBoAzzo8AAP8GANQgEBYSAAAAgAAAATBMFGhoBhhg2IoBkADAcCAgAAAMZxPAC2OMAAAAAAAGEgAAALAAAA"
    "EwTBRgahaYYNiEAaAAwHAgUAAADWoQDTFCEBNRGScRwPgC0OMAAAAAAAAAAAAAAA";

std::string hresult_hex(const HRESULT value) {
    char buffer[32]{};
    std::snprintf(buffer, sizeof(buffer), "0x%08" PRIx32,
                  static_cast<std::uint32_t>(value));
    return buffer;
}

std::vector<std::uint8_t> decode_base64(const std::string_view encoded) {
    std::vector<std::uint8_t> decoded;
    decoded.reserve((encoded.size() / 4U) * 3U);
    std::uint32_t accumulator = 0U;
    std::uint32_t bits = 0U;
    for (const unsigned char character : encoded) {
        if (character == '=') break;
        std::uint32_t value = 0U;
        if (character >= 'A' && character <= 'Z')
            value = static_cast<std::uint32_t>(character - 'A');
        else if (character >= 'a' && character <= 'z')
            value = static_cast<std::uint32_t>(character - 'a' + 26U);
        else if (character >= '0' && character <= '9')
            value = static_cast<std::uint32_t>(character - '0' + 52U);
        else if (character == '+')
            value = 62U;
        else if (character == '/')
            value = 63U;
        else
            continue;
        accumulator = (accumulator << 6U) | value;
        bits += 6U;
        if (bits >= 8U) {
            bits -= 8U;
            decoded.push_back(static_cast<std::uint8_t>((accumulator >> bits) & 0xffU));
        }
    }
    return decoded;
}

std::string utf8_from_wide(const wchar_t* value) {
    if (value == nullptr || value[0] == L'\0') return {};
    const int required = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
                                             value, -1, nullptr, 0, nullptr, nullptr);
    if (required <= 1) return {};
    std::string result(static_cast<std::size_t>(required), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value, -1,
                            result.data(), required, nullptr, nullptr) <= 0)
        return {};
    if (!result.empty() && result.back() == '\0') result.pop_back();
    return result;
}

struct D3D12Module final {
    HMODULE handle{};
    D3D12Module() = default;
    D3D12Module(const D3D12Module&) = delete;
    D3D12Module& operator=(const D3D12Module&) = delete;
    ~D3D12Module() {
        if (handle != nullptr) FreeLibrary(handle);
    }
    [[nodiscard]] bool load() {
        handle = LoadLibraryW(L"d3d12.dll");
        return handle != nullptr;
    }
    template <typename Function>
    [[nodiscard]] Function symbol(const char* name) const {
        if (handle == nullptr) return nullptr;
        return reinterpret_cast<Function>(GetProcAddress(handle, name));
    }
};

struct DxgiModule final {
    HMODULE handle{};
    DxgiModule() = default;
    DxgiModule(const DxgiModule&) = delete;
    DxgiModule& operator=(const DxgiModule&) = delete;
    ~DxgiModule() {
        if (handle != nullptr) FreeLibrary(handle);
    }
    [[nodiscard]] bool load() {
        handle = LoadLibraryW(L"dxgi.dll");
        return handle != nullptr;
    }
    template <typename Function>
    [[nodiscard]] Function symbol(const char* name) const {
        if (handle == nullptr) return nullptr;
        return reinterpret_cast<Function>(GetProcAddress(handle, name));
    }
};

bool resource_bytes_bounded(const std::uint64_t bytes) noexcept {
    return bytes > 0U && bytes <= native_d3d12_raytracing_context_max_resource_bytes;
}

HRESULT create_committed_buffer(ID3D12Device* device, const std::uint64_t bytes,
                                const D3D12_HEAP_TYPE heap_type,
                                const D3D12_RESOURCE_STATES state,
                                const D3D12_RESOURCE_FLAGS flags,
                                ComPtr<ID3D12Resource>& resource) {
    if (device == nullptr || !resource_bytes_bounded(bytes))
        return E_INVALIDARG;
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = heap_type;
    heap.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    heap.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;

    D3D12_RESOURCE_DESC description{};
    description.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    description.Width = bytes;
    description.Height = 1U;
    description.DepthOrArraySize = 1U;
    description.MipLevels = 1U;
    description.Format = DXGI_FORMAT_UNKNOWN;
    description.SampleDesc.Count = 1U;
    description.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    description.Flags = flags;

    return device->CreateCommittedResource(
        &heap, D3D12_HEAP_FLAG_NONE, &description, state, nullptr,
        IID_PPV_ARGS(&resource));
}

bool fill_upload_buffer(ID3D12Resource* resource, const void* data,
                        const std::size_t bytes) {
    if (resource == nullptr || data == nullptr || bytes == 0U)
        return false;
    void* mapped = nullptr;
    const D3D12_RANGE read_range{0U, 0U};
    if (FAILED(resource->Map(0U, &read_range, &mapped)) || mapped == nullptr)
        return false;
    std::memcpy(mapped, data, bytes);
    resource->Unmap(0U, nullptr);
    return true;
}

// Versioned full-frame camera ABI.  Each member is a float4 so HLSL cbuffer
// packing is explicit and stable; the upload allocation itself is rounded to
// D3D12's 256-byte CBV placement alignment.  No matrix transpose convention
// is involved because RayGen consumes the world-space basis vectors directly.
struct NativeD3D12RayTracingCameraConstants final {
    float position[4U];
    float right[4U];
    float up[4U];
    float forward[4U];
    float lens[4U]; // tan-half vertical FOV, aspect, near, far
};
static_assert(sizeof(NativeD3D12RayTracingCameraConstants) == 80U);
constexpr std::uint64_t native_d3d12_raytracing_camera_constants_bytes =
    D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT;

struct NativeD3D12RayTracingMaterialConstants final {
    float base_color[4U];
    float material_parameters[4U]; // metallic, roughness, emissive intensity, reserved
    float emissive[4U];
};
static_assert(sizeof(NativeD3D12RayTracingMaterialConstants) == 48U);

struct NativeD3D12RayTracingInstanceShadingConstants final {
    std::uint32_t material_index{};
    std::uint32_t normal_offset{};
    std::uint32_t normal_count{};
    std::uint32_t reserved{};
};
static_assert(sizeof(NativeD3D12RayTracingInstanceShadingConstants) == 16U);

struct NativeD3D12RayTracingLightingConstants final {
    float directional_direction[4U];
    float directional_color_intensity[4U];
    float ambient_color_intensity[4U];
};
static_assert(sizeof(NativeD3D12RayTracingLightingConstants) == 48U);

bool fill_camera_constants(ID3D12Resource* resource,
                           const NativeD3D12RayTracingCamera& camera) {
    if (resource == nullptr) return false;
    NativeD3D12RayTracingCameraConstants constants{};
    constants.position[0U] = camera.position[0U];
    constants.position[1U] = camera.position[1U];
    constants.position[2U] = camera.position[2U];
    constants.right[0U] = camera.right[0U];
    constants.right[1U] = camera.right[1U];
    constants.right[2U] = camera.right[2U];
    constants.up[0U] = camera.up[0U];
    constants.up[1U] = camera.up[1U];
    constants.up[2U] = camera.up[2U];
    constants.forward[0U] = camera.forward[0U];
    constants.forward[1U] = camera.forward[1U];
    constants.forward[2U] = camera.forward[2U];
    constants.lens[0U] = camera.vertical_fov_tan_half;
    constants.lens[1U] = camera.aspect_ratio;
    constants.lens[2U] = camera.near_plane;
    constants.lens[3U] = camera.far_plane;
    return fill_upload_buffer(resource, &constants, sizeof(constants));
}

#endif

} // namespace

struct NativeD3D12RayTracingContext::Impl final {
    enum class PendingGpuOperation : std::uint8_t {
        none = 0U,
        trace = 1U,
        output_copy = 2U,
    };

    NativeD3D12RayTracingContextOptions options;
    NativeD3D12RayTracingContextState state{
        NativeD3D12RayTracingContextState::uninitialized};
    std::uint64_t generation{};
    std::uint64_t scene_generation{};
    std::uint64_t resource_generation{};
    std::optional<NativeD3D12RayTracingScene> scene;
    std::uint64_t scene_hash{};
    bool scene_dirty{};
    NativeD3D12RayTracingCamera camera{};
    bool camera_valid{};
    bool camera_dirty{true};
    std::uint64_t camera_fingerprint{};
    bool camera_shader_consumed{};
    NativeD3D12RayTracingLighting lighting{};
    std::vector<NativeD3D12RayTracingMaterial> resolved_materials;
    bool shading_resources_ready{};
    std::uint64_t shading_scene_hash{};
    std::uint64_t shading_fingerprint{};
    bool linear_radiance_shader_active{};
    bool linear_radiance_shader_consumed{};
    bool native_handles_exposed{};
    std::string device_name;
    std::uint32_t raytracing_tier{};
    std::string last_code;
    std::string last_detail;
    NativeD3D12RayTracingContextFailureStage last_stage{
        NativeD3D12RayTracingContextFailureStage::none};
    bool borrowed_device_requested{};
    bool borrowed_command_queue_requested{};
    bool device_adopted{};
    bool command_queue_adopted{};
    bool shared_device{};
    bool shared_command_queue{};
    NativeD3D12RayTracingOutputSurfaceState output_surface_state{
        NativeD3D12RayTracingOutputSurfaceState::unavailable};
    std::uint64_t output_resource_generation{};
    std::uint64_t output_access_token{};
    std::uint64_t next_output_access_token{1U};
    std::uint64_t output_fence_value{};
    std::uint64_t last_submitted_fence_value{};
    std::uint64_t last_completed_fence_value{};
    // Common mirrors keep the metadata contract available on non-Windows
    // builds without leaking the platform-specific resource members below.
    bool output_surface_resource_ready{};
    bool output_surface_trace_completed{};
    bool last_output_copy_submitted{};
    bool last_output_copy_completed{};
    PendingGpuOperation pending_gpu_operation{PendingGpuOperation::none};
    std::uint64_t pending_fence_value{};

#if defined(_WIN32)
    struct GeometryResources final {
        ComPtr<ID3D12Resource> vertex_buffer;
        ComPtr<ID3D12Resource> vertex_upload;
        ComPtr<ID3D12Resource> index_buffer;
        ComPtr<ID3D12Resource> index_upload;
        ComPtr<ID3D12Resource> blas_result;
        ComPtr<ID3D12Resource> blas_scratch;
        std::uint64_t vertex_bytes{};
        std::uint64_t index_bytes{};
        std::uint64_t blas_result_bytes{};
        std::uint64_t blas_scratch_bytes{};
        std::uint32_t vertex_count{};
        std::uint32_t index_count{};
        bool allow_update{};
    };

    D3D12Module d3d12_module;
    DxgiModule dxgi_module;
    ComPtr<IDXGIFactory4> factory;
    ComPtr<ID3D12Device5> device;
    ComPtr<ID3D12CommandQueue> command_queue;
    ComPtr<ID3D12CommandAllocator> command_allocator;
    ComPtr<ID3D12GraphicsCommandList4> command_list;
    ComPtr<ID3D12Fence> fence;
    std::uint64_t next_fence_value{1U};
    HANDLE fence_event{};
    std::vector<GeometryResources> geometry_resources;
    ComPtr<ID3D12Resource> instance_buffer;
    ComPtr<ID3D12Resource> tlas_result;
    ComPtr<ID3D12Resource> tlas_scratch;
    ComPtr<ID3D12RootSignature> trace_root_signature;
    ComPtr<ID3D12StateObject> trace_state_object;
    ComPtr<ID3D12StateObjectProperties> trace_state_properties;
    ComPtr<ID3D12Resource> shader_table;
    ComPtr<ID3D12Resource> camera_constants;
    ComPtr<ID3D12Resource> shading_materials;
    ComPtr<ID3D12Resource> shading_lighting;
    ComPtr<ID3D12Resource> shading_instances;
    ComPtr<ID3D12Resource> shading_normals;
    ComPtr<ID3D12Resource> output_resource;
    ComPtr<ID3D12Resource> output_readback;
    std::uint64_t instance_buffer_bytes{};
    std::uint64_t tlas_result_bytes{};
    std::uint64_t tlas_scratch_bytes{};
    std::uint64_t shader_table_bytes{};
    std::uint64_t camera_constants_bytes{};
    std::uint64_t shading_material_bytes{};
    std::uint64_t shading_lighting_bytes{};
    std::uint64_t shading_instance_bytes{};
    std::uint64_t shading_normal_bytes{};
    std::uint64_t output_resource_bytes{};
    bool shader_pipeline_ready{};
    bool shader_table_ready{};
    bool camera_constants_ready{};
    bool full_frame_shader_active{};
    bool output_resource_ready{};
    bool last_trace_submitted{};
    bool last_trace_completed{};
    bool last_readback_completed{};
    bool output_in_copy_source{};
    std::uint32_t output_sentinel{};
    std::uint32_t output_hit{};
    bool output_radiance_valid{};
    std::array<float, 4U> output_radiance_probe{};
    std::uint64_t output_hash{};
    std::uint64_t built_scene_hash{};
    std::uint64_t built_topology_hash{};
    bool blas_ready{};
    bool tlas_ready{};
    bool last_build_submitted{};
    bool last_build_completed{};
    bool last_update_submitted{};
    bool last_update_completed{};
    bool last_synchronization_completed{};
#endif

    explicit Impl(NativeD3D12RayTracingContextOptions input)
        : options(std::move(input)), camera(options.camera), lighting(options.lighting) {
        if (options.output_width == 0U) options.output_width = 1U;
        if (options.output_height == 0U) options.output_height = 1U;
        options.output_width = std::min(options.output_width, 4096U);
        options.output_height = std::min(options.output_height, 4096U);
        if (options.max_geometry_count == 0U)
            options.max_geometry_count = native_d3d12_raytracing_context_max_geometry_count;
        options.max_geometry_count = std::min(
            options.max_geometry_count, native_d3d12_raytracing_context_max_geometry_count);
        if (options.max_resource_bytes == 0U)
            options.max_resource_bytes = native_d3d12_raytracing_context_max_resource_bytes;
        options.max_resource_bytes = std::min(
            options.max_resource_bytes, native_d3d12_raytracing_context_max_resource_bytes);
        if (options.synchronization_policy !=
                NativeD3D12RayTracingSynchronizationPolicy::wait_for_completion &&
            options.synchronization_policy !=
                NativeD3D12RayTracingSynchronizationPolicy::submit_only) {
            options.synchronization_policy =
                NativeD3D12RayTracingSynchronizationPolicy::wait_for_completion;
        }
        const auto validation = validate_camera(camera);
        camera_valid = validation.valid;
        camera_fingerprint = camera_valid ? camera_signature(camera) : 0U;
        shading_fingerprint = shading_signature(resolved_materials, lighting);
    }
};

void NativeD3D12RayTracingContext::save_result(
    NativeD3D12RayTracingContext::Impl& impl,
    const NativeD3D12RayTracingContextFailureStage stage,
    const std::string_view code, const std::string_view detail) {
    impl.last_stage = stage;
    impl.last_code = bounded_text(code);
    impl.last_detail = bounded_text(detail);
}

NativeD3D12RayTracingContextReceipt NativeD3D12RayTracingContext::receipt_from(
    const NativeD3D12RayTracingContext::Impl& impl, const std::string_view operation) {
    NativeD3D12RayTracingContextReceipt result;
    result.operation = std::string(operation);
    result.state = impl.state;
    result.failure_stage = impl.last_stage;
    result.code = impl.last_code;
    result.detail = impl.last_detail;
    result.device_name = bounded_text(impl.device_name);
    result.native_handle_exposed = false;
    result.initialized = impl.generation != 0U;
#if defined(_WIN32)
    result.device_ready = impl.device != nullptr;
    result.command_queue_ready = impl.command_queue != nullptr;
    result.fence_ready = impl.fence != nullptr;
#else
    result.device_ready = false;
    result.command_queue_ready = false;
    result.fence_ready = false;
#endif
    result.generation = impl.generation;
    result.scene_generation = impl.scene_generation;
    result.resource_generation = impl.resource_generation;
    result.borrowed_device_requested = impl.borrowed_device_requested;
    result.borrowed_command_queue_requested = impl.borrowed_command_queue_requested;
    result.device_adopted = impl.device_adopted;
    result.command_queue_adopted = impl.command_queue_adopted;
    result.shared_device = impl.shared_device;
    result.shared_command_queue = impl.shared_command_queue;
    result.output_copy_submitted = impl.last_output_copy_submitted;
    result.output_copy_completed = impl.last_output_copy_completed;
    result.synchronization_policy = std::string(
        native_d3d12_raytracing_synchronization_policy_name(
            impl.options.synchronization_policy));
    result.completion_pending = impl.pending_fence_value != 0U;
    result.submitted_fence_value = impl.last_submitted_fence_value;
    result.completed_fence_value = impl.last_completed_fence_value;
    result.full_frame_shader_ready =
        impl.options.shaders.full_frame_contract ==
            native_d3d12_raytracing_full_frame_shader_contract &&
        !impl.options.shaders.full_frame_library_dxil.empty();
    result.camera_ready = impl.camera_valid;
    result.camera_shader_consumed = impl.camera_shader_consumed;
    result.camera_schema = std::string(native_d3d12_raytracing_camera_schema);
    result.camera_fingerprint = impl.camera_fingerprint;
    result.shading_resources_ready = impl.shading_resources_ready;
    result.linear_radiance_shader_consumed = impl.linear_radiance_shader_consumed;
    result.claims_rtgi = false;
    result.shading_schema = std::string(native_d3d12_raytracing_shading_schema);
    result.shading_fingerprint = impl.shading_fingerprint;
    result.shading_material_count = static_cast<std::uint32_t>(
        impl.resolved_materials.size());
    result.shader_contract = result.full_frame_shader_ready
        ? impl.options.shaders.full_frame_contract
        : "noemancer.native-rt-marker-probe/0.1";
    result.scene_received = impl.scene.has_value();
    if (impl.scene) {
        result.scene_revision = impl.scene->revision;
        result.geometry_count = static_cast<std::uint32_t>(impl.scene->geometries.size());
        result.instance_count = result.geometry_count;
    }
    result.raytracing_tier = impl.raytracing_tier;
    result.output_width = impl.options.output_width;
    result.output_height = impl.options.output_height;
    result.output_pixel_stride_bytes = sizeof(std::uint32_t) * 4U;
    result.output_bytes = static_cast<std::uint64_t>(result.output_width) * result.output_height *
        result.output_pixel_stride_bytes;
    result.output_readback_bytes = result.output_bytes;
    result.output_surface.schema = std::string(native_d3d12_raytracing_output_surface_schema);
    result.output_surface.resource_kind = "buffer";
    result.output_surface.format = impl.linear_radiance_shader_active
        ? "R32G32B32A32_FLOAT" : "R32G32B32A32_UINT";
    result.output_surface.resource_state = impl.output_surface_state;
    result.output_surface.width = impl.options.output_width;
    result.output_surface.height = impl.options.output_height;
    result.output_surface.depth = 1U;
    result.output_surface.pixel_stride_bytes = result.output_pixel_stride_bytes;
    result.output_surface.bytes = result.output_bytes;
    result.output_surface.resource_generation = impl.output_resource_generation;
    result.output_surface.context_generation = impl.generation;
    result.output_surface.submitted_fence_value = impl.output_fence_value;
    result.output_surface.completed_fence_value = impl.last_completed_fence_value;
    result.output_surface.resource_ready = impl.output_surface_resource_ready;
    result.output_surface.gpu_write_complete =
        impl.output_surface_resource_ready &&
        impl.output_surface_state == NativeD3D12RayTracingOutputSurfaceState::copy_source &&
        impl.output_surface_trace_completed &&
        impl.output_fence_value != 0U &&
        impl.last_completed_fence_value >= impl.output_fence_value;
    result.output_surface.valid = result.output_surface.gpu_write_complete;
    result.output_surface.cpu_readback_required = false;
    result.output_surface.shared_device = impl.shared_device;
    result.output_surface.shared_command_queue = impl.shared_command_queue;
    const auto row_bytes = static_cast<std::uint64_t>(result.output_surface.width) *
        result.output_surface.pixel_stride_bytes;
    result.output_surface.direct_sdl_gpu_import_supported =
        result.output_surface.shared_device && result.output_surface.shared_command_queue &&
        row_bytes != 0U && (row_bytes % D3D12_TEXTURE_DATA_PITCH_ALIGNMENT) == 0U;
    result.output_surface.expired =
        impl.state == NativeD3D12RayTracingContextState::shutdown ||
        impl.output_surface_state == NativeD3D12RayTracingOutputSurfaceState::released;
    if (result.output_surface.expired) {
        result.output_surface.code = "native-d3d12.context.output-surface-expired";
        result.output_surface.detail =
            "The retained output resource was released by shutdown; previously borrowed views are invalid.";
    } else if (result.output_surface.valid) {
        result.output_surface.code = "native-d3d12.context.output-surface-ready";
        result.output_surface.detail =
            "The retained output is fence-complete in COPY_SOURCE state and can be consumed by a GPU-only runtime copy.";
    } else if (result.output_surface.resource_ready) {
        result.output_surface.code = "native-d3d12.context.output-surface-not-ready";
        result.output_surface.detail =
            "The retained output exists but is not yet a fence-complete COPY_SOURCE surface.";
    } else {
        result.output_surface.code = "native-d3d12.context.output-surface-unavailable";
        result.output_surface.detail =
            "A retained output surface is created only after the native trace pipeline is ready.";
    }
#if defined(_WIN32)
    result.blas_ready = impl.blas_ready;
    result.tlas_ready = impl.tlas_ready;
    result.build_submitted = impl.last_build_submitted;
    result.build_completed = impl.last_build_completed;
    result.update_submitted = impl.last_update_submitted;
    result.update_completed = impl.last_update_completed;
    result.synchronization_completed = impl.last_synchronization_completed;
    result.shader_pipeline_ready = impl.shader_pipeline_ready;
    result.shader_table_ready = impl.shader_table_ready;
    result.output_resource_ready = impl.output_resource_ready;
    result.trace_submitted = impl.last_trace_submitted;
    result.trace_completed = impl.last_trace_completed;
    result.readback_completed = impl.last_readback_completed;
    result.output_sentinel = impl.output_sentinel;
    result.output_hit = impl.output_hit;
    result.output_radiance_valid = impl.output_radiance_valid;
    result.output_radiance_probe = impl.output_radiance_probe;
    result.output_hash = impl.output_hash;
    result.shader_table_bytes = impl.shader_table_bytes;
    result.output_bytes = impl.output_resource_bytes;
    result.output_readback_bytes = impl.output_resource_bytes;
    for (const auto& geometry : impl.geometry_resources) {
        result.vertex_buffer_bytes += geometry.vertex_bytes;
        result.index_buffer_bytes += geometry.index_bytes;
        result.blas_result_bytes += geometry.blas_result_bytes;
        result.blas_scratch_bytes += geometry.blas_scratch_bytes;
    }
    result.blas_ready = impl.blas_ready;
    result.tlas_ready = impl.tlas_ready;
    result.tlas_result_bytes = impl.tlas_result_bytes;
    result.tlas_scratch_bytes = impl.tlas_scratch_bytes;
#endif
    result.fallback_active = impl.state == NativeD3D12RayTracingContextState::unsupported;
    if (impl.state == NativeD3D12RayTracingContextState::shutdown) result.shutdown_completed = true;
    return result;
}

void NativeD3D12RayTracingContext::mark_unsupported(
    NativeD3D12RayTracingContext::Impl& impl,
    const NativeD3D12RayTracingContextFailureStage stage,
    const std::string_view operation,
    const std::string_view code, const std::string_view detail,
    NativeD3D12RayTracingContextReceipt& receipt) {
    save_result(impl, stage, code, detail);
    receipt = receipt_from(impl, operation);
    receipt.state = NativeD3D12RayTracingContextState::unsupported;
    receipt.fallback_active = true;
}

bool NativeD3D12RayTracingContext::ensure_trace_pipeline(
    NativeD3D12RayTracingContext::Impl& impl,
    std::string& code,
    std::string& detail) {
#if !defined(_WIN32)
    (void)impl;
    code = "native-d3d12.context.platform-unavailable";
    detail = "The persistent D3D12 TraceRays pipeline is available only on Windows.";
    return false;
#else
    const bool has_full_frame_library =
        !impl.options.shaders.full_frame_library_dxil.empty() ||
        !impl.options.shaders.full_frame_contract.empty();
    if (impl.shader_pipeline_ready && impl.shader_table_ready &&
        impl.output_resource_ready && impl.trace_root_signature != nullptr &&
        impl.trace_state_object != nullptr && impl.shader_table != nullptr &&
        impl.output_resource != nullptr && impl.output_readback != nullptr &&
        (!has_full_frame_library ||
         (impl.camera_constants_ready && impl.camera_constants != nullptr)))
        return true;
    if (impl.device == nullptr) {
        code = "native-d3d12.context.device-not-ready";
        detail = "A D3D12 device with ray-tracing support is required before TraceRays.";
        return false;
    }
    const bool has_any_custom_shader =
        !impl.options.shaders.ray_generation_dxil.empty() ||
        !impl.options.shaders.miss_dxil.empty() ||
        !impl.options.shaders.closest_hit_dxil.empty();
    if (has_any_custom_shader) {
        // The public 0.1 contract intentionally does not guess entry-point
        // names or merge independent DXIL modules.  Keep the pinned probe as
        // the default and fail closed for a caller-provided incomplete or
        // ambiguous shader set until a versioned shader ABI is introduced.
        code = "native-d3d12.context.custom-shader-contract-unsupported";
        detail = impl.options.shaders.complete()
            ? "Custom DXIL was supplied, but the 0.1 context contract has no versioned export/layout ABI; use the default pinned probe or a future shader contract."
            : "A custom DXIL set must contain all RayGen/Miss/ClosestHit modules; the incomplete set is not guessed or merged.";
        return false;
    }
    if (has_full_frame_library &&
        (impl.options.shaders.full_frame_contract !=
             native_d3d12_raytracing_full_frame_shader_contract ||
         impl.options.shaders.full_frame_library_dxil.empty())) {
        code = "native-d3d12.context.full-frame-shader-contract-invalid";
        detail = "The production DXR library must provide the exact camera+shading noemancer.native-rt-full-frame/0.3 contract and non-empty pinned DXIL bytes.";
        return false;
    }

    const auto serialize_root_signature =
        impl.d3d12_module.symbol<SerializeRootSignatureFn>("D3D12SerializeRootSignature");
    if (serialize_root_signature == nullptr) {
        code = "native-d3d12.context.root-signature-serializer-unavailable";
        detail = "D3D12SerializeRootSignature is not exported by the loaded D3D12 runtime.";
        return false;
    }

    D3D12_ROOT_PARAMETER root_parameters[7U]{};
    root_parameters[0U].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    root_parameters[0U].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    root_parameters[0U].Descriptor.ShaderRegister = 0U;
    root_parameters[0U].Descriptor.RegisterSpace = 0U;
    root_parameters[1U].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    root_parameters[1U].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    root_parameters[1U].Descriptor.ShaderRegister = 0U;
    root_parameters[1U].Descriptor.RegisterSpace = 0U;
    if (has_full_frame_library) {
        root_parameters[2U].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        root_parameters[2U].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        root_parameters[2U].Descriptor.ShaderRegister = 0U;
        root_parameters[2U].Descriptor.RegisterSpace = 0U;
        root_parameters[3U].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
        root_parameters[3U].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        root_parameters[3U].Descriptor.ShaderRegister = 1U;
        root_parameters[3U].Descriptor.RegisterSpace = 0U;
        root_parameters[4U].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        root_parameters[4U].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        root_parameters[4U].Descriptor.ShaderRegister = 1U;
        root_parameters[4U].Descriptor.RegisterSpace = 0U;
        root_parameters[5U].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
        root_parameters[5U].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        root_parameters[5U].Descriptor.ShaderRegister = 2U;
        root_parameters[5U].Descriptor.RegisterSpace = 0U;
        root_parameters[6U].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
        root_parameters[6U].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        root_parameters[6U].Descriptor.ShaderRegister = 3U;
        root_parameters[6U].Descriptor.RegisterSpace = 0U;
    }
    D3D12_ROOT_SIGNATURE_DESC root_signature_description{};
    root_signature_description.NumParameters = has_full_frame_library ? 7U : 2U;
    root_signature_description.pParameters = root_parameters;
    ComPtr<ID3DBlob> root_signature_blob;
    ComPtr<ID3DBlob> root_signature_error;
    HRESULT hr = serialize_root_signature(
        &root_signature_description, D3D_ROOT_SIGNATURE_VERSION_1,
        &root_signature_blob, &root_signature_error);
    if (FAILED(hr) || !root_signature_blob || root_signature_blob->GetBufferSize() == 0U) {
        code = "native-d3d12.context.root-signature-serialize-failed";
        detail = "D3D12SerializeRootSignature failed with " + hresult_hex(hr) + ".";
        return false;
    }
    ComPtr<ID3D12RootSignature> root_signature;
    hr = impl.device->CreateRootSignature(
        0U, root_signature_blob->GetBufferPointer(), root_signature_blob->GetBufferSize(),
        IID_PPV_ARGS(&root_signature));
    if (FAILED(hr) || !root_signature) {
        code = "native-d3d12.context.root-signature-create-failed";
        detail = "CreateRootSignature failed with " + hresult_hex(hr) + ".";
        return false;
    }

    const auto embedded_dxil = decode_base64(kDxrProbeDxilBase64);
    const void* dxil_data = has_full_frame_library
        ? static_cast<const void*>(impl.options.shaders.full_frame_library_dxil.data())
        : static_cast<const void*>(embedded_dxil.data());
    const auto dxil_size = has_full_frame_library
        ? impl.options.shaders.full_frame_library_dxil.size()
        : embedded_dxil.size();
    if (dxil_data == nullptr || dxil_size == 0U || dxil_size > impl.options.max_resource_bytes) {
        code = "native-d3d12.context.dxil-artifact-invalid";
        detail = "The selected pinned DXIL artifact was empty or exceeded the context resource budget.";
        return false;
    }
    D3D12_EXPORT_DESC exports[3U]{};
    exports[0U].Name = L"RayGen";
    exports[1U].Name = L"Miss";
    exports[2U].Name = L"ClosestHit";
    D3D12_DXIL_LIBRARY_DESC dxil_library{};
    dxil_library.DXILLibrary.BytecodeLength = dxil_size;
    dxil_library.DXILLibrary.pShaderBytecode = dxil_data;
    dxil_library.NumExports = 3U;
    dxil_library.pExports = exports;
    D3D12_HIT_GROUP_DESC hit_group{};
    hit_group.Type = D3D12_HIT_GROUP_TYPE_TRIANGLES;
    hit_group.HitGroupExport = L"TriangleHitGroup";
    hit_group.ClosestHitShaderImport = L"ClosestHit";
    D3D12_RAYTRACING_SHADER_CONFIG shader_config{};
    shader_config.MaxPayloadSizeInBytes = sizeof(std::uint32_t) + sizeof(float) * 3U;
    shader_config.MaxAttributeSizeInBytes = sizeof(float) * 2U;
    D3D12_GLOBAL_ROOT_SIGNATURE global_root_signature{};
    global_root_signature.pGlobalRootSignature = root_signature.Get();
    D3D12_RAYTRACING_PIPELINE_CONFIG pipeline_config{};
    pipeline_config.MaxTraceRecursionDepth = 1U;
    std::array<D3D12_STATE_SUBOBJECT, 5U> subobjects{};
    subobjects[0U].Type = D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY;
    subobjects[0U].pDesc = &dxil_library;
    subobjects[1U].Type = D3D12_STATE_SUBOBJECT_TYPE_HIT_GROUP;
    subobjects[1U].pDesc = &hit_group;
    subobjects[2U].Type = D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_SHADER_CONFIG;
    subobjects[2U].pDesc = &shader_config;
    subobjects[3U].Type = D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE;
    subobjects[3U].pDesc = &global_root_signature;
    subobjects[4U].Type = D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG;
    subobjects[4U].pDesc = &pipeline_config;
    D3D12_STATE_OBJECT_DESC state_object_description{};
    state_object_description.Type = D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE;
    state_object_description.NumSubobjects = static_cast<UINT>(subobjects.size());
    state_object_description.pSubobjects = subobjects.data();
    ComPtr<ID3D12StateObject> state_object;
    hr = impl.device->CreateStateObject(
        &state_object_description, IID_PPV_ARGS(&state_object));
    if (FAILED(hr) || !state_object) {
        code = "native-d3d12.context.state-object-create-failed";
        detail = "CreateStateObject failed with " + hresult_hex(hr) + ".";
        return false;
    }
    ComPtr<ID3D12StateObjectProperties> state_properties;
    hr = state_object.As(&state_properties);
    if (FAILED(hr) || !state_properties) {
        code = "native-d3d12.context.state-object-properties-unavailable";
        detail = "ID3D12StateObjectProperties was unavailable after CreateStateObject.";
        return false;
    }

    constexpr std::uint32_t shader_record_bytes =
        D3D12_RAYTRACING_SHADER_RECORD_BYTE_ALIGNMENT;
    constexpr std::size_t shader_table_byte_count =
        static_cast<std::size_t>(shader_record_bytes) * 6U;
    const void* raygen_identifier = state_properties->GetShaderIdentifier(L"RayGen");
    const void* miss_identifier = state_properties->GetShaderIdentifier(L"Miss");
    const void* hit_identifier = state_properties->GetShaderIdentifier(L"TriangleHitGroup");
    if (raygen_identifier == nullptr || miss_identifier == nullptr || hit_identifier == nullptr) {
        code = "native-d3d12.context.shader-identifiers-missing";
        detail = "The persistent DXR state object did not expose all three shader identifiers.";
        return false;
    }
    std::array<std::uint8_t, shader_table_byte_count> shader_table_data{};
    std::memcpy(shader_table_data.data(), raygen_identifier,
                D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES);
    std::memcpy(shader_table_data.data() + shader_record_bytes * 2U,
                miss_identifier, D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES);
    std::memcpy(shader_table_data.data() + shader_record_bytes * 4U,
                hit_identifier, D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES);
    constexpr std::uint64_t output_stride_bytes = sizeof(std::uint32_t) * 4U;
    const auto output_bytes = static_cast<std::uint64_t>(impl.options.output_width) *
        impl.options.output_height * output_stride_bytes;
    const auto shader_table_bytes = static_cast<std::uint64_t>(shader_table_data.size());
    const auto camera_constants_bytes = has_full_frame_library
        ? native_d3d12_raytracing_camera_constants_bytes
        : 0U;
    if (!resource_bytes_bounded(shader_table_bytes) || !resource_bytes_bounded(output_bytes) ||
        (has_full_frame_library &&
         !resource_bytes_bounded(camera_constants_bytes)) ||
        shader_table_bytes > impl.options.max_resource_bytes ||
        output_bytes > impl.options.max_resource_bytes - shader_table_bytes ||
        output_bytes > impl.options.max_resource_bytes - shader_table_bytes - output_bytes ||
        camera_constants_bytes > impl.options.max_resource_bytes - shader_table_bytes -
            output_bytes - output_bytes) {
        code = "native-d3d12.context.trace-resource-budget-exceeded";
        detail = "Persistent shader-table/output/camera resources exceed the configured D3D12 context budget.";
        return false;
    }
    ComPtr<ID3D12Resource> shader_table_resource;
    hr = create_committed_buffer(
        impl.device.Get(), shader_table_bytes, D3D12_HEAP_TYPE_UPLOAD,
        D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_FLAG_NONE,
        shader_table_resource);
    if (FAILED(hr) || !shader_table_resource ||
        !fill_upload_buffer(shader_table_resource.Get(), shader_table_data.data(),
                            shader_table_data.size())) {
        code = "native-d3d12.context.shader-table-upload-failed";
        detail = "Persistent shader-table upload failed with " + hresult_hex(hr) + ".";
        return false;
    }
    ComPtr<ID3D12Resource> output_resource;
    hr = create_committed_buffer(
        impl.device.Get(), output_bytes, D3D12_HEAP_TYPE_DEFAULT,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
        output_resource);
    if (FAILED(hr) || !output_resource) {
        code = "native-d3d12.context.output-create-failed";
        detail = "Persistent UAV output resource creation failed with " + hresult_hex(hr) + ".";
        return false;
    }
    ComPtr<ID3D12Resource> output_readback;
    hr = create_committed_buffer(
        impl.device.Get(), output_bytes, D3D12_HEAP_TYPE_READBACK,
        D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_FLAG_NONE, output_readback);
    if (FAILED(hr) || !output_readback) {
        code = "native-d3d12.context.readback-create-failed";
        detail = "Persistent output readback resource creation failed with " + hresult_hex(hr) + ".";
        return false;
    }
    ComPtr<ID3D12Resource> camera_constants_resource;
    if (has_full_frame_library) {
        hr = create_committed_buffer(
            impl.device.Get(), camera_constants_bytes, D3D12_HEAP_TYPE_UPLOAD,
            D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_FLAG_NONE,
            camera_constants_resource);
        if (FAILED(hr) || !camera_constants_resource) {
            code = "native-d3d12.context.camera-constants-create-failed";
            detail = "Persistent full-frame camera constant upload creation failed with " +
                hresult_hex(hr) + ".";
            return false;
        }
        if (!fill_camera_constants(camera_constants_resource.Get(), impl.camera)) {
            code = "native-d3d12.context.camera-constants-upload-failed";
            detail = "Persistent full-frame camera constant upload failed.";
            return false;
        }
    }

    impl.trace_root_signature = std::move(root_signature);
    impl.trace_state_object = std::move(state_object);
    impl.trace_state_properties = std::move(state_properties);
    impl.shader_table = std::move(shader_table_resource);
    impl.camera_constants = std::move(camera_constants_resource);
    impl.output_resource = std::move(output_resource);
    impl.output_readback = std::move(output_readback);
    impl.shader_table_bytes = shader_table_bytes;
    impl.camera_constants_bytes = camera_constants_bytes;
    impl.output_resource_bytes = output_bytes;
    impl.shader_pipeline_ready = true;
    impl.shader_table_ready = true;
    impl.camera_constants_ready = has_full_frame_library;
    impl.full_frame_shader_active = has_full_frame_library;
    impl.linear_radiance_shader_active = has_full_frame_library;
    impl.linear_radiance_shader_consumed = false;
    impl.camera_dirty = false;
    impl.output_resource_ready = true;
    impl.output_surface_resource_ready = true;
    impl.output_surface_state = NativeD3D12RayTracingOutputSurfaceState::unordered_access;
    if (impl.output_resource_generation == 0U)
        impl.output_resource_generation = 1U;
    if (impl.next_output_access_token == 0U ||
        impl.next_output_access_token == std::numeric_limits<std::uint64_t>::max())
        impl.next_output_access_token = 1U;
    impl.output_access_token = impl.next_output_access_token++;
    code = "native-d3d12.context.trace-resources-ready";
    detail = has_full_frame_library
        ? "The versioned full-frame DXR library, camera CBV, persistent root signature, state object, aligned SBT and UAV/readback resources are ready for TraceRays."
        : "Persistent root signature, marker-probe DXR state object, aligned SBT and UAV/readback resources are ready for TraceRays.";
    return true;
#endif
}

bool NativeD3D12RayTracingContext::ensure_full_frame_shading_resources(
    NativeD3D12RayTracingContext::Impl& impl,
    std::string& code,
    std::string& detail) {
#if !defined(_WIN32)
    (void)impl;
    code = "native-d3d12.context.platform-unavailable";
    detail = "The persistent D3D12 shading resources are available only on Windows.";
    return false;
#else
    if (!impl.full_frame_shader_active) return true;
    if (!impl.scene) {
        code = "native-d3d12.context.shading-scene-unavailable";
        detail = "Full-frame shading resources require a retained scene.";
        return false;
    }
    if (impl.shading_resources_ready && impl.shading_scene_hash == impl.scene_hash &&
        impl.shading_materials != nullptr && impl.shading_lighting != nullptr &&
        impl.shading_instances != nullptr && impl.shading_normals != nullptr)
        return true;

    if (impl.resolved_materials.size() != impl.scene->geometries.size()) {
        std::vector<NativeD3D12RayTracingMaterial> resolved;
        const auto validation = resolve_materials(*impl.scene, impl.options, resolved);
        if (!validation.valid) {
            code = validation.code;
            detail = validation.detail;
            return false;
        }
        impl.resolved_materials = std::move(resolved);
        impl.shading_fingerprint = shading_signature(
            impl.resolved_materials, impl.lighting);
    }

    std::vector<NativeD3D12RayTracingMaterialConstants> material_constants;
    material_constants.reserve(impl.resolved_materials.size());
    for (const auto& material : impl.resolved_materials) {
        NativeD3D12RayTracingMaterialConstants constants{};
        std::copy(material.base_color.begin(), material.base_color.end(),
                  std::begin(constants.base_color));
        constants.material_parameters[0U] = material.metallic;
        constants.material_parameters[1U] = material.roughness;
        constants.material_parameters[2U] = material.emissive_intensity;
        std::copy(material.emissive.begin(), material.emissive.end(),
                  std::begin(constants.emissive));
        material_constants.push_back(constants);
    }

    std::vector<NativeD3D12RayTracingInstanceShadingConstants> instance_constants;
    std::vector<std::array<float, 4U>> normal_constants;
    instance_constants.reserve(impl.scene->geometries.size());
    for (std::size_t instance_index = 0U;
         instance_index < impl.scene->geometries.size(); ++instance_index) {
        const auto& geometry = impl.scene->geometries[instance_index];
        NativeD3D12RayTracingInstanceShadingConstants instance{};
        instance.material_index = static_cast<std::uint32_t>(instance_index);
        instance.normal_offset = static_cast<std::uint32_t>(normal_constants.size());
        const auto triangle_count = geometry.indices.size() / 3U;
        if (triangle_count == 0U || normal_constants.size() >
                std::numeric_limits<std::uint32_t>::max() - triangle_count) {
            code = "native-d3d12.context.shading-normal-count-invalid";
            detail = "The per-instance triangle normal table exceeded its bounded index range.";
            return false;
        }
        instance.normal_count = static_cast<std::uint32_t>(triangle_count);
        for (std::size_t triangle_index = 0U;
             triangle_index < triangle_count; ++triangle_index) {
            const auto index_base = triangle_index * 3U;
            const auto index_a = geometry.indices[index_base];
            const auto index_b = geometry.indices[index_base + 1U];
            const auto index_c = geometry.indices[index_base + 2U];
            const auto position = [&geometry](const std::uint32_t index) {
                const auto base = static_cast<std::size_t>(index) * 3U;
                return std::array<float, 3U>{geometry.position_xyz[base],
                                            geometry.position_xyz[base + 1U],
                                            geometry.position_xyz[base + 2U]};
            };
            const auto first = position(index_a);
            const auto second = position(index_b);
            const auto third = position(index_c);
            const std::array<float, 3U> edge_a{
                second[0U] - first[0U], second[1U] - first[1U],
                second[2U] - first[2U]};
            const std::array<float, 3U> edge_b{
                third[0U] - first[0U], third[1U] - first[1U],
                third[2U] - first[2U]};
            auto normal = camera_cross(edge_a, edge_b);
            const auto length = std::sqrt(camera_dot(normal, normal));
            if (!std::isfinite(length) || length <= 1.0e-6F) {
                // Degenerate triangles remain valid visibility geometry; use a
                // deterministic up normal rather than emitting NaN radiance.
                normal = {0.0F, 1.0F, 0.0F};
            } else {
                normal[0U] /= length;
                normal[1U] /= length;
                normal[2U] /= length;
            }
            normal_constants.push_back({normal[0U], normal[1U], normal[2U], 0.0F});
        }
        instance_constants.push_back(instance);
    }

    NativeD3D12RayTracingLightingConstants lighting_constants{};
    lighting_constants.directional_direction[0U] = impl.lighting.directional_direction[0U];
    lighting_constants.directional_direction[1U] = impl.lighting.directional_direction[1U];
    lighting_constants.directional_direction[2U] = impl.lighting.directional_direction[2U];
    lighting_constants.directional_color_intensity[0U] = impl.lighting.directional_color[0U];
    lighting_constants.directional_color_intensity[1U] = impl.lighting.directional_color[1U];
    lighting_constants.directional_color_intensity[2U] = impl.lighting.directional_color[2U];
    lighting_constants.directional_color_intensity[3U] = impl.lighting.directional_intensity;
    lighting_constants.ambient_color_intensity[0U] = impl.lighting.ambient_color[0U];
    lighting_constants.ambient_color_intensity[1U] = impl.lighting.ambient_color[1U];
    lighting_constants.ambient_color_intensity[2U] = impl.lighting.ambient_color[2U];
    lighting_constants.ambient_color_intensity[3U] = impl.lighting.ambient_intensity;

    const auto byte_count = [](const std::size_t count,
                               const std::size_t stride) -> std::uint64_t {
        if (count == 0U || count > std::numeric_limits<std::uint64_t>::max() / stride)
            return 0U;
        return static_cast<std::uint64_t>(count) * stride;
    };
    const auto material_bytes = byte_count(
        material_constants.size(), sizeof(NativeD3D12RayTracingMaterialConstants));
    const auto lighting_bytes = static_cast<std::uint64_t>(
        D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);
    const auto instance_bytes = byte_count(
        instance_constants.size(), sizeof(NativeD3D12RayTracingInstanceShadingConstants));
    const auto normal_bytes = byte_count(
        normal_constants.size(), sizeof(std::array<float, 4U>));
    if (material_bytes == 0U || lighting_bytes == 0U || instance_bytes == 0U ||
        normal_bytes == 0U) {
        code = "native-d3d12.context.shading-resource-size-invalid";
        detail = "The full-frame material, lighting, instance or normal table was empty or overflowed.";
        return false;
    }
    std::uint64_t total_bytes = 0U;
    const auto add_bytes = [&total_bytes, &impl](const std::uint64_t bytes) {
        if (bytes == 0U || bytes > impl.options.max_resource_bytes -
                std::min(total_bytes, impl.options.max_resource_bytes))
            return false;
        total_bytes += bytes;
        return total_bytes <= impl.options.max_resource_bytes;
    };
    if (!add_bytes(material_bytes) || !add_bytes(lighting_bytes) ||
        !add_bytes(instance_bytes) || !add_bytes(normal_bytes)) {
        code = "native-d3d12.context.shading-resource-budget-exceeded";
        detail = "The full-frame material, lighting, instance and normal tables exceed the context resource budget.";
        return false;
    }

    ComPtr<ID3D12Resource> next_materials;
    HRESULT hr = create_committed_buffer(
        impl.device.Get(), material_bytes, D3D12_HEAP_TYPE_UPLOAD,
        D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_FLAG_NONE, next_materials);
    if (FAILED(hr) || !next_materials ||
        !fill_upload_buffer(next_materials.Get(), material_constants.data(),
                            static_cast<std::size_t>(material_bytes))) {
        code = "native-d3d12.context.shading-material-buffer-failed";
        detail = "The per-instance material buffer upload failed with " + hresult_hex(hr) + ".";
        return false;
    }
    ComPtr<ID3D12Resource> next_lighting;
    hr = create_committed_buffer(
        impl.device.Get(), lighting_bytes, D3D12_HEAP_TYPE_UPLOAD,
        D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_FLAG_NONE, next_lighting);
    if (FAILED(hr) || !next_lighting ||
        !fill_upload_buffer(next_lighting.Get(), &lighting_constants,
                            sizeof(lighting_constants))) {
        code = "native-d3d12.context.shading-lighting-buffer-failed";
        detail = "The directional/ambient lighting constant upload failed with " + hresult_hex(hr) + ".";
        return false;
    }
    ComPtr<ID3D12Resource> next_instances;
    hr = create_committed_buffer(
        impl.device.Get(), instance_bytes, D3D12_HEAP_TYPE_UPLOAD,
        D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_FLAG_NONE, next_instances);
    if (FAILED(hr) || !next_instances ||
        !fill_upload_buffer(next_instances.Get(), instance_constants.data(),
                            static_cast<std::size_t>(instance_bytes))) {
        code = "native-d3d12.context.shading-instance-buffer-failed";
        detail = "The per-instance normal/material mapping upload failed with " + hresult_hex(hr) + ".";
        return false;
    }
    ComPtr<ID3D12Resource> next_normals;
    hr = create_committed_buffer(
        impl.device.Get(), normal_bytes, D3D12_HEAP_TYPE_UPLOAD,
        D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_FLAG_NONE, next_normals);
    if (FAILED(hr) || !next_normals ||
        !fill_upload_buffer(next_normals.Get(), normal_constants.data(),
                            static_cast<std::size_t>(normal_bytes))) {
        code = "native-d3d12.context.shading-normal-buffer-failed";
        detail = "The per-triangle normal table upload failed with " + hresult_hex(hr) + ".";
        return false;
    }

    impl.shading_materials = std::move(next_materials);
    impl.shading_lighting = std::move(next_lighting);
    impl.shading_instances = std::move(next_instances);
    impl.shading_normals = std::move(next_normals);
    impl.shading_material_bytes = material_bytes;
    impl.shading_lighting_bytes = lighting_bytes;
    impl.shading_instance_bytes = instance_bytes;
    impl.shading_normal_bytes = normal_bytes;
    impl.shading_scene_hash = impl.scene_hash;
    impl.shading_resources_ready = true;
    code = "native-d3d12.context.shading-resources-ready";
    detail = "Per-instance linear materials, one directional light, ambient term and geometric normal table are ready for the full-frame DXR shader.";
    return true;
#endif
}

namespace {

#if defined(_WIN32)

bool select_hardware_device(IDXGIFactory4* factory, CreateDeviceFn create_device,
                            ID3D12Device5** output_device, std::string& output_name,
                            std::uint32_t& output_tier) {
    if (factory == nullptr || create_device == nullptr || output_device == nullptr) return false;
    *output_device = nullptr;
    ComPtr<IDXGIAdapter1> adapter;
    for (UINT index = 0U; factory->EnumAdapters1(index, &adapter) != DXGI_ERROR_NOT_FOUND; ++index) {
        DXGI_ADAPTER_DESC1 description{};
        if (FAILED(adapter->GetDesc1(&description))) {
            adapter.Reset();
            continue;
        }
        if ((description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0U) {
            adapter.Reset();
            continue;
        }
        ComPtr<ID3D12Device5> candidate;
        const auto hr = create_device(adapter.Get(), D3D_FEATURE_LEVEL_12_1,
                                      __uuidof(ID3D12Device5),
                                      reinterpret_cast<void**>(candidate.GetAddressOf()));
        if (FAILED(hr) || !candidate) {
            adapter.Reset();
            continue;
        }
        D3D12_FEATURE_DATA_D3D12_OPTIONS5 options5{};
        if (FAILED(candidate->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5,
                                                  &options5, sizeof(options5))) ||
            options5.RaytracingTier == D3D12_RAYTRACING_TIER_NOT_SUPPORTED) {
            adapter.Reset();
            continue;
        }
        *output_device = candidate.Detach();
        output_name = utf8_from_wide(description.Description);
        output_tier = static_cast<std::uint32_t>(options5.RaytracingTier);
        return true;
    }
    return false;
}

HRESULT submit_and_wait(ID3D12CommandQueue* queue,
                        ID3D12Fence* fence,
                        ID3D12GraphicsCommandList4* command_list,
                        HANDLE& fence_event,
                        std::uint64_t& next_fence_value,
                        std::uint64_t* submitted_fence_value = nullptr,
                        std::uint64_t* completed_fence_value = nullptr,
                        const bool wait_for_completion = true) {
    if (submitted_fence_value != nullptr) *submitted_fence_value = 0U;
    if (completed_fence_value != nullptr) *completed_fence_value = 0U;
    if (queue == nullptr || fence == nullptr || command_list == nullptr ||
        next_fence_value == 0U || next_fence_value == std::numeric_limits<std::uint64_t>::max())
        return E_INVALIDARG;
    ID3D12CommandList* command_lists[] = {command_list};
    queue->ExecuteCommandLists(1U, command_lists);
    const auto fence_value = next_fence_value++;
    HRESULT hr = queue->Signal(fence, fence_value);
    if (FAILED(hr)) return hr;
    if (submitted_fence_value != nullptr) *submitted_fence_value = fence_value;
    const auto completed_value = fence->GetCompletedValue();
    if (completed_value >= fence_value) {
        if (completed_fence_value != nullptr)
            *completed_fence_value = completed_value;
        return S_OK;
    }
    // An asynchronous submission deliberately stops after Signal.  The
    // caller owns the later poll/wait through synchronize(); no event or CPU
    // wait is introduced on the render-loop path.
    if (!wait_for_completion) {
        if (completed_fence_value != nullptr)
            *completed_fence_value = completed_value;
        return S_OK;
    }
    if (fence_event == nullptr) {
        fence_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (fence_event == nullptr) return HRESULT_FROM_WIN32(GetLastError());
    }
    hr = fence->SetEventOnCompletion(fence_value, fence_event);
    if (FAILED(hr)) return hr;
    if (WaitForSingleObject(fence_event, INFINITE) != WAIT_OBJECT_0)
        return HRESULT_FROM_WIN32(GetLastError());
    if (completed_fence_value != nullptr)
        *completed_fence_value = fence->GetCompletedValue();
    return S_OK;
}

void transition_resource(ID3D12GraphicsCommandList4* command_list,
                         ID3D12Resource* resource,
                         const D3D12_RESOURCE_STATES before,
                         const D3D12_RESOURCE_STATES after) {
    if (command_list == nullptr || resource == nullptr || before == after) return;
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = resource;
    barrier.Transition.StateBefore = before;
    barrier.Transition.StateAfter = after;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    command_list->ResourceBarrier(1U, &barrier);
}

void uav_barrier(ID3D12GraphicsCommandList4* command_list, ID3D12Resource* resource) {
    if (command_list == nullptr || resource == nullptr) return;
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    barrier.UAV.pResource = resource;
    command_list->ResourceBarrier(1U, &barrier);
}

#endif

} // namespace

std::string_view native_d3d12_raytracing_context_state_name(
    const NativeD3D12RayTracingContextState state) noexcept {
    switch (state) {
    case NativeD3D12RayTracingContextState::uninitialized: return "uninitialized";
    case NativeD3D12RayTracingContextState::ready: return "ready";
    case NativeD3D12RayTracingContextState::unsupported: return "unsupported";
    case NativeD3D12RayTracingContextState::failed: return "failed";
    case NativeD3D12RayTracingContextState::shutdown: return "shutdown";
    }
    return "failed";
}

std::string_view native_d3d12_raytracing_context_failure_stage_name(
    const NativeD3D12RayTracingContextFailureStage stage) noexcept {
    switch (stage) {
    case NativeD3D12RayTracingContextFailureStage::none: return "none";
    case NativeD3D12RayTracingContextFailureStage::platform: return "platform";
    case NativeD3D12RayTracingContextFailureStage::loader: return "loader";
    case NativeD3D12RayTracingContextFailureStage::factory: return "factory";
    case NativeD3D12RayTracingContextFailureStage::adapter: return "adapter";
    case NativeD3D12RayTracingContextFailureStage::device: return "device";
    case NativeD3D12RayTracingContextFailureStage::feature: return "feature";
    case NativeD3D12RayTracingContextFailureStage::command_queue: return "command-queue";
    case NativeD3D12RayTracingContextFailureStage::command_allocator: return "command-allocator";
    case NativeD3D12RayTracingContextFailureStage::command_list: return "command-list";
    case NativeD3D12RayTracingContextFailureStage::fence: return "fence";
    case NativeD3D12RayTracingContextFailureStage::camera: return "camera";
    case NativeD3D12RayTracingContextFailureStage::shading: return "shading";
    case NativeD3D12RayTracingContextFailureStage::scene: return "scene";
    case NativeD3D12RayTracingContextFailureStage::blas: return "blas";
    case NativeD3D12RayTracingContextFailureStage::tlas: return "tlas";
    case NativeD3D12RayTracingContextFailureStage::shader_pipeline: return "shader-pipeline";
    case NativeD3D12RayTracingContextFailureStage::shader_table: return "shader-table";
    case NativeD3D12RayTracingContextFailureStage::output: return "output";
    case NativeD3D12RayTracingContextFailureStage::trace: return "trace";
    case NativeD3D12RayTracingContextFailureStage::readback: return "readback";
    case NativeD3D12RayTracingContextFailureStage::synchronization: return "synchronization";
    case NativeD3D12RayTracingContextFailureStage::cleanup: return "cleanup";
    }
    return "cleanup";
}

std::string_view native_d3d12_raytracing_output_surface_state_name(
    const NativeD3D12RayTracingOutputSurfaceState state) noexcept {
    switch (state) {
    case NativeD3D12RayTracingOutputSurfaceState::unavailable: return "unavailable";
    case NativeD3D12RayTracingOutputSurfaceState::unordered_access: return "unordered-access";
    case NativeD3D12RayTracingOutputSurfaceState::copy_source: return "copy-source";
    case NativeD3D12RayTracingOutputSurfaceState::released: return "released";
    }
    return "unavailable";
}

std::string_view native_d3d12_raytracing_synchronization_policy_name(
    const NativeD3D12RayTracingSynchronizationPolicy policy) noexcept {
    switch (policy) {
    case NativeD3D12RayTracingSynchronizationPolicy::wait_for_completion:
        return "wait-for-completion";
    case NativeD3D12RayTracingSynchronizationPolicy::submit_only:
        return "submit-only";
    }
    return "wait-for-completion";
}

NativeD3D12RayTracingContext::NativeD3D12RayTracingContext(
    NativeD3D12RayTracingContextOptions options)
    : impl_(std::make_unique<Impl>(std::move(options))) {}

NativeD3D12RayTracingContext::~NativeD3D12RayTracingContext() {
    static_cast<void>(shutdown());
}

NativeD3D12RayTracingContextReceipt NativeD3D12RayTracingContext::initialize() {
    if (impl_->state == NativeD3D12RayTracingContextState::shutdown) {
        save_result(*impl_, NativeD3D12RayTracingContextFailureStage::cleanup,
                    "native-d3d12.context.already-shutdown",
                    "The D3D12 context was already shut down and cannot be reinitialized.");
        auto result = receipt_from(*impl_, "initialize");
        result.state = NativeD3D12RayTracingContextState::shutdown;
        return result;
    }
    if (impl_->state != NativeD3D12RayTracingContextState::uninitialized) {
        save_result(*impl_, NativeD3D12RayTracingContextFailureStage::none,
                    "native-d3d12.context.already-initialized",
                    "The D3D12 device, queue and fence context is already initialized.");
        return receipt_from(*impl_, "initialize");
    }
    if (impl_->options.output_width == 0U || impl_->options.output_height == 0U) {
        impl_->state = NativeD3D12RayTracingContextState::failed;
        save_result(*impl_, NativeD3D12RayTracingContextFailureStage::output,
                    "native-d3d12.context.output-size-invalid",
                    "Output dimensions must be positive and bounded.");
        return receipt_from(*impl_, "initialize");
    }
    const auto camera_validation = validate_camera(impl_->camera);
    if (!camera_validation.valid) {
        impl_->state = NativeD3D12RayTracingContextState::failed;
        impl_->camera_valid = false;
        impl_->camera_fingerprint = 0U;
        save_result(*impl_, NativeD3D12RayTracingContextFailureStage::camera,
                    camera_validation.code, camera_validation.detail);
        return receipt_from(*impl_, "initialize");
    }
    impl_->camera_valid = true;
    impl_->camera_fingerprint = camera_signature(impl_->camera);
    const auto lighting_validation = validate_lighting(impl_->lighting);
    if (!lighting_validation.valid) {
        impl_->state = NativeD3D12RayTracingContextState::failed;
        save_result(*impl_, NativeD3D12RayTracingContextFailureStage::shading,
                    lighting_validation.code, lighting_validation.detail);
        return receipt_from(*impl_, "initialize");
    }
    const auto shader_bytes = static_cast<std::uint64_t>(
        impl_->options.shaders.ray_generation_dxil.size()) +
        static_cast<std::uint64_t>(impl_->options.shaders.miss_dxil.size()) +
        static_cast<std::uint64_t>(impl_->options.shaders.closest_hit_dxil.size());
    if (shader_bytes > impl_->options.max_resource_bytes) {
        impl_->state = NativeD3D12RayTracingContextState::failed;
        save_result(*impl_, NativeD3D12RayTracingContextFailureStage::shader_pipeline,
                    "native-d3d12.context.shader-budget-exceeded",
                    "The supplied DXIL shader set exceeds the bounded context resource budget.");
        return receipt_from(*impl_, "initialize");
    }
    impl_->borrowed_device_requested = impl_->options.borrowed_device != nullptr;
    impl_->borrowed_command_queue_requested =
        impl_->options.borrowed_command_queue != nullptr;

#if !defined(_WIN32)
    impl_->state = NativeD3D12RayTracingContextState::unsupported;
    save_result(*impl_, NativeD3D12RayTracingContextFailureStage::platform,
                "native-d3d12.context.platform-unavailable",
                "The persistent D3D12 context is available only on Windows; raster fallback remains explicit.");
    auto result = receipt_from(*impl_, "initialize");
    result.state = NativeD3D12RayTracingContextState::unsupported;
    result.fallback_active = true;
    return result;
#else
    if (impl_->borrowed_command_queue_requested &&
        !impl_->borrowed_device_requested) {
        impl_->state = NativeD3D12RayTracingContextState::failed;
        save_result(*impl_, NativeD3D12RayTracingContextFailureStage::device,
                    "native-d3d12.context.borrowed-queue-without-device",
                    "A borrowed command queue is only valid when its matching borrowed D3D12 device is supplied.");
        return receipt_from(*impl_, "initialize");
    }
    if (!impl_->d3d12_module.load() ||
        (!impl_->borrowed_device_requested && !impl_->dxgi_module.load())) {
        impl_->state = NativeD3D12RayTracingContextState::unsupported;
        save_result(*impl_, NativeD3D12RayTracingContextFailureStage::loader,
                    "native-d3d12.context.loader-unavailable",
                    "d3d12.dll or dxgi.dll could not be loaded; no native handles were retained.");
        auto result = receipt_from(*impl_, "initialize");
        result.state = NativeD3D12RayTracingContextState::unsupported;
        result.fallback_active = true;
        return result;
    }
    HRESULT hr = S_OK;
    if (impl_->borrowed_device_requested) {
        auto* borrowed_unknown = reinterpret_cast<IUnknown*>(impl_->options.borrowed_device);
        Microsoft::WRL::ComPtr<ID3D12Device5> borrowed_device;
        hr = borrowed_unknown == nullptr
            ? E_INVALIDARG
            : borrowed_unknown->QueryInterface(
                __uuidof(ID3D12Device5),
                reinterpret_cast<void**>(borrowed_device.GetAddressOf()));
        if (FAILED(hr) || !borrowed_device) {
            impl_->state = NativeD3D12RayTracingContextState::failed;
            save_result(*impl_, NativeD3D12RayTracingContextFailureStage::device,
                        "native-d3d12.context.borrowed-device-query-failed",
                        "The borrowed SDL_GPU D3D12 device did not expose ID3D12Device5 (" +
                            hresult_hex(hr) + ").");
            return receipt_from(*impl_, "initialize");
        }
        D3D12_FEATURE_DATA_D3D12_OPTIONS5 options5{};
        hr = borrowed_device->CheckFeatureSupport(
            D3D12_FEATURE_D3D12_OPTIONS5, &options5, sizeof(options5));
        if (FAILED(hr) ||
            options5.RaytracingTier == D3D12_RAYTRACING_TIER_NOT_SUPPORTED) {
            impl_->state = NativeD3D12RayTracingContextState::unsupported;
            save_result(*impl_, NativeD3D12RayTracingContextFailureStage::feature,
                        "native-d3d12.context.borrowed-device-raytracing-unsupported",
                        "The borrowed SDL_GPU D3D12 device has no supported ray-tracing tier; a second device is not created.");
            auto result = receipt_from(*impl_, "initialize");
            result.state = NativeD3D12RayTracingContextState::unsupported;
            result.fallback_active = true;
            return result;
        }
        impl_->device = std::move(borrowed_device);
        impl_->device_adopted = true;
        impl_->shared_device = true;
        impl_->device_name = "borrowed-sdl-gpu-d3d12-device";
        impl_->raytracing_tier = static_cast<std::uint32_t>(options5.RaytracingTier);
    } else {
        const auto create_device = impl_->d3d12_module.symbol<CreateDeviceFn>("D3D12CreateDevice");
        const auto create_factory = impl_->dxgi_module.symbol<CreateFactoryFn>("CreateDXGIFactory2");
        if (create_device == nullptr || create_factory == nullptr) {
            impl_->state = NativeD3D12RayTracingContextState::unsupported;
            save_result(*impl_, NativeD3D12RayTracingContextFailureStage::loader,
                        "native-d3d12.context.entrypoint-unavailable",
                        "The D3D12CreateDevice or CreateDXGIFactory2 entry point is unavailable.");
            auto result = receipt_from(*impl_, "initialize");
            result.state = NativeD3D12RayTracingContextState::unsupported;
            result.fallback_active = true;
            return result;
        }
        hr = create_factory(0U, __uuidof(IDXGIFactory4),
                            reinterpret_cast<void**>(impl_->factory.GetAddressOf()));
        if (FAILED(hr) || !impl_->factory) {
            impl_->state = NativeD3D12RayTracingContextState::failed;
            save_result(*impl_, NativeD3D12RayTracingContextFailureStage::factory,
                        "native-d3d12.context.factory-create-failed",
                        "CreateDXGIFactory2 failed with " + hresult_hex(hr) + ".");
            return receipt_from(*impl_, "initialize");
        }
        hr = select_hardware_device(impl_->factory.Get(), create_device,
                                    impl_->device.GetAddressOf(), impl_->device_name,
                                    impl_->raytracing_tier)
            ? S_OK : DXGI_ERROR_UNSUPPORTED;
        if (FAILED(hr) || !impl_->device) {
            impl_->state = NativeD3D12RayTracingContextState::unsupported;
            save_result(*impl_, NativeD3D12RayTracingContextFailureStage::feature,
                        "native-d3d12.context.hardware-unsupported",
                        impl_->options.probe_warp_fallback
                            ? "No hardware D3D12 ray-tracing device was available; WARP is recorded only as a raster fallback."
                            : "No hardware D3D12 device with a ray-tracing tier was available.");
            auto result = receipt_from(*impl_, "initialize");
            result.state = NativeD3D12RayTracingContextState::unsupported;
            result.fallback_active = true;
            return result;
        }
    }
    if (impl_->borrowed_command_queue_requested) {
        auto* borrowed_unknown = reinterpret_cast<IUnknown*>(
            impl_->options.borrowed_command_queue);
        ComPtr<ID3D12CommandQueue> borrowed_queue;
        hr = borrowed_unknown == nullptr
            ? E_INVALIDARG
            : borrowed_unknown->QueryInterface(
                __uuidof(ID3D12CommandQueue),
                reinterpret_cast<void**>(borrowed_queue.GetAddressOf()));
        if (FAILED(hr) || !borrowed_queue) {
            impl_->state = NativeD3D12RayTracingContextState::failed;
            save_result(*impl_, NativeD3D12RayTracingContextFailureStage::command_queue,
                        "native-d3d12.context.borrowed-queue-query-failed",
                        "The borrowed SDL_GPU D3D12 command queue did not expose ID3D12CommandQueue (" +
                            hresult_hex(hr) + ").");
            return receipt_from(*impl_, "initialize");
        }
        const auto queue_description = borrowed_queue->GetDesc();
        if (queue_description.Type != D3D12_COMMAND_LIST_TYPE_DIRECT) {
            impl_->state = NativeD3D12RayTracingContextState::failed;
            save_result(*impl_, NativeD3D12RayTracingContextFailureStage::command_queue,
                        "native-d3d12.context.borrowed-queue-type-invalid",
                        "The borrowed SDL_GPU command queue is not a direct queue and cannot record ray-tracing work.");
            return receipt_from(*impl_, "initialize");
        }
        ComPtr<ID3D12Device> queue_device;
        hr = borrowed_queue->GetDevice(
            __uuidof(ID3D12Device),
            reinterpret_cast<void**>(queue_device.GetAddressOf()));
        if (FAILED(hr) || !queue_device) {
            impl_->state = NativeD3D12RayTracingContextState::failed;
            save_result(*impl_, NativeD3D12RayTracingContextFailureStage::command_queue,
                        "native-d3d12.context.borrowed-queue-device-query-failed",
                        "The borrowed SDL_GPU command queue did not expose its owning D3D12 device (" +
                            hresult_hex(hr) + ").");
            return receipt_from(*impl_, "initialize");
        }
        ComPtr<IUnknown> queue_identity;
        ComPtr<IUnknown> context_identity;
        hr = queue_device->QueryInterface(
            __uuidof(IUnknown),
            reinterpret_cast<void**>(queue_identity.GetAddressOf()));
        if (SUCCEEDED(hr)) {
            hr = impl_->device->QueryInterface(
                __uuidof(IUnknown),
                reinterpret_cast<void**>(context_identity.GetAddressOf()));
        }
        if (FAILED(hr) || !queue_identity || !context_identity ||
            queue_identity.Get() != context_identity.Get()) {
            impl_->state = NativeD3D12RayTracingContextState::failed;
            save_result(*impl_, NativeD3D12RayTracingContextFailureStage::command_queue,
                        "native-d3d12.context.borrowed-queue-device-mismatch",
                        "The borrowed SDL_GPU command queue belongs to a different D3D12 device; no cross-device resource path is attempted.");
            return receipt_from(*impl_, "initialize");
        }
        impl_->command_queue = std::move(borrowed_queue);
        impl_->command_queue_adopted = true;
        impl_->shared_command_queue = true;
    } else {
        D3D12_COMMAND_QUEUE_DESC queue_description{};
        queue_description.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        hr = impl_->device->CreateCommandQueue(&queue_description,
                                               IID_PPV_ARGS(&impl_->command_queue));
        if (FAILED(hr) || !impl_->command_queue) {
            impl_->state = NativeD3D12RayTracingContextState::failed;
            save_result(*impl_, NativeD3D12RayTracingContextFailureStage::command_queue,
                        "native-d3d12.context.command-queue-create-failed",
                        "CreateCommandQueue failed with " + hresult_hex(hr) + ".");
            return receipt_from(*impl_, "initialize");
        }
    }
    hr = impl_->device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                IID_PPV_ARGS(&impl_->command_allocator));
    if (FAILED(hr) || !impl_->command_allocator) {
        impl_->state = NativeD3D12RayTracingContextState::failed;
        save_result(*impl_, NativeD3D12RayTracingContextFailureStage::command_allocator,
                    "native-d3d12.context.command-allocator-create-failed",
                    "CreateCommandAllocator failed with " + hresult_hex(hr) + ".");
        return receipt_from(*impl_, "initialize");
    }
    hr = impl_->device->CreateCommandList(0U, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                          impl_->command_allocator.Get(), nullptr,
                                          IID_PPV_ARGS(&impl_->command_list));
    if (FAILED(hr) || !impl_->command_list || FAILED(impl_->command_list->Close())) {
        impl_->state = NativeD3D12RayTracingContextState::failed;
        save_result(*impl_, NativeD3D12RayTracingContextFailureStage::command_list,
                    "native-d3d12.context.command-list-create-failed",
                    "CreateCommandList or its initial Close failed with " + hresult_hex(hr) + ".");
        return receipt_from(*impl_, "initialize");
    }
    hr = impl_->device->CreateFence(0U, D3D12_FENCE_FLAG_NONE,
                                    IID_PPV_ARGS(&impl_->fence));
    if (FAILED(hr) || !impl_->fence) {
        impl_->state = NativeD3D12RayTracingContextState::failed;
        save_result(*impl_, NativeD3D12RayTracingContextFailureStage::fence,
                    "native-d3d12.context.fence-create-failed",
                    "CreateFence failed with " + hresult_hex(hr) + ".");
        return receipt_from(*impl_, "initialize");
    }
    impl_->generation = 1U;
    impl_->state = NativeD3D12RayTracingContextState::ready;
    save_result(*impl_, NativeD3D12RayTracingContextFailureStage::none,
                "native-d3d12.context.initialized",
                "Persistent D3D12 device, direct queue, command allocator/list and fence are retained; BLAS/TLAS materialization is available while shader/SBT/trace remain separate capability gates.");
    return receipt_from(*impl_, "initialize");
#endif
}

NativeD3D12RayTracingContextReceipt NativeD3D12RayTracingContext::set_camera(
    const NativeD3D12RayTracingCamera& camera) {
    if (impl_->state == NativeD3D12RayTracingContextState::shutdown) {
        save_result(*impl_, NativeD3D12RayTracingContextFailureStage::cleanup,
                    "native-d3d12.context.already-shutdown",
                    "The D3D12 context was shut down; a new camera cannot be attached.");
        auto result = receipt_from(*impl_, "set-camera");
        result.state = NativeD3D12RayTracingContextState::shutdown;
        return result;
    }
    const auto validation = validate_camera(camera);
    if (!validation.valid) {
        save_result(*impl_, NativeD3D12RayTracingContextFailureStage::camera,
                    validation.code, validation.detail);
        auto result = receipt_from(*impl_, "set-camera");
        result.state = NativeD3D12RayTracingContextState::failed;
        result.fallback_active = false;
        return result;
    }
    if (impl_->state == NativeD3D12RayTracingContextState::uninitialized)
        static_cast<void>(initialize());
    if (impl_->state == NativeD3D12RayTracingContextState::shutdown) {
        save_result(*impl_, NativeD3D12RayTracingContextFailureStage::cleanup,
                    "native-d3d12.context.already-shutdown",
                    "The D3D12 context was shut down while initializing the camera.");
        auto result = receipt_from(*impl_, "set-camera");
        result.state = NativeD3D12RayTracingContextState::shutdown;
        return result;
    }
    if (impl_->state == NativeD3D12RayTracingContextState::failed) {
        auto result = receipt_from(*impl_, "set-camera");
        result.state = NativeD3D12RayTracingContextState::failed;
        return result;
    }
    const auto fingerprint = camera_signature(camera);
    const bool changed = !impl_->camera_valid || impl_->camera_fingerprint != fingerprint;
    if (changed) {
        impl_->camera = camera;
        impl_->options.camera = camera;
        impl_->camera_valid = true;
        impl_->camera_fingerprint = fingerprint;
        impl_->camera_dirty = true;
        impl_->camera_shader_consumed = false;
        // A view produced under the old camera must not remain consumable even
        // though its scene and persistent AS resources are still reusable.
        impl_->output_surface_trace_completed = false;
        impl_->output_access_token = 0U;
#if defined(_WIN32)
        impl_->last_trace_completed = false;
        impl_->last_readback_completed = false;
        impl_->last_output_copy_submitted = false;
        impl_->last_output_copy_completed = false;
#endif
    }
    save_result(*impl_, NativeD3D12RayTracingContextFailureStage::none,
                changed ? "native-d3d12.context.camera-updated"
                        : "native-d3d12.context.camera-unchanged",
                changed
                    ? "The validated world-space camera will be uploaded before the next full-frame TraceRays dispatch."
                    : "The camera fingerprint is unchanged; the retained camera constants can be reused.");
    auto result = receipt_from(*impl_, "set-camera");
    result.state = impl_->state;
    result.fallback_active = impl_->state == NativeD3D12RayTracingContextState::unsupported;
    return result;
}

NativeD3D12RayTracingContextReceipt NativeD3D12RayTracingContext::set_shading(
    const NativeD3D12RayTracingLighting& lighting,
    const std::vector<NativeD3D12RayTracingMaterial>& materials) {
    if (impl_->state == NativeD3D12RayTracingContextState::shutdown) {
        save_result(*impl_, NativeD3D12RayTracingContextFailureStage::cleanup,
                    "native-d3d12.context.already-shutdown",
                    "The D3D12 context was shut down; new shading inputs cannot be attached.");
        auto result = receipt_from(*impl_, "set-shading");
        result.state = NativeD3D12RayTracingContextState::shutdown;
        return result;
    }
    if (!impl_->scene) {
        save_result(*impl_, NativeD3D12RayTracingContextFailureStage::shading,
                    "native-d3d12.context.shading-scene-unavailable",
                    "Attach a canonical scene before mapping materials to geometry ids.");
        auto result = receipt_from(*impl_, "set-shading");
        result.state = NativeD3D12RayTracingContextState::failed;
        result.fallback_active = false;
        return result;
    }

    auto candidate_options = impl_->options;
    candidate_options.lighting = lighting;
    candidate_options.materials = materials;
    std::vector<NativeD3D12RayTracingMaterial> resolved;
    const auto validation = resolve_materials(*impl_->scene, candidate_options, resolved);
    if (!validation.valid) {
        save_result(*impl_, NativeD3D12RayTracingContextFailureStage::shading,
                    validation.code, validation.detail);
        auto result = receipt_from(*impl_, "set-shading");
        result.state = NativeD3D12RayTracingContextState::failed;
        result.fallback_active = false;
        return result;
    }

    const auto fingerprint = shading_signature(resolved, lighting);
    const bool changed = fingerprint != impl_->shading_fingerprint;
    if (changed) {
#if defined(_WIN32)
        // The upload resources can still be referenced by a submitted trace.
        // Shading changes are uncommon structural hand-offs, so wait here and
        // keep steady-state frame submission free of unconditional stalls.
        if (impl_->pending_fence_value != 0U) {
            const auto synchronized = synchronize(true);
            if (impl_->pending_fence_value != 0U ||
                synchronized.state == NativeD3D12RayTracingContextState::failed) {
                auto result = receipt_from(*impl_, "set-shading");
                result.state = NativeD3D12RayTracingContextState::failed;
                return result;
            }
        }
#endif
        impl_->options.lighting = lighting;
        impl_->options.materials = materials;
        impl_->lighting = lighting;
        impl_->resolved_materials = std::move(resolved);
        impl_->shading_fingerprint = fingerprint;
        impl_->shading_resources_ready = false;
        impl_->shading_scene_hash = 0U;
        impl_->linear_radiance_shader_consumed = false;
        impl_->output_surface_trace_completed = false;
        impl_->output_access_token = 0U;
#if defined(_WIN32)
        impl_->shading_materials.Reset();
        impl_->shading_lighting.Reset();
        impl_->shading_instances.Reset();
        impl_->shading_normals.Reset();
        impl_->shading_material_bytes = 0U;
        impl_->shading_lighting_bytes = 0U;
        impl_->shading_instance_bytes = 0U;
        impl_->shading_normal_bytes = 0U;
        impl_->last_trace_completed = false;
        impl_->last_readback_completed = false;
        impl_->last_output_copy_submitted = false;
        impl_->last_output_copy_completed = false;
        impl_->output_radiance_valid = false;
        impl_->output_radiance_probe = {};
#endif
    }
    save_result(*impl_, NativeD3D12RayTracingContextFailureStage::none,
                changed ? "native-d3d12.context.shading-updated"
                        : "native-d3d12.context.shading-unchanged",
                changed
                    ? "Validated scene-linear materials and lighting will be uploaded before the next full-frame trace without rebuilding acceleration structures."
                    : "The shading fingerprint is unchanged; retained shading resources can be reused.");
    auto result = receipt_from(*impl_, "set-shading");
    result.state = impl_->state;
    result.fallback_active = impl_->state == NativeD3D12RayTracingContextState::unsupported;
    return result;
}

NativeD3D12RayTracingContextReceipt NativeD3D12RayTracingContext::ensure_scene(
    const NativeD3D12RayTracingScene& input_scene) {
    auto canonical_scene = input_scene;
    sort_scene(canonical_scene);
    const auto validation = validate_scene(canonical_scene, impl_->options);
    if (!validation.valid) {
        save_result(*impl_, NativeD3D12RayTracingContextFailureStage::scene,
                    validation.code, validation.detail);
        auto result = receipt_from(*impl_, "ensure-scene");
        result.state = NativeD3D12RayTracingContextState::failed;
        result.fallback_active = false;
        return result;
    }
    std::vector<NativeD3D12RayTracingMaterial> resolved_materials;
    const auto shading_validation = resolve_materials(
        canonical_scene, impl_->options, resolved_materials);
    if (!shading_validation.valid) {
        save_result(*impl_, NativeD3D12RayTracingContextFailureStage::shading,
                    shading_validation.code, shading_validation.detail);
        auto result = receipt_from(*impl_, "ensure-scene");
        result.state = NativeD3D12RayTracingContextState::failed;
        result.fallback_active = false;
        return result;
    }
    if (impl_->state == NativeD3D12RayTracingContextState::uninitialized) {
        static_cast<void>(initialize());
    }
    if (impl_->state == NativeD3D12RayTracingContextState::shutdown) {
        save_result(*impl_, NativeD3D12RayTracingContextFailureStage::cleanup,
                    "native-d3d12.context.already-shutdown",
                    "The D3D12 context was shut down; a new scene cannot be attached.");
        auto result = receipt_from(*impl_, "ensure-scene");
        result.state = NativeD3D12RayTracingContextState::shutdown;
        return result;
    }
#if defined(_WIN32)
    // Scene replacement can invalidate every resource referenced by an
    // in-flight trace.  A structural hand-off is therefore an explicit wait
    // boundary, while the steady-state trace path remains submit-only when
    // requested by the caller.
    if (impl_->pending_fence_value != 0U) {
        const auto synchronized = synchronize(true);
        if (impl_->pending_fence_value != 0U ||
            synchronized.state == NativeD3D12RayTracingContextState::failed) {
            auto result = receipt_from(*impl_, "ensure-scene");
            result.state = NativeD3D12RayTracingContextState::failed;
            return result;
        }
    }
#endif

    const auto signature = scene_signature(canonical_scene);
    const bool changed = !impl_->scene || impl_->scene_hash != signature;
    if (changed) {
        impl_->scene = std::move(canonical_scene);
        impl_->scene_hash = signature;
        impl_->scene_dirty = true;
        impl_->resolved_materials = std::move(resolved_materials);
        impl_->shading_fingerprint = shading_signature(
            impl_->resolved_materials, impl_->lighting);
        impl_->shading_resources_ready = false;
        impl_->shading_scene_hash = 0U;
        impl_->linear_radiance_shader_consumed = false;
        // A previously published output belongs to the old scene.  Keep the
        // allocation alive for reuse, but invalidate its borrowed view until
        // a new TraceRays completion publishes a fresh access token.
        impl_->output_surface_trace_completed = false;
#if defined(_WIN32)
        impl_->last_trace_completed = false;
        impl_->last_readback_completed = false;
        impl_->last_output_copy_submitted = false;
        impl_->last_output_copy_completed = false;
#endif
        if (impl_->scene_generation != std::numeric_limits<std::uint64_t>::max())
            ++impl_->scene_generation;
    }
    save_result(*impl_, NativeD3D12RayTracingContextFailureStage::none,
                changed ? "native-d3d12.context.scene-changed" : "native-d3d12.context.scene-unchanged",
        changed ? "The bounded scene snapshot is retained for a later AS build/update operation."
                        : "The scene fingerprint is unchanged; persistent native resources can be reused without a new AS submission.");
    auto result = receipt_from(*impl_, "ensure-scene");
    result.scene_changed = changed;
    result.scene_received = true;
    if (impl_->state == NativeD3D12RayTracingContextState::unsupported) {
        result.state = NativeD3D12RayTracingContextState::unsupported;
        result.fallback_active = true;
    }
    return result;
}

NativeD3D12RayTracingContextReceipt NativeD3D12RayTracingContext::build_or_update() {
    if (impl_->state == NativeD3D12RayTracingContextState::uninitialized)
        static_cast<void>(initialize());
#if defined(_WIN32)
    impl_->last_build_submitted = false;
    impl_->last_build_completed = false;
    impl_->last_update_submitted = false;
    impl_->last_update_completed = false;
    impl_->last_synchronization_completed = false;
#endif
    if (!impl_->scene) {
        save_result(*impl_, NativeD3D12RayTracingContextFailureStage::scene,
                    "native-d3d12.context.scene-not-provided",
                    "Call ensure_scene with a validated triangle scene before build_or_update.");
        auto result = receipt_from(*impl_, "build-or-update");
        result.state = NativeD3D12RayTracingContextState::failed;
        result.fallback_active = false;
        return result;
    }
    if (impl_->state != NativeD3D12RayTracingContextState::ready) {
        auto result = receipt_from(*impl_, "build-or-update");
        result.scene_received = true;
        if (impl_->state == NativeD3D12RayTracingContextState::unsupported) {
            save_result(*impl_, NativeD3D12RayTracingContextFailureStage::blas,
                        "native-d3d12.context.as-materialization-unavailable",
                        "Hardware D3D12 ray-tracing resources are unavailable; the caller must use the explicit raster fallback.");
            result = receipt_from(*impl_, "build-or-update");
            result.state = NativeD3D12RayTracingContextState::unsupported;
            result.fallback_active = true;
        }
        return result;
    }
#if defined(_WIN32)
    // The single persistent allocator/list cannot be reset while a previous
    // asynchronous trace or output copy still references it.  Building a new
    // AS is a deliberate synchronization boundary, unlike the normal trace
    // submission path.
    if (impl_->pending_fence_value != 0U) {
        const auto synchronized = synchronize(true);
        if (impl_->pending_fence_value != 0U ||
            synchronized.state == NativeD3D12RayTracingContextState::failed) {
            auto result = receipt_from(*impl_, "build-or-update");
            result.state = NativeD3D12RayTracingContextState::failed;
            result.scene_received = true;
            return result;
        }
    }
#endif

#if !defined(_WIN32)
    auto result = receipt_from(*impl_, "build-or-update");
    result.scene_received = true;
    mark_unsupported(*impl_, NativeD3D12RayTracingContextFailureStage::platform,
                     "build-or-update",
                     "native-d3d12.context.platform-unavailable",
                     "The persistent D3D12 context is unavailable on this platform; no GPU resource is reported ready.",
                     result);
    return result;
#else
    const auto topology_hash = scene_topology_signature(*impl_->scene);
    const bool has_compatible_resources =
        impl_->blas_ready && impl_->tlas_ready &&
        impl_->built_topology_hash == topology_hash &&
        impl_->geometry_resources.size() == impl_->scene->geometries.size() &&
        impl_->instance_buffer != nullptr && impl_->tlas_result != nullptr &&
        impl_->tlas_scratch != nullptr;
    const bool can_update = has_compatible_resources && impl_->scene->allow_update &&
        std::ranges::all_of(impl_->scene->geometries,
                            [](const auto& geometry) { return geometry.allow_update; }) &&
        std::ranges::all_of(impl_->geometry_resources,
                            [](const auto& geometry) { return geometry.allow_update; });

    if (!impl_->scene_dirty && impl_->built_scene_hash == impl_->scene_hash &&
        has_compatible_resources) {
        save_result(*impl_, NativeD3D12RayTracingContextFailureStage::none,
                    "native-d3d12.context.resources-reused",
                    "The persistent vertex/index buffers, BLAS, TLAS and instance descriptors were reused without a new GPU submission.");
        auto result = receipt_from(*impl_, "build-or-update");
        result.scene_received = true;
        result.state = NativeD3D12RayTracingContextState::ready;
        result.synchronization_completed = true;
        return result;
    }

    const auto fail = [&](const NativeD3D12RayTracingContextFailureStage stage,
                          const std::string_view code,
                          const std::string_view detail) {
        impl_->state = NativeD3D12RayTracingContextState::failed;
        save_result(*impl_, stage, code, detail);
        auto result = receipt_from(*impl_, "build-or-update");
        result.state = NativeD3D12RayTracingContextState::failed;
        result.fallback_active = false;
        result.scene_received = true;
        return result;
    };

    const bool rebuild = !can_update;
    std::vector<Impl::GeometryResources> next_geometry_resources;
    ComPtr<ID3D12Resource> next_instance_buffer;
    ComPtr<ID3D12Resource> next_tlas_result;
    ComPtr<ID3D12Resource> next_tlas_scratch;
    std::uint64_t next_tlas_result_bytes = 0U;
    std::uint64_t next_tlas_scratch_bytes = 0U;
    std::uint64_t total_resource_bytes = 0U;
    const auto resource_budget = impl_->options.max_resource_bytes;
    const auto add_resource_budget = [&](const std::uint64_t bytes) {
        if (bytes == 0U || bytes > resource_budget -
                std::min(total_resource_bytes, resource_budget))
            return false;
        total_resource_bytes += bytes;
        return total_resource_bytes <= resource_budget;
    };

    if (rebuild) {
        next_geometry_resources.reserve(impl_->scene->geometries.size());
        for (const auto& geometry : impl_->scene->geometries) {
            Impl::GeometryResources resources;
            const auto vertex_bytes = static_cast<std::uint64_t>(
                geometry.position_xyz.size()) * sizeof(float);
            const auto index_bytes = static_cast<std::uint64_t>(
                geometry.indices.size()) * sizeof(std::uint32_t);
            resources.vertex_bytes = vertex_bytes;
            resources.index_bytes = index_bytes;
            resources.vertex_count = static_cast<std::uint32_t>(
                geometry.position_xyz.size() / 3U);
            resources.index_count = static_cast<std::uint32_t>(geometry.indices.size());
            resources.allow_update = impl_->scene->allow_update && geometry.allow_update;
            if (!add_resource_budget(vertex_bytes) || !add_resource_budget(vertex_bytes) ||
                !add_resource_budget(index_bytes) || !add_resource_budget(index_bytes)) {
                return fail(NativeD3D12RayTracingContextFailureStage::scene,
                            "native-d3d12.context.resource-budget-exceeded",
                            "Persistent vertex/index resources exceed the configured D3D12 context budget.");
            }
            HRESULT hr = create_committed_buffer(
                impl_->device.Get(), vertex_bytes, D3D12_HEAP_TYPE_DEFAULT,
                D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_FLAG_NONE,
                resources.vertex_buffer);
            if (FAILED(hr) || !resources.vertex_buffer) {
                return fail(NativeD3D12RayTracingContextFailureStage::scene,
                            "native-d3d12.context.vertex-buffer-create-failed",
                            "Persistent default-heap vertex buffer creation failed with " + hresult_hex(hr) + ".");
            }
            hr = create_committed_buffer(
                impl_->device.Get(), vertex_bytes, D3D12_HEAP_TYPE_UPLOAD,
                D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_FLAG_NONE,
                resources.vertex_upload);
            if (FAILED(hr) || !resources.vertex_upload ||
                !fill_upload_buffer(resources.vertex_upload.Get(),
                                    geometry.position_xyz.data(),
                                    static_cast<std::size_t>(vertex_bytes))) {
                return fail(NativeD3D12RayTracingContextFailureStage::scene,
                            "native-d3d12.context.vertex-upload-failed",
                            "Persistent upload-heap vertex initialization failed with " + hresult_hex(hr) + ".");
            }
            hr = create_committed_buffer(
                impl_->device.Get(), index_bytes, D3D12_HEAP_TYPE_DEFAULT,
                D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_FLAG_NONE,
                resources.index_buffer);
            if (FAILED(hr) || !resources.index_buffer) {
                return fail(NativeD3D12RayTracingContextFailureStage::scene,
                            "native-d3d12.context.index-buffer-create-failed",
                            "Persistent default-heap index buffer creation failed with " + hresult_hex(hr) + ".");
            }
            hr = create_committed_buffer(
                impl_->device.Get(), index_bytes, D3D12_HEAP_TYPE_UPLOAD,
                D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_FLAG_NONE,
                resources.index_upload);
            if (FAILED(hr) || !resources.index_upload ||
                !fill_upload_buffer(resources.index_upload.Get(),
                                    geometry.indices.data(),
                                    static_cast<std::size_t>(index_bytes))) {
                return fail(NativeD3D12RayTracingContextFailureStage::scene,
                            "native-d3d12.context.index-upload-failed",
                            "Persistent upload-heap index initialization failed with " + hresult_hex(hr) + ".");
            }

            D3D12_RAYTRACING_GEOMETRY_DESC geometry_desc{};
            geometry_desc.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
            geometry_desc.Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
            geometry_desc.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
            geometry_desc.Triangles.VertexCount = resources.vertex_count;
            geometry_desc.Triangles.VertexBuffer.StartAddress =
                resources.vertex_buffer->GetGPUVirtualAddress();
            geometry_desc.Triangles.VertexBuffer.StrideInBytes = sizeof(float) * 3U;
            geometry_desc.Triangles.IndexFormat = DXGI_FORMAT_R32_UINT;
            geometry_desc.Triangles.IndexCount = resources.index_count;
            geometry_desc.Triangles.IndexBuffer = resources.index_buffer->GetGPUVirtualAddress();

            D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS blas_inputs{};
            blas_inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
            blas_inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
            blas_inputs.NumDescs = 1U;
            blas_inputs.pGeometryDescs = &geometry_desc;
            blas_inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
            if (resources.allow_update)
                blas_inputs.Flags |= D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_UPDATE;
            D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO blas_info{};
            impl_->device->GetRaytracingAccelerationStructurePrebuildInfo(
                &blas_inputs, &blas_info);
            if (!resource_bytes_bounded(blas_info.ResultDataMaxSizeInBytes) ||
                !resource_bytes_bounded(blas_info.ScratchDataSizeInBytes) ||
                !add_resource_budget(blas_info.ResultDataMaxSizeInBytes) ||
                !add_resource_budget(blas_info.ScratchDataSizeInBytes)) {
                return fail(NativeD3D12RayTracingContextFailureStage::blas,
                            "native-d3d12.context.blas-prebuild-invalid",
                            "D3D12 returned an empty, unbounded or over-budget BLAS prebuild size.");
            }
            resources.blas_result_bytes = blas_info.ResultDataMaxSizeInBytes;
            resources.blas_scratch_bytes = blas_info.ScratchDataSizeInBytes;
            hr = create_committed_buffer(
                impl_->device.Get(), resources.blas_result_bytes,
                D3D12_HEAP_TYPE_DEFAULT,
                D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE,
                D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                resources.blas_result);
            if (FAILED(hr) || !resources.blas_result) {
                return fail(NativeD3D12RayTracingContextFailureStage::blas,
                            "native-d3d12.context.blas-result-create-failed",
                            "Persistent BLAS result resource creation failed with " + hresult_hex(hr) + ".");
            }
            hr = create_committed_buffer(
                impl_->device.Get(), resources.blas_scratch_bytes,
                D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                resources.blas_scratch);
            if (FAILED(hr) || !resources.blas_scratch) {
                return fail(NativeD3D12RayTracingContextFailureStage::blas,
                            "native-d3d12.context.blas-scratch-create-failed",
                            "Persistent BLAS scratch resource creation failed with " + hresult_hex(hr) + ".");
            }
            next_geometry_resources.push_back(std::move(resources));
        }
    }

    auto& geometry_resources = rebuild ? next_geometry_resources : impl_->geometry_resources;
    std::vector<D3D12_RAYTRACING_GEOMETRY_DESC> geometry_descriptors;
    geometry_descriptors.reserve(geometry_resources.size());
    for (const auto& resources : geometry_resources) {
        D3D12_RAYTRACING_GEOMETRY_DESC geometry_desc{};
        geometry_desc.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
        geometry_desc.Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
        geometry_desc.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
        geometry_desc.Triangles.VertexCount = resources.vertex_count;
        geometry_desc.Triangles.VertexBuffer.StartAddress =
            resources.vertex_buffer->GetGPUVirtualAddress();
        geometry_desc.Triangles.VertexBuffer.StrideInBytes = sizeof(float) * 3U;
        geometry_desc.Triangles.IndexFormat = DXGI_FORMAT_R32_UINT;
        geometry_desc.Triangles.IndexCount = resources.index_count;
        geometry_desc.Triangles.IndexBuffer = resources.index_buffer->GetGPUVirtualAddress();
        geometry_descriptors.push_back(geometry_desc);
    }

    std::vector<D3D12_RAYTRACING_INSTANCE_DESC> instance_descriptors;
    instance_descriptors.resize(geometry_resources.size());
    for (std::size_t index = 0U; index < geometry_resources.size(); ++index) {
        auto& instance = instance_descriptors[index];
        instance.Transform[0][0] = 1.0F;
        instance.Transform[1][1] = 1.0F;
        instance.Transform[2][2] = 1.0F;
        instance.InstanceID = static_cast<UINT>(index);
        instance.InstanceMask = 0xffU;
        instance.AccelerationStructure = geometry_resources[index].blas_result->GetGPUVirtualAddress();
    }
    const auto instance_bytes = static_cast<std::uint64_t>(instance_descriptors.size()) *
        sizeof(D3D12_RAYTRACING_INSTANCE_DESC);
    if (!resource_bytes_bounded(instance_bytes)) {
        return fail(NativeD3D12RayTracingContextFailureStage::tlas,
                    "native-d3d12.context.instance-buffer-size-invalid",
                    "The persistent TLAS instance descriptor buffer exceeded the bounded resource contract.");
    }
    if (rebuild) {
        if (!add_resource_budget(instance_bytes)) {
            return fail(NativeD3D12RayTracingContextFailureStage::tlas,
                        "native-d3d12.context.resource-budget-exceeded",
                        "Persistent TLAS instance descriptors exceed the configured D3D12 context budget.");
        }
        HRESULT hr = create_committed_buffer(
            impl_->device.Get(), instance_bytes, D3D12_HEAP_TYPE_UPLOAD,
            D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_FLAG_NONE,
            next_instance_buffer);
        if (FAILED(hr) || !next_instance_buffer ||
            !fill_upload_buffer(next_instance_buffer.Get(), instance_descriptors.data(),
                                static_cast<std::size_t>(instance_bytes))) {
            return fail(NativeD3D12RayTracingContextFailureStage::tlas,
                        "native-d3d12.context.instance-buffer-failed",
                        "Persistent TLAS instance descriptor upload failed with " + hresult_hex(hr) + ".");
        }
    } else if (!fill_upload_buffer(impl_->instance_buffer.Get(), instance_descriptors.data(),
                                   static_cast<std::size_t>(instance_bytes))) {
        return fail(NativeD3D12RayTracingContextFailureStage::tlas,
                    "native-d3d12.context.instance-buffer-update-failed",
                    "Updating the persistent TLAS instance descriptors failed.");
    }
    ID3D12Resource* instance_buffer = rebuild ? next_instance_buffer.Get() : impl_->instance_buffer.Get();

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS tlas_inputs{};
    tlas_inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
    tlas_inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    tlas_inputs.NumDescs = static_cast<UINT>(instance_descriptors.size());
    tlas_inputs.InstanceDescs = instance_buffer->GetGPUVirtualAddress();
    tlas_inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
    if (impl_->scene->allow_update)
        tlas_inputs.Flags |= D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_UPDATE;

    if (rebuild) {
        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO tlas_info{};
        impl_->device->GetRaytracingAccelerationStructurePrebuildInfo(
            &tlas_inputs, &tlas_info);
        if (!resource_bytes_bounded(tlas_info.ResultDataMaxSizeInBytes) ||
            !resource_bytes_bounded(tlas_info.ScratchDataSizeInBytes) ||
            !add_resource_budget(tlas_info.ResultDataMaxSizeInBytes) ||
            !add_resource_budget(tlas_info.ScratchDataSizeInBytes)) {
            return fail(NativeD3D12RayTracingContextFailureStage::tlas,
                        "native-d3d12.context.tlas-prebuild-invalid",
                        "D3D12 returned an empty, unbounded or over-budget TLAS prebuild size.");
        }
        next_tlas_result_bytes = tlas_info.ResultDataMaxSizeInBytes;
        next_tlas_scratch_bytes = tlas_info.ScratchDataSizeInBytes;
        HRESULT hr = create_committed_buffer(
            impl_->device.Get(), next_tlas_result_bytes, D3D12_HEAP_TYPE_DEFAULT,
            D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE,
            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, next_tlas_result);
        if (FAILED(hr) || !next_tlas_result) {
            return fail(NativeD3D12RayTracingContextFailureStage::tlas,
                        "native-d3d12.context.tlas-result-create-failed",
                        "Persistent TLAS result resource creation failed with " + hresult_hex(hr) + ".");
        }
        hr = create_committed_buffer(
            impl_->device.Get(), next_tlas_scratch_bytes, D3D12_HEAP_TYPE_DEFAULT,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, next_tlas_scratch);
        if (FAILED(hr) || !next_tlas_scratch) {
            return fail(NativeD3D12RayTracingContextFailureStage::tlas,
                        "native-d3d12.context.tlas-scratch-create-failed",
                        "Persistent TLAS scratch resource creation failed with " + hresult_hex(hr) + ".");
        }
    }
    ID3D12Resource* tlas_result = rebuild ? next_tlas_result.Get() : impl_->tlas_result.Get();
    ID3D12Resource* tlas_scratch = rebuild ? next_tlas_scratch.Get() : impl_->tlas_scratch.Get();
    if (tlas_result == nullptr || tlas_scratch == nullptr || instance_buffer == nullptr) {
        return fail(NativeD3D12RayTracingContextFailureStage::tlas,
                    "native-d3d12.context.tlas-resources-unavailable",
                    "Persistent TLAS resources were not available for command recording.");
    }

    HRESULT hr = impl_->command_allocator->Reset();
    if (FAILED(hr)) {
        return fail(NativeD3D12RayTracingContextFailureStage::command_allocator,
                    "native-d3d12.context.command-allocator-reset-failed",
                    "Resetting the persistent command allocator failed with " + hresult_hex(hr) + ".");
    }
    hr = impl_->command_list->Reset(impl_->command_allocator.Get(), nullptr);
    if (FAILED(hr)) {
        return fail(NativeD3D12RayTracingContextFailureStage::command_list,
                    "native-d3d12.context.command-list-reset-failed",
                    "Resetting the persistent command list failed with " + hresult_hex(hr) + ".");
    }

    for (std::size_t index = 0U; index < geometry_resources.size(); ++index) {
        auto& resources = geometry_resources[index];
        const auto& geometry = impl_->scene->geometries[index];
        if (rebuild) {
            impl_->command_list->CopyBufferRegion(resources.vertex_buffer.Get(), 0U,
                                                  resources.vertex_upload.Get(), 0U,
                                                  resources.vertex_bytes);
            transition_resource(impl_->command_list.Get(), resources.vertex_buffer.Get(),
                                D3D12_RESOURCE_STATE_COPY_DEST,
                                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            impl_->command_list->CopyBufferRegion(resources.index_buffer.Get(), 0U,
                                                  resources.index_upload.Get(), 0U,
                                                  resources.index_bytes);
            transition_resource(impl_->command_list.Get(), resources.index_buffer.Get(),
                                D3D12_RESOURCE_STATE_COPY_DEST,
                                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        } else {
            if (!fill_upload_buffer(resources.vertex_upload.Get(), geometry.position_xyz.data(),
                                    static_cast<std::size_t>(resources.vertex_bytes)) ||
                !fill_upload_buffer(resources.index_upload.Get(), geometry.indices.data(),
                                    static_cast<std::size_t>(resources.index_bytes))) {
                static_cast<void>(impl_->command_list->Close());
                return fail(NativeD3D12RayTracingContextFailureStage::scene,
                            "native-d3d12.context.geometry-update-upload-failed",
                            "Updating persistent vertex or index upload data failed.");
            }
            transition_resource(impl_->command_list.Get(), resources.vertex_buffer.Get(),
                                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                D3D12_RESOURCE_STATE_COPY_DEST);
            impl_->command_list->CopyBufferRegion(resources.vertex_buffer.Get(), 0U,
                                                  resources.vertex_upload.Get(), 0U,
                                                  resources.vertex_bytes);
            transition_resource(impl_->command_list.Get(), resources.vertex_buffer.Get(),
                                D3D12_RESOURCE_STATE_COPY_DEST,
                                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            transition_resource(impl_->command_list.Get(), resources.index_buffer.Get(),
                                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                D3D12_RESOURCE_STATE_COPY_DEST);
            impl_->command_list->CopyBufferRegion(resources.index_buffer.Get(), 0U,
                                                  resources.index_upload.Get(), 0U,
                                                  resources.index_bytes);
            transition_resource(impl_->command_list.Get(), resources.index_buffer.Get(),
                                D3D12_RESOURCE_STATE_COPY_DEST,
                                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        }
        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS blas_inputs{};
        blas_inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
        blas_inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
        blas_inputs.NumDescs = 1U;
        blas_inputs.pGeometryDescs = &geometry_descriptors[index];
        blas_inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
        if (resources.allow_update)
            blas_inputs.Flags |= D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_UPDATE;
        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC blas_build{};
        blas_build.Inputs = blas_inputs;
        blas_build.ScratchAccelerationStructureData = resources.blas_scratch->GetGPUVirtualAddress();
        blas_build.DestAccelerationStructureData = resources.blas_result->GetGPUVirtualAddress();
        if (!rebuild) {
            blas_build.Inputs.Flags |= D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PERFORM_UPDATE;
            blas_build.SourceAccelerationStructureData = resources.blas_result->GetGPUVirtualAddress();
        }
        impl_->command_list->BuildRaytracingAccelerationStructure(&blas_build, 0U, nullptr);
        uav_barrier(impl_->command_list.Get(), resources.blas_result.Get());
    }

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC tlas_build{};
    tlas_build.Inputs = tlas_inputs;
    tlas_build.ScratchAccelerationStructureData = tlas_scratch->GetGPUVirtualAddress();
    tlas_build.DestAccelerationStructureData = tlas_result->GetGPUVirtualAddress();
    if (!rebuild) {
        tlas_build.Inputs.Flags |= D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PERFORM_UPDATE;
        tlas_build.SourceAccelerationStructureData = tlas_result->GetGPUVirtualAddress();
    }
    impl_->command_list->BuildRaytracingAccelerationStructure(&tlas_build, 0U, nullptr);
    hr = impl_->command_list->Close();
    if (FAILED(hr)) {
        return fail(NativeD3D12RayTracingContextFailureStage::command_list,
                    "native-d3d12.context.command-list-close-failed",
                    "Closing the persistent AS build command list failed with " + hresult_hex(hr) + ".");
    }
    impl_->last_build_submitted = true;
    impl_->last_update_submitted = !rebuild;
    std::uint64_t submitted_fence_value = 0U;
    std::uint64_t completed_fence_value = 0U;
    hr = submit_and_wait(impl_->command_queue.Get(), impl_->fence.Get(),
                         impl_->command_list.Get(), impl_->fence_event,
                         impl_->next_fence_value,
                         &submitted_fence_value, &completed_fence_value);
    if (FAILED(hr)) {
        return fail(NativeD3D12RayTracingContextFailureStage::synchronization,
                    "native-d3d12.context.as-build-wait-failed",
                    "Waiting for the persistent BLAS/TLAS build failed with " + hresult_hex(hr) + ".");
    }
    impl_->last_submitted_fence_value = submitted_fence_value;
    impl_->last_completed_fence_value = completed_fence_value;

    if (rebuild) {
        impl_->geometry_resources = std::move(next_geometry_resources);
        impl_->instance_buffer = std::move(next_instance_buffer);
        impl_->tlas_result = std::move(next_tlas_result);
        impl_->tlas_scratch = std::move(next_tlas_scratch);
        impl_->instance_buffer_bytes = instance_bytes;
        impl_->tlas_result_bytes = next_tlas_result_bytes;
        impl_->tlas_scratch_bytes = next_tlas_scratch_bytes;
        if (impl_->resource_generation != std::numeric_limits<std::uint64_t>::max())
            ++impl_->resource_generation;
    }
    impl_->blas_ready = true;
    impl_->tlas_ready = true;
    impl_->last_build_completed = true;
    impl_->last_update_completed = !rebuild;
    impl_->last_synchronization_completed = true;
    impl_->built_scene_hash = impl_->scene_hash;
    impl_->built_topology_hash = topology_hash;
    impl_->scene_dirty = false;
    save_result(*impl_, NativeD3D12RayTracingContextFailureStage::none,
                rebuild ? "native-d3d12.context.as-build-complete"
                        : "native-d3d12.context.as-update-complete",
                rebuild
                    ? "Persistent vertex/index uploads, BLAS result/scratch, TLAS result/scratch and instance descriptors were built and synchronized on the retained D3D12 queue."
                    : "Persistent vertex/index uploads were refreshed and BLAS/TLAS update builds were synchronized on the retained D3D12 queue.");
    auto result = receipt_from(*impl_, "build-or-update");
    result.scene_received = true;
    result.state = NativeD3D12RayTracingContextState::ready;
    return result;
#endif
}

NativeD3D12RayTracingContextReceipt NativeD3D12RayTracingContext::trace() {
    if (impl_->state == NativeD3D12RayTracingContextState::uninitialized)
        static_cast<void>(initialize());
#if defined(_WIN32)
    // The context intentionally owns one command allocator/list and one
    // output allocation.  Do not reset either while an async submission is
    // still in flight; poll without introducing a CPU wait and ask the
    // caller to synchronize at its chosen frame boundary instead.
    if (impl_->pending_fence_value != 0U) {
        const auto progress = synchronize(false);
        if (impl_->pending_fence_value != 0U) {
            auto pending = receipt_from(*impl_, "trace");
            pending.state = progress.state == NativeD3D12RayTracingContextState::failed
                ? NativeD3D12RayTracingContextState::failed
                : NativeD3D12RayTracingContextState::ready;
            pending.fallback_active = false;
            return pending;
        }
    }
    impl_->last_trace_submitted = false;
    impl_->last_trace_completed = false;
    impl_->last_readback_completed = false;
    impl_->output_surface_trace_completed = false;
    impl_->camera_shader_consumed = false;
    impl_->linear_radiance_shader_consumed = false;
    impl_->output_radiance_valid = false;
    impl_->output_radiance_probe.fill(0.0F);
    impl_->output_access_token = 0U;
    impl_->last_output_copy_submitted = false;
    impl_->last_output_copy_completed = false;
    impl_->last_synchronization_completed = false;
#endif
    auto result = receipt_from(*impl_, "trace");
    if (!impl_->scene) {
        save_result(*impl_, NativeD3D12RayTracingContextFailureStage::scene,
                    "native-d3d12.context.trace-scene-not-ready",
                    "Trace requires a retained scene and a completed AS build/update.");
        result = receipt_from(*impl_, "trace");
        result.state = NativeD3D12RayTracingContextState::failed;
        return result;
    }
    if (impl_->state != NativeD3D12RayTracingContextState::ready) {
        if (impl_->state == NativeD3D12RayTracingContextState::unsupported) {
            save_result(*impl_, NativeD3D12RayTracingContextFailureStage::trace,
                        "native-d3d12.context.trace-unavailable",
                        "Hardware D3D12 ray-tracing is unavailable; TraceRays is explicitly skipped.");
            result = receipt_from(*impl_, "trace");
            result.state = NativeD3D12RayTracingContextState::unsupported;
            result.fallback_active = true;
        }
        return result;
    }
    if (!impl_->blas_ready || !impl_->tlas_ready) {
        save_result(*impl_, NativeD3D12RayTracingContextFailureStage::trace,
                    "native-d3d12.context.trace-as-not-ready",
                    "TraceRays requires a completed persistent BLAS and TLAS build/update.");
        result = receipt_from(*impl_, "trace");
        result.state = NativeD3D12RayTracingContextState::failed;
        return result;
    }

#if !defined(_WIN32)
    mark_unsupported(*impl_, NativeD3D12RayTracingContextFailureStage::platform,
                     "trace", "native-d3d12.context.platform-unavailable",
                     "The persistent D3D12 TraceRays path is unavailable on this platform.", result);
    return result;
#else
    std::string pipeline_code;
    std::string pipeline_detail;
    if (!ensure_trace_pipeline(*impl_, pipeline_code, pipeline_detail)) {
        save_result(*impl_, NativeD3D12RayTracingContextFailureStage::shader_pipeline,
                    pipeline_code, pipeline_detail);
        result = receipt_from(*impl_, "trace");
        result.state = NativeD3D12RayTracingContextState::unsupported;
        result.fallback_active = true;
        return result;
    }

    std::string shading_code;
    std::string shading_detail;
    if (!ensure_full_frame_shading_resources(*impl_, shading_code, shading_detail)) {
        impl_->state = NativeD3D12RayTracingContextState::failed;
        save_result(*impl_, NativeD3D12RayTracingContextFailureStage::shading,
                    shading_code, shading_detail);
        result = receipt_from(*impl_, "trace");
        result.state = NativeD3D12RayTracingContextState::failed;
        return result;
    }

    if (impl_->full_frame_shader_active && impl_->camera_dirty) {
        if (!impl_->camera_valid || impl_->camera_constants == nullptr ||
            !fill_camera_constants(impl_->camera_constants.Get(), impl_->camera)) {
            impl_->state = NativeD3D12RayTracingContextState::failed;
            save_result(*impl_, NativeD3D12RayTracingContextFailureStage::camera,
                        "native-d3d12.context.camera-constants-upload-failed",
                        "The validated camera could not be uploaded to the retained full-frame constant buffer.");
            result = receipt_from(*impl_, "trace");
            result.state = NativeD3D12RayTracingContextState::failed;
            return result;
        }
        impl_->camera_dirty = false;
    }

    HRESULT hr = impl_->command_allocator->Reset();
    if (FAILED(hr)) {
        impl_->state = NativeD3D12RayTracingContextState::failed;
        save_result(*impl_, NativeD3D12RayTracingContextFailureStage::command_allocator,
                    "native-d3d12.context.trace-command-allocator-reset-failed",
                    "Resetting the persistent trace command allocator failed with " + hresult_hex(hr) + ".");
        result = receipt_from(*impl_, "trace");
        result.state = NativeD3D12RayTracingContextState::failed;
        return result;
    }
    hr = impl_->command_list->Reset(impl_->command_allocator.Get(), nullptr);
    if (FAILED(hr)) {
        impl_->state = NativeD3D12RayTracingContextState::failed;
        save_result(*impl_, NativeD3D12RayTracingContextFailureStage::command_list,
                    "native-d3d12.context.trace-command-list-reset-failed",
                    "Resetting the persistent trace command list failed with " + hresult_hex(hr) + ".");
        result = receipt_from(*impl_, "trace");
        result.state = NativeD3D12RayTracingContextState::failed;
        return result;
    }
    if (impl_->output_in_copy_source) {
        transition_resource(impl_->command_list.Get(), impl_->output_resource.Get(),
                            D3D12_RESOURCE_STATE_COPY_SOURCE,
                            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        impl_->output_surface_state = NativeD3D12RayTracingOutputSurfaceState::unordered_access;
    }
    uav_barrier(impl_->command_list.Get(), impl_->tlas_result.Get());
    impl_->command_list->SetComputeRootSignature(impl_->trace_root_signature.Get());
    impl_->command_list->SetPipelineState1(impl_->trace_state_object.Get());
    impl_->command_list->SetComputeRootShaderResourceView(
        0U, impl_->tlas_result->GetGPUVirtualAddress());
    impl_->command_list->SetComputeRootUnorderedAccessView(
        1U, impl_->output_resource->GetGPUVirtualAddress());
    if (impl_->full_frame_shader_active) {
        impl_->command_list->SetComputeRootConstantBufferView(
            2U, impl_->camera_constants->GetGPUVirtualAddress());
        impl_->command_list->SetComputeRootShaderResourceView(
            3U, impl_->shading_materials->GetGPUVirtualAddress());
        impl_->command_list->SetComputeRootConstantBufferView(
            4U, impl_->shading_lighting->GetGPUVirtualAddress());
        impl_->command_list->SetComputeRootShaderResourceView(
            5U, impl_->shading_instances->GetGPUVirtualAddress());
        impl_->command_list->SetComputeRootShaderResourceView(
            6U, impl_->shading_normals->GetGPUVirtualAddress());
    }
    constexpr std::uint32_t shader_record_bytes =
        D3D12_RAYTRACING_SHADER_RECORD_BYTE_ALIGNMENT;
    D3D12_DISPATCH_RAYS_DESC dispatch{};
    const auto shader_table_address = impl_->shader_table->GetGPUVirtualAddress();
    dispatch.RayGenerationShaderRecord.StartAddress = shader_table_address;
    dispatch.RayGenerationShaderRecord.SizeInBytes = shader_record_bytes;
    dispatch.MissShaderTable.StartAddress = shader_table_address + shader_record_bytes * 2U;
    dispatch.MissShaderTable.SizeInBytes = shader_record_bytes;
    dispatch.MissShaderTable.StrideInBytes = shader_record_bytes;
    dispatch.HitGroupTable.StartAddress = shader_table_address + shader_record_bytes * 4U;
    dispatch.HitGroupTable.SizeInBytes = shader_record_bytes;
    dispatch.HitGroupTable.StrideInBytes = shader_record_bytes;
    dispatch.Width = impl_->options.output_width;
    dispatch.Height = impl_->options.output_height;
    dispatch.Depth = 1U;
    impl_->command_list->DispatchRays(&dispatch);
    transition_resource(impl_->command_list.Get(), impl_->output_resource.Get(),
                        D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                        D3D12_RESOURCE_STATE_COPY_SOURCE);
    impl_->command_list->CopyResource(impl_->output_readback.Get(), impl_->output_resource.Get());
    hr = impl_->command_list->Close();
    if (FAILED(hr)) {
        impl_->state = NativeD3D12RayTracingContextState::failed;
        save_result(*impl_, NativeD3D12RayTracingContextFailureStage::command_list,
                    "native-d3d12.context.trace-command-list-close-failed",
                    "Closing the persistent TraceRays command list failed with " + hresult_hex(hr) + ".");
        result = receipt_from(*impl_, "trace");
        result.state = NativeD3D12RayTracingContextState::failed;
        return result;
    }
    impl_->last_trace_submitted = true;
    std::uint64_t submitted_fence_value = 0U;
    std::uint64_t completed_fence_value = 0U;
    hr = submit_and_wait(impl_->command_queue.Get(), impl_->fence.Get(),
                         impl_->command_list.Get(), impl_->fence_event,
                         impl_->next_fence_value,
                         &submitted_fence_value, &completed_fence_value,
                         impl_->options.synchronization_policy ==
                             NativeD3D12RayTracingSynchronizationPolicy::wait_for_completion);
    if (FAILED(hr)) {
        impl_->state = NativeD3D12RayTracingContextState::failed;
        save_result(*impl_, NativeD3D12RayTracingContextFailureStage::synchronization,
                    "native-d3d12.context.trace-wait-failed",
                    "Waiting for persistent TraceRays completion failed with " + hresult_hex(hr) + ".");
        result = receipt_from(*impl_, "trace");
        result.state = NativeD3D12RayTracingContextState::failed;
        return result;
    }
    impl_->last_submitted_fence_value = submitted_fence_value;
    impl_->last_completed_fence_value = completed_fence_value;
    impl_->output_fence_value = submitted_fence_value;
    impl_->camera_shader_consumed = impl_->full_frame_shader_active;
    impl_->linear_radiance_shader_consumed = impl_->full_frame_shader_active;
    impl_->output_in_copy_source = true;
    if (completed_fence_value < submitted_fence_value) {
        // The transition to COPY_SOURCE and the optional readback copy are
        // already recorded in the command list, but the GPU has not proven
        // completion yet.  Keep the public surface unavailable until the
        // caller polls/waits through synchronize().
        impl_->pending_gpu_operation = Impl::PendingGpuOperation::trace;
        impl_->pending_fence_value = submitted_fence_value;
        impl_->output_surface_state = NativeD3D12RayTracingOutputSurfaceState::unordered_access;
        impl_->output_surface_trace_completed = false;
        impl_->last_trace_completed = false;
        impl_->last_synchronization_completed = false;
        save_result(*impl_, NativeD3D12RayTracingContextFailureStage::synchronization,
                    "native-d3d12.context.trace-submitted",
                    "TraceRays was submitted without a CPU wait; poll or wait with synchronize() before consuming the output view.");
        result = receipt_from(*impl_, "trace");
        result.state = NativeD3D12RayTracingContextState::ready;
        return result;
    }
    impl_->output_surface_state = NativeD3D12RayTracingOutputSurfaceState::copy_source;
    impl_->output_surface_trace_completed = true;
    if (impl_->next_output_access_token == 0U ||
        impl_->next_output_access_token == std::numeric_limits<std::uint64_t>::max())
        impl_->next_output_access_token = 1U;
    impl_->output_access_token = impl_->next_output_access_token++;
    impl_->last_trace_completed = true;
    impl_->last_synchronization_completed = true;
    save_result(*impl_, NativeD3D12RayTracingContextFailureStage::none,
                "native-d3d12.context.trace-complete",
                "Persistent TraceRays dispatched the retained SBT against the retained TLAS and synchronized the output readback copy.");
    result = receipt_from(*impl_, "trace");
    result.state = NativeD3D12RayTracingContextState::ready;
    return result;
#endif
}

NativeD3D12RayTracingContextReceipt NativeD3D12RayTracingContext::synchronize(
    const bool wait_for_completion) {
    if (impl_->state == NativeD3D12RayTracingContextState::uninitialized)
        static_cast<void>(initialize());

    auto result = receipt_from(*impl_, "synchronize");
    if (impl_->state == NativeD3D12RayTracingContextState::unsupported) {
        save_result(*impl_, NativeD3D12RayTracingContextFailureStage::synchronization,
                    "native-d3d12.context.synchronization-unavailable",
                    "The native D3D12 queue is unavailable; no asynchronous completion can be polled.");
        result = receipt_from(*impl_, "synchronize");
        result.state = NativeD3D12RayTracingContextState::unsupported;
        result.fallback_active = true;
        return result;
    }
    if (impl_->state != NativeD3D12RayTracingContextState::ready) {
        result = receipt_from(*impl_, "synchronize");
        result.state = impl_->state;
        result.fallback_active = false;
        return result;
    }
#if !defined(_WIN32)
    save_result(*impl_, NativeD3D12RayTracingContextFailureStage::platform,
                "native-d3d12.context.platform-unavailable",
                "Asynchronous D3D12 synchronization is unavailable on this platform.");
    result = receipt_from(*impl_, "synchronize");
    result.state = NativeD3D12RayTracingContextState::unsupported;
    result.fallback_active = true;
    return result;
#else
    if (impl_->pending_fence_value == 0U ||
        impl_->pending_gpu_operation == Impl::PendingGpuOperation::none) {
        save_result(*impl_, NativeD3D12RayTracingContextFailureStage::none,
                    "native-d3d12.context.synchronization-idle",
                    "No asynchronous D3D12 submission is waiting for completion.");
        result = receipt_from(*impl_, "synchronize");
        result.state = impl_->state;
        result.fallback_active = impl_->state == NativeD3D12RayTracingContextState::unsupported;
        return result;
    }
    if (impl_->fence == nullptr) {
        impl_->state = NativeD3D12RayTracingContextState::failed;
        save_result(*impl_, NativeD3D12RayTracingContextFailureStage::synchronization,
                    "native-d3d12.context.synchronization-fence-unavailable",
                    "An asynchronous submission exists but its retained fence is unavailable.");
        result = receipt_from(*impl_, "synchronize");
        result.state = NativeD3D12RayTracingContextState::failed;
        return result;
    }

    const auto pending_fence_value = impl_->pending_fence_value;
    auto completed_fence_value = impl_->fence->GetCompletedValue();
    if (completed_fence_value < pending_fence_value && wait_for_completion) {
        if (impl_->fence_event == nullptr) {
            impl_->fence_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            if (impl_->fence_event == nullptr) {
                impl_->state = NativeD3D12RayTracingContextState::failed;
                save_result(*impl_, NativeD3D12RayTracingContextFailureStage::synchronization,
                            "native-d3d12.context.synchronization-event-create-failed",
                            "Creating the asynchronous D3D12 fence event failed with " +
                                hresult_hex(HRESULT_FROM_WIN32(GetLastError())) + ".");
                result = receipt_from(*impl_, "synchronize");
                result.state = NativeD3D12RayTracingContextState::failed;
                return result;
            }
        }
        const auto hr = impl_->fence->SetEventOnCompletion(
            pending_fence_value, impl_->fence_event);
        if (FAILED(hr) ||
            WaitForSingleObject(impl_->fence_event, INFINITE) != WAIT_OBJECT_0) {
            impl_->state = NativeD3D12RayTracingContextState::failed;
            save_result(*impl_, NativeD3D12RayTracingContextFailureStage::synchronization,
                        "native-d3d12.context.synchronization-wait-failed",
                        "Waiting for asynchronous D3D12 completion failed with " +
                            hresult_hex(FAILED(hr) ? hr : HRESULT_FROM_WIN32(GetLastError())) + ".");
            result = receipt_from(*impl_, "synchronize");
            result.state = NativeD3D12RayTracingContextState::failed;
            return result;
        }
        completed_fence_value = impl_->fence->GetCompletedValue();
    }
    if (completed_fence_value < pending_fence_value) {
        save_result(*impl_, NativeD3D12RayTracingContextFailureStage::synchronization,
                    "native-d3d12.context.synchronization-pending",
                    "The asynchronous D3D12 submission is still in flight; no output view is published yet.");
        result = receipt_from(*impl_, "synchronize");
        result.state = NativeD3D12RayTracingContextState::ready;
        return result;
    }

    const auto completed_operation = impl_->pending_gpu_operation;
    impl_->pending_gpu_operation = Impl::PendingGpuOperation::none;
    impl_->pending_fence_value = 0U;
    impl_->last_completed_fence_value = completed_fence_value;
    if (completed_operation == Impl::PendingGpuOperation::trace) {
        impl_->output_in_copy_source = true;
        impl_->output_surface_state = NativeD3D12RayTracingOutputSurfaceState::copy_source;
        impl_->output_surface_trace_completed = true;
        if (impl_->next_output_access_token == 0U ||
            impl_->next_output_access_token == std::numeric_limits<std::uint64_t>::max())
            impl_->next_output_access_token = 1U;
        impl_->output_access_token = impl_->next_output_access_token++;
        impl_->last_trace_completed = true;
        impl_->last_synchronization_completed = true;
        save_result(*impl_, NativeD3D12RayTracingContextFailureStage::none,
                    "native-d3d12.context.trace-complete",
                    "The asynchronous TraceRays submission reached its fence; the output view is now current.");
    } else if (completed_operation == Impl::PendingGpuOperation::output_copy) {
        impl_->last_output_copy_completed = true;
        impl_->last_synchronization_completed = true;
        save_result(*impl_, NativeD3D12RayTracingContextFailureStage::none,
                    "native-d3d12.context.output-copy-complete",
                    "The asynchronous GPU-only output copy reached its fence.");
    }
    result = receipt_from(*impl_, "synchronize");
    result.state = NativeD3D12RayTracingContextState::ready;
    return result;
#endif
}

NativeD3D12RayTracingContextReceipt NativeD3D12RayTracingContext::readback() {
    if (impl_->state == NativeD3D12RayTracingContextState::uninitialized)
        static_cast<void>(initialize());
#if defined(_WIN32)
    // Readback is already an explicit diagnostic/CPU boundary, so complete a
    // pending submit before mapping the staging resource.  The normal render
    // loop can use synchronize(false) and never enters this path.
    if (impl_->pending_fence_value != 0U) {
        const auto synchronized = synchronize(true);
        if (impl_->pending_fence_value != 0U ||
            synchronized.state == NativeD3D12RayTracingContextState::failed) {
            auto pending = receipt_from(*impl_, "readback");
            pending.state = NativeD3D12RayTracingContextState::failed;
            return pending;
        }
    }
    impl_->last_readback_completed = false;
#endif
    auto result = receipt_from(*impl_, "readback");
    if (impl_->state == NativeD3D12RayTracingContextState::unsupported) {
        save_result(*impl_, NativeD3D12RayTracingContextFailureStage::readback,
                    "native-d3d12.context.readback-unavailable",
                    "Hardware D3D12 output resources are unavailable; no readback is reported ready.");
        result = receipt_from(*impl_, "readback");
        result.state = NativeD3D12RayTracingContextState::unsupported;
        result.fallback_active = true;
        return result;
    }
    if (impl_->state != NativeD3D12RayTracingContextState::ready ||
        !impl_->last_trace_completed) {
        save_result(*impl_, NativeD3D12RayTracingContextFailureStage::readback,
                    "native-d3d12.context.readback-not-ready",
                    "Readback is gated on a completed TraceRays dispatch; no output is reported ready before that proof.");
        result = receipt_from(*impl_, "readback");
        result.state = impl_->state == NativeD3D12RayTracingContextState::ready
            ? NativeD3D12RayTracingContextState::failed : impl_->state;
        result.fallback_active = impl_->state == NativeD3D12RayTracingContextState::unsupported;
        return result;
    }
#if !defined(_WIN32)
    save_result(*impl_, NativeD3D12RayTracingContextFailureStage::platform,
                "native-d3d12.context.platform-unavailable",
                "The persistent D3D12 output readback is unavailable on this platform.");
    result = receipt_from(*impl_, "readback");
    result.state = NativeD3D12RayTracingContextState::unsupported;
    result.fallback_active = true;
    return result;
#else
    void* mapped = nullptr;
    const D3D12_RANGE read_range{0U, static_cast<SIZE_T>(impl_->output_resource_bytes)};
    HRESULT hr = impl_->output_readback->Map(0U, &read_range, &mapped);
    if (FAILED(hr) || mapped == nullptr) {
        impl_->state = NativeD3D12RayTracingContextState::failed;
        save_result(*impl_, NativeD3D12RayTracingContextFailureStage::readback,
                    "native-d3d12.context.output-readback-map-failed",
                    "Mapping the completed persistent output readback failed with " + hresult_hex(hr) + ".");
        result = receipt_from(*impl_, "readback");
        result.state = NativeD3D12RayTracingContextState::failed;
        return result;
    }
    const auto* output_bytes = static_cast<const std::uint8_t*>(mapped);
    impl_->output_hash = hash_bytes(1469598103934665603ULL,
                                    output_bytes,
                                    static_cast<std::size_t>(impl_->output_resource_bytes));
    if (impl_->linear_radiance_shader_active) {
        const auto pixel_count = impl_->output_resource_bytes /
            (sizeof(float) * 4U);
        bool finite_output = pixel_count != 0U;
        bool hit_found = false;
        bool probe_captured = false;
        for (std::uint64_t pixel_index = 0U; pixel_index < pixel_count;
             ++pixel_index) {
            std::array<float, 4U> sample{};
            std::memcpy(sample.data(), output_bytes + pixel_index * sizeof(sample),
                        sizeof(sample));
            const bool finite_sample = std::all_of(
                sample.begin(), sample.end(), [](const float value) {
                    return std::isfinite(value);
                });
            const bool non_negative_radiance = sample[0U] >= 0.0F &&
                sample[1U] >= 0.0F && sample[2U] >= 0.0F && sample[3U] >= 0.0F &&
                sample[3U] <= 1.0F;
            finite_output = finite_output && finite_sample && non_negative_radiance;
            if (sample[3U] >= 0.5F &&
                (sample[0U] > 0.0F || sample[1U] > 0.0F || sample[2U] > 0.0F)) {
                hit_found = true;
                if (!probe_captured) {
                    impl_->output_radiance_probe = sample;
                    probe_captured = true;
                }
            }
        }
        impl_->output_sentinel = 0U;
        impl_->output_hit = hit_found ? 1U : 0U;
        impl_->output_radiance_valid = finite_output && hit_found;
        const D3D12_RANGE written_range{0U, 0U};
        impl_->output_readback->Unmap(0U, &written_range);
        impl_->last_readback_completed = true;
        if (!impl_->output_radiance_valid) {
            impl_->state = NativeD3D12RayTracingContextState::failed;
            save_result(*impl_, NativeD3D12RayTracingContextFailureStage::readback,
                        "native-d3d12.context.radiance-output-invalid",
                        "The full-frame linear-radiance readback contained non-finite data or no hit marker.");
            result = receipt_from(*impl_, "readback");
            result.state = NativeD3D12RayTracingContextState::failed;
            return result;
        }
        save_result(*impl_, NativeD3D12RayTracingContextFailureStage::none,
                    "native-d3d12.context.radiance-readback-complete",
                    "The full-frame shader produced finite scene-linear direct/ambient radiance and a hit alpha marker.");
        result = receipt_from(*impl_, "readback");
        result.state = NativeD3D12RayTracingContextState::ready;
        return result;
    }
    if (impl_->output_resource_bytes >= sizeof(std::uint32_t) * 2U) {
        std::memcpy(&impl_->output_sentinel, output_bytes, sizeof(std::uint32_t));
        std::memcpy(&impl_->output_hit, output_bytes + sizeof(std::uint32_t), sizeof(std::uint32_t));
    }
    const D3D12_RANGE written_range{0U, 0U};
    impl_->output_readback->Unmap(0U, &written_range);
    impl_->last_readback_completed = true;
    if (impl_->output_sentinel != 0x52415931U || impl_->output_hit != 1U) {
        impl_->state = NativeD3D12RayTracingContextState::failed;
        save_result(*impl_, NativeD3D12RayTracingContextFailureStage::readback,
                    "native-d3d12.context.output-sentinel-mismatch",
                    "Persistent TraceRays completed but the output readback did not contain the expected hit sentinel.");
        result = receipt_from(*impl_, "readback");
        result.state = NativeD3D12RayTracingContextState::failed;
        return result;
    }
    save_result(*impl_, NativeD3D12RayTracingContextFailureStage::none,
                "native-d3d12.context.readback-complete",
                "The persistent TraceRays output was mapped and its deterministic sentinel/hash were observed.");
    result = receipt_from(*impl_, "readback");
    result.state = NativeD3D12RayTracingContextState::ready;
    return result;
#endif
}

NativeD3D12RayTracingContextReceipt NativeD3D12RayTracingContext::shutdown() {
    if (impl_->state == NativeD3D12RayTracingContextState::shutdown) {
        save_result(*impl_, NativeD3D12RayTracingContextFailureStage::cleanup,
                    "native-d3d12.context.shutdown-idempotent",
                    "The persistent D3D12 context was already shut down; cleanup is complete.");
        auto result = receipt_from(*impl_, "shutdown");
        result.state = NativeD3D12RayTracingContextState::shutdown;
        result.shutdown_completed = true;
        return result;
    }
#if defined(_WIN32)
    // Resource destruction must wait for the last submitted fence even when
    // the caller selected submit_only and no event was needed on the hot
    // path.  Create the event lazily here so borrowed SDL queues and native
    // resources are never released while the GPU still references them.
    if (impl_->fence != nullptr && impl_->next_fence_value > 1U &&
        impl_->fence->GetCompletedValue() < impl_->next_fence_value - 1U) {
        if (impl_->fence_event == nullptr)
            impl_->fence_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (impl_->fence_event != nullptr &&
            SUCCEEDED(impl_->fence->SetEventOnCompletion(
                impl_->next_fence_value - 1U, impl_->fence_event)))
            static_cast<void>(WaitForSingleObject(impl_->fence_event, INFINITE));
    }
    impl_->geometry_resources.clear();
    impl_->instance_buffer.Reset();
    impl_->tlas_result.Reset();
    impl_->tlas_scratch.Reset();
    impl_->instance_buffer_bytes = 0U;
    impl_->tlas_result_bytes = 0U;
    impl_->tlas_scratch_bytes = 0U;
    impl_->built_scene_hash = 0U;
    impl_->built_topology_hash = 0U;
    impl_->blas_ready = false;
    impl_->tlas_ready = false;
    impl_->last_build_submitted = false;
    impl_->last_build_completed = false;
    impl_->last_update_submitted = false;
    impl_->last_update_completed = false;
    impl_->last_synchronization_completed = false;
    impl_->trace_root_signature.Reset();
    impl_->trace_state_object.Reset();
    impl_->trace_state_properties.Reset();
    impl_->shader_table.Reset();
    impl_->camera_constants.Reset();
    impl_->shading_materials.Reset();
    impl_->shading_lighting.Reset();
    impl_->shading_instances.Reset();
    impl_->shading_normals.Reset();
    impl_->output_resource.Reset();
    impl_->output_readback.Reset();
    impl_->shader_table_bytes = 0U;
    impl_->camera_constants_bytes = 0U;
    impl_->shading_material_bytes = 0U;
    impl_->shading_lighting_bytes = 0U;
    impl_->shading_instance_bytes = 0U;
    impl_->shading_normal_bytes = 0U;
    impl_->output_resource_bytes = 0U;
    impl_->shader_pipeline_ready = false;
    impl_->shader_table_ready = false;
    impl_->camera_constants_ready = false;
    impl_->full_frame_shader_active = false;
    impl_->linear_radiance_shader_active = false;
    impl_->linear_radiance_shader_consumed = false;
    impl_->shading_resources_ready = false;
    impl_->shading_scene_hash = 0U;
    impl_->resolved_materials.clear();
    impl_->output_resource_ready = false;
    impl_->output_surface_resource_ready = false;
    impl_->output_surface_trace_completed = false;
    impl_->output_surface_state = NativeD3D12RayTracingOutputSurfaceState::released;
    impl_->output_access_token = 0U;
    impl_->output_fence_value = 0U;
    impl_->last_submitted_fence_value = 0U;
    impl_->last_completed_fence_value = 0U;
    impl_->last_output_copy_submitted = false;
    impl_->last_output_copy_completed = false;
    impl_->pending_gpu_operation = Impl::PendingGpuOperation::none;
    impl_->pending_fence_value = 0U;
    impl_->last_trace_submitted = false;
    impl_->last_trace_completed = false;
    impl_->last_readback_completed = false;
    impl_->output_in_copy_source = false;
    impl_->output_sentinel = 0U;
    impl_->output_hit = 0U;
    impl_->output_radiance_valid = false;
    impl_->output_radiance_probe.fill(0.0F);
    impl_->output_hash = 0U;
    impl_->camera_shader_consumed = false;
    impl_->command_list.Reset();
    impl_->command_allocator.Reset();
    impl_->command_queue.Reset();
    impl_->fence.Reset();
    impl_->device.Reset();
    impl_->factory.Reset();
    if (impl_->fence_event != nullptr) {
        CloseHandle(impl_->fence_event);
        impl_->fence_event = nullptr;
    }
#endif
    // Keep the output contract terminal on every platform.  On Windows the
    // platform block above also releases the COM resources; these assignments
    // make the stale-view state explicit for portable tests and callers.
    impl_->output_surface_resource_ready = false;
    impl_->output_surface_trace_completed = false;
    impl_->output_surface_state = NativeD3D12RayTracingOutputSurfaceState::released;
    impl_->output_access_token = 0U;
    impl_->output_fence_value = 0U;
    impl_->last_submitted_fence_value = 0U;
    impl_->last_completed_fence_value = 0U;
    impl_->last_output_copy_submitted = false;
    impl_->last_output_copy_completed = false;
    impl_->pending_gpu_operation = Impl::PendingGpuOperation::none;
    impl_->pending_fence_value = 0U;
    impl_->state = NativeD3D12RayTracingContextState::shutdown;
    impl_->scene.reset();
    impl_->scene_dirty = false;
    impl_->device_adopted = false;
    impl_->command_queue_adopted = false;
    impl_->shared_device = false;
    impl_->shared_command_queue = false;
    save_result(*impl_, NativeD3D12RayTracingContextFailureStage::cleanup,
                "native-d3d12.context.shutdown-complete",
                "Persistent D3D12 handles and cached scene state were released exactly once.");
    auto result = receipt_from(*impl_, "shutdown");
    result.state = NativeD3D12RayTracingContextState::shutdown;
    result.scene_received = false;
    result.shutdown_completed = true;
    result.fallback_active = false;
    return result;
}

NativeD3D12RayTracingOutputSurfaceMetadata
NativeD3D12RayTracingContext::output_surface_metadata() const {
    return receipt_from(*impl_, "output-surface").output_surface;
}

NativeD3D12RayTracingContextPrivateOutputView
NativeD3D12RayTracingContext::private_output_surface_view() const {
    NativeD3D12RayTracingContextPrivateOutputView view;
    view.metadata = output_surface_metadata();
    if (!view.metadata.valid || impl_->output_access_token == 0U)
        return view;
#if defined(_WIN32)
    if (impl_->output_resource == nullptr || impl_->device == nullptr ||
        impl_->command_queue == nullptr || impl_->fence == nullptr)
        return view;
    view.access_token = impl_->output_access_token;
    view.resource = impl_->output_resource.Get();
    view.device = impl_->device.Get();
    view.command_queue = impl_->command_queue.Get();
    view.fence = impl_->fence.Get();
#endif
    return view;
}

bool NativeD3D12RayTracingContext::is_private_output_surface_view_current(
    const NativeD3D12RayTracingContextPrivateOutputView& view) const noexcept {
    if (impl_->state == NativeD3D12RayTracingContextState::shutdown ||
        impl_->output_access_token == 0U || view.access_token == 0U ||
        view.access_token != impl_->output_access_token ||
        view.metadata.resource_generation != impl_->output_resource_generation ||
        view.metadata.context_generation != impl_->generation ||
        !view.metadata.valid || !impl_->output_surface_resource_ready ||
        !impl_->output_surface_trace_completed ||
        impl_->output_surface_state != NativeD3D12RayTracingOutputSurfaceState::copy_source)
        return false;
#if defined(_WIN32)
    return view.resource == impl_->output_resource.Get() &&
        view.device == impl_->device.Get() &&
        view.command_queue == impl_->command_queue.Get() &&
        view.fence == impl_->fence.Get();
#else
    return false;
#endif
}

NativeD3D12RayTracingContextReceipt NativeD3D12RayTracingContext::copy_output_to(
    const NativeD3D12RayTracingContextPrivateOutputView& view,
    void* destination_resource) {
    impl_->last_output_copy_submitted = false;
    impl_->last_output_copy_completed = false;
    impl_->last_synchronization_completed = false;
    const auto fail = [&](const std::string_view code, const std::string_view detail) {
        save_result(*impl_, NativeD3D12RayTracingContextFailureStage::output,
                    code, detail);
        auto result = receipt_from(*impl_, "copy-output");
        result.state = NativeD3D12RayTracingContextState::failed;
        result.fallback_active = false;
        return result;
    };
#if defined(_WIN32)
    if (impl_->pending_fence_value != 0U) {
        const auto progress = synchronize(false);
        if (impl_->pending_fence_value != 0U) {
            save_result(*impl_, NativeD3D12RayTracingContextFailureStage::synchronization,
                        "native-d3d12.context.output-copy-pending",
                        "An earlier asynchronous trace or output copy is still in flight; poll or wait with synchronize() before issuing another copy.");
            auto pending = receipt_from(*impl_, "copy-output");
            pending.state = progress.state == NativeD3D12RayTracingContextState::failed
                ? NativeD3D12RayTracingContextState::failed
                : NativeD3D12RayTracingContextState::ready;
            pending.fallback_active = false;
            return pending;
        }
    }
#endif
    if (!is_private_output_surface_view_current(view)) {
        return fail("native-d3d12.context.output-view-stale",
                    "The output view token, generation, resource state or fence proof is no longer current.");
    }
    if (destination_resource == nullptr) {
        return fail("native-d3d12.context.output-destination-null",
                    "A runtime-private copy requires a non-null same-device destination resource.");
    }
#if !defined(_WIN32)
    (void)destination_resource;
    return fail("native-d3d12.context.platform-unavailable",
                "The runtime-private D3D12 output copy is unavailable on this platform.");
#else
    auto* destination = reinterpret_cast<ID3D12Resource*>(destination_resource);
    if (destination == impl_->output_resource.Get()) {
        return fail("native-d3d12.context.output-destination-alias",
                    "The output copy destination must be a distinct resource; self-copy is rejected.");
    }
    ComPtr<ID3D12Device> destination_device;
    HRESULT hr = destination->GetDevice(
        __uuidof(ID3D12Device),
        reinterpret_cast<void**>(destination_device.GetAddressOf()));
    if (FAILED(hr) || !destination_device) {
        return fail("native-d3d12.context.output-destination-device-query-failed",
                    "The output copy destination did not expose its owning D3D12 device (" +
                        hresult_hex(hr) + ").");
    }
    ComPtr<IUnknown> destination_identity;
    ComPtr<IUnknown> source_identity;
    hr = destination_device->QueryInterface(
        __uuidof(IUnknown),
        reinterpret_cast<void**>(destination_identity.GetAddressOf()));
    if (SUCCEEDED(hr)) {
        hr = impl_->device->QueryInterface(
            __uuidof(IUnknown),
            reinterpret_cast<void**>(source_identity.GetAddressOf()));
    }
    if (FAILED(hr) || !destination_identity || !source_identity ||
        destination_identity.Get() != source_identity.Get()) {
        return fail("native-d3d12.context.output-destination-device-mismatch",
                    "The output copy destination belongs to a different D3D12 device; cross-device sharing is rejected.");
    }
    const auto source_description = impl_->output_resource->GetDesc();
    const auto destination_description = destination->GetDesc();
    if (source_description.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER) {
        return fail("native-d3d12.context.output-destination-layout-unsupported",
                    "The retained native output is not the expected linear source buffer.");
    }
    const bool destination_is_buffer =
        destination_description.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER;
    const bool destination_is_texture =
        destination_description.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    const auto row_bytes = static_cast<std::uint64_t>(impl_->options.output_width) *
        sizeof(std::uint32_t) * 4U;
    if (destination_is_buffer && destination_description.Width < source_description.Width) {
        return fail("native-d3d12.context.output-destination-buffer-too-small",
                    "The same-device buffer destination is smaller than the retained native output.");
    }
    if (destination_is_texture &&
        (destination_description.Width != impl_->options.output_width ||
         destination_description.Height != impl_->options.output_height ||
         destination_description.DepthOrArraySize != 1U ||
         destination_description.MipLevels != 1U ||
         destination_description.Format != DXGI_FORMAT_R32G32B32A32_UINT ||
         destination_description.SampleDesc.Count != 1U ||
         row_bytes == 0U ||
         (row_bytes % D3D12_TEXTURE_DATA_PITCH_ALIGNMENT) != 0U)) {
        return fail("native-d3d12.context.output-destination-texture-footprint-unsupported",
                    "The exported texture must be a single-sample R32G32B32A32_UINT 2D surface with exact dimensions and a 256-byte-aligned linear row footprint.");
    }
    if (!destination_is_buffer && !destination_is_texture) {
        return fail("native-d3d12.context.output-destination-layout-unsupported",
                    "Only a same-device buffer or exact exported 2D texture can receive the native output.");
    }
    hr = impl_->command_allocator->Reset();
    if (FAILED(hr)) {
        return fail("native-d3d12.context.output-copy-command-allocator-reset-failed",
                    "Resetting the output copy command allocator failed with " + hresult_hex(hr) + ".");
    }
    hr = impl_->command_list->Reset(impl_->command_allocator.Get(), nullptr);
    if (FAILED(hr)) {
        return fail("native-d3d12.context.output-copy-command-list-reset-failed",
                    "Resetting the output copy command list failed with " + hresult_hex(hr) + ".");
    }
    // Buffer destinations retain the original low-level contract and must
    // already be in COPY_DEST.  Exported SDL textures are created with SAMPLER
    // usage, whose SDL D3D12 default state is ALL_SHADER_RESOURCE; transition
    // to COPY_DEST and restore that same default so SDL's own state tracker and
    // the native command stream agree at the hand-off boundary.
    if (destination_is_buffer) {
        impl_->command_list->CopyBufferRegion(
            destination, 0U, impl_->output_resource.Get(), 0U,
            impl_->output_resource_bytes);
    } else {
        transition_resource(impl_->command_list.Get(), destination,
                            D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE,
                            D3D12_RESOURCE_STATE_COPY_DEST);
        D3D12_TEXTURE_COPY_LOCATION source_location{};
        source_location.pResource = impl_->output_resource.Get();
        source_location.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        source_location.PlacedFootprint.Offset = 0U;
        source_location.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R32G32B32A32_UINT;
        source_location.PlacedFootprint.Footprint.Width = impl_->options.output_width;
        source_location.PlacedFootprint.Footprint.Height = impl_->options.output_height;
        source_location.PlacedFootprint.Footprint.Depth = 1U;
        source_location.PlacedFootprint.Footprint.RowPitch = static_cast<UINT>(row_bytes);
        D3D12_TEXTURE_COPY_LOCATION destination_location{};
        destination_location.pResource = destination;
        destination_location.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        destination_location.SubresourceIndex = 0U;
        impl_->command_list->CopyTextureRegion(
            &destination_location, 0U, 0U, 0U, &source_location, nullptr);
        transition_resource(impl_->command_list.Get(), destination,
                            D3D12_RESOURCE_STATE_COPY_DEST,
                            D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
    }
    hr = impl_->command_list->Close();
    if (FAILED(hr)) {
        return fail("native-d3d12.context.output-copy-command-list-close-failed",
                    "Closing the output copy command list failed with " + hresult_hex(hr) + ".");
    }
    std::uint64_t submitted_fence_value = 0U;
    std::uint64_t completed_fence_value = 0U;
    hr = submit_and_wait(impl_->command_queue.Get(), impl_->fence.Get(),
                         impl_->command_list.Get(), impl_->fence_event,
                         impl_->next_fence_value,
                         &submitted_fence_value, &completed_fence_value,
                         impl_->options.synchronization_policy ==
                             NativeD3D12RayTracingSynchronizationPolicy::wait_for_completion);
    if (FAILED(hr)) {
        return fail("native-d3d12.context.output-copy-wait-failed",
                    "Waiting for the GPU-only output copy failed with " + hresult_hex(hr) + ".");
    }
    impl_->last_submitted_fence_value = submitted_fence_value;
    impl_->last_completed_fence_value = completed_fence_value;
    impl_->last_output_copy_submitted = true;
    if (completed_fence_value < submitted_fence_value) {
        impl_->pending_gpu_operation = Impl::PendingGpuOperation::output_copy;
        impl_->pending_fence_value = submitted_fence_value;
        save_result(*impl_, NativeD3D12RayTracingContextFailureStage::synchronization,
                    "native-d3d12.context.output-copy-submitted",
                    "The GPU-only output copy was submitted without a CPU wait; poll or wait with synchronize() before reusing the command list.");
        auto result = receipt_from(*impl_, "copy-output");
        result.state = NativeD3D12RayTracingContextState::ready;
        return result;
    }
    impl_->last_output_copy_completed = true;
    impl_->last_synchronization_completed = true;
    save_result(*impl_, NativeD3D12RayTracingContextFailureStage::none,
                "native-d3d12.context.output-copy-complete",
                destination_is_texture
                    ? "The retained linear output was copied into an exact same-device D3D12 texture footprint and restored to SDL's shader-resource default state; no CPU readback was used."
                    : "The retained output was copied to a same-device D3D12 buffer by GPU command; no CPU readback or native handle was returned in the receipt.");
    auto result = receipt_from(*impl_, "copy-output");
    result.state = NativeD3D12RayTracingContextState::ready;
    return result;
#endif
}

NativeD3D12RayTracingContextReceipt NativeD3D12RayTracingContext::status() const {
    return receipt_from(*impl_, "status");
}

NativeD3D12RayTracingContextState NativeD3D12RayTracingContext::state() const noexcept {
    return impl_->state;
}

std::uint64_t NativeD3D12RayTracingContext::generation() const noexcept {
    return impl_->generation;
}

std::uint64_t NativeD3D12RayTracingContext::scene_generation() const noexcept {
    return impl_->scene_generation;
}

bool NativeD3D12RayTracingContext::is_shutdown() const noexcept {
    return impl_->state == NativeD3D12RayTracingContextState::shutdown;
}

} // namespace noemancer
