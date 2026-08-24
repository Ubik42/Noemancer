#pragma once

#include "engine/world.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace noemancer {

struct RenderCameraSnapshot final {
    std::string entity_id;
    std::array<float, 3> position{};
    std::array<float, 3> target{};
    float vertical_fov_degrees{45.0F};
    float near_clip{0.1F};
    float far_clip{100.0F};
    std::string projection{"perspective"};
    float orthographic_height{10.0F};
};

struct RenderDirectionalLightSnapshot final {
    std::string entity_id;
    std::array<float, 3> direction{-0.55F, -1.0F, -0.35F};
    std::array<float, 3> color{1.0F, 0.96F, 0.88F};
    float intensity{0.95F};
    float ambient_intensity{0.18F};
    bool casts_shadows{true};
};

struct RenderLocalLightSnapshot final {
    std::string entity_id;
    std::string kind{"point"};
    std::array<float, 3> position{};
    std::array<float, 3> direction{0.0F, -1.0F, 0.0F};
    std::array<float, 3> color{1.0F, 0.95F, 0.85F};
    float luminous_power_lumens{800.0F};
    float range_meters{8.0F};
    float inner_cone_degrees{25.0F};
    float outer_cone_degrees{35.0F};
    float source_radius_meters{0.05F};
    bool casts_shadows{};
};

struct RenderMaterialSnapshot final {
    std::array<float, 3> base_color{0.8F, 0.8F, 0.8F};
    float metallic{};
    float roughness{0.6F};
    std::string base_color_texture;
    std::array<float, 3> emissive_color{};
    float emissive_intensity{};
};

struct RenderInstanceSnapshot final {
    std::string entity_id;
    std::string mesh_asset;
    std::array<float, 3> position{};
    std::array<float, 3> scale{1.0F, 1.0F, 1.0F};
    std::array<float, 4> rotation{0.0F,0.0F,0.0F,1.0F};
    bool visible{true};
    bool casts_shadows{true};
    bool receives_shadows{true};
    bool material_override{};
    RenderMaterialSnapshot material;
    std::vector<std::array<float, 16>> skinning_matrices;
    bool vfx_particle{};
    std::uint64_t vfx_particle_id{};
    std::uint64_t vfx_emitter_id{};
    std::string vfx_graph_id;
};

struct RenderVfxParticleSnapshot final {
    std::uint64_t particle_id{};
    std::uint64_t emitter_id{};
    std::string graph_id;
    std::array<float, 3> position{};
    std::array<float, 3> velocity{};
    std::array<float, 4> color{};
    float age{};
    float lifetime{1.0F};
    float size{};
    std::string blend_mode{"additive"};
    std::string pixel_alignment{"profile"};
    std::string size_quantization{"profile"};
    std::string sampling{"profile"};
    float camera_distance_squared{};
    float size_start{0.15F};
    float size_end{};
    std::array<float, 4> color_start{1.0F,0.7F,0.2F,1.0F};
    std::array<float, 4> color_end{1.0F,0.1F,0.0F,0.0F};
    std::array<float, 3> gravity{0.0F,-9.81F,0.0F};
    float drag{0.1F};
    std::uint64_t spawn_seed{};
    std::uint32_t spawn_index{};
    std::array<float,3> spawn_origin{};
    float lifetime_min{0.5F};
    float lifetime_max{1.0F};
    float speed_min{1.0F};
    float speed_max{3.0F};
};

struct RenderSpriteSnapshot final {
    std::string entity_id;
    std::string sprite_asset;
    std::string clip_id;
    std::string frame_id;
    std::string texture_asset;
    std::array<std::uint32_t,2> texture_size{};
    std::array<std::uint32_t,4> pixel_rect{};
    std::array<float,4> uv_rect{};
    std::array<std::uint32_t,2> source_size{};
    std::array<std::uint32_t,2> trim_offset{};
    std::array<float,2> pivot{};
    float pixels_per_unit{100.0F};
    std::array<float,3> position{};
    std::array<float,3> scale{1.0F,1.0F,1.0F};
    std::array<float,4> rotation{0.0F,0.0F,0.0F,1.0F};
    bool flip_x{};
    bool flip_y{};
    bool visible{true};
    std::string sorting_layer{"default"};
    std::int32_t sorting_order{};
    std::string sampling{"nearest"};
    std::string alpha_mode{"cutout"};
    std::string normal_texture_asset;
    std::string emissive_mask_texture_asset;
    std::string depth_texture_asset;
    float normal_strength{1.0F};
    std::array<float,3> emissive_color{1.0F,1.0F,1.0F};
    float emissive_intensity{};
    float depth_bias{};
    std::string shading_model{"lit"};
    float metallic{};
    float roughness{0.8F};
    bool receives_shadows{true};
    bool casts_shadows{true};
};

struct RenderTilemapSnapshot final {
    std::string entity_id;
    std::string tilemap_asset;
    std::array<float,3> position{};
    std::array<float,3> scale{1.0F,1.0F,1.0F};
    std::array<float,4> rotation{0.0F,0.0F,0.0F,1.0F};
    bool visible{true};
    bool collision_enabled{true};
    bool resolved{};
    std::string palette_asset;
    std::size_t layer_count{};
    std::size_t chunk_count{};
    std::size_t cell_count{};
    std::size_t resolved_cell_count{};
    std::size_t compiled_chunk_count{};
    std::uint64_t compilation_revision{};
    std::size_t visible_chunk_count{};
    std::size_t culled_chunk_count{};
    std::size_t visible_cell_count{};
    bool cells_truncated{};
    bool early_visibility_applied{};
    std::size_t chunks_resolved{};
    std::size_t chunks_skipped_before_resolution{};
    std::size_t cells_skipped_before_resolution{};
};

