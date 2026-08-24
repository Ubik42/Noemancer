#include "engine/tilemap_asset.hpp"

#include <iostream>
#include <nlohmann/json.hpp>

int main() {
    const auto palette=noemancer::TilemapAssetCodec::parse_palette_json(R"({
      "schema":"noemancer.tile-palette/0.1","assetId":"palette.test","spriteAsset":"sprite.test.tiles",
      "tiles":[
        {"id":"ground","frame":"ground.0","collision":"solid","tags":["terrain"]},
        {"id":"platform","frame":"platform.0","collision":"one-way","tags":["platform","terrain"]},
        {"id":"flower","frame":"flower.0","collision":"none","tags":["decor"]}
      ]})");
    const auto tilemap=noemancer::TilemapAssetCodec::parse_tilemap_json(R"({
      "schema":"noemancer.tilemap/0.1","assetId":"tilemap.test","paletteAsset":"palette.test",
      "cellSize":[0.5,0.5],"chunkSize":[8,8],"layers":[
        {"id":"ground","sortingLayer":"terrain","sortingOrder":0,"collisionEnabled":true,"chunks":[
          {"position":[0,0],"cells":[[0,0,"ground"],[1,0,"ground"],[2,0,"ground"],[0,1,"ground"],[1,1,"ground"],[2,1,"ground"],[4,3,"platform"],[5,3,"platform"],[7,7,"flower",true,false]]},
          {"position":[1,0],"cells":[[0,0,"ground"]]}
        ]}
      ]})");
    if(!palette||!tilemap){std::cerr<<"Valid palette or tilemap was rejected\n";return 1;}
    const auto bake=noemancer::TilemapAssetCodec::bake_colliders(*palette.document,*tilemap.document);
    if(!bake.success||bake.colliders.size()!=3||bake.colliders[0].cell_width!=3||bake.colliders[0].cell_height!=2||
       bake.colliders[0].width!=1.5F||bake.colliders[1].collision!="one-way"||bake.colliders[1].cell_width!=2||
       bake.colliders[1].cell_height!=1||bake.colliders[2].cell_x!=8) {
        std::cerr<<"Deterministic solid/one-way collider merging is incorrect\n";return 2;
    }
    const auto bake_json=nlohmann::json::parse(noemancer::TilemapAssetCodec::collider_bake_json(bake));
    if(bake_json.at("schemaVersion")!="noemancer.tile-collider-bake/0.1"||bake_json.at("colliders").size()!=3) return 3;
    const auto palette_roundtrip=noemancer::TilemapAssetCodec::parse_palette_json(noemancer::TilemapAssetCodec::write_palette_canonical_json(*palette.document));
    const auto tilemap_roundtrip=noemancer::TilemapAssetCodec::parse_tilemap_json(noemancer::TilemapAssetCodec::write_tilemap_canonical_json(*tilemap.document));
    if(!palette_roundtrip||!tilemap_roundtrip||
       noemancer::TilemapAssetCodec::write_palette_canonical_json(*palette_roundtrip.document)!=noemancer::TilemapAssetCodec::write_palette_canonical_json(*palette.document)||
       noemancer::TilemapAssetCodec::write_tilemap_canonical_json(*tilemap_roundtrip.document)!=noemancer::TilemapAssetCodec::write_tilemap_canonical_json(*tilemap.document)) return 4;
    const auto invalid=noemancer::TilemapAssetCodec::parse_tilemap_json(R"({
      "schema":"noemancer.tilemap/0.1","assetId":"bad","paletteAsset":"palette.test","cellSize":[1,1],"chunkSize":[8,8],
      "layers":[{"id":"ground","sortingLayer":"terrain","sortingOrder":0,"collisionEnabled":true,
        "chunks":[{"position":[0,0],"cells":[[8,0,"ground"],[0,0,"ground"],[0,0,"ground"]]}]}]})");
    if(invalid||invalid.errors.size()!=2) {std::cerr<<"Out-of-chunk and duplicate cells were not rejected\n";return 5;}
    auto unknown=*tilemap.document;unknown.layers[0].chunks[0].cells[0].tile_id="missing";
    if(noemancer::TilemapAssetCodec::bake_colliders(*palette.document,unknown).success) return 6;
    noemancer::TilemapAssetLibrary library;
    if(!library.register_palette(*palette.document)||!library.register_tilemap(*tilemap.document)||
       !library.resolve("tilemap.test")||library.resolve("missing")) return 7;
    const auto* compiled=library.resolve_compiled("tilemap.test");
    if(!compiled||compiled->chunks.size()!=2||compiled->total_cell_count!=10||compiled->source_fingerprint.empty()||
       compiled->chunks[0].content_fingerprint.empty()||compiled->chunks[1].content_fingerprint.empty())return 17;
    const auto compiled_revision=compiled->compilation_revision;
    if(library.resolve_compiled("tilemap.test")!=compiled||library.resolve_compiled("tilemap.test")->compilation_revision!=compiled_revision)return 18;
    if(!library.register_palette(*palette.document)||!library.resolve_compiled("tilemap.test")||
       library.resolve_compiled("tilemap.test")->compilation_revision<=compiled_revision)return 19;
    noemancer::TilemapAssetLibrary invalid_library;static_cast<void>(invalid_library.register_palette(*palette.document));
    static_cast<void>(invalid_library.register_tilemap(std::move(unknown)));
    if(invalid_library.resolve("tilemap.test")) return 8;
    auto editable=*tilemap.document;
    const auto base=noemancer::TilemapAssetCodec::tilemap_fingerprint(editable);
    const auto stroke=noemancer::TilemapAssetCodec::plan_stroke(*palette.document,editable,"ground",{
        {-1,-1,std::string("flower"),true,false},{7,7,std::string("ground"),false,true},{8,0,std::nullopt,false,false}},
        "test.tile-brush",base);
    const auto plan_json=nlohmann::json::parse(noemancer::TilemapAssetCodec::stroke_plan_json(stroke));
    if(!stroke.valid||stroke.changed_cell_count!=3||stroke.created_chunk_count!=1||stroke.removed_chunk_count!=1||
       plan_json.at("schemaVersion")!="noemancer.tilemap-stroke-plan/0.1"||plan_json.at("edits").size()!=3||
       !noemancer::TilemapAssetCodec::apply_stroke(editable,stroke,true).success||
       noemancer::TilemapAssetCodec::tilemap_fingerprint(editable)!=base) return 9;
    const auto applied=noemancer::TilemapAssetCodec::apply_stroke(editable,stroke,false);
    if(!applied.success||applied.fingerprint_after==base||editable.layers[0].chunks.front().x!=-1||
       editable.layers[0].chunks.front().y!=-1||editable.layers[0].chunks.front().cells[0].x!=7||
       editable.layers[0].chunks.front().cells[0].y!=7||editable.layers[0].chunks.back().x!=0) return 10;
    auto conflicting=*tilemap.document;conflicting.cell_width=0.75F;
    if(noemancer::TilemapAssetCodec::apply_stroke(conflicting,stroke,false).code!="tilemap.stroke-conflict") return 11;
    auto duplicate=noemancer::TilemapAssetCodec::plan_stroke(*palette.document,*tilemap.document,"ground",{
        {1,1,std::string("ground")},{1,1,std::nullopt}},"test.tile-brush");
    if(duplicate.valid||duplicate.errors.empty()||duplicate.errors[0].code!="tilemap.stroke-duplicate-cell") return 12;
    auto tampered=stroke;tampered.changed_cell_count=99;auto tamper_target=*tilemap.document;
    if(noemancer::TilemapAssetCodec::apply_stroke(tamper_target,tampered,false).code!="tilemap.stroke-integrity-error") return 13;
    const auto rectangle=noemancer::TilemapAssetCodec::plan_rectangle(*palette.document,*tilemap.document,"ground",-2,-2,-1,-1,
        std::string("flower"),true,false,"test.rectangle");
    const auto oversized_rectangle=noemancer::TilemapAssetCodec::plan_rectangle(*palette.document,*tilemap.document,"ground",0,0,64,64,
        std::string("flower"),false,false,"test.rectangle");
    if(!rectangle.valid||rectangle.edits.size()!=4||rectangle.changed_cell_count!=4||rectangle.created_chunk_count!=1||
       oversized_rectangle.valid||oversized_rectangle.code!="tilemap.region-size")return 14;
    const auto flood=noemancer::TilemapAssetCodec::plan_flood_fill(*palette.document,*tilemap.document,"ground",0,0,
        std::string("flower"),false,false,"test.flood");
    const auto empty_flood=noemancer::TilemapAssetCodec::plan_flood_fill(*palette.document,*tilemap.document,"ground",100,100,
        std::string("flower"),false,false,"test.flood");
    if(!flood.valid||flood.edits.size()!=6||flood.changed_cell_count!=6||empty_flood.valid||
       empty_flood.code!="tilemap.fill-unbounded-empty"||flood.edits.back().x==8)return 15;
    const auto autotile_palette=noemancer::TilemapAssetCodec::parse_palette_json(R"({
      "schema":"noemancer.tile-palette/0.2","assetId":"palette.auto","spriteAsset":"sprite.auto",
      "tiles":[{"id":"terrain","frame":"terrain.base","collision":"solid","tags":["terrain"],
        "autotile":{"group":"terrain","variants":[{"mask":2,"frame":"terrain.east"},{"mask":8,"frame":"terrain.west"}]}}]})");
    const auto invalid_autotile=noemancer::TilemapAssetCodec::parse_palette_json(R"({
      "schema":"noemancer.tile-palette/0.2","assetId":"palette.bad-auto","spriteAsset":"sprite.auto",
      "tiles":[{"id":"terrain","frame":"terrain.base","collision":"solid","tags":[],
        "autotile":{"group":"terrain","variants":[{"mask":16,"frame":"bad"},{"mask":2,"frame":"a"},{"mask":2,"frame":"b"}]}}]})");
    if(!autotile_palette||invalid_autotile||
       noemancer::TilemapAssetCodec::resolve_autotile_frame(autotile_palette.document->tiles[0],2)!="terrain.east"||
       noemancer::TilemapAssetCodec::resolve_autotile_frame(autotile_palette.document->tiles[0],1)!="terrain.base"||
       !noemancer::TilemapAssetCodec::parse_palette_json(noemancer::TilemapAssetCodec::write_palette_canonical_json(*autotile_palette.document)))return 16;
    auto editable_palette=*palette.document;const auto palette_base=noemancer::TilemapAssetCodec::palette_fingerprint(editable_palette);
    const auto palette_plan=noemancer::TilemapAssetCodec::plan_autotile_update(editable_palette,"ground","terrain",
        {{2,"ground.0"},{8,"ground.0"}},"test.palette",palette_base);
    const auto palette_preview=noemancer::TilemapAssetCodec::apply_palette_edit(editable_palette,palette_plan,true);
    if(!palette_plan.valid||!palette_preview.success||noemancer::TilemapAssetCodec::palette_fingerprint(editable_palette)!=palette_base||
       !noemancer::TilemapAssetCodec::apply_palette_edit(editable_palette,palette_plan,false).success||
       editable_palette.schema!="noemancer.tile-palette/0.2"||editable_palette.tiles[0].autotile_variants.size()!=2)return 20;
    auto tampered_palette_plan=palette_plan;tampered_palette_plan.tile_id="flower";auto palette_target=*palette.document;
    if(noemancer::TilemapAssetCodec::apply_palette_edit(palette_target,tampered_palette_plan,false).code!="tilemap.palette-edit-integrity-error")return 21;
    return 0;
}
