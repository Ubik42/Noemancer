#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
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

// Sprite assets intentionally keep the existing single-texture schema. These
// limits bound authoring and ingestion work for long frame sequences and large
// clip sets without inventing a texture-array or multi-page persistence shape.
struct SpriteAssetValidationLimits final {
    std::size_t max_source_bytes{64U * 1024U * 1024U};
    std::size_t max_frames{65536U};
    std::size_t max_clips{4096U};
    std::size_t max_frames_per_clip{262144U};
    std::size_t max_total_clip_frame_references{1000000U};
};

// Renderer-neutral production evidence for a sprite atlas.  atlas_page_count
// is always one for the current schema because SpriteAssetDocument owns one
// textureAsset; the remaining fields quantify layout occupancy and reuse.
struct SpriteAssetProductionReport final {
    bool valid{};
    std::string code{"sprite.production-invalid"};
    std::size_t frame_count{};
    std::size_t clip_count{};
    std::size_t total_clip_frame_references{};
    std::size_t unique_referenced_frame_count{};
    std::size_t unreferenced_frame_count{};
    std::size_t max_clip_frame_count{};
    std::uint32_t atlas_page_count{1U};
    std::uint64_t atlas_area{};
    std::uint64_t frame_area_sum{};
    std::uint64_t occupied_area{};
    std::uint64_t free_area{};
    std::uint64_t overlap_area{};
    std::uint64_t layout_fingerprint{};
    std::vector<SpriteAssetError> diagnostics;
};

// Renderer-neutral planning limits for an in-memory multi-page atlas plan.
// SpriteAssetDocument deliberately remains the 0.2 single-texture persistence
// format; these limits only bound a production planning request.
struct SpriteAtlasPlanningLimits final {
    std::size_t max_frames{65536U};
    std::size_t max_pages{4096U};
    std::uint32_t max_page_width{8192U};
    std::uint32_t max_page_height{8192U};
    std::uint64_t max_estimated_cook_bytes{8ULL * 1024ULL * 1024ULL * 1024ULL};
    std::uint32_t estimated_bytes_per_pixel{4U};
};

struct SpriteAtlasPlanningOptions final {
    std::uint32_t page_width{1024U};
    std::uint32_t page_height{1024U};
    std::uint32_t padding{1U};
    SpriteAtlasPlanningLimits limits{};
};

struct SpriteAtlasFramePlacement final {
    std::string frame_id;
    std::uint32_t page_index{};
    std::uint32_t x{};
    std::uint32_t y{};
    std::uint32_t width{};
    std::uint32_t height{};
};

struct SpriteAtlasPageReport final {
    std::uint32_t page_index{};
    std::uint32_t width{};
    std::uint32_t height{};
    std::size_t frame_count{};
    std::uint64_t occupied_area{};
    std::uint64_t free_area{};
    std::uint64_t overlap_area{};
    // These are deterministic full-page planning estimates, not encoded file
    // sizes and not GPU upload or rendering measurements.
    std::uint64_t estimated_cook_pixels{};
    std::uint64_t estimated_cook_bytes{};
    std::uint64_t layout_fingerprint{};
};

struct SpriteAtlasPlanningReport final {
    bool valid{};
    std::string code{"sprite.atlas-plan-invalid"};
    std::uint32_t page_width{};
    std::uint32_t page_height{};
    std::uint32_t padding{};
    std::size_t frame_count{};
    std::size_t page_count{};
    std::size_t changed_frame_count{};
    std::uint64_t planned_cook_pixels{};
    std::uint64_t planned_cook_bytes{};
    std::uint64_t incremental_cook_pixels{};
    std::uint64_t incremental_cook_bytes{};
    std::uint64_t single_atlas_cook_pixels{};
    std::uint64_t single_atlas_cook_bytes{};
    std::uint64_t layout_fingerprint{};
    std::vector<std::uint32_t> affected_page_indices;
    std::vector<SpriteAtlasFramePlacement> placements;
    std::vector<SpriteAtlasPageReport> pages;
    std::vector<SpriteAssetError> diagnostics;
};

// JSON observation is deliberately bounded so a long animation cannot turn a
// single Agent read into an unbounded 65k-placement payload.  The projection
// reports total counts and truncation state alongside this stable prefix.
inline constexpr std::size_t sprite_atlas_plan_max_projected_placements{4096U};
inline constexpr std::size_t sprite_atlas_plan_max_projected_pages{4096U};

struct SpriteAssetParseResult final {
    std::optional<SpriteAssetDocument> document;
    std::vector<SpriteAssetError> errors;
    [[nodiscard]] explicit operator bool() const noexcept { return document.has_value(); }
};

