#include "engine/tilemap_asset.hpp"

#include <algorithm>
#include <cmath>
#include <deque>
#include <limits>
#include <map>
#include <nlohmann/json.hpp>
#include <iomanip>
#include <set>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

namespace noemancer {
namespace {
using Json=nlohmann::json;

void error(std::vector<TilemapAssetError>& errors,std::string code,std::string path,std::string message) {
    errors.push_back({std::move(code),std::move(path),std::move(message)});
}

bool fields(const Json& value,std::initializer_list<std::string_view> allowed,const std::string& path,
            std::vector<TilemapAssetError>& errors) {
    if(!value.is_object()){error(errors,"tilemap.invalid-object",path,"Expected an object.");return false;}
    bool valid=true;
    for(const auto& [name,unused]:value.items()) {
        static_cast<void>(unused);
        if(std::ranges::none_of(allowed,[&](const auto candidate){return candidate==name;})) {
            error(errors,"tilemap.unknown-field",path+"/"+name,"Unknown field.");valid=false;
        }
    }
    return valid;
}

bool text(const Json& value,const char* name,const std::string& path,std::string& output,
          std::vector<TilemapAssetError>& errors) {
    if(!value.contains(name)||!value.at(name).is_string()||(output=value.at(name).get<std::string>()).empty()) {
        error(errors,"tilemap.invalid-string",path+"/"+name,"Expected a non-empty string.");return false;
    }
    return true;
}

Json palette_json(const TilePaletteDocument& document) {
    Json tiles=Json::array();
    for(const auto& tile:document.tiles) {
        Json value={{"id",tile.id},{"frame",tile.frame_id},{"collision",tile.collision},{"tags",tile.tags}};
        if(!tile.autotile_group.empty()) {Json variants=Json::array();for(const auto& variant:tile.autotile_variants)
            variants.push_back({{"mask",variant.neighbor_mask},{"frame",variant.frame_id}});
            value["autotile"]={{"group",tile.autotile_group},{"variants",std::move(variants)}};}
        tiles.push_back(std::move(value));
    }
    return {{"schema",document.schema},{"assetId",document.asset_id},{"spriteAsset",document.sprite_asset},{"tiles",std::move(tiles)}};
}

Json tilemap_json(const TilemapDocument& document) {
    Json layers=Json::array();
    for(const auto& layer:document.layers) {
        Json chunks=Json::array();
        for(const auto& chunk:layer.chunks) {
            Json cells=Json::array();
            for(const auto& cell:chunk.cells)cells.push_back(Json::array({cell.x,cell.y,cell.tile_id,cell.flip_x,cell.flip_y}));
            chunks.push_back({{"position",{chunk.x,chunk.y}},{"cells",std::move(cells)}});
        }
        layers.push_back({{"id",layer.id},{"sortingLayer",layer.sorting_layer},{"sortingOrder",layer.sorting_order},
                          {"collisionEnabled",layer.collision_enabled},{"chunks",std::move(chunks)}});
    }
    return {{"schema",document.schema},{"assetId",document.asset_id},{"paletteAsset",document.palette_asset},
            {"cellSize",{document.cell_width,document.cell_height}},{"chunkSize",{document.chunk_width,document.chunk_height}},
            {"layers",std::move(layers)}};
}

std::string text_fingerprint(const std::string_view source) {
    std::uint64_t hash=1469598103934665603ULL;
    for(const unsigned char value:source){hash^=value;hash*=1099511628211ULL;}
    std::ostringstream output;output<<"fnv1a64:"<<std::hex<<std::setfill('0')<<std::setw(16)<<hash;return output.str();
}

std::string stroke_plan_id(const TilemapStrokePlan& plan) {
    const auto digest=text_fingerprint(plan.manager+"\n"+plan.layer_id+"\n"+plan.base_fingerprint+"\n"+
        plan.result_fingerprint+"\n"+std::to_string(plan.changed_cell_count));
    return "tilemap-stroke-"+digest.substr(digest.find(':')+1);
}

std::int32_t floor_divide(const std::int32_t value,const std::int32_t divisor) {
    auto quotient=value/divisor;const auto remainder=value%divisor;if(remainder<0)--quotient;return quotient;
}

} // namespace

TilePaletteParseResult TilemapAssetCodec::parse_palette_json(const std::string_view source) {
    TilePaletteParseResult result;const auto input=Json::parse(source,nullptr,false);
    if(input.is_discarded()||!input.is_object()){error(result.errors,"tilemap.invalid-json","/","Tile palette must be a JSON object.");return result;}
    fields(input,{"schema","assetId","spriteAsset","tiles"},"",result.errors);
    TilePaletteDocument document;text(input,"schema","",document.schema,result.errors);text(input,"assetId","",document.asset_id,result.errors);
    text(input,"spriteAsset","",document.sprite_asset,result.errors);
    const auto supports_autotile=document.schema=="noemancer.tile-palette/0.2";
    if(document.schema!="noemancer.tile-palette/0.1"&&!supports_autotile)
        error(result.errors,"tilemap.unsupported-schema","/schema","Expected noemancer.tile-palette/0.1 or noemancer.tile-palette/0.2.");
    std::unordered_set<std::string> ids;
    if(!input.contains("tiles")||!input.at("tiles").is_array()||input.at("tiles").empty())error(result.errors,"tilemap.invalid-tiles","/tiles","tiles must be a non-empty array.");
    else for(std::size_t index=0;index<input.at("tiles").size();++index) {
        const auto path="/tiles/"+std::to_string(index);const auto& value=input.at("tiles").at(index);
        if(!fields(value,{"id","frame","collision","tags","autotile"},path,result.errors))continue;
        TileDefinition tile;text(value,"id",path,tile.id,result.errors);text(value,"frame",path,tile.frame_id,result.errors);
        text(value,"collision",path,tile.collision,result.errors);
        if(!tile.id.empty()&&!ids.insert(tile.id).second)error(result.errors,"tilemap.duplicate-tile",path+"/id","Tile IDs must be unique.");
        if(tile.collision!="none"&&tile.collision!="solid"&&tile.collision!="one-way")
            error(result.errors,"tilemap.invalid-collision",path+"/collision","collision must be none, solid, or one-way.");
        if(!value.contains("tags")||!value.at("tags").is_array())error(result.errors,"tilemap.invalid-tags",path+"/tags","tags must be an array.");
        else {
            std::unordered_set<std::string> tags;
            for(std::size_t tag_index=0;tag_index<value.at("tags").size();++tag_index) {
                const auto& tag=value.at("tags").at(tag_index);
                if(!tag.is_string()||tag.get<std::string>().empty())error(result.errors,"tilemap.invalid-tag",path+"/tags/"+std::to_string(tag_index),"Tag must be a non-empty string.");
                else if(!tags.insert(tag.get<std::string>()).second)error(result.errors,"tilemap.duplicate-tag",path+"/tags/"+std::to_string(tag_index),"Tags must be unique.");
                else tile.tags.push_back(tag.get<std::string>());
            }
        }
        if(value.contains("autotile")) {
            if(!supports_autotile)error(result.errors,"tilemap.autotile-schema",path+"/autotile","Autotile rules require noemancer.tile-palette/0.2.");
            const auto& autotile=value.at("autotile");if(fields(autotile,{"group","variants"},path+"/autotile",result.errors)) {
                text(autotile,"group",path+"/autotile",tile.autotile_group,result.errors);std::set<std::uint8_t> masks;
                if(!autotile.contains("variants")||!autotile.at("variants").is_array()||autotile.at("variants").empty()||autotile.at("variants").size()>16)
                    error(result.errors,"tilemap.invalid-autotile-variants",path+"/autotile/variants","variants must contain 1 to 16 mask/frame entries.");
                else for(std::size_t variant_index=0;variant_index<autotile.at("variants").size();++variant_index) {
                    const auto variant_path=path+"/autotile/variants/"+std::to_string(variant_index);const auto& variant=autotile.at("variants").at(variant_index);
                    if(!fields(variant,{"mask","frame"},variant_path,result.errors))continue;TileAutotileVariant parsed;
                    if(!variant.contains("mask")||!variant.at("mask").is_number_unsigned()||variant.at("mask").get<std::uint64_t>()>15)
                        error(result.errors,"tilemap.invalid-autotile-mask",variant_path+"/mask","mask must be an unsigned four-neighbor bit mask in [0,15].");
                    else {parsed.neighbor_mask=variant.at("mask").get<std::uint8_t>();if(!masks.insert(parsed.neighbor_mask).second)
                        error(result.errors,"tilemap.duplicate-autotile-mask",variant_path+"/mask","Each autotile mask must be unique.");}
                    text(variant,"frame",variant_path,parsed.frame_id,result.errors);tile.autotile_variants.push_back(std::move(parsed));
                }
                std::ranges::sort(tile.autotile_variants,{},&TileAutotileVariant::neighbor_mask);
            }
        }
        std::ranges::sort(tile.tags);document.tiles.push_back(std::move(tile));
    }
    if(result.errors.empty())result.document=std::move(document);return result;
}

TilemapParseResult TilemapAssetCodec::parse_tilemap_json(const std::string_view source) {
    TilemapParseResult result;const auto input=Json::parse(source,nullptr,false);
    if(input.is_discarded()||!input.is_object()){error(result.errors,"tilemap.invalid-json","/","Tilemap must be a JSON object.");return result;}
    fields(input,{"schema","assetId","paletteAsset","cellSize","chunkSize","layers"},"",result.errors);
    TilemapDocument document;text(input,"schema","",document.schema,result.errors);text(input,"assetId","",document.asset_id,result.errors);
    text(input,"paletteAsset","",document.palette_asset,result.errors);
    if(document.schema!="noemancer.tilemap/0.1")error(result.errors,"tilemap.unsupported-schema","/schema","Expected noemancer.tilemap/0.1.");
    if(input.contains("cellSize")&&input.at("cellSize").is_array()&&input.at("cellSize").size()==2&&
       input.at("cellSize")[0].is_number()&&input.at("cellSize")[1].is_number()) {
        document.cell_width=input.at("cellSize")[0].get<float>();document.cell_height=input.at("cellSize")[1].get<float>();
    } else error(result.errors,"tilemap.invalid-cell-size","/cellSize","cellSize must contain two numbers.");
    if(!std::isfinite(document.cell_width)||!std::isfinite(document.cell_height)||document.cell_width<=0||document.cell_height<=0||
       document.cell_width>1024||document.cell_height>1024)error(result.errors,"tilemap.cell-size-range","/cellSize","Cell size must be finite in (0,1024].");
    if(input.contains("chunkSize")&&input.at("chunkSize").is_array()&&input.at("chunkSize").size()==2&&
       input.at("chunkSize")[0].is_number_unsigned()&&input.at("chunkSize")[1].is_number_unsigned()) {
        const auto width=input.at("chunkSize")[0].get<std::uint64_t>(),height=input.at("chunkSize")[1].get<std::uint64_t>();
        if(width>=4&&width<=128&&height>=4&&height<=128){document.chunk_width=static_cast<std::uint16_t>(width);document.chunk_height=static_cast<std::uint16_t>(height);}
        else error(result.errors,"tilemap.chunk-size-range","/chunkSize","Chunk dimensions must be in [4,128].");
    } else error(result.errors,"tilemap.invalid-chunk-size","/chunkSize","chunkSize must contain two unsigned integers.");
    std::unordered_set<std::string> layer_ids;
    std::size_t total_chunk_count{};
    std::size_t total_cell_count{};
    if(!input.contains("layers")||!input.at("layers").is_array()||input.at("layers").empty())error(result.errors,"tilemap.invalid-layers","/layers","layers must be a non-empty array.");
    else if(input.at("layers").size()>TilemapProductionLimits::maximum_layers) {
        error(result.errors,"tilemap.layer-limit","/layers","Tilemap layer count exceeds the production limit.");
    } else for(std::size_t layer_index=0;layer_index<input.at("layers").size();++layer_index) {
        const auto path="/layers/"+std::to_string(layer_index);const auto& value=input.at("layers").at(layer_index);
        if(!fields(value,{"id","sortingLayer","sortingOrder","collisionEnabled","chunks"},path,result.errors))continue;
        TileLayer layer;text(value,"id",path,layer.id,result.errors);text(value,"sortingLayer",path,layer.sorting_layer,result.errors);
        if(!layer.id.empty()&&!layer_ids.insert(layer.id).second)error(result.errors,"tilemap.duplicate-layer",path+"/id","Layer IDs must be unique.");
        if(value.contains("sortingOrder")&&value.at("sortingOrder").is_number_integer())layer.sorting_order=value.at("sortingOrder").get<std::int32_t>();
        else error(result.errors,"tilemap.invalid-sorting-order",path+"/sortingOrder","sortingOrder must be a signed integer.");
        if(value.contains("collisionEnabled")&&value.at("collisionEnabled").is_boolean())layer.collision_enabled=value.at("collisionEnabled").get<bool>();
        else error(result.errors,"tilemap.invalid-collision-enabled",path+"/collisionEnabled","collisionEnabled must be boolean.");
        std::set<std::pair<std::int32_t,std::int32_t>> chunk_positions;
        if(!value.contains("chunks")||!value.at("chunks").is_array())error(result.errors,"tilemap.invalid-chunks",path+"/chunks","chunks must be an array.");
        else if(total_chunk_count+value.at("chunks").size()>TilemapProductionLimits::maximum_chunks) {
            error(result.errors,"tilemap.chunk-limit",path+"/chunks","Tilemap chunk count exceeds the production limit.");
        } else for(std::size_t chunk_index=0;chunk_index<value.at("chunks").size();++chunk_index) {
            const auto chunk_path=path+"/chunks/"+std::to_string(chunk_index);const auto& item=value.at("chunks").at(chunk_index);
            if(!fields(item,{"position","cells"},chunk_path,result.errors))continue;
            TileChunk chunk;
            if(item.contains("position")&&item.at("position").is_array()&&item.at("position").size()==2&&
               item.at("position")[0].is_number_integer()&&item.at("position")[1].is_number_integer()) {
                chunk.x=item.at("position")[0].get<std::int32_t>();chunk.y=item.at("position")[1].get<std::int32_t>();
                if(!chunk_positions.emplace(chunk.x,chunk.y).second)error(result.errors,"tilemap.duplicate-chunk",chunk_path+"/position","Chunk positions must be unique per layer.");
            } else error(result.errors,"tilemap.invalid-chunk-position",chunk_path+"/position","position must contain two signed integers.");
            std::set<std::pair<std::uint16_t,std::uint16_t>> occupied;
            if(!item.contains("cells")||!item.at("cells").is_array())error(result.errors,"tilemap.invalid-cells",chunk_path+"/cells","cells must be an array.");
            else if(item.at("cells").size()>TilemapProductionLimits::maximum_cells_per_chunk) {
                error(result.errors,"tilemap.cell-limit","/cells","Cells in a chunk exceed the production limit.");
            } else if(total_cell_count+item.at("cells").size()>TilemapProductionLimits::maximum_cells) {
                error(result.errors,"tilemap.cell-limit","/layers","Tilemap cell count exceeds the production limit.");
            }
            if(!item.contains("cells")||!item.at("cells").is_array()||
               item.at("cells").size()>TilemapProductionLimits::maximum_cells_per_chunk||
               total_cell_count+item.at("cells").size()>TilemapProductionLimits::maximum_cells) {
                if(item.contains("cells")&&item.at("cells").is_array()) {
                    // The bounded parser rejects this source before entering a
                    // potentially unbounded cell loop.
                    return result;
                }
            }
            else for(std::size_t cell_index=0;cell_index<item.at("cells").size();++cell_index) {
                const auto cell_path=chunk_path+"/cells/"+std::to_string(cell_index);const auto& cell_value=item.at("cells").at(cell_index);
                if(!cell_value.is_array()||(cell_value.size()!=3&&cell_value.size()!=5)||!cell_value[0].is_number_unsigned()||
                   !cell_value[1].is_number_unsigned()||!cell_value[2].is_string()||cell_value[2].get<std::string>().empty()) {
                    error(result.errors,"tilemap.invalid-cell",cell_path,"Cell must be [x,y,tileId] or [x,y,tileId,flipX,flipY].");continue;
                }
                const auto x=cell_value[0].get<std::uint64_t>(),y=cell_value[1].get<std::uint64_t>();
                if(x>=document.chunk_width||y>=document.chunk_height){error(result.errors,"tilemap.cell-out-of-chunk",cell_path,"Local cell coordinates exceed chunkSize.");continue;}
                if(cell_value.size()==5&&(!cell_value[3].is_boolean()||!cell_value[4].is_boolean())){error(result.errors,"tilemap.invalid-cell-flip",cell_path,"Flip fields must be boolean.");continue;}
                TileCell cell{static_cast<std::uint16_t>(x),static_cast<std::uint16_t>(y),cell_value[2].get<std::string>(),
                    cell_value.size()==5?cell_value[3].get<bool>():false,cell_value.size()==5?cell_value[4].get<bool>():false};
                if(!occupied.emplace(cell.x,cell.y).second)error(result.errors,"tilemap.duplicate-cell",cell_path,"A chunk may define each local cell once.");
                chunk.cells.push_back(std::move(cell));
            }
            total_cell_count+=item.at("cells").size();
            ++total_chunk_count;
            std::ranges::sort(chunk.cells,{},[](const TileCell& cell){return std::pair{cell.y,cell.x};});layer.chunks.push_back(std::move(chunk));
        }
        std::ranges::sort(layer.chunks,{},[](const TileChunk& chunk){return std::pair{chunk.y,chunk.x};});document.layers.push_back(std::move(layer));
    }
    if(result.errors.empty())result.document=std::move(document);return result;
}

std::string TilemapAssetCodec::write_palette_canonical_json(const TilePaletteDocument& document){return palette_json(document).dump(2);}
std::string TilemapAssetCodec::write_tilemap_canonical_json(const TilemapDocument& document){return tilemap_json(document).dump(2);}

TilemapProductionStats TilemapAssetCodec::production_stats(const TilemapDocument& document) {
    TilemapProductionStats result;
    result.layer_count=document.layers.size();
    if(result.layer_count>TilemapProductionLimits::maximum_layers)
        error(result.errors,"tilemap.layer-limit","/layers","Tilemap layer count exceeds the production limit.");
    for(std::size_t layer_index{};layer_index<document.layers.size();++layer_index) {
        const auto& layer=document.layers[layer_index];
        if(result.chunk_count+layer.chunks.size()>TilemapProductionLimits::maximum_chunks)
            error(result.errors,"tilemap.chunk-limit","/layers/"+std::to_string(layer_index)+"/chunks",
                  "Tilemap chunk count exceeds the production limit.");
        for(std::size_t chunk_index{};chunk_index<layer.chunks.size();++chunk_index) {
            const auto& chunk=layer.chunks[chunk_index];
            ++result.chunk_count;
            result.maximum_cells_in_chunk=std::max(result.maximum_cells_in_chunk,chunk.cells.size());
            if(chunk.cells.size()>TilemapProductionLimits::maximum_cells_per_chunk)
                error(result.errors,"tilemap.cell-limit","/layers/"+std::to_string(layer_index)+"/chunks/"+
                          std::to_string(chunk_index)+"/cells","Cells in a chunk exceed the production limit.");
            if(chunk.cells.empty())++result.empty_chunk_count;
            if(result.occupied_cell_count+chunk.cells.size()>TilemapProductionLimits::maximum_cells)
                error(result.errors,"tilemap.cell-limit","/layers/"+std::to_string(layer_index)+"/chunks/"+
                          std::to_string(chunk_index)+"/cells","Tilemap cell count exceeds the production limit.");
            result.occupied_cell_count+=chunk.cells.size();
            for(const auto& cell:chunk.cells)
                if(cell.x>=document.chunk_width||cell.y>=document.chunk_height)
                    error(result.errors,"tilemap.cell-out-of-chunk","/layers/"+std::to_string(layer_index)+"/chunks/"+
                              std::to_string(chunk_index)+"/cells","Cell coordinates exceed chunkSize.");
        }
    }
    result.source_json_bytes=write_tilemap_canonical_json(document).size();
    result.estimated_packed_bytes=result.chunk_count*TilemapProductionLimits::packed_chunk_metadata_bytes+
        result.occupied_cell_count*TilemapProductionLimits::packed_cell_stride_bytes;
    result.valid=result.errors.empty();
    result.code=result.valid?"ok":result.errors.front().code;
    return result;
}

std::string TilemapAssetCodec::production_stats_json(const TilemapProductionStats& stats) {
    Json errors=Json::array();
    for(const auto& issue:stats.errors)
        errors.push_back({{"code",issue.code},{"path",issue.path},{"message",issue.message}});
    return Json{{"schemaVersion","noemancer.tilemap-production-stats/0.1"},{"valid",stats.valid},{"code",stats.code},
        {"layerCount",stats.layer_count},{"chunkCount",stats.chunk_count},{"occupiedCellCount",stats.occupied_cell_count},
        {"emptyChunkCount",stats.empty_chunk_count},{"maximumCellsInChunk",stats.maximum_cells_in_chunk},
        {"sourceJsonBytes",stats.source_json_bytes},{"estimatedPackedBytes",stats.estimated_packed_bytes},
        {"errors",std::move(errors)}}.dump();
}

std::string TilemapAssetCodec::tilemap_fingerprint(const TilemapDocument& document) {
    return text_fingerprint(write_tilemap_canonical_json(document));
}

std::string TilemapAssetCodec::palette_fingerprint(const TilePaletteDocument& document) {
    return text_fingerprint(write_palette_canonical_json(document));
}

std::string_view TilemapAssetCodec::resolve_autotile_frame(const TileDefinition& tile,const std::uint8_t neighbor_mask) noexcept {
    if(!tile.autotile_group.empty())if(const auto variant=std::ranges::find(tile.autotile_variants,neighbor_mask,&TileAutotileVariant::neighbor_mask);
        variant!=tile.autotile_variants.end())return variant->frame_id;
    return tile.frame_id;
}

TilemapStrokePlan TilemapAssetCodec::plan_stroke(const TilePaletteDocument& palette,const TilemapDocument& document,
    const std::string_view layer_id,std::vector<TilemapCellEdit> edits,const std::string_view manager,
    const std::string_view expected_fingerprint) {
    TilemapStrokePlan plan{.code="tilemap.stroke-invalid",.manager=std::string(manager),.layer_id=std::string(layer_id),
        .base_fingerprint=tilemap_fingerprint(document),.requested_edit_count=edits.size(),.edits=std::move(edits)};
    if(plan.manager.empty())error(plan.errors,"tilemap.invalid-manager","/manager","manager must be non-empty.");
    if(plan.layer_id.empty())error(plan.errors,"tilemap.invalid-layer","/layerId","layerId must be non-empty.");
    if(!expected_fingerprint.empty()&&expected_fingerprint!=plan.base_fingerprint) {
        error(plan.errors,"tilemap.stroke-conflict","/expectedFingerprint","Tilemap changed since the caller observed it.");
        plan.code="tilemap.stroke-conflict";return plan;
    }
    if(palette.asset_id!=document.palette_asset)error(plan.errors,"tilemap.palette-mismatch","/paletteAsset","Tilemap does not reference the supplied palette.");
    if(plan.edits.empty()||plan.edits.size()>4096)error(plan.errors,"tilemap.stroke-size","/edits","A stroke must contain 1 to 4096 edits.");
    std::unordered_set<std::string> tile_ids;for(const auto& tile:palette.tiles)tile_ids.insert(tile.id);
    std::set<std::pair<std::int32_t,std::int32_t>> positions;
    for(std::size_t index=0;index<plan.edits.size();++index) {
        const auto& edit=plan.edits[index];const auto path="/edits/"+std::to_string(index);
        if(std::abs(static_cast<std::int64_t>(edit.x))>10000000||std::abs(static_cast<std::int64_t>(edit.y))>10000000)
            error(plan.errors,"tilemap.stroke-coordinate-range",path,"World cell coordinates must be within +/-10,000,000.");
        if(!positions.emplace(edit.x,edit.y).second)error(plan.errors,"tilemap.stroke-duplicate-cell",path,"A stroke may edit each cell once.");
        if(edit.tile_id&&!tile_ids.contains(*edit.tile_id))error(plan.errors,"tilemap.unknown-tile",path+"/tileId","Stroke references an unknown Tile ID.");
    }
    auto result=document;const auto layer=std::ranges::find(result.layers,plan.layer_id,&TileLayer::id);
    if(layer==result.layers.end())error(plan.errors,"tilemap.layer-not-found","/layerId","Target layer does not exist.");
    if(!plan.errors.empty())return plan;
    const auto chunk_width=static_cast<std::int32_t>(result.chunk_width),chunk_height=static_cast<std::int32_t>(result.chunk_height);
    for(const auto& edit:plan.edits) {
        const auto chunk_x=floor_divide(edit.x,chunk_width),chunk_y=floor_divide(edit.y,chunk_height);
        const auto local_x=static_cast<std::uint16_t>(edit.x-chunk_x*chunk_width);
        const auto local_y=static_cast<std::uint16_t>(edit.y-chunk_y*chunk_height);
        auto chunk=std::ranges::find_if(layer->chunks,[&](const TileChunk& value){return value.x==chunk_x&&value.y==chunk_y;});
        if(chunk==layer->chunks.end()&&edit.tile_id) {
            layer->chunks.push_back({chunk_x,chunk_y,{}});chunk=std::prev(layer->chunks.end());++plan.created_chunk_count;
        }
        if(chunk==layer->chunks.end())continue;
        auto cell=std::ranges::find_if(chunk->cells,[&](const TileCell& value){return value.x==local_x&&value.y==local_y;});
        if(!edit.tile_id) {
            if(cell!=chunk->cells.end()){chunk->cells.erase(cell);++plan.changed_cell_count;++plan.erased_cell_count;}
        } else if(cell==chunk->cells.end()) {
            chunk->cells.push_back({local_x,local_y,*edit.tile_id,edit.flip_x,edit.flip_y});
            ++plan.changed_cell_count;++plan.painted_cell_count;
        } else if(cell->tile_id!=*edit.tile_id||cell->flip_x!=edit.flip_x||cell->flip_y!=edit.flip_y) {
            *cell={local_x,local_y,*edit.tile_id,edit.flip_x,edit.flip_y};++plan.changed_cell_count;++plan.painted_cell_count;
        }
    }
    const auto chunks_before=layer->chunks.size();
    std::erase_if(layer->chunks,[](const TileChunk& chunk){return chunk.cells.empty();});
    plan.removed_chunk_count=chunks_before-layer->chunks.size();
    for(auto& chunk:layer->chunks)std::ranges::sort(chunk.cells,{},[](const TileCell& cell){return std::pair{cell.y,cell.x};});
    std::ranges::sort(layer->chunks,{},[](const TileChunk& chunk){return std::pair{chunk.y,chunk.x};});
    const auto validated=parse_tilemap_json(write_tilemap_canonical_json(result));
    if(!validated){plan.errors=validated.errors;return plan;}
    plan.result=std::move(result);plan.result_fingerprint=tilemap_fingerprint(*plan.result);
    plan.valid=plan.changed_cell_count>0;plan.code=plan.valid?"ok":"tilemap.stroke-no-op";
    plan.plan_id=stroke_plan_id(plan);return plan;
}

TilemapStrokePlan TilemapAssetCodec::plan_rectangle(const TilePaletteDocument& palette,const TilemapDocument& document,
    const std::string_view layer_id,const std::int32_t first_x,const std::int32_t first_y,const std::int32_t second_x,const std::int32_t second_y,
    std::optional<std::string> tile_id,const bool flip_x,const bool flip_y,const std::string_view manager,const std::string_view expected_fingerprint) {
    if(!expected_fingerprint.empty()&&expected_fingerprint!=tilemap_fingerprint(document))
        return plan_stroke(palette,document,layer_id,{{first_x,first_y,tile_id,flip_x,flip_y}},manager,expected_fingerprint);
    const auto minimum_x=std::min(first_x,second_x),maximum_x=std::max(first_x,second_x);
    const auto minimum_y=std::min(first_y,second_y),maximum_y=std::max(first_y,second_y);
    const auto width=static_cast<std::uint64_t>(static_cast<std::int64_t>(maximum_x)-minimum_x+1);
    const auto height=static_cast<std::uint64_t>(static_cast<std::int64_t>(maximum_y)-minimum_y+1);
    if(width*height>4096U) {
        TilemapStrokePlan plan{.code="tilemap.region-size",.manager=std::string(manager),.layer_id=std::string(layer_id),
            .base_fingerprint=tilemap_fingerprint(document),.requested_edit_count=static_cast<std::size_t>(width*height)};
        error(plan.errors,"tilemap.region-size","/rectangle","A rectangle may cover at most 4096 cells.");return plan;
    }
    std::vector<TilemapCellEdit> edits;edits.reserve(static_cast<std::size_t>(width*height));
    for(auto y=minimum_y;;++y) {for(auto x=minimum_x;;++x) {
        edits.push_back({x,y,tile_id,flip_x,flip_y});if(x==maximum_x)break;
    }if(y==maximum_y)break;}
    return plan_stroke(palette,document,layer_id,std::move(edits),manager,expected_fingerprint);
}

TilemapStrokePlan TilemapAssetCodec::plan_flood_fill(const TilePaletteDocument& palette,const TilemapDocument& document,
    const std::string_view layer_id,const std::int32_t seed_x,const std::int32_t seed_y,std::optional<std::string> tile_id,
    const bool flip_x,const bool flip_y,const std::string_view manager,const std::string_view expected_fingerprint) {
    if(!expected_fingerprint.empty()&&expected_fingerprint!=tilemap_fingerprint(document))
        return plan_stroke(palette,document,layer_id,{{seed_x,seed_y,tile_id,flip_x,flip_y}},manager,expected_fingerprint);
    const auto layer=std::ranges::find(document.layers,layer_id,&TileLayer::id);
    if(layer==document.layers.end())return plan_stroke(palette,document,layer_id,{{seed_x,seed_y,tile_id,flip_x,flip_y}},manager,expected_fingerprint);
    std::map<std::pair<std::int32_t,std::int32_t>,std::string> occupied;
    for(const auto& chunk:layer->chunks)for(const auto& cell:chunk.cells) {
        const auto global_x=static_cast<std::int64_t>(chunk.x)*document.chunk_width+cell.x;
        const auto global_y=static_cast<std::int64_t>(chunk.y)*document.chunk_height+cell.y;
        if(global_x>=std::numeric_limits<std::int32_t>::min()&&global_x<=std::numeric_limits<std::int32_t>::max()&&
           global_y>=std::numeric_limits<std::int32_t>::min()&&global_y<=std::numeric_limits<std::int32_t>::max())
            occupied.emplace(std::pair{static_cast<std::int32_t>(global_x),static_cast<std::int32_t>(global_y)},cell.tile_id);
    }
    const auto seed=occupied.find({seed_x,seed_y});
    if(seed==occupied.end()) {
        TilemapStrokePlan plan{.code="tilemap.fill-unbounded-empty",.manager=std::string(manager),.layer_id=std::string(layer_id),
            .base_fingerprint=tilemap_fingerprint(document)};
        error(plan.errors,"tilemap.fill-unbounded-empty","/seed","Sparse Tilemaps have no finite empty boundary; flood fill must start on an occupied cell.");return plan;
    }
    const auto target_id=seed->second;std::deque<std::pair<std::int32_t,std::int32_t>> frontier{{seed_x,seed_y}};
    std::set<std::pair<std::int32_t,std::int32_t>> visited;std::vector<TilemapCellEdit> edits;
    while(!frontier.empty()) {
        const auto position=frontier.front();frontier.pop_front();if(visited.contains(position))continue;
        const auto cell=occupied.find(position);if(cell==occupied.end()||cell->second!=target_id)continue;visited.insert(position);
        edits.push_back({position.first,position.second,tile_id,flip_x,flip_y});
        if(edits.size()>4096U) {
            TilemapStrokePlan plan{.code="tilemap.region-size",.manager=std::string(manager),.layer_id=std::string(layer_id),
                .base_fingerprint=tilemap_fingerprint(document),.requested_edit_count=edits.size()};
            error(plan.errors,"tilemap.region-size","/seed","The connected flood region exceeds the 4096-cell transaction limit.");return plan;
        }
        if(position.first<std::numeric_limits<std::int32_t>::max())frontier.push_back({position.first+1,position.second});
        if(position.first>std::numeric_limits<std::int32_t>::min())frontier.push_back({position.first-1,position.second});
        if(position.second<std::numeric_limits<std::int32_t>::max())frontier.push_back({position.first,position.second+1});
        if(position.second>std::numeric_limits<std::int32_t>::min())frontier.push_back({position.first,position.second-1});
    }
    std::ranges::sort(edits,{},[](const TilemapCellEdit& edit){return std::pair{edit.y,edit.x};});
    return plan_stroke(palette,document,layer_id,std::move(edits),manager,expected_fingerprint);
}

TilemapStrokeApplyResult TilemapAssetCodec::apply_stroke(TilemapDocument& document,const TilemapStrokePlan& plan,const bool dry_run) {
    TilemapStrokeApplyResult result{.code="tilemap.stroke-invalid",.plan_id=plan.plan_id,
        .fingerprint_before=tilemap_fingerprint(document),.changed_cell_count=plan.changed_cell_count};
    if(!plan.valid||!plan.result){result.code=plan.code;return result;}
    if(plan.plan_id!=stroke_plan_id(plan)){result.code="tilemap.stroke-integrity-error";return result;}
    if(result.fingerprint_before!=plan.base_fingerprint){result.code="tilemap.stroke-conflict";return result;}
    if(tilemap_fingerprint(*plan.result)!=plan.result_fingerprint){result.code="tilemap.stroke-integrity-error";return result;}
    result.success=true;result.code=dry_run?"tilemap.stroke-dry-run":"ok";result.fingerprint_after=plan.result_fingerprint;
    if(!dry_run)document=*plan.result;return result;
}

std::string TilemapAssetCodec::stroke_plan_json(const TilemapStrokePlan& plan) {
    Json edits=Json::array();for(const auto& edit:plan.edits)edits.push_back({{"cell",{edit.x,edit.y}},
        {"operation",edit.tile_id?"paint":"erase"},{"tileId",edit.tile_id?Json(*edit.tile_id):Json(nullptr)},
        {"flipX",edit.flip_x},{"flipY",edit.flip_y}});
    Json errors=Json::array();for(const auto& issue:plan.errors)errors.push_back({{"code",issue.code},{"path",issue.path},{"message",issue.message}});
    return Json{{"schemaVersion","noemancer.tilemap-stroke-plan/0.1"},{"valid",plan.valid},{"code",plan.code},{"planId",plan.plan_id},
        {"manager",plan.manager},{"layerId",plan.layer_id},{"baseFingerprint",plan.base_fingerprint},{"resultFingerprint",plan.result_fingerprint},
        {"requestedEditCount",plan.requested_edit_count},{"changedCellCount",plan.changed_cell_count},{"paintedCellCount",plan.painted_cell_count},
        {"erasedCellCount",plan.erased_cell_count},{"createdChunkCount",plan.created_chunk_count},{"removedChunkCount",plan.removed_chunk_count},
        {"edits",std::move(edits)},{"errors",std::move(errors)}}.dump();
}

std::string TilemapAssetCodec::stroke_apply_json(const TilemapStrokeApplyResult& result) {
    return Json{{"schemaVersion","noemancer.tilemap-stroke-receipt/0.1"},{"success",result.success},{"code",result.code},
        {"planId",result.plan_id},{"fingerprintBefore",result.fingerprint_before},{"fingerprintAfter",result.fingerprint_after},
        {"changedCellCount",result.changed_cell_count}}.dump();
}

TilePaletteEditPlan TilemapAssetCodec::plan_autotile_update(const TilePaletteDocument& document,const std::string_view tile_id,
    std::string autotile_group,std::vector<TileAutotileVariant> variants,const std::string_view manager,
    const std::string_view expected_fingerprint) {
    TilePaletteEditPlan plan{.code="tilemap.palette-edit-invalid",.manager=std::string(manager),.tile_id=std::string(tile_id),
        .base_fingerprint=palette_fingerprint(document)};
    if(manager.empty()){error(plan.errors,"tilemap.manager-required","/manager","A stable manager is required.");return plan;}
    if(!expected_fingerprint.empty()&&expected_fingerprint!=plan.base_fingerprint) {
        plan.code="tilemap.palette-edit-conflict";error(plan.errors,plan.code,"/expectedFingerprint","Palette source changed after preview.");return plan;
    }
    auto result=document;auto tile=std::ranges::find(result.tiles,tile_id,&TileDefinition::id);
    if(tile==result.tiles.end()){plan.code="tilemap.unknown-tile";error(plan.errors,plan.code,"/tileId","Palette has no Tile with this ID.");return plan;}
    if(autotile_group.empty()&&!variants.empty()) {error(plan.errors,"tilemap.autotile-group-required","/autotileGroup","Variants require a non-empty group.");return plan;}
    tile->autotile_group=std::move(autotile_group);tile->autotile_variants=std::move(variants);
    if(!tile->autotile_group.empty())result.schema="noemancer.tile-palette/0.2";
    const auto parsed=parse_palette_json(write_palette_canonical_json(result));
    if(!parsed){plan.errors=parsed.errors;return plan;}
    plan.result=*parsed.document;plan.result_fingerprint=palette_fingerprint(*plan.result);
    if(plan.result_fingerprint==plan.base_fingerprint){plan.code="tilemap.palette-edit-no-op";return plan;}
    plan.plan_id="tilemap.palette-plan."+text_fingerprint(plan.manager+"\n"+plan.tile_id+"\n"+plan.base_fingerprint+"\n"+plan.result_fingerprint);
    plan.valid=true;plan.code="ok";return plan;
}

TilemapStrokeApplyResult TilemapAssetCodec::apply_palette_edit(TilePaletteDocument& document,const TilePaletteEditPlan& plan,const bool dry_run) {
    TilemapStrokeApplyResult result{.plan_id=plan.plan_id,.fingerprint_before=palette_fingerprint(document),.changed_cell_count=plan.valid?1U:0U};
    if(!plan.valid||!plan.result){result.code=plan.code;return result;}
    const auto expected_id="tilemap.palette-plan."+text_fingerprint(plan.manager+"\n"+plan.tile_id+"\n"+plan.base_fingerprint+"\n"+plan.result_fingerprint);
    if(plan.plan_id!=expected_id||palette_fingerprint(*plan.result)!=plan.result_fingerprint){result.code="tilemap.palette-edit-integrity-error";return result;}
    if(result.fingerprint_before!=plan.base_fingerprint){result.code="tilemap.palette-edit-conflict";return result;}
    result.success=true;result.code=dry_run?"tilemap.palette-edit-dry-run":"ok";result.fingerprint_after=plan.result_fingerprint;
    if(!dry_run)document=*plan.result;return result;
}

std::string TilemapAssetCodec::palette_edit_plan_json(const TilePaletteEditPlan& plan) {
    Json variants=Json::array();if(plan.result) {const auto tile=std::ranges::find(plan.result->tiles,plan.tile_id,&TileDefinition::id);
        if(tile!=plan.result->tiles.end())for(const auto& variant:tile->autotile_variants)variants.push_back({{"mask",variant.neighbor_mask},{"frame",variant.frame_id}});}
    Json errors=Json::array();for(const auto& issue:plan.errors)errors.push_back({{"code",issue.code},{"path",issue.path},{"message",issue.message}});
    return Json{{"schemaVersion","noemancer.tile-palette-edit-plan/0.1"},{"valid",plan.valid},{"code",plan.code},{"planId",plan.plan_id},
        {"manager",plan.manager},{"tileId",plan.tile_id},{"baseFingerprint",plan.base_fingerprint},{"resultFingerprint",plan.result_fingerprint},
        {"variants",std::move(variants)},{"errors",std::move(errors)}}.dump();
}

TileColliderBakeResult TilemapAssetCodec::bake_colliders(const TilePaletteDocument& palette,const TilemapDocument& tilemap) {
    TileColliderBakeResult result;
    if(palette.asset_id!=tilemap.palette_asset){error(result.errors,"tilemap.palette-mismatch","/paletteAsset","Tilemap does not reference the supplied palette.");return result;}
    const auto production=production_stats(tilemap);
    if(!production.valid){result.errors=production.errors;return result;}
    std::unordered_map<std::string,std::string> collision;
    for(const auto& tile:palette.tiles)collision.emplace(tile.id,tile.collision);
    for(const auto& layer:tilemap.layers) {
        if(!layer.collision_enabled)continue;
        std::map<std::pair<std::int32_t,std::int32_t>,std::string> cells;
        for(const auto& chunk:layer.chunks)for(const auto& cell:chunk.cells) {
            const auto found=collision.find(cell.tile_id);
            if(found==collision.end()){error(result.errors,"tilemap.unknown-tile","/layers/"+layer.id,"Cell references unknown tile "+cell.tile_id+".");continue;}
            if(found->second!="none") {
                const auto global_x=static_cast<std::int64_t>(chunk.x)*tilemap.chunk_width+cell.x;
                const auto global_y=static_cast<std::int64_t>(chunk.y)*tilemap.chunk_height+cell.y;
                if(global_x<std::numeric_limits<std::int32_t>::min()||global_x>std::numeric_limits<std::int32_t>::max()||
                   global_y<std::numeric_limits<std::int32_t>::min()||global_y>std::numeric_limits<std::int32_t>::max()) {
                    error(result.errors,"tilemap.cell-coordinate-range","/layers/"+layer.id,
                          "Cell world coordinates exceed the signed runtime range.");continue;
                }
                cells.emplace(std::pair{static_cast<std::int32_t>(global_x),static_cast<std::int32_t>(global_y)},found->second);
                ++result.input_collision_cell_count;
            }
        }
        std::set<std::pair<std::int32_t,std::int32_t>> consumed;
        for(const auto& [position,kind]:cells) {
            if(consumed.contains(position))continue;
            const auto [origin_x,origin_y]=position;std::uint32_t width=1,height=1;
            while(cells.contains({origin_x+static_cast<std::int32_t>(width),origin_y})&&cells.at({origin_x+static_cast<std::int32_t>(width),origin_y})==kind)++width;
            if(kind=="solid") {
                bool extend=true;
                while(extend) {
                    const auto y=origin_y+static_cast<std::int32_t>(height);
                    for(std::uint32_t x=0;x<width;++x)if(!cells.contains({origin_x+static_cast<std::int32_t>(x),y})||cells.at({origin_x+static_cast<std::int32_t>(x),y})!=kind){extend=false;break;}
                    if(extend)++height;
                }
            }
            for(std::uint32_t y=0;y<height;++y)for(std::uint32_t x=0;x<width;++x)consumed.emplace(origin_x+static_cast<std::int32_t>(x),origin_y+static_cast<std::int32_t>(y));
            result.merged_cell_count+=static_cast<std::size_t>(width)*height;
            if(result.colliders.size()>=TilemapProductionLimits::maximum_colliders) {
                error(result.errors,"tilemap.collider-limit","/colliders","Collider count exceeds the production limit.");
                return result;
            }
            const float world_width=width*tilemap.cell_width,world_height=height*tilemap.cell_height;
            result.colliders.push_back({layer.id,kind,origin_x,origin_y,width,height,
                (static_cast<float>(origin_x)+width*0.5F)*tilemap.cell_width,
                (static_cast<float>(origin_y)+height*0.5F)*tilemap.cell_height,world_width,world_height});
        }
    }
    result.estimated_output_bytes=result.colliders.size()*TilemapProductionLimits::packed_collider_record_bytes;
    result.success=result.errors.empty();return result;
}

std::string TilemapAssetCodec::collider_bake_json(const TileColliderBakeResult& bake) {
    Json colliders=Json::array();for(const auto& collider:bake.colliders)colliders.push_back({{"layerId",collider.layer_id},{"collision",collider.collision},
        {"cellRect",{collider.cell_x,collider.cell_y,collider.cell_width,collider.cell_height}},
        {"center",{collider.center_x,collider.center_y}},{"size",{collider.width,collider.height}}});
    Json errors=Json::array();for(const auto& issue:bake.errors)errors.push_back({{"code",issue.code},{"path",issue.path},{"message",issue.message}});
    return Json{{"schemaVersion","noemancer.tile-collider-bake/0.1"},{"success",bake.success},
        {"inputCollisionCellCount",bake.input_collision_cell_count},{"mergedCellCount",bake.merged_cell_count},
        {"estimatedOutputBytes",bake.estimated_output_bytes},{"colliders",std::move(colliders)},
        {"errors",std::move(errors)}}.dump();
}

bool TilemapAssetLibrary::register_palette(TilePaletteDocument document) {
    if(!TilemapAssetCodec::parse_palette_json(TilemapAssetCodec::write_palette_canonical_json(document)))return false;
    const auto id=document.asset_id;palettes_.insert_or_assign(id,std::move(document));
    for(const auto& [tilemap_id,tilemap]:tilemaps_)if(tilemap.palette_asset==id)rebuild(tilemap_id);
    return true;
}

bool TilemapAssetLibrary::register_tilemap(TilemapDocument document) {
    if(!TilemapAssetCodec::parse_tilemap_json(TilemapAssetCodec::write_tilemap_canonical_json(document)))return false;
    const auto id=document.asset_id;tilemaps_.insert_or_assign(id,std::move(document));rebuild(id);return true;
}

std::optional<ResolvedTilemapAsset> TilemapAssetLibrary::resolve(const std::string_view tilemap_asset) const {
    const auto map=tilemaps_.find(std::string(tilemap_asset));if(map==tilemaps_.end())return std::nullopt;
    const auto palette=palettes_.find(map->second.palette_asset);if(palette==palettes_.end())return std::nullopt;
    std::unordered_set<std::string> tile_ids;for(const auto& tile:palette->second.tiles)tile_ids.insert(tile.id);
    for(const auto& layer:map->second.layers)for(const auto& chunk:layer.chunks)for(const auto& cell:chunk.cells)
        if(!tile_ids.contains(cell.tile_id))return std::nullopt;
    return ResolvedTilemapAsset{map->second,palette->second};
}

const CompiledTilemapAsset* TilemapAssetLibrary::resolve_compiled(const std::string_view tilemap_asset) const noexcept {
    const auto found=compiled_.find(std::string(tilemap_asset));return found==compiled_.end()?nullptr:&found->second;
}

TilemapProductionStats TilemapAssetLibrary::production_stats(const std::string_view tilemap_asset) const {
    if(const auto* compiled=resolve_compiled(tilemap_asset);compiled!=nullptr)return compiled->production;
    TilemapProductionStats result;result.code="tilemap.asset-not-found";
    error(result.errors,"tilemap.asset-not-found","/assetId","No compiled tilemap is registered for the requested asset.");
    return result;
}

TilemapChunkVisibilityResult TilemapAssetLibrary::visible_chunks(
    const std::string_view tilemap_asset,const TilemapChunkVisibilityQuery& query) const {
    TilemapChunkVisibilityResult result;
    const auto* compiled=resolve_compiled(tilemap_asset);
    if(compiled==nullptr) {
        result.code="tilemap.asset-not-found";
        error(result.errors,"tilemap.asset-not-found","/assetId","No compiled tilemap is registered for the requested asset.");
        return result;
    }
    result.source_fingerprint=compiled->source_fingerprint;
    if(query.minimum_cell_x>query.maximum_cell_x||query.minimum_cell_y>query.maximum_cell_y) {
        result.code="tilemap.visibility-bounds";
        error(result.errors,"tilemap.visibility-bounds","/bounds","Visibility bounds must be ordered.");
        return result;
    }
    if(query.maximum_chunk_count==0U||query.maximum_chunk_count>TilemapProductionLimits::maximum_visible_chunks) {
        result.code="tilemap.visibility-limit";
        error(result.errors,"tilemap.visibility-limit","/maximumChunkCount","Visible chunk budget exceeds the production limit.");
        return result;
    }
    for(const auto& chunk:compiled->chunks) {
        if(!query.layer_id.empty()&&chunk.layer_id!=query.layer_id)continue;
        ++result.candidate_chunk_count;
        if(chunk.maximum_cell_x<query.minimum_cell_x||chunk.minimum_cell_x>query.maximum_cell_x||
           chunk.maximum_cell_y<query.minimum_cell_y||chunk.minimum_cell_y>query.maximum_cell_y)continue;
        if(result.chunks.size()>=query.maximum_chunk_count) {
            result.code="tilemap.visibility-limit";
            error(result.errors,"tilemap.visibility-limit","/maximumChunkCount","Visible chunk result exceeds the requested budget.");
            result.chunks.clear();result.visible_chunk_count=0U;result.visible_cell_count=0U;result.visible_packed_bytes=0U;
            return result;
        }
        result.chunks.push_back({chunk.stable_id,chunk.content_fingerprint,chunk.layer_id,chunk.chunk_x,chunk.chunk_y,
            chunk.cells.size(),chunk.gpu_range});
        result.visible_cell_count+=chunk.cells.size();
        result.visible_packed_bytes+=static_cast<std::size_t>(chunk.gpu_range.byte_size);
    }
    result.visible_chunk_count=result.chunks.size();result.success=true;result.code="ok";return result;
}

TilemapIncrementalUpdateResult TilemapAssetLibrary::apply_stroke(
    const std::string_view tilemap_asset,const TilemapStrokePlan& plan,const bool dry_run) {
    TilemapIncrementalUpdateResult result;result.dry_run=dry_run;
    const auto map_found=tilemaps_.find(std::string(tilemap_asset));
    const auto compiled_found=compiled_.find(std::string(tilemap_asset));
    if(map_found==tilemaps_.end()||compiled_found==compiled_.end()) {
        result.code="tilemap.asset-not-found";
        error(result.errors,"tilemap.asset-not-found","/assetId","No compiled tilemap is registered for the requested asset.");
        return result;
    }
    result.asset_id=map_found->second.asset_id;
    result.source_fingerprint_before=TilemapAssetCodec::tilemap_fingerprint(map_found->second);
    result.compilation_revision_before=compiled_found->second.compilation_revision;
    TilemapDocument candidate=map_found->second;
    const auto applied=TilemapAssetCodec::apply_stroke(candidate,plan,false);
    if(!applied.success) {
        result.code=applied.code;result.errors=plan.errors;return result;
    }
    const auto old_document=map_found->second;
    const auto old_compiled=compiled_found->second;
    const auto next_revision_before=next_compilation_revision_;
    map_found->second=std::move(candidate);
    rebuild(tilemap_asset);
    const auto rebuilt_found=compiled_.find(std::string(tilemap_asset));
    if(rebuilt_found==compiled_.end()) {
        map_found->second=old_document;compiled_.insert_or_assign(std::string(tilemap_asset),old_compiled);
        next_compilation_revision_=next_revision_before;
        result.code="tilemap.incremental-compile-failed";
        error(result.errors,"tilemap.incremental-compile-failed","/assetId","Incremental tilemap compilation failed validation.");
        return result;
    }
    const auto& next_compiled=rebuilt_found->second;
    result.source_fingerprint_after=TilemapAssetCodec::tilemap_fingerprint(map_found->second);
    result.compilation_revision_after=next_compiled.compilation_revision;
    result.changed_cell_count=applied.changed_cell_count;
    result.resident_packed_bytes=next_compiled.production.estimated_packed_bytes;
    std::unordered_map<std::string,const CompiledTilemapChunk*> old_chunks;
    old_chunks.reserve(old_compiled.chunks.size());
    for(const auto& chunk:old_compiled.chunks)old_chunks.emplace(chunk.stable_id,&chunk);
    std::set<std::string> dirty_ids;
    for(const auto& chunk:next_compiled.chunks) {
        const auto old=old_chunks.find(chunk.stable_id);
        if(old!=old_chunks.end()&&old->second->content_fingerprint==chunk.content_fingerprint) {
            ++result.reused_chunk_count;
            if(old->second->gpu_range==chunk.gpu_range)++result.stable_gpu_range_reuse_count;
            continue;
        }
        ++result.rebuilt_chunk_count;dirty_ids.insert(chunk.stable_id);
        ++result.created_chunk_count;
        if(old!=old_chunks.end())--result.created_chunk_count;
        result.uploaded_cell_count+=chunk.cells.size();
    }
    for(const auto& chunk:old_compiled.chunks)
        if(!std::ranges::any_of(next_compiled.chunks,[&](const auto& candidate_chunk){return candidate_chunk.stable_id==chunk.stable_id;})) {
            ++result.removed_chunk_count;dirty_ids.insert(chunk.stable_id);
        }
    result.dirty_chunk_ids.assign(dirty_ids.begin(),dirty_ids.end());result.dirty_chunk_count=dirty_ids.size();
    result.uploaded_bytes=result.uploaded_cell_count*TilemapProductionLimits::packed_cell_stride_bytes;
    result.success=true;result.code=dry_run?"tilemap.incremental-dry-run":"ok";
    if(dry_run) {
        map_found->second=old_document;compiled_.insert_or_assign(std::string(tilemap_asset),old_compiled);
        next_compilation_revision_=next_revision_before;
    }
    return result;
}

std::string TilemapAssetLibrary::visibility_json(const TilemapChunkVisibilityResult& result) {
    Json chunks=Json::array();
    for(const auto& chunk:result.chunks)
        chunks.push_back({{"stableId",chunk.stable_id},{"contentFingerprint",chunk.content_fingerprint},
            {"layerId",chunk.layer_id},{"chunk",{chunk.chunk_x,chunk.chunk_y}},{"cellCount",chunk.cell_count},
            {"gpuRange",{{"firstCell",chunk.gpu_range.first_cell},{"cellCount",chunk.gpu_range.cell_count},
                {"byteOffset",chunk.gpu_range.byte_offset},{"byteSize",chunk.gpu_range.byte_size}}}});
    Json errors=Json::array();for(const auto& issue:result.errors)
        errors.push_back({{"code",issue.code},{"path",issue.path},{"message",issue.message}});
    return Json{{"schemaVersion","noemancer.tilemap-visibility/0.1"},{"success",result.success},{"code",result.code},
        {"sourceFingerprint",result.source_fingerprint},{"candidateChunkCount",result.candidate_chunk_count},
        {"visibleChunkCount",result.visible_chunk_count},{"visibleCellCount",result.visible_cell_count},
        {"visiblePackedBytes",result.visible_packed_bytes},{"chunks",std::move(chunks)},{"errors",std::move(errors)}}.dump();
}

std::string TilemapAssetLibrary::incremental_update_json(const TilemapIncrementalUpdateResult& result) {
    Json dirty=Json::array();for(const auto& id:result.dirty_chunk_ids)dirty.push_back(id);
    Json errors=Json::array();for(const auto& issue:result.errors)
        errors.push_back({{"code",issue.code},{"path",issue.path},{"message",issue.message}});
    return Json{{"schemaVersion","noemancer.tilemap-incremental-receipt/0.1"},{"success",result.success},
        {"dryRun",result.dry_run},{"code",result.code},{"assetId",result.asset_id},
        {"sourceFingerprintBefore",result.source_fingerprint_before},{"sourceFingerprintAfter",result.source_fingerprint_after},
        {"compilationRevisionBefore",result.compilation_revision_before},{"compilationRevisionAfter",result.compilation_revision_after},
        {"changedCellCount",result.changed_cell_count},{"dirtyChunkCount",result.dirty_chunk_count},
        {"rebuiltChunkCount",result.rebuilt_chunk_count},{"reusedChunkCount",result.reused_chunk_count},
        {"stableGpuRangeReuseCount",result.stable_gpu_range_reuse_count},{"createdChunkCount",result.created_chunk_count},
        {"removedChunkCount",result.removed_chunk_count},{"uploadedCellCount",result.uploaded_cell_count},
        {"uploadedBytes",result.uploaded_bytes},{"residentPackedBytes",result.resident_packed_bytes},
        {"dirtyChunkIds",std::move(dirty)},{"errors",std::move(errors)}}.dump();
}

void TilemapAssetLibrary::rebuild(const std::string_view tilemap_asset) {
    const auto map_found=tilemaps_.find(std::string(tilemap_asset));
    if(map_found==tilemaps_.end()){compiled_.erase(std::string(tilemap_asset));return;}
    const auto palette_found=palettes_.find(map_found->second.palette_asset);
    if(palette_found==palettes_.end()){compiled_.erase(map_found->first);return;}
    const auto& map=map_found->second;const auto& palette=palette_found->second;
    std::unordered_map<std::string,const TileDefinition*> definitions;
    definitions.reserve(palette.tiles.size());for(const auto& tile:palette.tiles)definitions.emplace(tile.id,&tile);
    CompiledTilemapAsset compiled;
    compiled.source={map,palette};compiled.source_fingerprint=TilemapAssetCodec::tilemap_fingerprint(map);
    compiled.production=TilemapAssetCodec::production_stats(map);
    if(!compiled.production.valid){compiled_.erase(map_found->first);return;}
    compiled.compilation_revision=next_compilation_revision_++;
    std::uint64_t packed_cell_offset{};
    for(const auto& layer:map.layers) {
        struct Occupied {const TileCell* cell{};const TileDefinition* tile{};std::int32_t chunk_x{};std::int32_t chunk_y{};};
        std::map<std::pair<std::int32_t,std::int32_t>,Occupied> occupied;
        for(const auto& chunk:layer.chunks)for(const auto& cell:chunk.cells) {
            ++compiled.total_cell_count;const auto definition=definitions.find(cell.tile_id);
            if(definition==definitions.end()){compiled_.erase(map_found->first);return;}
            const auto global_x=static_cast<std::int64_t>(chunk.x)*map.chunk_width+cell.x;
            const auto global_y=static_cast<std::int64_t>(chunk.y)*map.chunk_height+cell.y;
            if(global_x<std::numeric_limits<std::int32_t>::min()||global_x>std::numeric_limits<std::int32_t>::max()||
               global_y<std::numeric_limits<std::int32_t>::min()||global_y>std::numeric_limits<std::int32_t>::max()) {
                compiled_.erase(map_found->first);return;
            }
            occupied.emplace(std::pair{static_cast<std::int32_t>(global_x),static_cast<std::int32_t>(global_y)},
                Occupied{&cell,definition->second,chunk.x,chunk.y});
        }
        std::map<std::pair<std::int32_t,std::int32_t>,CompiledTilemapChunk> chunks;
        for(const auto& [position,entry]:occupied) {
            std::uint8_t neighbor_mask{};
            if(!entry.tile->autotile_group.empty()) {
                const auto joins=[&](const std::int32_t x,const std::int32_t y) {const auto neighbor=occupied.find({x,y});
                    return neighbor!=occupied.end()&&neighbor->second.tile->autotile_group==entry.tile->autotile_group;};
                if(joins(position.first,position.second+1))neighbor_mask|=1U;
                if(joins(position.first+1,position.second))neighbor_mask|=2U;
                if(joins(position.first,position.second-1))neighbor_mask|=4U;
                if(joins(position.first-1,position.second))neighbor_mask|=8U;
            }
            auto [chunk_it,inserted]=chunks.try_emplace({entry.chunk_x,entry.chunk_y});auto& chunk=chunk_it->second;
            if(inserted) {
                chunk.stable_id=map.asset_id+"/"+layer.id+"/chunk/"+std::to_string(entry.chunk_x)+","+std::to_string(entry.chunk_y);
                chunk.layer_id=layer.id;chunk.sorting_layer=layer.sorting_layer;chunk.sorting_order=layer.sorting_order;
                chunk.chunk_x=entry.chunk_x;chunk.chunk_y=entry.chunk_y;
                chunk.minimum_cell_x=chunk.maximum_cell_x=position.first;chunk.minimum_cell_y=chunk.maximum_cell_y=position.second;
            } else {
                chunk.minimum_cell_x=std::min(chunk.minimum_cell_x,position.first);chunk.minimum_cell_y=std::min(chunk.minimum_cell_y,position.second);
                chunk.maximum_cell_x=std::max(chunk.maximum_cell_x,position.first);chunk.maximum_cell_y=std::max(chunk.maximum_cell_y,position.second);
            }
            chunk.cells.push_back({position.first,position.second,entry.cell->tile_id,entry.tile->autotile_group,neighbor_mask,
                std::string(TilemapAssetCodec::resolve_autotile_frame(*entry.tile,neighbor_mask)),entry.cell->flip_x,entry.cell->flip_y});
        }
        for(auto& [unused,chunk]:chunks) {
            std::string content=chunk.layer_id+"\n"+chunk.sorting_layer+"\n"+
                std::to_string(chunk.sorting_order)+"\n"+std::to_string(chunk.chunk_x)+","+std::to_string(chunk.chunk_y);
            for(const auto& cell:chunk.cells)content+="\n"+std::to_string(cell.cell_x)+","+std::to_string(cell.cell_y)+","+cell.tile_id+","+
                cell.autotile_group+","+std::to_string(cell.autotile_mask)+","+cell.frame_id+","+(cell.flip_x?"1":"0")+","+(cell.flip_y?"1":"0");
            chunk.content_fingerprint=text_fingerprint(content);
            const auto first_cell=packed_cell_offset;
            const auto cell_count=static_cast<std::uint64_t>(chunk.cells.size());
            chunk.gpu_range={first_cell,cell_count,first_cell*TilemapProductionLimits::packed_cell_stride_bytes,
                cell_count*TilemapProductionLimits::packed_cell_stride_bytes};
            packed_cell_offset+=cell_count;
            compiled.chunks.push_back(std::move(chunk));
        }
    }
    compiled_.insert_or_assign(map_found->first,std::move(compiled));
}

} // namespace noemancer
