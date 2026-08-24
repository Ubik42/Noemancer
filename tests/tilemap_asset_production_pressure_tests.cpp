#include "engine/tilemap_asset.hpp"

#include <nlohmann/json.hpp>

#include <cstddef>
#include <cstdint>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>

namespace {

using Json = nlohmann::json;
using namespace noemancer;

constexpr std::size_t pressure_chunk_count = 2048U;
constexpr std::size_t pressure_columns = 64U;
constexpr std::int32_t target_chunk_x = 3;
constexpr std::int32_t target_chunk_y = 10;
constexpr std::int32_t target_cell_x = target_chunk_x * 16;
constexpr std::int32_t target_cell_y = target_chunk_y * 16;

TilePaletteDocument pressure_palette() {
    TilePaletteDocument palette;
    palette.asset_id = "palette.tilemap-pressure";
    palette.sprite_asset = "sprite.tiles.pressure";
    palette.tiles = {
        {.id = "ground", .frame_id = "ground.0", .collision = "solid", .tags = {"terrain"}},
        {.id = "platform", .frame_id = "platform.0", .collision = "one-way", .tags = {"terrain", "platform"}},
        {.id = "flower", .frame_id = "flower.0", .collision = "none", .tags = {"decor"}}};
    return palette;
}

TilemapDocument pressure_tilemap() {
    TilemapDocument map;
    map.asset_id = "tilemap.sparse-pressure";
    map.palette_asset = "palette.tilemap-pressure";
    map.cell_width = 0.5F;
    map.cell_height = 0.5F;
    map.chunk_width = 16U;
    map.chunk_height = 16U;
    TileLayer layer;
    layer.id = "ground";
    layer.sorting_layer = "terrain";
    layer.collision_enabled = true;
    layer.chunks.reserve(pressure_chunk_count);
    for (std::size_t index{}; index < pressure_chunk_count; ++index) {
        const auto chunk_x = static_cast<std::int32_t>(index % pressure_columns);
        const auto chunk_y = static_cast<std::int32_t>(index / pressure_columns);
        layer.chunks.push_back({chunk_x, chunk_y,
                                {{0U, 0U, "ground"}, {1U, 0U, "ground"}, {0U, 1U, "platform"}}});
    }
    map.layers.push_back(std::move(layer));
    return map;
}

bool has_error(const std::vector<TilemapAssetError>& errors, const std::string_view code) {
    for (const auto& issue : errors)
        if (issue.code == code) return true;
    return false;
}

bool require(const bool condition, const char* message, const int code) {
    if (condition) return true;
    std::cerr << message << '\n';
    static_cast<void>(code);
    return false;
}

} // namespace

