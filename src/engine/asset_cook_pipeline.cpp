#include "engine/asset_cook_pipeline.hpp"

#include <nlohmann/json.hpp>

#if __has_include(<meshoptimizer.h>)
#include <meshoptimizer.h>
#define NOEMANCER_HAS_MESHOPTIMIZER 1
#else
#define NOEMANCER_HAS_MESHOPTIMIZER 0
#endif

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <iomanip>
#include <limits>
#include <sstream>
#include <span>
#include <stdexcept>

namespace noemancer {
namespace {

using Json = nlohmann::json;

constexpr std::string_view kTexturePipeline = "ktx2-basisu/0.1";
constexpr std::string_view kMeshPipeline = "meshoptimizer/1.2";

std::string hex_u64(const std::uint64_t value) {
    std::ostringstream stream;
    stream << std::hex << std::setfill('0') << std::setw(16) << value;
    return stream.str();
}

std::uint64_t fnv1a(const std::string_view value) {
    std::uint64_t hash = 14695981039346656037ULL;
    for (const auto character : value) {
        hash ^= static_cast<std::uint8_t>(character);
        hash *= 1099511628211ULL;
    }
    return hash;
}

std::uint64_t fnv1a(const std::span<const std::byte> bytes) {
    std::uint64_t hash = 14695981039346656037ULL;
    for (const auto byte : bytes) {
        hash ^= std::to_integer<std::uint8_t>(byte);
        hash *= 1099511628211ULL;
    }
    return hash;
}

std::string bool_name(const bool value) { return value ? "true" : "false"; }

std::string float_name(const float value) {
    if (!std::isfinite(value)) return "invalid";
    std::ostringstream stream;
    stream << std::setprecision(std::numeric_limits<float>::max_digits10) << value;
    return stream.str();
}

std::string profile_material(const CookPlatformProfile& profile) {
    return profile.id + "\n" + profile.texture_color_target + "\n" +
        profile.texture_normal_target + "\n" + profile.texture_mask_target + "\n" +
        profile.texture_hdr_target + "\n" + profile.texture_fallback_target + "\n" +
        profile.mesh_target + "\n" + profile.meshopt_version + "\n" +
        std::to_string(profile.texture_page_bytes) + "\n" +
        std::to_string(profile.mesh_page_bytes) + "\n" +
        bool_name(profile.generate_mipmaps) + "\n" +
        bool_name(profile.texture_streaming) + "\n" + bool_name(profile.mesh_streaming);
}

std::string source_error(const CookSource& source) {
    if (source.asset_id.empty()) return "Cook source asset ID is empty.";
    if (source.source_uri.empty()) return "Cook source URI is empty.";
    if (source.source_hash.empty()) return "Cook source hash is empty; content identity is required.";
    if (source.importer.empty()) return "Cook source importer is empty.";
    return {};
}

std::string semantic_target(const CookPlatformProfile& profile, const TextureSemantic semantic) {
    switch (semantic) {
    case TextureSemantic::normal: return profile.texture_normal_target;
    case TextureSemantic::metallic_roughness:
    case TextureSemantic::occlusion:
    case TextureSemantic::data: return profile.texture_mask_target;
    case TextureSemantic::hdr: return profile.texture_hdr_target;
    case TextureSemantic::base_color:
    case TextureSemantic::emissive:
    case TextureSemantic::ui: return profile.texture_color_target;
    }
    return {};
}

bool valid_lod_ratios(const std::vector<float>& lod_ratios) {
    if (lod_ratios.empty() || lod_ratios.size() > 16U) return false;
    if (lod_ratios.front() != 1.0F) return false;
    for (std::size_t index = 0; index < lod_ratios.size(); ++index) {
        const auto ratio = lod_ratios[index];
        if (!std::isfinite(ratio) || ratio <= 0.0F || ratio > 1.0F) return false;
        if (index > 0U && ratio > lod_ratios[index - 1U]) return false;
    }
    return true;
}

bool has_reduced_lod(const std::vector<float>& lod_ratios) {
    return std::ranges::any_of(lod_ratios, [](const float ratio) { return ratio < 1.0F; });
}

bool valid_mesh_input(const CookMeshInput& input, std::string& code, std::string& detail) {
    if (input.vertex_stride == 0U || input.vertex_stride > 256U || input.vertex_stride % 4U != 0U) {
        code = "asset.mesh-input-stride";
        detail = "Mesh vertex stride must be a non-zero multiple of four and no larger than 256 bytes.";
        return false;
    }
    if (input.vertices.empty() || input.vertices.size() % input.vertex_stride != 0U) {
        code = "asset.mesh-input-vertices";
        detail = "Mesh vertex bytes must contain complete interleaved vertices.";
        return false;
    }
    if (input.indices.empty() || input.indices.size() % 3U != 0U) {
        code = "asset.mesh-input-indices";
        detail = "Mesh indices must be a non-empty triangle list.";
        return false;
    }
    const auto vertex_count = input.vertices.size() / input.vertex_stride;
    if (vertex_count > std::numeric_limits<std::uint32_t>::max() ||
        input.indices.size() > std::numeric_limits<std::uint32_t>::max()) {
        code = "asset.mesh-input-too-large";
        detail = "Mesh input exceeds the 32-bit runtime index contract.";
        return false;
    }
    if (input.position_offset % alignof(float) != 0U || input.position_offset > input.vertex_stride ||
        input.position_offset + sizeof(float) * 3U > input.vertex_stride) {
        code = "asset.mesh-input-position";
        detail = "Mesh position offset must identify an aligned float3 inside every vertex.";
        return false;
    }
    for (const auto index : input.indices) {
        if (index >= vertex_count) {
            code = "asset.mesh-input-index-range";
            detail = "Mesh index references a vertex outside the input vertex buffer.";
            return false;
        }
    }
    return true;
}

void append_u32(std::vector<std::byte>& bytes, const std::uint32_t value) {
    bytes.push_back(static_cast<std::byte>(value & 0xffU));
    bytes.push_back(static_cast<std::byte>((value >> 8U) & 0xffU));
    bytes.push_back(static_cast<std::byte>((value >> 16U) & 0xffU));
    bytes.push_back(static_cast<std::byte>((value >> 24U) & 0xffU));
}

void append_u64(std::vector<std::byte>& bytes, const std::uint64_t value) {
    for (std::uint32_t shift = 0U; shift < 64U; shift += 8U) {
        bytes.push_back(static_cast<std::byte>((value >> shift) & 0xffULL));
    }
}

void append_f32(std::vector<std::byte>& bytes, const float value) {
    append_u32(bytes, std::bit_cast<std::uint32_t>(value));
}

void append_blob(std::vector<std::byte>& bytes, const std::vector<std::byte>& blob) {
    append_u64(bytes, static_cast<std::uint64_t>(blob.size()));
    bytes.insert(bytes.end(), blob.begin(), blob.end());
}

void append_mesh_magic(std::vector<std::byte>& bytes) {
    constexpr std::array<char, 8> magic{'N', 'M', 'M', 'S', 'H', '0', '0', '1'};
    for (const auto value : magic) bytes.push_back(static_cast<std::byte>(value));
}

std::vector<std::byte> serialize_mesh_product(const MeshCookProduct& product) {
    std::vector<std::byte> bytes;
    bytes.reserve(64U);
    append_mesh_magic(bytes);
    append_u32(bytes, 1U); // meshbin schema version
    append_u32(bytes, product.vertex_stride);
    append_u32(bytes, product.source_vertex_count);
    append_u32(bytes, product.source_index_count);
    append_u32(bytes, static_cast<std::uint32_t>(product.lods.size()));
    for (const auto& lod : product.lods) {
        append_f32(bytes, lod.ratio);
        append_f32(bytes, lod.simplification_error);
        append_u32(bytes, lod.vertex_count);
        append_u32(bytes, lod.index_count);
        append_blob(bytes, lod.encoded_vertices);
        append_blob(bytes, lod.encoded_indices);
    }
    append_u64(bytes, fnv1a(std::span<const std::byte>(bytes.data(), bytes.size())));
    return bytes;
}

CookArtifactContract invalid_artifact(std::string code, std::string detail) {
    CookArtifactContract result;
    result.code = std::move(code);
    result.detail = std::move(detail);
    result.diagnostics.push_back(result.detail);
    return result;
}

void populate_common(CookArtifactContract& result, const CookSource& source,
    const CookPlatformProfile& profile, const std::string_view pipeline,
    const std::string_view artifact_kind, const std::string& settings) {
    result.schema = "noemancer.cook-artifact/0.1";
    result.artifact_kind = artifact_kind;
    result.source_asset_id = source.asset_id;
    result.source_uri = source.source_uri;
    result.source_hash = source.source_hash;
    result.target_profile = profile.id;
    result.pipeline = std::string(pipeline);
    result.source_bytes = source.source_bytes;
    result.settings_fingerprint = "fnv1a64:" + hex_u64(fnv1a(settings));
    const auto material = std::string(pipeline) + "\n" + profile_material(profile) + "\n" +
        source.source_hash + "\n" + settings;
    result.cache_key = "cook:" + hex_u64(fnv1a(material));
    result.artifact_uri = "cache://cook/" + result.cache_key;
}

Json artifact_json(const CookArtifactContract& artifact) {
    return {
        {"schema", artifact.schema},
        {"valid", artifact.valid},
        {"code", artifact.code},
        {"detail", artifact.detail},
        {"artifactKind", artifact.artifact_kind},
        {"source", {
            {"assetId", artifact.source_asset_id},
            {"uri", artifact.source_uri},
            {"hash", artifact.source_hash},
            {"bytes", artifact.source_bytes}
        }},
        {"targetProfile", artifact.target_profile},
        {"pipeline", artifact.pipeline},
        {"payload", {
            {"format", artifact.payload_format},
            {"target", artifact.payload_target},
            {"colorSpace", artifact.color_space},
            {"alphaMode", artifact.alpha_mode},
            {"indexFormat", artifact.index_format}
        }},
        {"cacheKey", artifact.cache_key},
        {"artifactUri", artifact.artifact_uri},
        {"payloadUri", artifact.payload_uri},
        {"streaming", {
            {"policy", artifact.stream_policy},
            {"pageBytes", artifact.page_bytes}
        }},
        {"settingsFingerprint", artifact.settings_fingerprint},
        {"lodRatios", artifact.lod_ratios},
        {"dependencies", artifact.dependencies},
        {"diagnostics", artifact.diagnostics}
    };
}

} // namespace

std::string texture_semantic_name(const TextureSemantic semantic) {
    switch (semantic) {
    case TextureSemantic::base_color: return "base-color";
    case TextureSemantic::normal: return "normal";
    case TextureSemantic::metallic_roughness: return "metallic-roughness";
    case TextureSemantic::occlusion: return "occlusion";
    case TextureSemantic::emissive: return "emissive";
    case TextureSemantic::ui: return "ui";
    case TextureSemantic::hdr: return "hdr";
    case TextureSemantic::data: return "data";
    }
    return "unknown";
}

std::string texture_alpha_mode_name(const TextureAlphaMode mode) {
    switch (mode) {
    case TextureAlphaMode::opaque: return "opaque";
    case TextureAlphaMode::mask: return "mask";
    case TextureAlphaMode::blend: return "blend";
    }
    return "unknown";
}

std::string mesh_index_format_name(const MeshIndexFormat format) {
    switch (format) {
    case MeshIndexFormat::automatic: return "automatic";
    case MeshIndexFormat::uint16: return "uint16";
    case MeshIndexFormat::uint32: return "uint32";
    }
    return "unknown";
}

CookPlatformProfile cook_platform_profile(const std::string_view profile_id) {
    CookPlatformProfile profile;
    profile.id = std::string(profile_id);
    profile.texture_color_target = "bc7-rgba-srgb";
    profile.texture_normal_target = "bc5-rg-unorm";
    profile.texture_mask_target = "bc4-r-unorm";
    profile.texture_hdr_target = "rgba16f";
    profile.texture_fallback_target = "rgba8";
    profile.mesh_target = "meshopt-index-vertex-fetch";
    profile.meshopt_version = "meshoptimizer/1.2";

    if (profile_id == "windows-x64-debug" || profile_id == "windows-x64-release" ||
        profile_id == "linux-x64-debug" || profile_id == "linux-x64-release") {
        return profile;
    }
    if (profile_id == "android-arm64-release" || profile_id == "ios-arm64-release") {
        profile.texture_color_target = "astc-6x6-rgba-srgb";
        profile.texture_normal_target = "astc-6x6-rg-unorm";
        profile.texture_mask_target = "astc-6x6-r-unorm";
        profile.texture_fallback_target = "etc2-rgba8";
        profile.texture_page_bytes = 128U * 1024U;
        profile.mesh_page_bytes = 128U * 1024U;
        return profile;
    }
    if (profile_id == "webgpu-release") {
        profile.texture_color_target = "etc2-rgba8-srgb";
        profile.texture_normal_target = "etc2-rg-unorm";
        profile.texture_mask_target = "etc2-rg-unorm";
        profile.texture_fallback_target = "rgba8";
        profile.texture_streaming = false;
        return profile;
    }
    return {};
}

bool validate_cook_platform_profile(const CookPlatformProfile& profile,
    std::string& code, std::string& detail) {
    if (profile.id.empty() || profile.texture_color_target.empty() ||
        profile.texture_normal_target.empty() || profile.texture_mask_target.empty() ||
        profile.texture_hdr_target.empty() || profile.texture_fallback_target.empty() ||
        profile.mesh_target.empty() || profile.meshopt_version.empty()) {
        code = "asset.cook-profile-invalid";
        detail = "Cook platform profile is incomplete or unknown.";
        return false;
    }
    if (profile.texture_page_bytes < 16U * 1024U || profile.texture_page_bytes > 16U * 1024U * 1024U ||
        profile.mesh_page_bytes < 16U * 1024U || profile.mesh_page_bytes > 16U * 1024U * 1024U) {
        code = "asset.cook-profile-page-size";
        detail = "Cook streaming page size must be between 16 KiB and 16 MiB.";
        return false;
    }
    code = "ok";
    detail = "Cook platform profile is valid.";
    return true;
}

std::string cook_platform_profile_fingerprint(const CookPlatformProfile& profile) {
    return "profile:" + profile.id + ":" + hex_u64(fnv1a(profile_material(profile)));
}

CookArtifactContract plan_texture_cook(const CookSource& source,
    const CookPlatformProfile& profile, const TextureCookSettings& settings) {
    if (const auto error = source_error(source); !error.empty()) {
        return invalid_artifact("asset.cook-source-invalid", error);
    }
    std::string profile_code;
    std::string profile_detail;
    if (!validate_cook_platform_profile(profile, profile_code, profile_detail)) {
        return invalid_artifact(profile_code, profile_detail);
    }
    if (settings.quality > 5U || (settings.max_dimension != 0U && settings.max_dimension > 32768U)) {
        return invalid_artifact("asset.texture-settings-invalid",
            "Texture quality must be 0..5 and max dimension must not exceed 32768.");
    }
    const auto target = semantic_target(profile, settings.semantic);
    if (target.empty()) return invalid_artifact("asset.texture-target-unavailable",
        "The selected profile has no target for this texture semantic.");
    if (settings.semantic == TextureSemantic::hdr && settings.srgb) {
        return invalid_artifact("asset.texture-color-space-invalid",
            "HDR textures must use linear color space.");
    }
    if (settings.semantic == TextureSemantic::normal && settings.srgb) {
        return invalid_artifact("asset.texture-color-space-invalid",
            "Normal maps must use linear color space.");
    }

    const auto semantic = texture_semantic_name(settings.semantic);
    const auto settings_material = semantic + "\n" + texture_alpha_mode_name(settings.alpha_mode) + "\n" +
        bool_name(settings.srgb) + "\n" + bool_name(settings.generate_mipmaps) + "\n" +
        bool_name(settings.streaming) + "\n" + std::to_string(settings.max_dimension) + "\n" +
        std::to_string(settings.quality);
    CookArtifactContract result;
    populate_common(result, source, profile, kTexturePipeline, "texture", settings_material);
    result.valid = true;
    result.code = "ok";
    result.detail = "Texture Cook contract is valid; KTX2/BasisU adapter may execute it.";
    result.payload_format = "ktx2";
    result.payload_target = target;
    result.color_space = settings.srgb ? "srgb" : "linear";
    result.alpha_mode = texture_alpha_mode_name(settings.alpha_mode);
    result.stream_policy = settings.streaming && profile.texture_streaming ? "mip-tail-pages" : "resident";
    result.page_bytes = profile.texture_page_bytes;
    result.payload_uri = result.artifact_uri + "/payload.ktx2";
    result.dependencies = {std::string(kTexturePipeline), cook_platform_profile_fingerprint(profile),
        "basisu-transcode-target:" + target};
    result.diagnostics = {"executionPending:KTX-Software/Basis-Universal adapter",
        std::string("sourceSemantic:") + semantic, std::string("mipmapPolicy:") +
        (settings.generate_mipmaps && profile.generate_mipmaps ? "full-chain" : "source-only")};
    return result;
}

CookArtifactContract plan_mesh_cook(const CookSource& source,
    const CookPlatformProfile& profile, const MeshCookSettings& settings) {
    if (const auto error = source_error(source); !error.empty()) {
        return invalid_artifact("asset.cook-source-invalid", error);
    }
    std::string profile_code;
    std::string profile_detail;
    if (!validate_cook_platform_profile(profile, profile_code, profile_detail)) {
        return invalid_artifact(profile_code, profile_detail);
    }
    if (!valid_lod_ratios(settings.lod_ratios)) {
        return invalid_artifact("asset.mesh-settings-invalid",
            "Mesh LOD ratios must be finite, start at 1.0 and monotonically decrease.");
    }
    const auto settings_material = bool_name(settings.optimize_vertex_fetch) + "\n" +
        bool_name(settings.optimize_overdraw) + "\n" + bool_name(settings.simplify_lods) + "\n" +
        bool_name(settings.quantize_attributes) + "\n" + bool_name(settings.streaming) + "\n" +
        mesh_index_format_name(settings.index_format) + "\n" + [&] {
            std::string value;
            for (const auto ratio : settings.lod_ratios) value += float_name(ratio) + ";";
            return value;
        }();
    CookArtifactContract result;
    populate_common(result, source, profile, kMeshPipeline, "mesh", settings_material);
    result.valid = true;
    result.code = "ok";
    result.detail = "Mesh Cook contract is valid; execute_mesh_cook can produce deterministic meshbin output.";
    result.payload_format = "meshopt/meshbin";
    result.payload_target = profile.mesh_target;
    result.index_format = mesh_index_format_name(settings.index_format);
    result.stream_policy = settings.streaming && profile.mesh_streaming ? "lod-pages" : "resident";
    result.page_bytes = profile.mesh_page_bytes;
    result.lod_ratios = settings.lod_ratios;
    result.payload_uri = result.artifact_uri + "/payload.meshbin";
    result.dependencies = {std::string(kMeshPipeline), cook_platform_profile_fingerprint(profile),
        std::string("vertex-fetch:") + bool_name(settings.optimize_vertex_fetch),
        std::string("overdraw:") + bool_name(settings.optimize_overdraw),
        std::string("quantization:") + bool_name(settings.quantize_attributes)};
    result.diagnostics = {"executor:meshoptimizer/1.2",
        "lodCount:" + std::to_string(settings.lod_ratios.size()),
        "streamPolicy:" + result.stream_policy};
    return result;
}

bool meshoptimizer_available() noexcept {
#if NOEMANCER_HAS_MESHOPTIMIZER
    return true;
#else
    return false;
#endif
}

MeshCookProduct execute_mesh_cook(const CookSource& source, const CookMeshInput& input,
    const CookPlatformProfile& profile, const MeshCookSettings& settings) {
    MeshCookProduct product;
    product.meshoptimizer_available = meshoptimizer_available();
    product.source_asset_id = source.asset_id;
    product.source_hash = source.source_hash;
    product.target_profile = profile.id;
    product.vertex_stride = input.vertex_stride;

    const auto plan = plan_mesh_cook(source, profile, settings);
    if (!plan.valid) {
        product.code = plan.code;
        product.detail = plan.detail;
        product.diagnostics = plan.diagnostics;
        return product;
    }
    product.cache_key = plan.cache_key;
    if (!product.meshoptimizer_available) {
        product.code = "asset.meshopt-unavailable";
        product.detail = "meshoptimizer.h was not available to the Cook target.";
        product.diagnostics.push_back(product.detail);
        return product;
    }
    if (!valid_mesh_input(input, product.code, product.detail)) {
        product.diagnostics.push_back(product.detail);
        return product;
    }
    if (!settings.simplify_lods && has_reduced_lod(settings.lod_ratios)) {
        product.code = "asset.mesh-settings-lod-simplification-disabled";
        product.detail = "Reduced LOD ratios require simplify_lods to be enabled.";
        product.diagnostics.push_back(product.detail);
        return product;
    }

#if NOEMANCER_HAS_MESHOPTIMIZER
    const auto source_vertex_count = input.vertices.size() / input.vertex_stride;
    product.source_vertex_count = static_cast<std::uint32_t>(source_vertex_count);
    product.source_index_count = static_cast<std::uint32_t>(input.indices.size());

    const std::vector<unsigned int> source_indices(input.indices.begin(), input.indices.end());
    std::vector<unsigned int> remap(source_vertex_count);
    const auto remapped_vertex_count = meshopt_generateVertexRemap(
        remap.data(), source_indices.data(), source_indices.size(), input.vertices.data(),
        source_vertex_count, input.vertex_stride);
    if (remapped_vertex_count == 0U) {
        product.code = "asset.meshopt-remap-failed";
        product.detail = "meshoptimizer could not generate a non-empty vertex remap.";
        product.diagnostics.push_back(product.detail);
        return product;
    }

    std::vector<std::byte> remapped_vertices(remapped_vertex_count * input.vertex_stride);
    meshopt_remapVertexBuffer(remapped_vertices.data(), input.vertices.data(), source_vertex_count,
        input.vertex_stride, remap.data());
    std::vector<unsigned int> remapped_indices(input.indices.size());
    meshopt_remapIndexBuffer(remapped_indices.data(), source_indices.data(), source_indices.size(), remap.data());

    // The documented meshoptimizer order is cache first, then overdraw.  The
    // fetch pass is applied separately to each LOD after simplification.
    std::vector<unsigned int> cache_indices(remapped_indices.size());
    meshopt_optimizeVertexCache(cache_indices.data(), remapped_indices.data(), remapped_indices.size(),
        remapped_vertex_count);
    remapped_indices.swap(cache_indices);

    const auto position_pointer = [&](const std::vector<std::byte>& vertices) {
        return reinterpret_cast<const float*>(vertices.data() + input.position_offset);
    };
    if (settings.optimize_overdraw) {
        std::vector<unsigned int> overdraw_indices(remapped_indices.size());
        meshopt_optimizeOverdraw(overdraw_indices.data(), remapped_indices.data(), remapped_indices.size(),
            position_pointer(remapped_vertices), remapped_vertex_count, input.vertex_stride, 1.05F);
        remapped_indices.swap(overdraw_indices);
    }

    for (const auto ratio : settings.lod_ratios) {
        std::vector<std::byte> lod_vertices = remapped_vertices;
        std::vector<unsigned int> lod_indices = remapped_indices;
        float simplification_error{};
        if (ratio < 1.0F && settings.simplify_lods) {
            auto target_index_count = static_cast<std::size_t>(
                std::floor(static_cast<double>(lod_indices.size()) * static_cast<double>(ratio)));
            target_index_count = std::max<std::size_t>(3U, target_index_count);
            target_index_count -= target_index_count % 3U;
            if (target_index_count < 3U) target_index_count = 3U;
            std::vector<unsigned int> simplified(lod_indices.size());
            const auto simplified_count = meshopt_simplify(
                simplified.data(), lod_indices.data(), lod_indices.size(), position_pointer(lod_vertices),
                remapped_vertex_count, input.vertex_stride, target_index_count, 0.01F, 0U,
                &simplification_error);
            if (simplified_count < 3U || simplified_count % 3U != 0U) {
                product.code = "asset.meshopt-simplify-failed";
                product.detail = "meshoptimizer could not produce a valid simplified triangle list.";
                product.diagnostics.push_back(product.detail);
                return product;
            }
            simplified.resize(simplified_count);
            lod_indices.swap(simplified);
            std::vector<unsigned int> simplified_cache(lod_indices.size());
            meshopt_optimizeVertexCache(simplified_cache.data(), lod_indices.data(), lod_indices.size(),
                remapped_vertex_count);
            lod_indices.swap(simplified_cache);
            if (settings.optimize_overdraw) {
                std::vector<unsigned int> simplified_overdraw(lod_indices.size());
                meshopt_optimizeOverdraw(simplified_overdraw.data(), lod_indices.data(), lod_indices.size(),
                    position_pointer(lod_vertices), remapped_vertex_count, input.vertex_stride, 1.05F);
                lod_indices.swap(simplified_overdraw);
            }
        }

        std::size_t lod_vertex_count = remapped_vertex_count;
        if (settings.optimize_vertex_fetch) {
            std::vector<std::byte> fetched_vertices(lod_vertices.size());
            lod_vertex_count = meshopt_optimizeVertexFetch(fetched_vertices.data(), lod_indices.data(),
                lod_indices.size(), lod_vertices.data(), remapped_vertex_count, input.vertex_stride);
            if (lod_vertex_count == 0U) {
                product.code = "asset.meshopt-fetch-failed";
                product.detail = "meshoptimizer could not produce a non-empty vertex-fetch order.";
                product.diagnostics.push_back(product.detail);
                return product;
            }
            fetched_vertices.resize(lod_vertex_count * input.vertex_stride);
            lod_vertices.swap(fetched_vertices);
        }

        const auto encoded_vertex_bound = meshopt_encodeVertexBufferBound(lod_vertex_count, input.vertex_stride);
        const auto encoded_index_bound = meshopt_encodeIndexBufferBound(lod_indices.size(), lod_vertex_count);
        if (encoded_vertex_bound == 0U || encoded_index_bound == 0U) {
            product.code = "asset.meshopt-encode-bound";
            product.detail = "meshoptimizer rejected the Cook vertex or index layout.";
            product.diagnostics.push_back(product.detail);
            return product;
        }
        MeshCookLod cooked_lod;
        cooked_lod.ratio = ratio;
        cooked_lod.simplification_error = simplification_error;
        cooked_lod.vertex_count = static_cast<std::uint32_t>(lod_vertex_count);
        cooked_lod.index_count = static_cast<std::uint32_t>(lod_indices.size());
        cooked_lod.encoded_vertices.resize(encoded_vertex_bound);
        cooked_lod.encoded_indices.resize(encoded_index_bound);
        const auto encoded_vertex_size = meshopt_encodeVertexBuffer(
            reinterpret_cast<unsigned char*>(cooked_lod.encoded_vertices.data()), encoded_vertex_bound,
            lod_vertices.data(), lod_vertex_count, input.vertex_stride);
        const auto encoded_index_size = meshopt_encodeIndexBuffer(
            reinterpret_cast<unsigned char*>(cooked_lod.encoded_indices.data()), encoded_index_bound,
            lod_indices.data(), lod_indices.size());
        if (encoded_vertex_size == 0U || encoded_index_size == 0U) {
            product.code = "asset.meshopt-encode-failed";
            product.detail = "meshoptimizer failed to encode a Cook LOD payload.";
            product.diagnostics.push_back(product.detail);
            return product;
        }
        cooked_lod.encoded_vertices.resize(encoded_vertex_size);
        cooked_lod.encoded_indices.resize(encoded_index_size);
        product.lods.push_back(std::move(cooked_lod));
    }

    product.payload = serialize_mesh_product(product);
    product.payload_fingerprint = "fnv1a64:" + hex_u64(fnv1a(
        std::span<const std::byte>(product.payload.data(), product.payload.size())));
    product.valid = true;
    product.code = "ok";
    product.detail = "Mesh optimized with meshoptimizer remap, cache, overdraw, fetch and LOD passes.";
    if (settings.quantize_attributes) {
        product.diagnostics.push_back(
            "quantizationRequested:typed attribute quantization is deferred; source bytes remain lossless");
    }
    product.diagnostics.push_back("meshbin:NMMSH001/little-endian");
    return product;
#else
    static_cast<void>(input);
    static_cast<void>(settings);
    return product;
#endif
}

std::string mesh_cook_product_json(const MeshCookProduct& product) {
    Json lods = Json::array();
    for (const auto& lod : product.lods) {
        lods.push_back({
            {"ratio", lod.ratio},
            {"simplificationError", lod.simplification_error},
            {"vertexCount", lod.vertex_count},
            {"indexCount", lod.index_count},
            {"encodedVertexBytes", lod.encoded_vertices.size()},
            {"encodedIndexBytes", lod.encoded_indices.size()}
        });
    }
    return Json{
        {"schema", product.schema},
        {"valid", product.valid},
        {"code", product.code},
        {"detail", product.detail},
        {"meshoptimizer", {
            {"available", product.meshoptimizer_available},
            {"pipeline", kMeshPipeline},
            {"quantizationApplied", product.quantization_applied}
        }},
        {"source", {
            {"assetId", product.source_asset_id},
            {"hash", product.source_hash},
            {"vertexCount", product.source_vertex_count},
            {"indexCount", product.source_index_count},
            {"vertexStride", product.vertex_stride}
        }},
        {"targetProfile", product.target_profile},
        {"cacheKey", product.cache_key},
        {"payload", {
            {"format", product.payload_format},
            {"bytes", product.payload.size()},
            {"fingerprint", product.payload_fingerprint}
        }},
        {"lods", std::move(lods)},
        {"diagnostics", product.diagnostics}
    }.dump();
}

std::string cook_artifact_json(const CookArtifactContract& artifact) {
    return artifact_json(artifact).dump();
}

} // namespace noemancer
