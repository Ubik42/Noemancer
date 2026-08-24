#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace noemancer {

inline constexpr std::string_view shader_artifact_manifest_schema =
    "noemancer.shader-artifact-manifest/0.1";

// These values intentionally describe the shader contract rather than an SDL
// enum.  The Runtime adapter can map the selected format to its GPU API.
enum class ShaderArtifactStage : std::uint8_t {
    vertex,
    fragment,
    compute,
    Vertex = vertex,
    Fragment = fragment,
    Compute = compute
};

enum class ShaderArtifactBackend : std::uint8_t {
    automatic,
    dxil,
    spv,
    Auto = automatic,
    DXIL = dxil,
    SPV = spv
};

[[nodiscard]] std::string_view shader_artifact_stage_name(ShaderArtifactStage stage) noexcept;
[[nodiscard]] std::optional<ShaderArtifactStage> shader_artifact_stage_from_string(
    std::string_view value) noexcept;
[[nodiscard]] std::string_view shader_artifact_backend_name(ShaderArtifactBackend backend) noexcept;
[[nodiscard]] std::optional<ShaderArtifactBackend> shader_artifact_backend_from_string(
    std::string_view value) noexcept;

// Resolve the Runtime's conventional shader directory without exposing a
// platform or SDL path type.  A sibling `../shaders` directory next to the
// executable wins; the build-time NOEMANCER_SHADER_DIR is the fallback.
[[nodiscard]] std::filesystem::path default_shader_artifact_root();

// Counts mirror the resource classes consumed by SDL_GPU shader creation.
// storage_buffers is the portable aggregate.  The read-only/read-write pair
// is retained when a compiler manifest exposes that distinction.
struct ShaderResourceContract final {
    std::uint32_t uniform_buffers{};
    std::uint32_t samplers{};
    std::uint32_t storage_buffers{};
    std::uint32_t storage_textures{};
    std::uint32_t read_only_storage_buffers{};
    std::uint32_t read_write_storage_buffers{};
};

struct ShaderArtifactRequest final {
    std::string stem;
    // A request may use the strongly typed enum or the canonical stage name
    // ("vertex", "fragment", or "compute").  Invalid strings fail at load.
    std::variant<ShaderArtifactStage, std::string> stage{ShaderArtifactStage::vertex};
    ShaderResourceContract resources{};
};

enum class ShaderArtifactErrorCode : std::uint8_t {
    ok,
    not_loaded,
    manifest_open_failed,
    manifest_too_large,
    manifest_read_failed,
    manifest_parse_failed,
    manifest_not_object,
    unknown_schema,
    missing_field,
    invalid_field,
    manifest_hash_mismatch,
    duplicate_stem,
    unsafe_path,
    request_invalid,
    shader_not_found,
    stage_mismatch,
    resource_mismatch,
    backend_invalid,
    backend_unsupported,
    artifact_open_failed,
    artifact_too_large,
    artifact_read_failed,
    artifact_size_mismatch,
    artifact_hash_mismatch
};

[[nodiscard]] std::string_view shader_artifact_error_code_name(
    ShaderArtifactErrorCode code) noexcept;

struct ShaderArtifactLoadResult final {
    bool success{};
    std::string code;
    std::string detail;

    std::vector<std::byte> bytes;
    std::uintmax_t artifact_bytes{};
    std::string stem;
    std::string stage;
    std::string format;
    std::string entrypoint;
    ShaderResourceContract resources{};

    // All identities use the engine's sha256:<64 lowercase hex> spelling.
    std::string manifest_hash;
    std::string source_hash;
    std::string artifact_hash;
};

// Runtime-private loader for a versioned shader artifact manifest.  The
// manifest is parsed once by the constructor.  Artifact bytes are read only
// by load(), after the request has passed the reflected contract checks.
class ShaderArtifactContract final {
public:
    static constexpr std::size_t default_max_manifest_bytes = 4U * 1024U * 1024U;
    static constexpr std::size_t default_max_artifact_bytes = 512U * 1024U * 1024U;

    ShaderArtifactContract();
    explicit ShaderArtifactContract(const std::filesystem::path& manifest_path);
    ShaderArtifactContract(const std::filesystem::path& manifest_path,
        ShaderArtifactBackend backend);
    ShaderArtifactContract(const std::filesystem::path& manifest_path,
        std::string_view backend);

    // Convenience spelling for callers that prefer a factory.  It still
    // performs all parsing in the returned object's construction.
    [[nodiscard]] static ShaderArtifactContract load_manifest(
        const std::filesystem::path& manifest_path,
        ShaderArtifactBackend backend = ShaderArtifactBackend::automatic);
    [[nodiscard]] static ShaderArtifactContract load_manifest(
        const std::filesystem::path& manifest_path,
        std::string_view backend);
    [[nodiscard]] static ShaderArtifactContract load(
        const std::filesystem::path& manifest_path,
        ShaderArtifactBackend backend = ShaderArtifactBackend::automatic);
    [[nodiscard]] static ShaderArtifactContract load(
        const std::filesystem::path& manifest_path,
        std::string_view backend);

    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] explicit operator bool() const noexcept { return valid(); }
    [[nodiscard]] ShaderArtifactErrorCode error_code_value() const noexcept;
    [[nodiscard]] std::string_view error_code() const noexcept;
    [[nodiscard]] std::string_view error_detail() const noexcept;
    [[nodiscard]] const std::filesystem::path& manifest_path() const noexcept;
    [[nodiscard]] ShaderArtifactBackend selected_backend() const noexcept;
    [[nodiscard]] std::string_view manifest_hash() const noexcept;
    [[nodiscard]] std::string_view source_hash() const noexcept;

    [[nodiscard]] ShaderArtifactLoadResult load(const ShaderArtifactRequest& request) const;
    [[nodiscard]] ShaderArtifactLoadResult load(
        const ShaderArtifactRequest& request, ShaderArtifactBackend backend) const;
    [[nodiscard]] ShaderArtifactLoadResult load(
        const ShaderArtifactRequest& request, std::string_view backend) const;

    // The definition remains Runtime-private in the implementation file; the
    // incomplete declaration here keeps nlohmann::json out of this boundary.
    struct Impl;

private:
    std::shared_ptr<const Impl> impl_;
};

} // namespace noemancer