int main() {
    const auto palette = pressure_palette();
    const auto map = pressure_tilemap();
    const auto stats = TilemapAssetCodec::production_stats(map);
    const auto expected_packed_bytes = pressure_chunk_count * TilemapProductionLimits::packed_chunk_metadata_bytes +
        pressure_chunk_count * 3U * TilemapProductionLimits::packed_cell_stride_bytes;
    if (!require(stats.valid && stats.code == "ok" && stats.layer_count == 1U &&
                     stats.chunk_count == pressure_chunk_count && stats.occupied_cell_count == pressure_chunk_count * 3U &&
                     stats.empty_chunk_count == 0U && stats.maximum_cells_in_chunk == 3U &&
                     stats.source_json_bytes > 0U && stats.estimated_packed_bytes == expected_packed_bytes,
                 "Sparse production statistics were not bounded or deterministic.", 1))
        return 1;
    const auto stats_json = Json::parse(TilemapAssetCodec::production_stats_json(stats), nullptr, false);
    if (!require(!stats_json.is_discarded() && stats_json.at("schemaVersion") == "noemancer.tilemap-production-stats/0.1" &&
                     stats_json.at("occupiedCellCount") == pressure_chunk_count * 3U &&
                     stats_json.at("estimatedPackedBytes") == expected_packed_bytes && stats_json.at("errors").empty(),
                 "Production statistics did not expose a structured metric contract.", 2))
        return 2;

    TilemapAssetLibrary library;
    if (!require(library.register_palette(palette) && library.register_tilemap(map),
                 "The sparse production fixture could not be registered.", 3))
        return 3;
    const auto* compiled = library.resolve_compiled(map.asset_id);
    if (!require(compiled != nullptr && compiled->production.valid &&
                     compiled->total_cell_count == pressure_chunk_count * 3U &&
                     compiled->chunks.size() == pressure_chunk_count &&
                     compiled->production.estimated_packed_bytes == expected_packed_bytes,
                 "The compiled sparse workload did not preserve bounded production metrics.", 4))
        return 4;

    std::uint64_t expected_first_cell{};
    for (const auto& chunk : compiled->chunks) {
        if (!require(chunk.gpu_range.first_cell == expected_first_cell &&
                         chunk.gpu_range.cell_count == chunk.cells.size() &&
                         chunk.gpu_range.byte_offset == chunk.gpu_range.first_cell *
                             TilemapProductionLimits::packed_cell_stride_bytes &&
                         chunk.gpu_range.byte_size == chunk.gpu_range.cell_count *
                             TilemapProductionLimits::packed_cell_stride_bytes &&
                         !chunk.stable_id.empty() && !chunk.content_fingerprint.empty(),
                     "Compiled chunks did not receive deterministic packed ranges.", 5))
            return 5;
        expected_first_cell += chunk.cells.size();
    }

    const TilemapChunkVisibilityQuery local_query{
        .layer_id = "ground", .minimum_cell_x = target_cell_x, .minimum_cell_y = target_cell_y,
        .maximum_cell_x = target_cell_x + 1, .maximum_cell_y = target_cell_y + 1,
        .maximum_chunk_count = 8U};
    const auto visible = library.visible_chunks(map.asset_id, local_query);
    const auto visible_again = library.visible_chunks(map.asset_id, local_query);
    if (!require(visible.success && visible.code == "ok" && visible.candidate_chunk_count == pressure_chunk_count &&
                     visible.visible_chunk_count == 1U && visible.visible_cell_count == 3U &&
                     visible.visible_packed_bytes == 3U * TilemapProductionLimits::packed_cell_stride_bytes &&
                     visible.chunks.size() == 1U && visible.chunks.front().chunk_x == target_chunk_x &&
                     visible.chunks.front().chunk_y == target_chunk_y && visible.chunks.front().cell_count == 3U &&
                     visible.chunks == visible_again.chunks &&
                     TilemapAssetLibrary::visibility_json(visible) == TilemapAssetLibrary::visibility_json(visible_again),
                 "Chunk visibility was not deterministic or quantitatively bounded.", 6))
        return 6;
    const auto visible_json = Json::parse(TilemapAssetLibrary::visibility_json(visible), nullptr, false);
    if (!require(!visible_json.is_discarded() && visible_json.at("visibleChunkCount") == 1U &&
                     visible_json.at("chunks").at(0).at("gpuRange").at("cellCount") == 3U,
                 "Chunk visibility did not expose a stable range in structured form.", 7))
        return 7;
    const auto over_budget = library.visible_chunks(map.asset_id,
        {.layer_id = "ground", .minimum_cell_x = -100000, .minimum_cell_y = -100000,
         .maximum_cell_x = 100000, .maximum_cell_y = 100000, .maximum_chunk_count = 1U});
    if (!require(!over_budget.success && over_budget.code == "tilemap.visibility-limit" &&
                     has_error(over_budget.errors, "tilemap.visibility-limit"),
                 "Chunk visibility did not reject an over-budget result.", 8))
        return 8;

    const auto bake = TilemapAssetCodec::bake_colliders(palette, map);
    if (!require(bake.success && bake.input_collision_cell_count == pressure_chunk_count * 3U &&
                     bake.merged_cell_count == pressure_chunk_count * 3U && bake.colliders.size() == pressure_chunk_count * 2U &&
                     bake.estimated_output_bytes == bake.colliders.size() * TilemapProductionLimits::packed_collider_record_bytes,
                 "Large sparse collision baking did not expose bounded merge metrics.", 9))
        return 9;
    const auto bake_json = Json::parse(TilemapAssetCodec::collider_bake_json(bake), nullptr, false);
    if (!require(!bake_json.is_discarded() && bake_json.at("inputCollisionCellCount") == pressure_chunk_count * 3U &&
                     bake_json.at("mergedCellCount") == pressure_chunk_count * 3U &&
                     bake_json.at("estimatedOutputBytes") == bake.estimated_output_bytes,
                 "Collider bake metrics were not available as structured data.", 10))
        return 10;

    const auto base_fingerprint = TilemapAssetCodec::tilemap_fingerprint(map);
    const auto old_revision = compiled->compilation_revision;
    const auto edit = TilemapAssetCodec::plan_stroke(palette, map, "ground",
        {{target_cell_x, target_cell_y, std::string("platform"), false, false}}, "pressure-test", base_fingerprint);
    if (!require(edit.valid && edit.changed_cell_count == 1U, "The sparse incremental edit plan was invalid.", 11))
        return 11;
    const auto preview = library.apply_stroke(map.asset_id, edit, true);
    if (!require(preview.success && preview.dry_run && preview.code == "tilemap.incremental-dry-run" &&
                     preview.changed_cell_count == 1U && preview.dirty_chunk_count == 1U &&
                     preview.rebuilt_chunk_count == 1U && preview.reused_chunk_count == pressure_chunk_count - 1U &&
                     preview.created_chunk_count == 0U && preview.removed_chunk_count == 0U &&
                     preview.uploaded_cell_count == 3U &&
                     preview.uploaded_bytes == 3U * TilemapProductionLimits::packed_cell_stride_bytes &&
                     preview.stable_gpu_range_reuse_count == pressure_chunk_count - 1U &&
                     preview.dirty_chunk_ids.size() == 1U && preview.resident_packed_bytes == expected_packed_bytes &&
                     library.resolve_compiled(map.asset_id)->compilation_revision == old_revision,
                 "Dry-run incremental update did not quantify a one-Chunk change or preserve the source.", 12))
        return 12;
    const auto preview_json = Json::parse(TilemapAssetLibrary::incremental_update_json(preview), nullptr, false);
    if (!require(!preview_json.is_discarded() && preview_json.at("dryRun") == true &&
                     preview_json.at("dirtyChunkCount") == 1U && preview_json.at("uploadedBytes") == preview.uploaded_bytes,
                 "Incremental update metrics were not exposed as structured data.", 13))
        return 13;

    const auto committed = library.apply_stroke(map.asset_id, edit, false);
    if (!require(committed.success && !committed.dry_run && committed.compilation_revision_after > old_revision &&
                     committed.reused_chunk_count == pressure_chunk_count - 1U && committed.rebuilt_chunk_count == 1U &&
                     library.resolve_compiled(map.asset_id)->total_cell_count == pressure_chunk_count * 3U,
                 "The committed incremental update did not preserve the sparse compiled workload.", 14))
        return 14;

    const auto after_edit = library.resolve(map.asset_id);
    if (!require(after_edit.has_value(), "The committed sparse tilemap could not be resolved.", 15)) return 15;
    const auto erase_fingerprint = TilemapAssetCodec::tilemap_fingerprint(after_edit->tilemap);
    const auto erase = TilemapAssetCodec::plan_stroke(palette, after_edit->tilemap, "ground",
        {{target_cell_x, target_cell_y, std::nullopt, false, false},
         {target_cell_x + 1, target_cell_y, std::nullopt, false, false},
         {target_cell_x, target_cell_y + 1, std::nullopt, false, false}},
        "pressure-test", erase_fingerprint);
    const auto removed = library.apply_stroke(map.asset_id, erase, false);
    if (!require(removed.success && removed.changed_cell_count == 3U && removed.removed_chunk_count == 1U &&
                     removed.created_chunk_count == 0U && removed.uploaded_cell_count == 0U &&
                     removed.dirty_chunk_count == 1U &&
                     library.resolve_compiled(map.asset_id)->chunks.size() == pressure_chunk_count - 1U,
                 "Incremental erasure did not quantify Chunk removal.", 16))
        return 16;

    Json too_many_layers{{"schema", "noemancer.tilemap/0.1"}, {"assetId", "too-many"},
                         {"paletteAsset", palette.asset_id}, {"cellSize", {1.0F, 1.0F}},
                         {"chunkSize", {16U, 16U}}, {"layers", Json::array()}};
    for (std::size_t index{}; index < TilemapProductionLimits::maximum_layers + 1U; ++index)
        too_many_layers["layers"].push_back({{"id", "layer." + std::to_string(index)}, {"sortingLayer", "default"},
                                               {"sortingOrder", 0}, {"collisionEnabled", false}, {"chunks", Json::array()}});
    const auto rejected = TilemapAssetCodec::parse_tilemap_json(too_many_layers.dump());
    if (!require(!rejected && has_error(rejected.errors, "tilemap.layer-limit"),
                 "The parser accepted an unbounded layer workload.", 17))
        return 17;
    auto oversized = map;
    oversized.layers.resize(TilemapProductionLimits::maximum_layers + 1U);
    if (!require(!TilemapAssetCodec::production_stats(oversized).valid &&
                     has_error(TilemapAssetCodec::production_stats(oversized).errors, "tilemap.layer-limit"),
                 "Production metrics accepted an unbounded in-memory workload.", 18))
        return 18;

    return 0;
}
