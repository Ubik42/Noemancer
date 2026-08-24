#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace noemancer {

inline constexpr std::string_view animation_clip_asset_schema = "noemancer.animation-clip/0.1";
inline constexpr std::size_t animation_clip_asset_max_source_bytes = 64U * 1024U;

struct AnimationClipAssetError final {
    std::string code;
    std::string path;
    std::string message;
};

struct AnimationClipAssetDocument final {
    std::string schema_version{std::string(animation_clip_asset_schema)};
    std::string asset_id;
    std::string source_asset;
    std::uint32_t skin_index{};
    std::uint32_t animation_index{};
    std::string compression{"ozz_runtime_baseline"};
};

struct AnimationClipAssetParseResult final {
    std::optional<AnimationClipAssetDocument> document;
    std::string code;
    std::string detail;
    std::vector<AnimationClipAssetError> errors;

    [[nodiscard]] explicit operator bool() const noexcept { return document.has_value(); }
};

class AnimationClipAssetCodec final {
public:
    [[nodiscard]] static AnimationClipAssetParseResult parse_json(std::string_view json);
    [[nodiscard]] static std::vector<AnimationClipAssetError> validate(
        const AnimationClipAssetDocument& document);
    [[nodiscard]] static std::string write_canonical_json(
        const AnimationClipAssetDocument& document);
    // Source assets are Cook-only build inputs. They must not become Runtime
    // Package dependencies or the source FBX/GLB would leak into distribution.
    [[nodiscard]] static std::vector<std::string> build_inputs(
        const AnimationClipAssetDocument& document);
};

} // namespace noemancer
