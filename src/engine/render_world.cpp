#include "engine/render_world.hpp"
#include "engine/stable_range_allocator.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <map>
#include <nlohmann/json.hpp>
#include <set>

namespace noemancer {

void TilemapRenderBakeCache::begin_frame(){touched_.clear();hits_=0;rebuilds_=0;retained_offscreen_=0;++frame_;}
const std::vector<RenderTileCellSnapshot>* TilemapRenderBakeCache::find(const std::string_view key,const std::string_view signature) {
    const auto found=entries_.find(std::string(key));if(found==entries_.end()||found->second.signature!=signature)return nullptr;
    touched_.insert(found->first);found->second.last_touched_frame=frame_;++hits_;return &found->second.cells;
}
void TilemapRenderBakeCache::store(std::string key,std::string signature,std::vector<RenderTileCellSnapshot> cells) {
    touched_.insert(key);entries_.insert_or_assign(std::move(key),Entry{std::move(signature),std::move(cells),frame_});++rebuilds_;
}
std::size_t TilemapRenderBakeCache::end_frame(const bool retain_untouched) {const auto before=entries_.size();constexpr std::uint64_t retention_frames=120;
    std::erase_if(entries_,[&](const auto& entry){return !touched_.contains(entry.first)&&
        (!retain_untouched||frame_-entry.second.last_touched_frame>retention_frames);});
    for(const auto& entry:entries_)if(!touched_.contains(entry.first))++retained_offscreen_;return before-entries_.size();}

namespace {
SpriteMaterialChannels projected_sprite_material(const std::optional<SpriteMaterialChannels>& authored) {
    if(authored)return *authored;
    SpriteMaterialChannels legacy_unlit;
    legacy_unlit.shading_model="unlit";
    legacy_unlit.receives_shadows=false;
    legacy_unlit.casts_shadows=false;
    return legacy_unlit;
}
void append_signature(std::string& target,const std::string_view value){target+=value;target.push_back('\0');}
void append_signature(std::string& target,const std::uint32_t value){target.append(reinterpret_cast<const char*>(&value),sizeof(value));}
void append_signature(std::string& target,const float value){append_signature(target,std::bit_cast<std::uint32_t>(value));}
std::string tile_chunk_signature(const WorldEntityView& entity,const std::vector<const ResolvedTilemapCellView*>& cells) {
    std::string result;result.reserve(256+cells.size()*48);append_signature(result,cells.front()->chunk_content_fingerprint);
    const auto& transform=*entity.transform;for(const float value:{transform.x,transform.y,transform.z,transform.scale_x,transform.scale_y,transform.scale_z,
        transform.rotation_x,transform.rotation_y,transform.rotation_z,transform.rotation_w})append_signature(result,value);
    append_signature(result,entity.tilemap_renderer->visible?1U:0U);
    for(const auto* cell:cells) {const auto& resolved=cell->sprite_frame;const auto& frame=resolved.frame;
        const auto material=projected_sprite_material(resolved.material);
        append_signature(result,resolved.asset_id);append_signature(result,resolved.texture_asset);append_signature(result,resolved.texture_width);
        append_signature(result,resolved.texture_height);append_signature(result,resolved.pixels_per_unit);append_signature(result,resolved.sampling);
        append_signature(result,resolved.alpha_mode);append_signature(result,frame.id);for(const auto value:{frame.x,frame.y,frame.width,frame.height})append_signature(result,value);
        append_signature(result,material.normal_texture_asset);append_signature(result,material.emissive_mask_texture_asset);
        append_signature(result,material.depth_texture_asset);append_signature(result,material.normal_strength);
        append_signature(result,material.emissive_r);append_signature(result,material.emissive_g);
        append_signature(result,material.emissive_b);append_signature(result,material.emissive_intensity);
        append_signature(result,material.depth_bias);append_signature(result,material.shading_model);
        append_signature(result,material.metallic);append_signature(result,material.roughness);
        append_signature(result,material.receives_shadows?1U:0U);append_signature(result,material.casts_shadows?1U:0U);
    }return result;
}
RenderTileCellSnapshot bake_tile_cell(const WorldEntityView& entity,const ResolvedTilemapCellView& cell) {
    const auto& resolved=cell.sprite_frame;const auto& frame=resolved.frame;const auto inverse_width=1.0F/static_cast<float>(resolved.texture_width);
    const auto inverse_height=1.0F/static_cast<float>(resolved.texture_height);const auto left=static_cast<float>(cell.cell_x)*cell.cell_width;
    const auto bottom=static_cast<float>(cell.cell_y)*cell.cell_height;
    const auto material=projected_sprite_material(resolved.material);
    return {cell.stable_id,entity.id,cell.tilemap_asset,cell.layer_id,cell.cell_x,cell.cell_y,cell.chunk_x,cell.chunk_y,cell.chunk_content_fingerprint,
        cell.tile_id,cell.autotile_group,cell.autotile_mask,frame.id,resolved.texture_asset,
        {frame.x*inverse_width,frame.y*inverse_height,(frame.x+frame.width)*inverse_width,(frame.y+frame.height)*inverse_height},
        {left,bottom+cell.cell_height,left+cell.cell_width,bottom},{entity.transform->x,entity.transform->y,entity.transform->z},
        {entity.transform->scale_x,entity.transform->scale_y,entity.transform->scale_z},
        {entity.transform->rotation_x,entity.transform->rotation_y,entity.transform->rotation_z,entity.transform->rotation_w},
        cell.flip_x,cell.flip_y,entity.tilemap_renderer->visible,cell.sorting_layer,cell.sorting_order,resolved.sampling,resolved.alpha_mode,
        resolved.material?resolved.material->normal_texture_asset:std::string{},resolved.material?resolved.material->emissive_mask_texture_asset:std::string{},
        resolved.material?resolved.material->depth_texture_asset:std::string{},resolved.material?resolved.material->normal_strength:1.0F,
        resolved.material?std::array<float,3>{resolved.material->emissive_r,resolved.material->emissive_g,resolved.material->emissive_b}:std::array<float,3>{1,1,1},
        resolved.material?resolved.material->emissive_intensity:0.0F,resolved.material?resolved.material->depth_bias:0.0F,
        material.shading_model,material.metallic,material.roughness,material.receives_shadows,material.casts_shadows};
}
void rebuild_tile_chunk_ranges(RenderWorldSnapshot& snapshot) {
    snapshot.tile_chunk_ranges.clear();
    for(std::size_t index=0;index<snapshot.tile_cells.size();++index) {
        const auto& cell=snapshot.tile_cells[index];const auto key=cell.entity_id+"/"+cell.layer_id+"/"+
            std::to_string(cell.chunk_x)+","+std::to_string(cell.chunk_y);
        if(!snapshot.tile_chunk_ranges.empty()&&snapshot.tile_chunk_ranges.back().key==key) {
            ++snapshot.tile_chunk_ranges.back().cell_count;continue;
        }
        snapshot.tile_chunk_ranges.push_back({key,cell.entity_id,cell.layer_id,cell.chunk_x,cell.chunk_y,
            cell.chunk_content_fingerprint,index,1});
    }
}
} // namespace

RenderWorldSnapshot RenderWorldExtractor::extract(
    const std::uint64_t world_revision,
    const std::uint64_t frame_index,
    std::vector<WorldEntityView> entities,
    const std::span<const VfxRuntime::Particle> vfx_particles,TilemapRenderBakeCache* tilemap_bake_cache) {
    std::ranges::sort(entities, {}, &WorldEntityView::id);
    RenderWorldSnapshot snapshot;
    snapshot.world_revision = world_revision;
    snapshot.frame_index = frame_index;
    snapshot.extraction_id = "render.extract." + std::to_string(world_revision) + "." + std::to_string(frame_index);
    if(tilemap_bake_cache)tilemap_bake_cache->begin_frame();
    for (const auto& entity : entities) {
        if (!snapshot.camera && entity.camera && entity.camera->primary && entity.transform) {
            snapshot.camera = RenderCameraSnapshot{
                entity.id,
                {entity.transform->x, entity.transform->y, entity.transform->z},
                {entity.camera->target_x, entity.camera->target_y, entity.camera->target_z},
                entity.camera->vertical_fov_degrees, entity.camera->near_clip, entity.camera->far_clip,
                entity.camera->projection,entity.camera->orthographic_height};
        }
        if (!snapshot.directional_light && entity.directional_light) {
            const auto& light = *entity.directional_light;
            snapshot.directional_light = RenderDirectionalLightSnapshot{
                entity.id, {light.direction_x, light.direction_y, light.direction_z},
                {light.color_r, light.color_g, light.color_b}, light.intensity,
                light.ambient_intensity, light.casts_shadows};
        }
        if(entity.local_light&&entity.transform) {
            const auto& light=*entity.local_light;
            snapshot.local_lights.push_back({entity.id,light.kind,
                {entity.transform->x,entity.transform->y,entity.transform->z},
                {light.direction_x,light.direction_y,light.direction_z},{light.color_r,light.color_g,light.color_b},
                light.luminous_power_lumens,light.range_meters,light.inner_cone_degrees,light.outer_cone_degrees,
                light.source_radius_meters,light.casts_shadows});
        }
        if(entity.transform&&entity.sprite_renderer&&entity.sprite_frame) {
            const auto& renderer=*entity.sprite_renderer;const auto& resolved=*entity.sprite_frame;const auto& frame=resolved.frame;
            const auto inverse_width=1.0F/static_cast<float>(resolved.texture_width);
            const auto inverse_height=1.0F/static_cast<float>(resolved.texture_height);
            const auto material=projected_sprite_material(resolved.material);
            snapshot.sprites.push_back(RenderSpriteSnapshot{entity.id,resolved.asset_id,resolved.clip_id,frame.id,
                resolved.texture_asset,{resolved.texture_width,resolved.texture_height},{frame.x,frame.y,frame.width,frame.height},
                {frame.x*inverse_width,frame.y*inverse_height,(frame.x+frame.width)*inverse_width,(frame.y+frame.height)*inverse_height},
                {frame.source_width,frame.source_height},{frame.trim_x,frame.trim_y},{frame.pivot_x,frame.pivot_y},resolved.pixels_per_unit,
                {entity.transform->x,entity.transform->y,entity.transform->z},
                {entity.transform->scale_x,entity.transform->scale_y,entity.transform->scale_z},
                {entity.transform->rotation_x,entity.transform->rotation_y,entity.transform->rotation_z,entity.transform->rotation_w},
                renderer.flip_x,renderer.flip_y,renderer.visible,renderer.sorting_layer,renderer.sorting_order,
                resolved.sampling,resolved.alpha_mode,
                resolved.material?resolved.material->normal_texture_asset:std::string{},
                resolved.material?resolved.material->emissive_mask_texture_asset:std::string{},
                resolved.material?resolved.material->depth_texture_asset:std::string{},
                resolved.material?resolved.material->normal_strength:1.0F,
                resolved.material?std::array<float,3>{resolved.material->emissive_r,resolved.material->emissive_g,resolved.material->emissive_b}:std::array<float,3>{1,1,1},
                resolved.material?resolved.material->emissive_intensity:0.0F,resolved.material?resolved.material->depth_bias:0.0F,
                material.shading_model,material.metallic,material.roughness,material.receives_shadows,material.casts_shadows});
        }
        if(entity.transform&&entity.tilemap_renderer) {
            std::size_t chunks=0,cells=0;if(entity.tilemap_asset)for(const auto& layer:entity.tilemap_asset->tilemap.layers){chunks+=layer.chunks.size();for(const auto& chunk:layer.chunks)cells+=chunk.cells.size();}
            snapshot.tilemaps.push_back({entity.id,entity.tilemap_renderer->tilemap_asset,
                {entity.transform->x,entity.transform->y,entity.transform->z},
                {entity.transform->scale_x,entity.transform->scale_y,entity.transform->scale_z},
                {entity.transform->rotation_x,entity.transform->rotation_y,entity.transform->rotation_z,entity.transform->rotation_w},
                entity.tilemap_renderer->visible,entity.tilemap_renderer->collision_enabled,entity.tilemap_asset.has_value(),
                entity.tilemap_asset?entity.tilemap_asset->palette.asset_id:std::string{},
                entity.tilemap_asset?entity.tilemap_asset->tilemap.layers.size():0U,chunks,cells,
                entity.tilemap_cells.size(),entity.tilemap_compiled_chunk_count,entity.tilemap_compilation_revision,
                entity.tilemap_compiled_chunk_count,0U,entity.tilemap_cells.size(),
                entity.tilemap_cells_truncated,entity.tilemap_early_visibility_applied,entity.tilemap_resolved_chunk_count,
                entity.tilemap_skipped_chunk_count,entity.tilemap_cells_skipped_before_resolution});
            snapshot.tilemap_early_visibility_applied=snapshot.tilemap_early_visibility_applied||entity.tilemap_early_visibility_applied;
            snapshot.tilemap_chunks_resolved+=entity.tilemap_resolved_chunk_count;
            snapshot.tilemap_chunks_skipped_before_resolution+=entity.tilemap_skipped_chunk_count;
            snapshot.tilemap_cells_skipped_before_resolution+=entity.tilemap_cells_skipped_before_resolution;
            using ChunkKey=std::tuple<std::string,std::int32_t,std::int32_t>;
            std::map<ChunkKey,std::vector<const ResolvedTilemapCellView*>> grouped;
            for(const auto& cell:entity.tilemap_cells)grouped[{cell.layer_id,cell.chunk_x,cell.chunk_y}].push_back(&cell);
            for(const auto& [chunk_key,chunk_cells]:grouped) {
                const auto cache_key=entity.id+"/"+entity.tilemap_renderer->tilemap_asset+"/"+std::get<0>(chunk_key)+"/"+
                    std::to_string(std::get<1>(chunk_key))+","+std::to_string(std::get<2>(chunk_key));
                const auto signature=tile_chunk_signature(entity,chunk_cells);
                if(tilemap_bake_cache)if(const auto* cached=tilemap_bake_cache->find(cache_key,signature)) {
                    snapshot.tile_cells.insert(snapshot.tile_cells.end(),cached->begin(),cached->end());continue;
                }
                std::vector<RenderTileCellSnapshot> baked;baked.reserve(chunk_cells.size());
                for(const auto* cell:chunk_cells)baked.push_back(bake_tile_cell(entity,*cell));
                snapshot.tile_cells.insert(snapshot.tile_cells.end(),baked.begin(),baked.end());
                if(tilemap_bake_cache)tilemap_bake_cache->store(cache_key,signature,std::move(baked));
            }
        }
        if (!entity.transform || !entity.mesh_renderer) continue;
        RenderInstanceSnapshot instance;
        instance.entity_id = entity.id;
        instance.mesh_asset = entity.mesh_renderer->mesh_asset;
        instance.position = {entity.transform->x, entity.transform->y, entity.transform->z};
        instance.scale = {entity.transform->scale_x, entity.transform->scale_y, entity.transform->scale_z};
        instance.rotation = {entity.transform->rotation_x,entity.transform->rotation_y,entity.transform->rotation_z,entity.transform->rotation_w};
        instance.visible = entity.mesh_renderer->visible;
        instance.casts_shadows = entity.mesh_renderer->casts_shadows;
        instance.receives_shadows = entity.mesh_renderer->receives_shadows;
        instance.material_override = entity.pbr_material.has_value();
        if (entity.skeletal_pose) instance.skinning_matrices = entity.skeletal_pose->skinning_matrices;
        if (entity.pbr_material) {
            instance.material.base_color = {entity.pbr_material->base_r, entity.pbr_material->base_g, entity.pbr_material->base_b};
            instance.material.metallic = entity.pbr_material->metallic;
            instance.material.roughness = entity.pbr_material->roughness;
            instance.material.base_color_texture = entity.pbr_material->base_color_texture;
            instance.material.emissive_color = {entity.pbr_material->emissive_r, entity.pbr_material->emissive_g, entity.pbr_material->emissive_b};
            instance.material.emissive_intensity = entity.pbr_material->emissive_intensity;
        }
        snapshot.instances.push_back(std::move(instance));
    }
    constexpr std::size_t maximum_extracted_vfx_particles = 4096;
    std::vector<const VfxRuntime::Particle*> particle_order;
    particle_order.reserve(vfx_particles.size());
    for(const auto& particle:vfx_particles) particle_order.push_back(&particle);
    const auto camera_position=snapshot.camera?snapshot.camera->position:std::array<float,3>{};
    const auto distance_squared=[&](const VfxRuntime::Particle& particle) {
        const auto x=particle.position.x-camera_position[0],y=particle.position.y-camera_position[1],z=particle.position.z-camera_position[2];
        return x*x+y*y+z*z;
    };
    const auto blend_rank=[](const std::string_view mode) { return mode=="cutout"?0:mode=="additive"?1:2; };
    std::ranges::stable_sort(particle_order,[&](const auto* left,const auto* right) {
        const auto left_rank=blend_rank(left->blend_mode),right_rank=blend_rank(right->blend_mode);
        if(left_rank!=right_rank) return left_rank<right_rank;
        if(left->blend_mode=="alpha") {
            const auto left_distance=distance_squared(*left),right_distance=distance_squared(*right);
            if(left_distance!=right_distance) return left_distance>right_distance;
        }
        return left->id<right->id;
    });
    const auto particle_count = std::min(particle_order.size(), maximum_extracted_vfx_particles);
    snapshot.instances.reserve(snapshot.instances.size() + particle_count);
    snapshot.vfx_particles.reserve(particle_count);
    for (std::size_t index = 0; index < particle_count; ++index) {
        const auto& particle = *particle_order[index];
        snapshot.vfx_particles.push_back(RenderVfxParticleSnapshot{
            particle.id, particle.emitter_id, particle.graph_id,
            {particle.position.x, particle.position.y, particle.position.z},
            {particle.velocity.x, particle.velocity.y, particle.velocity.z},
            {particle.color.r, particle.color.g, particle.color.b, particle.color.a},
            particle.age, particle.lifetime, particle.size,particle.blend_mode,
            particle.pixel_alignment,particle.size_quantization,particle.sampling,distance_squared(particle),
            particle.size_start,particle.size_end,
            {particle.color_start.r,particle.color_start.g,particle.color_start.b,particle.color_start.a},
            {particle.color_end.r,particle.color_end.g,particle.color_end.b,particle.color_end.a},
            {particle.gravity.x,particle.gravity.y,particle.gravity.z},particle.drag,
            particle.spawn_seed,particle.spawn_index,{particle.spawn_origin.x,particle.spawn_origin.y,particle.spawn_origin.z},
            particle.lifetime_min,particle.lifetime_max,particle.speed_min,particle.speed_max});
        RenderInstanceSnapshot instance;
        instance.entity_id = "vfx.particle." + std::to_string(particle.id);
        instance.mesh_asset = "asset.primitive.sphere";
        instance.position = {particle.position.x, particle.position.y, particle.position.z};
        const auto size = std::max(particle.size, 0.001F);
        instance.scale = {size, size, size};
        instance.casts_shadows = false;
        instance.receives_shadows = false;
        instance.material_override = true;
        instance.material.base_color = {particle.color.r, particle.color.g, particle.color.b};
        instance.material.roughness = 0.35F;
        instance.material.emissive_color = instance.material.base_color;
        instance.material.emissive_intensity = 4.0F * std::clamp(particle.color.a, 0.0F, 1.0F);
        instance.vfx_particle = true;
        instance.vfx_particle_id = particle.id;
        instance.vfx_emitter_id = particle.emitter_id;
        instance.vfx_graph_id = particle.graph_id;
        snapshot.instances.push_back(std::move(instance));
    }
    snapshot.vfx_particle_count = particle_count;
    snapshot.vfx_total_particle_count=vfx_particles.size();
    snapshot.vfx_truncated=particle_count<vfx_particles.size();
    std::ranges::sort(snapshot.instances, {}, &RenderInstanceSnapshot::entity_id);
    std::ranges::sort(snapshot.sprites,[](const auto& left,const auto& right) {
        if(left.sorting_layer!=right.sorting_layer)return left.sorting_layer<right.sorting_layer;
        if(left.sorting_order!=right.sorting_order)return left.sorting_order<right.sorting_order;
        return left.entity_id<right.entity_id;
    });
    std::ranges::sort(snapshot.tile_cells,[](const auto& left,const auto& right) {
        if(left.sorting_layer!=right.sorting_layer)return left.sorting_layer<right.sorting_layer;
        if(left.sorting_order!=right.sorting_order)return left.sorting_order<right.sorting_order;
        if(left.entity_id!=right.entity_id)return left.entity_id<right.entity_id;
        if(left.layer_id!=right.layer_id)return left.layer_id<right.layer_id;
        if(left.chunk_y!=right.chunk_y)return left.chunk_y<right.chunk_y;
        if(left.chunk_x!=right.chunk_x)return left.chunk_x<right.chunk_x;
        return left.stable_id<right.stable_id;
    });
    rebuild_tile_chunk_ranges(snapshot);
    if(tilemap_bake_cache) {snapshot.tilemap_bake_cache_applied=true;snapshot.tilemap_bake_cache_hits=tilemap_bake_cache->hits();
        snapshot.tilemap_bake_cache_rebuilds=tilemap_bake_cache->rebuilds();snapshot.tilemap_bake_cache_evictions=tilemap_bake_cache->end_frame(snapshot.tilemap_early_visibility_applied);
        snapshot.tilemap_bake_cached_chunks=tilemap_bake_cache->size();snapshot.tilemap_bake_retained_offscreen_chunks=tilemap_bake_cache->retained_offscreen();}
    return snapshot;
}

void RenderWorldExtractor::cull_tilemap_chunks(RenderWorldSnapshot& snapshot,const std::uint32_t viewport_width,
                                                const std::uint32_t viewport_height) {
    snapshot.tilemap_cells_before_culling=snapshot.tile_cells.size();
    if(!snapshot.camera||viewport_width==0||viewport_height==0)return;
    const auto& camera=*snapshot.camera;
    const auto subtract=[](const std::array<float,3>& a,const std::array<float,3>& b) {
        return std::array<float,3>{a[0]-b[0],a[1]-b[1],a[2]-b[2]};};
    const auto dot=[](const std::array<float,3>& a,const std::array<float,3>& b) {return a[0]*b[0]+a[1]*b[1]+a[2]*b[2];};
    const auto normalize=[&](const std::array<float,3>& value) {const auto length=std::sqrt(dot(value,value));
        return length>1.0e-6F?std::array<float,3>{value[0]/length,value[1]/length,value[2]/length}:std::array<float,3>{};};
    const auto cross=[](const std::array<float,3>& a,const std::array<float,3>& b) {
        return std::array<float,3>{a[1]*b[2]-a[2]*b[1],a[2]*b[0]-a[0]*b[2],a[0]*b[1]-a[1]*b[0]};};
    const auto forward=normalize(subtract(camera.target,camera.position));
    auto right=normalize(cross(forward,{0.0F,1.0F,0.0F}));if(dot(right,right)<0.5F)right={1.0F,0.0F,0.0F};
    const auto up=normalize(cross(right,forward));if(dot(forward,forward)<0.5F||dot(up,up)<0.5F)return;
    const float aspect=static_cast<float>(viewport_width)/static_cast<float>(viewport_height);
    const float tangent=std::tan(camera.vertical_fov_degrees*0.008726646259971648F);
    struct ChunkBounds {std::array<float,3> center{};float radius{};bool initialized{};};
    using ChunkKey=std::tuple<std::string,std::string,std::int32_t,std::int32_t>;
    std::map<ChunkKey,ChunkBounds> bounds;
    const auto rotate=[](const std::array<float,4>& q,const std::array<float,3>& p) {
        const std::array<float,3> u{q[0],q[1],q[2]};const float s=q[3];
        const auto uv=u[0]*p[0]+u[1]*p[1]+u[2]*p[2];const auto uu=u[0]*u[0]+u[1]*u[1]+u[2]*u[2];
        return std::array<float,3>{2.0F*uv*u[0]+(s*s-uu)*p[0]+2.0F*s*(u[1]*p[2]-u[2]*p[1]),
            2.0F*uv*u[1]+(s*s-uu)*p[1]+2.0F*s*(u[2]*p[0]-u[0]*p[2]),
            2.0F*uv*u[2]+(s*s-uu)*p[2]+2.0F*s*(u[0]*p[1]-u[1]*p[0])};};
    for(const auto& cell:snapshot.tile_cells) {
        auto& bound=bounds[{cell.entity_id,cell.layer_id,cell.chunk_x,cell.chunk_y}];
        const float cx=(cell.local_rect[0]+cell.local_rect[2])*0.5F,cy=(cell.local_rect[1]+cell.local_rect[3])*0.5F;
        auto local=std::array<float,3>{cx*cell.scale[0],cy*cell.scale[1],0.0F};const auto rotated=rotate(cell.rotation,local);
        const std::array<float,3> center{cell.position[0]+rotated[0],cell.position[1]+rotated[1],cell.position[2]+rotated[2]};
        const float half_width=std::abs(cell.local_rect[2]-cell.local_rect[0])*std::abs(cell.scale[0])*0.5F;
        const float half_height=std::abs(cell.local_rect[1]-cell.local_rect[3])*std::abs(cell.scale[1])*0.5F;
        const float cell_radius=std::sqrt(half_width*half_width+half_height*half_height);
        if(!bound.initialized){bound={center,cell_radius,true};continue;}
        const auto offset=subtract(center,bound.center);const auto distance=std::sqrt(dot(offset,offset));
        bound.radius=std::max(bound.radius,distance+cell_radius);
    }
    std::set<ChunkKey> visible;
    for(const auto& [key,bound]:bounds) {
        const auto delta=subtract(bound.center,camera.position);const float depth=dot(delta,forward);
        bool inside=depth+bound.radius>=camera.near_clip&&depth-bound.radius<=camera.far_clip;
        if(inside&&camera.projection=="orthographic") {
            const float half_height=camera.orthographic_height*0.5F,half_width=half_height*aspect;
            inside=std::abs(dot(delta,right))<=half_width+bound.radius&&std::abs(dot(delta,up))<=half_height+bound.radius;
        } else if(inside) {
            inside=depth+bound.radius>0.0F&&std::abs(dot(delta,right))<=std::max(depth,0.0F)*tangent*aspect+bound.radius&&
                std::abs(dot(delta,up))<=std::max(depth,0.0F)*tangent+bound.radius;
        }
        if(inside)visible.insert(key);
    }
    std::erase_if(snapshot.tile_cells,[&](const RenderTileCellSnapshot& cell) {
        return !visible.contains({cell.entity_id,cell.layer_id,cell.chunk_x,cell.chunk_y});});
    rebuild_tile_chunk_ranges(snapshot);
    snapshot.tilemap_chunk_culling_applied=true;snapshot.tilemap_visible_chunk_count=visible.size();
    snapshot.tilemap_culled_chunk_count=bounds.size()-visible.size();
    for(auto& tilemap:snapshot.tilemaps) {
        tilemap.visible_chunk_count=0;tilemap.culled_chunk_count=0;tilemap.visible_cell_count=0;
        for(const auto& key:visible)if(std::get<0>(key)==tilemap.entity_id)++tilemap.visible_chunk_count;
        for(const auto& [key,unused]:bounds)if(std::get<0>(key)==tilemap.entity_id&&!visible.contains(key))++tilemap.culled_chunk_count;
        for(const auto& cell:snapshot.tile_cells)if(cell.entity_id==tilemap.entity_id)++tilemap.visible_cell_count;
    }
}

std::string render_world_json(const RenderWorldSnapshot& snapshot) {
    nlohmann::json out{{"schemaVersion", snapshot.schema_version}, {"extractionId", snapshot.extraction_id},
        {"worldRevision", snapshot.world_revision}, {"frameIndex", snapshot.frame_index},
        {"vfxParticleCount", snapshot.vfx_particle_count},{"vfxTotalParticleCount",snapshot.vfx_total_particle_count},
        {"vfxTruncated",snapshot.vfx_truncated},{"vfxSortPolicy",snapshot.vfx_sort_policy},
        {"tilemapChunkCulling",{{"applied",snapshot.tilemap_chunk_culling_applied},{"visibleChunks",snapshot.tilemap_visible_chunk_count},
            {"culledChunks",snapshot.tilemap_culled_chunk_count},{"cellsBefore",snapshot.tilemap_cells_before_culling},
            {"cellsAfter",snapshot.tile_cells.size()}}},
        {"tilemapEarlyVisibility",{{"applied",snapshot.tilemap_early_visibility_applied},{"chunksResolved",snapshot.tilemap_chunks_resolved},
            {"chunksSkippedBeforeResolution",snapshot.tilemap_chunks_skipped_before_resolution},
            {"cellsSkippedBeforeResolution",snapshot.tilemap_cells_skipped_before_resolution},
            {"stage","compiled-chunk-before-sprite-frame-resolution"}}},
        {"tilemapBakeCache",{{"applied",snapshot.tilemap_bake_cache_applied},{"hits",snapshot.tilemap_bake_cache_hits},
            {"rebuilds",snapshot.tilemap_bake_cache_rebuilds},{"evictions",snapshot.tilemap_bake_cache_evictions},
            {"cachedChunks",snapshot.tilemap_bake_cached_chunks},{"retainedOffscreenChunks",snapshot.tilemap_bake_retained_offscreen_chunks},
            {"offscreenRetentionFrames",120},{"invalidation","chunk-content/sprite-frame/entity-transform/render-visibility"}}},
        {"spriteSortPolicy",snapshot.sprite_sort_policy},{"instances", nlohmann::json::array()},
        {"localLights",nlohmann::json::array()},
        {"sprites",nlohmann::json::array()},
        {"tilemaps",nlohmann::json::array()},
        {"tileChunkRanges",nlohmann::json::array()},
        {"tileCells",nlohmann::json::array()},
        {"vfxParticles", nlohmann::json::array()}};
    out["camera"] = nullptr;
    if (snapshot.camera) out["camera"] = {{"entityId", snapshot.camera->entity_id}, {"position", snapshot.camera->position},
        {"target", snapshot.camera->target}, {"verticalFovDegrees", snapshot.camera->vertical_fov_degrees},
        {"nearClip", snapshot.camera->near_clip}, {"farClip", snapshot.camera->far_clip},
        {"projection",snapshot.camera->projection},{"orthographicHeight",snapshot.camera->orthographic_height}};
    out["directionalLight"] = nullptr;
    if (snapshot.directional_light) out["directionalLight"] = {{"entityId", snapshot.directional_light->entity_id},
        {"direction", snapshot.directional_light->direction}, {"color", snapshot.directional_light->color},
        {"intensity", snapshot.directional_light->intensity}, {"ambientIntensity", snapshot.directional_light->ambient_intensity},
        {"castsShadows", snapshot.directional_light->casts_shadows}};
    for(const auto& light:snapshot.local_lights)out["localLights"].push_back({{"entityId",light.entity_id},{"kind",light.kind},
        {"position",light.position},{"direction",light.direction},{"color",light.color},
        {"luminousPowerLumens",light.luminous_power_lumens},{"rangeMeters",light.range_meters},
        {"innerConeDegrees",light.inner_cone_degrees},{"outerConeDegrees",light.outer_cone_degrees},
        {"sourceRadiusMeters",light.source_radius_meters},{"castsShadows",light.casts_shadows}});
    for (const auto& instance : snapshot.instances) out["instances"].push_back({{"entityId", instance.entity_id},
        {"meshAsset", instance.mesh_asset}, {"position", instance.position}, {"rotationQuaternion",instance.rotation}, {"scale", instance.scale}, {"visible", instance.visible},
        {"castsShadows", instance.casts_shadows}, {"receivesShadows", instance.receives_shadows},
        {"materialOverride", instance.material_override}, {"skinningJointCount", instance.skinning_matrices.size()},
        {"material", {{"baseColor", instance.material.base_color}, {"metallic", instance.material.metallic},
            {"roughness", instance.material.roughness}, {"baseColorTexture", instance.material.base_color_texture},
            {"emissiveColor", instance.material.emissive_color}, {"emissiveIntensity", instance.material.emissive_intensity}}},
        {"vfx", instance.vfx_particle ? nlohmann::json{{"particleId", instance.vfx_particle_id},
            {"emitterId", instance.vfx_emitter_id}, {"graphId", instance.vfx_graph_id}} : nlohmann::json(nullptr)}});
    for(const auto& sprite:snapshot.sprites) out["sprites"].push_back({{"entityId",sprite.entity_id},
        {"spriteAsset",sprite.sprite_asset},{"clipId",sprite.clip_id},{"frameId",sprite.frame_id},
        {"textureAsset",sprite.texture_asset},{"textureSize",sprite.texture_size},{"pixelRect",sprite.pixel_rect},
        {"uvRect",sprite.uv_rect},{"sourceSize",sprite.source_size},{"trimOffset",sprite.trim_offset},
        {"pivot",sprite.pivot},{"pixelsPerUnit",sprite.pixels_per_unit},{"position",sprite.position},
        {"scale",sprite.scale},{"rotationQuaternion",sprite.rotation},{"flipX",sprite.flip_x},{"flipY",sprite.flip_y},
        {"visible",sprite.visible},{"sortingLayer",sprite.sorting_layer},{"sortingOrder",sprite.sorting_order},
        {"sampling",sprite.sampling},{"alphaMode",sprite.alpha_mode},{"material",{
            {"normalTextureAsset",sprite.normal_texture_asset},{"emissiveMaskTextureAsset",sprite.emissive_mask_texture_asset},
            {"depthTextureAsset",sprite.depth_texture_asset},{"normalStrength",sprite.normal_strength},
            {"emissiveColor",sprite.emissive_color},{"emissiveIntensity",sprite.emissive_intensity},{"depthBias",sprite.depth_bias},
            {"shadingModel",sprite.shading_model},{"metallic",sprite.metallic},{"roughness",sprite.roughness},
            {"receivesShadows",sprite.receives_shadows},{"castsShadows",sprite.casts_shadows}}}});
    for(const auto& tilemap:snapshot.tilemaps)out["tilemaps"].push_back({{"entityId",tilemap.entity_id},{"tilemapAsset",tilemap.tilemap_asset},
        {"position",tilemap.position},{"scale",tilemap.scale},{"rotationQuaternion",tilemap.rotation},
        {"visible",tilemap.visible},{"collisionEnabled",tilemap.collision_enabled},{"resolved",tilemap.resolved},
        {"paletteAsset",tilemap.palette_asset},{"layerCount",tilemap.layer_count},{"chunkCount",tilemap.chunk_count},{"cellCount",tilemap.cell_count},
        {"resolvedCellCount",tilemap.resolved_cell_count},{"compiledChunkCount",tilemap.compiled_chunk_count},
        {"compilationRevision",tilemap.compilation_revision},{"visibleChunkCount",tilemap.visible_chunk_count},
        {"culledChunkCount",tilemap.culled_chunk_count},{"visibleCellCount",tilemap.visible_cell_count},
        {"cellsTruncated",tilemap.cells_truncated},{"earlyVisibility",{{"applied",tilemap.early_visibility_applied},
            {"chunksResolved",tilemap.chunks_resolved},{"chunksSkippedBeforeResolution",tilemap.chunks_skipped_before_resolution},
            {"cellsSkippedBeforeResolution",tilemap.cells_skipped_before_resolution}}}});
    for(const auto& range:snapshot.tile_chunk_ranges)out["tileChunkRanges"].push_back({{"key",range.key},{"entityId",range.entity_id},
        {"layerId",range.layer_id},{"chunk",{range.chunk_x,range.chunk_y}},{"contentFingerprint",range.content_fingerprint},
        {"firstCell",range.first_cell},{"cellCount",range.cell_count},{"layout","contiguous-render-cell-range"}});
    for(const auto& cell:snapshot.tile_cells)out["tileCells"].push_back({{"stableId",cell.stable_id},{"entityId",cell.entity_id},
        {"tilemapAsset",cell.tilemap_asset},{"layerId",cell.layer_id},{"cell",{cell.cell_x,cell.cell_y}},
        {"chunk",{{"position",{cell.chunk_x,cell.chunk_y}},{"contentFingerprint",cell.chunk_content_fingerprint}}},
        {"tileId",cell.tile_id},{"autotile",{{"group",cell.autotile_group},{"neighborMask",cell.autotile_mask},
            {"neighborBits",{{"north",1},{"east",2},{"south",4},{"west",8}}}}},
        {"frameId",cell.frame_id},{"textureAsset",cell.texture_asset},{"uvRect",cell.uv_rect},{"localRect",cell.local_rect},
        {"position",cell.position},{"scale",cell.scale},{"rotationQuaternion",cell.rotation},{"flipX",cell.flip_x},{"flipY",cell.flip_y},
        {"visible",cell.visible},{"sortingLayer",cell.sorting_layer},{"sortingOrder",cell.sorting_order},{"sampling",cell.sampling},
        {"alphaMode",cell.alpha_mode},{"material",{{"normalTextureAsset",cell.normal_texture_asset},
            {"emissiveMaskTextureAsset",cell.emissive_mask_texture_asset},{"depthTextureAsset",cell.depth_texture_asset},
            {"normalStrength",cell.normal_strength},{"emissiveColor",cell.emissive_color},
            {"emissiveIntensity",cell.emissive_intensity},{"depthBias",cell.depth_bias},
            {"shadingModel",cell.shading_model},{"metallic",cell.metallic},{"roughness",cell.roughness},
            {"receivesShadows",cell.receives_shadows},{"castsShadows",cell.casts_shadows}}}});
    for (const auto& particle : snapshot.vfx_particles) out["vfxParticles"].push_back({
        {"particleId", particle.particle_id}, {"emitterId", particle.emitter_id}, {"graphId", particle.graph_id},
        {"position", particle.position}, {"velocity", particle.velocity}, {"color", particle.color},
        {"age", particle.age}, {"lifetime", particle.lifetime}, {"size", particle.size},
        {"blendMode",particle.blend_mode},{"spritePolicy",{{"pixelAlignment",particle.pixel_alignment},
            {"sizeQuantization",particle.size_quantization},{"sampling",particle.sampling}}},
        {"cameraDistanceSquared",particle.camera_distance_squared},
        {"curve",{{"sizeStart",particle.size_start},{"sizeEnd",particle.size_end},
            {"colorStart",particle.color_start},{"colorEnd",particle.color_end}}},
        {"simulation",{{"gravity",particle.gravity},{"drag",particle.drag}}},
        {"spawnProvenance",{{"seed",particle.spawn_seed},{"particleIndex",particle.spawn_index},
            {"origin",particle.spawn_origin},{"lifetimeRange",{particle.lifetime_min,particle.lifetime_max}},
            {"speedRange",{particle.speed_min,particle.speed_max}}}}});
    return out.dump();
}

std::string tilemap_pressure_report_json(const std::uint32_t chunk_columns,const std::uint32_t chunk_rows,
    const std::uint32_t chunk_size,const std::uint32_t visible_chunk_radius,const std::uint32_t occupied_cells_per_chunk) {
    const auto total_chunks=static_cast<std::uint64_t>(chunk_columns)*chunk_rows;
    const auto cells_per_chunk=static_cast<std::uint64_t>(chunk_size)*chunk_size;
    const auto occupied_per_chunk=occupied_cells_per_chunk==0?cells_per_chunk:occupied_cells_per_chunk;
    const auto addressable_cells=total_chunks*cells_per_chunk;
    const auto total_cells=total_chunks*occupied_per_chunk;
    if(chunk_columns==0||chunk_rows==0||chunk_columns>64||chunk_rows>64||chunk_size<4||chunk_size>32||
       visible_chunk_radius>32||occupied_per_chunk>cells_per_chunk||total_cells>262144) return nlohmann::json{{"schemaVersion","noemancer.tilemap-pressure/0.3"},
        {"valid",false},{"code","tilemap.pressure.invalid-budget"},{"limits",{{"maximumChunksPerAxis",64},{"chunkSize",{4,32}},
        {"maximumOccupiedCells",262144},{"maximumVisibleChunkRadius",32},{"occupiedCellsPerChunk","0 means dense; otherwise [1, chunkSize^2]"}}}}.dump();
    RenderWorldSnapshot snapshot;const float center_x=static_cast<float>(chunk_columns*chunk_size)*0.5F;
    const float center_y=static_cast<float>(chunk_rows*chunk_size)*0.5F;
    snapshot.camera=RenderCameraSnapshot{.entity_id="camera.pressure",.position={center_x,center_y,10.0F},.target={center_x,center_y,0.0F},
        .near_clip=0.1F,.far_clip=100.0F,.projection="orthographic",
        .orthographic_height=static_cast<float>((visible_chunk_radius*2+1)*chunk_size)};
    snapshot.tilemaps.push_back(RenderTilemapSnapshot{.entity_id="entity.tilemap.pressure",.tilemap_asset="tilemap.pressure",
        .resolved=true,.palette_asset="palette.pressure",.layer_count=1,.chunk_count=static_cast<std::size_t>(total_chunks),
        .cell_count=static_cast<std::size_t>(total_cells),.resolved_cell_count=static_cast<std::size_t>(total_cells),
        .compiled_chunk_count=static_cast<std::size_t>(total_chunks),.compilation_revision=1,
        .visible_chunk_count=static_cast<std::size_t>(total_chunks),.visible_cell_count=static_cast<std::size_t>(total_cells)});
    snapshot.tile_cells.reserve(static_cast<std::size_t>(total_cells));
    for(std::uint32_t chunk_y=0;chunk_y<chunk_rows;++chunk_y)for(std::uint32_t chunk_x=0;chunk_x<chunk_columns;++chunk_x)
        for(std::uint32_t occupied_index=0;occupied_index<occupied_per_chunk;++occupied_index) {
            const auto local_x=occupied_index%chunk_size,local_y=occupied_index/chunk_size;
            const auto x=static_cast<std::int32_t>(chunk_x*chunk_size+local_x),y=static_cast<std::int32_t>(chunk_y*chunk_size+local_y);
            RenderTileCellSnapshot cell;cell.stable_id="pressure/"+std::to_string(chunk_x)+","+std::to_string(chunk_y)+"/"+std::to_string(local_x)+","+std::to_string(local_y);
            cell.entity_id="entity.tilemap.pressure";cell.tilemap_asset="tilemap.pressure";cell.layer_id="ground";cell.cell_x=x;cell.cell_y=y;
            cell.chunk_x=static_cast<std::int32_t>(chunk_x);cell.chunk_y=static_cast<std::int32_t>(chunk_y);cell.tile_id="ground";
            cell.frame_id="ground.0";cell.texture_asset="texture.pressure";cell.local_rect={static_cast<float>(x),static_cast<float>(y+1),static_cast<float>(x+1),static_cast<float>(y)};
            snapshot.tile_cells.push_back(std::move(cell));
        }
    RenderWorldExtractor::cull_tilemap_chunks(snapshot,1600,900);
    constexpr std::size_t batch_capacity=16;const auto visible_cells=snapshot.tile_cells.size();
    const auto estimated_draws=(visible_cells+batch_capacity-1)/batch_capacity;
    const auto reserved_per_chunk=std::bit_ceil(static_cast<std::size_t>(occupied_per_chunk));
    StableRangeAllocator ranges(static_cast<std::size_t>(total_chunks)*reserved_per_chunk*2U);
    std::unordered_map<std::string,std::size_t> original_offsets;
    for(std::uint64_t chunk=0;chunk<total_chunks;++chunk) {
        auto key="chunk/"+std::to_string(chunk);const auto allocation=ranges.acquire(key,static_cast<std::size_t>(occupied_per_chunk),1);
        original_offsets.insert_or_assign(std::move(key),allocation.first);
    }
    for(std::uint64_t chunk=1;chunk<total_chunks;chunk+=2U)
        static_cast<void>(ranges.acquire("chunk/"+std::to_string(chunk),static_cast<std::size_t>(occupied_per_chunk),2));
    const auto evicted=ranges.sweep(122,120);std::size_t stable_offsets=0;
    for(std::uint64_t chunk=1;chunk<total_chunks;chunk+=2U) {
        const auto allocation=ranges.acquire("chunk/"+std::to_string(chunk),static_cast<std::size_t>(occupied_per_chunk),123);
        if(allocation.first==original_offsets.at("chunk/"+std::to_string(chunk)))++stable_offsets;
    }
    std::size_t replacements=0;
    for(std::uint64_t chunk=0;chunk<total_chunks;chunk+=2U)
        if(ranges.acquire("replacement/"+std::to_string(chunk),static_cast<std::size_t>(occupied_per_chunk),123).valid)++replacements;
    const auto residency=ranges.statistics();
    return nlohmann::json{{"schemaVersion","noemancer.tilemap-pressure/0.3"},{"valid",true},{"code","ok"},
        {"workload",{{"chunkColumns",chunk_columns},{"chunkRows",chunk_rows},{"chunkSize",chunk_size},
            {"totalChunks",total_chunks},{"addressableCells",addressable_cells},{"occupiedCells",total_cells},
            {"totalCells",total_cells},{"occupiedCellsPerChunk",occupied_per_chunk},
            {"occupancyRatio",addressable_cells==0?0.0:static_cast<double>(total_cells)/static_cast<double>(addressable_cells)},
            {"visibleChunkRadius",visible_chunk_radius}}},
        {"culling",{{"visibleChunks",snapshot.tilemap_visible_chunk_count},{"culledChunks",snapshot.tilemap_culled_chunk_count},
            {"visibleCells",visible_cells},{"culledCells",total_cells-visible_cells},
            {"cellCullRatio",total_cells==0?0.0:static_cast<double>(total_cells-visible_cells)/static_cast<double>(total_cells)}}},
        {"submissionEstimate",{{"policy","single-texture/stable-order/16-instances-per-draw"},{"maximumInstancesPerDraw",batch_capacity},
            {"drawsWithoutInstancing",visible_cells},{"estimatedDraws",estimated_draws},{"estimatedDrawsSaved",visible_cells-estimated_draws}}},
        {"stableResidency",{{"policy","power-of-two-ranges/best-fit/coalescing/draw-index-indirection"},
            {"initialRanges",total_chunks},{"evictedRanges",evicted},{"replacementRanges",replacements},
            {"retainedRanges",total_chunks/2U},{"retainedOffsetsStable",stable_offsets},
            {"highWaterSlots",residency.high_water},{"liveSlots",residency.live_slots},{"freeSlots",residency.free_slots},
            {"largestFreeRange",residency.largest_free_range},{"rangeMoves",residency.moves},
            {"drawIndexBytes",visible_cells*sizeof(std::uint32_t)},
            {"packedTailWorstCaseBytes",visible_cells*208U}}},
        {"scope","deterministic-render-extraction-and-residency-simulation-not-gpu-timing"}}.dump();
}

} // namespace noemancer
