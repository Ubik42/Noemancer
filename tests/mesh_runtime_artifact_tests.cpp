#include "engine/mesh_runtime_artifact.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

using noemancer::GltfDecodedImage;
using noemancer::GltfDecodedPrimitive;
using noemancer::GltfDecodedVertex;
using noemancer::GltfMeshData;

constexpr std::string_view source_hash =
    "sha256:0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";

bool near(const float left, const float right) {
    return std::abs(left - right) <= 0.00001F;
}

template <typename T, std::size_t N>
bool near_array(const std::array<T, N>& left, const std::array<T, N>& right) {
    for (std::size_t index = 0U; index < N; ++index) {
        if constexpr (std::is_floating_point_v<T>) {
            if (!near(left[index], right[index])) return false;
        } else if (left[index] != right[index]) {
            return false;
        }
    }
    return true;
}

bool same_single_triangle(const GltfMeshData& expected, const GltfMeshData& actual) {
    if (expected.vertices.size() != 3U || expected.indices.size() != 3U ||
        actual.vertices.size() != expected.vertices.size() || actual.indices.size() != expected.indices.size()) {
        return false;
    }
    std::array<std::uint32_t, 3> actual_to_expected{};
    std::array<bool, 3> matched{};
    for (std::size_t actual_index = 0U; actual_index < actual.vertices.size(); ++actual_index) {
        for (std::size_t expected_index = 0U; expected_index < expected.vertices.size(); ++expected_index) {
            const auto& actual_vertex = actual.vertices[actual_index];
            const auto& expected_vertex = expected.vertices[expected_index];
            if (!matched[expected_index] && near_array(actual_vertex.position, expected_vertex.position) &&
                near_array(actual_vertex.normal, expected_vertex.normal) &&
                near_array(actual_vertex.texcoord, expected_vertex.texcoord) &&
                near_array(actual_vertex.tangent, expected_vertex.tangent) &&
                near_array(actual_vertex.joints, expected_vertex.joints) &&
                near_array(actual_vertex.weights, expected_vertex.weights)) {
                actual_to_expected[actual_index] = static_cast<std::uint32_t>(expected_index);
                matched[expected_index] = true;
                break;
            }
        }
    }
    if (!std::all_of(matched.begin(), matched.end(), [](const bool value) { return value; })) return false;
    std::array<std::uint32_t, 3> expected_triangle = {
        expected.indices[0], expected.indices[1], expected.indices[2]};
    std::array<std::uint32_t, 3> actual_triangle{};
    for (std::size_t index = 0U; index < actual.indices.size(); ++index) {
        if (actual.indices[index] >= actual_to_expected.size()) return false;
        actual_triangle[index] = actual_to_expected[actual.indices[index]];
    }
    std::ranges::sort(expected_triangle);
    std::ranges::sort(actual_triangle);
    return expected_triangle == actual_triangle;
}

GltfMeshData make_mesh() {
    GltfMeshData mesh;
    mesh.valid = true;
    mesh.code = "ok";
    mesh.detail = "fixture";
    mesh.vertices = {
        GltfDecodedVertex{
            .position = {-1.0F, -1.0F, 0.0F},
            .normal = {0.0F, 0.0F, 1.0F},
            .texcoord = {0.0F, 0.0F},
            .tangent = {1.0F, 0.0F, 0.0F, 1.0F},
            .joints = {0U, 0U, 0U, 0U},
            .weights = {1.0F, 0.0F, 0.0F, 0.0F}},
        GltfDecodedVertex{
            .position = {1.0F, -1.0F, 0.0F},
            .normal = {0.0F, 0.0F, 1.0F},
            .texcoord = {1.0F, 0.0F},
            .tangent = {1.0F, 0.0F, 0.0F, 1.0F},
            .joints = {0U, 0U, 0U, 0U},
            .weights = {1.0F, 0.0F, 0.0F, 0.0F}},
        GltfDecodedVertex{
            .position = {0.0F, 1.0F, 0.0F},
            .normal = {0.0F, 0.0F, 1.0F},
            .texcoord = {0.5F, 1.0F},
            .tangent = {1.0F, 0.0F, 0.0F, 1.0F},
            .joints = {0U, 0U, 0U, 0U},
            .weights = {1.0F, 0.0F, 0.0F, 0.0F}}
    };
    mesh.indices = {0U, 1U, 2U};

    GltfDecodedPrimitive primitive;
    primitive.first_index = 0U;
    primitive.index_count = 3U;
    primitive.base_color = {0.8F, 0.7F, 0.6F, 0.9F};
    primitive.metallic = 0.35F;
    primitive.roughness = 0.45F;
    primitive.unlit = true;
    primitive.base_color_image = 0;
    primitive.normal_image = 0;
    primitive.metallic_roughness_image = 0;
    primitive.occlusion_image = 0;
    primitive.emissive_image = 0;
    primitive.emissive_factor = {0.1F, 0.2F, 0.3F};
    primitive.normal_scale = 0.75F;
    primitive.occlusion_strength = 0.6F;
    primitive.alpha_cutoff = 0.4F;
    primitive.alpha_mode = "MASK";
    primitive.double_sided = true;
    primitive.node_name = "FixtureNode";
    primitive.mesh_name = "FixtureMesh";
    primitive.skin = -1;
    primitive.bounds_center = {0.0F, 0.0F, 0.0F};
    primitive.bounds_radius = 1.5F;
    mesh.primitives.push_back(primitive);

    GltfDecodedImage image;
    image.valid = true;
    image.code = "ok";
    image.mime_type = "image/png";
    image.width = 2U;
    image.height = 2U;
    image.rgba8 = {
        0xffU, 0x00U, 0x00U, 0xffU,
        0x00U, 0xffU, 0x00U, 0xffU,
        0x00U, 0x00U, 0xffU, 0xffU,
        0xffU, 0xffU, 0x00U, 0xffU
    };
    mesh.images.push_back(std::move(image));
    return mesh;
}

