#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include <unordered_map>

namespace noemancer {

struct SpriteAssetError final {
    std::string code;
    std::string path;
    std::string message;
};

struct SpriteFrame final {
    std::string id;
    std::uint32_t x{};
    std::uint32_t y{};
    std::uint32_t width{};
    std::uint32_t height{};
    std::uint32_t trim_x{};
    std::uint32_t trim_y{};
    std::uint32_t source_width{};
    std::uint32_t source_height{};
    float pivot_x{0.5F};
    float pivot_y{0.5F};
    std::string collision_profile;
};

struct SpriteClipFrame final {
    std::string frame_id;
    std::uint32_t duration_ms{100};
    std::string event;
};

struct SpriteClip final {
    std::string id;
    bool looping{true};
    std::vector<SpriteClipFrame> frames;
};

struct SpriteProvenance final {
    std::string source_uri;
    std::string source_sha256;
    std::string generator;
    std::string license;
};

struct SpriteMaterialChannels final {
    std::string normal_texture_asset;
    std::string emissive_mask_texture_asset;
    std::string depth_texture_asset;
    float normal_strength{1.0F};
    float emissive_r{1.0F};
    float emissive_g{1.0F};
    float emissive_b{1.0F};
    float emissive_intensity{};
    float depth_bias{};
    std::string shading_model{"lit"};
    float metallic{};
    float roughness{0.8F};
    bool receives_shadows{true};
    bool casts_shadows{true};
};

struct SpriteAssetDocument final {
    std::string schema{"noemancer.sprite-asset/0.1"};
    std::string asset_id;
    std::string texture_asset;
    std::uint32_t texture_width{};
    std::uint32_t texture_height{};
    float pixels_per_unit{100.0F};
    std::string sampling{"nearest"};
    std::string alpha_mode{"cutout"};
    std::vector<SpriteFrame> frames;
    std::vector<SpriteClip> clips;
    std::optional<SpriteMaterialChannels> material;
    SpriteProvenance provenance;
};

struct SpriteAssetParseResult final {
    std::optional<SpriteAssetDocument> document;
    std::vector<SpriteAssetError> errors;
    [[nodiscard]] explicit operator bool() const noexcept { return document.has_value(); }
};

class SpriteAssetCodec final {
public:
    [[nodiscard]] static SpriteAssetParseResult parse_json(std::string_view json);
    [[nodiscard]] static std::vector<SpriteAssetError> validate(const SpriteAssetDocument& document);
    [[nodiscard]] static std::string write_canonical_json(const SpriteAssetDocument& document);
    [[nodiscard]] static std::vector<std::string> asset_dependencies(const SpriteAssetDocument& document);
};

struct SpritePlaybackState final {
    std::string asset_id;
    std::string clip_id;
    std::size_t frame_index{};
    double elapsed_in_frame_ms{};
    bool playing{true};
    std::uint64_t completed_loops{};
    std::string last_event;
};

struct SpritePlaybackResult final {
    bool success{};
    std::string code;
    std::string frame_id;
    std::string event;
    bool frame_changed{};
    bool looped{};
    std::size_t transitions{};
};

// Fully resolved, renderer-neutral view of one playback cursor. It owns only
// stable values so it can cross the ECS/render extraction boundary safely.
struct SpriteResolvedFrame final {
    std::string asset_id;
    std::string clip_id;
    std::string texture_asset;
    std::uint32_t texture_width{};
    std::uint32_t texture_height{};
    float pixels_per_unit{100.0F};
    std::string sampling{"nearest"};
    std::string alpha_mode{"cutout"};
    std::optional<SpriteMaterialChannels> material;
    SpriteFrame frame;
    std::uint32_t duration_ms{};
    std::string event;
};

class SpriteAssetLibrary final {
public:
    [[nodiscard]] bool register_asset(SpriteAssetDocument document);
    [[nodiscard]] const SpriteAssetDocument* find(std::string_view asset_id) const noexcept;
    [[nodiscard]] std::optional<SpriteResolvedFrame> resolve_frame(std::string_view asset_id,
                                                                    std::string_view frame_id) const;
    [[nodiscard]] std::optional<SpriteResolvedFrame> resolve(const SpritePlaybackState& state) const;
    [[nodiscard]] SpritePlaybackResult advance(SpritePlaybackState& state,double delta_seconds) const;
    [[nodiscard]] std::string observe_json(const SpritePlaybackState& state) const;
private:
    std::unordered_map<std::string,SpriteAssetDocument> assets_;
};

} // namespace noemancer
