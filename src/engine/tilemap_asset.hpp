#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include <unordered_map>

namespace noemancer {

struct TilemapAssetError final {
    std::string code;
    std::string path;
    std::string message;
};

// Tilemaps are intentionally sparse, but every source and derived workload
// still needs a deterministic ceiling.  These limits are shared by parsing,
// collision baking and production observations so an Agent cannot accidentally
// turn a fill/import request into an unbounded allocation.
struct TilemapProductionLimits final {
    static constexpr std::size_t maximum_layers = 256U;
    static constexpr std::size_t maximum_chunks = 131072U;
    static constexpr std::size_t maximum_cells = 8388608U;
    static constexpr std::size_t maximum_cells_per_chunk = 16384U;
    static constexpr std::size_t maximum_visible_chunks = 8192U;
    static constexpr std::size_t maximum_colliders = 1048576U;

    // This is a renderer-neutral packed-cell budgeting stride, not a native
    // GPU handle or backend ABI.  The eventual renderer may choose a tighter
    // format while preserving the same source/Chunk identity contract.
    static constexpr std::size_t packed_cell_stride_bytes = 32U;
    static constexpr std::size_t packed_chunk_metadata_bytes = 128U;
    static constexpr std::size_t packed_collider_record_bytes = 64U;
};

struct TileAutotileVariant final {
    std::uint8_t neighbor_mask{};
    std::string frame_id;
};

struct TileDefinition final {
    std::string id;
    std::string frame_id;
    std::string collision{"none"};
    std::vector<std::string> tags;
    std::string autotile_group;
    std::vector<TileAutotileVariant> autotile_variants;
};

struct TilePaletteDocument final {
    std::string schema{"noemancer.tile-palette/0.1"};
    std::string asset_id;
    std::string sprite_asset;
    std::vector<TileDefinition> tiles;
};

struct TileCell final {
    std::uint16_t x{};
    std::uint16_t y{};
    std::string tile_id;
    bool flip_x{};
    bool flip_y{};
};

struct TileChunk final {
    std::int32_t x{};
    std::int32_t y{};
    std::vector<TileCell> cells;
};

struct TileLayer final {
    std::string id;
    std::string sorting_layer{"default"};
    std::int32_t sorting_order{};
    bool collision_enabled{};
    std::vector<TileChunk> chunks;
};

struct TilemapDocument final {
    std::string schema{"noemancer.tilemap/0.1"};
    std::string asset_id;
    std::string palette_asset;
    float cell_width{1.0F};
    float cell_height{1.0F};
    std::uint16_t chunk_width{32};
    std::uint16_t chunk_height{32};
    std::vector<TileLayer> layers;
};

struct TilePaletteParseResult final {
    std::optional<TilePaletteDocument> document;
    std::vector<TilemapAssetError> errors;
    [[nodiscard]] explicit operator bool() const noexcept { return document.has_value(); }
};

struct TilemapParseResult final {
    std::optional<TilemapDocument> document;
    std::vector<TilemapAssetError> errors;
    [[nodiscard]] explicit operator bool() const noexcept { return document.has_value(); }
};

struct TileColliderRect final {
    std::string layer_id;
    std::string collision;
    std::int32_t cell_x{};
    std::int32_t cell_y{};
    std::uint32_t cell_width{};
    std::uint32_t cell_height{};
    float center_x{};
    float center_y{};
    float width{};
    float height{};
};

struct TileColliderBakeResult final {
    bool success{};
    std::size_t input_collision_cell_count{};
    std::size_t merged_cell_count{};
    std::size_t estimated_output_bytes{};
    std::vector<TileColliderRect> colliders;
    std::vector<TilemapAssetError> errors;
};

struct TilemapProductionStats final {
    bool valid{};
    std::string code{"tilemap.production-invalid"};
    std::size_t layer_count{};
    std::size_t chunk_count{};
    std::size_t occupied_cell_count{};
    std::size_t empty_chunk_count{};
    std::size_t maximum_cells_in_chunk{};
    std::size_t source_json_bytes{};
    std::size_t estimated_packed_bytes{};
    std::vector<TilemapAssetError> errors;
};

struct TilemapCellEdit final {
    std::int32_t x{};
    std::int32_t y{};
    std::optional<std::string> tile_id;
    bool flip_x{};
    bool flip_y{};
};

struct TilemapStrokePlan final {
    bool valid{};
    std::string code;
    std::string plan_id;
    std::string manager;
    std::string layer_id;
    std::string base_fingerprint;
    std::string result_fingerprint;
    std::size_t requested_edit_count{};
    std::size_t changed_cell_count{};
    std::size_t painted_cell_count{};
    std::size_t erased_cell_count{};
    std::size_t created_chunk_count{};
    std::size_t removed_chunk_count{};
    std::vector<TilemapCellEdit> edits;
    std::optional<TilemapDocument> result;
    std::vector<TilemapAssetError> errors;
};

struct TilemapStrokeApplyResult final {
    bool success{};
    std::string code;
    std::string plan_id;
    std::string fingerprint_before;
    std::string fingerprint_after;
    std::size_t changed_cell_count{};
};

struct TilePaletteEditPlan final {
    bool valid{};
    std::string code;
    std::string plan_id;
    std::string manager;
    std::string tile_id;
    std::string base_fingerprint;
    std::string result_fingerprint;
    std::optional<TilePaletteDocument> result;
    std::vector<TilemapAssetError> errors;
};

struct TilemapGpuRange final {
    std::uint64_t first_cell{};
    std::uint64_t cell_count{};
    std::uint64_t byte_offset{};
    std::uint64_t byte_size{};
    [[nodiscard]] friend bool operator==(const TilemapGpuRange&, const TilemapGpuRange&) = default;
};

struct TilemapChunkVisibilityQuery final {
    std::string layer_id;
    std::int32_t minimum_cell_x{};
    std::int32_t minimum_cell_y{};
    std::int32_t maximum_cell_x{};
    std::int32_t maximum_cell_y{};
    std::size_t maximum_chunk_count{TilemapProductionLimits::maximum_visible_chunks};
};

struct TilemapVisibleChunk final {
    std::string stable_id;
    std::string content_fingerprint;
    std::string layer_id;
    std::int32_t chunk_x{};
    std::int32_t chunk_y{};
    std::size_t cell_count{};
    TilemapGpuRange gpu_range;
    [[nodiscard]] friend bool operator==(const TilemapVisibleChunk&, const TilemapVisibleChunk&) = default;
};

struct TilemapChunkVisibilityResult final {
    bool success{};
    std::string code{"tilemap.visibility-invalid"};
    std::string source_fingerprint;
    std::size_t candidate_chunk_count{};
    std::size_t visible_chunk_count{};
    std::size_t visible_cell_count{};
    std::size_t visible_packed_bytes{};
    std::vector<TilemapVisibleChunk> chunks;
    std::vector<TilemapAssetError> errors;
};

struct TilemapIncrementalUpdateResult final {
    bool success{};
    bool dry_run{};
    std::string code{"tilemap.incremental-invalid"};
    std::string asset_id;
    std::string source_fingerprint_before;
    std::string source_fingerprint_after;
    std::uint64_t compilation_revision_before{};
    std::uint64_t compilation_revision_after{};
    std::size_t changed_cell_count{};
    std::size_t dirty_chunk_count{};
    std::size_t rebuilt_chunk_count{};
    std::size_t reused_chunk_count{};
    std::size_t stable_gpu_range_reuse_count{};
    std::size_t created_chunk_count{};
    std::size_t removed_chunk_count{};
    std::size_t uploaded_cell_count{};
    std::size_t uploaded_bytes{};
    std::size_t resident_packed_bytes{};
    std::vector<std::string> dirty_chunk_ids;
    std::vector<TilemapAssetError> errors;
};

class TilemapAssetCodec final {
public:
    [[nodiscard]] static TilePaletteParseResult parse_palette_json(std::string_view json);
    [[nodiscard]] static TilemapParseResult parse_tilemap_json(std::string_view json);
    [[nodiscard]] static std::string write_palette_canonical_json(const TilePaletteDocument& document);
    [[nodiscard]] static std::string write_tilemap_canonical_json(const TilemapDocument& document);
    [[nodiscard]] static TilemapProductionStats production_stats(const TilemapDocument& document);
    [[nodiscard]] static std::string production_stats_json(const TilemapProductionStats& stats);
    [[nodiscard]] static TileColliderBakeResult bake_colliders(const TilePaletteDocument& palette,const TilemapDocument& tilemap);
    [[nodiscard]] static std::string collider_bake_json(const TileColliderBakeResult& bake);
    [[nodiscard]] static std::string tilemap_fingerprint(const TilemapDocument& document);
    [[nodiscard]] static std::string palette_fingerprint(const TilePaletteDocument& document);
    [[nodiscard]] static std::string_view resolve_autotile_frame(const TileDefinition& tile,std::uint8_t neighbor_mask) noexcept;
    [[nodiscard]] static TilemapStrokePlan plan_stroke(const TilePaletteDocument& palette,const TilemapDocument& document,
        std::string_view layer_id,std::vector<TilemapCellEdit> edits,std::string_view manager,std::string_view expected_fingerprint={});
    [[nodiscard]] static TilemapStrokePlan plan_rectangle(const TilePaletteDocument& palette,const TilemapDocument& document,
        std::string_view layer_id,std::int32_t first_x,std::int32_t first_y,std::int32_t second_x,std::int32_t second_y,
        std::optional<std::string> tile_id,bool flip_x,bool flip_y,std::string_view manager,std::string_view expected_fingerprint={});
    [[nodiscard]] static TilemapStrokePlan plan_flood_fill(const TilePaletteDocument& palette,const TilemapDocument& document,
        std::string_view layer_id,std::int32_t seed_x,std::int32_t seed_y,std::optional<std::string> tile_id,
        bool flip_x,bool flip_y,std::string_view manager,std::string_view expected_fingerprint={});
    [[nodiscard]] static TilemapStrokeApplyResult apply_stroke(TilemapDocument& document,const TilemapStrokePlan& plan,bool dry_run=false);
    [[nodiscard]] static std::string stroke_plan_json(const TilemapStrokePlan& plan);
    [[nodiscard]] static std::string stroke_apply_json(const TilemapStrokeApplyResult& result);
    [[nodiscard]] static TilePaletteEditPlan plan_autotile_update(const TilePaletteDocument& document,std::string_view tile_id,
        std::string autotile_group,std::vector<TileAutotileVariant> variants,std::string_view manager,
        std::string_view expected_fingerprint={});
    [[nodiscard]] static TilemapStrokeApplyResult apply_palette_edit(TilePaletteDocument& document,const TilePaletteEditPlan& plan,bool dry_run=false);
    [[nodiscard]] static std::string palette_edit_plan_json(const TilePaletteEditPlan& plan);
};

struct ResolvedTilemapAsset final {
    TilemapDocument tilemap;
    TilePaletteDocument palette;
};

struct CompiledTilemapCell final {
    std::int32_t cell_x{};
    std::int32_t cell_y{};
    std::string tile_id;
    std::string autotile_group;
    std::uint8_t autotile_mask{};
    std::string frame_id;
    bool flip_x{};
    bool flip_y{};
};

struct CompiledTilemapChunk final {
    std::string stable_id;
    std::string content_fingerprint;
    std::string layer_id;
    std::string sorting_layer{"default"};
    std::int32_t sorting_order{};
    std::int32_t chunk_x{};
    std::int32_t chunk_y{};
    std::int32_t minimum_cell_x{};
    std::int32_t minimum_cell_y{};
    std::int32_t maximum_cell_x{};
    std::int32_t maximum_cell_y{};
    TilemapGpuRange gpu_range;
    std::vector<CompiledTilemapCell> cells;
};

struct CompiledTilemapAsset final {
    ResolvedTilemapAsset source;
    std::string source_fingerprint;
    std::uint64_t compilation_revision{};
    std::size_t total_cell_count{};
    TilemapProductionStats production;
    std::vector<CompiledTilemapChunk> chunks;
};

class TilemapAssetLibrary final {
public:
    [[nodiscard]] bool register_palette(TilePaletteDocument document);
    [[nodiscard]] bool register_tilemap(TilemapDocument document);
    [[nodiscard]] std::optional<ResolvedTilemapAsset> resolve(std::string_view tilemap_asset) const;
    [[nodiscard]] const CompiledTilemapAsset* resolve_compiled(std::string_view tilemap_asset) const noexcept;
    [[nodiscard]] TilemapProductionStats production_stats(std::string_view tilemap_asset) const;
    [[nodiscard]] TilemapChunkVisibilityResult visible_chunks(
        std::string_view tilemap_asset, const TilemapChunkVisibilityQuery& query) const;
    [[nodiscard]] TilemapIncrementalUpdateResult apply_stroke(
        std::string_view tilemap_asset, const TilemapStrokePlan& plan, bool dry_run = false);
    [[nodiscard]] static std::string visibility_json(const TilemapChunkVisibilityResult& result);
    [[nodiscard]] static std::string incremental_update_json(const TilemapIncrementalUpdateResult& result);
private:
    void rebuild(std::string_view tilemap_asset);
    std::unordered_map<std::string,TilePaletteDocument> palettes_;
    std::unordered_map<std::string,TilemapDocument> tilemaps_;
    std::unordered_map<std::string,CompiledTilemapAsset> compiled_;
    std::uint64_t next_compilation_revision_{1};
};

} // namespace noemancer