class SpriteAssetCodec final {
public:
    [[nodiscard]] static SpriteAssetParseResult parse_json(std::string_view json);
    [[nodiscard]] static SpriteAssetParseResult parse_json(
        std::string_view json, const SpriteAssetValidationLimits& limits);
    [[nodiscard]] static std::vector<SpriteAssetError> validate(const SpriteAssetDocument& document);
    [[nodiscard]] static std::vector<SpriteAssetError> validate(
        const SpriteAssetDocument& document, const SpriteAssetValidationLimits& limits);
    [[nodiscard]] static SpriteAssetProductionReport production_report(
        const SpriteAssetDocument& document);
    [[nodiscard]] static SpriteAssetProductionReport production_report(
        const SpriteAssetDocument& document, const SpriteAssetValidationLimits& limits);
    [[nodiscard]] static SpriteAtlasPlanningReport plan_atlas_pages(
        const SpriteAssetDocument& document, const SpriteAtlasPlanningOptions& options);
    [[nodiscard]] static SpriteAtlasPlanningReport plan_atlas_pages(
        const SpriteAssetDocument& document, const SpriteAtlasPlanningOptions& options,
        const std::vector<std::string>& changed_frame_ids);
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

// Runtime-only page binding overlay.  Authoring SpriteAsset 0.2 JSON remains
// a single texture contract; this data selects a renderer-neutral derived page
// without rewriting the authored frame or persistence schema.
struct SpriteRuntimePageBinding final {
    std::string sprite_asset_id;
    std::string frame_id;
    std::string derived_texture_asset_id;
    std::uint32_t page_index{};
    std::uint32_t page_width{};
    std::uint32_t page_height{};
    std::uint32_t x{};
    std::uint32_t y{};
    std::uint32_t width{};
    std::uint32_t height{};
    std::uint64_t layout_fingerprint{};
    // The artifact's page identity is preserved verbatim (for example,
    // "sha256:<digest>") rather than being lossy-converted to an integer.
    std::string page_fingerprint;
};

struct SpritePageBindingUpdateResult final {
    bool success{};
    std::string code{"sprite.page-binding-invalid"};
    std::uint64_t revision{};
    std::size_t binding_count{};
    std::vector<SpriteAssetError> diagnostics;
};

inline constexpr std::size_t sprite_runtime_binding_max_entries{65536U};
inline constexpr std::uint32_t sprite_runtime_binding_max_page_dimension{16384U};
inline constexpr std::size_t sprite_runtime_binding_max_observation_entries{4096U};

class SpriteAssetLibrary final {
public:
    [[nodiscard]] bool register_asset(SpriteAssetDocument document);
    // Validates the complete batch before replacing the current overlay.  A
    // failure leaves both the prior bindings and their revision untouched.
    [[nodiscard]] SpritePageBindingUpdateResult replace_page_bindings(
        std::string_view asset_id, const std::vector<SpriteRuntimePageBinding>& bindings,
        std::optional<std::uint64_t> expected_revision = std::nullopt);
    [[nodiscard]] SpritePageBindingUpdateResult register_page_bindings(
        std::string_view asset_id, const std::vector<SpriteRuntimePageBinding>& bindings,
        std::optional<std::uint64_t> expected_revision = std::nullopt);
    [[nodiscard]] SpritePageBindingUpdateResult clear_page_bindings(
        std::string_view asset_id, std::optional<std::uint64_t> expected_revision = std::nullopt);
    [[nodiscard]] const SpriteAssetDocument* find(std::string_view asset_id) const noexcept;
    [[nodiscard]] std::optional<SpriteResolvedFrame> resolve_frame(std::string_view asset_id,
                                                                    std::string_view frame_id) const;
    [[nodiscard]] std::optional<SpriteResolvedFrame> resolve(const SpritePlaybackState& state) const;
    [[nodiscard]] SpritePlaybackResult advance(SpritePlaybackState& state,double delta_seconds) const;
    [[nodiscard]] std::string observe_json(const SpritePlaybackState& state) const;
    [[nodiscard]] std::string observe_page_bindings_json(std::string_view asset_id) const;
private:
    struct RuntimePageBindingState final {
        std::uint64_t revision{};
        std::unordered_map<std::string, SpriteRuntimePageBinding> bindings;
    };

    [[nodiscard]] const SpriteRuntimePageBinding* find_page_binding(
        std::string_view asset_id, std::string_view frame_id) const noexcept;

    std::unordered_map<std::string,SpriteAssetDocument> assets_;
    std::unordered_map<std::string,RuntimePageBindingState> page_bindings_;
};

// Deterministic synthetic long-sequence probe used by CLI/Agent acceptance.
// It measures the existing single-atlas contract and never claims GPU timing.
[[nodiscard]] std::string sprite_pressure_report_json(std::uint32_t frame_count,std::uint32_t clip_count,
    std::uint32_t frames_per_clip,std::uint32_t atlas_columns=64,std::uint32_t frame_edge=16,
    std::uint32_t planned_page_edge=1024,std::uint32_t planned_padding=1,
    std::uint32_t changed_frame_index=std::numeric_limits<std::uint32_t>::max());

// Serialize a bounded Agent-facing observation of an in-memory atlas plan.
// The output is a planning estimate only; it does not claim encoded sizes,
// GPU upload cost, or runtime rendering performance.
[[nodiscard]] std::string sprite_atlas_plan_json(
    const SpriteAssetDocument& document, const SpriteAtlasPlanningOptions& options,
    const std::vector<std::string>& changed_frame_ids);

} // namespace noemancer