noemancer::CookSource make_source() {
    return noemancer::CookSource{
        .asset_id = "mesh.fixture.runtime",
        .source_uri = "asset://fixtures/runtime.glb",
        .source_hash = std::string(source_hash),
        .source_bytes = 1024U,
        .importer = "gltf.binary/0.1"
    };
}

std::uint32_t read_u32(const std::span<const std::byte> bytes, const std::size_t offset) {
    if (offset > bytes.size() || bytes.size() - offset < 4U) return 0U;
    std::uint32_t result{};
    for (std::size_t index = 0U; index < 4U; ++index)
        result |= static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(bytes[offset + index])) << (index * 8U);
    return result;
}

std::uint64_t read_u64(const std::span<const std::byte> bytes, const std::size_t offset) {
    if (offset > bytes.size() || bytes.size() - offset < 8U) return 0U;
    std::uint64_t result{};
    for (std::size_t index = 0U; index < 8U; ++index)
        result |= static_cast<std::uint64_t>(std::to_integer<std::uint8_t>(bytes[offset + index])) << (index * 8U);
    return result;
}

bool replace_manifest_text(std::vector<std::byte>& payload,
                           const std::string_view from, const std::string_view to) {
    if (from.size() != to.size() || payload.size() < 48U) return false;
    const auto manifest_bytes = read_u32(payload, 16U);
    if (manifest_bytes == 0U || 48U + manifest_bytes > payload.size()) return false;
    auto* manifest_begin = reinterpret_cast<char*>(payload.data() + 48U);
    std::string manifest(manifest_begin, manifest_bytes);
    const auto position = manifest.find(from);
    if (position == std::string::npos) return false;
    manifest.replace(position, from.size(), to);
    std::copy(manifest.begin(), manifest.end(), manifest_begin);
    return true;
}

bool dependency_unavailable(const std::string_view code) {
    return code == "asset.meshopt-unavailable" || code == "asset.ktx2-unavailable";
}

int fail(const int code, const std::string_view detail) {
    std::cerr << "mesh_runtime_artifact_tests: " << detail << '\n';
    return code;
}

} // namespace

