#pragma once

#include <cstdint>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace noemancer {

// This contract is deliberately independent of KTX-Software, Basis Universal
// and meshoptimizer headers.  Those libraries are implementation dependencies
// of the Cook adapters; their types must not leak into the engine's authoring,
// Agent or persisted-data boundaries.
enum class TextureSemantic {
    base_color,
    normal,
    metallic_roughness,
    occlusion,
    emissive,
    ui,
    hdr,
    data
};

enum class TextureAlphaMode {
    opaque,
    mask,
    blend
};

enum class MeshIndexFormat {
    automatic,
    uint16,
    uint32
};

struct CookPlatformProfile final {
    std::string id;
    std::string texture_color_target;
    std::string texture_normal_target;
    std::string texture_mask_target;
    std::string texture_hdr_target;
    std::string texture_fallback_target;
    std::string mesh_target;
    std::string meshopt_version;
    std::uint32_t texture_page_bytes{256U * 1024U};
    std::uint32_t mesh_page_bytes{256U * 1024U};
    bool generate_mipmaps{true};
    bool texture_streaming{true};
    bool mesh_streaming{true};
};

struct CookSource final {
    std::string asset_id;
    std::string source_uri;
    std::string source_hash;
    std::uintmax_t source_bytes{};
    std::string importer;
};

struct TextureCookSettings final {
    TextureSemantic semantic{TextureSemantic::base_color};
    TextureAlphaMode alpha_mode{TextureAlphaMode::opaque};
    bool srgb{true};
    bool generate_mipmaps{true};
    bool streaming{true};
    std::uint32_t max_dimension{};
    std::uint32_t quality{2U};
};

struct MeshCookSettings final {
    bool optimize_vertex_fetch{true};
    bool optimize_overdraw{true};
    bool simplify_lods{true};
    bool quantize_attributes{true};
    bool streaming{true};
    MeshIndexFormat index_format{MeshIndexFormat::automatic};
    std::vector<float> lod_ratios{1.0F, 0.5F, 0.25F};
};

// Engine-owned mesh input.  The bytes are an interleaved runtime vertex
// stream; position_offset points at a float3 used by overdraw and
// simplification.  No meshoptimizer or glTF types cross this boundary.
struct CookMeshInput final {
    std::vector<std::byte> vertices;
    std::uint32_t vertex_stride{};
    std::uint32_t position_offset{};
    std::vector<std::uint32_t> indices;
    std::string vertex_layout;
};

struct MeshCookLod final {
    float ratio{1.0F};
    float simplification_error{};
    std::uint32_t vertex_count{};
    std::uint32_t index_count{};
    std::vector<std::byte> encoded_vertices;
    std::vector<std::byte> encoded_indices;
};

struct MeshCookProduct final {
    bool valid{};
    bool meshoptimizer_available{};
    bool quantization_applied{};
    std::string code;
    std::string detail;
    std::string schema{"noemancer.mesh-artifact/0.1"};
    std::string source_asset_id;
    std::string source_hash;
    std::string target_profile;
    std::string cache_key;
    std::string payload_format{"meshopt/meshbin"};
    std::string payload_fingerprint;
    std::uint32_t source_vertex_count{};
    std::uint32_t source_index_count{};
    std::uint32_t vertex_stride{};
    std::vector<MeshCookLod> lods;
    std::vector<std::byte> payload;
    std::vector<std::string> diagnostics;
};

// A planned artifact is a stable, serializable description of the work that
// a concrete KTX2/BasisU adapter will perform.  It is not a claim that texture
// compression has already happened.  Mesh Cook has a separate executable
// adapter below; both adapters must preserve the key and emit payload/metadata
// under artifact_uri.
struct CookArtifactContract final {
    bool valid{};
    std::string code;
    std::string detail;
    std::string schema{"noemancer.cook-artifact/0.1"};
    std::string artifact_kind;
    std::string source_asset_id;
    std::string source_uri;
    std::string source_hash;
    std::string target_profile;
    std::string pipeline;
    std::string payload_format;
    std::string payload_target;
    std::string color_space;
    std::string alpha_mode;
    std::string index_format;
    std::string cache_key;
    std::string artifact_uri;
    std::string payload_uri;
    std::string stream_policy;
    std::string settings_fingerprint;
    std::uintmax_t source_bytes{};
    std::uint32_t page_bytes{};
    std::vector<float> lod_ratios;
    std::vector<std::string> dependencies;
    std::vector<std::string> diagnostics;
};

// Known target profiles are intentionally explicit.  A typo must fail during
// Cook planning instead of silently selecting a lowest-common-denominator
// format that changes runtime quality or memory use.
[[nodiscard]] CookPlatformProfile cook_platform_profile(std::string_view profile_id);
[[nodiscard]] bool validate_cook_platform_profile(const CookPlatformProfile& profile,
    std::string& code, std::string& detail);
[[nodiscard]] std::string cook_platform_profile_fingerprint(const CookPlatformProfile& profile);

[[nodiscard]] CookArtifactContract plan_texture_cook(
    const CookSource& source,
    const CookPlatformProfile& profile,
    const TextureCookSettings& settings = {});

[[nodiscard]] CookArtifactContract plan_mesh_cook(
    const CookSource& source,
    const CookPlatformProfile& profile,
    const MeshCookSettings& settings = {});

[[nodiscard]] bool meshoptimizer_available() noexcept;

[[nodiscard]] MeshCookProduct execute_mesh_cook(
    const CookSource& source,
    const CookMeshInput& input,
    const CookPlatformProfile& profile,
    const MeshCookSettings& settings = {});

[[nodiscard]] std::string mesh_cook_product_json(const MeshCookProduct& product);

// JSON is an observation/plan representation only.  The public C++ contract
// above stays plain data so no JSON or third-party types enter domain models.
[[nodiscard]] std::string cook_artifact_json(const CookArtifactContract& artifact);

[[nodiscard]] std::string texture_semantic_name(TextureSemantic semantic);
[[nodiscard]] std::string texture_alpha_mode_name(TextureAlphaMode mode);
[[nodiscard]] std::string mesh_index_format_name(MeshIndexFormat format);

} // namespace noemancer