struct RenderTileCellSnapshot final {
    std::string stable_id;
    std::string entity_id;
    std::string tilemap_asset;
    std::string layer_id;
    std::int32_t cell_x{};
    std::int32_t cell_y{};
    std::int32_t chunk_x{};
    std::int32_t chunk_y{};
    std::string chunk_content_fingerprint;
    std::string tile_id;
    std::string autotile_group;
    std::uint8_t autotile_mask{};
    std::string frame_id;
    std::string texture_asset;
    std::array<float,4> uv_rect{};
    std::array<float,4> local_rect{};
    std::array<float,3> position{};
    std::array<float,3> scale{1.0F,1.0F,1.0F};
    std::array<float,4> rotation{0.0F,0.0F,0.0F,1.0F};
    bool flip_x{};
    bool flip_y{};
    bool visible{true};
    std::string sorting_layer{"default"};
    std::int32_t sorting_order{};
    std::string sampling{"nearest"};
    std::string alpha_mode{"cutout"};
    std::string normal_texture_asset;
    std::string emissive_mask_texture_asset;
    std::string depth_texture_asset;
    float normal_strength{1.0F};
    std::array<float,3> emissive_color{1.0F,1.0F,1.0F};
    float emissive_intensity{};
    float depth_bias{};
    std::string shading_model{"lit"};
    float metallic{};
    float roughness{0.8F};
    bool receives_shadows{true};
    bool casts_shadows{true};
};

struct RenderTileChunkRangeSnapshot final {
    std::string key;
    std::string entity_id;
    std::string layer_id;
    std::int32_t chunk_x{};
    std::int32_t chunk_y{};
    std::string content_fingerprint;
    std::size_t first_cell{};
    std::size_t cell_count{};
};

struct RenderWorldSnapshot final {
    std::string schema_version{"noemancer.render-world.v15"};
    std::string extraction_id;
    std::uint64_t world_revision{};
    std::uint64_t frame_index{};
    std::optional<RenderCameraSnapshot> camera;
    std::optional<RenderDirectionalLightSnapshot> directional_light;
    std::vector<RenderLocalLightSnapshot> local_lights;
    std::vector<RenderInstanceSnapshot> instances;
    std::vector<RenderSpriteSnapshot> sprites;
    std::vector<RenderTilemapSnapshot> tilemaps;
    std::vector<RenderTileCellSnapshot> tile_cells;
    std::vector<RenderTileChunkRangeSnapshot> tile_chunk_ranges;
    std::vector<RenderVfxParticleSnapshot> vfx_particles;
    std::size_t vfx_particle_count{};
    std::size_t vfx_total_particle_count{};
    bool vfx_truncated{};
    std::string vfx_sort_policy{"blend-group/alpha-back-to-front/stable-id"};
    std::string sprite_sort_policy{"sorting-layer/sorting-order/stable-id"};
    bool tilemap_chunk_culling_applied{};
    std::size_t tilemap_visible_chunk_count{};
    std::size_t tilemap_culled_chunk_count{};
    std::size_t tilemap_cells_before_culling{};
    bool tilemap_early_visibility_applied{};
    std::size_t tilemap_chunks_resolved{};
    std::size_t tilemap_chunks_skipped_before_resolution{};
    std::size_t tilemap_cells_skipped_before_resolution{};
    bool tilemap_bake_cache_applied{};
    std::size_t tilemap_bake_cache_hits{};
    std::size_t tilemap_bake_cache_rebuilds{};
    std::size_t tilemap_bake_cache_evictions{};
    std::size_t tilemap_bake_cached_chunks{};
    std::size_t tilemap_bake_retained_offscreen_chunks{};
};

class TilemapRenderBakeCache final {
public:
    void begin_frame();
    [[nodiscard]] const std::vector<RenderTileCellSnapshot>* find(std::string_view key,std::string_view signature);
    void store(std::string key,std::string signature,std::vector<RenderTileCellSnapshot> cells);
    [[nodiscard]] std::size_t end_frame(bool retain_untouched);
    [[nodiscard]] std::size_t hits() const noexcept { return hits_; }
    [[nodiscard]] std::size_t rebuilds() const noexcept { return rebuilds_; }
    [[nodiscard]] std::size_t size() const noexcept { return entries_.size(); }
    [[nodiscard]] std::size_t retained_offscreen() const noexcept { return retained_offscreen_; }
private:
    struct Entry final {std::string signature;std::vector<RenderTileCellSnapshot> cells;std::uint64_t last_touched_frame{};};
    std::unordered_map<std::string,Entry> entries_;
    std::unordered_set<std::string> touched_;
    std::size_t hits_{};
    std::size_t rebuilds_{};
    std::size_t retained_offscreen_{};
    std::uint64_t frame_{};
};

class RenderWorldExtractor final {
public:
    [[nodiscard]] static RenderWorldSnapshot extract(
        std::uint64_t world_revision,
        std::uint64_t frame_index,
        std::vector<WorldEntityView> entities,
        std::span<const VfxRuntime::Particle> vfx_particles = {},
        TilemapRenderBakeCache* tilemap_bake_cache = nullptr);
    static void cull_tilemap_chunks(RenderWorldSnapshot& snapshot,std::uint32_t viewport_width,std::uint32_t viewport_height);
};

[[nodiscard]] std::string render_world_json(const RenderWorldSnapshot& snapshot);
[[nodiscard]] std::string tilemap_pressure_report_json(std::uint32_t chunk_columns,std::uint32_t chunk_rows,
    std::uint32_t chunk_size,std::uint32_t visible_chunk_radius,std::uint32_t occupied_cells_per_chunk=0);

} // namespace noemancer