int main() {
    using namespace noemancer;

    const auto source = make_source();
    const auto mesh = make_mesh();
    auto profile = cook_platform_profile("windows-x64-debug");
    MeshCookSettings settings;
    settings.lod_ratios = {1.0F};

    const auto cooked = cook_mesh_runtime_artifact(source, mesh, profile, settings);
    if (!cooked.success) {
        if (dependency_unavailable(cooked.code)) {
            std::cout << "mesh_runtime_artifact_tests: dependency unavailable; skipped\n";
            return 0;
        }
        return fail(1, "minimal engine-owned mesh did not Cook: " + cooked.code + " - " + cooked.detail);
    }
    if (cooked.schema_version != mesh_runtime_artifact_schema || cooked.asset_id != source.asset_id ||
        cooked.source_hash != source.source_hash || cooked.payload.empty() ||
        cooked.payload_hash.size() != 71U || !cooked.payload_hash.starts_with("sha256:") ||
        cooked.lod_count != 1U || cooked.primitive_count != 1U || cooked.image_count != 1U) {
        return fail(2, "Cook result did not expose the stable artifact identity and counts");
    }

    const auto repeated = cook_mesh_runtime_artifact(source, mesh, profile, settings);
    if (!repeated.success || repeated.payload != cooked.payload ||
        repeated.payload_hash != cooked.payload_hash || repeated.lod_count != cooked.lod_count ||
        repeated.primitive_count != cooked.primitive_count || repeated.image_count != cooked.image_count) {
        return fail(3, "Cook output was not deterministic");
    }

    const auto loaded = load_mesh_runtime_artifact(cooked.payload, source.asset_id,
        source.source_hash, cooked.payload_hash);
    if (!loaded.success || loaded.code != "ok" || loaded.schema_version != mesh_runtime_artifact_schema ||
        loaded.asset_id != source.asset_id || loaded.source_hash != source.source_hash ||
        loaded.payload_hash != cooked.payload_hash || loaded.lod_count != 1U || !loaded.mesh.valid ||
        !same_single_triangle(mesh, loaded.mesh) ||
        loaded.mesh.primitives.size() != 1U || loaded.mesh.images.size() != 1U) {
        return fail(4, "Cooked artifact did not load with the expected identity and geometry: " +
            loaded.code + " - " + loaded.detail);
    }
    const auto loaded_repeat = load_mesh_runtime_artifact(cooked.payload, source.asset_id,
        source.source_hash, cooked.payload_hash);
    if (!loaded_repeat.success || loaded_repeat.payload_hash != loaded.payload_hash ||
        !same_single_triangle(loaded.mesh, loaded_repeat.mesh) ||
        loaded_repeat.mesh.primitives.size() != loaded.mesh.primitives.size() ||
        loaded_repeat.mesh.images.size() != loaded.mesh.images.size()) {
        return fail(5, "Loading the same artifact was not deterministic");
    }

    const auto& expected_primitive = mesh.primitives.front();
    const auto& actual_primitive = loaded.mesh.primitives.front();
    if (actual_primitive.first_index != expected_primitive.first_index ||
        actual_primitive.index_count != expected_primitive.index_count ||
        !near_array(actual_primitive.base_color, expected_primitive.base_color) ||
        !near(actual_primitive.metallic, expected_primitive.metallic) ||
        !near(actual_primitive.roughness, expected_primitive.roughness) ||
        actual_primitive.unlit != expected_primitive.unlit ||
        actual_primitive.base_color_image != expected_primitive.base_color_image ||
        actual_primitive.normal_image != expected_primitive.normal_image ||
        actual_primitive.metallic_roughness_image != expected_primitive.metallic_roughness_image ||
        actual_primitive.occlusion_image != expected_primitive.occlusion_image ||
        actual_primitive.emissive_image != expected_primitive.emissive_image ||
        !near_array(actual_primitive.emissive_factor, expected_primitive.emissive_factor) ||
        !near(actual_primitive.normal_scale, expected_primitive.normal_scale) ||
        !near(actual_primitive.occlusion_strength, expected_primitive.occlusion_strength) ||
        !near(actual_primitive.alpha_cutoff, expected_primitive.alpha_cutoff) ||
        actual_primitive.alpha_mode != expected_primitive.alpha_mode ||
        actual_primitive.double_sided != expected_primitive.double_sided ||
        actual_primitive.node_name != expected_primitive.node_name ||
        actual_primitive.mesh_name != expected_primitive.mesh_name ||
        actual_primitive.skin != expected_primitive.skin ||
        !near_array(actual_primitive.bounds_center, expected_primitive.bounds_center) ||
        !near(actual_primitive.bounds_radius, expected_primitive.bounds_radius)) {
        return fail(6, "Primitive material, alpha, names or bounds were not preserved");
    }

    const auto& actual_image = loaded.mesh.images.front();
    if (!actual_image.valid || actual_image.code != "ok" ||
        actual_image.mime_type != "image/ktx2-cooked" || actual_image.width != 2U ||
        actual_image.height != 2U || actual_image.rgba8.size() != mesh.images.front().rgba8.size()) {
        return fail(7, "Embedded image did not make a valid KTX2 round-trip");
    }

    auto multi_primitive_mesh = mesh;
    multi_primitive_mesh.indices.insert(multi_primitive_mesh.indices.end(), {0U, 2U, 1U});
    auto second_primitive = multi_primitive_mesh.primitives.front();
    second_primitive.first_index = 3U;
    second_primitive.base_color = {0.1F, 0.25F, 0.9F, 1.0F};
    second_primitive.mesh_name = "FixtureMeshSecond";
    multi_primitive_mesh.primitives.push_back(second_primitive);
    const auto multi_cooked = cook_mesh_runtime_artifact(source, multi_primitive_mesh, profile, settings);
    const auto multi_loaded = multi_cooked.success
        ? load_mesh_runtime_artifact(multi_cooked.payload, source.asset_id, source.source_hash,
            multi_cooked.payload_hash)
        : MeshRuntimeArtifactLoadResult{};
    if (!multi_cooked.success || !multi_loaded.success || multi_loaded.mesh.primitives.size() != 2U ||
        multi_loaded.mesh.indices.size() != 6U || multi_loaded.mesh.primitives[0].first_index != 0U ||
        multi_loaded.mesh.primitives[0].index_count != 3U ||
        multi_loaded.mesh.primitives[1].first_index != 3U ||
        multi_loaded.mesh.primitives[1].index_count != 3U ||
        !near_array(multi_loaded.mesh.primitives[1].base_color, second_primitive.base_color)) {
        return fail(21, "Per-primitive Cook did not preserve independent geometry and material ranges");
    }

    const auto wrong_asset = load_mesh_runtime_artifact(cooked.payload, "mesh.fixture.other",
        source.source_hash, cooked.payload_hash);
    if (wrong_asset.success || wrong_asset.code != "mesh.artifact-identity-mismatch")
        return fail(8, "Mismatched expected asset identity was accepted");
    const auto wrong_source = load_mesh_runtime_artifact(cooked.payload, source.asset_id,
        "sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", cooked.payload_hash);
    if (wrong_source.success || wrong_source.code != "mesh.artifact-identity-mismatch")
        return fail(9, "Mismatched expected source identity was accepted");
    const auto wrong_payload = load_mesh_runtime_artifact(cooked.payload, source.asset_id,
        source.source_hash, "sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
    if (wrong_payload.success || wrong_payload.code != "mesh.artifact-hash-mismatch")
        return fail(10, "Mismatched expected payload identity was accepted");

    if (cooked.payload.size() <= 48U) return fail(11, "Cooked payload was unexpectedly smaller than its envelope");
    auto truncated_header = cooked.payload;
    truncated_header.resize(20U);
    const auto truncated_header_result = load_mesh_runtime_artifact(truncated_header);
    if (truncated_header_result.success || truncated_header_result.code != "mesh.artifact-header-invalid")
        return fail(12, "Truncated artifact header was accepted");

    auto truncated_payload = cooked.payload;
    truncated_payload.pop_back();
    const auto truncated_result = load_mesh_runtime_artifact(truncated_payload);
    if (truncated_result.success || truncated_result.code != "mesh.artifact-range-invalid")
        return fail(13, "Truncated artifact section was accepted");

    const auto manifest_bytes = read_u32(cooked.payload, 16U);
    const auto geometry_bytes = read_u64(cooked.payload, 32U);
    const auto texture_begin = 48U + static_cast<std::size_t>(manifest_bytes) +
        static_cast<std::size_t>(geometry_bytes);
    if (texture_begin >= cooked.payload.size()) return fail(14, "Cooked texture section was not present");
    auto tampered = cooked.payload;
    tampered[texture_begin] = static_cast<std::byte>(
        std::to_integer<std::uint8_t>(tampered[texture_begin]) ^ 0x01U);
    const auto tampered_hash_result = load_mesh_runtime_artifact(tampered, source.asset_id,
        source.source_hash, cooked.payload_hash);
    if (tampered_hash_result.success || tampered_hash_result.code != "mesh.artifact-hash-mismatch")
        return fail(15, "Tampered texture bytes bypassed the expected payload hash");
    const auto tampered_section_result = load_mesh_runtime_artifact(tampered, source.asset_id,
        source.source_hash);
    if (tampered_section_result.success || tampered_section_result.code != "mesh.artifact-section-hash-mismatch")
        return fail(16, "Tampered texture bytes bypassed the section hash");

    auto invalid_primitive = cooked.payload;
    if (!replace_manifest_text(invalid_primitive, "\"firstIndex\":0", "\"firstIndex\":1"))
        return fail(17, "Could not locate the serialized primitive range");
    const auto invalid_primitive_result = load_mesh_runtime_artifact(invalid_primitive,
        source.asset_id, source.source_hash);
    if (invalid_primitive_result.success || invalid_primitive_result.code != "mesh.artifact-primitive-invalid")
        return fail(18, "Out-of-range serialized primitive range was accepted");

    auto invalid_mesh = mesh;
    invalid_mesh.indices.front() = 99U;
    const auto invalid_mesh_result = cook_mesh_runtime_artifact(source, invalid_mesh, profile, settings);
    if (invalid_mesh_result.success || invalid_mesh_result.code != "asset.mesh-input-index-range")
        return fail(19, "Out-of-range source mesh index was accepted during Cook");

    auto invalid_image = mesh;
    invalid_image.images.front().valid = false;
    const auto invalid_image_result = cook_mesh_runtime_artifact(source, invalid_image, profile, settings);
    if (invalid_image_result.success || invalid_image_result.code != "mesh.artifact-referenced-image-invalid")
        return fail(20, "Referenced invalid embedded image was accepted during Cook");

    std::cout << "mesh_runtime_artifact_tests: ok\n";
    return 0;
}
