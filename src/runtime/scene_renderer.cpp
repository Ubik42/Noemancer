#include "runtime/scene_renderer.hpp"
#include "runtime/vfs_asset_reader.hpp"

#include "engine/cascaded_shadow.hpp"
#include "engine/fbx_asset.hpp"
#include "engine/gltf_mesh.hpp"
#include "engine/ibl_cook.hpp"
#include "engine/image_decoder.hpp"
#include "engine/hybrid_pixel_post.hpp"
#include "engine/ktx2_cook_adapter.hpp"
#include "engine/linear_dirty_ranges.hpp"
#include "engine/mesh_runtime_artifact.hpp"
#include "engine/native_raytracing_shading.hpp"
#include "engine/shadow_scalability_policy.hpp"
#include "engine/sprite_asset.hpp"
#include "engine/sprite_atlas_artifact.hpp"
#include "engine/temporal_aa.hpp"
#include "engine/visibility_culling.hpp"
#include "runtime/runtime_texture_upload.hpp"
#include "runtime/gpu_batch_resource_identity.hpp"
#include "runtime/scene_raytracing_bridge.hpp"
#include "runtime/shader_artifact_contract.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <map>
#include <numeric>
#include <nlohmann/json.hpp>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace noemancer {
namespace {

constexpr SDL_GPUTextureFormat color_format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
constexpr SDL_GPUTextureFormat depth_format = SDL_GPU_TEXTUREFORMAT_D32_FLOAT;
constexpr SDL_GPUTextureFormat object_id_format = SDL_GPU_TEXTUREFORMAT_R32_UINT;
constexpr SDL_GPUTextureFormat normal_format = SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT;
constexpr SDL_GPUTextureFormat motion_format = SDL_GPU_TEXTUREFORMAT_R16G16_FLOAT;
constexpr SDL_GPUTextureFormat reactive_mask_format = SDL_GPU_TEXTUREFORMAT_R8_UNORM;
constexpr SDL_GPUTextureFormat history_depth_format = SDL_GPU_TEXTUREFORMAT_R32_FLOAT;
constexpr SDL_GPUTextureFormat ambient_occlusion_format = SDL_GPU_TEXTUREFORMAT_R8_UNORM;
constexpr SDL_GPUTextureFormat exposure_format = SDL_GPU_TEXTUREFORMAT_R16_FLOAT;
constexpr std::uint32_t shadow_size = 2048;
constexpr std::uint32_t shadow_cascade_count = 4;
constexpr std::uint32_t local_shadow_layer_count = 8;
// Renderer status never emits page IDs. Keep the private accounting set and
// every published count bounded even if a malformed Registry contains a very
// large number of atlas manifests.
constexpr std::size_t sprite_atlas_status_max_items = 65536U;

void hash_bytes(std::uint64_t& hash,const void* data,const std::size_t size) {
    constexpr std::uint64_t prime=1099511628211ULL;
    const auto* bytes=static_cast<const std::uint8_t*>(data);
    for(std::size_t index=0;index<size;++index){hash^=bytes[index];hash*=prime;}
}

template<class T>
void hash_value(std::uint64_t& hash,const T& value) { hash_bytes(hash,&value,sizeof(value)); }

void hash_string(std::uint64_t& hash,const std::string& value) { hash_bytes(hash,value.data(),value.size()); }

std::string visibility_index_set_hash(const std::vector<std::vector<std::uint32_t>>& batches) {
    std::uint64_t hash=14695981039346656037ULL;
    const auto append_le=[&](const std::uint64_t value,const std::size_t bytes) {
        for(std::size_t offset=0;offset<bytes;++offset) {
            const auto byte=static_cast<std::uint8_t>((value>>(offset*8U))&0xffU);
            hash_bytes(hash,&byte,1U);
        }
    };
    for(std::size_t batch_index=0;batch_index<batches.size();++batch_index) {
        append_le(static_cast<std::uint64_t>(batch_index),8U);
        append_le(static_cast<std::uint64_t>(batches[batch_index].size()),8U);
        for(const auto candidate:batches[batch_index])append_le(candidate,4U);
    }
    std::ostringstream output;
    output<<"fnv1a64:"<<std::hex<<std::setfill('0')<<std::setw(16)<<hash;
    return output.str();
}

struct Vertex {
    float position[3];
    float normal[3];
    float texcoord[2];
    float tangent[4]{1.0F, 0.0F, 0.0F, 1.0F};
    float joints[4]{};
    float weights[4]{1.0F, 0.0F, 0.0F, 0.0F};
};
struct Vec3 { float x; float y; float z; };
struct Mat4 { std::array<float, 16> value{}; };
Mat4 identity();

struct alignas(16) SkyAtmosphereGpuData final {
    Mat4 inverse_view_projection;
    std::array<float,4> camera_position{};
    std::array<float,4> camera_forward{};
    std::array<float,4> planet_center{};
    std::array<float,4> planet_radii{};
    std::array<float,4> sun_direction_intensity{};
    std::array<float,4> sun_color_exposure{};
    std::array<float,4> rayleigh_scattering_scale_height{};
    std::array<float,4> mie_scattering_scale_height{};
    std::array<float,4> mie_absorption_phase_g{};
    std::array<float,4> ground_albedo_intensity{};
    std::array<float,4> fallback_sky{};
    std::array<std::uint32_t,4> quality{};
    std::array<float,4> viewport_and_time{};
};
static_assert(sizeof(SkyAtmosphereGpuData)==272,
    "SkyAtmosphereGpuData must match sky_atmosphere.frag.hlsl b0");

struct alignas(16) SkyAtmosphereLutGpuData final {
    std::array<float,4> planet_parameters{};
    std::array<float,4> density_parameters{};
    std::array<float,4> ground_albedo{};
    std::array<float,4> rayleigh_scattering{};
    std::array<float,4> rayleigh_absorption{};
    std::array<float,4> mie_scattering{};
    std::array<float,4> mie_absorption{};
    std::array<float,4> ozone_absorption{};
    std::array<float,4> sun_direction{};
    std::array<float,4> sun_irradiance{};
    std::array<float,4> target_parameters{};
    std::array<std::uint32_t,4> quality{};
};
static_assert(sizeof(SkyAtmosphereLutGpuData)==192,
    "SkyAtmosphereLutGpuData must match atmosphere compute b0");

struct alignas(16) SkyAtmosphereCameraVolumeGpuData final {
    std::array<float,4> planet_parameters{};
    std::array<float,4> density_parameters{};
    std::array<float,4> ground_albedo{};
    std::array<float,4> rayleigh_scattering{};
    std::array<float,4> rayleigh_absorption{};
    std::array<float,4> mie_scattering{};
    std::array<float,4> mie_absorption{};
    std::array<float,4> ozone_absorption{};
    std::array<float,4> sun_direction{};
    std::array<float,4> sun_irradiance{};
    std::array<float,4> target_parameters{};
    std::array<float,4> depth_parameters{};
    std::array<float,4> camera_position{};
    std::array<float,4> camera_right{};
    std::array<float,4> camera_up{};
    std::array<float,4> camera_forward{};
    std::array<float,4> planet_center{};
    std::array<std::uint32_t,4> quality{};
};
static_assert(sizeof(SkyAtmosphereCameraVolumeGpuData)==288,
    "SkyAtmosphereCameraVolumeGpuData must match camera-volume compute b0");

struct alignas(16) AerialPerspectiveGpuData final {
    Mat4 inverse_view_projection;
    std::array<float,4> camera_position{};
    std::array<float,4> depth_parameters{};
    std::array<float,4> volume_parameters{};
    std::array<float,4> camera_forward{};
    std::array<float,4> projection_parameters{};
};
static_assert(sizeof(AerialPerspectiveGpuData)==144,
    "AerialPerspectiveGpuData must match aerial_perspective.frag.hlsl b0");

struct alignas(16) ObjectData {
    Mat4 model;
    Mat4 view_projection;
    Mat4 light_view_projection;
    std::array<float, 4> color;
    std::array<float, 4> material;
    std::array<float, 4> emissive_normal;
    std::array<float, 4> occlusion_alpha_flags;
    std::array<std::uint32_t, 4> object_identity{};
    Mat4 previous_model;
    Mat4 previous_view_projection;
};

constexpr std::size_t sprite_instance_capacity=131072;
struct alignas(16) GpuSpriteInstance final {
    Mat4 model;
    Mat4 previous_model;
    std::array<float,4> uv_rect{};
    std::array<float,4> local_rect{};
    std::array<std::uint32_t,4> identity_flags{};
    std::array<float,4> material_parameters{};
    std::array<float,4> emissive_color{};
    std::array<float,4> surface_parameters{};
};
static_assert(sizeof(GpuSpriteInstance)==224,"GpuSpriteInstance must match sprite.vert.hlsl SpriteInstance");
struct alignas(16) SpriteDrawData final {
    Mat4 view_projection;
    Mat4 previous_view_projection;
    std::array<std::uint32_t,4> draw_metadata{};
};
static_assert(sizeof(SpriteDrawData)==144,"SpriteDrawData must match sprite.vert.hlsl b0");

struct alignas(16) LightingData {
    std::array<float, 4> direction_intensity;
    std::array<float, 4> ambient_bias;
    std::array<float, 4> color;
    std::array<float, 4> camera_position;
    std::array<float, 4> camera_forward;
    std::array<float, 4> cascade_splits;
    std::array<Mat4, shadow_cascade_count> cascade_view_projections;
    std::array<float, 4> shadow_parameters;
    std::array<float, 4> cluster_dimensions;
    std::array<float, 4> cluster_depth;
    std::array<float, 4> render_dimensions;
    std::array<Mat4, local_shadow_layer_count> local_shadow_view_projections;
    std::array<float, 4> local_shadow_parameters;
};

struct alignas(16) GpuLocalLight final {
    std::array<float,4> position_range{};
    std::array<float,4> direction_kind{};
    std::array<float,4> color_intensity{};
    std::array<float,4> cone_source{};
};
static_assert(sizeof(GpuLocalLight)==64,"GpuLocalLight must match scene_lit.frag.hlsl");
static_assert(sizeof(ClusterLightRange)==8,"ClusterLightRange must match uint2 cluster header");

struct alignas(16) SkinningData {
    std::array<Mat4, SkeletalPose::maximum_joints> joints{};
    std::array<std::uint32_t, 4> metadata{};
};
constexpr std::size_t skinning_palette_bytes=sizeof(Mat4)*SkeletalPose::maximum_joints;
static_assert(skinning_palette_bytes==4096);

constexpr std::size_t maximum_instances_per_draw=16;
struct alignas(16) InstancingData {
    std::array<Mat4,maximum_instances_per_draw> models{};
    std::array<Mat4,maximum_instances_per_draw> previous_models{};
    std::array<std::array<std::uint32_t,4>,maximum_instances_per_draw> object_identities{};
    std::array<std::array<float,4>,maximum_instances_per_draw> colors{};
    std::array<std::array<float,4>,maximum_instances_per_draw> materials{};
    std::array<std::array<float,4>,maximum_instances_per_draw> emissive_normals{};
    std::array<std::array<float,4>,maximum_instances_per_draw> occlusion_alpha_flags{};
    std::array<std::uint32_t,4> metadata{};
};
static_assert(sizeof(InstancingData)==3344, "InstancingData must match scene_lit.vert.hlsl b3");

constexpr std::uint32_t gpu_driven_instance_capacity=16384U;
constexpr std::uint32_t gpu_driven_batch_capacity=1024U;
constexpr std::uint32_t gpu_driven_minimum_batch_size=32U;
struct alignas(16) GpuDrivenInstance final {
    Mat4 model;
    Mat4 previous_model;
    std::array<float,4> color{};
    std::array<float,4> material{};
    std::array<float,4> emissive_normal{};
    std::array<float,4> occlusion_alpha_flags{};
    std::array<std::uint32_t,4> object_identity{};
    std::array<float,4> bounds{};
};
static_assert(sizeof(GpuDrivenInstance)==224,"GpuDrivenInstance must match GPU-driven shaders");
struct alignas(16) GpuDrivenBatch final {
    std::uint32_t candidate_offset{};
    std::uint32_t candidate_count{};
    std::uint32_t visible_offset{};
    std::uint32_t padding{};
};
static_assert(sizeof(GpuDrivenBatch)==16,"GpuDrivenBatch must match gpu_visibility.comp.hlsl");
struct GpuIndexedIndirectCommand final {
    std::uint32_t index_count{};
    std::uint32_t instance_count{};
    std::uint32_t first_index{};
    std::int32_t vertex_offset{};
    std::uint32_t first_instance{};
};
static_assert(sizeof(GpuIndexedIndirectCommand)==20,"SDL indexed indirect command ABI changed");
struct alignas(16) GpuDrivenDrawData final {
    Mat4 view_projection;
    Mat4 previous_view_projection;
    std::array<std::uint32_t,4> metadata{};
};
static_assert(sizeof(GpuDrivenDrawData)==144,"GpuDrivenDrawData must match scene_gpu_driven.vert.hlsl");
struct alignas(16) GpuVisibilityParameters final {
    std::array<std::array<float,4>,6> frustum_planes{};
    std::uint32_t candidate_count{};
    std::uint32_t batch_count{};
    std::array<std::uint32_t,2> padding{};
};
static_assert(sizeof(GpuVisibilityParameters)==112,"GpuVisibilityParameters must match gpu_visibility.comp.hlsl");
struct alignas(16) GpuOcclusionParameters final {
    std::array<std::array<float,4>,6> frustum_planes{};
    Mat4 view_projection;
    std::array<float,4> viewport{};
    std::array<float,4> depth_parameters{};
    std::array<float,4> occlusion_parameters{};
    std::array<std::uint32_t,4> dispatch_parameters{};
};
static_assert(sizeof(GpuOcclusionParameters)==224,"GpuOcclusionParameters must match gpu_occlusion.comp.hlsl");
constexpr std::uint32_t gpu_occlusion_statistic_count=8U;
constexpr std::uint32_t gpu_occlusion_statistics_bytes=
    gpu_occlusion_statistic_count*sizeof(std::uint32_t);

struct alignas(16) ShadowInstancingData {
    std::array<Mat4,maximum_instances_per_draw> models{};
    std::array<std::array<float,4>,maximum_instances_per_draw> colors{};
    std::array<std::array<float,4>,maximum_instances_per_draw> occlusion_alpha_flags{};
    std::array<std::uint32_t,4> metadata{};
};
static_assert(sizeof(ShadowInstancingData)==1552, "ShadowInstancingData must match shadow_depth.vert.hlsl b2");

SkinningData skinning_data(const std::vector<std::array<float, 16>>& source) {
    SkinningData result;
    for (auto& joint : result.joints) joint = identity();
    const auto count = std::min(source.size(), result.joints.size());
    for (std::size_t joint = 0; joint < count; ++joint) result.joints[joint].value = source[joint];
    result.metadata[0] = static_cast<std::uint32_t>(count);
    return result;
}

struct alignas(16) ToneMapSettings {
    float exposure_compensation{1.0F};
    float white_point{1.0F};
    float bloom_strength{0.35F};
    float debug_bypass{};
    std::array<float,4> lift{};
    std::array<float,4> gamma{1.0F,1.0F,1.0F,0.0F};
    std::array<float,4> gain{1.0F,1.0F,1.0F,0.0F};
    float saturation{1.0F};
    float contrast{1.0F};
    float temperature{};
    float tint{};
};
static_assert(sizeof(ToneMapSettings)==80, "ToneMapSettings must match tone_map.frag.hlsl b0");

struct alignas(16) NativeRtCompositeSettings final {
    std::array<std::uint32_t,4> extent_mode{};
    std::array<float,4> clear_color_linear{};
};
static_assert(sizeof(NativeRtCompositeSettings)==32,
    "NativeRtCompositeSettings must match native_rt_composite.frag.hlsl b0");

struct alignas(16) GtaoSettings {
    std::array<float,2> inverse_resolution{};
    float near_clip{0.1F};
    float far_clip{100.0F};
    float radius_pixels{14.0F};
    float intensity{1.35F};
    float bias{0.02F};
    float power{1.25F};
};
static_assert(sizeof(GtaoSettings)==32, "GtaoSettings must match gtao.frag.hlsl b0");

struct alignas(16) GtaoDenoiseSettings {
    std::array<float,2> inverse_resolution{};
    std::array<float,2> direction{};
    float near_clip{0.1F};
    float far_clip{100.0F};
    float depth_sigma{1.5F};
    float normal_power{16.0F};
};
static_assert(sizeof(GtaoDenoiseSettings)==32, "GtaoDenoiseSettings must match ao_denoise.frag.hlsl b0");

struct alignas(16) AutoExposureSettings {
    float minimum_exposure{0.25F};
    float maximum_exposure{4.0F};
    float key_value{0.18F};
    float delta_seconds{1.0F/60.0F};
    float speed_up{3.0F};
    float speed_down{1.0F};
    float history_valid{};
    float padding{};
};
static_assert(sizeof(AutoExposureSettings)==32, "AutoExposureSettings must match auto_exposure.frag.hlsl b0");

struct alignas(16) BloomDownsampleSettings {
    std::array<float,2> inverse_source_resolution{};
    float threshold{1.0F};
    float soft_knee{0.5F};
    float apply_threshold{1.0F};
    std::array<float,3> padding{};
};
static_assert(sizeof(BloomDownsampleSettings)==32,"BloomDownsampleSettings must match bloom_downsample.frag.hlsl b0");

struct alignas(16) BloomUpsampleSettings {
    std::array<float,2> inverse_low_resolution{};
    float scatter{0.7F};
    float padding{};
};
static_assert(sizeof(BloomUpsampleSettings)==16,"BloomUpsampleSettings must match bloom_upsample.frag.hlsl b0");

struct alignas(16) FxaaSettings {
    std::array<float,2> inverse_resolution{};
    float edge_threshold{0.125F};
    float edge_threshold_min{0.0312F};
};

struct alignas(16) TemporalDenoiseSettings final {
    std::array<float,4> resolution_and_history{};
    std::array<float,4> projection_parameters{};
    std::array<float,4> rejection_parameters{};
    std::array<float,4> output_parameters{};
};
static_assert(sizeof(TemporalDenoiseSettings)==64,
    "TemporalDenoiseSettings must match temporal_denoise.frag.hlsl b0");

struct alignas(16) DepthPyramidSeedSettings final {std::array<float,4> depth_parameters{};};
struct alignas(16) DepthPyramidReduceSettings final {std::array<std::uint32_t,4> reduce_parameters{};};
static_assert(sizeof(DepthPyramidSeedSettings)==16&&sizeof(DepthPyramidReduceSettings)==16,
    "Depth-pyramid settings must match compute shader b0");

struct alignas(16) SsrTraceSettings final {
    Mat4 inverse_view_projection;
    Mat4 view_projection;
    std::array<float,4> camera_position{};
    std::array<float,4> camera_forward{};
    std::array<float,4> resolution{};
    std::array<float,4> depth_and_distance{};
    std::array<float,4> quality{};
    std::array<float,4> debug{};
    std::array<float,4> ray_policy{};
    std::array<float,4> edge_policy{};
};
static_assert(sizeof(SsrTraceSettings)==256,"SsrTraceSettings must match ssr_hiz_trace.frag.hlsl b0");

struct alignas(16) SsrTemporalSettings final {
    std::array<float,4> resolution_and_history{};
    std::array<float,4> rejection{};
    std::array<float,4> output{};
    std::array<float,4> normal_rejection{};
};
static_assert(sizeof(SsrTemporalSettings)==64,"SsrTemporalSettings must match ssr_temporal_resolve.frag.hlsl b0");

struct alignas(16) SsrCompositeSettings final {std::array<float,4> composite{};};
static_assert(sizeof(SsrCompositeSettings)==16,"SsrCompositeSettings must match ssr_composite.frag.hlsl b0");

struct alignas(16) SsgiGatherSettings final {
    Mat4 inverse_view_projection;
    Mat4 view_projection;
    std::array<float,4> camera_position{};
    std::array<float,4> camera_forward{};
    std::array<float,4> resolution{};
    std::array<float,4> depth_and_radius{};
    std::array<float,4> quality{};
    std::array<float,4> debug{};
    std::array<float,4> gather_policy{};
};
static_assert(sizeof(SsgiGatherSettings)==240,"SsgiGatherSettings must match ssgi_hiz_gather.frag.hlsl b0");
struct alignas(16) SsgiSpatialSettings final {
    std::array<float,4> resolution_and_filter{};
    std::array<float,4> policy{};
    std::array<float,4> output{};
};
static_assert(sizeof(SsgiSpatialSettings)==48,"SsgiSpatialSettings must match ssgi_spatial_resolve.frag.hlsl b0");
struct alignas(16) SsgiTemporalSettings final {
    std::array<float,4> resolution_and_history{};
    std::array<float,4> rejection{};
    std::array<float,4> output{};
};
static_assert(sizeof(SsgiTemporalSettings)==48,"SsgiTemporalSettings must match ssgi_temporal_resolve.frag.hlsl b0");
struct alignas(16) SsgiCompositeSettings final {std::array<float,4> composite{};};
static_assert(sizeof(SsgiCompositeSettings)==16,"SsgiCompositeSettings must match ssgi_composite.frag.hlsl b0");

struct alignas(16) VfxCameraData {
    Mat4 view_projection;
    std::array<float,4> camera_right;
    std::array<float,4> camera_up;
    float delta_seconds{1.0F/60.0F};
    std::array<float,3> padding{};
    std::array<float,2> render_size{};
    float world_units_per_pixel{};
    std::uint32_t hybrid_pixel_flags{};
};
static_assert(sizeof(VfxCameraData)==128, "VfxCameraData must match vfx_billboard.vert.hlsl b0");

constexpr std::array<Vertex, 40> vertices{{
    {{-1,-1,-1},{0,0,-1},{0,0}}, {{1,-1,-1},{0,0,-1},{1,0}}, {{1,1,-1},{0,0,-1},{1,1}}, {{-1,1,-1},{0,0,-1},{0,1}},
    {{1,-1,1},{0,0,1},{0,0}}, {{-1,-1,1},{0,0,1},{1,0}}, {{-1,1,1},{0,0,1},{1,1}}, {{1,1,1},{0,0,1},{0,1}},
    {{-1,-1,1},{-1,0,0},{0,0}}, {{-1,-1,-1},{-1,0,0},{1,0}}, {{-1,1,-1},{-1,0,0},{1,1}}, {{-1,1,1},{-1,0,0},{0,1}},
    {{1,-1,-1},{1,0,0},{0,0}}, {{1,-1,1},{1,0,0},{1,0}}, {{1,1,1},{1,0,0},{1,1}}, {{1,1,-1},{1,0,0},{0,1}},
    {{-1,1,-1},{0,1,0},{0,0}}, {{1,1,-1},{0,1,0},{1,0}}, {{1,1,1},{0,1,0},{1,1}}, {{-1,1,1},{0,1,0},{0,1}},
    {{-1,-1,1},{0,-1,0},{0,0}}, {{1,-1,1},{0,-1,0},{1,0}}, {{1,-1,-1},{0,-1,0},{1,1}}, {{-1,-1,-1},{0,-1,0},{0,1}},
    {{-7,0,-7},{0,1,0},{0,0}}, {{7,0,-7},{0,1,0},{8,0}}, {{7,0,7},{0,1,0},{8,8}}, {{-7,0,7},{0,1,0},{0,8}},
    {{-0.525731F,0.850651F,0},{-0.525731F,0.850651F,0},{0,0}}, {{0.525731F,0.850651F,0},{0.525731F,0.850651F,0},{1,0}},
    {{-0.525731F,-0.850651F,0},{-0.525731F,-0.850651F,0},{0,1}}, {{0.525731F,-0.850651F,0},{0.525731F,-0.850651F,0},{1,1}},
    {{0,-0.525731F,0.850651F},{0,-0.525731F,0.850651F},{0.25F,0.75F}}, {{0,0.525731F,0.850651F},{0,0.525731F,0.850651F},{0.25F,0.25F}},
    {{0,-0.525731F,-0.850651F},{0,-0.525731F,-0.850651F},{0.75F,0.75F}}, {{0,0.525731F,-0.850651F},{0,0.525731F,-0.850651F},{0.75F,0.25F}},
    {{0.850651F,0,-0.525731F},{0.850651F,0,-0.525731F},{0.875F,0.5F}}, {{0.850651F,0,0.525731F},{0.850651F,0,0.525731F},{0.125F,0.5F}},
    {{-0.850651F,0,-0.525731F},{-0.850651F,0,-0.525731F},{0.625F,0.5F}}, {{-0.850651F,0,0.525731F},{-0.850651F,0,0.525731F},{0.375F,0.5F}}
}};

constexpr std::array<std::uint32_t, 102> indices{{
    0,1,2, 0,2,3, 4,5,6, 4,6,7, 8,9,10, 8,10,11,
    12,13,14, 12,14,15, 16,17,18, 16,18,19, 20,21,22, 20,22,23,
    24,26,25, 24,27,26,
    28,39,33, 28,33,29, 28,29,35, 28,35,38, 28,38,39,
    29,33,37, 33,39,32, 39,38,30, 38,35,34, 35,29,36,
    31,37,32, 31,32,30, 31,30,34, 31,34,36, 31,36,37,
    32,37,33, 30,32,39, 34,30,38, 36,34,35, 37,36,29
}};

constexpr std::uint32_t builtin_base_vertex_count=28U;
constexpr std::uint32_t builtin_base_index_count=42U;
constexpr std::uint32_t builtin_sphere_segments=32U;
constexpr std::uint32_t builtin_sphere_rings=16U;
constexpr std::uint32_t builtin_sphere_first_index=builtin_base_index_count;
constexpr std::uint32_t builtin_sphere_index_count=builtin_sphere_segments*builtin_sphere_rings*6U;

Vec3 operator-(const Vec3 a, const Vec3 b) { return {a.x-b.x, a.y-b.y, a.z-b.z}; }
Vec3 operator+(const Vec3 a, const Vec3 b) { return {a.x+b.x, a.y+b.y, a.z+b.z}; }
Vec3 operator*(const Vec3 value, const float scale) { return {value.x*scale,value.y*scale,value.z*scale}; }
float dot(const Vec3 a, const Vec3 b) { return a.x*b.x + a.y*b.y + a.z*b.z; }
Vec3 cross(const Vec3 a, const Vec3 b) {
    return {a.y*b.z-a.z*b.y, a.z*b.x-a.x*b.z, a.x*b.y-a.y*b.x};
}
Vec3 normalize(const Vec3 value) {
    const float length = std::sqrt(dot(value, value));
    return {value.x/length, value.y/length, value.z/length};
}

float half_to_float(const std::uint16_t value) {
    const std::uint32_t sign = static_cast<std::uint32_t>(value & 0x8000U) << 16U;
    std::uint32_t exponent = (value >> 10U) & 0x1FU;
    std::uint32_t mantissa = value & 0x03FFU;
    std::uint32_t bits{};
    if (exponent == 0) {
        if (mantissa == 0) bits = sign;
        else {
            exponent = 113U;
            while ((mantissa & 0x0400U) == 0) { mantissa <<= 1U; --exponent; }
            bits = sign | (exponent << 23U) | ((mantissa & 0x03FFU) << 13U);
        }
    } else if (exponent == 31U) {
        bits = sign | 0x7F800000U | (mantissa << 13U);
    } else {
        bits = sign | ((exponent + 112U) << 23U) | (mantissa << 13U);
    }
    float result{};
    std::memcpy(&result, &bits, sizeof(result));
    return result;
}

Mat4 identity() {
    Mat4 result{};
    result.value[0] = result.value[5] = result.value[10] = result.value[15] = 1.0F;
    return result;
}

Mat4 render_matrix(const ShadowMat4& source) { return {source.value}; }

Mat4 multiply(const Mat4& left, const Mat4& right) {
    Mat4 result{};
    for (int column = 0; column < 4; ++column)
        for (int row = 0; row < 4; ++row)
            for (int k = 0; k < 4; ++k)
                result.value[column*4+row] += left.value[k*4+row] * right.value[column*4+k];
    return result;
}

Mat4 perspective(const float fov, const float aspect, const float near_plane, const float far_plane) {
    Mat4 result{};
    const float scale = 1.0F / std::tan(fov * 0.5F);
    result.value[0] = scale / aspect;
    result.value[5] = scale;
    result.value[10] = far_plane / (near_plane - far_plane);
    result.value[11] = -1.0F;
    result.value[14] = near_plane * far_plane / (near_plane - far_plane);
    return result;
}

Mat4 orthographic(const float left, const float right, const float bottom, const float top, const float near_plane, const float far_plane) {
    Mat4 result = identity();
    result.value[0] = 2.0F / (right-left);
    result.value[5] = 2.0F / (top-bottom);
    result.value[10] = 1.0F / (near_plane-far_plane);
    result.value[12] = -(right+left)/(right-left);
    result.value[13] = -(top+bottom)/(top-bottom);
    result.value[14] = near_plane/(near_plane-far_plane);
    return result;
}

Mat4 look_at(const Vec3 eye, const Vec3 target, const Vec3 up) {
    const Vec3 forward = normalize(target-eye);
    const Vec3 side = normalize(cross(forward, up));
    const Vec3 corrected_up = cross(side, forward);
    Mat4 result = identity();
    result.value[0]=side.x; result.value[1]=corrected_up.x; result.value[2]=-forward.x;
    result.value[4]=side.y; result.value[5]=corrected_up.y; result.value[6]=-forward.y;
    result.value[8]=side.z; result.value[9]=corrected_up.z; result.value[10]=-forward.z;
    result.value[12]=-dot(side,eye); result.value[13]=-dot(corrected_up,eye); result.value[14]=dot(forward,eye);
    return result;
}

Mat4 model_matrix(const Vec3 translation, const Vec3 scale, const std::array<float,4> rotation) {
    Mat4 result = identity();
    const auto x=rotation[0],y=rotation[1],z=rotation[2],w=rotation[3];
    result.value[0]=(1.0F-2.0F*(y*y+z*z))*scale.x;result.value[1]=(2.0F*(x*y+w*z))*scale.x;result.value[2]=(2.0F*(x*z-w*y))*scale.x;
    result.value[4]=(2.0F*(x*y-w*z))*scale.y;result.value[5]=(1.0F-2.0F*(x*x+z*z))*scale.y;result.value[6]=(2.0F*(y*z+w*x))*scale.y;
    result.value[8]=(2.0F*(x*z+w*y))*scale.z;result.value[9]=(2.0F*(y*z-w*x))*scale.z;result.value[10]=(1.0F-2.0F*(x*x+y*y))*scale.z;
    result.value[12]=translation.x; result.value[13]=translation.y; result.value[14]=translation.z;
    return result;
}

std::vector<Uint8> read_binary(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input) return {};
    const auto size = input.tellg();
    std::vector<Uint8> bytes(static_cast<std::size_t>(size));
    input.seekg(0);
    input.read(reinterpret_cast<char*>(bytes.data()), size);
    return bytes;
}

const ShaderArtifactContract& runtime_shader_artifacts() {
    static const ShaderArtifactContract artifacts(
        default_shader_artifact_root()/"shader-artifact-manifest.json");
    return artifacts;
}

thread_local std::string shader_artifact_failure;

SDL_GPUShader* load_shader(SDL_GPUDevice* device, const char* stem, const SDL_GPUShaderStage stage, const Uint32 samplers, const Uint32 uniforms,
                           const Uint32 storage_buffers=0) {
    const auto formats=SDL_GetGPUShaderFormats(device);
    const bool use_dxil=(formats&SDL_GPU_SHADERFORMAT_DXIL)!=0;
    const bool use_spirv=!use_dxil && (formats&SDL_GPU_SHADERFORMAT_SPIRV)!=0;
    if (!use_dxil && !use_spirv) return nullptr;
    const auto artifact=runtime_shader_artifacts().load(ShaderArtifactRequest{
        .stem=stem,
        .stage=stage==SDL_GPU_SHADERSTAGE_VERTEX?ShaderArtifactStage::vertex:ShaderArtifactStage::fragment,
        .resources={.uniform_buffers=uniforms,.samplers=samplers,.storage_buffers=storage_buffers}},
        use_dxil?ShaderArtifactBackend::dxil:ShaderArtifactBackend::spv);
    if(!artifact.success) {
        shader_artifact_failure=artifact.code+": "+artifact.detail;
        return nullptr;
    }
    SDL_GPUShaderCreateInfo info{};
    info.code = reinterpret_cast<const Uint8*>(artifact.bytes.data()); info.code_size = artifact.bytes.size(); info.entrypoint = artifact.entrypoint.c_str();
    info.format = use_dxil?SDL_GPU_SHADERFORMAT_DXIL:SDL_GPU_SHADERFORMAT_SPIRV; info.stage = stage;
    info.num_samplers = samplers; info.num_uniform_buffers = uniforms; info.num_storage_buffers=storage_buffers;
    return SDL_CreateGPUShader(device, &info);
}

SDL_GPUComputePipeline* load_vfx_compute_pipeline(SDL_GPUDevice* device,const std::string_view shader_name,
                                                   const std::uint32_t readwrite_buffers,
                                                   const std::uint32_t thread_count=64U) {
    const auto formats=SDL_GetGPUShaderFormats(device);
    const bool use_dxil=(formats&SDL_GPU_SHADERFORMAT_DXIL)!=0;
    const bool use_spirv=!use_dxil && (formats&SDL_GPU_SHADERFORMAT_SPIRV)!=0;
    if (!use_dxil && !use_spirv) return nullptr;
    const auto artifact=runtime_shader_artifacts().load(ShaderArtifactRequest{
        .stem=std::string(shader_name),.stage=ShaderArtifactStage::compute,
        .resources={.uniform_buffers=1,.read_write_storage_buffers=readwrite_buffers}},
        use_dxil?ShaderArtifactBackend::dxil:ShaderArtifactBackend::spv);
    if(!artifact.success) {
        shader_artifact_failure=artifact.code+": "+artifact.detail;
        return nullptr;
    }
    SDL_GPUComputePipelineCreateInfo info{};
    info.code=reinterpret_cast<const Uint8*>(artifact.bytes.data()); info.code_size=artifact.bytes.size(); info.entrypoint=artifact.entrypoint.c_str();
    info.format=use_dxil?SDL_GPU_SHADERFORMAT_DXIL:SDL_GPU_SHADERFORMAT_SPIRV;
    info.num_readwrite_storage_buffers=readwrite_buffers; info.num_uniform_buffers=1;
    info.threadcount_x=thread_count; info.threadcount_y=1; info.threadcount_z=1;
    return SDL_CreateGPUComputePipeline(device,&info);
}

SDL_GPUComputePipeline* load_visibility_compute_pipeline(SDL_GPUDevice* device) {
    const auto formats=SDL_GetGPUShaderFormats(device);
    const bool use_dxil=(formats&SDL_GPU_SHADERFORMAT_DXIL)!=0;
    const bool use_spirv=!use_dxil&&(formats&SDL_GPU_SHADERFORMAT_SPIRV)!=0;
    if(!use_dxil&&!use_spirv)return nullptr;
    const auto artifact=runtime_shader_artifacts().load(ShaderArtifactRequest{
        .stem="gpu_visibility.comp",.stage=ShaderArtifactStage::compute,
        .resources={.uniform_buffers=1,.read_only_storage_buffers=2,.read_write_storage_buffers=2}},
        use_dxil?ShaderArtifactBackend::dxil:ShaderArtifactBackend::spv);
    if(!artifact.success) {
        shader_artifact_failure=artifact.code+": "+artifact.detail;
        return nullptr;
    }
    SDL_GPUComputePipelineCreateInfo info{};
    info.code=reinterpret_cast<const Uint8*>(artifact.bytes.data());info.code_size=artifact.bytes.size();info.entrypoint=artifact.entrypoint.c_str();
    info.format=use_dxil?SDL_GPU_SHADERFORMAT_DXIL:SDL_GPU_SHADERFORMAT_SPIRV;
    info.num_readonly_storage_buffers=2;info.num_readwrite_storage_buffers=2;info.num_uniform_buffers=1;
    info.threadcount_x=64;info.threadcount_y=1;info.threadcount_z=1;
    return SDL_CreateGPUComputePipeline(device,&info);
}

constexpr std::uint32_t vfx_gpu_capacity=8192U;
constexpr std::uint32_t vfx_particle_stride=112U;
constexpr std::uint32_t vfx_spawn_identity_stride=16U;
constexpr std::uint32_t vfx_spawn_graph_stride=96U;
constexpr std::uint32_t vfx_counter_bytes=16U;

} // namespace

SDL_GPUComputePipeline* load_atmosphere_compute_pipeline(SDL_GPUDevice* device,
    std::string_view shader_name,std::uint32_t samplers);

SceneRenderer::SceneRenderer(SDL_GPUDevice* device, const AssetRegistry& asset_registry,
                             std::shared_ptr<VirtualFileSystem> virtual_file_system,
                             const AssetVfsCatalog& asset_vfs_catalog,
                             TextureResourceTable& texture_resources, const bool gpu_debug)
    : device_(device), asset_registry_(asset_registry), virtual_file_system_(std::move(virtual_file_system)),
      asset_vfs_catalog_(asset_vfs_catalog), texture_resources_(texture_resources),
      gpu_debug_(gpu_debug), gpu_pass_timestamps_(device), render_graph_(make_forward_render_graph()) {
    gpu_backend_=SDL_GetGPUDeviceDriver(device_);
    sdl_native_device_bridge_=inspect_sdl_gpu_native_device(device_);
    const auto properties=SDL_GetGPUDeviceProperties(device_);
    gpu_device_name_=SDL_GetStringProperty(properties,SDL_PROP_GPU_DEVICE_NAME_STRING,"unknown");
    gpu_driver_name_=SDL_GetStringProperty(properties,SDL_PROP_GPU_DEVICE_DRIVER_NAME_STRING,"unknown");
    gpu_driver_version_=SDL_GetStringProperty(properties,SDL_PROP_GPU_DEVICE_DRIVER_VERSION_STRING,"unknown");
    gpu_driver_info_=SDL_GetStringProperty(properties,SDL_PROP_GPU_DEVICE_DRIVER_INFO_STRING,"");
    const auto formats=SDL_GetGPUShaderFormats(device_);
    shader_artifact_format_=(formats&SDL_GPU_SHADERFORMAT_DXIL)!=0?"DXIL":((formats&SDL_GPU_SHADERFORMAT_SPIRV)!=0?"SPIR-V":"unsupported");
    const auto driver_count=SDL_GetNumGPUDrivers();
    available_gpu_backends_.reserve(driver_count>0?static_cast<std::size_t>(driver_count):0U);
    for (int index=0;index<driver_count;++index) available_gpu_backends_.emplace_back(SDL_GetGPUDriver(index));
}
SceneRenderer::~SceneRenderer() { release(); }

void SceneRenderer::set_native_raytracing_session_enabled(const bool enabled) {
    if (native_raytracing_session_enabled_ == enabled) return;
    native_raytracing_session_enabled_ = enabled;
    render_graph_ = make_forward_render_graph(enabled);
}

void SceneRenderer::set_sky_atmosphere(SkyAtmosphereSettings settings) {
    sky_atmosphere_=std::move(settings);
}

SDL_GPUComputePipeline* load_occlusion_compute_pipeline(SDL_GPUDevice* device) {
    const auto formats=SDL_GetGPUShaderFormats(device);
    const bool use_dxil=(formats&SDL_GPU_SHADERFORMAT_DXIL)!=0;
    const bool use_spirv=!use_dxil&&(formats&SDL_GPU_SHADERFORMAT_SPIRV)!=0;
    if(!use_dxil&&!use_spirv)return nullptr;
    const auto artifact=runtime_shader_artifacts().load(ShaderArtifactRequest{
        .stem="gpu_occlusion.comp",.stage=ShaderArtifactStage::compute,
        .resources={.uniform_buffers=1,.samplers=1,.read_only_storage_buffers=2,
            .read_write_storage_buffers=3}},
        use_dxil?ShaderArtifactBackend::dxil:ShaderArtifactBackend::spv);
    if(!artifact.success) {
        shader_artifact_failure=artifact.code+": "+artifact.detail;
        return nullptr;
    }
    SDL_GPUComputePipelineCreateInfo info{};
    info.code=reinterpret_cast<const Uint8*>(artifact.bytes.data());
    info.code_size=artifact.bytes.size();info.entrypoint=artifact.entrypoint.c_str();
    info.format=use_dxil?SDL_GPU_SHADERFORMAT_DXIL:SDL_GPU_SHADERFORMAT_SPIRV;
    info.num_samplers=1;info.num_readonly_storage_buffers=2;
    info.num_readwrite_storage_buffers=3;info.num_uniform_buffers=1;
    info.threadcount_x=64;info.threadcount_y=1;info.threadcount_z=1;
    auto* pipeline=SDL_CreateGPUComputePipeline(device,&info);
    if(!pipeline)shader_artifact_failure="pipeline-create: "+std::string(SDL_GetError());
    return pipeline;
}

bool SceneRenderer::create_sky_atmosphere_resources() {
    shader_artifact_failure.clear();
    sky_lut_fallback_reason_.clear();
    sky_transmittance_pipeline_=load_atmosphere_compute_pipeline(device_,"sky_atmosphere_transmittance.comp",0);
    sky_multi_scattering_pipeline_=load_atmosphere_compute_pipeline(device_,"sky_atmosphere_multi_scattering.comp",1);
    sky_view_pipeline_=load_atmosphere_compute_pipeline(device_,"sky_atmosphere_sky_view.comp",2);
    sky_camera_volume_pipeline_=load_atmosphere_compute_pipeline(device_,"sky_atmosphere_camera_volume.comp",2);
    if(!sky_transmittance_pipeline_||!sky_multi_scattering_pipeline_||!sky_view_pipeline_||
       !sky_camera_volume_pipeline_) {
        sky_lut_fallback_reason_="Unable to create sky-atmosphere LUT pipelines: "+
            (shader_artifact_failure.empty()?std::string(SDL_GetError()):shader_artifact_failure);
        if(sky_transmittance_pipeline_)SDL_ReleaseGPUComputePipeline(device_,sky_transmittance_pipeline_);
        if(sky_multi_scattering_pipeline_)SDL_ReleaseGPUComputePipeline(device_,sky_multi_scattering_pipeline_);
        if(sky_view_pipeline_)SDL_ReleaseGPUComputePipeline(device_,sky_view_pipeline_);
        if(sky_camera_volume_pipeline_)SDL_ReleaseGPUComputePipeline(device_,sky_camera_volume_pipeline_);
        sky_transmittance_pipeline_=nullptr;sky_multi_scattering_pipeline_=nullptr;
        sky_view_pipeline_=nullptr;sky_camera_volume_pipeline_=nullptr;return true;
    }
    SDL_GPUSamplerCreateInfo sampler{};sampler.min_filter=SDL_GPU_FILTER_LINEAR;
    sampler.mag_filter=SDL_GPU_FILTER_LINEAR;sampler.mipmap_mode=SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
    sampler.address_mode_u=sampler.address_mode_v=sampler.address_mode_w=SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    sky_lut_sampler_=SDL_CreateGPUSampler(device_,&sampler);
    if(!sky_lut_sampler_) {sky_lut_fallback_reason_="Unable to create sky-atmosphere LUT sampler: "+std::string(SDL_GetError());return true;}
    if(!ensure_sky_atmosphere_resources()) {
        sky_lut_fallback_reason_=last_error_;last_error_.clear();return true;
    }
    return true;
}

bool SceneRenderer::ensure_sky_atmosphere_resources() {
    const auto quality=sky_atmosphere_.quality==SkyAtmosphereQuality::off?SkyAtmosphereQuality::low:sky_atmosphere_.quality;
    const auto& budget=sky_atmosphere_quality_budget(quality);
    if(sky_transmittance_lut_&&sky_multi_scattering_lut_&&sky_view_lut_&&sky_camera_volume_lut_&&
       sky_lut_width_==budget.sky_view_width&&sky_lut_height_==budget.sky_view_height&&
       sky_camera_volume_extent_==std::array<std::uint32_t,3>{budget.camera_volume_width,
           budget.camera_volume_height,budget.camera_volume_slices})return true;
    if(sky_transmittance_lut_)SDL_ReleaseGPUTexture(device_,sky_transmittance_lut_);
    if(sky_multi_scattering_lut_)SDL_ReleaseGPUTexture(device_,sky_multi_scattering_lut_);
    if(sky_view_lut_)SDL_ReleaseGPUTexture(device_,sky_view_lut_);
    if(sky_camera_volume_lut_)SDL_ReleaseGPUTexture(device_,sky_camera_volume_lut_);
    sky_transmittance_lut_=nullptr;sky_multi_scattering_lut_=nullptr;sky_view_lut_=nullptr;
    sky_camera_volume_lut_=nullptr;
    const auto create_lut=[&](SDL_GPUTexture*& texture,const std::uint32_t width,
                              const std::uint32_t height,const char* name) {
        SDL_GPUTextureCreateInfo info{};info.type=SDL_GPU_TEXTURETYPE_2D;
        info.format=SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT;
        info.usage=SDL_GPU_TEXTUREUSAGE_SAMPLER|SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_WRITE;
        info.width=width;info.height=height;info.layer_count_or_depth=1;
        info.num_levels=1;info.sample_count=SDL_GPU_SAMPLECOUNT_1;
        texture=SDL_CreateGPUTexture(device_,&info);if(texture)SDL_SetGPUTextureName(device_,texture,name);
        return texture!=nullptr;
    };
    if(!create_lut(sky_transmittance_lut_,budget.transmittance_width,budget.transmittance_height,
            "sky-atmosphere.transmittance")||
       !create_lut(sky_multi_scattering_lut_,budget.multi_scattering_width,budget.multi_scattering_height,
            "sky-atmosphere.multi-scattering")||
       !create_lut(sky_view_lut_,budget.sky_view_width,budget.sky_view_height,"sky-atmosphere.sky-view")) {
        last_error_="Unable to allocate sky-atmosphere LUT textures: "+std::string(SDL_GetError());return false;
    }
    SDL_GPUTextureCreateInfo volume_info{};volume_info.type=SDL_GPU_TEXTURETYPE_3D;
    volume_info.format=SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT;
    volume_info.usage=SDL_GPU_TEXTUREUSAGE_SAMPLER|SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_WRITE;
    volume_info.width=budget.camera_volume_width;volume_info.height=budget.camera_volume_height;
    volume_info.layer_count_or_depth=budget.camera_volume_slices;volume_info.num_levels=1;
    volume_info.sample_count=SDL_GPU_SAMPLECOUNT_1;
    sky_camera_volume_lut_=SDL_CreateGPUTexture(device_,&volume_info);
    if(!sky_camera_volume_lut_) {
        last_error_="Unable to allocate sky-atmosphere camera-volume LUT: "+std::string(SDL_GetError());return false;
    }
    SDL_SetGPUTextureName(device_,sky_camera_volume_lut_,"sky-atmosphere.camera-volume");
    sky_lut_width_=budget.sky_view_width;sky_lut_height_=budget.sky_view_height;
    sky_camera_volume_extent_={budget.camera_volume_width,budget.camera_volume_height,budget.camera_volume_slices};
    sky_medium_lut_valid_=false;sky_lut_valid_=false;sky_medium_lut_identity_.clear();
    sky_lut_identity_.clear();sky_camera_volume_identity_.clear();return true;
}

bool SceneRenderer::dispatch_sky_atmosphere_luts(SDL_GPUCommandBuffer* command,
    const std::array<float,3>& camera_position,const std::array<float,3>& camera_right,
    const std::array<float,3>& camera_up,const std::array<float,3>& camera_forward,
    const float tan_half_fov_y,const float aspect_ratio,const float near_clip,const float far_clip,
    const bool orthographic_projection,const float orthographic_height) {
    if(!command||!sky_transmittance_pipeline_||!sky_multi_scattering_pipeline_||
       !sky_view_pipeline_||!sky_camera_volume_pipeline_||!sky_lut_sampler_||
       !ensure_sky_atmosphere_resources())return false;
    const auto& atmosphere=sky_atmosphere_;const auto& budget=sky_atmosphere_quality_budget(atmosphere.quality);
    auto medium_settings=atmosphere;
    medium_settings.sun_direction={0.0F,1.0F,0.0F};medium_settings.sun_irradiance={1.0F,1.0F,1.0F};
    medium_settings.debug_view=SkyAtmosphereDebugView::final;
    const auto medium_identity=sky_atmosphere_history_reset_identity(medium_settings);
    const auto identity=sky_atmosphere_history_reset_identity(atmosphere);
    const SkyAtmosphereLutGpuData data{
        {atmosphere.planet_radius_m,atmosphere.atmosphere_height_m,
            atmosphere.planet_radius_m+std::max(camera_position[1],1.0F),atmosphere.atmosphere_height_m*2.0F},
        {atmosphere.rayleigh_scale_height_m,atmosphere.mie_scale_height_m,
            atmosphere.ozone_center_height_m,atmosphere.ozone_width_m},
        {atmosphere.ground_albedo[0],atmosphere.ground_albedo[1],atmosphere.ground_albedo[2],1.0F},
        {atmosphere.rayleigh_scattering_per_m[0],atmosphere.rayleigh_scattering_per_m[1],atmosphere.rayleigh_scattering_per_m[2],0},
        {atmosphere.rayleigh_absorption_per_m[0],atmosphere.rayleigh_absorption_per_m[1],atmosphere.rayleigh_absorption_per_m[2],0},
        {atmosphere.mie_scattering_per_m[0],atmosphere.mie_scattering_per_m[1],atmosphere.mie_scattering_per_m[2],0},
        {atmosphere.mie_absorption_per_m[0],atmosphere.mie_absorption_per_m[1],atmosphere.mie_absorption_per_m[2],atmosphere.mie_phase_g},
        {atmosphere.ozone_absorption_per_m[0],atmosphere.ozone_absorption_per_m[1],atmosphere.ozone_absorption_per_m[2],0},
        {atmosphere.sun_direction[0],atmosphere.sun_direction[1],atmosphere.sun_direction[2],atmosphere.sun_angular_radius_rad},
        // The authoring contract stores a normalized spectral tint. Match the
        // scene-light photometric scale here instead of baking exposure into
        // the LUT shader.
        {atmosphere.sun_irradiance[0]*20.0F,atmosphere.sun_irradiance[1]*20.0F,
            atmosphere.sun_irradiance[2]*20.0F,20.0F},
        {static_cast<float>(budget.sky_view_width),static_cast<float>(budget.sky_view_height),0,0},
        {budget.transmittance_samples,budget.multi_scattering_samples,budget.sky_view_samples,0}};
    const auto dispatch=[&](SDL_GPUComputePipeline* pipeline,SDL_GPUTexture* output,
                            const std::span<const SDL_GPUTextureSamplerBinding> inputs,
                            const std::uint32_t width,const std::uint32_t height) {
        SDL_PushGPUComputeUniformData(command,0,&data,sizeof(data));
        const SDL_GPUStorageTextureReadWriteBinding target{output,0,0,false,0,0,0};
        auto* pass=SDL_BeginGPUComputePass(command,&target,1,nullptr,0);if(!pass)return false;
        SDL_BindGPUComputePipeline(pass,pipeline);
        if(!inputs.empty())SDL_BindGPUComputeSamplers(pass,0,inputs.data(),static_cast<Uint32>(inputs.size()));
        SDL_DispatchGPUCompute(pass,(width+7U)/8U,(height+7U)/8U,1);SDL_EndGPUComputePass(pass);return true;
    };
    if(!sky_medium_lut_valid_||medium_identity!=sky_medium_lut_identity_) {
        if(!dispatch(sky_transmittance_pipeline_,sky_transmittance_lut_,{},budget.transmittance_width,budget.transmittance_height))return false;
        const std::array<SDL_GPUTextureSamplerBinding,1> transmittance{{{sky_transmittance_lut_,sky_lut_sampler_}}};
        if(!dispatch(sky_multi_scattering_pipeline_,sky_multi_scattering_lut_,transmittance,
                budget.multi_scattering_width,budget.multi_scattering_height))return false;
        sky_medium_lut_valid_=true;sky_medium_lut_identity_=medium_identity;
        sky_lut_valid_=false;sky_lut_identity_.clear();sky_camera_volume_identity_.clear();
        ++sky_medium_lut_regenerations_;
    }
    if(!sky_lut_valid_||identity!=sky_lut_identity_) {
        const std::array<SDL_GPUTextureSamplerBinding,2> sky_inputs{{
            {sky_transmittance_lut_,sky_lut_sampler_},{sky_multi_scattering_lut_,sky_lut_sampler_}}};
        if(!dispatch(sky_view_pipeline_,sky_view_lut_,sky_inputs,budget.sky_view_width,budget.sky_view_height))return false;
        sky_lut_valid_=true;sky_lut_identity_=identity;sky_camera_volume_identity_.clear();
        ++sky_view_lut_regenerations_;++sky_lut_regenerations_;
    }

    std::uint64_t camera_hash=14695981039346656037ULL;hash_string(camera_hash,identity);
    hash_bytes(camera_hash,camera_position.data(),sizeof(camera_position));
    hash_bytes(camera_hash,camera_right.data(),sizeof(camera_right));hash_bytes(camera_hash,camera_up.data(),sizeof(camera_up));
    hash_bytes(camera_hash,camera_forward.data(),sizeof(camera_forward));hash_value(camera_hash,tan_half_fov_y);
    hash_value(camera_hash,aspect_ratio);hash_value(camera_hash,near_clip);hash_value(camera_hash,far_clip);
    hash_value(camera_hash,orthographic_projection);hash_value(camera_hash,orthographic_height);
    std::ostringstream camera_identity_stream;camera_identity_stream<<std::hex<<camera_hash;
    const auto camera_identity=camera_identity_stream.str();
    if(camera_identity==sky_camera_volume_identity_)return true;

    const float volume_far=std::max(near_clip+0.0001F,
        std::min(far_clip,atmosphere.atmosphere_height_m*2.0F));
    const SkyAtmosphereCameraVolumeGpuData volume_data{
        {atmosphere.planet_radius_m,atmosphere.atmosphere_height_m,0.0F,volume_far},
        {atmosphere.rayleigh_scale_height_m,atmosphere.mie_scale_height_m,
            atmosphere.ozone_center_height_m,atmosphere.ozone_width_m},
        {atmosphere.ground_albedo[0],atmosphere.ground_albedo[1],atmosphere.ground_albedo[2],1.0F},
        {atmosphere.rayleigh_scattering_per_m[0],atmosphere.rayleigh_scattering_per_m[1],atmosphere.rayleigh_scattering_per_m[2],0.0F},
        {atmosphere.rayleigh_absorption_per_m[0],atmosphere.rayleigh_absorption_per_m[1],atmosphere.rayleigh_absorption_per_m[2],0.0F},
        {atmosphere.mie_scattering_per_m[0],atmosphere.mie_scattering_per_m[1],atmosphere.mie_scattering_per_m[2],0.0F},
        {atmosphere.mie_absorption_per_m[0],atmosphere.mie_absorption_per_m[1],atmosphere.mie_absorption_per_m[2],atmosphere.mie_phase_g},
        {atmosphere.ozone_absorption_per_m[0],atmosphere.ozone_absorption_per_m[1],atmosphere.ozone_absorption_per_m[2],0.0F},
        {atmosphere.sun_direction[0],atmosphere.sun_direction[1],atmosphere.sun_direction[2],atmosphere.sun_angular_radius_rad},
        {atmosphere.sun_irradiance[0]*20.0F,atmosphere.sun_irradiance[1]*20.0F,atmosphere.sun_irradiance[2]*20.0F,20.0F},
        {static_cast<float>(budget.camera_volume_width),static_cast<float>(budget.camera_volume_height),
            orthographic_projection?orthographic_height:tan_half_fov_y,aspect_ratio},
        {near_clip,volume_far,2.0F,orthographic_projection?1.0F:0.0F},
        {camera_position[0],camera_position[1],camera_position[2],1.0F},
        {camera_right[0],camera_right[1],camera_right[2],0.0F},
        {camera_up[0],camera_up[1],camera_up[2],0.0F},
        {camera_forward[0],camera_forward[1],camera_forward[2],0.0F},
        {0.0F,-atmosphere.planet_radius_m,0.0F,0.0F},
        {budget.aerial_perspective_samples,0U,0U,0U}};
    SDL_PushGPUComputeUniformData(command,0,&volume_data,sizeof(volume_data));
    const SDL_GPUStorageTextureReadWriteBinding target{sky_camera_volume_lut_,0,0,false,0,0,0};
    auto* pass=SDL_BeginGPUComputePass(command,&target,1,nullptr,0);if(!pass)return false;
    SDL_BindGPUComputePipeline(pass,sky_camera_volume_pipeline_);
    const std::array<SDL_GPUTextureSamplerBinding,2> inputs{{
        {sky_transmittance_lut_,sky_lut_sampler_},{sky_multi_scattering_lut_,sky_lut_sampler_}}};
    SDL_BindGPUComputeSamplers(pass,0,inputs.data(),static_cast<Uint32>(inputs.size()));
    SDL_DispatchGPUCompute(pass,(budget.camera_volume_width+7U)/8U,
        (budget.camera_volume_height+7U)/8U,budget.camera_volume_slices);
    SDL_EndGPUComputePass(pass);sky_camera_volume_identity_=camera_identity;
    ++sky_camera_volume_regenerations_;return true;
}

bool SceneRenderer::initialize() {
    if (!SDL_SetGPUAllowedFramesInFlight(device_,allowed_frames_in_flight_)) {
        last_error_="Unable to configure GPU frames in flight: "+std::string(SDL_GetError());
        return false;
    }
    if (!create_geometry() || !create_material_resources() || !create_sprite_resources() || !create_clustered_lighting_resources() ||
        !create_gpu_driven_resources() ||
        !create_environment_resources() || !create_imported_geometry() || !create_pipelines() || !create_targets(960, 540)) {
        if (last_error_.empty()) last_error_ = SDL_GetError();
        release();
        return false;
    }
    SDL_GPUSamplerCreateInfo sampler_info{};
    // PCF is performed explicitly in the shader. Point sampling keeps each
    // comparison tied to one stored depth value at shadow discontinuities.
    sampler_info.min_filter = SDL_GPU_FILTER_NEAREST;
    sampler_info.mag_filter = SDL_GPU_FILTER_NEAREST;
    sampler_info.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
    sampler_info.address_mode_u = sampler_info.address_mode_v = sampler_info.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    shadow_sampler_ = SDL_CreateGPUSampler(device_, &sampler_info);
    if (!shadow_sampler_) { last_error_ = SDL_GetError(); release(); return false; }
    if(!create_sky_atmosphere_resources()) {release();return false;}
    shader_artifact_failure.clear();
    vfx_compute_pipeline_=load_vfx_compute_pipeline(device_,"vfx_sim.comp",7);
    if (!vfx_compute_pipeline_) { last_error_="Unable to create VFX compute ABI pipeline: "+
        (shader_artifact_failure.empty()?std::string(SDL_GetError()):shader_artifact_failure); release(); return false; }
    vfx_spawn_pipeline_=load_vfx_compute_pipeline(device_,"vfx_spawn.comp",7);
    if (!vfx_spawn_pipeline_) { last_error_="Unable to create VFX spawn pipeline: "+
        (shader_artifact_failure.empty()?std::string(SDL_GetError()):shader_artifact_failure); release(); return false; }
    vfx_group_pipeline_=load_vfx_compute_pipeline(device_,"vfx_group.comp",7);
    if (!vfx_group_pipeline_) { last_error_="Unable to create VFX blend-group pipeline: "+
        (shader_artifact_failure.empty()?std::string(SDL_GetError()):shader_artifact_failure); release(); return false; }
    vfx_sort_alpha_pipeline_=load_vfx_compute_pipeline(device_,"vfx_sort_alpha.comp",3,256U);
    if (!vfx_sort_alpha_pipeline_) { last_error_="Unable to create VFX alpha-sort pipeline: "+
        (shader_artifact_failure.empty()?std::string(SDL_GetError()):shader_artifact_failure); release(); return false; }
    if (!create_vfx_compute_resources()) { release(); return false; }
    return true;
}

bool SceneRenderer::create_gpu_driven_resources() {
    shader_artifact_failure.clear();
    gpu_visibility_pipeline_=load_visibility_compute_pipeline(device_);
    if(!gpu_visibility_pipeline_) {
        if(!shader_artifact_failure.empty()) {
            last_error_="Unable to load GPU visibility Shader Artifact Contract: "+shader_artifact_failure;
            return false;
        }
        return true;
    }
    shader_artifact_failure.clear();
    gpu_occlusion_pipeline_=load_occlusion_compute_pipeline(device_);
    if(!gpu_occlusion_pipeline_) {
        gpu_occlusion_fallback_reason_=shader_artifact_failure.empty()
            ? "gpu-occlusion-pipeline-unavailable"
            : "shader-artifact: "+shader_artifact_failure;
    }
    shader_artifact_failure.clear();
    const auto create_buffer=[&](SDL_GPUBuffer*& buffer,const std::uint32_t size,const SDL_GPUBufferUsageFlags usage,const char* name) {
        SDL_GPUBufferCreateInfo info{};info.usage=usage;info.size=size;buffer=SDL_CreateGPUBuffer(device_,&info);
        if(buffer)SDL_SetGPUBufferName(device_,buffer,name);return buffer!=nullptr;
    };
    const auto compute_read=SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ;
    const auto compute_write=SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_WRITE;
    if(!create_buffer(gpu_driven_instance_buffer_,gpu_driven_instance_capacity*sizeof(GpuDrivenInstance),
            compute_read|SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ,"gpu-driven.instances") ||
       !create_buffer(gpu_driven_batch_buffer_,gpu_driven_batch_capacity*sizeof(GpuDrivenBatch),compute_read,"gpu-driven.batches") ||
       !create_buffer(gpu_driven_visible_index_buffer_,gpu_driven_instance_capacity*sizeof(std::uint32_t),
            compute_write|SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ,"gpu-driven.visible-indices") ||
       !create_buffer(gpu_driven_indirect_buffer_,gpu_driven_batch_capacity*sizeof(GpuIndexedIndirectCommand),
            compute_write|SDL_GPU_BUFFERUSAGE_INDIRECT,"gpu-driven.indirect-commands")) {
        if(gpu_driven_instance_buffer_)SDL_ReleaseGPUBuffer(device_,gpu_driven_instance_buffer_);
        if(gpu_driven_batch_buffer_)SDL_ReleaseGPUBuffer(device_,gpu_driven_batch_buffer_);
        if(gpu_driven_visible_index_buffer_)SDL_ReleaseGPUBuffer(device_,gpu_driven_visible_index_buffer_);
        if(gpu_driven_indirect_buffer_)SDL_ReleaseGPUBuffer(device_,gpu_driven_indirect_buffer_);
        SDL_ReleaseGPUComputePipeline(device_,gpu_visibility_pipeline_);
        if(gpu_occlusion_pipeline_)SDL_ReleaseGPUComputePipeline(device_,gpu_occlusion_pipeline_);
        gpu_driven_instance_buffer_=nullptr;gpu_driven_batch_buffer_=nullptr;gpu_driven_visible_index_buffer_=nullptr;
        gpu_driven_indirect_buffer_=nullptr;gpu_visibility_pipeline_=nullptr;gpu_occlusion_pipeline_=nullptr;return true;
    }
    if(gpu_occlusion_pipeline_ &&
       !create_buffer(gpu_occlusion_statistics_buffer_,gpu_occlusion_statistics_bytes,
            compute_write,"gpu-driven.occlusion-statistics")) {
        SDL_ReleaseGPUComputePipeline(device_,gpu_occlusion_pipeline_);
        gpu_occlusion_pipeline_=nullptr;
        gpu_occlusion_fallback_reason_="statistics-buffer-unavailable";
    }
    constexpr auto upload_size=gpu_driven_instance_capacity*sizeof(GpuDrivenInstance)+
        gpu_driven_batch_capacity*sizeof(GpuDrivenBatch)+gpu_driven_batch_capacity*sizeof(GpuIndexedIndirectCommand)+
        gpu_occlusion_statistics_bytes;
    SDL_GPUTransferBufferCreateInfo upload_info{SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD,static_cast<Uint32>(upload_size),0};
    gpu_driven_upload_buffer_=SDL_CreateGPUTransferBuffer(device_,&upload_info);
    if(!gpu_driven_upload_buffer_) {
        SDL_ReleaseGPUBuffer(device_,gpu_driven_instance_buffer_);SDL_ReleaseGPUBuffer(device_,gpu_driven_batch_buffer_);
        SDL_ReleaseGPUBuffer(device_,gpu_driven_visible_index_buffer_);SDL_ReleaseGPUBuffer(device_,gpu_driven_indirect_buffer_);
        SDL_ReleaseGPUComputePipeline(device_,gpu_visibility_pipeline_);
        if(gpu_occlusion_pipeline_)SDL_ReleaseGPUComputePipeline(device_,gpu_occlusion_pipeline_);
        if(gpu_occlusion_statistics_buffer_)SDL_ReleaseGPUBuffer(device_,gpu_occlusion_statistics_buffer_);
        gpu_driven_instance_buffer_=nullptr;gpu_driven_batch_buffer_=nullptr;gpu_driven_visible_index_buffer_=nullptr;
        gpu_driven_indirect_buffer_=nullptr;gpu_visibility_pipeline_=nullptr;gpu_occlusion_pipeline_=nullptr;
        gpu_occlusion_statistics_buffer_=nullptr;return true;
    }
    return true;
}

bool SceneRenderer::create_vfx_compute_resources() {
    const auto compute_usage=SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ|SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_WRITE;
    const auto create_buffer=[&](SDL_GPUBuffer*& buffer,const std::uint32_t size,const SDL_GPUBufferUsageFlags usage) {
        SDL_GPUBufferCreateInfo info{}; info.usage=usage; info.size=size;
        buffer=SDL_CreateGPUBuffer(device_,&info); return buffer!=nullptr;
    };
    const std::array<std::uint32_t,13> sizes{
        vfx_gpu_capacity*vfx_particle_stride,vfx_gpu_capacity*4U,vfx_gpu_capacity*4U,vfx_gpu_capacity*4U,
        vfx_counter_bytes,vfx_counter_bytes,vfx_counter_bytes,vfx_gpu_capacity*vfx_spawn_identity_stride,
        vfx_gpu_capacity*vfx_spawn_graph_stride,vfx_gpu_capacity*4U,vfx_gpu_capacity*4U,vfx_counter_bytes,vfx_counter_bytes};
    if (!create_buffer(vfx_particle_buffer_,sizes[0],compute_usage|SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ) ||
        !create_buffer(vfx_alive_buffers_[0],sizes[1],compute_usage|SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ) ||
        !create_buffer(vfx_alive_buffers_[1],sizes[2],compute_usage|SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ) ||
        !create_buffer(vfx_dead_buffer_,sizes[3],compute_usage) ||
        !create_buffer(vfx_counter_buffers_[0],sizes[4],compute_usage|SDL_GPU_BUFFERUSAGE_INDIRECT) ||
        !create_buffer(vfx_counter_buffers_[1],sizes[5],compute_usage|SDL_GPU_BUFFERUSAGE_INDIRECT) ||
        !create_buffer(vfx_dead_counter_buffer_,sizes[6],compute_usage) ||
        !create_buffer(vfx_spawn_buffer_,sizes[7],compute_usage) ||
        !create_buffer(vfx_spawn_graph_buffer_,sizes[8],compute_usage) ||
        !create_buffer(vfx_additive_indices_buffer_,sizes[9],compute_usage|SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ) ||
        !create_buffer(vfx_alpha_indices_buffer_,sizes[10],compute_usage|SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ) ||
        !create_buffer(vfx_additive_counter_buffer_,sizes[11],compute_usage|SDL_GPU_BUFFERUSAGE_INDIRECT) ||
        !create_buffer(vfx_alpha_counter_buffer_,sizes[12],compute_usage|SDL_GPU_BUFFERUSAGE_INDIRECT)) {
        last_error_="Unable to allocate VFX compute buffers: "+std::string(SDL_GetError()); return false;
    }
    const std::array<const char*,13> names{"vfx.particles","vfx.alive-a","vfx.alive-b","vfx.dead-list",
        "vfx.lifecycle-indirect-a","vfx.lifecycle-indirect-b","vfx.dead-counter","vfx.spawn-commands",
        "vfx.spawn-graph-parameters","vfx.additive-indices","vfx.alpha-indices","vfx.additive-indirect","vfx.alpha-indirect"};
    const std::array<SDL_GPUBuffer*,13> named_buffers{vfx_particle_buffer_,vfx_alive_buffers_[0],vfx_alive_buffers_[1],
        vfx_dead_buffer_,vfx_counter_buffers_[0],vfx_counter_buffers_[1],vfx_dead_counter_buffer_,vfx_spawn_buffer_,
        vfx_spawn_graph_buffer_,vfx_additive_indices_buffer_,vfx_alpha_indices_buffer_,vfx_additive_counter_buffer_,vfx_alpha_counter_buffer_};
    for (std::size_t index=0;index<named_buffers.size();++index)
        SDL_SetGPUBufferName(device_,named_buffers[index],names[index]);
    const auto total=std::accumulate(sizes.begin(),sizes.end(),0U);
    SDL_GPUTransferBufferCreateInfo transfer_info{SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD,total,0};
    vfx_upload_buffer_=SDL_CreateGPUTransferBuffer(device_,&transfer_info);
    auto* mapped=vfx_upload_buffer_?static_cast<std::byte*>(SDL_MapGPUTransferBuffer(device_,vfx_upload_buffer_,false)):nullptr;
    if (!mapped) { last_error_="Unable to map VFX initialization upload: "+std::string(SDL_GetError()); return false; }
    std::memset(mapped,0,total);
    std::uint32_t offset=sizes[0]+sizes[1]+sizes[2];
    auto* dead_indices=reinterpret_cast<std::uint32_t*>(mapped+offset);
    for (std::uint32_t slot=0;slot<vfx_gpu_capacity;++slot) dead_indices[slot]=slot;
    offset+=sizes[3];
    for (std::uint32_t counter=0;counter<2;++counter) {
        reinterpret_cast<std::uint32_t*>(mapped+offset)[0]=6U;
        offset+=sizes[4+counter];
    }
    reinterpret_cast<std::uint32_t*>(mapped+offset)[0]=vfx_gpu_capacity;
    offset+=sizes[6]+sizes[7]+sizes[8]+sizes[9]+sizes[10];
    for (std::uint32_t counter=0;counter<2;++counter) {
        reinterpret_cast<std::uint32_t*>(mapped+offset)[0]=6U;
        offset+=sizes[11+counter];
    }
    SDL_UnmapGPUTransferBuffer(device_,vfx_upload_buffer_);
    auto* command=SDL_AcquireGPUCommandBuffer(device_); auto* pass=command?SDL_BeginGPUCopyPass(command):nullptr;
    if (!pass) { last_error_="Unable to begin VFX initialization upload: "+std::string(SDL_GetError()); return false; }
    const std::array<SDL_GPUBuffer*,13> buffers{vfx_particle_buffer_,vfx_alive_buffers_[0],vfx_alive_buffers_[1],
        vfx_dead_buffer_,vfx_counter_buffers_[0],vfx_counter_buffers_[1],vfx_dead_counter_buffer_,vfx_spawn_buffer_,
        vfx_spawn_graph_buffer_,vfx_additive_indices_buffer_,vfx_alpha_indices_buffer_,vfx_additive_counter_buffer_,vfx_alpha_counter_buffer_};
    offset=0;
    for (std::size_t index=0;index<buffers.size();++index) {
        SDL_GPUTransferBufferLocation source{vfx_upload_buffer_,offset};
        SDL_GPUBufferRegion destination{buffers[index],0,sizes[index]};
        SDL_UploadToGPUBuffer(pass,&source,&destination,false); offset+=sizes[index];
    }
    SDL_EndGPUCopyPass(pass); const bool submitted=SDL_SubmitGPUCommandBuffer(command);
    if (!submitted) { last_error_="Unable to submit VFX initialization upload: "+std::string(SDL_GetError()); return false; }
    return true;
}

bool SceneRenderer::upload_vfx_compute_state(SDL_GPUCommandBuffer* command,const RenderWorldSnapshot& render_world) {
    if (!command || !vfx_upload_buffer_) return false;
    struct GpuSpawnIdentity final {
        std::array<std::uint32_t,4> value;
    };
    struct GpuSpawnGraph final {
        std::array<float,4> origin_age;
        std::array<float,4> lifetime_speed;
        std::array<float,4> color_start_size_start;
        std::array<float,4> color_end_size_end;
        std::array<float,4> gravity_drag;
        std::array<std::uint32_t,4> seed_blend;
    };
    static_assert(sizeof(GpuSpawnIdentity)==vfx_spawn_identity_stride);
    static_assert(sizeof(GpuSpawnGraph)==vfx_spawn_graph_stride);
    std::vector<std::uint64_t> particle_ids;
    particle_ids.reserve(render_world.vfx_particles.size());
    for (const auto& particle : render_world.vfx_particles) particle_ids.push_back(particle.particle_id);
    const auto residency = vfx_gpu_residency_.synchronize(particle_ids);

    auto* mapped=static_cast<std::byte*>(SDL_MapGPUTransferBuffer(device_,vfx_upload_buffer_,true));
    if (!mapped) { last_error_="Unable to map VFX frame upload: "+std::string(SDL_GetError()); return false; }
    auto* identities=reinterpret_cast<GpuSpawnIdentity*>(mapped);
    constexpr std::uint32_t graph_upload_offset=vfx_gpu_capacity*vfx_spawn_identity_stride;
    auto* graphs=reinterpret_cast<GpuSpawnGraph*>(mapped+graph_upload_offset);
    std::unordered_map<std::uint64_t,std::uint32_t> graph_by_emitter;
    graph_by_emitter.reserve(residency.uploads.size());
    std::uint32_t graph_count{};
    for (std::size_t upload_index=0;upload_index<residency.uploads.size();++upload_index) {
        const auto& upload=residency.uploads[upload_index];
        const auto& source=render_world.vfx_particles[upload.source_index];
        auto [found,inserted]=graph_by_emitter.emplace(source.emitter_id,graph_count);
        if (inserted) {
            const auto alpha_start=static_cast<std::uint32_t>(std::lround(std::clamp(source.color_start[3],0.0F,1.0F)*65535.0F));
            const auto alpha_end=static_cast<std::uint32_t>(std::lround(std::clamp(source.color_end[3],0.0F,1.0F)*65535.0F));
            const std::uint32_t render_policy_flags=(source.blend_mode=="additive"?0U:1U)|
                (source.pixel_alignment=="profile"?2U:0U)|
                (source.size_quantization=="profile"?4U:0U)|
                (source.sampling=="profile"?8U:0U);
            graphs[graph_count]={{source.spawn_origin[0],source.spawn_origin[1],source.spawn_origin[2],source.age},
                {source.lifetime_min,source.lifetime_max,source.speed_min,source.speed_max},
                {source.color_start[0],source.color_start[1],source.color_start[2],source.size_start},
                {source.color_end[0],source.color_end[1],source.color_end[2],source.size_end},
                {source.gravity[0],source.gravity[1],source.gravity[2],source.drag},
                {static_cast<std::uint32_t>(source.spawn_seed),static_cast<std::uint32_t>(source.spawn_seed>>32U),
                 render_policy_flags,alpha_start|(alpha_end<<16U)}};
            ++graph_count;
        }
        identities[upload_index]={{static_cast<std::uint32_t>(source.particle_id),
            static_cast<std::uint32_t>(source.particle_id>>32U),source.spawn_index,found->second}};
    }
    constexpr std::uint32_t counter_upload_offset=graph_upload_offset+vfx_gpu_capacity*vfx_spawn_graph_stride;
    auto* counters=reinterpret_cast<std::uint32_t*>(mapped+counter_upload_offset);
    counters[0]=6U; counters[1]=0U; counters[2]=0U; counters[3]=0U;
    counters[4]=6U; counters[5]=0U; counters[6]=0U; counters[7]=0U;
    counters[8]=6U; counters[9]=0U; counters[10]=0U; counters[11]=0U;
    SDL_UnmapGPUTransferBuffer(device_,vfx_upload_buffer_);
    auto* pass=SDL_BeginGPUCopyPass(command);
    if (!pass) { last_error_="Unable to begin VFX frame upload: "+std::string(SDL_GetError()); return false; }
    if (!residency.uploads.empty()) {
        SDL_GPUTransferBufferLocation source{vfx_upload_buffer_,0};
        SDL_GPUBufferRegion destination{vfx_spawn_buffer_,0,
            static_cast<Uint32>(residency.uploads.size()*vfx_spawn_identity_stride)};
        SDL_UploadToGPUBuffer(pass,&source,&destination,false);
        source.offset=graph_upload_offset;
        destination={vfx_spawn_graph_buffer_,0,graph_count*vfx_spawn_graph_stride};
        SDL_UploadToGPUBuffer(pass,&source,&destination,false);
    }
    const auto output_index=1U-vfx_alive_buffer_index_;
    SDL_GPUTransferBufferLocation counter_source{vfx_upload_buffer_,counter_upload_offset};
    SDL_GPUBufferRegion counter_destination{vfx_counter_buffers_[output_index],0,vfx_counter_bytes};
    SDL_UploadToGPUBuffer(pass,&counter_source,&counter_destination,false);
    counter_source.offset+=vfx_counter_bytes;
    counter_destination={vfx_additive_counter_buffer_,0,vfx_counter_bytes};
    SDL_UploadToGPUBuffer(pass,&counter_source,&counter_destination,false);
    counter_source.offset+=vfx_counter_bytes;
    counter_destination={vfx_alpha_counter_buffer_,0,vfx_counter_bytes};
    SDL_UploadToGPUBuffer(pass,&counter_source,&counter_destination,false);
    SDL_EndGPUCopyPass(pass);
    ++vfx_state_uploads_;
    vfx_particles_uploaded_=residency.uploads.size();
    vfx_spawn_graph_commands_uploaded_=graph_count;
    vfx_dynamic_attributes_uploaded_=0;
    vfx_alive_input_count_=residency.resident;
    vfx_resident_particles_=residency.resident;
    vfx_slots_reclaimed_=residency.reclaimed;
    vfx_particles_dropped_=residency.dropped;
    vfx_expected_additive_particles_=static_cast<std::size_t>(std::ranges::count_if(render_world.vfx_particles,
        [](const auto& particle){ return particle.blend_mode=="additive"; }));
    vfx_expected_alpha_particles_=render_world.vfx_particles.size()-vfx_expected_additive_particles_;
    vfx_upload_bytes_+=residency.uploads.size()*vfx_spawn_identity_stride+
        static_cast<std::uint64_t>(graph_count)*vfx_spawn_graph_stride+vfx_counter_bytes*3U;
    return true;
}

void SceneRenderer::dispatch_vfx_compute(SDL_GPUCommandBuffer* command) {
    if (!command || !vfx_compute_pipeline_ || !vfx_particle_buffer_ || !vfx_alive_buffers_[0] ||
        !vfx_alive_buffers_[1] || !vfx_dead_buffer_ || !vfx_counter_buffers_[0] ||
        !vfx_counter_buffers_[1] || !vfx_dead_counter_buffer_ || !vfx_spawn_buffer_ || !vfx_spawn_graph_buffer_) return;
    struct alignas(16) SimulationParameters final {
        float delta_seconds;
        std::uint32_t input_count;
        std::uint32_t capacity;
        std::uint32_t padding;
        std::array<float,3> gravity;
        float drag;
    };
    static_assert(sizeof(SimulationParameters)==32);
    const SimulationParameters parameters{1.0F/60.0F,0U,vfx_gpu_capacity,0U,
        {0.0F,-9.81F,0.0F},0.08F};
    SDL_PushGPUDebugGroup(command,"vfx.lifecycle");
    SDL_PushGPUComputeUniformData(command,0,&parameters,sizeof(parameters));
    const auto output_index=1U-vfx_alive_buffer_index_;
    const std::array<SDL_GPUStorageBufferReadWriteBinding,7> bindings{{
        {vfx_particle_buffer_,false,0,0,0},{vfx_alive_buffers_[vfx_alive_buffer_index_],false,0,0,0},
        {vfx_alive_buffers_[output_index],false,0,0,0},{vfx_counter_buffers_[output_index],false,0,0,0},
        {vfx_dead_buffer_,false,0,0,0},{vfx_dead_counter_buffer_,false,0,0,0},
        {vfx_counter_buffers_[vfx_alive_buffer_index_],false,0,0,0}}};
    auto* pass=SDL_BeginGPUComputePass(command,nullptr,0,bindings.data(),static_cast<Uint32>(bindings.size()));
    if (!pass) { last_error_="Unable to begin VFX compute dispatch: "+std::string(SDL_GetError()); SDL_PopGPUDebugGroup(command); return; }
    SDL_BindGPUComputePipeline(pass,vfx_compute_pipeline_);
    vfx_dispatch_groups_=(vfx_gpu_capacity+63U)/64U;
    SDL_DispatchGPUCompute(pass,vfx_dispatch_groups_,1,1);
    SDL_EndGPUComputePass(pass); ++vfx_compute_dispatches_;

    if (vfx_particles_uploaded_==0 || !vfx_spawn_pipeline_) { SDL_PopGPUDebugGroup(command); return; }
    struct alignas(16) SpawnParameters final { std::uint32_t spawn_count; std::uint32_t capacity; std::array<std::uint32_t,2> padding; };
    const SpawnParameters spawn_parameters{static_cast<std::uint32_t>(vfx_particles_uploaded_),vfx_gpu_capacity,{}};
    SDL_PushGPUComputeUniformData(command,0,&spawn_parameters,sizeof(spawn_parameters));
    const std::array<SDL_GPUStorageBufferReadWriteBinding,7> spawn_bindings{{
        {vfx_particle_buffer_,false,0,0,0},{vfx_alive_buffers_[output_index],false,0,0,0},
        {vfx_dead_buffer_,false,0,0,0},{vfx_counter_buffers_[output_index],false,0,0,0},
        {vfx_dead_counter_buffer_,false,0,0,0},{vfx_spawn_buffer_,false,0,0,0},
        {vfx_spawn_graph_buffer_,false,0,0,0}}};
    pass=SDL_BeginGPUComputePass(command,nullptr,0,spawn_bindings.data(),static_cast<Uint32>(spawn_bindings.size()));
    if (!pass) { last_error_="Unable to begin VFX spawn dispatch: "+std::string(SDL_GetError()); SDL_PopGPUDebugGroup(command); return; }
    SDL_BindGPUComputePipeline(pass,vfx_spawn_pipeline_);
    SDL_DispatchGPUCompute(pass,static_cast<Uint32>((vfx_particles_uploaded_+63U)/64U),1,1);
    SDL_EndGPUComputePass(pass); ++vfx_spawn_dispatches_;
    SDL_PopGPUDebugGroup(command);
}

void SceneRenderer::dispatch_vfx_group_sort(SDL_GPUCommandBuffer* command,const std::array<float,3>& camera_position) {
    if (!command || !vfx_group_pipeline_ || !vfx_sort_alpha_pipeline_ || !vfx_particle_buffer_ ||
        !vfx_additive_indices_buffer_ || !vfx_alpha_indices_buffer_ || !vfx_additive_counter_buffer_ ||
        !vfx_alpha_counter_buffer_) return;
    SDL_PushGPUDebugGroup(command,"vfx.blend-group-and-sort");
    const auto output_index=1U-vfx_alive_buffer_index_;
    struct alignas(16) GroupParameters final { std::uint32_t capacity; std::uint32_t mode; std::array<std::uint32_t,2> padding; };
    const std::array<SDL_GPUStorageBufferReadWriteBinding,7> group_bindings{{
        {vfx_particle_buffer_,false,0,0,0},{vfx_alive_buffers_[output_index],false,0,0,0},
        {vfx_counter_buffers_[output_index],false,0,0,0},{vfx_additive_indices_buffer_,false,0,0,0},
        {vfx_alpha_indices_buffer_,false,0,0,0},{vfx_additive_counter_buffer_,false,0,0,0},
        {vfx_alpha_counter_buffer_,false,0,0,0}}};
    const GroupParameters clear_parameters{vfx_gpu_capacity,0U,{}};
    SDL_PushGPUComputeUniformData(command,0,&clear_parameters,sizeof(clear_parameters));
    auto* pass=SDL_BeginGPUComputePass(command,nullptr,0,group_bindings.data(),static_cast<Uint32>(group_bindings.size()));
    if (!pass) { last_error_="Unable to begin VFX blend-group clear dispatch: "+std::string(SDL_GetError()); SDL_PopGPUDebugGroup(command); return; }
    SDL_BindGPUComputePipeline(pass,vfx_group_pipeline_);
    SDL_DispatchGPUCompute(pass,(vfx_gpu_capacity+63U)/64U,1,1);
    SDL_EndGPUComputePass(pass); ++vfx_group_dispatches_;

    const GroupParameters group_parameters{vfx_gpu_capacity,1U,{}};
    SDL_PushGPUComputeUniformData(command,0,&group_parameters,sizeof(group_parameters));
    pass=SDL_BeginGPUComputePass(command,nullptr,0,group_bindings.data(),static_cast<Uint32>(group_bindings.size()));
    if (!pass) { last_error_="Unable to begin VFX blend-group dispatch: "+std::string(SDL_GetError()); SDL_PopGPUDebugGroup(command); return; }
    SDL_BindGPUComputePipeline(pass,vfx_group_pipeline_);
    SDL_DispatchGPUCompute(pass,(vfx_gpu_capacity+63U)/64U,1,1);
    SDL_EndGPUComputePass(pass); ++vfx_group_dispatches_;

    struct alignas(16) SortParameters final {
        std::array<float,3> camera_position;
        std::uint32_t capacity;
        std::uint32_t sequence_length;
        std::uint32_t compare_stride;
        std::uint32_t sort_span;
        std::uint32_t padding;
    };
    static_assert(sizeof(SortParameters)==32);
    const std::array<SDL_GPUStorageBufferReadWriteBinding,3> sort_bindings{{
        {vfx_particle_buffer_,false,0,0,0},{vfx_alpha_indices_buffer_,false,0,0,0},
        {vfx_alpha_counter_buffer_,false,0,0,0}}};
    const auto sort_plan=plan_vfx_gpu_alpha_sort(vfx_expected_alpha_particles_,vfx_gpu_capacity,256U);
    for (const auto& stage:sort_plan.stages) {
        const SortParameters sort_parameters{camera_position,vfx_gpu_capacity,stage.sequence_length,
            stage.compare_stride,sort_plan.span,0U};
        SDL_PushGPUComputeUniformData(command,0,&sort_parameters,sizeof(sort_parameters));
        pass=SDL_BeginGPUComputePass(command,nullptr,0,sort_bindings.data(),static_cast<Uint32>(sort_bindings.size()));
        if (!pass) { last_error_="Unable to begin VFX alpha-sort dispatch: "+std::string(SDL_GetError()); SDL_PopGPUDebugGroup(command); return; }
        SDL_BindGPUComputePipeline(pass,vfx_sort_alpha_pipeline_);
        SDL_DispatchGPUCompute(pass,stage.dispatch_groups,1,1);
        SDL_EndGPUComputePass(pass); ++vfx_sort_dispatches_;
    }
    SDL_PopGPUDebugGroup(command);
}

bool SceneRenderer::create_environment_resources() {
    constexpr std::uint32_t specular_size=64U,specular_levels=7U,irradiance_size=16U,lut_size=128U;
    DecodedHdrImage hdr;
    environment_source_id_="asset.environment.procedural-sky";
    std::string source_fingerprint="builtin:procedural-sky/2";
    for (const auto& asset:asset_registry_.records()) {
        if (!asset.available || asset.kind!="Environment" || asset.extension!=".hdr") continue;
        const auto file=read_vfs_asset(*virtual_file_system_,asset_vfs_catalog_,asset.id,
            {.byte_budget=256U*1024U*1024U});
        if (!file.success) continue;
        hdr=decode_radiance_hdr(file.bytes);
        if (hdr.valid) {
            environment_source_id_=asset.id; environment_source_width_=hdr.width; environment_source_height_=hdr.height;
            source_fingerprint=asset.content_hash.empty()?asset.id+":"+std::to_string(asset.source_bytes):asset.content_hash;
            break;
        }
    }
    const auto cache=load_or_cook_split_sum_ibl(asset_registry_.asset_root().parent_path()/"generated"/"renderer-cache"/"ibl",
        hdr.valid?&hdr:nullptr,environment_source_id_,source_fingerprint);
    if (!cache.product.valid) { last_error_=cache.product.code+": "+cache.product.detail; return false; }
    ibl_cache_hit_=cache.cache_hit; ibl_cache_rebuilt_=cache.cache_rebuilt;
    ibl_cook_microseconds_=cache.cook_microseconds; ibl_artifact_bytes_=cache.artifact_bytes;
    ibl_artifact_path_=cache.artifact_path.generic_string(); ibl_source_fingerprint_=source_fingerprint;
    const auto& specular=cache.product.specular_rgba16f;
    std::array<std::array<std::size_t,6>,specular_levels> specular_offsets{};
    std::size_t specular_offset{};
    for (std::uint32_t level=0;level<specular_levels;++level) {
        const auto size=std::max(1U,specular_size>>level);
        for (std::uint32_t face=0;face<6U;++face) {
            specular_offsets[level][face]=specular_offset;
            specular_offset+=static_cast<std::size_t>(size)*size*4U*sizeof(std::uint16_t);
        }
    }
    const auto& irradiance=cache.product.irradiance_rgba16f;
    std::array<std::size_t,6> irradiance_offsets{};
    for (std::uint32_t face=0;face<6U;++face)
        irradiance_offsets[face]=static_cast<std::size_t>(face)*irradiance_size*irradiance_size*4U*sizeof(std::uint16_t);
    const auto& brdf_lut=cache.product.brdf_lut_rg16f;
    const auto upload_cube=[&](SDL_GPUTexture*& texture,const std::uint32_t size,const std::uint32_t levels,
        const std::vector<std::uint16_t>& data,const auto& offsets) {
        SDL_GPUTextureCreateInfo info{}; info.type=SDL_GPU_TEXTURETYPE_CUBE; info.format=SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT;
        info.usage=SDL_GPU_TEXTUREUSAGE_SAMPLER; info.width=size; info.height=size; info.layer_count_or_depth=6;
        info.num_levels=levels; info.sample_count=SDL_GPU_SAMPLECOUNT_1; texture=SDL_CreateGPUTexture(device_,&info);
        SDL_GPUTransferBufferCreateInfo transfer_info{SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD,static_cast<Uint32>(data.size()*sizeof(std::uint16_t)),0};
        auto* transfer=texture?SDL_CreateGPUTransferBuffer(device_,&transfer_info):nullptr;
        if (!transfer) return false;
        void* mapped=SDL_MapGPUTransferBuffer(device_,transfer,false);
        if (!mapped) { SDL_ReleaseGPUTransferBuffer(device_,transfer); return false; }
        std::memcpy(mapped,data.data(),data.size()*sizeof(std::uint16_t)); SDL_UnmapGPUTransferBuffer(device_,transfer);
        auto* command=SDL_AcquireGPUCommandBuffer(device_); auto* pass=command?SDL_BeginGPUCopyPass(command):nullptr;
        if (!pass) { SDL_ReleaseGPUTransferBuffer(device_,transfer); return false; }
        for (std::uint32_t level=0;level<levels;++level) for (std::uint32_t face=0;face<6U;++face) {
            const auto level_size=std::max(1U,size>>level);
            SDL_GPUTextureTransferInfo source{transfer,static_cast<Uint32>(offsets[level][face]),level_size,level_size};
            SDL_GPUTextureRegion destination{texture,level,face,0,0,0,level_size,level_size,1};
            SDL_UploadToGPUTexture(pass,&source,&destination,false);
        }
        SDL_EndGPUCopyPass(pass); const bool submitted=SDL_SubmitGPUCommandBuffer(command); SDL_ReleaseGPUTransferBuffer(device_,transfer);
        return submitted;
    };
    std::array<std::array<std::size_t,6>,1> irradiance_upload_offsets{irradiance_offsets};
    if (!upload_cube(environment_texture_,specular_size,specular_levels,specular,specular_offsets) ||
        !upload_cube(irradiance_texture_,irradiance_size,1U,irradiance,irradiance_upload_offsets)) {
        last_error_=SDL_GetError(); return false;
    }
    SDL_GPUTextureCreateInfo lut_info{}; lut_info.type=SDL_GPU_TEXTURETYPE_2D; lut_info.format=SDL_GPU_TEXTUREFORMAT_R16G16_FLOAT;
    lut_info.usage=SDL_GPU_TEXTUREUSAGE_SAMPLER; lut_info.width=lut_size; lut_info.height=lut_size; lut_info.layer_count_or_depth=1;
    lut_info.num_levels=1; lut_info.sample_count=SDL_GPU_SAMPLECOUNT_1; brdf_lut_texture_=SDL_CreateGPUTexture(device_,&lut_info);
    SDL_GPUTransferBufferCreateInfo lut_transfer_info{SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD,static_cast<Uint32>(brdf_lut.size()*sizeof(std::uint16_t)),0};
    auto* lut_transfer=brdf_lut_texture_?SDL_CreateGPUTransferBuffer(device_,&lut_transfer_info):nullptr;
    if (!lut_transfer) { last_error_=SDL_GetError(); return false; }
    void* lut_mapped=SDL_MapGPUTransferBuffer(device_,lut_transfer,false);
    if (!lut_mapped) { SDL_ReleaseGPUTransferBuffer(device_,lut_transfer); last_error_=SDL_GetError(); return false; }
    std::memcpy(lut_mapped,brdf_lut.data(),brdf_lut.size()*sizeof(std::uint16_t)); SDL_UnmapGPUTransferBuffer(device_,lut_transfer);
    auto* lut_command=SDL_AcquireGPUCommandBuffer(device_); auto* lut_pass=lut_command?SDL_BeginGPUCopyPass(lut_command):nullptr;
    if (!lut_pass) { SDL_ReleaseGPUTransferBuffer(device_,lut_transfer); last_error_=SDL_GetError(); return false; }
    SDL_GPUTextureTransferInfo lut_source{lut_transfer,0,lut_size,lut_size};
    SDL_GPUTextureRegion lut_destination{brdf_lut_texture_,0,0,0,0,0,lut_size,lut_size,1};
    SDL_UploadToGPUTexture(lut_pass,&lut_source,&lut_destination,false); SDL_EndGPUCopyPass(lut_pass);
    const bool lut_submitted=SDL_SubmitGPUCommandBuffer(lut_command); SDL_ReleaseGPUTransferBuffer(device_,lut_transfer);
    if (!lut_submitted) { last_error_=SDL_GetError(); return false; }
    SDL_GPUSamplerCreateInfo sampler_info{};
    sampler_info.min_filter=SDL_GPU_FILTER_LINEAR; sampler_info.mag_filter=SDL_GPU_FILTER_LINEAR;
    sampler_info.mipmap_mode=SDL_GPU_SAMPLERMIPMAPMODE_LINEAR;
    sampler_info.address_mode_u=sampler_info.address_mode_v=sampler_info.address_mode_w=SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    environment_sampler_=SDL_CreateGPUSampler(device_,&sampler_info);
    if (!environment_sampler_) { last_error_=SDL_GetError(); return false; }
    return true;
}

bool SceneRenderer::create_rgba8_texture(const std::uint32_t width, const std::uint32_t height,
                                         const std::uint8_t* pixels, const std::size_t byte_count,
                                         const bool srgb, SDL_GPUTexture*& texture) {
    if (width == 0U || height == 0U || pixels == nullptr || byte_count != static_cast<std::size_t>(width) * height * 4U ||
        byte_count > std::numeric_limits<Uint32>::max()) return false;
    SDL_GPUTextureCreateInfo texture_info{};
    texture_info.type=SDL_GPU_TEXTURETYPE_2D; texture_info.format=srgb ? SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM_SRGB : SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    const auto longest=std::max(width,height);
    const auto levels=1U+static_cast<std::uint32_t>(std::floor(std::log2(static_cast<double>(longest))));
    texture_info.usage=SDL_GPU_TEXTUREUSAGE_SAMPLER|SDL_GPU_TEXTUREUSAGE_COLOR_TARGET; texture_info.width=width; texture_info.height=height;
    texture_info.layer_count_or_depth=1; texture_info.num_levels=levels; texture_info.sample_count=SDL_GPU_SAMPLECOUNT_1;
    texture=SDL_CreateGPUTexture(device_,&texture_info);
    if (!texture) return false;
    SDL_GPUTransferBufferCreateInfo transfer_info{};
    transfer_info.usage=SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD; transfer_info.size=static_cast<Uint32>(byte_count);
    SDL_GPUTransferBuffer* transfer=SDL_CreateGPUTransferBuffer(device_,&transfer_info);
    if (!transfer) { SDL_ReleaseGPUTexture(device_,texture); texture=nullptr; return false; }
    void* mapped=SDL_MapGPUTransferBuffer(device_,transfer,false);
    if (!mapped) { SDL_ReleaseGPUTransferBuffer(device_,transfer); SDL_ReleaseGPUTexture(device_,texture); texture=nullptr; return false; }
    std::memcpy(mapped,pixels,byte_count); SDL_UnmapGPUTransferBuffer(device_,transfer);
    SDL_GPUCommandBuffer* command=SDL_AcquireGPUCommandBuffer(device_);
    if (!command) { SDL_ReleaseGPUTransferBuffer(device_,transfer); SDL_ReleaseGPUTexture(device_,texture); texture=nullptr; return false; }
    SDL_GPUCopyPass* pass=SDL_BeginGPUCopyPass(command);
    SDL_GPUTextureTransferInfo source{transfer,0,width,height};
    SDL_GPUTextureRegion destination{texture,0,0,0,0,0,width,height,1};
    SDL_UploadToGPUTexture(pass,&source,&destination,false); SDL_EndGPUCopyPass(pass);
    if (levels>1U) SDL_GenerateMipmapsForGPUTexture(command,texture);
    const bool submitted=SDL_SubmitGPUCommandBuffer(command);
    SDL_ReleaseGPUTransferBuffer(device_,transfer);
    if (!submitted) { SDL_ReleaseGPUTexture(device_,texture); texture=nullptr; return false; }
    return true;
}

bool SceneRenderer::create_material_resources() {
    constexpr std::array<std::uint8_t,4> white{255U,255U,255U,255U};
    constexpr std::array<std::uint8_t,4> flat_normal{128U,128U,255U,255U};
    constexpr std::array<std::uint8_t,4> black{0U,0U,0U,255U};
    if (!create_rgba8_texture(1U,1U,white.data(),white.size(),true,white_texture_) ||
        !create_rgba8_texture(1U,1U,white.data(),white.size(),false,linear_white_texture_) ||
        !create_rgba8_texture(1U,1U,black.data(),black.size(),false,linear_black_texture_) ||
        !create_rgba8_texture(1U,1U,flat_normal.data(),flat_normal.size(),false,flat_normal_texture_) ||
        !create_rgba8_texture(1U,1U,black.data(),black.size(),true,black_texture_)) {
        last_error_=SDL_GetError(); return false;
    }
    std::array<std::uint8_t,8U*8U*4U> checker{};
    for (std::uint32_t y=0; y<8U; ++y) for (std::uint32_t x=0; x<8U; ++x) {
        const bool bright=((x/2U)+(y/2U))%2U==0U;
        const std::size_t offset=static_cast<std::size_t>(y*8U+x)*4U;
        checker[offset]=bright?255U:45U; checker[offset+1U]=bright?255U:55U;
        checker[offset+2U]=bright?255U:75U; checker[offset+3U]=255U;
    }
    if (!create_rgba8_texture(8U,8U,checker.data(),checker.size(),true,checker_texture_)) {
        last_error_=SDL_GetError(); return false;
    }
    SDL_GPUSamplerCreateInfo sampler_info{};
    sampler_info.min_filter=SDL_GPU_FILTER_LINEAR; sampler_info.mag_filter=SDL_GPU_FILTER_LINEAR;
    sampler_info.mipmap_mode=SDL_GPU_SAMPLERMIPMAPMODE_LINEAR;
    sampler_info.address_mode_u=sampler_info.address_mode_v=sampler_info.address_mode_w=SDL_GPU_SAMPLERADDRESSMODE_REPEAT;
    material_sampler_=SDL_CreateGPUSampler(device_,&sampler_info);
    if (!material_sampler_) { last_error_=SDL_GetError(); return false; }
    sampler_info.address_mode_u=sampler_info.address_mode_v=sampler_info.address_mode_w=SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    tone_map_sampler_=SDL_CreateGPUSampler(device_,&sampler_info);
    if (!tone_map_sampler_) { last_error_=SDL_GetError(); return false; }
    return true;
}

bool SceneRenderer::create_sprite_resources() {
    sprite_atlas_manifests_discovered_=0;
    sprite_atlas_manifests_valid_=0;
    sprite_atlas_manifests_invalid_=0;
    sprite_atlas_declared_page_assets_=0;
    sprite_atlas_unique_page_assets_=0;
    sprite_atlas_page_textures_uploaded_=0;
    sprite_atlas_page_textures_missing_=0;
    sprite_atlas_page_textures_available_=0;
    sprite_atlas_counts_truncated_=false;
    std::unordered_set<std::string> atlas_page_assets;
    const auto count_atlas_status = [this](std::size_t& counter, const std::size_t amount=1U) {
        const auto bounded_amount=std::min(amount,sprite_atlas_status_max_items);
        if (counter>sprite_atlas_status_max_items-bounded_amount) {
            counter=sprite_atlas_status_max_items;
            sprite_atlas_counts_truncated_=true;
            return;
        }
        counter+=bounded_amount;
        if (amount!=bounded_amount) sprite_atlas_counts_truncated_=true;
    };
    const auto finalize_atlas_status = [&]() {
        sprite_atlas_unique_page_assets_=atlas_page_assets.size();
        sprite_atlas_page_textures_available_=0;
        for (const auto& page_asset_id : atlas_page_assets) {
            const auto handle=texture_resources_.find(page_asset_id,"sprite-base-color-srgb");
            if (handle&&texture_resources_.resolve(*handle)!=nullptr)
                count_atlas_status(sprite_atlas_page_textures_available_);
        }
        sprite_atlas_page_textures_missing_=sprite_atlas_unique_page_assets_-
            std::min(sprite_atlas_unique_page_assets_,sprite_atlas_page_textures_available_);
    };
    SDL_GPUSamplerCreateInfo sampler_info{};
    sampler_info.min_filter=SDL_GPU_FILTER_NEAREST;sampler_info.mag_filter=SDL_GPU_FILTER_NEAREST;
    sampler_info.mipmap_mode=SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
    sampler_info.address_mode_u=sampler_info.address_mode_v=sampler_info.address_mode_w=SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    sprite_nearest_sampler_=SDL_CreateGPUSampler(device_,&sampler_info);
    if(!sprite_nearest_sampler_){last_error_=SDL_GetError();finalize_atlas_status();return false;}
    SDL_GPUBufferCreateInfo instance_info{SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ,
        static_cast<Uint32>(sprite_instance_capacity*sizeof(GpuSpriteInstance)),0};
    sprite_instance_buffer_=SDL_CreateGPUBuffer(device_,&instance_info);
    SDL_GPUTransferBufferCreateInfo transfer_info{SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD,
        static_cast<Uint32>(sprite_instance_capacity*sizeof(GpuSpriteInstance)),0};
    sprite_instance_upload_=SDL_CreateGPUTransferBuffer(device_,&transfer_info);
    SDL_GPUBufferCreateInfo draw_index_info{SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ,
        static_cast<Uint32>(sprite_instance_capacity*sizeof(std::uint32_t)),0};
    sprite_draw_index_buffer_=SDL_CreateGPUBuffer(device_,&draw_index_info);
    SDL_GPUTransferBufferCreateInfo draw_index_transfer_info{SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD,
        static_cast<Uint32>(sprite_instance_capacity*sizeof(std::uint32_t)),0};
    sprite_draw_index_upload_=SDL_CreateGPUTransferBuffer(device_,&draw_index_transfer_info);
    if(!sprite_instance_buffer_||!sprite_instance_upload_||!sprite_draw_index_buffer_||!sprite_draw_index_upload_){
        last_error_="Unable to allocate Sprite stable instance storage: "+std::string(SDL_GetError());
        finalize_atlas_status();return false;}
    SDL_SetGPUBufferName(device_,sprite_instance_buffer_,"sprite.instances");
    SDL_SetGPUBufferName(device_,sprite_draw_index_buffer_,"sprite.draw-indices");
    std::unordered_set<std::string> srgb_assets;
    std::unordered_set<std::string> linear_assets;
    for(const auto& asset:asset_registry_.records()) {
        if(!asset.available||(asset.kind!="Sprite"&&!asset.relative_path.ends_with(".sprite.json")))continue;
        const auto source=read_vfs_asset(*virtual_file_system_,asset_vfs_catalog_,asset.id,
            {.byte_budget=16U*1024U*1024U});if(!source.success)continue;
        const auto parsed=SpriteAssetCodec::parse_json(std::string_view(
            reinterpret_cast<const char*>(source.bytes.data()),source.bytes.size()));
        if(!parsed)continue;
        srgb_assets.insert(parsed.document->texture_asset);
        if(parsed.document->material) {
            if(!parsed.document->material->normal_texture_asset.empty())linear_assets.insert(parsed.document->material->normal_texture_asset);
            if(!parsed.document->material->emissive_mask_texture_asset.empty())linear_assets.insert(parsed.document->material->emissive_mask_texture_asset);
            if(!parsed.document->material->depth_texture_asset.empty())linear_assets.insert(parsed.document->material->depth_texture_asset);
        }
    }
    for(const auto& asset:asset_registry_.records()) {
        if(asset.kind!="SpriteAtlas")continue;
        count_atlas_status(sprite_atlas_manifests_discovered_);
        if(!asset.available){count_atlas_status(sprite_atlas_manifests_invalid_);continue;}
        const auto source=read_vfs_asset(*virtual_file_system_,asset_vfs_catalog_,asset.id,
            {.byte_budget=64U*1024U*1024U});
        if(!source.success){count_atlas_status(sprite_atlas_manifests_invalid_);continue;}
        const auto manifest=nlohmann::json::parse(std::string_view(
            reinterpret_cast<const char*>(source.bytes.data()),source.bytes.size()),nullptr,false);
        if(manifest.is_object()&&manifest.contains("authoringDocument")&&
            manifest.at("authoringDocument").is_object()) {
            const auto authoring=SpriteAssetCodec::parse_json(manifest.at("authoringDocument").dump());
            if(authoring&&authoring.document->material) {
                const auto& material=*authoring.document->material;
                if(!material.normal_texture_asset.empty())linear_assets.insert(material.normal_texture_asset);
                if(!material.emissive_mask_texture_asset.empty())linear_assets.insert(material.emissive_mask_texture_asset);
                if(!material.depth_texture_asset.empty())linear_assets.insert(material.depth_texture_asset);
            }
        }
        const auto parsed=parse_sprite_atlas_artifact_json(std::string_view(
            reinterpret_cast<const char*>(source.bytes.data()),source.bytes.size()));
        if(!parsed||!parsed.artifact){count_atlas_status(sprite_atlas_manifests_invalid_);continue;}
        count_atlas_status(sprite_atlas_manifests_valid_);
        for(const auto& page:parsed.artifact->pages) {
            count_atlas_status(sprite_atlas_declared_page_assets_);
            if(page.asset_id.empty())continue;
            if(!atlas_page_assets.contains(page.asset_id)) {
                if(atlas_page_assets.size()<sprite_atlas_status_max_items)
                    atlas_page_assets.insert(page.asset_id);
                else sprite_atlas_counts_truncated_=true;
            }
            srgb_assets.insert(page.asset_id);
        }
    }
    const auto atlas_page_asset = [&atlas_page_assets](const std::string_view asset_id) {
        return atlas_page_assets.contains(std::string(asset_id));
    };
    for(const auto& asset:asset_registry_.records()) {
        const bool is_atlas_page=atlas_page_asset(asset.id);
        const bool needs_srgb=srgb_assets.contains(asset.id);const bool needs_linear=linear_assets.contains(asset.id);
        if(!asset.available||(asset.extension!=".png"&&asset.extension!=".ktx2")||(!needs_srgb&&!needs_linear))continue;
        const auto bytes=read_vfs_asset(*virtual_file_system_,asset_vfs_catalog_,asset.id,
            {.byte_budget=512U*1024U*1024U});
        if(!bytes.success)continue;
        std::uint32_t width{},height{};std::vector<std::byte> rgba8;
        SDL_GPUTexture* texture=nullptr;SDL_GPUTexture* linear_texture=nullptr;
        if(asset.extension==".ktx2") {
            const auto upload=[&](const bool srgb,SDL_GPUTexture*& destination) {
                const auto initial_tail=asset.streaming_mode=="resident"
                    ?std::numeric_limits<std::uint32_t>::max():4U;
                auto result=create_ktx2_texture_stream(device_,std::span<const std::byte>(
                     bytes.bytes.data(),bytes.bytes.size()),srgb,initial_tail);
                if(!result.valid){last_error_=asset.id+": "+result.code+" - "+result.detail;return false;}
                result.asset_id=asset.id;result.linear_semantic=!srgb;result.streaming_enabled=asset.streaming_mode=="stream";
                result.authored_priority=asset.streaming_priority;
                result.authored_importance=static_cast<std::uint8_t>(texture_streaming_importance_from_name(
                    asset.streaming_importance).value_or(TextureStreamingImportance::normal));
                if(!result.streaming_enabled){result.maximum_mip_start=0U;result.target_mip_start=0U;}
                const auto semantic=srgb?"sprite-base-color-srgb":"sprite-linear-data";
                const auto handle=texture_resources_.acquire({
                    .stable_id=asset.id,.semantic=semantic,.owner="scene.sprite",
                    .source=asset.relative_path,.residency=result.streaming_enabled?"stream":"resident",
                    .metadata={result.width,result.height,result.level_count,result.resident_mip_start,result.resident_bytes}},
                    result.texture);
                if(!handle.valid()) {
                    last_error_=asset.id+": unable to acquire stable texture resource "+semantic;
                    release_texture_stream(device_,result);return false;
                }
                destination=result.texture;++ktx_textures_uploaded_;
                if(result.native_compressed)++ktx_native_compressed_textures_;else ++ktx_rgba8_fallback_textures_;
                ktx_mip_levels_uploaded_+=result.uploaded_level_count;ktx_source_bytes_+=result.source_bytes;
                ktx_resident_bytes_+=result.resident_bytes;ktx_tail_bytes_+=result.tail_bytes;
                ktx_staging_bytes_+=result.staging_bytes;
                texture_streaming_bytes_total_+=result.uploaded_source_bytes;
                texture_streaming_copy_bytes_total_+=result.uploaded_copy_bytes;
                texture_stream_lookup_[result.texture]=texture_streams_.size();
                texture_streams_.push_back(std::move(result));texture_stream_handles_.push_back(handle);return true;
            };
            if((needs_srgb&&!upload(true,texture))||(needs_linear&&!upload(false,linear_texture))) {
                if(texture&&!texture_stream_lookup_.contains(texture))SDL_ReleaseGPUTexture(device_,texture);
                if(linear_texture&&!texture_stream_lookup_.contains(linear_texture))SDL_ReleaseGPUTexture(device_,linear_texture);
                finalize_atlas_status();return false;
            }
        } else {
            const auto decoded=decode_png_rgba8(bytes.bytes);
            if(!decoded.valid)continue;width=decoded.width;height=decoded.height;
            rgba8.assign(reinterpret_cast<const std::byte*>(decoded.rgba8.data()),
                reinterpret_cast<const std::byte*>(decoded.rgba8.data()+decoded.rgba8.size()));
            const auto* pixels=reinterpret_cast<const std::uint8_t*>(rgba8.data());
            if((needs_srgb&&!create_rgba8_texture(width,height,pixels,rgba8.size(),true,texture)) ||
               (needs_linear&&!create_rgba8_texture(width,height,pixels,rgba8.size(),false,linear_texture))) {
                if(texture)SDL_ReleaseGPUTexture(device_,texture);
                if(linear_texture)SDL_ReleaseGPUTexture(device_,linear_texture);
               last_error_=asset.id+": sprite GPU texture upload failed - "+SDL_GetError();
               finalize_atlas_status();return false;
            }
        }
        const auto acquire_png=[&](SDL_GPUTexture* uploaded,const bool srgb) {
            if(!uploaded)return TextureResourceHandle{};
            const auto levels=1U+static_cast<std::uint32_t>(std::floor(std::log2(
                static_cast<double>(std::max(width,height)))));
            std::uint64_t bytes_estimate{};auto level_width=width;auto level_height=height;
            for(std::uint32_t level=0;level<levels;++level) {
                bytes_estimate+=static_cast<std::uint64_t>(level_width)*level_height*4U;
                level_width=std::max(1U,level_width/2U);level_height=std::max(1U,level_height/2U);
            }
            return texture_resources_.acquire({.stable_id=asset.id,
                .semantic=srgb?"sprite-base-color-srgb":"sprite-linear-data",.owner="scene.sprite",
                .source=asset.relative_path,.residency="resident",
                .metadata={width,height,levels,0U,bytes_estimate}},uploaded);
        };
        if(texture) {
            SDL_SetGPUTextureName(device_,texture,("sprite.texture."+asset.id).c_str());
            auto handle=texture_resources_.find(asset.id,"sprite-base-color-srgb").value_or(TextureResourceHandle{});
            if(!handle.valid())handle=acquire_png(texture,true);
            if(!handle.valid()){
                SDL_ReleaseGPUTexture(device_,texture);texture=nullptr;
                if(linear_texture&&!texture_resources_.find(asset.id,"sprite-linear-data")) {
                    SDL_ReleaseGPUTexture(device_,linear_texture);linear_texture=nullptr;
                }
                last_error_=asset.id+": stable sprite texture registration failed";
                finalize_atlas_status();return false;
            }
            sprite_textures_.emplace(asset.id,handle);++sprite_textures_uploaded_;
            if(is_atlas_page)count_atlas_status(sprite_atlas_page_textures_uploaded_);
        }
        if(linear_texture) {
            SDL_SetGPUTextureName(device_,linear_texture,("sprite.linear-texture."+asset.id).c_str());
            auto handle=texture_resources_.find(asset.id,"sprite-linear-data").value_or(TextureResourceHandle{});
            if(!handle.valid())handle=acquire_png(linear_texture,false);
            if(!handle.valid()){
                SDL_ReleaseGPUTexture(device_,linear_texture);linear_texture=nullptr;
                last_error_=asset.id+": stable sprite linear texture registration failed";
                finalize_atlas_status();return false;
            }
            sprite_linear_textures_.emplace(asset.id,handle);
        }
    }
    finalize_atlas_status();
    return true;
}

SDL_GPUComputePipeline* load_atmosphere_compute_pipeline(SDL_GPUDevice* device,
                                                          const std::string_view shader_name,
                                                          const std::uint32_t samplers) {
    const auto formats=SDL_GetGPUShaderFormats(device);
    const bool use_dxil=(formats&SDL_GPU_SHADERFORMAT_DXIL)!=0;
    const bool use_spirv=!use_dxil&&(formats&SDL_GPU_SHADERFORMAT_SPIRV)!=0;
    if(!use_dxil&&!use_spirv)return nullptr;
    const auto artifact=runtime_shader_artifacts().load(ShaderArtifactRequest{
        .stem=std::string(shader_name),.stage=ShaderArtifactStage::compute,
        .resources={.uniform_buffers=1,.samplers=samplers,.storage_textures=1}},
        use_dxil?ShaderArtifactBackend::dxil:ShaderArtifactBackend::spv);
    if(!artifact.success) {
        shader_artifact_failure=artifact.code+": "+artifact.detail;
        return nullptr;
    }
    SDL_GPUComputePipelineCreateInfo info{};
    info.code=reinterpret_cast<const Uint8*>(artifact.bytes.data());
    info.code_size=artifact.bytes.size();info.entrypoint=artifact.entrypoint.c_str();
    info.format=use_dxil?SDL_GPU_SHADERFORMAT_DXIL:SDL_GPU_SHADERFORMAT_SPIRV;
    info.num_samplers=samplers;info.num_readwrite_storage_textures=1;
    info.num_uniform_buffers=1;info.threadcount_x=8;info.threadcount_y=8;info.threadcount_z=1;
    return SDL_CreateGPUComputePipeline(device,&info);
}

SDL_GPUComputePipeline* load_screen_space_compute_pipeline(SDL_GPUDevice* device,
                                                            const std::string_view shader_name) {
    const auto formats=SDL_GetGPUShaderFormats(device);
    const bool use_dxil=(formats&SDL_GPU_SHADERFORMAT_DXIL)!=0;
    const bool use_spirv=!use_dxil&&(formats&SDL_GPU_SHADERFORMAT_SPIRV)!=0;
    if(!use_dxil&&!use_spirv)return nullptr;
    const auto artifact=runtime_shader_artifacts().load(ShaderArtifactRequest{
        .stem=std::string(shader_name),.stage=ShaderArtifactStage::compute,
        .resources={.uniform_buffers=1,.samplers=1,.storage_textures=1}},
        use_dxil?ShaderArtifactBackend::dxil:ShaderArtifactBackend::spv);
    if(!artifact.success) {
        shader_artifact_failure=artifact.code+": "+artifact.detail;
        return nullptr;
    }
    SDL_GPUComputePipelineCreateInfo info{};
    info.code=reinterpret_cast<const Uint8*>(artifact.bytes.data());
    info.code_size=artifact.bytes.size();info.entrypoint=artifact.entrypoint.c_str();
    info.format=use_dxil?SDL_GPU_SHADERFORMAT_DXIL:SDL_GPU_SHADERFORMAT_SPIRV;
    info.num_samplers=1;info.num_readwrite_storage_textures=1;info.num_uniform_buffers=1;
    info.threadcount_x=8;info.threadcount_y=8;info.threadcount_z=1;
    return SDL_CreateGPUComputePipeline(device,&info);
}

Mat4 inverse_or_identity(const Mat4& source) {
    std::array<std::array<double,8>,4> augmented{};
    for(std::size_t row=0;row<4;++row) {
        for(std::size_t column=0;column<4;++column)
            augmented[row][column]=source.value[column*4+row];
        augmented[row][row+4]=1.0;
    }
    for(std::size_t pivot=0;pivot<4;++pivot) {
        std::size_t best=pivot;
        for(std::size_t row=pivot+1;row<4;++row)
            if(std::abs(augmented[row][pivot])>std::abs(augmented[best][pivot]))best=row;
        if(std::abs(augmented[best][pivot])<1.0e-12)return identity();
        if(best!=pivot)std::swap(augmented[best],augmented[pivot]);
        const auto divisor=augmented[pivot][pivot];
        for(auto& value:augmented[pivot])value/=divisor;
        for(std::size_t row=0;row<4;++row) {
            if(row==pivot)continue;
            const auto factor=augmented[row][pivot];
            for(std::size_t column=0;column<8;++column)
                augmented[row][column]-=factor*augmented[pivot][column];
        }
    }
    Mat4 result{};
    for(std::size_t row=0;row<4;++row)
        for(std::size_t column=0;column<4;++column)
            result.value[column*4+row]=static_cast<float>(augmented[row][column+4]);
    return result;
}

bool SceneRenderer::create_clustered_lighting_resources() {
    const auto cluster_count=clustered_lighting_config_.tiles_x*clustered_lighting_config_.tiles_y*clustered_lighting_config_.depth_slices;
    const auto light_bytes=static_cast<Uint32>(clustered_lighting_config_.maximum_lights*sizeof(GpuLocalLight));
    const auto cluster_bytes=static_cast<Uint32>(cluster_count*sizeof(ClusterLightRange));
    const auto index_bytes=static_cast<Uint32>(cluster_count*clustered_lighting_config_.maximum_lights_per_cluster*sizeof(std::uint32_t));
    const auto create_buffer=[&](SDL_GPUBuffer*& buffer,SDL_GPUTransferBuffer*& upload,const Uint32 bytes,const char* name) {
        const SDL_GPUBufferCreateInfo buffer_info{SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ,bytes,0};
        const SDL_GPUTransferBufferCreateInfo upload_info{SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD,bytes,0};
        buffer=SDL_CreateGPUBuffer(device_,&buffer_info);upload=SDL_CreateGPUTransferBuffer(device_,&upload_info);
        if(buffer)SDL_SetGPUBufferName(device_,buffer,name);
        return buffer&&upload;
    };
    if(!create_buffer(local_light_buffer_,local_light_upload_,light_bytes,"lighting.local-lights")||
       !create_buffer(light_cluster_buffer_,light_cluster_upload_,cluster_bytes,"lighting.cluster-ranges")||
       !create_buffer(light_cluster_index_buffer_,light_cluster_index_upload_,index_bytes,"lighting.cluster-indices")) {
        last_error_="Unable to allocate clustered lighting resources: "+std::string(SDL_GetError());return false;
    }
    return true;
}

bool SceneRenderer::upload_clustered_lighting(SDL_GPUCommandBuffer* command,const RenderWorldSnapshot& render_world,
    const ClusteredLightingCamera& camera,const std::span<const std::int32_t> shadow_base_layers) {
    const auto assignment=build_clustered_lighting(clustered_lighting_config_,camera,render_world.local_lights);
    std::vector<GpuLocalLight> lights;lights.reserve(assignment.accepted_light_count);
    constexpr float pi=3.14159265358979323846F;
    for(std::size_t index=0;index<assignment.accepted_light_count;++index) {
        const auto& source=render_world.local_lights[index];
        const auto outer_radians=source.outer_cone_degrees*pi/180.0F;
        const auto solid_angle=source.kind=="spot"?2.0F*pi*(1.0F-std::cos(outer_radians)):4.0F*pi;
        const auto candela=source.luminous_power_lumens/std::max(solid_angle,0.001F);
        const auto shadow_layer=index<shadow_base_layers.size()?shadow_base_layers[index]:-1;
        lights.push_back({{source.position[0],source.position[1],source.position[2],source.range_meters},
            {source.direction[0],source.direction[1],source.direction[2],source.kind=="spot"?1.0F:0.0F},
            {source.color[0],source.color[1],source.color[2],candela},
            {std::cos(source.inner_cone_degrees*pi/180.0F),std::cos(outer_radians),source.source_radius_meters,
             static_cast<float>(shadow_layer)}});
    }
    const auto light_bytes=static_cast<Uint32>(lights.size()*sizeof(GpuLocalLight));
    const auto cluster_bytes=static_cast<Uint32>(assignment.clusters.size()*sizeof(ClusterLightRange));
    const auto index_bytes=static_cast<Uint32>(assignment.light_indices.size()*sizeof(std::uint32_t));
    const auto map_copy=[&](SDL_GPUTransferBuffer* upload,const void* source,const Uint32 bytes) {
        if(bytes==0)return true;auto* mapped=SDL_MapGPUTransferBuffer(device_,upload,true);if(!mapped)return false;
        std::memcpy(mapped,source,bytes);SDL_UnmapGPUTransferBuffer(device_,upload);return true;
    };
    if(!map_copy(local_light_upload_,lights.data(),light_bytes)||
       !map_copy(light_cluster_upload_,assignment.clusters.data(),cluster_bytes)||
       !map_copy(light_cluster_index_upload_,assignment.light_indices.data(),index_bytes)) {
        last_error_="Unable to map clustered lighting upload: "+std::string(SDL_GetError());return false;
    }
    auto* pass=SDL_BeginGPUCopyPass(command);if(!pass){last_error_=SDL_GetError();return false;}
    const auto upload_buffer=[&](SDL_GPUTransferBuffer* source,SDL_GPUBuffer* destination,const Uint32 bytes) {
        if(bytes==0)return;const SDL_GPUTransferBufferLocation source_region{source,0};
        const SDL_GPUBufferRegion destination_region{destination,0,bytes};SDL_UploadToGPUBuffer(pass,&source_region,&destination_region,true);
    };
    upload_buffer(local_light_upload_,local_light_buffer_,light_bytes);
    upload_buffer(light_cluster_upload_,light_cluster_buffer_,cluster_bytes);
    upload_buffer(light_cluster_index_upload_,light_cluster_index_buffer_,index_bytes);
    SDL_EndGPUCopyPass(pass);
    local_lights_submitted_=assignment.accepted_light_count;local_lights_dropped_=assignment.dropped_light_count;
    light_cluster_assignments_=assignment.light_indices.size();light_cluster_overflows_=assignment.overflowed_assignments;
    clustered_lighting_upload_bytes_+=static_cast<std::uint64_t>(light_bytes)+cluster_bytes+index_bytes;
    return true;
}

bool SceneRenderer::create_geometry() {
    std::vector<Vertex> upload_vertices(vertices.begin(),vertices.begin()+builtin_base_vertex_count);
    std::vector<std::uint32_t> upload_indices(indices.begin(),indices.begin()+builtin_base_index_count);
    for (std::size_t index = 0; index < 24U; ++index) {
        if (upload_vertices[index].position[1] > 0.0F) upload_vertices[index].joints[0] = 1.0F;
    }
    constexpr float pi=3.14159265358979323846F;
    const auto sphere_vertex_offset=static_cast<std::uint32_t>(upload_vertices.size());
    upload_vertices.reserve(upload_vertices.size()+(builtin_sphere_rings+1U)*(builtin_sphere_segments+1U));
    upload_indices.reserve(upload_indices.size()+builtin_sphere_index_count);
    for(std::uint32_t ring=0;ring<=builtin_sphere_rings;++ring) {
        const float v=static_cast<float>(ring)/static_cast<float>(builtin_sphere_rings);
        const float phi=v*pi;const float y=std::cos(phi);const float radius=std::sin(phi);
        for(std::uint32_t segment=0;segment<=builtin_sphere_segments;++segment) {
            const float u=static_cast<float>(segment)/static_cast<float>(builtin_sphere_segments);
            const float theta=u*pi*2.0F;const float x=std::cos(theta)*radius;const float z=std::sin(theta)*radius;
            upload_vertices.push_back({{x,y,z},{x,y,z},{u,v}});
        }
    }
    for(std::uint32_t ring=0;ring<builtin_sphere_rings;++ring) {
        for(std::uint32_t segment=0;segment<builtin_sphere_segments;++segment) {
            const auto current=sphere_vertex_offset+ring*(builtin_sphere_segments+1U)+segment;
            const auto next=current+builtin_sphere_segments+1U;
            upload_indices.insert(upload_indices.end(),{current,next,current+1U,current+1U,next,next+1U});
        }
    }
    raytracing_geometries_.clear();
    const auto retain_builtin_geometry=[&](const std::string_view geometry_id,
                                           const std::size_t first_vertex,
                                           const std::size_t vertex_count,
                                           const std::size_t first_index,
                                           const std::size_t index_count) {
        std::vector<std::array<float,3U>> positions;
        positions.reserve(vertex_count);
        for(std::size_t index=0U;index<vertex_count;++index) {
            const auto& source=upload_vertices[first_vertex+index].position;
            positions.push_back({source[0],source[1],source[2]});
        }
        std::vector<std::uint32_t> local_indices;
        local_indices.reserve(index_count);
        for(std::size_t index=0U;index<index_count;++index)
            local_indices.push_back(upload_indices[first_index+index]-static_cast<std::uint32_t>(first_vertex));
        SceneRayTracingPrimitiveInput primitive;
        primitive.primitive_id=std::string(geometry_id)+"#0";
        primitive.index_count=static_cast<std::uint32_t>(local_indices.size());
        auto geometry=make_builtin_scene_raytracing_geometry_input(
            geometry_id,positions,local_indices,std::span<const SceneRayTracingPrimitiveInput>(&primitive,1U));
        raytracing_geometries_.insert_or_assign(std::string(geometry_id),std::move(geometry));
    };
    retain_builtin_geometry("asset.primitive.cube",0U,24U,0U,36U);
    retain_builtin_geometry("asset.primitive.plane",24U,4U,36U,6U);
    retain_builtin_geometry("asset.primitive.sphere",sphere_vertex_offset,
        upload_vertices.size()-sphere_vertex_offset,builtin_sphere_first_index,builtin_sphere_index_count);
    const auto vertex_bytes=static_cast<Uint32>(upload_vertices.size()*sizeof(Vertex));
    const auto index_bytes=static_cast<Uint32>(upload_indices.size()*sizeof(std::uint32_t));
    SDL_GPUBufferCreateInfo vertex_info{SDL_GPU_BUFFERUSAGE_VERTEX, vertex_bytes, 0};
    SDL_GPUBufferCreateInfo index_info{SDL_GPU_BUFFERUSAGE_INDEX, index_bytes, 0};
    vertex_buffer_ = SDL_CreateGPUBuffer(device_, &vertex_info);
    index_buffer_ = SDL_CreateGPUBuffer(device_, &index_info);
    if (!vertex_buffer_ || !index_buffer_) return false;

    SDL_GPUTransferBufferCreateInfo transfer_info{};
    transfer_info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    transfer_info.size = vertex_bytes+index_bytes;
    SDL_GPUTransferBuffer* transfer = SDL_CreateGPUTransferBuffer(device_, &transfer_info);
    if (!transfer) return false;
    auto* mapped = static_cast<std::byte*>(SDL_MapGPUTransferBuffer(device_, transfer, false));
    if (!mapped) { SDL_ReleaseGPUTransferBuffer(device_, transfer); return false; }
    std::memcpy(mapped,upload_vertices.data(),vertex_bytes);
    std::memcpy(mapped+vertex_bytes,upload_indices.data(),index_bytes);
    SDL_UnmapGPUTransferBuffer(device_, transfer);
    SDL_GPUCommandBuffer* command = SDL_AcquireGPUCommandBuffer(device_);
    if (!command) { SDL_ReleaseGPUTransferBuffer(device_, transfer); return false; }
    SDL_GPUCopyPass* pass = SDL_BeginGPUCopyPass(command);
    SDL_GPUTransferBufferLocation source{transfer, 0};
    SDL_GPUBufferRegion destination{vertex_buffer_,0,vertex_bytes};
    SDL_UploadToGPUBuffer(pass, &source, &destination, false);
    source.offset=vertex_bytes;
    destination={index_buffer_,0,index_bytes};
    SDL_UploadToGPUBuffer(pass, &source, &destination, false);
    SDL_EndGPUCopyPass(pass);
    const bool submitted = SDL_SubmitGPUCommandBuffer(command);
    SDL_ReleaseGPUTransferBuffer(device_, transfer);
    return submitted;
}

bool SceneRenderer::create_imported_geometry() {
    for (const auto& asset : asset_registry_.records()) {
        if (!asset.available || (asset.extension != ".meshbin" && asset.extension != ".glb" &&
            asset.extension != ".gltf" && asset.extension != ".fbx")) continue;
        GltfMeshData decoded;
        if (asset.extension == ".meshbin") {
            constexpr std::uintmax_t maximum_mesh_artifact_bytes = 512U * 1024U * 1024U;
            const auto bytes=read_vfs_asset(*virtual_file_system_,asset_vfs_catalog_,asset.id,
                {.byte_budget=maximum_mesh_artifact_bytes});
            if(!bytes.success||bytes.bytes.empty()) {
                last_error_=asset.id+": "+bytes.code+" - "+bytes.detail;
                return false;
            }
            const auto loaded = load_mesh_runtime_artifact(
                bytes.bytes,
                asset.id, {}, asset.content_hash);
            if (!loaded.success) {
                last_error_ = asset.id + ": " + loaded.code + " - " + loaded.detail;
                return false;
            }
            decoded = loaded.mesh;
            ++cooked_geometry_loads_;
        } else {
            const auto asset_path = asset_registry_.source_path(asset);
            decoded = asset.extension == ".fbx" ? decode_fbx_asset(asset_path) : decode_gltf_mesh(asset_path);
            ++source_geometry_decodes_;
        }
        if (!decoded.valid) {
            last_error_ = asset.id + ": " + decoded.code + " - " + decoded.detail;
            return false;
        }
        if (decoded.vertices.size() > std::numeric_limits<Uint32>::max() / sizeof(Vertex) ||
            decoded.indices.size() > std::numeric_limits<Uint32>::max() / sizeof(std::uint32_t)) {
            last_error_ = asset.id + ": decoded mesh exceeds SDL_GPU buffer limits";
            return false;
        }
        std::vector<Vertex> mesh_vertices;
        mesh_vertices.reserve(decoded.vertices.size());
        for (const auto& vertex : decoded.vertices) {
            mesh_vertices.push_back({
                {vertex.position[0], vertex.position[1], vertex.position[2]},
                {vertex.normal[0], vertex.normal[1], vertex.normal[2]},
                {vertex.texcoord[0], vertex.texcoord[1]},
                {vertex.tangent[0], vertex.tangent[1], vertex.tangent[2], vertex.tangent[3]},
                {static_cast<float>(vertex.joints[0]), static_cast<float>(vertex.joints[1]),
                 static_cast<float>(vertex.joints[2]), static_cast<float>(vertex.joints[3])},
                {vertex.weights[0], vertex.weights[1], vertex.weights[2], vertex.weights[3]}
            });
        }
        GpuMesh mesh;
        mesh.vertex_count = mesh_vertices.size(); mesh.index_count = decoded.indices.size();
        SDL_GPUBufferCreateInfo vertex_info{SDL_GPU_BUFFERUSAGE_VERTEX, static_cast<Uint32>(mesh_vertices.size() * sizeof(Vertex)), 0};
        SDL_GPUBufferCreateInfo index_info{SDL_GPU_BUFFERUSAGE_INDEX, static_cast<Uint32>(decoded.indices.size() * sizeof(std::uint32_t)), 0};
        mesh.vertex_buffer = SDL_CreateGPUBuffer(device_, &vertex_info);
        mesh.index_buffer = SDL_CreateGPUBuffer(device_, &index_info);
        if (!mesh.vertex_buffer || !mesh.index_buffer) {
            if (mesh.vertex_buffer) SDL_ReleaseGPUBuffer(device_, mesh.vertex_buffer);
            if (mesh.index_buffer) SDL_ReleaseGPUBuffer(device_, mesh.index_buffer);
            last_error_ = SDL_GetError(); return false;
        }
        const Uint32 vertex_bytes = vertex_info.size;
        const Uint32 index_bytes = index_info.size;
        SDL_GPUTransferBufferCreateInfo transfer_info{};
        transfer_info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
        transfer_info.size = vertex_bytes + index_bytes;
        SDL_GPUTransferBuffer* transfer = SDL_CreateGPUTransferBuffer(device_, &transfer_info);
        if (!transfer) { SDL_ReleaseGPUBuffer(device_, mesh.vertex_buffer); SDL_ReleaseGPUBuffer(device_, mesh.index_buffer); last_error_ = SDL_GetError(); return false; }
        auto* mapped = static_cast<std::byte*>(SDL_MapGPUTransferBuffer(device_, transfer, false));
        if (!mapped) { SDL_ReleaseGPUTransferBuffer(device_, transfer); SDL_ReleaseGPUBuffer(device_, mesh.vertex_buffer); SDL_ReleaseGPUBuffer(device_, mesh.index_buffer); last_error_ = SDL_GetError(); return false; }
        std::memcpy(mapped, mesh_vertices.data(), vertex_bytes);
        std::memcpy(mapped + vertex_bytes, decoded.indices.data(), index_bytes);
        SDL_UnmapGPUTransferBuffer(device_, transfer);
        SDL_GPUCommandBuffer* command = SDL_AcquireGPUCommandBuffer(device_);
        if (!command) { SDL_ReleaseGPUTransferBuffer(device_, transfer); SDL_ReleaseGPUBuffer(device_, mesh.vertex_buffer); SDL_ReleaseGPUBuffer(device_, mesh.index_buffer); last_error_ = SDL_GetError(); return false; }
        SDL_GPUCopyPass* pass = SDL_BeginGPUCopyPass(command);
        SDL_GPUTransferBufferLocation source{transfer, 0};
        SDL_GPUBufferRegion destination{mesh.vertex_buffer, 0, vertex_bytes};
        SDL_UploadToGPUBuffer(pass, &source, &destination, false);
        source.offset = vertex_bytes; destination = {mesh.index_buffer, 0, index_bytes};
        SDL_UploadToGPUBuffer(pass, &source, &destination, false);
        SDL_EndGPUCopyPass(pass);
        if (!SDL_SubmitGPUCommandBuffer(command)) {
            SDL_ReleaseGPUTransferBuffer(device_, transfer); SDL_ReleaseGPUBuffer(device_, mesh.vertex_buffer); SDL_ReleaseGPUBuffer(device_, mesh.index_buffer); last_error_ = SDL_GetError(); return false;
        }
        SDL_ReleaseGPUTransferBuffer(device_, transfer);
        mesh.textures_srgb.resize(decoded.images.size());
        mesh.textures_linear.resize(decoded.images.size());
        std::vector<bool> srgb_usage(decoded.images.size());
        std::vector<bool> linear_usage(decoded.images.size());
        const auto mark=[](std::vector<bool>& usage,const int image) {
            if (image>=0 && static_cast<std::size_t>(image)<usage.size()) usage[static_cast<std::size_t>(image)]=true;
        };
        for (const auto& primitive : decoded.primitives) {
            mark(srgb_usage,primitive.base_color_image); mark(srgb_usage,primitive.emissive_image);
            mark(linear_usage,primitive.normal_image); mark(linear_usage,primitive.metallic_roughness_image);
            mark(linear_usage,primitive.occlusion_image);
        }
        for (std::size_t image_index=0; image_index<decoded.images.size(); ++image_index) {
            const auto& image=decoded.images[image_index];
            if (!image.valid) continue;
            SDL_GPUTexture* srgb_texture{};SDL_GPUTexture* linear_texture{};
            const bool srgb_ok=!srgb_usage[image_index] || create_rgba8_texture(
                image.width,image.height,image.rgba8.data(),image.rgba8.size(),true,srgb_texture);
            const bool linear_ok=!linear_usage[image_index] || create_rgba8_texture(
                image.width,image.height,image.rgba8.data(),image.rgba8.size(),false,linear_texture);
            if (!srgb_ok || !linear_ok) {
                if(srgb_texture)SDL_ReleaseGPUTexture(device_,srgb_texture);
                if(linear_texture)SDL_ReleaseGPUTexture(device_,linear_texture);
                for (const auto handle : mesh.textures_srgb) if(handle.valid())
                    if(auto* texture=texture_resources_.remove(handle))SDL_ReleaseGPUTexture(device_,texture);
                for (const auto handle : mesh.textures_linear) if(handle.valid())
                    if(auto* texture=texture_resources_.remove(handle))SDL_ReleaseGPUTexture(device_,texture);
                SDL_ReleaseGPUBuffer(device_,mesh.vertex_buffer); SDL_ReleaseGPUBuffer(device_,mesh.index_buffer);
                last_error_=asset.id+": GPU texture upload failed - "+SDL_GetError(); return false;
            }
            const auto levels=1U+static_cast<std::uint32_t>(std::floor(std::log2(
                static_cast<double>(std::max(image.width,image.height)))));
            std::uint64_t bytes_estimate{};auto level_width=image.width;auto level_height=image.height;
            for(std::uint32_t level=0;level<levels;++level) {
                bytes_estimate+=static_cast<std::uint64_t>(level_width)*level_height*4U;
                level_width=std::max(1U,level_width/2U);level_height=std::max(1U,level_height/2U);
            }
            const auto stable_image_id=asset.id+"::image/"+std::to_string(image_index);
            if(srgb_texture)mesh.textures_srgb[image_index]=texture_resources_.acquire({
                .stable_id=stable_image_id,.semantic="pbr-srgb",.owner="scene.imported-pbr",
                .source=asset.relative_path,.residency="resident",
                .metadata={image.width,image.height,levels,0U,bytes_estimate}},srgb_texture);
            if(linear_texture)mesh.textures_linear[image_index]=texture_resources_.acquire({
                .stable_id=stable_image_id,.semantic="pbr-linear-data",.owner="scene.imported-pbr",
                .source=asset.relative_path,.residency="resident",
                .metadata={image.width,image.height,levels,0U,bytes_estimate}},linear_texture);
            if((srgb_texture&&!mesh.textures_srgb[image_index].valid())||
               (linear_texture&&!mesh.textures_linear[image_index].valid())) {
                if(srgb_texture&&!mesh.textures_srgb[image_index].valid())SDL_ReleaseGPUTexture(device_,srgb_texture);
                if(linear_texture&&!mesh.textures_linear[image_index].valid())SDL_ReleaseGPUTexture(device_,linear_texture);
                for (const auto handle : mesh.textures_srgb) if(handle.valid())
                    if(auto* texture=texture_resources_.remove(handle))SDL_ReleaseGPUTexture(device_,texture);
                for (const auto handle : mesh.textures_linear) if(handle.valid())
                    if(auto* texture=texture_resources_.remove(handle))SDL_ReleaseGPUTexture(device_,texture);
                SDL_ReleaseGPUBuffer(device_,mesh.vertex_buffer);SDL_ReleaseGPUBuffer(device_,mesh.index_buffer);
                last_error_=asset.id+": stable imported texture registration failed";return false;
            }
            imported_textures_+=static_cast<std::size_t>(srgb_usage[image_index])+static_cast<std::size_t>(linear_usage[image_index]);
        }
        mesh.primitives.reserve(decoded.primitives.size());
        for (const auto& primitive : decoded.primitives) {
            GpuPrimitive gpu;
            gpu.first_index=primitive.first_index; gpu.index_count=primitive.index_count; gpu.base_color=primitive.base_color;
            gpu.metallic=primitive.metallic; gpu.roughness=primitive.roughness; gpu.unlit=primitive.unlit;
            gpu.base_color_image=primitive.base_color_image; gpu.normal_image=primitive.normal_image;
            gpu.metallic_roughness_image=primitive.metallic_roughness_image; gpu.occlusion_image=primitive.occlusion_image;
            gpu.emissive_image=primitive.emissive_image; gpu.emissive_factor=primitive.emissive_factor;
            gpu.normal_scale=primitive.normal_scale; gpu.occlusion_strength=primitive.occlusion_strength;
            gpu.alpha_cutoff=primitive.alpha_cutoff; gpu.alpha_mode=primitive.alpha_mode; gpu.double_sided=primitive.double_sided;
            gpu.skin=primitive.skin;
            gpu.bounds_center=primitive.bounds_center; gpu.bounds_radius=primitive.bounds_radius;
            if (gpu.normal_image>=0) ++normal_mapped_primitives_;
            if (gpu.metallic_roughness_image>=0) ++metallic_roughness_mapped_primitives_;
            if (gpu.occlusion_image>=0) ++occlusion_mapped_primitives_;
            if (gpu.emissive_image>=0) ++emissive_mapped_primitives_;
            if (gpu.alpha_mode=="MASK") ++alpha_masked_primitives_;
            if (gpu.alpha_mode=="BLEND") ++alpha_blended_primitives_;
            if (gpu.double_sided) ++double_sided_primitives_;
            mesh.primitives.push_back(std::move(gpu));
        }
        imported_primitives_ += mesh.primitives.size();
        raytracing_geometries_.insert_or_assign(
            asset.id,make_scene_raytracing_geometry_input(asset.id,decoded));
        gpu_meshes_.emplace(asset.id, std::move(mesh));
        gpu_batch_resource_keys_.clear();
        gpu_driven_cached_plan_valid_=false;
    }
    imported_meshes_ = gpu_meshes_.size();
    return true;
}

bool SceneRenderer::create_pipelines() {
    shader_artifact_failure.clear();
    depth_pyramid_seed_pipeline_=load_screen_space_compute_pipeline(device_,"depth_pyramid_seed.comp");
    depth_pyramid_reduce_pipeline_=load_screen_space_compute_pipeline(device_,"depth_pyramid_reduce.comp");
    if(!depth_pyramid_seed_pipeline_||!depth_pyramid_reduce_pipeline_) {
        last_error_="Unable to create shared depth-pyramid pipelines: "+
            (shader_artifact_failure.empty()?std::string(SDL_GetError()):shader_artifact_failure);
        return false;
    }
    SDL_GPUShader* lit_vertex = load_shader(device_, "scene_lit.vert", SDL_GPU_SHADERSTAGE_VERTEX, 0, 4);
    SDL_GPUShader* gpu_driven_lit_vertex=load_shader(device_,"scene_gpu_driven.vert",SDL_GPU_SHADERSTAGE_VERTEX,0,1,2);
    SDL_GPUShader* lit_fragment = load_shader(device_, "scene_lit.frag", SDL_GPU_SHADERSTAGE_FRAGMENT, 10, 1, 3);
    SDL_GPUShader* sprite_vertex = load_shader(device_, "sprite.vert", SDL_GPU_SHADERSTAGE_VERTEX, 0, 1, 2);
    SDL_GPUShader* sprite_fragment = load_shader(device_, "sprite.frag", SDL_GPU_SHADERSTAGE_FRAGMENT, 6, 1, 3);
    SDL_GPUShader* sprite_shadow_vertex = load_shader(device_, "sprite_shadow.vert", SDL_GPU_SHADERSTAGE_VERTEX, 0, 1, 2);
    SDL_GPUShader* shadow_vertex = load_shader(device_, "shadow_depth.vert", SDL_GPU_SHADERSTAGE_VERTEX, 0, 3);
    SDL_GPUShader* shadow_fragment = load_shader(device_, "shadow_depth.frag", SDL_GPU_SHADERSTAGE_FRAGMENT, 1, 0);
    SDL_GPUShader* tone_vertex = load_shader(device_, "tone_map.vert", SDL_GPU_SHADERSTAGE_VERTEX, 0, 0);
    SDL_GPUShader* tone_fragment = load_shader(device_, "tone_map.frag", SDL_GPU_SHADERSTAGE_FRAGMENT, 3, 1);
    SDL_GPUShader* native_rt_composite_vertex=load_shader(
        device_,"native_rt_composite.vert",SDL_GPU_SHADERSTAGE_VERTEX,0,0);
    SDL_GPUShader* native_rt_composite_fragment=load_shader(
        device_,"native_rt_composite.frag",SDL_GPU_SHADERSTAGE_FRAGMENT,1,1);
    SDL_GPUShader* sky_vertex = load_shader(device_, "sky_atmosphere.vert", SDL_GPU_SHADERSTAGE_VERTEX, 0, 0);
    SDL_GPUShader* sky_fragment = load_shader(device_, "sky_atmosphere.frag", SDL_GPU_SHADERSTAGE_FRAGMENT, 1, 1);
    SDL_GPUShader* sky_analytic_fragment=load_shader(device_,"sky_atmosphere_analytic.frag",SDL_GPU_SHADERSTAGE_FRAGMENT,0,1);
    SDL_GPUShader* aerial_fragment=load_shader(device_,"aerial_perspective.frag",SDL_GPU_SHADERSTAGE_FRAGMENT,3,1);
    SDL_GPUShader* gtao_fragment = load_shader(device_, "gtao.frag", SDL_GPU_SHADERSTAGE_FRAGMENT, 2, 1);
    SDL_GPUShader* ao_denoise_fragment = load_shader(device_, "ao_denoise.frag", SDL_GPU_SHADERSTAGE_FRAGMENT, 3, 1);
    SDL_GPUShader* ao_composite_fragment = load_shader(device_, "ao_composite.frag", SDL_GPU_SHADERSTAGE_FRAGMENT, 3, 0);
    SDL_GPUShader* auto_exposure_fragment = load_shader(device_, "auto_exposure.frag", SDL_GPU_SHADERSTAGE_FRAGMENT, 2, 1);
    SDL_GPUShader* bloom_downsample_fragment=load_shader(device_,"bloom_downsample.frag",SDL_GPU_SHADERSTAGE_FRAGMENT,1,1);
    SDL_GPUShader* bloom_upsample_fragment=load_shader(device_,"bloom_upsample.frag",SDL_GPU_SHADERSTAGE_FRAGMENT,2,1);
    SDL_GPUShader* fxaa_vertex = load_shader(device_, "fxaa.vert", SDL_GPU_SHADERSTAGE_VERTEX, 0, 0);
    SDL_GPUShader* fxaa_fragment = load_shader(device_, "fxaa.frag", SDL_GPU_SHADERSTAGE_FRAGMENT, 1, 1);
    SDL_GPUShader* taa_vertex = load_shader(device_, "taa.vert", SDL_GPU_SHADERSTAGE_VERTEX, 0, 0);
    SDL_GPUShader* taa_fragment = load_shader(device_, "temporal_denoise.frag", SDL_GPU_SHADERSTAGE_FRAGMENT, 8, 1);
    SDL_GPUShader* ssr_trace_fragment=load_shader(device_,"ssr_hiz_trace.frag",SDL_GPU_SHADERSTAGE_FRAGMENT,5,1);
    SDL_GPUShader* ssr_temporal_fragment=load_shader(device_,"ssr_temporal_resolve.frag",SDL_GPU_SHADERSTAGE_FRAGMENT,8,1);
    SDL_GPUShader* ssr_composite_fragment=load_shader(device_,"ssr_composite.frag",SDL_GPU_SHADERSTAGE_FRAGMENT,6,1);
    SDL_GPUShader* ssgi_gather_fragment=load_shader(device_,"ssgi_hiz_gather.frag",SDL_GPU_SHADERSTAGE_FRAGMENT,5,1);
    SDL_GPUShader* ssgi_spatial_fragment=load_shader(device_,"ssgi_spatial_resolve.frag",SDL_GPU_SHADERSTAGE_FRAGMENT,5,1);
    SDL_GPUShader* ssgi_temporal_fragment=load_shader(device_,"ssgi_temporal_resolve.frag",SDL_GPU_SHADERSTAGE_FRAGMENT,8,1);
    SDL_GPUShader* ssgi_composite_fragment=load_shader(device_,"ssgi_composite.frag",SDL_GPU_SHADERSTAGE_FRAGMENT,6,1);
    SDL_GPUShader* vfx_vertex = load_shader(device_, "vfx_billboard.vert", SDL_GPU_SHADERSTAGE_VERTEX, 0, 1, 2);
    SDL_GPUShader* vfx_fragment = load_shader(device_, "vfx_billboard.frag", SDL_GPU_SHADERSTAGE_FRAGMENT, 0, 0);
    if (!lit_vertex || !lit_fragment || !sprite_vertex || !sprite_fragment || !sprite_shadow_vertex || !shadow_vertex || !shadow_fragment || !tone_vertex || !tone_fragment || !native_rt_composite_vertex || !native_rt_composite_fragment || !sky_vertex || !sky_fragment || !sky_analytic_fragment || !aerial_fragment || !gtao_fragment || !ao_denoise_fragment || !ao_composite_fragment || !auto_exposure_fragment || !bloom_downsample_fragment || !bloom_upsample_fragment || !fxaa_vertex || !fxaa_fragment || !taa_vertex || !taa_fragment || !ssr_trace_fragment || !ssr_temporal_fragment || !ssr_composite_fragment || !ssgi_gather_fragment || !ssgi_spatial_fragment || !ssgi_temporal_fragment || !ssgi_composite_fragment || !vfx_vertex || !vfx_fragment) {
        last_error_ = "Unable to load compiled shaders for GPU backend " + gpu_backend_ + " using " + shader_artifact_format_;
        if(!shader_artifact_failure.empty())last_error_+="; "+shader_artifact_failure;
        if (lit_vertex) SDL_ReleaseGPUShader(device_, lit_vertex);
        if (gpu_driven_lit_vertex) SDL_ReleaseGPUShader(device_,gpu_driven_lit_vertex);
        if (lit_fragment) SDL_ReleaseGPUShader(device_, lit_fragment);
        if (sprite_vertex) SDL_ReleaseGPUShader(device_,sprite_vertex);
        if (sprite_fragment) SDL_ReleaseGPUShader(device_,sprite_fragment);
        if (sprite_shadow_vertex) SDL_ReleaseGPUShader(device_,sprite_shadow_vertex);
        if (shadow_vertex) SDL_ReleaseGPUShader(device_, shadow_vertex);
        if (shadow_fragment) SDL_ReleaseGPUShader(device_, shadow_fragment);
        if (tone_vertex) SDL_ReleaseGPUShader(device_, tone_vertex);
        if (tone_fragment) SDL_ReleaseGPUShader(device_, tone_fragment);
        if(native_rt_composite_vertex)SDL_ReleaseGPUShader(device_,native_rt_composite_vertex);
        if(native_rt_composite_fragment)SDL_ReleaseGPUShader(device_,native_rt_composite_fragment);
        if (sky_vertex) SDL_ReleaseGPUShader(device_, sky_vertex);
        if (sky_fragment) SDL_ReleaseGPUShader(device_, sky_fragment);
        if(sky_analytic_fragment)SDL_ReleaseGPUShader(device_,sky_analytic_fragment);
        if(aerial_fragment)SDL_ReleaseGPUShader(device_,aerial_fragment);
        if (gtao_fragment) SDL_ReleaseGPUShader(device_, gtao_fragment);
        if (ao_denoise_fragment) SDL_ReleaseGPUShader(device_, ao_denoise_fragment);
        if (ao_composite_fragment) SDL_ReleaseGPUShader(device_, ao_composite_fragment);
        if (auto_exposure_fragment) SDL_ReleaseGPUShader(device_, auto_exposure_fragment);
        if(bloom_downsample_fragment)SDL_ReleaseGPUShader(device_,bloom_downsample_fragment);
        if(bloom_upsample_fragment)SDL_ReleaseGPUShader(device_,bloom_upsample_fragment);
        if (fxaa_vertex) SDL_ReleaseGPUShader(device_, fxaa_vertex);
        if (fxaa_fragment) SDL_ReleaseGPUShader(device_, fxaa_fragment);
        if (taa_vertex) SDL_ReleaseGPUShader(device_,taa_vertex);
        if (taa_fragment) SDL_ReleaseGPUShader(device_,taa_fragment);
        if(ssr_trace_fragment)SDL_ReleaseGPUShader(device_,ssr_trace_fragment);
        if(ssr_temporal_fragment)SDL_ReleaseGPUShader(device_,ssr_temporal_fragment);
        if(ssr_composite_fragment)SDL_ReleaseGPUShader(device_,ssr_composite_fragment);
        if(ssgi_gather_fragment)SDL_ReleaseGPUShader(device_,ssgi_gather_fragment);
        if(ssgi_spatial_fragment)SDL_ReleaseGPUShader(device_,ssgi_spatial_fragment);
        if(ssgi_temporal_fragment)SDL_ReleaseGPUShader(device_,ssgi_temporal_fragment);
        if(ssgi_composite_fragment)SDL_ReleaseGPUShader(device_,ssgi_composite_fragment);
        if (vfx_vertex) SDL_ReleaseGPUShader(device_,vfx_vertex);
        if (vfx_fragment) SDL_ReleaseGPUShader(device_,vfx_fragment);
        return false;
    }
    SDL_GPUVertexBufferDescription buffer_description{0, sizeof(Vertex), SDL_GPU_VERTEXINPUTRATE_VERTEX, 0};
    std::array<SDL_GPUVertexAttribute,6> attributes{{
        {0,0,SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3,0},
        {1,0,SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3,12},
        {2,0,SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2,24},
        {3,0,SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4,32},
        {4,0,SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4,48},
        {5,0,SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4,64}
    }};
    std::array<SDL_GPUColorTargetDescription,8> color_descriptions{};
    color_descriptions[0].format = normal_format;
    color_descriptions[1].format = object_id_format;
    color_descriptions[2].format = normal_format;
    color_descriptions[3].format = motion_format;
    color_descriptions[4].format = reactive_mask_format;
    color_descriptions[5].format = normal_format;
    color_descriptions[6].format = normal_format;
    color_descriptions[7].format = normal_format;
    SDL_GPUGraphicsPipelineCreateInfo info{};
    info.vertex_shader=lit_vertex; info.fragment_shader=lit_fragment;
    info.vertex_input_state={&buffer_description,1,attributes.data(),static_cast<Uint32>(attributes.size())};
    info.primitive_type=SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
    info.rasterizer_state.cull_mode=SDL_GPU_CULLMODE_BACK;
    info.rasterizer_state.front_face=SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE;
    info.rasterizer_state.enable_depth_clip=true;
    info.depth_stencil_state.compare_op=SDL_GPU_COMPAREOP_LESS_OR_EQUAL;
    info.depth_stencil_state.enable_depth_test=true; info.depth_stencil_state.enable_depth_write=true;
    info.target_info={color_descriptions.data(),static_cast<Uint32>(color_descriptions.size()),depth_format,true,0,0,0};
    lit_pipeline_=SDL_CreateGPUGraphicsPipeline(device_, &info);
    info.rasterizer_state.cull_mode=SDL_GPU_CULLMODE_NONE;
    lit_double_sided_pipeline_=SDL_CreateGPUGraphicsPipeline(device_, &info);
    if(gpu_driven_lit_vertex&&gpu_visibility_pipeline_) {
        info.vertex_shader=gpu_driven_lit_vertex;
        info.rasterizer_state.cull_mode=SDL_GPU_CULLMODE_BACK;
        gpu_driven_lit_pipeline_=SDL_CreateGPUGraphicsPipeline(device_,&info);
        info.rasterizer_state.cull_mode=SDL_GPU_CULLMODE_NONE;
        gpu_driven_lit_double_sided_pipeline_=SDL_CreateGPUGraphicsPipeline(device_,&info);
    }
    info.vertex_shader=lit_vertex;

    color_descriptions[0].blend_state.src_color_blendfactor=SDL_GPU_BLENDFACTOR_SRC_ALPHA;
    color_descriptions[0].blend_state.dst_color_blendfactor=SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
    color_descriptions[0].blend_state.color_blend_op=SDL_GPU_BLENDOP_ADD;
    color_descriptions[0].blend_state.src_alpha_blendfactor=SDL_GPU_BLENDFACTOR_ONE;
    color_descriptions[0].blend_state.dst_alpha_blendfactor=SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
    color_descriptions[0].blend_state.alpha_blend_op=SDL_GPU_BLENDOP_ADD;
    color_descriptions[0].blend_state.enable_blend=true;
    info.depth_stencil_state.enable_depth_write=false;
    transparent_double_sided_pipeline_=SDL_CreateGPUGraphicsPipeline(device_,&info);
    info.rasterizer_state.cull_mode=SDL_GPU_CULLMODE_BACK;
    transparent_pipeline_=SDL_CreateGPUGraphicsPipeline(device_,&info);

    SDL_GPUGraphicsPipelineCreateInfo sprite_info{};
    sprite_info.vertex_shader=sprite_vertex;sprite_info.fragment_shader=sprite_fragment;
    sprite_info.primitive_type=SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
    sprite_info.rasterizer_state.cull_mode=SDL_GPU_CULLMODE_NONE;
    sprite_info.rasterizer_state.enable_depth_clip=true;
    sprite_info.depth_stencil_state.compare_op=SDL_GPU_COMPAREOP_LESS_OR_EQUAL;
    sprite_info.depth_stencil_state.enable_depth_test=true;sprite_info.depth_stencil_state.enable_depth_write=true;
    std::array<SDL_GPUColorTargetDescription,8> sprite_targets{};
    sprite_targets[0].format=normal_format;sprite_targets[1].format=object_id_format;sprite_targets[2].format=normal_format;
    sprite_targets[3].format=motion_format;sprite_targets[4].format=reactive_mask_format;sprite_targets[5].format=normal_format;
    sprite_targets[6].format=normal_format;sprite_targets[7].format=normal_format;
    sprite_info.target_info={sprite_targets.data(),static_cast<Uint32>(sprite_targets.size()),depth_format,true,0,0,0};
    sprite_cutout_pipeline_=SDL_CreateGPUGraphicsPipeline(device_,&sprite_info);
    if(!sprite_cutout_pipeline_&&last_error_.empty())last_error_="Unable to create Sprite cutout pipeline: "+std::string(SDL_GetError());
    sprite_targets[0].blend_state.src_color_blendfactor=SDL_GPU_BLENDFACTOR_SRC_ALPHA;
    sprite_targets[0].blend_state.dst_color_blendfactor=SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
    sprite_targets[0].blend_state.color_blend_op=SDL_GPU_BLENDOP_ADD;
    sprite_targets[0].blend_state.src_alpha_blendfactor=SDL_GPU_BLENDFACTOR_ONE;
    sprite_targets[0].blend_state.dst_alpha_blendfactor=SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
    sprite_targets[0].blend_state.alpha_blend_op=SDL_GPU_BLENDOP_ADD;sprite_targets[0].blend_state.enable_blend=true;
    sprite_info.depth_stencil_state.enable_depth_write=false;
    sprite_info.target_info={sprite_targets.data(),static_cast<Uint32>(sprite_targets.size()),depth_format,true,0,0,0};
    sprite_alpha_pipeline_=SDL_CreateGPUGraphicsPipeline(device_,&sprite_info);
    if(!sprite_alpha_pipeline_&&last_error_.empty())last_error_="Unable to create Sprite alpha pipeline: "+std::string(SDL_GetError());

    SDL_GPUGraphicsPipelineCreateInfo sprite_shadow_info{};
    sprite_shadow_info.vertex_shader=sprite_shadow_vertex;sprite_shadow_info.fragment_shader=shadow_fragment;
    sprite_shadow_info.primitive_type=SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
    sprite_shadow_info.rasterizer_state.cull_mode=SDL_GPU_CULLMODE_NONE;
    sprite_shadow_info.rasterizer_state.enable_depth_clip=true;
    sprite_shadow_info.rasterizer_state.enable_depth_bias=true;
    sprite_shadow_info.rasterizer_state.depth_bias_constant_factor=2.0F;
    sprite_shadow_info.rasterizer_state.depth_bias_slope_factor=2.0F;
    sprite_shadow_info.depth_stencil_state.compare_op=SDL_GPU_COMPAREOP_LESS_OR_EQUAL;
    sprite_shadow_info.depth_stencil_state.enable_depth_test=true;
    sprite_shadow_info.depth_stencil_state.enable_depth_write=true;
    sprite_shadow_info.target_info={nullptr,0,depth_format,true,0,0,0};
    sprite_shadow_pipeline_=SDL_CreateGPUGraphicsPipeline(device_,&sprite_shadow_info);
    if(!sprite_shadow_pipeline_&&last_error_.empty())last_error_="Unable to create Sprite shadow pipeline: "+std::string(SDL_GetError());

    SDL_GPUGraphicsPipelineCreateInfo vfx_info{};
    vfx_info.vertex_shader=vfx_vertex; vfx_info.fragment_shader=vfx_fragment;
    vfx_info.primitive_type=SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
    vfx_info.rasterizer_state.cull_mode=SDL_GPU_CULLMODE_NONE;
    vfx_info.rasterizer_state.enable_depth_clip=true;
    vfx_info.depth_stencil_state.compare_op=SDL_GPU_COMPAREOP_LESS_OR_EQUAL;
    vfx_info.depth_stencil_state.enable_depth_test=true;
    vfx_info.depth_stencil_state.enable_depth_write=false;
    vfx_info.target_info={color_descriptions.data(),static_cast<Uint32>(color_descriptions.size()),depth_format,true,0,0,0};
    vfx_alpha_draw_pipeline_=SDL_CreateGPUGraphicsPipeline(device_,&vfx_info);
    color_descriptions[0].blend_state.src_color_blendfactor=SDL_GPU_BLENDFACTOR_SRC_ALPHA;
    color_descriptions[0].blend_state.dst_color_blendfactor=SDL_GPU_BLENDFACTOR_ONE;
    color_descriptions[0].blend_state.src_alpha_blendfactor=SDL_GPU_BLENDFACTOR_ONE;
    color_descriptions[0].blend_state.dst_alpha_blendfactor=SDL_GPU_BLENDFACTOR_ONE;
    vfx_info.target_info={color_descriptions.data(),static_cast<Uint32>(color_descriptions.size()),depth_format,true,0,0,0};
    vfx_additive_draw_pipeline_=SDL_CreateGPUGraphicsPipeline(device_,&vfx_info);

    info.vertex_shader=shadow_vertex; info.fragment_shader=shadow_fragment;
    info.depth_stencil_state.enable_depth_write=true;
    info.target_info={nullptr,0,depth_format,true,0,0,0};
    info.rasterizer_state.cull_mode=SDL_GPU_CULLMODE_FRONT;
    info.rasterizer_state.enable_depth_bias=true;
    info.rasterizer_state.depth_bias_constant_factor=2.0F;
    info.rasterizer_state.depth_bias_slope_factor=2.0F;
    shadow_pipeline_=SDL_CreateGPUGraphicsPipeline(device_, &info);
    info.rasterizer_state.cull_mode=SDL_GPU_CULLMODE_NONE;
    shadow_double_sided_pipeline_=SDL_CreateGPUGraphicsPipeline(device_, &info);

    SDL_GPUColorTargetDescription tone_target{}; tone_target.format=color_format;
    SDL_GPUGraphicsPipelineCreateInfo tone_info{};
    tone_info.vertex_shader=tone_vertex; tone_info.fragment_shader=tone_fragment;
    tone_info.primitive_type=SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
    tone_info.rasterizer_state.cull_mode=SDL_GPU_CULLMODE_NONE;
    tone_info.target_info={&tone_target,1,SDL_GPU_TEXTUREFORMAT_INVALID,false,0,0,0};
    tone_map_pipeline_=SDL_CreateGPUGraphicsPipeline(device_,&tone_info);
    tone_info.vertex_shader=native_rt_composite_vertex;
    tone_info.fragment_shader=native_rt_composite_fragment;
    native_rt_composite_pipeline_=SDL_CreateGPUGraphicsPipeline(device_,&tone_info);
    tone_target.format=normal_format;
    tone_info.vertex_shader=sky_vertex;tone_info.fragment_shader=sky_fragment;
    sky_atmosphere_pipeline_=SDL_CreateGPUGraphicsPipeline(device_,&tone_info);
    tone_info.fragment_shader=sky_analytic_fragment;
    sky_atmosphere_analytic_pipeline_=SDL_CreateGPUGraphicsPipeline(device_,&tone_info);
    tone_info.vertex_shader=tone_vertex;
    tone_info.fragment_shader=aerial_fragment;
    aerial_perspective_pipeline_=SDL_CreateGPUGraphicsPipeline(device_,&tone_info);
    tone_target.format=ambient_occlusion_format;
    tone_info.fragment_shader=gtao_fragment;
    gtao_pipeline_=SDL_CreateGPUGraphicsPipeline(device_,&tone_info);
    tone_info.fragment_shader=ao_denoise_fragment;
    ao_denoise_pipeline_=SDL_CreateGPUGraphicsPipeline(device_,&tone_info);
    tone_target.format=normal_format;
    tone_info.fragment_shader=ao_composite_fragment;
    ao_composite_pipeline_=SDL_CreateGPUGraphicsPipeline(device_,&tone_info);
    tone_target.format=exposure_format;
    tone_info.fragment_shader=auto_exposure_fragment;
    auto_exposure_pipeline_=SDL_CreateGPUGraphicsPipeline(device_,&tone_info);
    tone_target.format=normal_format;
    tone_info.vertex_shader=tone_vertex;tone_info.fragment_shader=bloom_downsample_fragment;
    bloom_downsample_pipeline_=SDL_CreateGPUGraphicsPipeline(device_,&tone_info);
    tone_info.fragment_shader=bloom_upsample_fragment;
    bloom_upsample_pipeline_=SDL_CreateGPUGraphicsPipeline(device_,&tone_info);
    tone_target.format=color_format;
    tone_info.vertex_shader=fxaa_vertex; tone_info.fragment_shader=fxaa_fragment;
    fxaa_pipeline_=SDL_CreateGPUGraphicsPipeline(device_,&tone_info);
    std::array<SDL_GPUColorTargetDescription,4> taa_targets{};
    taa_targets[0].format=normal_format;taa_targets[1].format=normal_format;
    taa_targets[2].format=history_depth_format;taa_targets[3].format=normal_format;
    tone_info.vertex_shader=taa_vertex; tone_info.fragment_shader=taa_fragment;
    tone_info.target_info={taa_targets.data(),static_cast<Uint32>(taa_targets.size()),SDL_GPU_TEXTUREFORMAT_INVALID,false,0,0,0};
    taa_pipeline_=SDL_CreateGPUGraphicsPipeline(device_,&tone_info);
    SDL_GPUColorTargetDescription ssr_target{};ssr_target.format=normal_format;
    tone_info.vertex_shader=taa_vertex;tone_info.fragment_shader=ssr_trace_fragment;
    tone_info.target_info={&ssr_target,1,SDL_GPU_TEXTUREFORMAT_INVALID,false,0,0,0};
    ssr_trace_pipeline_=SDL_CreateGPUGraphicsPipeline(device_,&tone_info);
    std::array<SDL_GPUColorTargetDescription,2> ssr_temporal_targets{};
    for(auto& target:ssr_temporal_targets)target.format=normal_format;
    tone_info.fragment_shader=ssr_temporal_fragment;
    tone_info.target_info={ssr_temporal_targets.data(),static_cast<Uint32>(ssr_temporal_targets.size()),SDL_GPU_TEXTUREFORMAT_INVALID,false,0,0,0};
    ssr_temporal_pipeline_=SDL_CreateGPUGraphicsPipeline(device_,&tone_info);
    tone_info.fragment_shader=ssr_composite_fragment;
    tone_info.target_info={&ssr_target,1,SDL_GPU_TEXTUREFORMAT_INVALID,false,0,0,0};
    ssr_composite_pipeline_=SDL_CreateGPUGraphicsPipeline(device_,&tone_info);
    std::array<SDL_GPUColorTargetDescription,2> ssgi_pair_targets{};
    for(auto& target:ssgi_pair_targets)target.format=normal_format;
    tone_info.fragment_shader=ssgi_gather_fragment;
    tone_info.target_info={ssgi_pair_targets.data(),static_cast<Uint32>(ssgi_pair_targets.size()),SDL_GPU_TEXTUREFORMAT_INVALID,false,0,0,0};
    ssgi_gather_pipeline_=SDL_CreateGPUGraphicsPipeline(device_,&tone_info);
    tone_info.fragment_shader=ssgi_spatial_fragment;
    ssgi_spatial_pipeline_=SDL_CreateGPUGraphicsPipeline(device_,&tone_info);
    std::array<SDL_GPUColorTargetDescription,4> ssgi_temporal_targets{};
    for(auto& target:ssgi_temporal_targets)target.format=normal_format;
    tone_info.fragment_shader=ssgi_temporal_fragment;
    tone_info.target_info={ssgi_temporal_targets.data(),static_cast<Uint32>(ssgi_temporal_targets.size()),SDL_GPU_TEXTUREFORMAT_INVALID,false,0,0,0};
    ssgi_temporal_pipeline_=SDL_CreateGPUGraphicsPipeline(device_,&tone_info);
    tone_info.fragment_shader=ssgi_composite_fragment;
    tone_info.target_info={&ssr_target,1,SDL_GPU_TEXTUREFORMAT_INVALID,false,0,0,0};
    ssgi_composite_pipeline_=SDL_CreateGPUGraphicsPipeline(device_,&tone_info);
    SDL_ReleaseGPUShader(device_,lit_vertex);if(gpu_driven_lit_vertex)SDL_ReleaseGPUShader(device_,gpu_driven_lit_vertex);SDL_ReleaseGPUShader(device_,lit_fragment);
    SDL_ReleaseGPUShader(device_,sprite_vertex);SDL_ReleaseGPUShader(device_,sprite_fragment);SDL_ReleaseGPUShader(device_,sprite_shadow_vertex);
    SDL_ReleaseGPUShader(device_, shadow_vertex); SDL_ReleaseGPUShader(device_, shadow_fragment);
    SDL_ReleaseGPUShader(device_, tone_vertex); SDL_ReleaseGPUShader(device_, tone_fragment);
    SDL_ReleaseGPUShader(device_,native_rt_composite_vertex);
    SDL_ReleaseGPUShader(device_,native_rt_composite_fragment);
    SDL_ReleaseGPUShader(device_,sky_vertex);SDL_ReleaseGPUShader(device_,sky_fragment);
    SDL_ReleaseGPUShader(device_,sky_analytic_fragment);SDL_ReleaseGPUShader(device_,aerial_fragment);
    SDL_ReleaseGPUShader(device_,gtao_fragment);SDL_ReleaseGPUShader(device_,ao_denoise_fragment);SDL_ReleaseGPUShader(device_,ao_composite_fragment);SDL_ReleaseGPUShader(device_,auto_exposure_fragment);
    SDL_ReleaseGPUShader(device_,bloom_downsample_fragment);SDL_ReleaseGPUShader(device_,bloom_upsample_fragment);
    SDL_ReleaseGPUShader(device_, fxaa_vertex); SDL_ReleaseGPUShader(device_, fxaa_fragment);
    SDL_ReleaseGPUShader(device_,taa_vertex); SDL_ReleaseGPUShader(device_,taa_fragment);
    SDL_ReleaseGPUShader(device_,ssr_trace_fragment);SDL_ReleaseGPUShader(device_,ssr_temporal_fragment);
    SDL_ReleaseGPUShader(device_,ssr_composite_fragment);
    SDL_ReleaseGPUShader(device_,ssgi_gather_fragment);SDL_ReleaseGPUShader(device_,ssgi_spatial_fragment);
    SDL_ReleaseGPUShader(device_,ssgi_temporal_fragment);SDL_ReleaseGPUShader(device_,ssgi_composite_fragment);
    SDL_ReleaseGPUShader(device_,vfx_vertex); SDL_ReleaseGPUShader(device_,vfx_fragment);
    return lit_pipeline_ && lit_double_sided_pipeline_ && transparent_pipeline_ && transparent_double_sided_pipeline_ &&
        sprite_cutout_pipeline_ && sprite_alpha_pipeline_ && sprite_shadow_pipeline_ &&
        shadow_pipeline_ && shadow_double_sided_pipeline_ && bloom_downsample_pipeline_ && bloom_upsample_pipeline_ && gtao_pipeline_ && ao_denoise_pipeline_ && ao_composite_pipeline_ &&
        auto_exposure_pipeline_ && tone_map_pipeline_ && native_rt_composite_pipeline_ && sky_atmosphere_pipeline_ && sky_atmosphere_analytic_pipeline_ && aerial_perspective_pipeline_ && fxaa_pipeline_ && taa_pipeline_ &&
        ssr_trace_pipeline_ && ssr_temporal_pipeline_ && ssr_composite_pipeline_ &&
        ssgi_gather_pipeline_ && ssgi_spatial_pipeline_ && ssgi_temporal_pipeline_ && ssgi_composite_pipeline_ &&
        vfx_alpha_draw_pipeline_ && vfx_additive_draw_pipeline_;
}

bool SceneRenderer::create_targets(const std::uint32_t width, const std::uint32_t height) {
    depth_pyramid_history_ready_=false;
    const bool hybrid_pixel = hybrid_pixel_active();
    if (hybrid_pixel) {
        pixel_presentation_ = plan_pixel_presentation({
            .virtual_extent = {hybrid_pixel_profile_->virtual_width, hybrid_pixel_profile_->virtual_height},
            .physical_output_extent = {width, height}});
        if (!pixel_presentation_.valid) {
            last_error_ = pixel_presentation_.code + ": " + pixel_presentation_.detail;
            return false;
        }
        post_width_ = hybrid_pixel_profile_->virtual_width;
        post_height_ = hybrid_pixel_profile_->virtual_height;
        render_width_ = post_width_;
        render_height_ = post_height_;
    } else {
        pixel_presentation_ = {};
        post_width_ = width;
        post_height_ = height;
        const auto extent = temporal_render_extent(post_width_, post_height_, render_scale_);
        render_width_ = extent.render_width;
        render_height_ = extent.render_height;
    }
    SDL_GPUTextureCreateInfo info{};
    info.type=SDL_GPU_TEXTURETYPE_2D; info.layer_count_or_depth=1; info.num_levels=1; info.sample_count=SDL_GPU_SAMPLECOUNT_1;
    info.format=color_format; info.usage=SDL_GPU_TEXTUREUSAGE_COLOR_TARGET|SDL_GPU_TEXTUREUSAGE_SAMPLER; info.width=width; info.height=height;
    color_texture_=SDL_CreateGPUTexture(device_, &info);
    info.width=post_width_; info.height=post_height_;
    info.format=color_format; info.usage=SDL_GPU_TEXTUREUSAGE_COLOR_TARGET|SDL_GPU_TEXTUREUSAGE_SAMPLER; tone_mapped_texture_=SDL_CreateGPUTexture(device_,&info);
    info.format=normal_format; info.usage=SDL_GPU_TEXTUREUSAGE_COLOR_TARGET|SDL_GPU_TEXTUREUSAGE_SAMPLER;
    taa_resolved_texture_=SDL_CreateGPUTexture(device_,&info);
    taa_history_textures_[0]=SDL_CreateGPUTexture(device_,&info); taa_history_textures_[1]=SDL_CreateGPUTexture(device_,&info);
    bloom_working_set_bytes_=0;
    const auto create_bloom_target=[&](const std::uint32_t divisor) {
        info.width=std::max(1U,post_width_/divisor);info.height=std::max(1U,post_height_/divisor);
        bloom_working_set_bytes_+=static_cast<std::uint64_t>(info.width)*info.height*8U;
        return SDL_CreateGPUTexture(device_,&info);
    };
    for(std::size_t level=0;level<bloom_downsample_textures_.size();++level)
        bloom_downsample_textures_[level]=create_bloom_target(2U<<level);
    bloom_upsample_textures_[0]=create_bloom_target(8U);
    bloom_upsample_textures_[1]=create_bloom_target(4U);
    bloom_upsample_textures_[2]=create_bloom_target(2U);
    info.width=1; info.height=1; info.format=exposure_format;
    exposure_history_textures_[0]=SDL_CreateGPUTexture(device_,&info); exposure_history_textures_[1]=SDL_CreateGPUTexture(device_,&info);
    info.width=post_width_; info.height=post_height_;
    info.format=history_depth_format;
    taa_history_depth_textures_[0]=SDL_CreateGPUTexture(device_,&info); taa_history_depth_textures_[1]=SDL_CreateGPUTexture(device_,&info);
    info.format=normal_format;
    temporal_history_normal_textures_[0]=SDL_CreateGPUTexture(device_,&info);
    temporal_history_normal_textures_[1]=SDL_CreateGPUTexture(device_,&info);
    info.width=std::max(1U,render_width_/2U); info.height=std::max(1U,render_height_/2U);
    info.format=ambient_occlusion_format; info.usage=SDL_GPU_TEXTUREUSAGE_COLOR_TARGET|SDL_GPU_TEXTUREUSAGE_SAMPLER;
    ambient_occlusion_texture_=SDL_CreateGPUTexture(device_,&info);
    ambient_occlusion_temp_texture_=SDL_CreateGPUTexture(device_,&info);
    ambient_occlusion_filtered_texture_=SDL_CreateGPUTexture(device_,&info);
    info.width=render_width_; info.height=render_height_;
    info.format=normal_format; info.usage=SDL_GPU_TEXTUREUSAGE_COLOR_TARGET|SDL_GPU_TEXTUREUSAGE_SAMPLER;
    hdr_texture_=SDL_CreateGPUTexture(device_,&info);
    aerial_hdr_texture_=SDL_CreateGPUTexture(device_,&info);
    ao_composited_hdr_texture_=SDL_CreateGPUTexture(device_,&info);
    indirect_lighting_texture_=SDL_CreateGPUTexture(device_,&info);
    specular_indirect_texture_=SDL_CreateGPUTexture(device_,&info);
    reflection_properties_texture_=SDL_CreateGPUTexture(device_,&info);
    ssr_raw_texture_=SDL_CreateGPUTexture(device_,&info);
    ssr_resolved_texture_=SDL_CreateGPUTexture(device_,&info);
    ssr_history_textures_[0]=SDL_CreateGPUTexture(device_,&info);
    ssr_history_textures_[1]=SDL_CreateGPUTexture(device_,&info);
    ssgi_raw_texture_=SDL_CreateGPUTexture(device_,&info);
    ssgi_raw_bent_normal_texture_=SDL_CreateGPUTexture(device_,&info);
    ssgi_spatial_texture_=SDL_CreateGPUTexture(device_,&info);
    ssgi_spatial_bent_normal_texture_=SDL_CreateGPUTexture(device_,&info);
    ssgi_bent_normal_texture_=SDL_CreateGPUTexture(device_,&info);
    ssgi_resolved_texture_=SDL_CreateGPUTexture(device_,&info);
    ssgi_history_textures_[0]=SDL_CreateGPUTexture(device_,&info);
    ssgi_history_textures_[1]=SDL_CreateGPUTexture(device_,&info);
    ssgi_bent_normal_history_textures_[0]=SDL_CreateGPUTexture(device_,&info);
    ssgi_bent_normal_history_textures_[1]=SDL_CreateGPUTexture(device_,&info);
    ssgi_composited_hdr_texture_=SDL_CreateGPUTexture(device_,&info);
    reflected_hdr_texture_=SDL_CreateGPUTexture(device_,&info);
    info.format=object_id_format; info.usage=SDL_GPU_TEXTUREUSAGE_COLOR_TARGET; object_id_texture_=SDL_CreateGPUTexture(device_, &info);
    info.format=normal_format; info.usage=SDL_GPU_TEXTUREUSAGE_COLOR_TARGET|SDL_GPU_TEXTUREUSAGE_SAMPLER; normal_texture_=SDL_CreateGPUTexture(device_, &info);
    info.format=motion_format; info.usage=SDL_GPU_TEXTUREUSAGE_COLOR_TARGET|SDL_GPU_TEXTUREUSAGE_SAMPLER; motion_texture_=SDL_CreateGPUTexture(device_,&info);
    info.format=reactive_mask_format; info.usage=SDL_GPU_TEXTUREUSAGE_COLOR_TARGET|SDL_GPU_TEXTUREUSAGE_SAMPLER; reactive_mask_texture_=SDL_CreateGPUTexture(device_,&info);
    info.format=depth_format; info.usage=SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET|SDL_GPU_TEXTUREUSAGE_SAMPLER; depth_texture_=SDL_CreateGPUTexture(device_, &info);
    depth_pyramid_mip_count_=1U;
    for(auto extent=std::max(render_width_,render_height_);extent>1U;extent=(extent+1U)/2U)++depth_pyramid_mip_count_;
    info.type=SDL_GPU_TEXTURETYPE_2D;info.layer_count_or_depth=1U;info.width=render_width_;info.height=render_height_;
    info.num_levels=depth_pyramid_mip_count_;info.format=SDL_GPU_TEXTUREFORMAT_R32G32_FLOAT;
    info.usage=SDL_GPU_TEXTUREUSAGE_SAMPLER|SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_SIMULTANEOUS_READ_WRITE;
    depth_pyramid_texture_=SDL_CreateGPUTexture(device_,&info);
    depth_pyramid_working_set_bytes_=0U;
    for(std::uint32_t mip=0U,mip_width=render_width_,mip_height=render_height_;mip<depth_pyramid_mip_count_;++mip) {
        depth_pyramid_working_set_bytes_+=static_cast<std::uint64_t>(mip_width)*mip_height*8U;
        mip_width=std::max(1U,(mip_width+1U)/2U);mip_height=std::max(1U,(mip_height+1U)/2U);
    }
    info.num_levels=1U;
    info.type=SDL_GPU_TEXTURETYPE_2D_ARRAY;info.format=depth_format;info.layer_count_or_depth=shadow_cascade_count;
    info.usage=SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET|SDL_GPU_TEXTUREUSAGE_SAMPLER; info.width=shadow_size; info.height=shadow_size;
    shadow_texture_=SDL_CreateGPUTexture(device_, &info);
    info.layer_count_or_depth=local_shadow_layer_count;info.width=local_shadow_resolution_;info.height=local_shadow_resolution_;
    local_shadow_texture_=SDL_CreateGPUTexture(device_,&info);
    if (!color_texture_ || !hdr_texture_ || !aerial_hdr_texture_ || !ao_composited_hdr_texture_ || !indirect_lighting_texture_ ||
        !specular_indirect_texture_ || !reflection_properties_texture_ || !ssr_raw_texture_ || !ssr_resolved_texture_ ||
        !ssr_history_textures_[0] || !ssr_history_textures_[1] || !reflected_hdr_texture_ ||
        !ssgi_raw_texture_ || !ssgi_raw_bent_normal_texture_ || !ssgi_spatial_texture_ || !ssgi_spatial_bent_normal_texture_ ||
        !ssgi_bent_normal_texture_ || !ssgi_resolved_texture_ ||
        !ssgi_history_textures_[0] || !ssgi_history_textures_[1] || !ssgi_composited_hdr_texture_ ||
        !ssgi_bent_normal_history_textures_[0] || !ssgi_bent_normal_history_textures_[1] ||
        !tone_mapped_texture_ || !object_id_texture_ || !normal_texture_ || !motion_texture_ || !reactive_mask_texture_ ||
        !taa_resolved_texture_ || std::ranges::any_of(bloom_downsample_textures_,[](const auto* texture){return texture==nullptr;}) ||
        std::ranges::any_of(bloom_upsample_textures_,[](const auto* texture){return texture==nullptr;}) ||
        !ambient_occlusion_texture_ || !ambient_occlusion_temp_texture_ || !ambient_occlusion_filtered_texture_ ||
        !exposure_history_textures_[0] || !exposure_history_textures_[1] || !taa_history_textures_[0] || !taa_history_textures_[1] ||
        !taa_history_depth_textures_[0] || !taa_history_depth_textures_[1] ||
        !temporal_history_normal_textures_[0]||!temporal_history_normal_textures_[1]||
        !depth_texture_||!depth_pyramid_texture_||!shadow_texture_||!local_shadow_texture_)return false;
    const std::array<SDL_GPUTexture*,45> named_textures{color_texture_,tone_mapped_texture_,taa_resolved_texture_,
        taa_history_textures_[0],taa_history_textures_[1],bloom_downsample_textures_[0],bloom_downsample_textures_[1],
        bloom_downsample_textures_[2],bloom_downsample_textures_[3],bloom_upsample_textures_[0],
        bloom_upsample_textures_[1],bloom_upsample_textures_[2],exposure_history_textures_[0],
        exposure_history_textures_[1],taa_history_depth_textures_[0],taa_history_depth_textures_[1],
        ambient_occlusion_texture_,ambient_occlusion_temp_texture_,ambient_occlusion_filtered_texture_,hdr_texture_,
        aerial_hdr_texture_,ao_composited_hdr_texture_,indirect_lighting_texture_,specular_indirect_texture_,
        reflection_properties_texture_,ssr_raw_texture_,ssr_resolved_texture_,ssr_history_textures_[0],
        ssr_history_textures_[1],ssgi_raw_texture_,ssgi_raw_bent_normal_texture_,ssgi_spatial_texture_,ssgi_spatial_bent_normal_texture_,
        ssgi_bent_normal_texture_,ssgi_resolved_texture_,ssgi_history_textures_[0],ssgi_history_textures_[1],
        ssgi_bent_normal_history_textures_[0],ssgi_bent_normal_history_textures_[1],ssgi_composited_hdr_texture_,reflected_hdr_texture_,
        object_id_texture_,normal_texture_,motion_texture_,reactive_mask_texture_};
    const std::array<const char*,45> texture_names{"render.scene-color","render.tone-mapped","render.taa-resolved",
        "render.taa-history-a","render.taa-history-b","render.bloom-half-down","render.bloom-quarter-down",
        "render.bloom-eighth-down","render.bloom-sixteenth-down","render.bloom-eighth-up","render.bloom-quarter-up",
        "render.bloom-half","render.exposure-history-a","render.exposure-history-b","render.taa-depth-history-a",
        "render.taa-depth-history-b","render.ambient-occlusion","render.ambient-occlusion-temp",
        "render.ambient-occlusion-filtered","render.scene-hdr","render.scene-hdr-aerial","render.scene-hdr-ao","render.scene-indirect",
        "render.scene-specular-indirect","render.surface-reflection-properties","render.ssr-raw","render.ssr-resolved",
        "render.ssr-history-a","render.ssr-history-b","render.ssgi-raw","render.ssgi-raw-bent-normal","render.ssgi-spatial",
        "render.ssgi-spatial-bent-normal","render.ssgi-bent-normal-visibility","render.ssgi-resolved","render.ssgi-history-a",
        "render.ssgi-history-b","render.ssgi-bent-history-a","render.ssgi-bent-history-b","render.scene-hdr-gi","render.scene-hdr-reflected","render.object-id",
        "render.world-normal","render.motion-vectors","render.reactive-mask"};
    for (std::size_t index=0;index<named_textures.size();++index)
        SDL_SetGPUTextureName(device_,named_textures[index],texture_names[index]);
    SDL_SetGPUTextureName(device_,depth_texture_,"render.scene-depth");
    SDL_SetGPUTextureName(device_,depth_pyramid_texture_,"render.scene-depth-pyramid");
    SDL_SetGPUTextureName(device_,temporal_history_normal_textures_[0],"render.temporal-normal-history-a");
    SDL_SetGPUTextureName(device_,temporal_history_normal_textures_[1],"render.temporal-normal-history-b");
    SDL_SetGPUTextureName(device_,shadow_texture_,"render.shadow-cascades");
    SDL_SetGPUTextureName(device_,local_shadow_texture_,"render.local-shadow-array");
    local_shadow_texture_bytes_=static_cast<std::uint64_t>(local_shadow_resolution_)*local_shadow_resolution_*local_shadow_layer_count*4U;
    directional_shadow_cascade_cache_valid_.fill(false);
    local_shadow_face_cache_valid_.fill(false);
    temporal_history_valid_=false;
    ssr_history_valid_=false;
    ssgi_history_valid_=false;
    taa_history_resets_=temporal_history_authority_.state(TemporalHistoryConsumer::taa).reset_count;
    taa_history_index_=0;ssr_history_index_=0;ssgi_history_index_=0; exposure_history_valid_=false; exposure_history_index_=0;
    width_=width; height_=height; allocated_render_scale_=render_scale_;
    return true;
}

bool SceneRenderer::resize(std::uint32_t width, std::uint32_t height) {
    width=std::clamp(width,64U,2048U); height=std::clamp(height,64U,2048U);
    if (width==width_ && height==height_ && std::abs(allocated_render_scale_-render_scale_)<0.0001F) return true;
    release_targets();
    if (!create_targets(width,height)) { last_error_=SDL_GetError(); return false; }
    return true;
}

void SceneRenderer::set_exposure(const float exposure) {
    exposure_=std::clamp(exposure,0.125F,8.0F);
}

void SceneRenderer::set_gpu_driven_enabled(const bool enabled) {
    if (gpu_driven_enabled_ == enabled) return;
    gpu_driven_enabled_ = enabled;
    gpu_batch_cache_.clear();
    gpu_driven_cached_plan_={};gpu_driven_topology_fingerprint_=0U;gpu_driven_cached_plan_valid_=false;
    gpu_driven_instance_mirror_.clear();
    gpu_driven_batch_mirror_.clear();
    gpu_driven_topology_reused_ = false;
}

void SceneRenderer::set_render_scale(const float render_scale) {
    render_scale_=std::clamp(render_scale,0.5F,1.0F);
}

bool SceneRenderer::hybrid_pixel_active() const noexcept {
    return hybrid_pixel_profile_.has_value() && hybrid_pixel_profile_->enabled;
}

bool SceneRenderer::set_hybrid_pixel_profile(std::optional<HybridPixelProfile> profile) {
    if (profile && !HybridPixelProfileCodec::validate(*profile).empty()) {
        last_error_ = "hybrid-pixel.profile-invalid";
        return false;
    }
    if (width_ != 0U || height_ != 0U) release_targets();
    hybrid_pixel_profile_ = std::move(profile);
    hybrid_pixel_projection_ = {};
    return true;
}

void SceneRenderer::set_hybrid_pixel_projection(HybridPixelRenderProjection projection) noexcept {
    hybrid_pixel_projection_ = std::move(projection);
}

bool SceneRenderer::set_shadow_quality(const std::string& quality) {
    if(quality=="low") {
        shadow_quality_=quality;local_shadow_resolution_=512;maximum_shadowed_point_lights_=1;maximum_shadowed_spot_lights_=1;
    } else if(quality=="medium") {
        shadow_quality_=quality;local_shadow_resolution_=768;maximum_shadowed_point_lights_=1;maximum_shadowed_spot_lights_=1;
    } else if(quality=="high") {
        shadow_quality_=quality;local_shadow_resolution_=1024;maximum_shadowed_point_lights_=1;maximum_shadowed_spot_lights_=2;
    } else {
        last_error_="render.shadow-quality-invalid: expected low, medium, or high";return false;
    }
    local_shadow_face_cache_valid_.fill(false);
    return true;
}

void SceneRenderer::set_texture_streaming_budget_kib(const std::uint32_t budget_kib) {
    texture_streaming_budget_bytes_=static_cast<std::uint64_t>(std::clamp(budget_kib,1U,65536U))*1024U;
}

void SceneRenderer::set_texture_streaming_resident_budget_kib(const std::uint32_t budget_kib) {
    texture_streaming_resident_budget_bytes_=static_cast<std::uint64_t>(
        std::clamp(budget_kib,1024U,4U*1024U*1024U))*1024U;
}

void SceneRenderer::set_texture_streaming_workload(std::string workload) {
    texture_streaming_workload_=std::move(workload);
}

void SceneRenderer::refresh_texture_stream_bindings() {
    texture_stream_lookup_.clear();
    for(std::size_t index=0;index<texture_streams_.size();++index) {
        auto& stream=texture_streams_[index];
        if(!stream.texture)continue;
        texture_stream_lookup_[stream.texture]=index;
        if(index>=texture_stream_handles_.size())continue;
        const TextureResourceMetadata metadata{stream.width,stream.height,stream.level_count,
            stream.resident_mip_start,stream.resident_bytes};
        const auto state=texture_resources_.view(texture_stream_handles_[index]);
        if(!state)continue;
        if(stream.transition_pending&&!state->transition_pending) {
            if(!texture_resources_.stage_replacement(texture_stream_handles_[index],stream.texture,metadata))
                last_error_="texture.resource-stage-failed: "+stream.asset_id;
        } else if(!stream.transition_pending&&!state->transition_pending&&state->texture!=stream.texture) {
            last_error_="texture.resource-pointer-diverged: "+stream.asset_id;
        }
    }
}

void SceneRenderer::refresh_texture_streaming_statistics() {
    ktx_resident_bytes_=0;ktx_mip_levels_uploaded_=0;
    texture_streaming_pending_levels_=0;texture_streaming_completed_streams_=0;
    for(const auto& stream:texture_streams_) {
        ktx_resident_bytes_+=stream.resident_bytes;ktx_mip_levels_uploaded_+=stream.level_count-stream.resident_mip_start;
        if(stream.at_target())++texture_streaming_completed_streams_;
        const auto resident=stream.resident_mip_start;
        const auto target=stream.target_mip_start;
        texture_streaming_pending_levels_+=resident>target?resident-target:target-resident;
    }
}

void SceneRenderer::commit_texture_streaming_frame() {
    for(std::size_t index=0;index<texture_streams_.size();++index) {
        auto& stream=texture_streams_[index];
        if(stream.transition_pending) {
            if(stream.resident_mip_start>stream.transition_previous_resident_mip_start) {
                stream.physically_evicted=true;++texture_streaming_evictions_total_;
            } else if(stream.resident_mip_start<stream.transition_previous_resident_mip_start&&stream.physically_evicted) {
                ++texture_streaming_reuploads_total_;
            }
            if(index<texture_stream_handles_.size())
                static_cast<void>(texture_resources_.commit_replacement(texture_stream_handles_[index]));
        }
        commit_texture_stream_transition(device_,stream);
    }
    texture_streaming_bytes_total_+=texture_streaming_bytes_this_frame_;
    texture_streaming_copy_bytes_total_+=texture_streaming_copy_bytes_this_frame_;
    texture_streaming_bytes_released_total_+=texture_streaming_bytes_released_this_frame_;
    refresh_texture_stream_bindings();refresh_texture_streaming_statistics();
}

void SceneRenderer::rollback_texture_streaming_frame() {
    for(std::size_t index=0;index<texture_streams_.size();++index) {
        if(texture_streams_[index].transition_pending&&index<texture_stream_handles_.size())
            static_cast<void>(texture_resources_.rollback_replacement(texture_stream_handles_[index]));
        rollback_texture_stream_transition(device_,texture_streams_[index]);
    }
    texture_streaming_bytes_this_frame_=0;texture_streaming_copy_bytes_this_frame_=0;
    texture_streaming_bytes_released_this_frame_=0;
    texture_streaming_levels_this_frame_=0;texture_streaming_upgrades_this_frame_=0;
    texture_streaming_downgrades_this_frame_=0;
    refresh_texture_stream_bindings();refresh_texture_streaming_statistics();
}

SDL_GPUSampler* SceneRenderer::sprite_sampler(SDL_GPUTexture* texture,const bool nearest) {
    const auto found=texture_stream_lookup_.find(texture);
    if(found==texture_stream_lookup_.end()||found->second>=texture_streams_.size())
        return nearest?sprite_nearest_sampler_:tone_map_sampler_;
    // Physical mip tiers are rebased: authored mip N becomes GPU mip zero.
    // The source-level residency remains observable on RuntimeTextureStream,
    // while the sampler always starts from the replacement texture's level 0.
    constexpr std::size_t lod=0U;
    auto& sampler=(nearest?sprite_nearest_lod_samplers_:sprite_linear_lod_samplers_)[lod];
    if(sampler)return sampler;
    SDL_GPUSamplerCreateInfo info{};
    info.min_filter=nearest?SDL_GPU_FILTER_NEAREST:SDL_GPU_FILTER_LINEAR;
    info.mag_filter=nearest?SDL_GPU_FILTER_NEAREST:SDL_GPU_FILTER_LINEAR;
    info.mipmap_mode=nearest?SDL_GPU_SAMPLERMIPMAPMODE_NEAREST:SDL_GPU_SAMPLERMIPMAPMODE_LINEAR;
    info.address_mode_u=info.address_mode_v=info.address_mode_w=SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    info.min_lod=static_cast<float>(lod);info.max_lod=1000.0F;
    sampler=SDL_CreateGPUSampler(device_,&info);
    if(!sampler)last_error_="Unable to create texture-stream LOD sampler: "+std::string(SDL_GetError());
    return sampler?sampler:(nearest?sprite_nearest_sampler_:tone_map_sampler_);
}

void SceneRenderer::record_texture_streaming(SDL_GPUCommandBuffer* command,const RenderWorldSnapshot& render_world) {
    texture_streaming_bytes_this_frame_=0;texture_streaming_copy_bytes_this_frame_=0;
    texture_streaming_bytes_released_this_frame_=0;texture_streaming_levels_this_frame_=0;
    texture_streaming_upgrades_this_frame_=0;texture_streaming_downgrades_this_frame_=0;
    if(texture_streams_.empty())return;

    struct Footprint final { std::uint32_t width{};std::uint32_t height{};bool visible{}; };
    std::unordered_map<std::string,Footprint> footprints;
    const auto project=[&](const float world_width,const float world_height,const std::array<float,3>& position) {
        float pixels_per_world=1.0F;
        if(render_world.camera) {
            const auto& camera=*render_world.camera;
            if(camera.projection=="orthographic")pixels_per_world=static_cast<float>(height_)/std::max(camera.orthographic_height,0.01F);
            else {
                const auto dx=position[0]-camera.position[0],dy=position[1]-camera.position[1],dz=position[2]-camera.position[2];
                const auto distance=std::max(std::sqrt(dx*dx+dy*dy+dz*dz),camera.near_clip);
                const auto fov=std::max(camera.vertical_fov_degrees,1.0F)*0.0174532925F;
                pixels_per_world=static_cast<float>(height_)/(2.0F*std::tan(fov*0.5F)*distance);
            }
        }
        const auto bounded=[](const float value) {return static_cast<std::uint32_t>(
            std::clamp(std::ceil(std::abs(value)),1.0F,1048576.0F));};
        return std::array<std::uint32_t,2>{bounded(world_width*pixels_per_world),bounded(world_height*pixels_per_world)};
    };
    const auto mark=[&](const std::string& asset_id,const std::array<std::uint32_t,2>& size) {
        if(asset_id.empty())return;auto& value=footprints[asset_id];value.visible=true;
        value.width=std::max(value.width,size[0]);value.height=std::max(value.height,size[1]);
    };
    for(const auto& sprite:render_world.sprites)if(sprite.visible) {
        const auto ppu=std::max(sprite.pixels_per_unit,0.001F);
        const auto projected=project(static_cast<float>(sprite.source_size[0])/ppu*sprite.scale[0],
            static_cast<float>(sprite.source_size[1])/ppu*sprite.scale[1],sprite.position);
        mark(sprite.texture_asset,projected);mark(sprite.normal_texture_asset,projected);
        mark(sprite.emissive_mask_texture_asset,projected);mark(sprite.depth_texture_asset,projected);
    }
    for(const auto& cell:render_world.tile_cells)if(cell.visible) {
        const auto projected=project((cell.local_rect[2]-cell.local_rect[0])*cell.scale[0],
            (cell.local_rect[1]-cell.local_rect[3])*cell.scale[1],cell.position);
        mark(cell.texture_asset,projected);mark(cell.normal_texture_asset,projected);
        mark(cell.emissive_mask_texture_asset,projected);mark(cell.depth_texture_asset,projected);
    }
    if(texture_streaming_workload_=="noemancer.texture-streaming.pressure/0.1") {
        footprints.clear();const auto active_group=(render_world.frame_index/24U)%2U;
        for(std::size_t index=0;index<texture_streams_.size();++index)if(index%2U==active_group)
            footprints[texture_streams_[index].asset_id]={texture_streams_[index].width,texture_streams_[index].height,true};
    }
    const auto key=[](const RuntimeTextureStream& stream) {
        return stream.asset_id+(stream.linear_semantic?"#linear":"#srgb");
    };
    const auto samples=[&]() {
        std::vector<TextureStreamingDemandSample> result;result.reserve(texture_streams_.size());
        for(const auto& stream:texture_streams_) {
            const auto found=footprints.find(stream.asset_id);const bool visible=found!=footprints.end()&&found->second.visible;
            std::vector<std::uint64_t> bytes;bytes.reserve(stream.levels.size());
            for(const auto& level:stream.levels)bytes.push_back(level.source_bytes);
            result.push_back({key(stream),static_cast<TextureStreamingImportance>(std::min<std::uint8_t>(stream.authored_importance,3U)),
                stream.authored_priority,stream.width,stream.height,visible?found->second.width:0U,visible?found->second.height:0U,
                visible,stream.resident_mip_start,stream.maximum_mip_start,std::move(bytes),stream.demand_age_frames,
                stream.visibility_age_frames,2U,8U});
        }
        return result;
    };
    auto demand_samples=samples();
    const auto probe=plan_texture_streaming_demand(demand_samples,std::numeric_limits<std::uint64_t>::max());
    if(!probe.valid){texture_streaming_plan_code_=probe.code;last_error_=probe.code+": "+probe.detail;return;}
    for(const auto& asset:probe.assets)for(auto& stream:texture_streams_)if(key(stream)==asset.asset_id) {
        if(stream.screen_mip_start==asset.screen_mip_start)stream.demand_age_frames=std::min(
            stream.demand_age_frames+static_cast<std::uint32_t>(stream.demand_age_frames<std::numeric_limits<std::uint32_t>::max()),
            std::numeric_limits<std::uint32_t>::max());
        else {stream.screen_mip_start=asset.screen_mip_start;stream.demand_age_frames=1U;}
        if(asset.visible){stream.visibility_age_frames=0U;stream.last_used_frame=render_world.frame_index;}
        else if(stream.visibility_age_frames<std::numeric_limits<std::uint32_t>::max())++stream.visibility_age_frames;
        break;
    }
    demand_samples=samples();
    const auto plan=plan_texture_streaming_demand(demand_samples,texture_streaming_resident_budget_bytes_);
    texture_streaming_plan_code_=plan.code;texture_streaming_over_budget_=plan.over_budget;
    texture_streaming_demand_bytes_=plan.demand_bytes;texture_streaming_planned_bytes_=plan.planned_bytes;
    if(!plan.valid){last_error_=plan.code+": "+plan.detail;return;}
    std::unordered_map<std::string,std::size_t> stream_by_key;
    for(std::size_t index=0;index<texture_streams_.size();++index)stream_by_key.emplace(key(texture_streams_[index]),index);
    for(const auto& asset:plan.assets)if(const auto found=stream_by_key.find(asset.asset_id);found!=stream_by_key.end())
        texture_streams_[found->second].target_mip_start=asset.target_mip_start;

    std::vector<std::size_t> priority;priority.reserve(plan.priority_order.size());
    for(const auto& id:plan.priority_order)if(const auto found=stream_by_key.find(id);found!=stream_by_key.end())priority.push_back(found->second);
    const auto record_step=[&](const std::size_t index) {
        auto& stream=texture_streams_[index];if(stream.at_target())return;
        const bool upgrading=stream.resident_mip_start>stream.target_mip_start;
        const auto next_mip=upgrading?stream.resident_mip_start-1U:stream.resident_mip_start+1U;
        std::uint64_t copy_bytes{};for(std::uint32_t level=next_mip;level<stream.level_count;++level)
            copy_bytes+=stream.levels[level].copy_bytes();
        if(texture_streaming_levels_this_frame_>0U&&
           texture_streaming_copy_bytes_this_frame_+copy_bytes>texture_streaming_budget_bytes_)return;
        const auto step=record_texture_stream_rebase(device_,command,stream,next_mip);
        if(!step.valid){last_error_=step.code+": "+step.detail;return;}
        ++texture_streaming_levels_this_frame_;
        texture_streaming_bytes_this_frame_+=step.source_bytes;
        texture_streaming_copy_bytes_this_frame_+=step.copy_bytes;
        if(upgrading) {
            ++texture_streaming_upgrades_this_frame_;
        } else {
            ++texture_streaming_downgrades_this_frame_;
            texture_streaming_bytes_released_this_frame_+=step.resident_bytes_before-step.resident_bytes_after;
        }
    };
    for(auto cursor=priority.rbegin();cursor!=priority.rend();++cursor)
        if(texture_streams_[*cursor].resident_mip_start<texture_streams_[*cursor].target_mip_start)record_step(*cursor);
    for(const auto index:priority)
        if(texture_streams_[index].resident_mip_start>texture_streams_[index].target_mip_start)record_step(index);
    if(!priority.empty())texture_streaming_cursor_=(texture_streaming_cursor_+1U)%priority.size();
    refresh_texture_stream_bindings();refresh_texture_streaming_statistics();
}

void SceneRenderer::set_temporal_debug_mode(const std::string& mode) {
    temporal_debug_mode_name_=mode;
    temporal_debug_mode_=mode=="motion"?1U:(mode=="reactive"?2U:(mode=="disocclusion"?3U:
        (mode=="history-weight"?4U:(mode=="history-clamp"?5U:(mode=="linear-depth"?6U:(mode=="normal"?7U:0U))))));
    if(temporal_debug_mode_==0U)temporal_debug_mode_name_="final";
}

bool SceneRenderer::set_ssr_options(const bool enabled, const std::string& quality,
                                    const std::string& debug_mode) {
    if(quality!="low"&&quality!="medium"&&quality!="high") {
        last_error_="render.ssr.quality.invalid: expected low, medium, or high";
        return false;
    }
    if(debug_mode!="final"&&debug_mode!="confidence"&&debug_mode!="hit-distance"&&
       debug_mode!="roughness"&&debug_mode!="miss"&&debug_mode!="normal") {
        last_error_="render.ssr.debug.invalid: unsupported debug view";
        return false;
    }
    ssr_enabled_=enabled;ssr_quality_=quality;ssr_debug_mode_name_=debug_mode;
    ssr_debug_mode_=debug_mode=="confidence"?1U:(debug_mode=="hit-distance"?2U:
        (debug_mode=="roughness"?3U:(debug_mode=="miss"?4U:(debug_mode=="normal"?5U:0U))));
    const auto quality_value=screen_space_reflections_quality_from_string(enabled?quality:"off");
    if(!quality_value) {
        last_error_="render.ssr.quality.invalid: could not build engine SSR policy";
        return false;
    }
    ssr_config_=screen_space_reflections_quality_defaults(*quality_value);
    return true;
}

bool SceneRenderer::set_ssgi_options(const bool enabled, const std::string& quality,
                                     const std::string& debug_mode) {
    if(quality!="low"&&quality!="medium"&&quality!="high") {
        last_error_="render.ssgi.quality.invalid: expected low, medium, or high";
        return false;
    }
    if(debug_mode!="final"&&debug_mode!="confidence"&&debug_mode!="visibility"&&
       debug_mode!="bent-normal"&&debug_mode!="miss") {
        last_error_="render.ssgi.debug.invalid: unsupported debug view";
        return false;
    }
    ssgi_enabled_=enabled;ssgi_quality_=quality;ssgi_debug_mode_name_=debug_mode;
    ssgi_debug_mode_=debug_mode=="confidence"?1U:(debug_mode=="visibility"?2U:
        (debug_mode=="bent-normal"?5U:(debug_mode=="miss"?4U:0U)));
    const auto quality_value=screen_space_global_illumination_quality_from_string(enabled?quality:"off");
    if(!quality_value) {
        last_error_="render.ssgi.quality.invalid: could not build engine SSGI policy";
        return false;
    }
    ssgi_config_=screen_space_global_illumination_quality_defaults(*quality_value);
    return true;
}

void SceneRenderer::set_capture_contract_json(std::string contract_json) {
    capture_contract_json_=std::move(contract_json);
}

void SceneRenderer::update_native_raytracing_scene(const RenderWorldSnapshot& render_world) {
    const auto backend=gpu_backend_=="direct3d12"?std::string{"d3d12"}:gpu_backend_;
    if(!native_raytracing_session_enabled_) {
        native_rt_composite_plan_={};
        native_raytracing_status_json_=nlohmann::json{
            {"schema",std::string(scene_raytracing_bridge_schema)},
            {"requested",false},{"enabled",false},{"backend",backend},
            {"sceneAccepted",false},{"nativeAsReady",false},{"nativeTraceReady",false},
            {"visualPath","ssgi-raster-fallback"},{"fallbackCode","bridge.disabled"},
            {"fallbackDetail","Native RT production binding is opt-in until its output can be shared with SDL_GPU."},
            {"rtgiReady",false}}.dump();
        return;
    }

    std::vector<const RenderInstanceSnapshot*> eligible;
    eligible.reserve(render_world.instances.size());
    std::unordered_set<std::string> geometry_ids;
    for(const auto& instance:render_world.instances) {
        if(!instance.visible||instance.vfx_particle||!raytracing_geometries_.contains(instance.mesh_asset))continue;
        eligible.push_back(&instance);geometry_ids.insert(instance.mesh_asset);
    }
    std::ranges::sort(eligible,[](const auto* left,const auto* right) {
        if(left->entity_id!=right->entity_id)return left->entity_id<right->entity_id;
        return left->mesh_asset<right->mesh_asset;
    });
    std::vector<std::string> ordered_geometry_ids(geometry_ids.begin(),geometry_ids.end());
    std::ranges::sort(ordered_geometry_ids);

    SceneRayTracingGeometryCacheInput input;
    input.scene_id=render_world.extraction_id.empty()?"scene.renderer":render_world.extraction_id;
    input.allow_update=true;
    input.geometries.reserve(ordered_geometry_ids.size());
    for(const auto& id:ordered_geometry_ids)input.geometries.push_back(raytracing_geometries_.at(id));
    input.instances.reserve(eligible.size());
    for(const auto* instance:eligible) {
        SceneRayTracingInstanceInput transfer;
        transfer.instance_id=instance->entity_id;
        transfer.geometry_id=instance->mesh_asset;
        transfer.transform=model_matrix(
            {instance->position[0],instance->position[1],instance->position[2]},
            {instance->scale[0],instance->scale[1],instance->scale[2]},instance->rotation).value;
        input.instances.push_back(std::move(transfer));
    }

    const auto cache_update=raytracing_geometry_cache_.update(input);
    if(!cache_update.accepted) {
        native_rt_composite_plan_={};
        native_raytracing_status_json_=nlohmann::json{
            {"schema",std::string(scene_raytracing_bridge_schema)},
            {"requested",true},{"enabled",true},{"backend",backend},
            {"sceneAccepted",false},{"cacheState",scene_raytracing_geometry_cache_state_name(cache_update.state)},
            {"nativeAsReady",false},{"nativeTraceReady",false},{"visualPath","ssgi-raster-fallback"},
            {"fallbackCode",cache_update.fallback.code},{"fallbackDetail",cache_update.fallback.detail},
            {"topologyRevision",cache_update.topology_revision},{"contentRevision",cache_update.content_revision},
            {"triangleCount",cache_update.statistics.world_triangle_count},{"rtgiReady",false}}.dump();
        return;
    }
    const auto& native_rt_cache_snapshot=raytracing_geometry_cache_.snapshot();
    NativeRayTracingShadingInput native_rt_shading_input;
    native_rt_shading_input.scene_id=input.scene_id;
    native_rt_shading_input.scene_revision=std::max<std::uint64_t>(
        native_rt_cache_snapshot.content_revision,1U);
    if(render_world.directional_light) {
        const auto& light=*render_world.directional_light;
        // RenderWorld stores the direction travelled by the directional
        // light; the RT shading ABI stores the direction from surface to sun.
        native_rt_shading_input.directional_light.direction={
            -light.direction[0],-light.direction[1],-light.direction[2]};
        native_rt_shading_input.directional_light.color=light.color;
        native_rt_shading_input.directional_light.intensity=std::max(light.intensity,0.0F);
        native_rt_shading_input.directional_light.enabled=light.intensity>0.0F;
        native_rt_shading_input.environment.color={1.0F,1.0F,1.0F};
        native_rt_shading_input.environment.intensity=std::max(light.ambient_intensity,0.0F);
        native_rt_shading_input.environment.enabled=light.ambient_intensity>0.0F;
    }
    std::unordered_map<std::string,std::vector<const SceneRayTracingGeometryCachePrimitiveRange*>>
        native_rt_ranges_by_instance;
    native_rt_ranges_by_instance.reserve(native_rt_cache_snapshot.world_instances.size());
    for(const auto& range:native_rt_cache_snapshot.primitive_ranges)
        native_rt_ranges_by_instance[range.instance_id].push_back(&range);
    native_rt_shading_input.instances.reserve(eligible.size());
    for(const auto* source:eligible) {
        NativeRayTracingShadingInstance shading_instance;
        shading_instance.instance_id=source->entity_id;
        shading_instance.geometry_id=source->mesh_asset;
        const auto found=native_rt_ranges_by_instance.find(source->entity_id);
        if(found!=native_rt_ranges_by_instance.end()) {
            shading_instance.primitives.reserve(found->second.size());
            for(const auto* range:found->second) {
                NativeRayTracingShadingPrimitive primitive;
                primitive.primitive_id=range->primitive_id;
                primitive.material.material_id=source->entity_id;
                primitive.material.base_color={source->material.base_color[0],
                    source->material.base_color[1],source->material.base_color[2],1.0F};
                primitive.material.metallic=std::clamp(source->material.metallic,0.0F,1.0F);
                primitive.material.roughness=std::clamp(source->material.roughness,0.02F,1.0F);
                primitive.material.emissive_color=source->material.emissive_color;
                primitive.material.emissive_intensity=std::max(source->material.emissive_intensity,0.0F);
                primitive.material.has_base_color_texture=!source->material.base_color_texture.empty();
                primitive.material.receives_shadows=source->receives_shadows;
                shading_instance.primitives.push_back(std::move(primitive));
            }
        }
        native_rt_shading_input.instances.push_back(std::move(shading_instance));
    }
    const auto native_rt_shading_plan=build_native_raytracing_shading_plan(
        native_rt_shading_input,NativeRayTracingShadingOutputMode::diagnostic_hit_mask);
    if(!native_rt_shading_plan.valid||!native_rt_shading_plan.supported) {
        native_rt_composite_plan_={};
        native_raytracing_status_json_=nlohmann::json{
            {"schema",std::string(scene_raytracing_bridge_schema)},
            {"requested",true},{"enabled",true},{"backend",backend},
            {"sceneAccepted",true},{"shadingValid",false},
            {"nativeAsReady",false},{"nativeTraceReady",false},
            {"visualPath","ssgi-raster-fallback"},{"fallbackCode",native_rt_shading_plan.code},
            {"fallbackDetail",native_rt_shading_plan.detail},{"rtgiReady",false}}.dump();
        return;
    }
    if(!native_rt_texture_export_.ready||native_rt_texture_export_.width!=width_||
        native_rt_texture_export_.height!=height_) {
        release_sdl_gpu_native_rt_texture(device_,native_rt_texture_export_);
        if(native_rt_texture_generation_!=std::numeric_limits<std::uint64_t>::max())
            ++native_rt_texture_generation_;
        native_rt_texture_export_=create_sdl_gpu_native_rt_texture(
            device_,std::max(width_,1U),std::max(height_,1U),native_rt_texture_generation_);
        scene_raytracing_bridge_.reset();
    }
    if(!scene_raytracing_bridge_) {
        std::vector<std::byte> native_rt_dxil;
        if(backend=="d3d12") {
            const auto bytes=read_binary(
                default_shader_artifact_root()/"native_rt_full_frame.lib.dxil");
            native_rt_dxil.resize(bytes.size());
            std::transform(bytes.begin(),bytes.end(),native_rt_dxil.begin(),
                [](const Uint8 value){return static_cast<std::byte>(value);});
        }
        scene_raytracing_bridge_=std::make_unique<SceneRayTracingBridge>(
            SceneRayTracingBridgeOptions{.allow_fallback=true,.output_width=std::max(width_,1U),
                .output_height=std::max(height_,1U),.graph_generation=native_rt_texture_generation_,
                .d3d12_full_frame_library_dxil=std::move(native_rt_dxil),
                .native_device=sdl_native_device_bridge_.handles});
    }
    // Production rendering keeps the native result on the GPU. The context
    // readback path is a diagnostic proof for controlled fixtures and would
    // otherwise serialize every frame (and mistake a legitimate miss in an
    // arbitrary scene for a failed fixture). Presentation interop consumes
    // this native output directly once resource sharing lands.
    NativeRayTracingViewInput native_rt_view;
    native_rt_view.camera_id="camera.unowned";
    native_rt_view.camera_revision=std::max<std::uint64_t>(render_world.world_revision,1U);
    native_rt_view.position={7.0F,5.5F,8.5F};
    native_rt_view.forward={-7.0F,-4.5F,-8.5F};
    native_rt_view.up={0.0F,1.0F,0.0F};
    native_rt_view.projection="perspective";
    native_rt_view.vertical_fov_degrees=45.0F;
    native_rt_view.orthographic_height=10.0F;
    native_rt_view.aspect=static_cast<float>(std::max(width_,1U)) /
        static_cast<float>(std::max(height_,1U));
    native_rt_view.near_clip=0.1F;
    native_rt_view.far_clip=100.0F;
    native_rt_view.output_width=std::max(width_,1U);
    native_rt_view.output_height=std::max(height_,1U);
    if(render_world.camera) {
        const auto& camera=*render_world.camera;
        native_rt_view.camera_id=camera.entity_id.empty()?"camera.unowned":camera.entity_id;
        native_rt_view.position=camera.position;
        native_rt_view.forward={camera.target[0]-camera.position[0],
            camera.target[1]-camera.position[1],camera.target[2]-camera.position[2]};
        const auto forward_length=std::sqrt(native_rt_view.forward[0]*native_rt_view.forward[0]+
            native_rt_view.forward[1]*native_rt_view.forward[1]+
            native_rt_view.forward[2]*native_rt_view.forward[2]);
        if(forward_length>0.00001F&&
           std::abs(native_rt_view.forward[1]/forward_length)>0.999F)
            native_rt_view.up={0.0F,0.0F,1.0F};
        native_rt_view.projection=camera.projection;
        native_rt_view.vertical_fov_degrees=camera.vertical_fov_degrees;
        native_rt_view.orthographic_height=camera.orthographic_height;
        native_rt_view.near_clip=camera.near_clip;
        native_rt_view.far_clip=camera.far_clip;
    }
    const auto receipt=scene_raytracing_bridge_->execute(
        {.backend=backend,.enabled=true,.request_trace=true,.request_readback=false,
            .view=std::move(native_rt_view),.shading=native_rt_shading_plan},
        native_rt_cache_snapshot);
    RayTracingContextSessionOutputTransferReceipt transfer;
    transfer.backend=backend;
    if(receipt.output_transfer_candidate&&native_rt_texture_export_.ready&&
        native_rt_texture_export_.native_resource!=nullptr) {
        transfer=scene_raytracing_bridge_->transfer_output_to(
            native_rt_texture_export_.native_resource);
    } else {
        transfer.unsupported=true;
        transfer.code="renderer.native-output-transfer-not-candidate";
        transfer.detail="The current backend, dimensions or exported texture did not satisfy the bounded same-device transfer contract.";
    }
    const auto native_rt_composite_mode=receipt.shader_contract==
        native_raytracing_composite_radiance_contract
        ?NativeRayTracingCompositeMode::linear_radiance
        :NativeRayTracingCompositeMode::debug_marker;
    native_rt_composite_plan_=build_native_raytracing_composite_plan({
        .resource_id="render.resource.native-rt-debug-output",
        .resource_kind="texture2d",
        .format=native_rt_texture_export_.format,
        .producer_shader_contract=receipt.shader_contract,
        .width=native_rt_texture_export_.width,
        .height=native_rt_texture_export_.height,
        .resource_generation=native_rt_texture_export_.generation,
        .producer_complete=transfer.completed&&receipt.output_trace_written,
        .shader_readable=transfer.completed},native_rt_composite_mode);
    native_raytracing_status_json_=nlohmann::json{
        {"schema",receipt.schema},{"requested",receipt.requested},{"enabled",receipt.enabled},
        {"backend",receipt.backend},{"sceneAccepted",receipt.scene_accepted},{"cacheState",receipt.cache_state},
        {"nativeAsReady",receipt.native_as_ready},{"nativeTraceReady",receipt.native_trace_ready},
        {"visualPath",native_rt_composite_plan_.valid?native_rt_composite_plan_.visual_path:receipt.visual_path},
        {"fallbackCode",native_rt_composite_plan_.valid?native_rt_composite_plan_.code:receipt.fallback_code},
        {"fallbackDetail",native_rt_composite_plan_.valid?native_rt_composite_plan_.detail:
            receipt.fallback_detail},{"planFingerprint",receipt.plan_fingerprint},
        {"planValid",receipt.plan_valid},{"planSupported",receipt.plan_supported},
        {"sessionExecuted",receipt.session_executed},{"failed",receipt.failed},
        {"sharedDevice",receipt.shared_device},{"sharedQueue",receipt.shared_queue},
        {"outputResourceLive",receipt.output_resource_live},
        {"outputTraceWritten",receipt.output_trace_written},
        {"outputTransferCandidate",receipt.output_transfer_candidate},
        {"fullFrameShaderReady",receipt.full_frame_shader_ready},
        {"shadingRequested",receipt.shading_requested},{"shadingValid",receipt.shading_valid},
        {"shadingResourcesReady",receipt.shading_resources_ready},
        {"linearRadianceShaderConsumed",receipt.linear_radiance_shader_consumed},
        {"shadingSchema",receipt.shading_schema},{"shadingFingerprint",receipt.shading_fingerprint},
        {"shadingMaterialCount",receipt.shading_material_count},
        {"outputRadianceValid",receipt.output_radiance_valid},{"claimsRtgi",receipt.claims_rtgi},
        {"cameraRequested",receipt.camera_requested},{"cameraValid",receipt.camera_valid},
        {"cameraShaderConsumed",receipt.camera_shader_consumed},{"cameraId",receipt.camera_id},
        {"cameraProjection",receipt.camera_projection},{"cameraFingerprint",receipt.camera_fingerprint},
        {"shaderContract",receipt.shader_contract},
        {"outputTransferAttempted",transfer.attempted},
        {"outputTransferCompleted",transfer.completed},
        {"outputTransferCode",transfer.code},
        {"outputTransferGeneration",transfer.resource_generation},
        {"outputResourceGeneration",receipt.output_resource_generation},
        {"outputFormat",receipt.output_format},
        {"contentUpdated",receipt.content_updated},{"topologyRebuilt",receipt.topology_rebuilt},
        {"frameGeneration",receipt.frame_generation},{"graphGeneration",receipt.graph_generation},
        {"topologyRevision",receipt.topology_revision},{"contentRevision",receipt.content_revision},
        {"triangleCount",receipt.triangle_count},{"rtgiReady",false},
        {"composite",{{"schema",native_rt_composite_plan_.schema},
            {"valid",native_rt_composite_plan_.valid},{"supported",native_rt_composite_plan_.supported},
            {"code",native_rt_composite_plan_.code},{"visualPath",native_rt_composite_plan_.visual_path},
            {"shaderContract",native_rt_composite_plan_.shader_contract},
            {"producerShaderContract",native_rt_composite_plan_.producer_shader_contract},
            {"mode",native_raytracing_composite_mode_name(native_rt_composite_plan_.mode)},
            {"decode",native_rt_composite_plan_.decode},{"stage",native_rt_composite_plan_.stage},
            {"linearRadiance",native_rt_composite_plan_.linear_radiance_composite},
            {"claimsRtgi",native_rt_composite_plan_.claims_rtgi},
            {"inputFormat",native_rt_composite_plan_.input_format},
            {"outputFormat",native_rt_composite_plan_.output_format}}},
        {"resourceInterop",transfer.completed?"native-output-copied-to-sdl-texture":
            (sdl_native_device_bridge_.observation.same_device_candidate?
                "sdl-native-device-ready-output-not-consumed":"native-context-output-not-shared-with-sdl-gpu")},
        {"sdlNativeDevice",{{"backend",sdl_native_device_bridge_.observation.backend},
            {"sameDeviceCandidate",sdl_native_device_bridge_.observation.same_device_candidate},
            {"code",sdl_native_device_bridge_.observation.code}}},
        {"sdlNativeOutput",{{"ready",native_rt_texture_export_.ready},
            {"code",native_rt_texture_export_.code},{"format",native_rt_texture_export_.format},
            {"width",native_rt_texture_export_.width},{"height",native_rt_texture_export_.height},
            {"generation",native_rt_texture_export_.generation}}}}.dump();
}

void SceneRenderer::render(SDL_GPUCommandBuffer* command, const RenderWorldSnapshot& render_world) {
    last_error_.clear();
    gpu_occlusion_used_this_frame_=false;
    native_rt_composite_recorded_=false;
    if (!color_texture_ || !shadow_texture_) return;
    if (render_graph_.graph_id.empty())
        render_graph_ = make_forward_render_graph(native_raytracing_session_enabled_);
    if (!render_graph_.valid) { last_error_ = "Render graph is invalid"; return; }
    extraction_id_ = render_world.extraction_id;
    world_revision_ = render_world.world_revision;
    frame_index_ = render_world.frame_index;
    update_native_raytracing_scene(render_world);
    record_texture_streaming(command,render_world);
    skinned_render_instances_=0;skinning_joint_matrices_=0;
    for(const auto& instance:render_world.instances)if(!instance.skinning_matrices.empty()) {
        ++skinned_render_instances_;skinning_joint_matrices_+=instance.skinning_matrices.size();
    }
    vfx_particles_submitted_ = render_world.vfx_particle_count;
    const bool vfx_state_requires_sync=!render_world.vfx_particles.empty()||vfx_resident_particles_>0U;
    const bool vfx_compute_active=vfx_state_requires_sync&&upload_vfx_compute_state(command,render_world)&&vfx_resident_particles_>0U;
    if (vfx_compute_active) dispatch_vfx_compute(command);
    Vec3 camera_position{7.0F,5.5F,8.5F};
    Vec3 camera_target{0,1,0};
    float camera_fov=0.785398F;
    float camera_near=0.1F;
    float camera_far=100.0F;
    bool camera_orthographic=false;
    float camera_orthographic_height=10.0F;
    Vec3 light_direction=normalize({-0.55F,-1.0F,-0.35F});
    std::array<float,3> light_color{1.0F,0.96F,0.88F};
    float light_intensity=0.95F;
    float ambient_intensity=0.18F;
    bool light_casts_shadows=true;
    active_camera_id_.clear(); active_light_id_.clear();
    if (render_world.camera) {
        const auto& camera = *render_world.camera;
        camera_position={camera.position[0],camera.position[1],camera.position[2]};
        camera_target={camera.target[0],camera.target[1],camera.target[2]};
        camera_fov=camera.vertical_fov_degrees*0.0174532925F;
        camera_near=camera.near_clip; camera_far=camera.far_clip;
        camera_orthographic=camera.projection=="orthographic";
        camera_orthographic_height=camera.orthographic_height;
        active_camera_id_=camera.entity_id;
    }
    if (render_world.directional_light) {
        const auto& light=*render_world.directional_light;
        light_direction=normalize({light.direction[0],light.direction[1],light.direction[2]});
        light_color=light.color;
        light_intensity=light.intensity; ambient_intensity=light.ambient_intensity;
        light_casts_shadows=light.casts_shadows; active_light_id_=light.entity_id;
    }
    const ClusteredLightingCamera clustered_camera{{camera_position.x,camera_position.y,camera_position.z},
        {camera_target.x,camera_target.y,camera_target.z},camera_fov/0.0174532925F,
        static_cast<float>(render_width_)/static_cast<float>(render_height_),camera_near,camera_far,
        camera_orthographic,camera_orthographic_height};
    sky_last_camera_orthographic_=camera_orthographic;
    struct LocalShadowFacePlan final { Mat4 view_projection; VisibilityFrustum frustum; std::uint32_t layer{}; std::string cache_key; };
    std::array<Mat4,local_shadow_layer_count> local_shadow_matrices{};
    for(auto& matrix:local_shadow_matrices)matrix=identity();
    const auto accepted_local_lights=std::min(render_world.local_lights.size(),
        static_cast<std::size_t>(clustered_lighting_config_.maximum_lights));
    std::vector<std::int32_t> local_shadow_base_layers(accepted_local_lights,-1);
    std::vector<LocalShadowFacePlan> local_shadow_faces;
    local_shadow_faces.reserve(local_shadow_layer_count);
    local_shadow_requested_lights_=0;local_shadow_selected_lights_=0;local_shadow_dropped_lights_=0;
    local_shadow_point_lights_=0;local_shadow_spot_lights_=0;
    local_shadow_selected_ids_.clear();
    std::uint32_t next_local_shadow_layer=0;
    constexpr std::array<Vec3,6> point_directions{{{1,0,0},{-1,0,0},{0,1,0},{0,-1,0},{0,0,1},{0,0,-1}}};
    constexpr std::array<Vec3,6> point_up{{{0,-1,0},{0,-1,0},{0,0,1},{0,0,-1},{0,-1,0},{0,-1,0}}};
    std::vector<std::size_t> local_shadow_candidates;
    for(std::size_t index=0;index<accepted_local_lights;++index)
        if(render_world.local_lights[index].casts_shadows)local_shadow_candidates.push_back(index);
    local_shadow_requested_lights_=local_shadow_candidates.size();
    std::ranges::sort(local_shadow_candidates,[&](const std::size_t left_index,const std::size_t right_index) {
        const auto importance=[&](const RenderLocalLightSnapshot& light) {
            const float dx=light.position[0]-camera_position.x,dy=light.position[1]-camera_position.y,dz=light.position[2]-camera_position.z;
            const float distance_squared=std::max(dx*dx+dy*dy+dz*dz,1.0F);
            return light.luminous_power_lumens*light.range_meters*light.range_meters/distance_squared;
        };
        const auto& left=render_world.local_lights[left_index];const auto& right=render_world.local_lights[right_index];
        const float left_importance=importance(left),right_importance=importance(right);
        return left_importance==right_importance?left.entity_id<right.entity_id:left_importance>right_importance;
    });
    for(const auto light_index:local_shadow_candidates) {
        const auto& light=render_world.local_lights[light_index];
        const bool spot=light.kind=="spot";
        if((spot&&local_shadow_spot_lights_>=maximum_shadowed_spot_lights_)||
           (!spot&&local_shadow_point_lights_>=maximum_shadowed_point_lights_)) {++local_shadow_dropped_lights_;continue;}
        const std::uint32_t face_count=spot?1U:6U;
        if(next_local_shadow_layer+face_count>local_shadow_layer_count){++local_shadow_dropped_lights_;continue;}
        local_shadow_base_layers[light_index]=static_cast<std::int32_t>(next_local_shadow_layer);
        ++local_shadow_selected_lights_;if(spot)++local_shadow_spot_lights_;else ++local_shadow_point_lights_;
        local_shadow_selected_ids_.push_back(light.entity_id);
        const Vec3 position{light.position[0],light.position[1],light.position[2]};
        const float near_plane=std::max(0.05F,light.source_radius_meters*0.5F);
        const float far_plane=std::max(near_plane+0.05F,light.range_meters);
        if(spot) {
            const Vec3 direction=normalize({light.direction[0],light.direction[1],light.direction[2]});
            const Vec3 up=std::abs(direction.y)>0.98F?Vec3{0,0,1}:Vec3{0,1,0};
            const Mat4 matrix=multiply(perspective(light.outer_cone_degrees*2.0F*0.0174532925F,1.0F,near_plane,far_plane),
                look_at(position,position+direction,up));
            local_shadow_matrices[next_local_shadow_layer]=matrix;
            local_shadow_faces.push_back({matrix,extract_visibility_frustum(matrix.value),next_local_shadow_layer++,light.entity_id+":spot"});
        } else {
            const Mat4 projection=perspective(1.57079632679F,1.0F,near_plane,far_plane);
            for(std::uint32_t face=0;face<6U;++face) {
                const Mat4 matrix=multiply(projection,look_at(position,position+point_directions[face],point_up[face]));
                local_shadow_matrices[next_local_shadow_layer]=matrix;
                local_shadow_faces.push_back({matrix,extract_visibility_frustum(matrix.value),next_local_shadow_layer++,light.entity_id+":point:"+std::to_string(face)});
            }
        }
    }
    if(std::ranges::any_of(local_shadow_faces,[](const auto& face){return !face.frustum.valid;})) {
        last_error_="lighting.local-shadow-invalid-frustum";return;
    }
    if(!upload_clustered_lighting(command,render_world,clustered_camera,local_shadow_base_layers))return;
    if(vfx_compute_active)dispatch_vfx_group_sort(command,{camera_position.x,camera_position.y,camera_position.z});
    const Mat4 camera_view=look_at(camera_position,camera_target,{0,1,0});
    const float camera_aspect=static_cast<float>(render_width_)/static_cast<float>(render_height_);
    const float orthographic_half_height=camera_orthographic_height*0.5F;
    const Mat4 unjittered_camera_projection=camera_orthographic
        ? orthographic(-orthographic_half_height*camera_aspect,orthographic_half_height*camera_aspect,
            -orthographic_half_height,orthographic_half_height,camera_near,camera_far)
        : perspective(camera_fov,camera_aspect,camera_near,camera_far);
    const auto jitter=hybrid_pixel_active()
        ? TemporalJitter{}
        : temporal_jitter(frame_index_,render_width_,render_height_);
    temporal_jitter_sample_=jitter.sample_index;
    temporal_jitter_pixels_=jitter.pixel_offset;
    temporal_jitter_ndc_=jitter.ndc_offset;
    auto jittered_projection=unjittered_camera_projection.value;
    if(camera_orthographic) { jittered_projection[12]+=jitter.ndc_offset[0]; jittered_projection[13]+=jitter.ndc_offset[1]; }
    else jittered_projection=apply_projection_jitter(jittered_projection,jitter);
    const Mat4 camera_projection{jittered_projection};
    const Mat4 view_projection=multiply(camera_projection,camera_view);
    const Mat4 unjittered_view_projection=multiply(unjittered_camera_projection,camera_view);
    float view_projection_delta{};
    if (temporal_history_valid_)
        for (std::size_t index=0;index<view_projection.value.size();++index)
            view_projection_delta=std::max(view_projection_delta,std::abs(view_projection.value[index]-previous_view_projection_[index]));
    const bool camera_cut=temporal_history_valid_ && view_projection_delta>2.0F;
    float unjittered_projection_delta{};
    if(frame_index_>0U) {
        for(std::size_t index=0;index<unjittered_view_projection.value.size();++index)
            unjittered_projection_delta=std::max(unjittered_projection_delta,
                std::abs(unjittered_view_projection.value[index]-previous_unjittered_view_projection_[index]));
    }
    if(camera_cut)++temporal_camera_cut_epoch_;
    std::ostringstream temporal_projection_identity;
    temporal_projection_identity << (camera_orthographic?"orthographic":"perspective")
        << ":near=" << std::setprecision(7) << camera_near << ":far=" << camera_far
        << (camera_orthographic?":height=":":vfov=")
        << (camera_orthographic?camera_orthographic_height:camera_fov)
        << ":cut=" << temporal_camera_cut_epoch_;
    std::ostringstream temporal_quality_identity;
    temporal_quality_identity << "shared-temporal-denoise/0.1:render=" << render_width_ << 'x' << render_height_
        << ":output=" << post_width_ << 'x' << post_height_ << ":scale=" << std::setprecision(6) << render_scale_;
    const TemporalHistoryRequest temporal_request{
        TemporalHistoryConsumer::taa,
        {{post_width_,post_height_},"rgba16f+r32f+rgba16f",frame_index_,
            active_camera_id_.empty()?"camera.unowned":active_camera_id_,temporal_projection_identity.str(),
            hybrid_pixel_profile_?"hybrid:"+hybrid_pixel_profile_->profile_id:"raster",
            temporal_quality_identity.str()},
        !hybrid_pixel_active(),std::nullopt};
    const auto temporal_history_plan=temporal_history_authority_.plan(temporal_request);
    if(!temporal_history_plan.valid) {
        last_error_=temporal_history_plan.code+": "+temporal_history_plan.detail;
        return;
    }
    temporal_history_valid_=temporal_history_plan.use_previous;
    gpu_occlusion_history_valid_=gpu_occlusion_enabled_&&gpu_occlusion_pipeline_&&
        gpu_occlusion_statistics_buffer_&&depth_pyramid_texture_&&depth_pyramid_history_ready_&&
        !camera_orthographic&&!camera_cut&&
        frame_index_>0U&&previous_temporal_frame_+1U==frame_index_&&
        unjittered_projection_delta<=0.000001F;
    if(!gpu_occlusion_enabled_)gpu_occlusion_fallback_reason_="disabled-by-policy";
    else if(!gpu_occlusion_pipeline_||!gpu_occlusion_statistics_buffer_)gpu_occlusion_fallback_reason_="pipeline-or-statistics-unavailable";
    else if(!depth_pyramid_texture_)gpu_occlusion_fallback_reason_="shared-hiz-unavailable";
    else if(!depth_pyramid_history_ready_)gpu_occlusion_fallback_reason_="shared-hiz-history-unavailable";
    else if(camera_orthographic)gpu_occlusion_fallback_reason_="orthographic-conservative-fallback";
    else if(camera_cut)gpu_occlusion_fallback_reason_="camera-cut";
    else if(frame_index_==0U||previous_temporal_frame_+1U!=frame_index_)gpu_occlusion_fallback_reason_="history-unavailable";
    else if(unjittered_projection_delta>0.000001F)gpu_occlusion_fallback_reason_="camera-motion-conservative-fallback";
    else gpu_occlusion_fallback_reason_="none";
    // Consume last frame's readiness once. The reduce pass republishes it
    // only after recording the complete mip chain, so an early-returning
    // frame cannot accidentally reuse an older pyramid as fresh history.
    depth_pyramid_history_ready_=false;
    ssgi_plan_=build_screen_space_global_illumination_plan(ssgi_config_,hybrid_pixel_active(),{
        depth_pyramid_texture_&&depth_pyramid_seed_pipeline_&&depth_pyramid_reduce_pipeline_,
        normal_texture_!=nullptr,
        reflection_properties_texture_!=nullptr&&indirect_lighting_texture_!=nullptr,
        ssgi_history_textures_[0]&&ssgi_history_textures_[1]&&
            ssgi_bent_normal_history_textures_[0]&&ssgi_bent_normal_history_textures_[1]},
        {ssgi_history_valid_,camera_cut,false,false});
    if(!ssgi_plan_.valid) {
        last_error_=ssgi_plan_.code+": "+ssgi_plan_.detail;
        return;
    }
    const TemporalHistoryRequest ssgi_history_request{
        TemporalHistoryConsumer::ssgi,
        {{render_width_,render_height_},"rgba16f+rgba16f",frame_index_,
            active_camera_id_.empty()?"camera.unowned":active_camera_id_,temporal_projection_identity.str(),
            hybrid_pixel_profile_?"hybrid:"+hybrid_pixel_profile_->profile_id:"raster",
            screen_space_global_illumination_fingerprint(ssgi_plan_)},
        ssgi_plan_.enabled&&ssgi_plan_.history.required,std::nullopt};
    const auto ssgi_history_plan=temporal_history_authority_.plan(ssgi_history_request);
    if(!ssgi_history_plan.valid) {
        last_error_=ssgi_history_plan.code+": "+ssgi_history_plan.detail;
        return;
    }
    ssgi_history_valid_=ssgi_history_plan.use_previous;
    ssr_plan_=build_screen_space_reflections_plan(ssr_config_,hybrid_pixel_active(),{
        depth_pyramid_texture_&&depth_pyramid_seed_pipeline_&&depth_pyramid_reduce_pipeline_,
        ao_composited_hdr_texture_!=nullptr,
        reflection_properties_texture_!=nullptr&&specular_indirect_texture_!=nullptr});
    if(!ssr_plan_.valid) {
        last_error_=ssr_plan_.code+": "+ssr_plan_.detail;
        return;
    }
    const TemporalHistoryRequest ssr_history_request{
        TemporalHistoryConsumer::ssr,
        {{render_width_,render_height_},"rgba16f",frame_index_,
            active_camera_id_.empty()?"camera.unowned":active_camera_id_,temporal_projection_identity.str(),
            hybrid_pixel_profile_?"hybrid:"+hybrid_pixel_profile_->profile_id:"raster",
            screen_space_reflections_fingerprint(ssr_plan_)},
        ssr_plan_.enabled&&ssr_plan_.temporal_history_required,std::nullopt};
    const auto ssr_history_plan=temporal_history_authority_.plan(ssr_history_request);
    if(!ssr_history_plan.valid) {
        last_error_=ssr_history_plan.code+": "+ssr_history_plan.detail;
        return;
    }
    ssr_history_valid_=ssr_history_plan.use_previous;
    CascadedShadowConfig shadow_config;
    shadow_config.resolution=shadow_size;
    shadow_config.maximum_distance=std::min(80.0F,camera_far);
    const auto shadow_plan=build_cascaded_shadow_plan(
        {camera_position.x,camera_position.y,camera_position.z},{camera_target.x,camera_target.y,camera_target.z},{0,1,0},
        camera_fov,static_cast<float>(render_width_)/static_cast<float>(render_height_),camera_near,camera_far,
        {light_direction.x,light_direction.y,light_direction.z},shadow_config);
    if (!shadow_plan.valid) { last_error_=shadow_plan.code+": "+shadow_plan.detail; return; }
    std::array<Mat4,shadow_cascade_count> cascade_view_projections{};
    for (std::uint32_t index=0;index<shadow_cascade_count;++index) {
        const auto& cascade=shadow_plan.cascades[index];
        cascade_view_projections[index]=render_matrix(cascade.view_projection);
        shadow_splits_[index]=cascade.far_distance; shadow_radii_[index]=cascade.radius;
        shadow_world_units_per_texel_[index]=cascade.world_units_per_texel;
    }
    const auto camera_frustum=extract_visibility_frustum(unjittered_view_projection.value);
    std::array<VisibilityFrustum,shadow_cascade_count> cascade_frustums{};
    for (std::uint32_t index=0;index<shadow_cascade_count;++index)
        cascade_frustums[index]=extract_visibility_frustum(cascade_view_projections[index].value);
    if (!camera_frustum.valid || std::ranges::any_of(cascade_frustums,[](const auto& frustum){return !frustum.valid;})) {
        last_error_="visibility.invalid-frustum"; return;
    }
    shadow_texture_bytes_=shadow_plan.texture_bytes;
    const Mat4 light_view_projection=cascade_view_projections[0];
    const Vec3 camera_forward=normalize(camera_target-camera_position);
    const Vec3 camera_right=normalize(cross(camera_forward,{0,1,0}));
    const Vec3 camera_up=cross(camera_right,camera_forward);
    struct DrawItem {
        ObjectData data;
        const std::vector<std::array<float,16>>* skinning_matrices;
        const std::vector<std::array<float,16>>* previous_skinning_matrices;
        SDL_GPUBuffer* vertex_buffer;
        SDL_GPUBuffer* index_buffer;
        SDL_GPUTexture* base_color_texture;
        SDL_GPUTexture* normal_texture;
        SDL_GPUTexture* metallic_roughness_texture;
        SDL_GPUTexture* occlusion_texture;
        SDL_GPUTexture* emissive_texture;
        Uint32 index_count;
        Uint32 first_index;
        bool casts_shadows;
        bool double_sided;
        bool transparent;
        float camera_distance_squared;
        VisibilitySphere bounds;
        bool camera_visible;
        std::string history_key;
        const GpuBatchKey* gpu_batch_key{};
    };
    struct SpriteDrawItem final {
        Mat4 model;
        Mat4 previous_model;
        std::array<float,4> uv_rect{};
        std::array<float,4> local_rect{};
        std::array<std::uint32_t,4> identity_flags{};
        SDL_GPUTexture* texture{};
        SDL_GPUTexture* normal_texture{};
        SDL_GPUTexture* emissive_mask_texture{};
        SDL_GPUTexture* depth_texture{};
        SDL_GPUSampler* base_sampler{};
        SDL_GPUSampler* normal_sampler{};
        SDL_GPUSampler* emissive_mask_sampler{};
        SDL_GPUSampler* depth_sampler{};
        std::array<float,4> material_parameters{};
        std::array<float,4> emissive_color{};
        std::array<float,4> surface_parameters{};
        bool casts_shadows{};
        VisibilitySphere bounds{};
        bool transparent{};
        std::string history_key;
        std::string sorting_layer;
        std::int32_t sorting_order{};
        std::string stable_id;
        std::string allocation_group;
        std::size_t allocation_ordinal{};
        std::size_t allocation_count{1};
        std::size_t instance_slot{};
    };
    std::vector<DrawItem> objects;
    if(gpu_batch_texture_table_revision_!=texture_resources_.revision()) {
        gpu_batch_resource_keys_.clear();gpu_batch_texture_table_revision_=texture_resources_.revision();
        gpu_driven_cached_plan_valid_=false;
    }
    auto& frame_gpu_batch_keys=gpu_batch_resource_keys_;
    std::vector<SpriteDrawItem> sprites;
    std::vector<std::string> stable_object_entities;
    stable_object_entities.reserve(render_world.instances.size());
    for(const auto& entity:render_world.instances)
        if(entity.visible&&!entity.vfx_particle)stable_object_entities.push_back(entity.entity_id);
    std::sort(stable_object_entities.begin(),stable_object_entities.end());
    stable_object_entities.erase(std::unique(stable_object_entities.begin(),stable_object_entities.end()),stable_object_entities.end());
    object_id_entities_.assign(1,{});object_id_entities_.insert(object_id_entities_.end(),stable_object_entities.begin(),stable_object_entities.end());
    std::unordered_map<std::string,std::uint32_t> stable_object_ids;
    stable_object_ids.reserve(stable_object_entities.size());
    for(std::size_t index=0;index<stable_object_entities.size();++index)
        stable_object_ids.emplace(stable_object_entities[index],static_cast<std::uint32_t>(index+1U));
    const auto stable_generation=[&](const std::string& asset_id) {
        std::uint64_t value=14695981039346656037ULL;
        if(const auto* asset=asset_registry_.find(asset_id);asset!=nullptr&&!asset->content_hash.empty())
            hash_string(value,asset->content_hash);
        else hash_string(value,asset_id);
        return value;
    };
    const auto batch_texture_binding=[](const TextureResourceHandle handle,const std::string& semantic,
                                        const std::string& fallback_id) {
        return GpuBatchTextureBindingDescriptor{handle,semantic,{fallback_id}};
    };
    std::string gpu_batch_identity_failure;
    std::size_t visible_entities = 0;
    std::size_t shadow_entities = 0;
    for (const auto& entity : render_world.instances) {
        if (!entity.visible) continue;
        if (entity.vfx_particle) continue;
        const auto object_id=stable_object_ids.at(entity.entity_id);
        const bool plane=entity.mesh_asset=="asset.primitive.plane";
        const bool cube=entity.mesh_asset=="asset.primitive.cube";
        const bool sphere=entity.mesh_asset=="asset.primitive.sphere";
        const Mat4 model=model_matrix({entity.position[0],entity.position[1],entity.position[2]},
            {entity.scale[0],entity.scale[1],entity.scale[2]},entity.rotation);
        const float camera_distance_squared=(entity.position[0]-camera_position.x)*(entity.position[0]-camera_position.x)+
            (entity.position[1]-camera_position.y)*(entity.position[1]-camera_position.y)+
            (entity.position[2]-camera_position.z)*(entity.position[2]-camera_position.z);
        if (plane || cube || sphere) {
            std::array<float,4> color{entity.material.base_color[0],entity.material.base_color[1],entity.material.base_color[2],1.0F};
            std::array<float,4> material{entity.material.metallic,entity.material.roughness,0.0F,entity.receives_shadows?1.0F:0.0F};
            SDL_GPUTexture* base_color_texture=white_texture_;
            if (entity.material.base_color_texture=="asset.texture.checker")
                base_color_texture=checker_texture_;
            const std::array<float,3> emissive{
                entity.material.emissive_color[0]*entity.material.emissive_intensity,
                entity.material.emissive_color[1]*entity.material.emissive_intensity,
                entity.material.emissive_color[2]*entity.material.emissive_intensity};
            SDL_GPUTexture* emissive_texture=entity.material.emissive_intensity>0.0F?white_texture_:black_texture_;
            const auto maximum_scale=std::max({entity.scale[0],entity.scale[1],entity.scale[2]});
            VisibilitySphere bounds{{entity.position[0],entity.position[1],entity.position[2]},
                (plane?9.8995F:(sphere?1.0F:1.7321F))*maximum_scale};
            if (!entity.skinning_matrices.empty()) bounds.radius*=1.5F;
            objects.push_back({{model,view_projection,light_view_projection,color,material,{emissive[0],emissive[1],emissive[2],1},{1,0.5F,0,0},{object_id,0,0,0}},
                entity.skinning_matrices.empty()?nullptr:&entity.skinning_matrices,nullptr,
                vertex_buffer_,index_buffer_,base_color_texture,flat_normal_texture_,linear_white_texture_,
                linear_white_texture_,emissive_texture,plane?6U:(sphere?builtin_sphere_index_count:36U),
                plane?36U:(sphere?builtin_sphere_first_index:0U),entity.casts_shadows,false,false,camera_distance_squared,
                bounds,sphere_intersects_frustum(camera_frustum,bounds),entity.entity_id+":"+std::to_string(plane?36U:(sphere?42U:0U))});
            if(gpu_driven_enabled_) {
                const auto cache_id=entity.mesh_asset+":"+std::to_string(objects.back().first_index)+":"+
                    std::to_string(objects.back().index_count)+":"+entity.material.base_color_texture+":"+
                    (entity.material.emissive_intensity>0.0F?"emissive":"dark");
                auto found=frame_gpu_batch_keys.find(cache_id);
                if(found==frame_gpu_batch_keys.end()) {
                    GpuBatchResourceIdentityDescriptor batch_descriptor;
                    batch_descriptor.geometry_id=entity.mesh_asset+":"+std::to_string(objects.back().first_index)+":"+std::to_string(objects.back().index_count);
                    batch_descriptor.geometry_generation=stable_generation(entity.mesh_asset);
                    batch_descriptor.material_id="builtin-pbr";batch_descriptor.material_generation=1U;batch_descriptor.raster_generation=1U;
                    batch_descriptor.textures={
                        batch_texture_binding({},"base-color",entity.material.base_color_texture=="asset.texture.checker"?"asset.texture.checker":"runtime.fallback.white"),
                        batch_texture_binding({},"normal","runtime.fallback.flat-normal"),
                        batch_texture_binding({},"metallic-roughness","runtime.fallback.linear-white"),
                        batch_texture_binding({},"occlusion","runtime.fallback.linear-white"),
                        batch_texture_binding({},"emissive",entity.material.emissive_intensity>0.0F?"runtime.fallback.white":"runtime.fallback.black")};
                    auto batch_identity=build_gpu_batch_resource_identity(texture_resources_,batch_descriptor);
                    if(!batch_identity.valid) {
                        if(gpu_batch_identity_failure.empty())gpu_batch_identity_failure=
                            "gpu-batch-identity."+batch_identity.code+": "+batch_identity.detail;
                    } else found=frame_gpu_batch_keys.emplace(cache_id,std::move(batch_identity.key)).first;
                }
                if(found!=frame_gpu_batch_keys.end())objects.back().gpu_batch_key=&found->second;
            }
            ++visible_entities;
            if (entity.casts_shadows) ++shadow_entities;
            continue;
        }
        const auto imported=gpu_meshes_.find(entity.mesh_asset);
        if (imported==gpu_meshes_.end()) continue;
        for (const auto& primitive : imported->second.primitives) {
            auto color=primitive.base_color;
            std::array<float,4> material{primitive.metallic,primitive.roughness,primitive.unlit?1.0F:0.0F,
                entity.receives_shadows?1.0F:0.0F};
            if (entity.material_override) {
                color={entity.material.base_color[0],entity.material.base_color[1],entity.material.base_color[2],1.0F};
                material[0]=entity.material.metallic; material[1]=entity.material.roughness; material[2]=0.0F;
            }
            std::array<float,3> emissive_factor=primitive.emissive_factor;
            if (entity.material_override && entity.material.emissive_intensity>0.0F)
                emissive_factor={entity.material.emissive_color[0]*entity.material.emissive_intensity,
                    entity.material.emissive_color[1]*entity.material.emissive_intensity,
                    entity.material.emissive_color[2]*entity.material.emissive_intensity};
            const auto srgb_texture=[&](const int index, SDL_GPUTexture* fallback) {
                if(index<0||static_cast<std::size_t>(index)>=imported->second.textures_srgb.size())return fallback;
                auto* resolved=texture_resources_.resolve(imported->second.textures_srgb[static_cast<std::size_t>(index)]);
                return resolved?resolved:fallback;
            };
            const auto linear_texture=[&](const int index, SDL_GPUTexture* fallback) {
                if(index<0||static_cast<std::size_t>(index)>=imported->second.textures_linear.size())return fallback;
                auto* resolved=texture_resources_.resolve(imported->second.textures_linear[static_cast<std::size_t>(index)]);
                return resolved?resolved:fallback;
            };
            const float alpha_mode=primitive.alpha_mode=="MASK"?1.0F:(primitive.alpha_mode=="BLEND"?2.0F:0.0F);
            const auto& matrix=model.value;const auto& local_center=primitive.bounds_center;
            const auto maximum_scale=std::max({std::abs(entity.scale[0]),std::abs(entity.scale[1]),std::abs(entity.scale[2])});
            VisibilitySphere bounds{{matrix[0]*local_center[0]+matrix[4]*local_center[1]+matrix[8]*local_center[2]+matrix[12],
                matrix[1]*local_center[0]+matrix[5]*local_center[1]+matrix[9]*local_center[2]+matrix[13],
                matrix[2]*local_center[0]+matrix[6]*local_center[1]+matrix[10]*local_center[2]+matrix[14]},
                primitive.bounds_radius*maximum_scale};
            objects.push_back({{model,view_projection,light_view_projection,color,material,
                {emissive_factor[0],emissive_factor[1],emissive_factor[2],primitive.normal_scale},
                {primitive.occlusion_strength,primitive.alpha_cutoff,alpha_mode,primitive.double_sided?1.0F:0.0F},{object_id,0,0,0}},
                primitive.skin>=0&&!entity.skinning_matrices.empty()?&entity.skinning_matrices:nullptr,nullptr,
                imported->second.vertex_buffer,imported->second.index_buffer,
                srgb_texture(primitive.base_color_image,white_texture_),linear_texture(primitive.normal_image,flat_normal_texture_),
                linear_texture(primitive.metallic_roughness_image,linear_white_texture_),linear_texture(primitive.occlusion_image,linear_white_texture_),
                entity.material_override && entity.material.emissive_intensity>0.0F?white_texture_:srgb_texture(primitive.emissive_image,black_texture_),primitive.index_count,primitive.first_index,
                entity.casts_shadows && primitive.alpha_mode!="BLEND",primitive.double_sided,primitive.alpha_mode=="BLEND",camera_distance_squared,
                bounds,sphere_intersects_frustum(camera_frustum,bounds),entity.entity_id+":"+std::to_string(primitive.first_index)});
            if(gpu_driven_enabled_) {
                const auto cache_id=entity.mesh_asset+":"+std::to_string(primitive.first_index)+":"+std::to_string(primitive.index_count)+":"+
                    std::to_string(primitive.base_color_image)+":"+std::to_string(primitive.normal_image)+":"+
                    std::to_string(primitive.metallic_roughness_image)+":"+std::to_string(primitive.occlusion_image)+":"+
                    std::to_string(primitive.emissive_image)+":"+(entity.material_override&&entity.material.emissive_intensity>0.0F?"override-emissive":"authored-emissive")+":"+
                    (primitive.double_sided?"double":"single");
                auto found=frame_gpu_batch_keys.find(cache_id);
                if(found==frame_gpu_batch_keys.end()) {
                    GpuBatchResourceIdentityDescriptor batch_descriptor;
                    batch_descriptor.geometry_id=entity.mesh_asset+":"+std::to_string(primitive.first_index)+":"+std::to_string(primitive.index_count);
                    batch_descriptor.geometry_generation=stable_generation(entity.mesh_asset);
                    batch_descriptor.material_id=batch_descriptor.geometry_id+":material";
                    batch_descriptor.material_generation=batch_descriptor.geometry_generation;
                    batch_descriptor.raster_generation=primitive.double_sided?2U:1U;
                    const auto handle_at=[](const std::vector<TextureResourceHandle>& handles,const int index) {
                        return index>=0&&static_cast<std::size_t>(index)<handles.size()?handles[static_cast<std::size_t>(index)]:TextureResourceHandle{};
                    };
                    batch_descriptor.textures={
                        batch_texture_binding(handle_at(imported->second.textures_srgb,primitive.base_color_image),"base-color","runtime.fallback.white"),
                        batch_texture_binding(handle_at(imported->second.textures_linear,primitive.normal_image),"normal","runtime.fallback.flat-normal"),
                        batch_texture_binding(handle_at(imported->second.textures_linear,primitive.metallic_roughness_image),"metallic-roughness","runtime.fallback.linear-white"),
                        batch_texture_binding(handle_at(imported->second.textures_linear,primitive.occlusion_image),"occlusion","runtime.fallback.linear-white"),
                        entity.material_override&&entity.material.emissive_intensity>0.0F
                            ?batch_texture_binding({},"emissive","runtime.fallback.white")
                            :batch_texture_binding(handle_at(imported->second.textures_srgb,primitive.emissive_image),"emissive","runtime.fallback.black")};
                    auto batch_identity=build_gpu_batch_resource_identity(texture_resources_,batch_descriptor);
                    if(!batch_identity.valid) {
                        if(gpu_batch_identity_failure.empty())gpu_batch_identity_failure=
                            "gpu-batch-identity."+batch_identity.code+": "+batch_identity.detail;
                    } else found=frame_gpu_batch_keys.emplace(cache_id,std::move(batch_identity.key)).first;
                }
                if(found!=frame_gpu_batch_keys.end())objects.back().gpu_batch_key=&found->second;
            }
        }
        ++visible_entities;
        if (entity.casts_shadows) ++shadow_entities;
    }
    sprite_draws_missing_texture_=0;sprite_material_textures_missing_=0;sprite_draws_submitted_=0;sprite_instances_submitted_=0;sprite_draws_saved_=0;
    sprite_lit_instances_=0;sprite_unlit_instances_=0;sprite_shadow_receivers_=0;sprite_shadow_casters_=0;
    tilemap_count_=render_world.tilemaps.size();tile_cell_instances_requested_=render_world.tile_cells.size();tile_cell_instances_submitted_=0;
    tilemap_visible_chunks_=render_world.tilemap_visible_chunk_count;tilemap_culled_chunks_=render_world.tilemap_culled_chunk_count;
    tilemap_bake_cache_hits_=render_world.tilemap_bake_cache_hits;tilemap_bake_cache_rebuilds_=render_world.tilemap_bake_cache_rebuilds;
    tilemap_bake_cache_evictions_=render_world.tilemap_bake_cache_evictions;tilemap_bake_cached_chunks_=render_world.tilemap_bake_cached_chunks;
    tilemap_bake_retained_offscreen_chunks_=render_world.tilemap_bake_retained_offscreen_chunks;
    tilemap_early_visibility_applied_=render_world.tilemap_early_visibility_applied;tilemap_chunks_resolved_=render_world.tilemap_chunks_resolved;
    tilemap_chunks_skipped_before_resolution_=render_world.tilemap_chunks_skipped_before_resolution;
    tilemap_cells_skipped_before_resolution_=render_world.tilemap_cells_skipped_before_resolution;
    tilemap_chunk_ranges_=render_world.tile_chunk_ranges.size();tilemap_largest_chunk_range_=0;
    for(const auto& range:render_world.tile_chunk_ranges)tilemap_largest_chunk_range_=std::max(tilemap_largest_chunk_range_,range.cell_count);
    sprites.reserve(render_world.sprites.size()+render_world.tile_cells.size());
    const auto sprite_bounds=[](const Mat4& model,const std::array<float,4>& rect) {
        const auto local_x=(rect[0]+rect[2])*0.5F;const auto local_y=(rect[1]+rect[3])*0.5F;
        const auto& matrix=model.value;
        const std::array<float,3> center{matrix[0]*local_x+matrix[4]*local_y+matrix[12],
            matrix[1]*local_x+matrix[5]*local_y+matrix[13],
            matrix[2]*local_x+matrix[6]*local_y+matrix[14]};
        const auto axis_x=std::sqrt(matrix[0]*matrix[0]+matrix[1]*matrix[1]+matrix[2]*matrix[2]);
        const auto axis_y=std::sqrt(matrix[4]*matrix[4]+matrix[5]*matrix[5]+matrix[6]*matrix[6]);
        const auto local_radius=std::hypot((rect[2]-rect[0])*0.5F,(rect[3]-rect[1])*0.5F);
        return VisibilitySphere{center,local_radius*std::max(axis_x,axis_y)};
    };
    for(const auto& sprite:render_world.sprites) {
        if(!sprite.visible)continue;
        const auto texture=sprite_textures_.find(sprite.texture_asset);
        auto* base_texture=texture==sprite_textures_.end()?nullptr:texture_resources_.resolve(texture->second);
        if(!base_texture){++sprite_draws_missing_texture_;continue;}
        const auto material_texture=[&](const std::string& asset_id,SDL_GPUTexture* fallback) {
            if(asset_id.empty())return fallback;
            const auto found=sprite_linear_textures_.find(asset_id);
            if(found!=sprite_linear_textures_.end())if(auto* resolved=texture_resources_.resolve(found->second))return resolved;
            ++sprite_material_textures_missing_;return fallback;
        };
        auto* normal_texture=material_texture(sprite.normal_texture_asset,flat_normal_texture_);
        auto* emissive_mask_texture=material_texture(sprite.emissive_mask_texture_asset,linear_white_texture_);
        auto* depth_texture=material_texture(sprite.depth_texture_asset,linear_black_texture_);
        const bool nearest=sprite.sampling=="nearest";
        object_id_entities_.push_back(sprite.entity_id);
        const auto object_id=static_cast<std::uint32_t>(object_id_entities_.size()-1U);
        const auto ppu=std::max(sprite.pixels_per_unit,0.001F);
        const auto left=(static_cast<float>(sprite.trim_offset[0])-sprite.pivot[0]*static_cast<float>(sprite.source_size[0]))/ppu;
        const auto top=(sprite.pivot[1]*static_cast<float>(sprite.source_size[1])-static_cast<float>(sprite.trim_offset[1]))/ppu;
        const auto right=left+static_cast<float>(sprite.pixel_rect[2])/ppu;
        const auto bottom=top-static_cast<float>(sprite.pixel_rect[3])/ppu;
        const auto model=model_matrix({sprite.position[0],sprite.position[1],sprite.position[2]},
            {sprite.scale[0],sprite.scale[1],sprite.scale[2]},sprite.rotation);
        const std::array<float,4> local_rect{left,top,right,bottom};
        sprites.push_back({model,{},sprite.uv_rect,local_rect,
            {object_id,sprite.alpha_mode=="blend"?2U:1U,sprite.flip_x?1U:0U,sprite.flip_y?1U:0U},
            base_texture,normal_texture,emissive_mask_texture,depth_texture,
            sprite_sampler(base_texture,nearest),sprite_sampler(normal_texture,nearest),
            sprite_sampler(emissive_mask_texture,nearest),sprite_sampler(depth_texture,nearest),
            {sprite.normal_strength,sprite.emissive_intensity,sprite.depth_bias,sprite.depth_texture_asset.empty()?0.0F:1.0F},
            {sprite.emissive_color[0],sprite.emissive_color[1],sprite.emissive_color[2],1.0F},
            {sprite.metallic,sprite.roughness,sprite.shading_model=="lit"?1.0F:0.0F,sprite.receives_shadows?1.0F:0.0F},
            sprite.casts_shadows&&sprite.alpha_mode!="blend",sprite_bounds(model,local_rect),
            sprite.alpha_mode=="blend",sprite.entity_id+":sprite",sprite.sorting_layer,sprite.sorting_order,sprite.entity_id,
            "sprite/"+sprite.entity_id,0,1,0});
        if(sprite.shading_model=="lit")++sprite_lit_instances_;else ++sprite_unlit_instances_;
        if(sprite.receives_shadows&&sprite.shading_model=="lit")++sprite_shadow_receivers_;
        if(sprite.casts_shadows&&sprite.alpha_mode!="blend"){++sprite_shadow_casters_;++shadow_entities;}
        ++visible_entities;
    }
    std::vector<std::string> tile_allocation_groups(render_world.tile_cells.size());
    std::vector<std::size_t> tile_allocation_ordinals(render_world.tile_cells.size());
    std::vector<std::size_t> tile_allocation_counts(render_world.tile_cells.size(),1);
    for(const auto& range:render_world.tile_chunk_ranges) {
        const auto end=std::min(render_world.tile_cells.size(),range.first_cell+range.cell_count);
        for(auto index=range.first_cell;index<end;++index) {
            tile_allocation_groups[index]="tile/"+range.key;
            tile_allocation_ordinals[index]=index-range.first_cell;
            tile_allocation_counts[index]=range.cell_count;
        }
    }
    for(std::size_t cell_index=0;cell_index<render_world.tile_cells.size();++cell_index) {
        const auto& cell=render_world.tile_cells[cell_index];
        if(!cell.visible)continue;
        const auto texture=sprite_textures_.find(cell.texture_asset);
        auto* base_texture=texture==sprite_textures_.end()?nullptr:texture_resources_.resolve(texture->second);
        if(!base_texture){++sprite_draws_missing_texture_;continue;}
        const auto material_texture=[&](const std::string& asset_id,SDL_GPUTexture* fallback) {
            if(asset_id.empty())return fallback;
            const auto found=sprite_linear_textures_.find(asset_id);
            if(found!=sprite_linear_textures_.end())if(auto* resolved=texture_resources_.resolve(found->second))return resolved;
            ++sprite_material_textures_missing_;return fallback;
        };
        object_id_entities_.push_back(cell.stable_id);
        const auto object_id=static_cast<std::uint32_t>(object_id_entities_.size()-1U);
        const auto model=model_matrix({cell.position[0],cell.position[1],cell.position[2]},
            {cell.scale[0],cell.scale[1],cell.scale[2]},cell.rotation);
        auto* normal_texture=material_texture(cell.normal_texture_asset,flat_normal_texture_);
        auto* emissive_mask_texture=material_texture(cell.emissive_mask_texture_asset,linear_white_texture_);
        auto* depth_texture=material_texture(cell.depth_texture_asset,linear_black_texture_);
        const bool nearest=cell.sampling=="nearest";
        sprites.push_back({model,{},cell.uv_rect,cell.local_rect,
            {object_id,cell.alpha_mode=="blend"?2U:1U,cell.flip_x?1U:0U,cell.flip_y?1U:0U},
            base_texture,normal_texture,emissive_mask_texture,depth_texture,
            sprite_sampler(base_texture,nearest),sprite_sampler(normal_texture,nearest),
            sprite_sampler(emissive_mask_texture,nearest),sprite_sampler(depth_texture,nearest),
            {cell.normal_strength,cell.emissive_intensity,cell.depth_bias,cell.depth_texture_asset.empty()?0.0F:1.0F},
            {cell.emissive_color[0],cell.emissive_color[1],cell.emissive_color[2],1.0F},
            {cell.metallic,cell.roughness,cell.shading_model=="lit"?1.0F:0.0F,cell.receives_shadows?1.0F:0.0F},
            cell.casts_shadows&&cell.alpha_mode!="blend",sprite_bounds(model,cell.local_rect),
            cell.alpha_mode=="blend",cell.stable_id+":tile",cell.sorting_layer,cell.sorting_order,cell.stable_id,
            tile_allocation_groups[cell_index].empty()?"tile/"+cell.stable_id:tile_allocation_groups[cell_index],
            tile_allocation_ordinals[cell_index],tile_allocation_counts[cell_index],0});
        if(cell.shading_model=="lit")++sprite_lit_instances_;else ++sprite_unlit_instances_;
        if(cell.receives_shadows&&cell.shading_model=="lit")++sprite_shadow_receivers_;
        if(cell.casts_shadows&&cell.alpha_mode!="blend"){++sprite_shadow_casters_;++shadow_entities;}
        ++tile_cell_instances_submitted_;
        ++visible_entities;
    }
    std::ranges::stable_sort(sprites,[](const SpriteDrawItem& left,const SpriteDrawItem& right) {
        if(left.transparent!=right.transparent)return !left.transparent;
        if(left.sorting_layer!=right.sorting_layer)return left.sorting_layer<right.sorting_layer;
        if(left.sorting_order!=right.sorting_order)return left.sorting_order<right.sorting_order;
        return left.stable_id<right.stable_id;
    });
    visible_renderables_=visible_entities;
    const Mat4 previous_view_projection=temporal_history_valid_?Mat4{previous_view_projection_}:view_projection;
    for (auto& object:objects) {
        const auto previous_model=previous_models_.find(object.history_key);
        object.data.previous_model=previous_model==previous_models_.end()?object.data.model:Mat4{previous_model->second};
        object.data.previous_view_projection=previous_view_projection;
        const auto previous_skin=previous_skinning_matrices_.find(object.history_key);
        object.previous_skinning_matrices=previous_skin==previous_skinning_matrices_.end()
            ? object.skinning_matrices:&previous_skin->second;
        object.data.object_identity[1]=object.skinning_matrices?
            static_cast<std::uint32_t>(std::min(object.skinning_matrices->size(),static_cast<std::size_t>(SkeletalPose::maximum_joints))):0U;
        object.data.object_identity[2]=object.previous_skinning_matrices?
            static_cast<std::uint32_t>(std::min(object.previous_skinning_matrices->size(),static_cast<std::size_t>(SkeletalPose::maximum_joints))):0U;
    }
    for(auto& sprite:sprites) {
        const auto previous_model=previous_models_.find(sprite.history_key);
        sprite.previous_model=previous_model==previous_models_.end()?sprite.model:Mat4{previous_model->second};
    }
    sprite_instances_dropped_=0;sprite_stable_range_evictions_=sprite_range_allocator_.sweep(render_world.frame_index,120);
    for(auto& sprite:sprites) {
        const auto allocation=sprite_range_allocator_.acquire(sprite.allocation_group,sprite.allocation_count,render_world.frame_index);
        if(!allocation.valid||sprite.allocation_ordinal>=allocation.count) {
            sprite.instance_slot=sprite_instance_capacity;++sprite_instances_dropped_;
            if(sprite.allocation_group.starts_with("tile/")&&tile_cell_instances_submitted_>0)--tile_cell_instances_submitted_;
        }
        else sprite.instance_slot=allocation.first+sprite.allocation_ordinal;
    }
    std::erase_if(sprites,[](const SpriteDrawItem& sprite){return sprite.instance_slot>=sprite_instance_capacity;});
    const auto range_statistics=sprite_range_allocator_.statistics();
    std::vector<GpuSpriteInstance> current_instances(range_statistics.high_water);sprite_instances_uploaded_=0;sprite_instance_upload_bytes_=0;
    sprite_instance_dirty_ranges_=0;sprite_instances_reused_=0;
    const auto retained_bytes=std::min(sprite_instance_mirror_.size(),current_instances.size()*sizeof(GpuSpriteInstance));
    if(retained_bytes>0)std::memcpy(current_instances.data(),sprite_instance_mirror_.data(),retained_bytes);
    std::vector<std::uint32_t> current_draw_indices;current_draw_indices.reserve(sprites.size());
    for(const auto& sprite:sprites) {
        current_instances[sprite.instance_slot]={sprite.model,sprite.previous_model,sprite.uv_rect,sprite.local_rect,
            sprite.identity_flags,sprite.material_parameters,sprite.emissive_color,sprite.surface_parameters};
        current_draw_indices.push_back(static_cast<std::uint32_t>(sprite.instance_slot));
    }
    struct DirtyRange final {std::size_t first{};std::size_t count{};std::size_t upload_offset{};};
    std::vector<DirtyRange> dirty_ranges;const auto old_count=sprite_instance_mirror_.size()/sizeof(GpuSpriteInstance);
    for(std::size_t index=0;index<current_instances.size();) {
        const bool dirty=index>=old_count||std::memcmp(sprite_instance_mirror_.data()+index*sizeof(GpuSpriteInstance),&current_instances[index],sizeof(GpuSpriteInstance))!=0;
        if(!dirty){++sprite_instances_reused_;++index;continue;}
        const auto first=index++;while(index<current_instances.size()&&(index>=old_count||
            std::memcmp(sprite_instance_mirror_.data()+index*sizeof(GpuSpriteInstance),&current_instances[index],sizeof(GpuSpriteInstance))!=0))++index;
        dirty_ranges.push_back({first,index-first,static_cast<std::size_t>(sprite_instance_upload_bytes_)});
        sprite_instance_upload_bytes_+=(index-first)*sizeof(GpuSpriteInstance);
    }
    sprite_instance_dirty_ranges_=dirty_ranges.size();sprite_instances_uploaded_=current_instances.size()-sprite_instances_reused_;
    sprite_instance_upload_bytes_total_+=sprite_instance_upload_bytes_;
    if(!dirty_ranges.empty()) {
        auto* mapped=static_cast<std::byte*>(SDL_MapGPUTransferBuffer(device_,sprite_instance_upload_,true));
        if(!mapped){last_error_="Unable to map Sprite instance upload: "+std::string(SDL_GetError());return;}
        for(const auto& range:dirty_ranges)std::memcpy(mapped+range.upload_offset,current_instances.data()+range.first,range.count*sizeof(GpuSpriteInstance));
        SDL_UnmapGPUTransferBuffer(device_,sprite_instance_upload_);
        auto* copy=SDL_BeginGPUCopyPass(command);if(!copy){last_error_="Unable to begin Sprite instance upload: "+std::string(SDL_GetError());return;}
        for(const auto& range:dirty_ranges) {const SDL_GPUTransferBufferLocation source{sprite_instance_upload_,static_cast<Uint32>(range.upload_offset)};
            const SDL_GPUBufferRegion destination{sprite_instance_buffer_,static_cast<Uint32>(range.first*sizeof(GpuSpriteInstance)),
                static_cast<Uint32>(range.count*sizeof(GpuSpriteInstance))};SDL_UploadToGPUBuffer(copy,&source,&destination,false);}
        SDL_EndGPUCopyPass(copy);
    }
    sprite_instance_mirror_.resize(current_instances.size()*sizeof(GpuSpriteInstance));
    if(!current_instances.empty())std::memcpy(sprite_instance_mirror_.data(),current_instances.data(),sprite_instance_mirror_.size());
    std::vector<DirtyRange> draw_index_dirty_ranges;sprite_draw_index_upload_bytes_=0;sprite_draw_indices_uploaded_=0;
    for(std::size_t index=0;index<current_draw_indices.size();) {
        if(index<sprite_draw_index_mirror_.size()&&sprite_draw_index_mirror_[index]==current_draw_indices[index]){++index;continue;}
        const auto first=index++;while(index<current_draw_indices.size()&&
            (index>=sprite_draw_index_mirror_.size()||sprite_draw_index_mirror_[index]!=current_draw_indices[index]))++index;
        draw_index_dirty_ranges.push_back({first,index-first,static_cast<std::size_t>(sprite_draw_index_upload_bytes_)});
        sprite_draw_index_upload_bytes_+=(index-first)*sizeof(std::uint32_t);
        sprite_draw_indices_uploaded_+=index-first;
    }
    sprite_draw_index_dirty_ranges_=draw_index_dirty_ranges.size();sprite_draw_index_upload_bytes_total_+=sprite_draw_index_upload_bytes_;
    if(!draw_index_dirty_ranges.empty()) {
        auto* mapped=static_cast<std::byte*>(SDL_MapGPUTransferBuffer(device_,sprite_draw_index_upload_,true));
        if(!mapped){last_error_="Unable to map Sprite draw-index upload: "+std::string(SDL_GetError());return;}
        for(const auto& range:draw_index_dirty_ranges)std::memcpy(mapped+range.upload_offset,current_draw_indices.data()+range.first,range.count*sizeof(std::uint32_t));
        SDL_UnmapGPUTransferBuffer(device_,sprite_draw_index_upload_);
        auto* copy=SDL_BeginGPUCopyPass(command);if(!copy){last_error_="Unable to begin Sprite draw-index upload: "+std::string(SDL_GetError());return;}
        for(const auto& range:draw_index_dirty_ranges) {const SDL_GPUTransferBufferLocation source{sprite_draw_index_upload_,static_cast<Uint32>(range.upload_offset)};
            const SDL_GPUBufferRegion destination{sprite_draw_index_buffer_,static_cast<Uint32>(range.first*sizeof(std::uint32_t)),
                static_cast<Uint32>(range.count*sizeof(std::uint32_t))};SDL_UploadToGPUBuffer(copy,&source,&destination,false);}
        SDL_EndGPUCopyPass(copy);
    }
    sprite_draw_index_mirror_=std::move(current_draw_indices);
    visible_draws_=static_cast<std::size_t>(std::ranges::count_if(objects,[](const DrawItem& item){return item.camera_visible;}));
    camera_culled_draws_=objects.size()-visible_draws_;
    std::sort(objects.begin(),objects.end(),[](const DrawItem& left,const DrawItem& right){
        if (left.transparent!=right.transparent) return !left.transparent;
        if (left.transparent && left.camera_distance_squared!=right.camera_distance_squared)
            return left.camera_distance_squared>right.camera_distance_squared;
        if (left.data.object_identity[0]!=right.data.object_identity[0]) return left.data.object_identity[0]<right.data.object_identity[0];
        return left.first_index<right.first_index;
    });
    const auto compatible_instance_state=[](const DrawItem& left,const DrawItem& right) {
        return !left.transparent && !right.transparent && left.skinning_matrices==nullptr && right.skinning_matrices==nullptr &&
            left.vertex_buffer==right.vertex_buffer && left.index_buffer==right.index_buffer &&
            left.base_color_texture==right.base_color_texture && left.normal_texture==right.normal_texture &&
            left.metallic_roughness_texture==right.metallic_roughness_texture && left.occlusion_texture==right.occlusion_texture &&
            left.emissive_texture==right.emissive_texture && left.index_count==right.index_count &&
            left.first_index==right.first_index && left.double_sided==right.double_sided;
    };
    struct GpuDrivenRenderBatch final {
        std::size_t representative{};
        std::uint32_t visible_offset{};
        std::uint32_t indirect_offset{};
        std::size_t reference_visible{};
    };
    std::vector<GpuDrivenInstance> gpu_instances;
    std::vector<GpuDrivenBatch> gpu_batches;
    std::vector<GpuIndexedIndirectCommand> gpu_commands;
    std::vector<GpuDrivenRenderBatch> gpu_render_batches;
    std::vector<std::vector<std::uint32_t>> gpu_reference_visible_indices;
    std::vector<std::string> gpu_candidate_draw_ids;
    std::vector<bool> gpu_driven_items(objects.size(),false);
    gpu_instances.reserve(std::min<std::size_t>(objects.size(),gpu_driven_instance_capacity));
    const bool gpu_driven_available=gpu_visibility_pipeline_&&gpu_driven_lit_pipeline_&&gpu_driven_lit_double_sided_pipeline_&&
        gpu_driven_instance_buffer_&&gpu_driven_batch_buffer_&&gpu_driven_visible_index_buffer_&&
        gpu_driven_indirect_buffer_&&gpu_driven_upload_buffer_;
    gpu_driven_fallback_reason_=gpu_batch_identity_failure;
    const GpuBatchPlan* gpu_batch_plan_ptr{};
    std::vector<GpuBatchDrawInput> gpu_batch_inputs;
    std::unordered_map<std::string_view,std::size_t> gpu_draw_lookup;
    if(gpu_driven_enabled_&&gpu_driven_available) {
        gpu_draw_lookup.reserve(objects.size());
        std::uint64_t topology_fingerprint=14695981039346656037ULL;
        for(std::size_t object_index=0;object_index<objects.size();++object_index) {
            const auto& item=objects[object_index];
            gpu_draw_lookup.emplace(item.history_key,object_index);
            const bool eligible=!item.transparent&&item.skinning_matrices==nullptr&&item.gpu_batch_key!=nullptr;
            hash_string(topology_fingerprint,item.history_key);hash_value(topology_fingerprint,eligible);
            if(item.gpu_batch_key) {
                const auto& key=*item.gpu_batch_key;
                hash_string(topology_fingerprint,key.geometry_id);hash_value(topology_fingerprint,key.geometry_generation);
                hash_string(topology_fingerprint,key.material_id);hash_value(topology_fingerprint,key.material_generation);
                hash_value(topology_fingerprint,key.raster_generation);hash_value(topology_fingerprint,key.textures.size());
                for(const auto& texture:key.textures){hash_string(topology_fingerprint,texture.semantic);
                    hash_string(topology_fingerprint,texture.stable_id);hash_value(topology_fingerprint,texture.resource_generation);}
            }
        }
        gpu_driven_topology_reused_=gpu_driven_cached_plan_valid_&&topology_fingerprint==gpu_driven_topology_fingerprint_;
        if(gpu_driven_topology_reused_) {
            gpu_batch_plan_ptr=&gpu_driven_cached_plan_;++gpu_driven_topology_reuses_;
        } else {
            gpu_batch_inputs.reserve(objects.size());
            for(const auto& item:objects) {
                auto identity=item.data.object_identity;identity[3]=0U;
                const GpuDrivenInstance instance{item.data.model,item.data.previous_model,item.data.color,item.data.material,
                    item.data.emissive_normal,item.data.occlusion_alpha_flags,identity,
                    {item.bounds.center[0],item.bounds.center[1],item.bounds.center[2],item.bounds.radius}};
                std::uint64_t revision=14695981039346656037ULL;hash_bytes(revision,&instance,sizeof(instance));
                gpu_batch_inputs.push_back({item.history_key,item.gpu_batch_key?*item.gpu_batch_key:GpuBatchKey{},
                    !item.transparent&&item.skinning_matrices==nullptr&&item.gpu_batch_key!=nullptr,revision});
            }
            auto candidate_plan=gpu_batch_cache_.update(gpu_batch_inputs);
            if(candidate_plan.valid) {
                gpu_driven_cached_plan_=std::move(candidate_plan);gpu_driven_topology_fingerprint_=topology_fingerprint;
                gpu_driven_cached_plan_valid_=true;gpu_batch_plan_ptr=&gpu_driven_cached_plan_;++gpu_driven_topology_rebuilds_;
            } else {
                gpu_driven_cached_plan_valid_=false;
                gpu_driven_fallback_reason_="stable-batch-cache."+candidate_plan.code+": "+candidate_plan.detail;
            }
        }
    } else {
        gpu_batch_cache_.clear();gpu_driven_cached_plan_={};gpu_driven_cached_plan_valid_=false;
    }
    if(gpu_batch_plan_ptr) {
        const auto& gpu_batch_plan=*gpu_batch_plan_ptr;
        gpu_driven_stable_slots_reused_=gpu_driven_topology_reused_?gpu_batch_plan.linear_slots.size():gpu_batch_plan.statistics.reused_draws;
        gpu_driven_moved_slots_=gpu_driven_topology_reused_?0U:gpu_batch_plan.statistics.moved_draws;
        gpu_instances.reserve(gpu_batch_plan.linear_slots.size());
        for(const auto& slot:gpu_batch_plan.linear_slots) {
            const auto found=gpu_draw_lookup.find(slot.draw_id);
            if(found==gpu_draw_lookup.end())continue;
            const auto item_index=found->second;const auto& item=objects[item_index];
            auto identity=item.data.object_identity;identity[3]=slot.batch_index;
            gpu_instances.push_back({item.data.model,item.data.previous_model,item.data.color,item.data.material,
                item.data.emissive_normal,item.data.occlusion_alpha_flags,identity,
                {item.bounds.center[0],item.bounds.center[1],item.bounds.center[2],item.bounds.radius}});
            gpu_candidate_draw_ids.push_back(slot.draw_id);
            gpu_driven_items[item_index]=true;
        }
        gpu_batches.reserve(gpu_batch_plan.batches.size());gpu_commands.reserve(gpu_batch_plan.batches.size());
        gpu_render_batches.reserve(gpu_batch_plan.batches.size());gpu_reference_visible_indices.reserve(gpu_batch_plan.batches.size());
        for(std::size_t batch_index=0;batch_index<gpu_batch_plan.batches.size();++batch_index) {
            const auto& group=gpu_batch_plan.batches[batch_index];
            const auto representative_slot=group.first_linear_index;
            if(representative_slot>=gpu_batch_plan.linear_slots.size())continue;
            const auto representative_found=gpu_draw_lookup.find(gpu_batch_plan.linear_slots[representative_slot].draw_id);
            if(representative_found==gpu_draw_lookup.end())continue;
            const auto representative_index=representative_found->second;const auto& representative=objects[representative_index];
            std::size_t reference_visible{};std::vector<std::uint32_t> reference_visible_indices;
            reference_visible_indices.reserve(group.instance_count);
            for(std::uint32_t local_index=0;local_index<group.instance_count;++local_index) {
                const auto linear_index=group.first_linear_index+local_index;
                const auto found=gpu_draw_lookup.find(gpu_batch_plan.linear_slots[linear_index].draw_id);
                if(found!=gpu_draw_lookup.end()&&objects[found->second].camera_visible) {
                    ++reference_visible;reference_visible_indices.push_back(linear_index);
                }
            }
            gpu_batches.push_back({group.first_linear_index,group.instance_count,group.first_linear_index,0U});
            gpu_commands.push_back({representative.index_count,0U,representative.first_index,0,0U});
            gpu_render_batches.push_back({representative_index,group.first_linear_index,
                static_cast<std::uint32_t>(batch_index*sizeof(GpuIndexedIndirectCommand)),reference_visible});
            gpu_reference_visible_indices.push_back(std::move(reference_visible_indices));
        }
    } else {
        gpu_driven_topology_reused_=false;gpu_driven_stable_slots_reused_=0;gpu_driven_moved_slots_=0;
    }
    gpu_driven_candidates_=gpu_instances.size();gpu_driven_batches_=gpu_batches.size();gpu_driven_reference_visible_=0;
    gpu_driven_candidate_draw_ids_=std::move(gpu_candidate_draw_ids);
    gpu_driven_batch_candidate_offsets_.clear();
    gpu_driven_batch_candidate_counts_.clear();
    gpu_driven_batch_visible_offsets_.clear();
    gpu_driven_batch_candidate_offsets_.reserve(gpu_batches.size());
    gpu_driven_batch_candidate_counts_.reserve(gpu_batches.size());
    gpu_driven_batch_visible_offsets_.reserve(gpu_batches.size());
    for(const auto& batch:gpu_batches) {
        gpu_driven_batch_candidate_offsets_.push_back(batch.candidate_offset);
        gpu_driven_batch_candidate_counts_.push_back(batch.candidate_count);
        gpu_driven_batch_visible_offsets_.push_back(batch.visible_offset);
    }
    gpu_driven_batch_reference_visible_indices_=std::move(gpu_reference_visible_indices);
    for(const auto& batch:gpu_render_batches)gpu_driven_reference_visible_+=batch.reference_visible;
    gpu_driven_fallback_instances_=static_cast<std::size_t>(std::ranges::count_if(objects,[&](const DrawItem& item) {
        const auto index=static_cast<std::size_t>(&item-objects.data());
        return !gpu_driven_items[index];
    }));
    gpu_driven_upload_bytes_=0;gpu_driven_instance_upload_bytes_=0;gpu_driven_batch_upload_bytes_=0;
    gpu_driven_command_upload_bytes_=0;gpu_driven_dirty_ranges_=0;gpu_driven_dirty_instances_=0;
    bool gpu_frame_ready=!gpu_instances.empty();
    GpuVisibilityParameters gpu_visibility_parameters{};
    GpuOcclusionParameters gpu_occlusion_parameters{};
    if(!gpu_instances.empty()) {
        const auto instances_bytes=static_cast<std::uint32_t>(gpu_instances.size()*sizeof(GpuDrivenInstance));
        const auto batches_bytes=static_cast<std::uint32_t>(gpu_batches.size()*sizeof(GpuDrivenBatch));
        const auto commands_bytes=static_cast<std::uint32_t>(gpu_commands.size()*sizeof(GpuIndexedIndirectCommand));
        const auto current_instance_bytes=std::span<const std::byte>{
            reinterpret_cast<const std::byte*>(gpu_instances.data()),instances_bytes};
        const auto instance_dirty=compare_linear_dirty_ranges(gpu_driven_instance_mirror_,current_instance_bytes,
            sizeof(GpuDrivenInstance),!gpu_driven_topology_reused_);
        if(!instance_dirty.valid){gpu_frame_ready=false;gpu_driven_fallback_reason_="stable-batch-dirty."+instance_dirty.code;}
        const auto batches_changed=gpu_driven_batch_mirror_.size()!=batches_bytes||
            std::memcmp(gpu_driven_batch_mirror_.data(),gpu_batches.data(),batches_bytes)!=0;
        for(const auto& range:instance_dirty.ranges){gpu_driven_dirty_instances_+=range.count;
            gpu_driven_instance_upload_bytes_+=range.count*sizeof(GpuDrivenInstance);}
        gpu_driven_dirty_ranges_=instance_dirty.ranges.size();
        gpu_driven_batch_upload_bytes_=batches_changed?batches_bytes:0U;
        gpu_driven_command_upload_bytes_=commands_bytes;
        auto* mapped=gpu_frame_ready?static_cast<std::byte*>(SDL_MapGPUTransferBuffer(device_,gpu_driven_upload_buffer_,true)):nullptr;
        std::uint32_t upload_cursor{};
        if(gpu_frame_ready&&!mapped){gpu_frame_ready=false;gpu_driven_fallback_reason_="transfer-map-failed: "+std::string(SDL_GetError());}
        if(gpu_frame_ready) {
            struct PackedDirtyRange final { LinearDirtyRange range;std::uint32_t upload_offset{}; };
            std::vector<PackedDirtyRange> packed_dirty_ranges;packed_dirty_ranges.reserve(instance_dirty.ranges.size());
            for(const auto& range:instance_dirty.ranges) {
                const auto byte_offset=range.offset*sizeof(GpuDrivenInstance);const auto byte_count=range.count*sizeof(GpuDrivenInstance);
                packed_dirty_ranges.push_back({range,upload_cursor});
                std::memcpy(mapped+upload_cursor,current_instance_bytes.data()+byte_offset,byte_count);
                upload_cursor+=static_cast<std::uint32_t>(byte_count);
            }
            const auto batch_upload_offset=upload_cursor;
            if(batches_changed){std::memcpy(mapped+upload_cursor,gpu_batches.data(),batches_bytes);upload_cursor+=batches_bytes;}
            const auto command_upload_offset=upload_cursor;
            std::memcpy(mapped+upload_cursor,gpu_commands.data(),commands_bytes);
            upload_cursor+=commands_bytes;
            const auto occlusion_statistics_upload_offset=upload_cursor;
            const std::array<std::uint32_t,gpu_occlusion_statistic_count> zero_statistics{};
            std::memcpy(mapped+upload_cursor,zero_statistics.data(),gpu_occlusion_statistics_bytes);
            upload_cursor+=gpu_occlusion_statistics_bytes;
            SDL_UnmapGPUTransferBuffer(device_,gpu_driven_upload_buffer_);
            auto* copy=SDL_BeginGPUCopyPass(command);
            if(!copy){gpu_frame_ready=false;gpu_driven_fallback_reason_="copy-pass-failed: "+std::string(SDL_GetError());}
            if(copy) {
                SDL_GPUTransferBufferLocation source{gpu_driven_upload_buffer_,0};SDL_GPUBufferRegion destination{};
                const bool full_instance_upload=packed_dirty_ranges.size()==1U&&packed_dirty_ranges[0].range.offset==0U&&
                    packed_dirty_ranges[0].range.count==gpu_instances.size();
                for(const auto& packed:packed_dirty_ranges) {
                    source.offset=packed.upload_offset;
                    destination={gpu_driven_instance_buffer_,static_cast<Uint32>(packed.range.offset*sizeof(GpuDrivenInstance)),
                        static_cast<Uint32>(packed.range.count*sizeof(GpuDrivenInstance))};
                    SDL_UploadToGPUBuffer(copy,&source,&destination,full_instance_upload);
                }
                if(batches_changed){source.offset=batch_upload_offset;destination={gpu_driven_batch_buffer_,0,batches_bytes};
                    SDL_UploadToGPUBuffer(copy,&source,&destination,true);}
                source.offset=command_upload_offset;destination={gpu_driven_indirect_buffer_,0,commands_bytes};
                SDL_UploadToGPUBuffer(copy,&source,&destination,true);
                if(gpu_occlusion_statistics_buffer_) {
                    source.offset=occlusion_statistics_upload_offset;
                    destination={gpu_occlusion_statistics_buffer_,0,gpu_occlusion_statistics_bytes};
                    // This copy pass is ordered before visibility compute in the
                    // same command buffer, so the counter allocation is safe to
                    // reuse. Cycling it allowed Vulkan to keep an older backing
                    // allocation and accumulate evidence across frames.
                    SDL_UploadToGPUBuffer(copy,&source,&destination,false);
                }
                SDL_EndGPUCopyPass(copy);gpu_driven_upload_bytes_=gpu_driven_instance_upload_bytes_+
                    gpu_driven_batch_upload_bytes_+gpu_driven_command_upload_bytes_+
                    (gpu_occlusion_statistics_buffer_?gpu_occlusion_statistics_bytes:0U);
                gpu_driven_stable_upload_bytes_total_+=gpu_driven_instance_upload_bytes_+gpu_driven_batch_upload_bytes_;
                if(!packed_dirty_ranges.empty()){gpu_driven_instance_mirror_.assign(current_instance_bytes.begin(),current_instance_bytes.end());
                    ++gpu_driven_instance_uploads_;}
                if(batches_changed)gpu_driven_batch_mirror_.assign(reinterpret_cast<const std::byte*>(gpu_batches.data()),
                    reinterpret_cast<const std::byte*>(gpu_batches.data())+batches_bytes);
            }
        }
        if(gpu_frame_ready) {
            for(std::size_t plane=0;plane<camera_frustum.planes.size();++plane) {
                const auto& source_plane=camera_frustum.planes[plane];
                gpu_visibility_parameters.frustum_planes[plane]={source_plane.normal[0],source_plane.normal[1],source_plane.normal[2],source_plane.distance};
            }
            gpu_visibility_parameters.candidate_count=static_cast<std::uint32_t>(gpu_instances.size());
            gpu_visibility_parameters.batch_count=static_cast<std::uint32_t>(gpu_batches.size());
            gpu_occlusion_parameters.frustum_planes=gpu_visibility_parameters.frustum_planes;
            gpu_occlusion_parameters.view_projection=Mat4{previous_view_projection_};
            gpu_occlusion_parameters.viewport={static_cast<float>(render_width_),static_cast<float>(render_height_),
                1.0F/static_cast<float>(render_width_),1.0F/static_cast<float>(render_height_)};
            gpu_occlusion_parameters.depth_parameters={camera_near,camera_far,0.0F,0.05F};
            gpu_occlusion_parameters.occlusion_parameters={gpu_occlusion_history_valid_?1.0F:0.0F,
                static_cast<float>(depth_pyramid_mip_count_?depth_pyramid_mip_count_-1U:0U),
                static_cast<float>(depth_pyramid_mip_count_),1.0F};
            gpu_occlusion_parameters.dispatch_parameters={gpu_visibility_parameters.candidate_count,
                gpu_visibility_parameters.batch_count,1U,0U};
        }
        if(!gpu_frame_ready) {
            std::fill(gpu_driven_items.begin(),gpu_driven_items.end(),false);gpu_render_batches.clear();
            gpu_driven_fallback_instances_=objects.size();gpu_driven_batches_=0;gpu_driven_reference_visible_=0;
        }
    }
    shadow_casters_=shadow_entities;
    shadow_primitives_=std::accumulate(objects.begin(),objects.end(),std::size_t{},
        [](const std::size_t total,const DrawItem& item) {
            return total+(item.casts_shadows?static_cast<std::size_t>(item.index_count/3U):0U);
        })+2U*std::ranges::count_if(sprites,[](const SpriteDrawItem& sprite){return sprite.casts_shadows;});
    skinned_draw_items_=std::ranges::count_if(objects,[](const DrawItem& item){return item.skinning_matrices!=nullptr;});
    shadow_caster_draws_=0; shadow_instances_submitted_=0; shadow_draw_calls_saved_=0;
    shadow_draws_per_cascade_.fill(0); shadow_instances_per_cascade_.fill(0);
    shadow_draw_calls_saved_per_cascade_.fill(0); shadow_culled_per_cascade_.fill(0);
    directional_shadow_cascades_rendered_=0;directional_shadow_cascades_cached_=0;
    directional_shadow_avoided_instances_=0;directional_shadow_avoided_draws_=0;
    local_shadow_faces_rendered_=0;local_shadow_faces_cached_=0;local_shadow_avoided_instances_=0;local_shadow_avoided_draws_=0;
    local_shadow_instances_submitted_=0;local_shadow_draw_calls_=0;
    local_shadow_draw_calls_saved_=0;local_shadow_culled_draws_=0;
    const LightingData lighting{{light_direction.x,light_direction.y,light_direction.z,light_intensity},
        {ambient_intensity,0.0035F,0.0008F,0},{light_color[0],light_color[1],light_color[2],1},
        {camera_position.x,camera_position.y,camera_position.z,1},
        {camera_forward.x,camera_forward.y,camera_forward.z,0},shadow_splits_,cascade_view_projections,
        {static_cast<float>(shadow_size),static_cast<float>(shadow_cascade_count),0.10F,shadow_config.maximum_distance},
        {static_cast<float>(clustered_lighting_config_.tiles_x),static_cast<float>(clustered_lighting_config_.tiles_y),
            static_cast<float>(clustered_lighting_config_.depth_slices),local_lights_submitted_>0?1.0F:0.0F},
        {camera_near,camera_far,1.0F/std::log(camera_far/camera_near),static_cast<float>(local_lights_submitted_)},
        {static_cast<float>(render_width_),static_cast<float>(render_height_),1.0F/static_cast<float>(render_width_),1.0F/static_cast<float>(render_height_)},
        local_shadow_matrices,{static_cast<float>(local_shadow_resolution_),static_cast<float>(next_local_shadow_layer),0.0025F,0.0006F}};
    const auto& atmosphere=sky_atmosphere_;
    SkyAtmosphereGpuData atmosphere_gpu{
        inverse_or_identity(unjittered_view_projection),
        {camera_position.x,camera_position.y,camera_position.z,1.0F},
        {camera_forward.x,camera_forward.y,camera_forward.z,0.0F},
        {0.0F,-atmosphere.planet_radius_m,0.0F,0.0F},
        {atmosphere.planet_radius_m,atmosphere.planet_radius_m+atmosphere.atmosphere_height_m,
            atmosphere.atmosphere_height_m*2.0F,0.0F},
        {atmosphere.sun_direction[0],atmosphere.sun_direction[1],atmosphere.sun_direction[2],20.0F},
        {atmosphere.sun_irradiance[0],atmosphere.sun_irradiance[1],atmosphere.sun_irradiance[2],0.0F},
        {atmosphere.rayleigh_scattering_per_m[0],atmosphere.rayleigh_scattering_per_m[1],
            atmosphere.rayleigh_scattering_per_m[2],atmosphere.rayleigh_scale_height_m},
        {atmosphere.mie_scattering_per_m[0],atmosphere.mie_scattering_per_m[1],
            atmosphere.mie_scattering_per_m[2],atmosphere.mie_scale_height_m},
        {atmosphere.mie_absorption_per_m[0],atmosphere.mie_absorption_per_m[1],
            atmosphere.mie_absorption_per_m[2],atmosphere.mie_phase_g},
        {atmosphere.ground_albedo[0],atmosphere.ground_albedo[1],atmosphere.ground_albedo[2],1.0F},
        {0.08F,0.18F,0.42F,1.0F},
        {8U,4U,1U,static_cast<std::uint32_t>(frame_index_)},
        {static_cast<float>(render_width_),static_cast<float>(render_height_),
            static_cast<float>(frame_index_)/60.0F,0.0F}};
    const auto draw_sprite_batches=[&](SDL_GPURenderPass* pass,const bool transparent) {
        std::size_t cursor=0;
        while(cursor<sprites.size()) {
            while(cursor<sprites.size()&&sprites[cursor].transparent!=transparent)++cursor;
            if(cursor>=sprites.size())break;
            auto* texture=sprites[cursor].texture;auto* normal_texture=sprites[cursor].normal_texture;
            auto* emissive_mask_texture=sprites[cursor].emissive_mask_texture;auto* depth_texture=sprites[cursor].depth_texture;
            auto* base_sampler=sprites[cursor].base_sampler;auto* normal_sampler=sprites[cursor].normal_sampler;
            auto* emissive_mask_sampler=sprites[cursor].emissive_mask_sampler;auto* depth_sampler=sprites[cursor].depth_sampler;
            const auto first=cursor;
            while(cursor<sprites.size()) {
                const auto& candidate=sprites[cursor];
                if(candidate.transparent!=transparent)break;
                if(candidate.texture!=texture||candidate.normal_texture!=normal_texture||
                   candidate.emissive_mask_texture!=emissive_mask_texture||candidate.depth_texture!=depth_texture||
                   candidate.base_sampler!=base_sampler||candidate.normal_sampler!=normal_sampler||
                   candidate.emissive_mask_sampler!=emissive_mask_sampler||candidate.depth_sampler!=depth_sampler)break;
                ++cursor;
            }
            const auto count=cursor-first;SpriteDrawData batch{view_projection,previous_view_projection,
                {static_cast<std::uint32_t>(first),static_cast<std::uint32_t>(count),0U,0U}};
            SDL_BindGPUGraphicsPipeline(pass,transparent?sprite_alpha_pipeline_:sprite_cutout_pipeline_);
            const std::array<SDL_GPUBuffer*,2> sprite_buffers{sprite_instance_buffer_,sprite_draw_index_buffer_};
            SDL_BindGPUVertexStorageBuffers(pass,0,sprite_buffers.data(),static_cast<Uint32>(sprite_buffers.size()));
            const std::array<SDL_GPUTextureSamplerBinding,6> bindings{{{texture,base_sampler},{normal_texture,normal_sampler},
                {emissive_mask_texture,emissive_mask_sampler},{depth_texture,depth_sampler},
                {shadow_texture_,shadow_sampler_},{local_shadow_texture_,shadow_sampler_}}};
            SDL_BindGPUFragmentSamplers(pass,0,bindings.data(),static_cast<Uint32>(bindings.size()));
            SDL_PushGPUVertexUniformData(command,0,&batch,sizeof(batch));
            SDL_DrawGPUPrimitives(pass,6,static_cast<Uint32>(count),0,0);
            ++sprite_draws_submitted_;sprite_instances_submitted_+=count;sprite_draws_saved_+=count-1U;
        }
    };
    const auto draw_sprite_shadow_batches=[&](SDL_GPURenderPass* pass,const Mat4& shadow_view_projection,
        const std::vector<bool>& visible,const bool local_shadow,const std::uint32_t cascade_index) {
        std::size_t cursor=0;
        while(cursor<sprites.size()) {
            while(cursor<sprites.size()&&(!visible[cursor]||!sprites[cursor].casts_shadows))++cursor;
            if(cursor>=sprites.size())break;
            auto* texture=sprites[cursor].texture;auto* sampler=sprites[cursor].base_sampler;
            const auto first=cursor++;
            while(cursor<sprites.size()&&visible[cursor]&&sprites[cursor].casts_shadows&&
                sprites[cursor].texture==texture&&sprites[cursor].base_sampler==sampler)++cursor;
            const auto count=cursor-first;
            const SpriteDrawData batch{shadow_view_projection,shadow_view_projection,
                {static_cast<std::uint32_t>(first),static_cast<std::uint32_t>(count),0U,0U}};
            SDL_BindGPUGraphicsPipeline(pass,sprite_shadow_pipeline_);
            const std::array<SDL_GPUBuffer*,2> sprite_buffers{sprite_instance_buffer_,sprite_draw_index_buffer_};
            SDL_BindGPUVertexStorageBuffers(pass,0,sprite_buffers.data(),static_cast<Uint32>(sprite_buffers.size()));
            const SDL_GPUTextureSamplerBinding base_color_binding{texture,sampler};
            SDL_BindGPUFragmentSamplers(pass,0,&base_color_binding,1);
            SDL_PushGPUVertexUniformData(command,0,&batch,sizeof(batch));
            SDL_DrawGPUPrimitives(pass,6,static_cast<Uint32>(count),0,0);
            if(local_shadow) {
                local_shadow_instances_submitted_+=count;local_shadow_draw_calls_saved_+=count-1U;++local_shadow_draw_calls_;
            } else {
                shadow_instances_per_cascade_[cascade_index]+=count;
                shadow_draw_calls_saved_per_cascade_[cascade_index]+=count-1U;
                ++shadow_draws_per_cascade_[cascade_index];++shadow_caster_draws_;
                shadow_instances_submitted_+=count;shadow_draw_calls_saved_+=count-1U;
            }
        }
    };
    bloom_record_microseconds_=0.0;ao_denoise_record_microseconds_=0.0;
    ssr_trace_record_microseconds_=ssr_temporal_record_microseconds_=ssr_composite_record_microseconds_=0.0;
    ssgi_gather_record_microseconds_=ssgi_spatial_record_microseconds_=0.0;
    ssgi_temporal_record_microseconds_=ssgi_composite_record_microseconds_=0.0;
    const auto temporal_begin=temporal_history_authority_.begin(temporal_history_plan);
    if(!temporal_begin.success) {
        last_error_=temporal_begin.code+": "+temporal_begin.detail;
        return;
    }
    const auto ssgi_begin=temporal_history_authority_.begin(ssgi_history_plan);
    if(!ssgi_begin.success) {
        (void)temporal_history_authority_.commit({TemporalHistoryConsumer::taa,
            temporal_begin.transaction_id,temporal_begin.base_revision,false,
            temporal_history_reset_mask(TemporalHistoryResetReason::output_invalid)});
        last_error_=ssgi_begin.code+": "+ssgi_begin.detail;
        return;
    }
    const auto ssr_begin=temporal_history_authority_.begin(ssr_history_plan);
    if(!ssr_begin.success) {
        (void)temporal_history_authority_.commit({TemporalHistoryConsumer::taa,
            temporal_begin.transaction_id,temporal_begin.base_revision,false,
            temporal_history_reset_mask(TemporalHistoryResetReason::output_invalid)});
        (void)temporal_history_authority_.commit({TemporalHistoryConsumer::ssgi,
            ssgi_begin.transaction_id,ssgi_begin.base_revision,false,
            temporal_history_reset_mask(TemporalHistoryResetReason::output_invalid)});
        last_error_=ssr_begin.code+": "+ssr_begin.detail;
        return;
    }
    bool temporal_history_committed=false;
    const auto commit_temporal_history=[&](const bool produced_history,
        const TemporalHistoryResetMask output_reset_reasons=0U) {
        if(temporal_history_committed)return true;
        const auto commit=temporal_history_authority_.commit({TemporalHistoryConsumer::taa,
            temporal_begin.transaction_id,temporal_begin.base_revision,produced_history,output_reset_reasons});
        if(!commit.success) {
            last_error_=commit.code+": "+commit.detail;
            return false;
        }
        temporal_history_committed=true;
        temporal_history_valid_=commit.current_valid;
        taa_history_resets_=temporal_history_authority_.state(TemporalHistoryConsumer::taa).reset_count;
        return true;
    };
    bool ssgi_history_committed=false;
    const auto commit_ssgi_history=[&](const bool produced_history,
        const TemporalHistoryResetMask output_reset_reasons=0U) {
        if(ssgi_history_committed)return true;
        const auto commit=temporal_history_authority_.commit({TemporalHistoryConsumer::ssgi,
            ssgi_begin.transaction_id,ssgi_begin.base_revision,produced_history,output_reset_reasons});
        if(!commit.success) {last_error_=commit.code+": "+commit.detail;return false;}
        ssgi_history_committed=true;ssgi_history_valid_=commit.current_valid;return true;
    };
    bool ssr_history_committed=false;
    const auto commit_ssr_history=[&](const bool produced_history,
        const TemporalHistoryResetMask output_reset_reasons=0U) {
        if(ssr_history_committed)return true;
        const auto commit=temporal_history_authority_.commit({TemporalHistoryConsumer::ssr,
            ssr_begin.transaction_id,ssr_begin.base_revision,produced_history,output_reset_reasons});
        if(!commit.success) {last_error_=commit.code+": "+commit.detail;return false;}
        ssr_history_committed=true;ssr_history_valid_=commit.current_valid;return true;
    };
    (void)gpu_pass_timestamps_.begin_frame(command,frame_index_,render_graph_.execution_order);
    for (const auto& pass_id : render_graph_.execution_order) {
    gpu_pass_timestamps_.begin_pass(command,pass_id);
    SDL_PushGPUDebugGroup(command,pass_id.c_str());
    const auto record_start=std::chrono::steady_clock::now();
    if (pass_id == "render.pass.shadow-depth") {
    for (std::uint32_t cascade_index=0;cascade_index<shadow_cascade_count;++cascade_index) {
        std::vector<bool> cascade_visible(objects.size(),false);
        std::vector<bool> cascade_sprite_visible(sprites.size(),false);
        std::size_t visible_casters{};std::uint64_t fingerprint=1469598103934665603ULL;
        hash_value(fingerprint,cascade_index);hash_value(fingerprint,light_casts_shadows);
        hash_bytes(fingerprint,cascade_view_projections[cascade_index].value.data(),sizeof(cascade_view_projections[cascade_index].value));
        for (std::size_t object_index=0;object_index<objects.size();++object_index) {
            const auto& object=objects[object_index];
            if (!light_casts_shadows || !object.casts_shadows) continue;
            cascade_visible[object_index]=sphere_intersects_frustum(cascade_frustums[cascade_index],object.bounds);
            if (!cascade_visible[object_index]) {++shadow_culled_per_cascade_[cascade_index];continue;}
            ++visible_casters;hash_string(fingerprint,object.history_key);
            hash_bytes(fingerprint,object.data.model.value.data(),sizeof(object.data.model.value));
            hash_bytes(fingerprint,object.data.color.data(),sizeof(object.data.color));
            hash_bytes(fingerprint,object.data.occlusion_alpha_flags.data(),sizeof(object.data.occlusion_alpha_flags));
            hash_value(fingerprint,object.index_count);hash_value(fingerprint,object.first_index);hash_value(fingerprint,object.double_sided);
            const std::array<std::uintptr_t,3> resource_identity{
                reinterpret_cast<std::uintptr_t>(object.vertex_buffer),reinterpret_cast<std::uintptr_t>(object.index_buffer),
                reinterpret_cast<std::uintptr_t>(object.base_color_texture)};
            hash_bytes(fingerprint,resource_identity.data(),sizeof(resource_identity));
            if(object.skinning_matrices)
                for(const auto& joint:*object.skinning_matrices)hash_bytes(fingerprint,joint.data(),sizeof(joint));
        }
        for(std::size_t sprite_index=0;sprite_index<sprites.size();++sprite_index) {
            const auto& sprite=sprites[sprite_index];
            if(!light_casts_shadows||!sprite.casts_shadows)continue;
            cascade_sprite_visible[sprite_index]=sphere_intersects_frustum(cascade_frustums[cascade_index],sprite.bounds);
            if(!cascade_sprite_visible[sprite_index]){++shadow_culled_per_cascade_[cascade_index];continue;}
            ++visible_casters;hash_string(fingerprint,sprite.history_key);
            hash_bytes(fingerprint,sprite.model.value.data(),sizeof(sprite.model.value));
            hash_bytes(fingerprint,sprite.uv_rect.data(),sizeof(sprite.uv_rect));
            hash_bytes(fingerprint,sprite.local_rect.data(),sizeof(sprite.local_rect));
            hash_bytes(fingerprint,sprite.identity_flags.data(),sizeof(sprite.identity_flags));
            const std::array<std::uintptr_t,2> resource_identity{reinterpret_cast<std::uintptr_t>(sprite.texture),
                reinterpret_cast<std::uintptr_t>(sprite.base_sampler)};
            hash_bytes(fingerprint,resource_identity.data(),sizeof(resource_identity));
        }
        const bool cache_hit=directional_shadow_cascade_cache_valid_[cascade_index]&&
            directional_shadow_cascade_fingerprints_[cascade_index]==fingerprint;
        if(cache_hit) {
            ++directional_shadow_cascades_cached_;++directional_shadow_cache_hits_;
            directional_shadow_avoided_instances_+=visible_casters;directional_shadow_avoided_draws_+=visible_casters;
            continue;
        }
        ++directional_shadow_cache_misses_;++directional_shadow_cascades_rendered_;
        directional_shadow_cascade_fingerprints_[cascade_index]=fingerprint;
        directional_shadow_cascade_cache_valid_[cascade_index]=true;
        SDL_GPUDepthStencilTargetInfo shadow_target{};
        shadow_target.texture=shadow_texture_; shadow_target.clear_depth=1.0F; shadow_target.layer=static_cast<Uint8>(cascade_index);
        shadow_target.load_op=SDL_GPU_LOADOP_CLEAR; shadow_target.store_op=SDL_GPU_STOREOP_STORE;
        shadow_target.stencil_load_op=SDL_GPU_LOADOP_DONT_CARE; shadow_target.stencil_store_op=SDL_GPU_STOREOP_DONT_CARE;
        SDL_GPURenderPass* pass=SDL_BeginGPURenderPass(command,nullptr,0,&shadow_target);
        if(!pass){last_error_=SDL_GetError();(void)commit_temporal_history(false,
            temporal_history_reset_mask(TemporalHistoryResetReason::output_invalid));(void)commit_ssgi_history(false,
            temporal_history_reset_mask(TemporalHistoryResetReason::output_invalid));(void)commit_ssr_history(false,
            temporal_history_reset_mask(TemporalHistoryResetReason::output_invalid));SDL_PopGPUDebugGroup(command);gpu_pass_timestamps_.end_pass(command,pass_id);gpu_pass_timestamps_.end_frame(command);return;}
        std::vector<bool> submitted(objects.size(),false);
        for (std::size_t object_index=0;object_index<objects.size();++object_index) {
            if (submitted[object_index] || !cascade_visible[object_index]) continue;
            const auto& object=objects[object_index];
            std::vector<std::size_t> batch{object_index}; submitted[object_index]=true;
            if (object.skinning_matrices==nullptr) {
                for (std::size_t candidate=object_index+1;candidate<objects.size() && batch.size()<maximum_instances_per_draw;++candidate) {
                    if (!submitted[candidate] && cascade_visible[candidate] && compatible_instance_state(object,objects[candidate])) {
                        submitted[candidate]=true; batch.push_back(candidate);
                    }
                }
            }
            ShadowInstancingData instancing{};
            if (batch.size()>1U) {
                instancing.metadata[0]=static_cast<std::uint32_t>(batch.size());
                for (std::size_t instance=0;instance<batch.size();++instance) {
                    const auto& source=objects[batch[instance]].data;
                    instancing.models[instance]=source.model;
                    instancing.colors[instance]=source.color;
                    instancing.occlusion_alpha_flags[instance]=source.occlusion_alpha_flags;
                }
            }
            shadow_instances_per_cascade_[cascade_index]+=batch.size();
            shadow_draw_calls_saved_per_cascade_[cascade_index]+=batch.size()-1U;
            ++shadow_draws_per_cascade_[cascade_index]; ++shadow_caster_draws_;
            shadow_instances_submitted_+=batch.size(); shadow_draw_calls_saved_+=batch.size()-1U;
            SDL_BindGPUGraphicsPipeline(pass,object.double_sided?shadow_double_sided_pipeline_:shadow_pipeline_);
            const SDL_GPUBufferBinding vertex_binding{object.vertex_buffer,0};
            const SDL_GPUBufferBinding index_binding{object.index_buffer,0};
            SDL_BindGPUVertexBuffers(pass,0,&vertex_binding,1); SDL_BindGPUIndexBuffer(pass,&index_binding,SDL_GPU_INDEXELEMENTSIZE_32BIT);
            const SDL_GPUTextureSamplerBinding base_color_binding{object.base_color_texture,material_sampler_};
            SDL_BindGPUFragmentSamplers(pass,0,&base_color_binding,1);
            auto shadow_data=object.data; shadow_data.light_view_projection=cascade_view_projections[cascade_index];
            SDL_PushGPUVertexUniformData(command,0,&shadow_data,sizeof(ObjectData));
            const auto object_skinning=object.skinning_matrices?skinning_data(*object.skinning_matrices):SkinningData{};
            SDL_PushGPUVertexUniformData(command,1,&object_skinning,skinning_palette_bytes);
            SDL_PushGPUVertexUniformData(command,2,&instancing,sizeof(instancing));
            SDL_DrawGPUIndexedPrimitives(pass,object.index_count,static_cast<Uint32>(batch.size()),object.first_index,0,0);
        }
        draw_sprite_shadow_batches(pass,cascade_view_projections[cascade_index],cascade_sprite_visible,false,cascade_index);
        SDL_EndGPURenderPass(pass);
    }
    for(const auto& face:local_shadow_faces) {
        std::vector<bool> face_visible(objects.size(),false);
        std::vector<bool> face_sprite_visible(sprites.size(),false);
        std::size_t visible_casters{};
        std::uint64_t fingerprint=1469598103934665603ULL;
        hash_string(fingerprint,face.cache_key);hash_bytes(fingerprint,face.view_projection.value.data(),sizeof(face.view_projection.value));
        for(std::size_t object_index=0;object_index<objects.size();++object_index) {
            const auto& object=objects[object_index];if(!object.casts_shadows)continue;
            face_visible[object_index]=sphere_intersects_frustum(face.frustum,object.bounds);
            if(!face_visible[object_index]){++local_shadow_culled_draws_;continue;}
            ++visible_casters;hash_string(fingerprint,object.history_key);
            hash_bytes(fingerprint,object.data.model.value.data(),sizeof(object.data.model.value));
            hash_bytes(fingerprint,object.data.color.data(),sizeof(object.data.color));
            hash_bytes(fingerprint,object.data.occlusion_alpha_flags.data(),sizeof(object.data.occlusion_alpha_flags));
            hash_value(fingerprint,object.index_count);hash_value(fingerprint,object.first_index);hash_value(fingerprint,object.double_sided);
            const std::array<std::uintptr_t,3> resource_identity{
                reinterpret_cast<std::uintptr_t>(object.vertex_buffer),reinterpret_cast<std::uintptr_t>(object.index_buffer),
                reinterpret_cast<std::uintptr_t>(object.base_color_texture)};
            hash_bytes(fingerprint,resource_identity.data(),sizeof(resource_identity));
            if(object.skinning_matrices)
                for(const auto& joint:*object.skinning_matrices)hash_bytes(fingerprint,joint.data(),sizeof(joint));
        }
        for(std::size_t sprite_index=0;sprite_index<sprites.size();++sprite_index) {
            const auto& sprite=sprites[sprite_index];if(!sprite.casts_shadows)continue;
            face_sprite_visible[sprite_index]=sphere_intersects_frustum(face.frustum,sprite.bounds);
            if(!face_sprite_visible[sprite_index]){++local_shadow_culled_draws_;continue;}
            ++visible_casters;hash_string(fingerprint,sprite.history_key);
            hash_bytes(fingerprint,sprite.model.value.data(),sizeof(sprite.model.value));
            hash_bytes(fingerprint,sprite.uv_rect.data(),sizeof(sprite.uv_rect));
            hash_bytes(fingerprint,sprite.local_rect.data(),sizeof(sprite.local_rect));
            hash_bytes(fingerprint,sprite.identity_flags.data(),sizeof(sprite.identity_flags));
            const std::array<std::uintptr_t,2> resource_identity{reinterpret_cast<std::uintptr_t>(sprite.texture),
                reinterpret_cast<std::uintptr_t>(sprite.base_sampler)};
            hash_bytes(fingerprint,resource_identity.data(),sizeof(resource_identity));
        }
        const bool cache_hit=face.layer<local_shadow_face_cache_valid_.size()&&local_shadow_face_cache_valid_[face.layer]&&
            local_shadow_face_fingerprints_[face.layer]==fingerprint;
        if(cache_hit) {
            ++local_shadow_faces_cached_;++local_shadow_cache_hits_;
            local_shadow_avoided_instances_+=visible_casters;local_shadow_avoided_draws_+=visible_casters;
            continue;
        }
        ++local_shadow_cache_misses_;local_shadow_face_fingerprints_[face.layer]=fingerprint;local_shadow_face_cache_valid_[face.layer]=true;
        SDL_GPUDepthStencilTargetInfo shadow_target{};
        shadow_target.texture=local_shadow_texture_;shadow_target.clear_depth=1.0F;shadow_target.layer=static_cast<Uint8>(face.layer);
        shadow_target.load_op=SDL_GPU_LOADOP_CLEAR;shadow_target.store_op=SDL_GPU_STOREOP_STORE;
        shadow_target.stencil_load_op=SDL_GPU_LOADOP_DONT_CARE;shadow_target.stencil_store_op=SDL_GPU_STOREOP_DONT_CARE;
        SDL_GPURenderPass* pass=SDL_BeginGPURenderPass(command,nullptr,0,&shadow_target);
        if(!pass){last_error_=SDL_GetError();(void)commit_temporal_history(false,
            temporal_history_reset_mask(TemporalHistoryResetReason::output_invalid));(void)commit_ssgi_history(false,
            temporal_history_reset_mask(TemporalHistoryResetReason::output_invalid));(void)commit_ssr_history(false,
            temporal_history_reset_mask(TemporalHistoryResetReason::output_invalid));SDL_PopGPUDebugGroup(command);gpu_pass_timestamps_.end_pass(command,pass_id);gpu_pass_timestamps_.end_frame(command);return;}
        ++local_shadow_faces_rendered_;
        std::vector<bool> submitted(objects.size(),false);
        for(std::size_t object_index=0;object_index<objects.size();++object_index) {
            if(submitted[object_index]||!face_visible[object_index])continue;
            const auto& object=objects[object_index];std::vector<std::size_t> batch{object_index};submitted[object_index]=true;
            if(object.skinning_matrices==nullptr) {
                for(std::size_t candidate=object_index+1;candidate<objects.size()&&batch.size()<maximum_instances_per_draw;++candidate) {
                    if(!submitted[candidate]&&face_visible[candidate]&&compatible_instance_state(object,objects[candidate])) {
                        submitted[candidate]=true;batch.push_back(candidate);
                    }
                }
            }
            ShadowInstancingData instancing{};
            if(batch.size()>1U) {
                instancing.metadata[0]=static_cast<std::uint32_t>(batch.size());
                for(std::size_t instance=0;instance<batch.size();++instance) {
                    const auto& source=objects[batch[instance]].data;instancing.models[instance]=source.model;
                    instancing.colors[instance]=source.color;instancing.occlusion_alpha_flags[instance]=source.occlusion_alpha_flags;
                }
            }
            local_shadow_instances_submitted_+=batch.size();local_shadow_draw_calls_saved_+=batch.size()-1U;++local_shadow_draw_calls_;
            SDL_BindGPUGraphicsPipeline(pass,object.double_sided?shadow_double_sided_pipeline_:shadow_pipeline_);
            const SDL_GPUBufferBinding vertex_binding{object.vertex_buffer,0};const SDL_GPUBufferBinding index_binding{object.index_buffer,0};
            SDL_BindGPUVertexBuffers(pass,0,&vertex_binding,1);SDL_BindGPUIndexBuffer(pass,&index_binding,SDL_GPU_INDEXELEMENTSIZE_32BIT);
            const SDL_GPUTextureSamplerBinding base_color_binding{object.base_color_texture,material_sampler_};
            SDL_BindGPUFragmentSamplers(pass,0,&base_color_binding,1);
            auto shadow_data=object.data;shadow_data.light_view_projection=face.view_projection;
            SDL_PushGPUVertexUniformData(command,0,&shadow_data,sizeof(ObjectData));
            const auto object_skinning=object.skinning_matrices?skinning_data(*object.skinning_matrices):SkinningData{};
            SDL_PushGPUVertexUniformData(command,1,&object_skinning,skinning_palette_bytes);
            SDL_PushGPUVertexUniformData(command,2,&instancing,sizeof(instancing));
            SDL_DrawGPUIndexedPrimitives(pass,object.index_count,static_cast<Uint32>(batch.size()),object.first_index,0,0);
        }
        draw_sprite_shadow_batches(pass,face.view_projection,face_sprite_visible,true,0U);
        SDL_EndGPURenderPass(pass);
    }
    } else if(pass_id=="render.pass.gpu-visibility") {
    if(gpu_frame_ready) {
        const bool use_occlusion=gpu_occlusion_history_valid_&&gpu_occlusion_pipeline_&&
            gpu_occlusion_statistics_buffer_&&depth_pyramid_texture_;
        gpu_occlusion_used_this_frame_=use_occlusion;
        const std::array<SDL_GPUStorageBufferReadWriteBinding,3> outputs{{
            {gpu_driven_visible_index_buffer_,true,0,0,0},{gpu_driven_indirect_buffer_,false,0,0,0},
            {gpu_occlusion_statistics_buffer_,true,0,0,0}}};
        auto* compute=SDL_BeginGPUComputePass(command,nullptr,0,outputs.data(),use_occlusion?3U:2U);
        if(!compute){gpu_frame_ready=false;gpu_driven_fallback_reason_="compute-pass-failed: "+std::string(SDL_GetError());}
        if(compute) {
            if(use_occlusion) {
                SDL_PushGPUComputeUniformData(command,0,&gpu_occlusion_parameters,sizeof(gpu_occlusion_parameters));
                SDL_BindGPUComputePipeline(compute,gpu_occlusion_pipeline_);
                const SDL_GPUTextureSamplerBinding hiz_source{depth_pyramid_texture_,sprite_nearest_sampler_};
                SDL_BindGPUComputeSamplers(compute,0,&hiz_source,1U);
            } else {
                SDL_PushGPUComputeUniformData(command,0,&gpu_visibility_parameters,sizeof(gpu_visibility_parameters));
                SDL_BindGPUComputePipeline(compute,gpu_visibility_pipeline_);
            }
            const std::array<SDL_GPUBuffer*,2> inputs{gpu_driven_instance_buffer_,gpu_driven_batch_buffer_};
            SDL_BindGPUComputeStorageBuffers(compute,0,inputs.data(),static_cast<Uint32>(inputs.size()));
            SDL_DispatchGPUCompute(compute,(gpu_visibility_parameters.candidate_count+63U)/64U,1,1);
            SDL_EndGPUComputePass(compute);++gpu_driven_dispatches_;
        }
        if(!gpu_frame_ready) {
            std::fill(gpu_driven_items.begin(),gpu_driven_items.end(),false);gpu_render_batches.clear();
            gpu_driven_fallback_instances_=objects.size();gpu_driven_batches_=0;gpu_driven_reference_visible_=0;
        }
    }
    } else if (pass_id == "render.pass.sky-atmosphere") {
    const bool atmosphere_active=atmosphere.enabled&&atmosphere.quality!=SkyAtmosphereQuality::off;
    const bool analytic_quality=atmosphere.quality==SkyAtmosphereQuality::low;
    const bool lut_ready=atmosphere_active&&!analytic_quality&&dispatch_sky_atmosphere_luts(command,
        {camera_position.x,camera_position.y,camera_position.z},
        {camera_right.x,camera_right.y,camera_right.z},{camera_up.x,camera_up.y,camera_up.z},
        {camera_forward.x,camera_forward.y,camera_forward.z},std::tan(camera_fov*0.5F),
        static_cast<float>(render_width_)/static_cast<float>(render_height_),camera_near,camera_far,
        camera_orthographic,camera_orthographic_height);
    if(lut_ready)atmosphere_gpu.quality[2]|=2U;
    SDL_GPUColorTargetInfo color_target{};color_target.texture=hdr_texture_;
    color_target.clear_color={0.018F,0.028F,0.052F,1.0F};
    color_target.load_op=SDL_GPU_LOADOP_CLEAR;color_target.store_op=SDL_GPU_STOREOP_STORE;
    SDL_GPURenderPass* pass=SDL_BeginGPURenderPass(command,&color_target,1,nullptr);
    if(pass&&atmosphere_active) {
        const bool use_analytic=analytic_quality||!lut_ready;
        sky_last_path_=use_analytic?(analytic_quality?"analytic-low":"analytic-fallback"):
            "transmittance/multi-scattering/sky-view/camera-volume";
        SDL_BindGPUGraphicsPipeline(pass,use_analytic?sky_atmosphere_analytic_pipeline_:sky_atmosphere_pipeline_);
        if(!use_analytic) {
            const SDL_GPUTextureSamplerBinding lut_binding{sky_view_lut_,sky_lut_sampler_};
            SDL_BindGPUFragmentSamplers(pass,0,&lut_binding,1);
        }
        SDL_PushGPUFragmentUniformData(command,0,&atmosphere_gpu,sizeof(atmosphere_gpu));
        SDL_DrawGPUPrimitives(pass,3,1,0,0);
    } else if(!atmosphere_active)sky_last_path_="disabled-clear";
    if(pass)SDL_EndGPURenderPass(pass);
    } else if (pass_id == "render.pass.opaque-lit") {
    std::array<SDL_GPUColorTargetInfo,8> color_targets{};
    color_targets[0].texture=hdr_texture_; color_targets[0].clear_color={0.018F,0.028F,0.052F,1};
    color_targets[1].texture=object_id_texture_; color_targets[1].clear_color={0,0,0,0};
    color_targets[2].texture=normal_texture_; color_targets[2].clear_color={0,0,0,0};
    color_targets[3].texture=motion_texture_; color_targets[3].clear_color={0,0,0,0};
    color_targets[4].texture=reactive_mask_texture_; color_targets[4].clear_color={0,0,0,0};
    color_targets[5].texture=indirect_lighting_texture_; color_targets[5].clear_color={0,0,0,0};
    color_targets[6].texture=specular_indirect_texture_; color_targets[6].clear_color={0,0,0,0};
    color_targets[7].texture=reflection_properties_texture_; color_targets[7].clear_color={0,0,0,1};
    for (auto& target : color_targets) { target.load_op=SDL_GPU_LOADOP_CLEAR; target.store_op=SDL_GPU_STOREOP_STORE; }
    color_targets[0].load_op=SDL_GPU_LOADOP_LOAD;
    SDL_GPUDepthStencilTargetInfo depth_target{}; depth_target.texture=depth_texture_; depth_target.clear_depth=1.0F;
    depth_target.load_op=SDL_GPU_LOADOP_CLEAR; depth_target.store_op=SDL_GPU_STOREOP_STORE;
    depth_target.stencil_load_op=SDL_GPU_LOADOP_DONT_CARE; depth_target.stencil_store_op=SDL_GPU_STOREOP_DONT_CARE;
    SDL_GPURenderPass* pass=SDL_BeginGPURenderPass(command,color_targets.data(),static_cast<Uint32>(color_targets.size()),&depth_target);
    SDL_PushGPUFragmentUniformData(command,0,&lighting,sizeof(lighting));
    const std::array<SDL_GPUBuffer*,3> clustered_buffers{local_light_buffer_,light_cluster_buffer_,light_cluster_index_buffer_};
    SDL_BindGPUFragmentStorageBuffers(pass,0,clustered_buffers.data(),static_cast<Uint32>(clustered_buffers.size()));
    opaque_instances_submitted_=0; opaque_draw_calls_=0; opaque_draw_calls_saved_=0;
    for(std::size_t batch_index=0;batch_index<gpu_render_batches.size();++batch_index) {
        const auto& batch=gpu_render_batches[batch_index];
        const auto& object=objects[batch.representative];
        SDL_BindGPUGraphicsPipeline(pass,object.double_sided?gpu_driven_lit_double_sided_pipeline_:gpu_driven_lit_pipeline_);
        const SDL_GPUBufferBinding vertex_binding{object.vertex_buffer,0};const SDL_GPUBufferBinding index_binding{object.index_buffer,0};
        SDL_BindGPUVertexBuffers(pass,0,&vertex_binding,1);SDL_BindGPUIndexBuffer(pass,&index_binding,SDL_GPU_INDEXELEMENTSIZE_32BIT);
        const std::array<SDL_GPUBuffer*,2> gpu_buffers{gpu_driven_instance_buffer_,gpu_driven_visible_index_buffer_};
        SDL_BindGPUVertexStorageBuffers(pass,0,gpu_buffers.data(),static_cast<Uint32>(gpu_buffers.size()));
        const std::array<SDL_GPUTextureSamplerBinding,10> texture_bindings{{
            {shadow_texture_,shadow_sampler_},{object.base_color_texture,material_sampler_},{object.normal_texture,material_sampler_},
            {object.metallic_roughness_texture,material_sampler_},{object.occlusion_texture,material_sampler_},
            {object.emissive_texture,material_sampler_},{irradiance_texture_,environment_sampler_},
            {environment_texture_,environment_sampler_},{brdf_lut_texture_,environment_sampler_},
            {local_shadow_texture_,shadow_sampler_}}};
        SDL_BindGPUFragmentSamplers(pass,0,texture_bindings.data(),static_cast<Uint32>(texture_bindings.size()));
        const GpuDrivenDrawData draw_data{view_projection,previous_view_projection,
            {batch.visible_offset,gpu_batches[batch_index].candidate_count,
             static_cast<std::uint32_t>(gpu_instances.size()),0U}};
        SDL_PushGPUVertexUniformData(command,0,&draw_data,sizeof(draw_data));
        SDL_DrawGPUIndexedPrimitivesIndirect(pass,gpu_driven_indirect_buffer_,batch.indirect_offset,1);
        opaque_instances_submitted_+=batch.reference_visible;++opaque_draw_calls_;++gpu_driven_indirect_draws_;
        opaque_draw_calls_saved_+=batch.reference_visible>0?batch.reference_visible-1U:0U;
    }
    std::vector<bool> submitted(objects.size(),false);
    for (std::size_t object_index=0;object_index<objects.size();++object_index) {
        if (submitted[object_index] || gpu_driven_items[object_index] || objects[object_index].transparent || !objects[object_index].camera_visible) continue;
        std::vector<std::size_t> batch{object_index}; submitted[object_index]=true;
        if (objects[object_index].skinning_matrices==nullptr) {
            for (std::size_t candidate=object_index+1;candidate<objects.size() && batch.size()<maximum_instances_per_draw;++candidate) {
                if (!submitted[candidate] && !gpu_driven_items[candidate] && objects[candidate].camera_visible && compatible_instance_state(objects[object_index],objects[candidate])) {
                    submitted[candidate]=true; batch.push_back(candidate);
                }
            }
        }
        const auto& object=objects[object_index];
        InstancingData instancing{};
        if (batch.size()>1U) {
            instancing.metadata[0]=static_cast<std::uint32_t>(batch.size());
            for (std::size_t instance=0;instance<batch.size();++instance) {
                const auto& source=objects[batch[instance]];
                instancing.models[instance]=source.data.model;
                instancing.previous_models[instance]=source.data.previous_model;
                instancing.object_identities[instance]=source.data.object_identity;
                instancing.colors[instance]=source.data.color;
                instancing.materials[instance]=source.data.material;
                instancing.emissive_normals[instance]=source.data.emissive_normal;
                instancing.occlusion_alpha_flags[instance]=source.data.occlusion_alpha_flags;
            }
        }
        SDL_BindGPUGraphicsPipeline(pass,object.double_sided?lit_double_sided_pipeline_:lit_pipeline_);
        const SDL_GPUBufferBinding vertex_binding{object.vertex_buffer,0};
        const SDL_GPUBufferBinding index_binding{object.index_buffer,0};
        SDL_BindGPUVertexBuffers(pass,0,&vertex_binding,1); SDL_BindGPUIndexBuffer(pass,&index_binding,SDL_GPU_INDEXELEMENTSIZE_32BIT);
        const std::array<SDL_GPUTextureSamplerBinding,10> texture_bindings{{
            {shadow_texture_,shadow_sampler_},{object.base_color_texture,material_sampler_},{object.normal_texture,material_sampler_},
            {object.metallic_roughness_texture,material_sampler_},{object.occlusion_texture,material_sampler_},
            {object.emissive_texture,material_sampler_},{irradiance_texture_,environment_sampler_},
            {environment_texture_,environment_sampler_},{brdf_lut_texture_,environment_sampler_},
            {local_shadow_texture_,shadow_sampler_}
        }};
        SDL_BindGPUFragmentSamplers(pass,0,texture_bindings.data(),static_cast<Uint32>(texture_bindings.size()));
        SDL_PushGPUVertexUniformData(command,0,&object.data,sizeof(ObjectData));
        const auto object_skinning=object.skinning_matrices?skinning_data(*object.skinning_matrices):SkinningData{};
        const auto object_previous_skinning=object.previous_skinning_matrices?
            skinning_data(*object.previous_skinning_matrices):SkinningData{};
        SDL_PushGPUVertexUniformData(command,1,&object_skinning,skinning_palette_bytes);
        SDL_PushGPUVertexUniformData(command,2,&object_previous_skinning,skinning_palette_bytes);
        SDL_PushGPUVertexUniformData(command,3,&instancing,sizeof(instancing));
        SDL_DrawGPUIndexedPrimitives(pass,object.index_count,static_cast<Uint32>(batch.size()),object.first_index,0,0);
        opaque_instances_submitted_+=batch.size(); ++opaque_draw_calls_; opaque_draw_calls_saved_+=batch.size()-1U;
    }
    draw_sprite_batches(pass,false);
    SDL_EndGPURenderPass(pass);
    } else if(pass_id=="render.pass.depth-pyramid-seed") {
    const DepthPyramidSeedSettings settings{{camera_near,camera_far,0.0F,0.0F}};
    SDL_PushGPUComputeUniformData(command,0,&settings,sizeof(settings));
    const SDL_GPUStorageTextureReadWriteBinding target{depth_pyramid_texture_,0U,0U,false,0U,0U,0U};
    auto* pass=SDL_BeginGPUComputePass(command,&target,1U,nullptr,0U);
    if(!pass) {
        last_error_=SDL_GetError();(void)commit_temporal_history(false,
            temporal_history_reset_mask(TemporalHistoryResetReason::output_invalid));
        (void)commit_ssgi_history(false,temporal_history_reset_mask(TemporalHistoryResetReason::output_invalid));
        (void)commit_ssr_history(false,temporal_history_reset_mask(TemporalHistoryResetReason::output_invalid));
        SDL_PopGPUDebugGroup(command);gpu_pass_timestamps_.end_pass(command,pass_id);gpu_pass_timestamps_.end_frame(command);return;
    }
    SDL_BindGPUComputePipeline(pass,depth_pyramid_seed_pipeline_);
    const SDL_GPUTextureSamplerBinding source{depth_texture_,shadow_sampler_};
    SDL_BindGPUComputeSamplers(pass,0U,&source,1U);
    SDL_DispatchGPUCompute(pass,(render_width_+7U)/8U,(render_height_+7U)/8U,1U);
    SDL_EndGPUComputePass(pass);++depth_pyramid_seed_dispatches_;
    } else if(pass_id=="render.pass.depth-pyramid-reduce") {
    std::uint32_t source_width=render_width_,source_height=render_height_;
    for(std::uint32_t destination_mip=1U;destination_mip<depth_pyramid_mip_count_;++destination_mip) {
        const std::uint32_t destination_width=std::max(1U,(source_width+1U)/2U);
        const std::uint32_t destination_height=std::max(1U,(source_height+1U)/2U);
        const DepthPyramidReduceSettings settings{{destination_mip-1U,source_width,source_height,depth_pyramid_mip_count_}};
        SDL_PushGPUComputeUniformData(command,0,&settings,sizeof(settings));
        const SDL_GPUStorageTextureReadWriteBinding target{depth_pyramid_texture_,destination_mip,0U,false,0U,0U,0U};
        auto* pass=SDL_BeginGPUComputePass(command,&target,1U,nullptr,0U);
        if(!pass) {
            last_error_=SDL_GetError();(void)commit_temporal_history(false,
                temporal_history_reset_mask(TemporalHistoryResetReason::output_invalid));
            (void)commit_ssgi_history(false,temporal_history_reset_mask(TemporalHistoryResetReason::output_invalid));
            (void)commit_ssr_history(false,temporal_history_reset_mask(TemporalHistoryResetReason::output_invalid));
            SDL_PopGPUDebugGroup(command);gpu_pass_timestamps_.end_pass(command,pass_id);gpu_pass_timestamps_.end_frame(command);return;
        }
        SDL_BindGPUComputePipeline(pass,depth_pyramid_reduce_pipeline_);
        const SDL_GPUTextureSamplerBinding source{depth_pyramid_texture_,tone_map_sampler_};
        SDL_BindGPUComputeSamplers(pass,0U,&source,1U);
        SDL_DispatchGPUCompute(pass,(destination_width+7U)/8U,(destination_height+7U)/8U,1U);
        SDL_EndGPUComputePass(pass);++depth_pyramid_reduce_dispatches_;
        source_width=destination_width;source_height=destination_height;
    }
    depth_pyramid_history_ready_=true;
    } else if (pass_id == "render.pass.ambient-occlusion") {
    SDL_GPUColorTargetInfo color_target{}; color_target.texture=ambient_occlusion_texture_;
    color_target.clear_color={1,1,1,1}; color_target.load_op=SDL_GPU_LOADOP_CLEAR; color_target.store_op=SDL_GPU_STOREOP_STORE;
    SDL_GPURenderPass* pass=SDL_BeginGPURenderPass(command,&color_target,1,nullptr);
    SDL_BindGPUGraphicsPipeline(pass,gtao_pipeline_);
    const std::array<SDL_GPUTextureSamplerBinding,2> bindings{{
        {depth_texture_,shadow_sampler_},{normal_texture_,tone_map_sampler_}
    }};
    SDL_BindGPUFragmentSamplers(pass,0,bindings.data(),static_cast<Uint32>(bindings.size()));
    const GtaoSettings settings{{1.0F/static_cast<float>(render_width_),1.0F/static_cast<float>(render_height_)},
        camera_near,camera_far,gtao_radius_pixels_,gtao_intensity_,gtao_bias_,gtao_power_};
    SDL_PushGPUFragmentUniformData(command,0,&settings,sizeof(settings));
    if(ambient_occlusion_enabled_)SDL_DrawGPUPrimitives(pass,3,1,0,0);
    SDL_EndGPURenderPass(pass);
    } else if(pass_id=="render.pass.ambient-occlusion-denoise-horizontal" ||
              pass_id=="render.pass.ambient-occlusion-denoise-vertical") {
    const bool horizontal=pass_id.ends_with("horizontal");
    SDL_GPUColorTargetInfo color_target{};
    color_target.texture=horizontal?ambient_occlusion_temp_texture_:ambient_occlusion_filtered_texture_;
    color_target.clear_color={1,1,1,1};color_target.load_op=SDL_GPU_LOADOP_CLEAR;color_target.store_op=SDL_GPU_STOREOP_STORE;
    SDL_GPURenderPass* pass=SDL_BeginGPURenderPass(command,&color_target,1,nullptr);
    SDL_BindGPUGraphicsPipeline(pass,ao_denoise_pipeline_);
    const std::array<SDL_GPUTextureSamplerBinding,3> bindings{{
        {horizontal?ambient_occlusion_texture_:ambient_occlusion_temp_texture_,tone_map_sampler_},
        {depth_texture_,shadow_sampler_},{normal_texture_,tone_map_sampler_}}};
    SDL_BindGPUFragmentSamplers(pass,0,bindings.data(),static_cast<Uint32>(bindings.size()));
    const GtaoDenoiseSettings settings{
        {1.0F/static_cast<float>(std::max(1U,render_width_/2U)),1.0F/static_cast<float>(std::max(1U,render_height_/2U))},
        horizontal?std::array<float,2>{1.0F,0.0F}:std::array<float,2>{0.0F,1.0F},
        camera_near,camera_far,gtao_denoise_depth_sigma_,gtao_denoise_normal_power_};
    SDL_PushGPUFragmentUniformData(command,0,&settings,sizeof(settings));
    SDL_DrawGPUPrimitives(pass,3,1,0,0);
    SDL_EndGPURenderPass(pass);
    } else if(pass_id=="render.pass.aerial-perspective") {
    SDL_GPUColorTargetInfo color_target{};color_target.texture=aerial_hdr_texture_;
    color_target.clear_color={0,0,0,1};color_target.load_op=SDL_GPU_LOADOP_CLEAR;color_target.store_op=SDL_GPU_STOREOP_STORE;
    SDL_GPURenderPass* pass=SDL_BeginGPURenderPass(command,&color_target,1,nullptr);
    SDL_BindGPUGraphicsPipeline(pass,aerial_perspective_pipeline_);
    const std::array<SDL_GPUTextureSamplerBinding,3> bindings{{
        {hdr_texture_,tone_map_sampler_},{depth_texture_,shadow_sampler_},{sky_camera_volume_lut_,sky_lut_sampler_}}};
    SDL_BindGPUFragmentSamplers(pass,0,bindings.data(),static_cast<Uint32>(bindings.size()));
    const auto& volume_budget=sky_atmosphere_quality_budget(atmosphere.quality);
    const float volume_far=std::max(camera_near+0.0001F,
        std::min(camera_far,atmosphere.atmosphere_height_m*2.0F));
    const bool aerial_active=atmosphere.enabled&&atmosphere.quality!=SkyAtmosphereQuality::off&&
        sky_last_path_=="transmittance/multi-scattering/sky-view/camera-volume"&&
        sky_lut_valid_&&!sky_camera_volume_identity_.empty();
    const AerialPerspectiveGpuData settings{
        inverse_or_identity(unjittered_view_projection),
        {camera_position.x,camera_position.y,camera_position.z,1.0F},
        {camera_near,volume_far,2.0F,aerial_active?1.0F:0.0F},
        {static_cast<float>(volume_budget.camera_volume_width),
            static_cast<float>(volume_budget.camera_volume_height),
            static_cast<float>(volume_budget.camera_volume_slices),
            static_cast<float>(atmosphere.debug_view)},
        {camera_forward.x,camera_forward.y,camera_forward.z,0.0F},
        {camera_orthographic?1.0F:0.0F,camera_orthographic_height,
            static_cast<float>(render_width_)/static_cast<float>(render_height_),0.0F}};
    SDL_PushGPUFragmentUniformData(command,0,&settings,sizeof(settings));
    SDL_DrawGPUPrimitives(pass,3,1,0,0);SDL_EndGPURenderPass(pass);
    } else if (pass_id == "render.pass.ambient-occlusion-composite") {
    SDL_GPUColorTargetInfo color_target{};color_target.texture=ao_composited_hdr_texture_;
    color_target.clear_color={0,0,0,1};color_target.load_op=SDL_GPU_LOADOP_CLEAR;color_target.store_op=SDL_GPU_STOREOP_STORE;
    SDL_GPURenderPass* pass=SDL_BeginGPURenderPass(command,&color_target,1,nullptr);
    SDL_BindGPUGraphicsPipeline(pass,ao_composite_pipeline_);
    const std::array<SDL_GPUTextureSamplerBinding,3> bindings{{
        {aerial_hdr_texture_,tone_map_sampler_},{indirect_lighting_texture_,tone_map_sampler_},
        {ambient_occlusion_filtered_texture_,tone_map_sampler_}}};
    SDL_BindGPUFragmentSamplers(pass,0,bindings.data(),static_cast<Uint32>(bindings.size()));
    SDL_DrawGPUPrimitives(pass,3,1,0,0);
    SDL_EndGPURenderPass(pass);
    } else if(pass_id=="render.pass.ssgi-hierarchical-gather") {
    std::array<SDL_GPUColorTargetInfo,2> targets{};
    targets[0].texture=ssgi_raw_texture_;targets[1].texture=ssgi_raw_bent_normal_texture_;
    for(auto& target:targets){target.clear_color={0,0,0,0};target.load_op=SDL_GPU_LOADOP_CLEAR;target.store_op=SDL_GPU_STOREOP_STORE;}
    auto* pass=SDL_BeginGPURenderPass(command,targets.data(),static_cast<Uint32>(targets.size()),nullptr);
    if(!pass){last_error_=SDL_GetError();(void)commit_temporal_history(false,temporal_history_reset_mask(TemporalHistoryResetReason::output_invalid));
        (void)commit_ssgi_history(false,temporal_history_reset_mask(TemporalHistoryResetReason::output_invalid));
        (void)commit_ssr_history(false,temporal_history_reset_mask(TemporalHistoryResetReason::output_invalid));
        SDL_PopGPUDebugGroup(command);gpu_pass_timestamps_.end_pass(command,pass_id);gpu_pass_timestamps_.end_frame(command);return;}
    SDL_BindGPUGraphicsPipeline(pass,ssgi_gather_pipeline_);
    const std::array<SDL_GPUTextureSamplerBinding,5> bindings{{
        {ao_composited_hdr_texture_,tone_map_sampler_},{depth_texture_,shadow_sampler_},
        {depth_pyramid_texture_,tone_map_sampler_},{normal_texture_,tone_map_sampler_},
        {reflection_properties_texture_,tone_map_sampler_}}};
    SDL_BindGPUFragmentSamplers(pass,0,bindings.data(),static_cast<Uint32>(bindings.size()));
    const auto& config=ssgi_plan_.config;
    const SsgiGatherSettings settings{
        inverse_or_identity(unjittered_view_projection),unjittered_view_projection,
        {camera_position.x,camera_position.y,camera_position.z,config.sampling.max_distance},
        {camera_forward.x,camera_forward.y,camera_forward.z,
            static_cast<float>(std::min(config.sampling.max_steps,32U))},
        {static_cast<float>(render_width_),static_cast<float>(render_height_),
            1.0F/static_cast<float>(render_width_),1.0F/static_cast<float>(render_height_)},
        {camera_near,camera_far,config.sampling.radius,config.sampling.thickness},
        {static_cast<float>(std::min(config.sampling.sample_count,16U)),
            static_cast<float>(std::min(config.sampling.max_mip,depth_pyramid_mip_count_-1U)),
            config.composition.confidence_threshold,ssgi_plan_.enabled?1.0F:0.0F},
        {static_cast<float>(ssgi_debug_mode_),static_cast<float>(frame_index_&0x00ffffffU),0.03F,0.12F},
        {0.02F,config.sampling.falloff,config.sampling.intensity,config.sampling.ray_step_pixels}};
    SDL_PushGPUFragmentUniformData(command,0,&settings,sizeof(settings));
    SDL_DrawGPUPrimitives(pass,3,1,0,0);SDL_EndGPURenderPass(pass);
    } else if(pass_id=="render.pass.ssgi-spatial-resolve") {
    std::array<SDL_GPUColorTargetInfo,2> targets{};
    targets[0].texture=ssgi_spatial_texture_;targets[1].texture=ssgi_spatial_bent_normal_texture_;
    for(auto& target:targets){target.clear_color={0,0,0,0};target.load_op=SDL_GPU_LOADOP_CLEAR;target.store_op=SDL_GPU_STOREOP_STORE;}
    auto* pass=SDL_BeginGPURenderPass(command,targets.data(),static_cast<Uint32>(targets.size()),nullptr);
    if(!pass){last_error_=SDL_GetError();(void)commit_temporal_history(false,temporal_history_reset_mask(TemporalHistoryResetReason::output_invalid));
        (void)commit_ssgi_history(false,temporal_history_reset_mask(TemporalHistoryResetReason::output_invalid));
        (void)commit_ssr_history(false,temporal_history_reset_mask(TemporalHistoryResetReason::output_invalid));
        SDL_PopGPUDebugGroup(command);gpu_pass_timestamps_.end_pass(command,pass_id);gpu_pass_timestamps_.end_frame(command);return;}
    SDL_BindGPUGraphicsPipeline(pass,ssgi_spatial_pipeline_);
    const std::array<SDL_GPUTextureSamplerBinding,5> bindings{{
        {ssgi_raw_texture_,tone_map_sampler_},{ssgi_raw_bent_normal_texture_,tone_map_sampler_},
        {depth_texture_,shadow_sampler_},{normal_texture_,tone_map_sampler_},{reflection_properties_texture_,tone_map_sampler_}}};
    SDL_BindGPUFragmentSamplers(pass,0,bindings.data(),static_cast<Uint32>(bindings.size()));
    const SsgiSpatialSettings settings{{1.0F/static_cast<float>(render_width_),1.0F/static_cast<float>(render_height_),80.0F,16.0F},
        {0.02F,ssgi_plan_.config.composition.confidence_threshold,ssgi_plan_.enabled?1.0F:0.0F,static_cast<float>(ssgi_debug_mode_)},{1.0F,0,0,0}};
    SDL_PushGPUFragmentUniformData(command,0,&settings,sizeof(settings));SDL_DrawGPUPrimitives(pass,3,1,0,0);SDL_EndGPURenderPass(pass);
    } else if(pass_id=="render.pass.ssgi-temporal-resolve") {
    const std::uint32_t history_read=ssgi_history_index_,history_write=1U-history_read;
    std::array<SDL_GPUColorTargetInfo,4> targets{};
    targets[0].texture=ssgi_resolved_texture_;targets[1].texture=ssgi_bent_normal_texture_;
    targets[2].texture=ssgi_history_textures_[history_write];targets[3].texture=ssgi_bent_normal_history_textures_[history_write];
    for(auto& target:targets){target.clear_color={0,0,0,0};target.load_op=SDL_GPU_LOADOP_CLEAR;target.store_op=SDL_GPU_STOREOP_STORE;}
    auto* pass=SDL_BeginGPURenderPass(command,targets.data(),static_cast<Uint32>(targets.size()),nullptr);
    if(!pass){last_error_=SDL_GetError();(void)commit_temporal_history(false,temporal_history_reset_mask(TemporalHistoryResetReason::output_invalid));
        (void)commit_ssgi_history(false,temporal_history_reset_mask(TemporalHistoryResetReason::output_invalid));
        (void)commit_ssr_history(false,temporal_history_reset_mask(TemporalHistoryResetReason::output_invalid));
        SDL_PopGPUDebugGroup(command);gpu_pass_timestamps_.end_pass(command,pass_id);gpu_pass_timestamps_.end_frame(command);return;}
    SDL_BindGPUGraphicsPipeline(pass,ssgi_temporal_pipeline_);
    const std::array<SDL_GPUTextureSamplerBinding,8> bindings{{
        {ssgi_spatial_texture_,tone_map_sampler_},{ssgi_spatial_bent_normal_texture_,tone_map_sampler_},
        {ssgi_history_textures_[history_read],tone_map_sampler_},{ssgi_bent_normal_history_textures_[history_read],tone_map_sampler_},
        {depth_texture_,shadow_sampler_},{taa_history_depth_textures_[taa_history_index_],shadow_sampler_},
        {motion_texture_,tone_map_sampler_},{reactive_mask_texture_,tone_map_sampler_}}};
    SDL_BindGPUFragmentSamplers(pass,0,bindings.data(),static_cast<Uint32>(bindings.size()));
    const SsgiTemporalSettings settings{{1.0F/static_cast<float>(render_width_),1.0F/static_cast<float>(render_height_),
        ssgi_plan_.config.history.weight,ssgi_begin.use_previous?1.0F:0.0F},
        {ssgi_plan_.config.history.depth_rejection,1.0F,64.0F,ssgi_plan_.enabled?1.0F:0.0F},
        {static_cast<float>(ssgi_debug_mode_),static_cast<float>(frame_index_&0x00ffffffU),ssgi_plan_.enabled?1.0F:0.0F,0.0F}};
    SDL_PushGPUFragmentUniformData(command,0,&settings,sizeof(settings));SDL_DrawGPUPrimitives(pass,3,1,0,0);SDL_EndGPURenderPass(pass);
    ssgi_history_index_=history_write;
    if(!commit_ssgi_history(ssgi_plan_.enabled&&ssgi_plan_.history.required)){
        SDL_PopGPUDebugGroup(command);gpu_pass_timestamps_.end_pass(command,pass_id);gpu_pass_timestamps_.end_frame(command);return;}
    } else if(pass_id=="render.pass.ssgi-composite") {
    SDL_GPUColorTargetInfo target{};target.texture=ssgi_composited_hdr_texture_;target.clear_color={0,0,0,1};
    target.load_op=SDL_GPU_LOADOP_CLEAR;target.store_op=SDL_GPU_STOREOP_STORE;
    auto* pass=SDL_BeginGPURenderPass(command,&target,1U,nullptr);
    if(!pass){last_error_=SDL_GetError();(void)commit_temporal_history(false,temporal_history_reset_mask(TemporalHistoryResetReason::output_invalid));
        (void)commit_ssgi_history(false,temporal_history_reset_mask(TemporalHistoryResetReason::output_invalid));
        (void)commit_ssr_history(false,temporal_history_reset_mask(TemporalHistoryResetReason::output_invalid));
        SDL_PopGPUDebugGroup(command);gpu_pass_timestamps_.end_pass(command,pass_id);gpu_pass_timestamps_.end_frame(command);return;}
    SDL_BindGPUGraphicsPipeline(pass,ssgi_composite_pipeline_);
    const std::array<SDL_GPUTextureSamplerBinding,6> bindings{{
        {ao_composited_hdr_texture_,tone_map_sampler_},{ssgi_resolved_texture_,tone_map_sampler_},
        {ssgi_bent_normal_texture_,tone_map_sampler_},{reflection_properties_texture_,tone_map_sampler_},
        {indirect_lighting_texture_,tone_map_sampler_},{specular_indirect_texture_,tone_map_sampler_}}};
    SDL_BindGPUFragmentSamplers(pass,0,bindings.data(),static_cast<Uint32>(bindings.size()));
    const SsgiCompositeSettings settings{{ssgi_plan_.enabled?1.0F:0.0F,ssgi_plan_.config.sampling.intensity,
        static_cast<float>(ssgi_debug_mode_),0.02F}};
    SDL_PushGPUFragmentUniformData(command,0,&settings,sizeof(settings));SDL_DrawGPUPrimitives(pass,3,1,0,0);SDL_EndGPURenderPass(pass);
    } else if(pass_id=="render.pass.ssr-hierarchical-trace") {
    SDL_GPUColorTargetInfo target{};target.texture=ssr_raw_texture_;target.clear_color={0,0,0,0};
    target.load_op=SDL_GPU_LOADOP_CLEAR;target.store_op=SDL_GPU_STOREOP_STORE;
    auto* pass=SDL_BeginGPURenderPass(command,&target,1U,nullptr);
    if(!pass) {
        last_error_=SDL_GetError();(void)commit_temporal_history(false,
            temporal_history_reset_mask(TemporalHistoryResetReason::output_invalid));
        (void)commit_ssgi_history(false,temporal_history_reset_mask(TemporalHistoryResetReason::output_invalid));
        (void)commit_ssr_history(false,temporal_history_reset_mask(TemporalHistoryResetReason::output_invalid));
        SDL_PopGPUDebugGroup(command);gpu_pass_timestamps_.end_pass(command,pass_id);gpu_pass_timestamps_.end_frame(command);return;
    }
    SDL_BindGPUGraphicsPipeline(pass,ssr_trace_pipeline_);
    const std::array<SDL_GPUTextureSamplerBinding,5> bindings{{
        {ssgi_composited_hdr_texture_,tone_map_sampler_},{depth_texture_,shadow_sampler_},
        {depth_pyramid_texture_,tone_map_sampler_},{normal_texture_,tone_map_sampler_},
        {reflection_properties_texture_,tone_map_sampler_}}};
    SDL_BindGPUFragmentSamplers(pass,0,bindings.data(),static_cast<Uint32>(bindings.size()));
    const auto& config=ssr_plan_.config;
    const SsrTraceSettings settings{
        inverse_or_identity(unjittered_view_projection),unjittered_view_projection,
        {camera_position.x,camera_position.y,camera_position.z,1.0F},
        {camera_forward.x,camera_forward.y,camera_forward.z,0.0F},
        {static_cast<float>(render_width_),static_cast<float>(render_height_),
            1.0F/static_cast<float>(render_width_),1.0F/static_cast<float>(render_height_)},
        {camera_near,camera_far,config.ray_march.thickness,config.ray_march.max_distance},
        {static_cast<float>(config.ray_march.max_steps),
            static_cast<float>(std::min(config.ray_march.max_mip,depth_pyramid_mip_count_-1U)),
            config.material.roughness_cutoff,ssr_plan_.enabled?1.0F:0.0F},
        {static_cast<float>(ssr_debug_mode_),static_cast<float>(frame_index_&0x00ffffffU),0.0F,0.0F},
        {static_cast<float>(config.ray_march.start_mip),static_cast<float>(config.ray_march.binary_search_steps),
            config.ray_march.initial_step_pixels,config.ray_march.mip_bias},
        {config.edge_fade.start,config.edge_fade.end,config.composition.confidence_threshold,0.0F}};
    SDL_PushGPUFragmentUniformData(command,0,&settings,sizeof(settings));
    SDL_DrawGPUPrimitives(pass,3,1,0,0);SDL_EndGPURenderPass(pass);
    } else if(pass_id=="render.pass.ssr-temporal-resolve") {
    const std::uint32_t history_read=ssr_history_index_,history_write=1U-history_read;
    std::array<SDL_GPUColorTargetInfo,2> targets{};
    targets[0].texture=ssr_resolved_texture_;targets[1].texture=ssr_history_textures_[history_write];
    for(auto& target:targets){target.clear_color={0,0,0,0};target.load_op=SDL_GPU_LOADOP_CLEAR;target.store_op=SDL_GPU_STOREOP_STORE;}
    auto* pass=SDL_BeginGPURenderPass(command,targets.data(),static_cast<Uint32>(targets.size()),nullptr);
    if(!pass) {
        last_error_=SDL_GetError();(void)commit_temporal_history(false,
            temporal_history_reset_mask(TemporalHistoryResetReason::output_invalid));
        (void)commit_ssgi_history(false,temporal_history_reset_mask(TemporalHistoryResetReason::output_invalid));
        (void)commit_ssr_history(false,temporal_history_reset_mask(TemporalHistoryResetReason::output_invalid));
        SDL_PopGPUDebugGroup(command);gpu_pass_timestamps_.end_pass(command,pass_id);gpu_pass_timestamps_.end_frame(command);return;
    }
    SDL_BindGPUGraphicsPipeline(pass,ssr_temporal_pipeline_);
    const std::array<SDL_GPUTextureSamplerBinding,8> bindings{{
        {ssr_raw_texture_,tone_map_sampler_},{ssr_history_textures_[history_read],tone_map_sampler_},
        {depth_texture_,shadow_sampler_},{taa_history_depth_textures_[taa_history_index_],shadow_sampler_},
        {motion_texture_,tone_map_sampler_},{reactive_mask_texture_,tone_map_sampler_},
        {normal_texture_,tone_map_sampler_},{temporal_history_normal_textures_[taa_history_index_],tone_map_sampler_}}};
    SDL_BindGPUFragmentSamplers(pass,0,bindings.data(),static_cast<Uint32>(bindings.size()));
    const SsrTemporalSettings settings{
        {1.0F/static_cast<float>(render_width_),1.0F/static_cast<float>(render_height_),
            ssr_plan_.config.composition.history_weight,ssr_begin.use_previous?1.0F:0.0F},
        {camera_near,camera_far,0.02F,1.0F},
        {64.0F,0.0F,ssr_plan_.enabled&&ssr_plan_.temporal_history_required?1.0F:0.0F,0.0F},
        {0.85F,0.0F,0.0F,0.0F}};
    SDL_PushGPUFragmentUniformData(command,0,&settings,sizeof(settings));
    SDL_DrawGPUPrimitives(pass,3,1,0,0);SDL_EndGPURenderPass(pass);
    ssr_history_index_=history_write;
    if(!commit_ssr_history(ssr_plan_.enabled)) {
        SDL_PopGPUDebugGroup(command);gpu_pass_timestamps_.end_pass(command,pass_id);gpu_pass_timestamps_.end_frame(command);return;
    }
    } else if(pass_id=="render.pass.ssr-composite") {
    SDL_GPUColorTargetInfo target{};target.texture=reflected_hdr_texture_;target.clear_color={0,0,0,1};
    target.load_op=SDL_GPU_LOADOP_CLEAR;target.store_op=SDL_GPU_STOREOP_STORE;
    auto* pass=SDL_BeginGPURenderPass(command,&target,1U,nullptr);
    if(!pass) {
        last_error_=SDL_GetError();(void)commit_temporal_history(false,
            temporal_history_reset_mask(TemporalHistoryResetReason::output_invalid));
        SDL_PopGPUDebugGroup(command);gpu_pass_timestamps_.end_pass(command,pass_id);gpu_pass_timestamps_.end_frame(command);return;
    }
    SDL_BindGPUGraphicsPipeline(pass,ssr_composite_pipeline_);
    const std::array<SDL_GPUTextureSamplerBinding,6> bindings{{
        {ssgi_composited_hdr_texture_,tone_map_sampler_},{ssr_resolved_texture_,tone_map_sampler_},
        {reflection_properties_texture_,tone_map_sampler_},{specular_indirect_texture_,tone_map_sampler_},
        {ambient_occlusion_filtered_texture_,tone_map_sampler_},{normal_texture_,tone_map_sampler_}}};
    SDL_BindGPUFragmentSamplers(pass,0,bindings.data(),static_cast<Uint32>(bindings.size()));
    const SsrCompositeSettings settings{{ssr_plan_.enabled?1.0F:0.0F,1.0F,
        static_cast<float>(ssr_debug_mode_),ssr_plan_.config.material.roughness_cutoff}};
    SDL_PushGPUFragmentUniformData(command,0,&settings,sizeof(settings));
    SDL_DrawGPUPrimitives(pass,3,1,0,0);SDL_EndGPURenderPass(pass);
    } else if (pass_id == "render.pass.transparent-lit") {
    std::array<SDL_GPUColorTargetInfo,8> color_targets{};
    color_targets[0].texture=reflected_hdr_texture_; color_targets[1].texture=object_id_texture_; color_targets[2].texture=normal_texture_;
    color_targets[3].texture=motion_texture_; color_targets[4].texture=reactive_mask_texture_;
    color_targets[5].texture=indirect_lighting_texture_;
    color_targets[6].texture=specular_indirect_texture_;
    color_targets[7].texture=reflection_properties_texture_;
    for (auto& target:color_targets) { target.load_op=SDL_GPU_LOADOP_LOAD; target.store_op=SDL_GPU_STOREOP_STORE; }
    SDL_GPUDepthStencilTargetInfo depth_target{}; depth_target.texture=depth_texture_; depth_target.clear_depth=1.0F;
    depth_target.load_op=SDL_GPU_LOADOP_LOAD; depth_target.store_op=SDL_GPU_STOREOP_STORE;
    depth_target.stencil_load_op=SDL_GPU_LOADOP_DONT_CARE; depth_target.stencil_store_op=SDL_GPU_STOREOP_DONT_CARE;
    SDL_GPURenderPass* pass=SDL_BeginGPURenderPass(command,color_targets.data(),static_cast<Uint32>(color_targets.size()),&depth_target);
    SDL_PushGPUFragmentUniformData(command,0,&lighting,sizeof(lighting));
    const std::array<SDL_GPUBuffer*,3> clustered_buffers{local_light_buffer_,light_cluster_buffer_,light_cluster_index_buffer_};
    SDL_BindGPUFragmentStorageBuffers(pass,0,clustered_buffers.data(),static_cast<Uint32>(clustered_buffers.size()));
    const InstancingData no_instancing{};
    for (const auto& object:objects) {
        if (!object.transparent || !object.camera_visible) continue;
        SDL_BindGPUGraphicsPipeline(pass,object.double_sided?transparent_double_sided_pipeline_:transparent_pipeline_);
        const SDL_GPUBufferBinding vertex_binding{object.vertex_buffer,0};
        const SDL_GPUBufferBinding index_binding{object.index_buffer,0};
        SDL_BindGPUVertexBuffers(pass,0,&vertex_binding,1); SDL_BindGPUIndexBuffer(pass,&index_binding,SDL_GPU_INDEXELEMENTSIZE_32BIT);
        const std::array<SDL_GPUTextureSamplerBinding,10> texture_bindings{{
            {shadow_texture_,shadow_sampler_},{object.base_color_texture,material_sampler_},{object.normal_texture,material_sampler_},
            {object.metallic_roughness_texture,material_sampler_},{object.occlusion_texture,material_sampler_},
            {object.emissive_texture,material_sampler_},{irradiance_texture_,environment_sampler_},
            {environment_texture_,environment_sampler_},{brdf_lut_texture_,environment_sampler_},
            {local_shadow_texture_,shadow_sampler_}
        }};
        SDL_BindGPUFragmentSamplers(pass,0,texture_bindings.data(),static_cast<Uint32>(texture_bindings.size()));
        const auto object_skinning=object.skinning_matrices?skinning_data(*object.skinning_matrices):SkinningData{};
        const auto object_previous_skinning=object.previous_skinning_matrices?
            skinning_data(*object.previous_skinning_matrices):SkinningData{};
        SDL_PushGPUVertexUniformData(command,0,&object.data,sizeof(ObjectData));
        SDL_PushGPUVertexUniformData(command,1,&object_skinning,skinning_palette_bytes);
        SDL_PushGPUVertexUniformData(command,2,&object_previous_skinning,skinning_palette_bytes);
        SDL_PushGPUVertexUniformData(command,3,&no_instancing,sizeof(no_instancing));
        SDL_DrawGPUIndexedPrimitives(pass,object.index_count,1,object.first_index,0,0);
    }
    draw_sprite_batches(pass,true);
    if (vfx_alpha_draw_pipeline_ && vfx_additive_draw_pipeline_ && vfx_particle_buffer_ &&
        vfx_additive_indices_buffer_ && vfx_alpha_indices_buffer_ && vfx_additive_counter_buffer_ && vfx_alpha_counter_buffer_) {
        const bool pixel_stable_vfx=hybrid_pixel_active()&&hybrid_pixel_projection_.valid;
        const VfxCameraData vfx_camera{view_projection,{camera_right.x,camera_right.y,camera_right.z,0.0F},
            {camera_up.x,camera_up.y,camera_up.z,0.0F},1.0F/60.0F,{},
            {static_cast<float>(render_width_),static_cast<float>(render_height_)},
            pixel_stable_vfx?static_cast<float>(hybrid_pixel_projection_.world_units_per_pixel):0.0F,
            pixel_stable_vfx?7U:0U};
        SDL_PushGPUVertexUniformData(command,0,&vfx_camera,sizeof(vfx_camera));
        SDL_BindGPUGraphicsPipeline(pass,vfx_additive_draw_pipeline_);
        const std::array<SDL_GPUBuffer*,2> additive_buffers{{vfx_particle_buffer_,vfx_additive_indices_buffer_}};
        SDL_BindGPUVertexStorageBuffers(pass,0,additive_buffers.data(),static_cast<Uint32>(additive_buffers.size()));
        SDL_DrawGPUPrimitivesIndirect(pass,vfx_additive_counter_buffer_,0,1);
        SDL_BindGPUGraphicsPipeline(pass,vfx_alpha_draw_pipeline_);
        const std::array<SDL_GPUBuffer*,2> alpha_buffers{{vfx_particle_buffer_,vfx_alpha_indices_buffer_}};
        SDL_BindGPUVertexStorageBuffers(pass,0,alpha_buffers.data(),static_cast<Uint32>(alpha_buffers.size()));
        SDL_DrawGPUPrimitivesIndirect(pass,vfx_alpha_counter_buffer_,0,1);
        vfx_indirect_draws_+=2U;
    }
    SDL_EndGPURenderPass(pass);
    } else if (pass_id == "render.pass.temporal-resolve") {
    const std::uint32_t history_read=taa_history_index_;
    const std::uint32_t history_write=1U-history_read;
    std::array<SDL_GPUColorTargetInfo,4> color_targets{};
    color_targets[0].texture=taa_resolved_texture_;
    color_targets[1].texture=taa_history_textures_[history_write];
    color_targets[2].texture=taa_history_depth_textures_[history_write];
    color_targets[3].texture=temporal_history_normal_textures_[history_write];
    for (auto& target:color_targets) {
        target.clear_color={0,0,0,1}; target.load_op=SDL_GPU_LOADOP_CLEAR; target.store_op=SDL_GPU_STOREOP_STORE;
    }
    SDL_GPURenderPass* pass=SDL_BeginGPURenderPass(command,color_targets.data(),static_cast<Uint32>(color_targets.size()),nullptr);
    if(!pass) {
        last_error_=SDL_GetError();
        (void)commit_temporal_history(false,temporal_history_reset_mask(TemporalHistoryResetReason::output_invalid));
        (void)commit_ssgi_history(false,temporal_history_reset_mask(TemporalHistoryResetReason::output_invalid));
        (void)commit_ssr_history(false,temporal_history_reset_mask(TemporalHistoryResetReason::output_invalid));
        SDL_PopGPUDebugGroup(command);gpu_pass_timestamps_.end_pass(command,pass_id);gpu_pass_timestamps_.end_frame(command);
        return;
    }
    SDL_BindGPUGraphicsPipeline(pass,taa_pipeline_);
    const std::array<SDL_GPUTextureSamplerBinding,8> bindings{{
        {reflected_hdr_texture_,tone_map_sampler_},{motion_texture_,tone_map_sampler_},{depth_texture_,shadow_sampler_},
        {normal_texture_,tone_map_sampler_},{taa_history_textures_[history_read],tone_map_sampler_},
        {taa_history_depth_textures_[history_read],shadow_sampler_},{temporal_history_normal_textures_[history_read],tone_map_sampler_},
        {reactive_mask_texture_,tone_map_sampler_}
    }};
    SDL_BindGPUFragmentSamplers(pass,0,bindings.data(),static_cast<Uint32>(bindings.size()));
    const bool hybrid_pixel = hybrid_pixel_active();
    const TemporalDenoiseSettings settings{
        {1.0F/static_cast<float>(post_width_),1.0F/static_cast<float>(post_height_),hybrid_pixel?0.0F:0.90F,
            !hybrid_pixel&&temporal_begin.use_previous?1.0F:0.0F},
        {camera_near,camera_far,0.0F,0.0F},{0.02F,0.85F,64.0F,1.0F},
        {1.0F,static_cast<float>(temporal_debug_mode_),0.0F,0.0F}};
    SDL_PushGPUFragmentUniformData(command,0,&settings,sizeof(settings));
    SDL_DrawGPUPrimitives(pass,3,1,0,0);
    SDL_EndGPURenderPass(pass);
    taa_history_index_=history_write;
    if(!commit_temporal_history(true)) {
        SDL_PopGPUDebugGroup(command);gpu_pass_timestamps_.end_pass(command,pass_id);gpu_pass_timestamps_.end_frame(command);return;
    }
    } else if (pass_id == "render.pass.auto-exposure") {
    const std::uint32_t history_read=exposure_history_index_;
    const std::uint32_t history_write=1U-history_read;
    SDL_GPUColorTargetInfo color_target{}; color_target.texture=exposure_history_textures_[history_write];
    color_target.clear_color={1,1,1,1}; color_target.load_op=SDL_GPU_LOADOP_CLEAR; color_target.store_op=SDL_GPU_STOREOP_STORE;
    SDL_GPURenderPass* pass=SDL_BeginGPURenderPass(command,&color_target,1,nullptr);
    SDL_BindGPUGraphicsPipeline(pass,auto_exposure_pipeline_);
    const std::array<SDL_GPUTextureSamplerBinding,2> bindings{{
        {taa_resolved_texture_,tone_map_sampler_},{exposure_history_textures_[history_read],tone_map_sampler_}
    }};
    SDL_BindGPUFragmentSamplers(pass,0,bindings.data(),static_cast<Uint32>(bindings.size()));
    // Hybrid Pixel output must not breathe as a small emissive sprite enters or
    // leaves the 8x8 luminance probe.  Keep the existing pass/resource graph,
    // but make it write a deterministic unity exposure for that profile.
    const auto pixel_post=hybrid_pixel_profile_
        ?derive_hybrid_pixel_post_process_strategy(*hybrid_pixel_profile_)
        :HybridPixelPostProcessStrategy{};
    const bool stable_pixel_exposure=(hybrid_pixel_active()&&pixel_post.valid)||!auto_exposure_enabled_;
    const bool forced_unity_exposure=!auto_exposure_enabled_;
    const AutoExposureSettings settings{
        forced_unity_exposure?1.0F:(stable_pixel_exposure?pixel_post.auto_exposure.minimum_exposure:auto_exposure_min_),
        forced_unity_exposure?1.0F:(stable_pixel_exposure?pixel_post.auto_exposure.maximum_exposure:auto_exposure_max_),
        forced_unity_exposure?1.0F:(stable_pixel_exposure?pixel_post.auto_exposure.key_value:auto_exposure_key_),1.0F/60.0F,
        forced_unity_exposure?0.0F:(stable_pixel_exposure?pixel_post.auto_exposure.speed_up:auto_exposure_speed_up_),
        forced_unity_exposure?0.0F:(stable_pixel_exposure?pixel_post.auto_exposure.speed_down:auto_exposure_speed_down_),
        !stable_pixel_exposure&&exposure_history_valid_?1.0F:0.0F,0.0F};
    SDL_PushGPUFragmentUniformData(command,0,&settings,sizeof(settings));
    SDL_DrawGPUPrimitives(pass,3,1,0,0);
    SDL_EndGPURenderPass(pass);
    exposure_history_index_=history_write;
    exposure_history_valid_=!stable_pixel_exposure;
    } else if(pass_id.starts_with("render.pass.bloom-downsample-")) {
    std::size_t level{};
    if(pass_id=="render.pass.bloom-downsample-quarter")level=1U;
    else if(pass_id=="render.pass.bloom-downsample-eighth")level=2U;
    else if(pass_id=="render.pass.bloom-downsample-sixteenth")level=3U;
    SDL_GPUTexture* source=level==0U?taa_resolved_texture_:bloom_downsample_textures_[level-1U];
    SDL_GPUColorTargetInfo color_target{};color_target.texture=bloom_downsample_textures_[level];
    color_target.clear_color={0,0,0,1};color_target.load_op=SDL_GPU_LOADOP_CLEAR;color_target.store_op=SDL_GPU_STOREOP_STORE;
    SDL_GPURenderPass* pass=SDL_BeginGPURenderPass(command,&color_target,1,nullptr);
    SDL_BindGPUGraphicsPipeline(pass,bloom_downsample_pipeline_);
    const SDL_GPUTextureSamplerBinding source_binding{source,tone_map_sampler_};
    SDL_BindGPUFragmentSamplers(pass,0,&source_binding,1);
    const auto source_divisor=1U<<level;
    const auto source_width=std::max(1U,post_width_/source_divisor);
    const auto source_height=std::max(1U,post_height_/source_divisor);
    const auto pixel_post=hybrid_pixel_profile_
        ?derive_hybrid_pixel_post_process_strategy(*hybrid_pixel_profile_)
        :HybridPixelPostProcessStrategy{};
    const bool controlled_bloom=hybrid_pixel_active()&&pixel_post.valid;
    const BloomDownsampleSettings settings{{1.0F/static_cast<float>(source_width),1.0F/static_cast<float>(source_height)},
        controlled_bloom?pixel_post.bloom.threshold:bloom_threshold_,
        controlled_bloom?pixel_post.bloom.soft_knee:bloom_soft_knee_,level==0U?1.0F:0.0F,{}};
    SDL_PushGPUFragmentUniformData(command,0,&settings,sizeof(settings));
    SDL_DrawGPUPrimitives(pass,3,1,0,0);SDL_EndGPURenderPass(pass);
    } else if(pass_id.starts_with("render.pass.bloom-upsample-")) {
    std::size_t level{};
    if(pass_id=="render.pass.bloom-upsample-quarter")level=1U;
    else if(pass_id=="render.pass.bloom-upsample-half")level=2U;
    const std::array<SDL_GPUTexture*,3> high_sources{bloom_downsample_textures_[2],bloom_downsample_textures_[1],bloom_downsample_textures_[0]};
    const std::array<SDL_GPUTexture*,3> low_sources{bloom_downsample_textures_[3],bloom_upsample_textures_[0],bloom_upsample_textures_[1]};
    const std::array<std::uint32_t,3> low_divisors{16U,8U,4U};
    SDL_GPUColorTargetInfo color_target{};color_target.texture=bloom_upsample_textures_[level];
    color_target.clear_color={0,0,0,1};color_target.load_op=SDL_GPU_LOADOP_CLEAR;color_target.store_op=SDL_GPU_STOREOP_STORE;
    SDL_GPURenderPass* pass=SDL_BeginGPURenderPass(command,&color_target,1,nullptr);
    SDL_BindGPUGraphicsPipeline(pass,bloom_upsample_pipeline_);
    const std::array<SDL_GPUTextureSamplerBinding,2> bindings{{
        {high_sources[level],tone_map_sampler_},{low_sources[level],tone_map_sampler_}}};
    SDL_BindGPUFragmentSamplers(pass,0,bindings.data(),static_cast<Uint32>(bindings.size()));
    const auto low_width=std::max(1U,post_width_/low_divisors[level]);
    const auto low_height=std::max(1U,post_height_/low_divisors[level]);
    const auto pixel_post=hybrid_pixel_profile_
        ?derive_hybrid_pixel_post_process_strategy(*hybrid_pixel_profile_)
        :HybridPixelPostProcessStrategy{};
    const bool controlled_bloom=hybrid_pixel_active()&&pixel_post.valid;
    const BloomUpsampleSettings settings{{1.0F/static_cast<float>(low_width),1.0F/static_cast<float>(low_height)},
        controlled_bloom?pixel_post.bloom.scatter:bloom_scatter_,0.0F};
    SDL_PushGPUFragmentUniformData(command,0,&settings,sizeof(settings));
    SDL_DrawGPUPrimitives(pass,3,1,0,0);SDL_EndGPURenderPass(pass);
    } else if (pass_id == "render.pass.tone-map") {
    SDL_GPUColorTargetInfo color_target{};
    color_target.texture=hybrid_pixel_active()?tone_mapped_texture_:color_texture_;
    color_target.clear_color={0,0,0,1}; color_target.load_op=SDL_GPU_LOADOP_CLEAR; color_target.store_op=SDL_GPU_STOREOP_STORE;
    SDL_GPURenderPass* pass=SDL_BeginGPURenderPass(command,&color_target,1,nullptr);
    SDL_BindGPUGraphicsPipeline(pass,tone_map_pipeline_);
    const std::array<SDL_GPUTextureSamplerBinding,3> bindings{{
        {taa_resolved_texture_,tone_map_sampler_},{bloom_upsample_textures_[2],tone_map_sampler_},
        {exposure_history_textures_[exposure_history_index_],tone_map_sampler_}
    }};
    SDL_BindGPUFragmentSamplers(pass,0,bindings.data(),static_cast<Uint32>(bindings.size()));
    const auto pixel_post=hybrid_pixel_profile_
        ?derive_hybrid_pixel_post_process_strategy(*hybrid_pixel_profile_)
        :HybridPixelPostProcessStrategy{};
    const bool controlled_bloom=hybrid_pixel_active()&&pixel_post.valid;
    const ToneMapSettings settings{exposure_,white_point_,
        controlled_bloom?pixel_post.bloom.strength:bloom_strength_,temporal_debug_mode_>0U?1.0F:0.0F,
        color_lift_,color_gamma_,color_gain_,color_saturation_,color_contrast_,color_temperature_,color_tint_};
    SDL_PushGPUFragmentUniformData(command,0,&settings,sizeof(settings));
    SDL_DrawGPUPrimitives(pass,3,1,0,0);
    SDL_EndGPURenderPass(pass);
    if (hybrid_pixel_active()) {
        const auto& source = pixel_presentation_.virtual_rect;
        const auto& destination = pixel_presentation_.content_rect;
        const SDL_GPUBlitInfo blit{
            .source = {tone_mapped_texture_, 0U, 0U, source.x, source.y, source.width, source.height},
            .destination = {color_texture_, 0U, 0U, destination.x, destination.y,
                destination.width, destination.height},
            .load_op = SDL_GPU_LOADOP_CLEAR,
            .clear_color = {0.0F, 0.0F, 0.0F, 1.0F},
            .flip_mode = SDL_FLIP_NONE,
            .filter = SDL_GPU_FILTER_NEAREST,
            .cycle = false};
        SDL_BlitGPUTexture(command, &blit);
    }
    } else if(pass_id=="render.pass.native-rt-debug-composite") {
    if(native_rt_composite_plan_.valid&&native_rt_texture_export_.texture&&
       native_rt_composite_pipeline_) {
        SDL_GPUColorTargetInfo target{};target.texture=color_texture_;
        target.clear_color={0.0F,0.0F,0.0F,1.0F};
        target.load_op=SDL_GPU_LOADOP_CLEAR;target.store_op=SDL_GPU_STOREOP_STORE;
        SDL_GPURenderPass* pass=SDL_BeginGPURenderPass(command,&target,1,nullptr);
        SDL_BindGPUGraphicsPipeline(pass,native_rt_composite_pipeline_);
        const SDL_GPUTextureSamplerBinding binding{
            native_rt_texture_export_.texture,tone_map_sampler_};
        SDL_BindGPUFragmentSamplers(pass,0,&binding,1);
        const NativeRtCompositeSettings settings{{
            native_rt_composite_plan_.width,native_rt_composite_plan_.height,
            native_rt_composite_plan_.debug_mode,0U},{0.02F,0.025F,0.03F,1.0F}};
        SDL_PushGPUFragmentUniformData(command,0,&settings,sizeof(settings));
        SDL_DrawGPUPrimitives(pass,3,1,0,0);
        SDL_EndGPURenderPass(pass);
        native_rt_composite_recorded_=true;
    }
    } else if (pass_id == "render.pass.fxaa") {
    SDL_GPUColorTargetInfo color_target{}; color_target.texture=color_texture_;
    color_target.clear_color={0,0,0,1}; color_target.load_op=SDL_GPU_LOADOP_CLEAR; color_target.store_op=SDL_GPU_STOREOP_STORE;
    SDL_GPURenderPass* pass=SDL_BeginGPURenderPass(command,&color_target,1,nullptr);
    SDL_BindGPUGraphicsPipeline(pass,fxaa_pipeline_);
    const SDL_GPUTextureSamplerBinding source_binding{tone_mapped_texture_,tone_map_sampler_};
    SDL_BindGPUFragmentSamplers(pass,0,&source_binding,1);
    const FxaaSettings settings{{1.0F/static_cast<float>(post_width_),1.0F/static_cast<float>(post_height_)},fxaa_edge_threshold_,fxaa_edge_threshold_min_};
    SDL_PushGPUFragmentUniformData(command,0,&settings,sizeof(settings));
    SDL_DrawGPUPrimitives(pass,3,1,0,0);
    SDL_EndGPURenderPass(pass);
    }
    SDL_PopGPUDebugGroup(command);
    gpu_pass_timestamps_.end_pass(command,pass_id);
    const auto record_duration=std::chrono::duration<double,std::micro>(std::chrono::steady_clock::now()-record_start).count();
    if (pass_id=="render.pass.shadow-depth") shadow_record_microseconds_=record_duration;
    else if(pass_id=="render.pass.gpu-visibility")gpu_visibility_record_microseconds_=record_duration;
    else if(pass_id=="render.pass.sky-atmosphere")sky_atmosphere_record_microseconds_=record_duration;
    else if (pass_id=="render.pass.opaque-lit") opaque_record_microseconds_=record_duration;
    else if(pass_id=="render.pass.aerial-perspective")aerial_perspective_record_microseconds_=record_duration;
    else if (pass_id=="render.pass.ambient-occlusion") gtao_record_microseconds_=record_duration;
    else if(pass_id.starts_with("render.pass.ambient-occlusion-denoise-"))ao_denoise_record_microseconds_+=record_duration;
    else if (pass_id=="render.pass.ambient-occlusion-composite") ao_composite_record_microseconds_=record_duration;
    else if(pass_id=="render.pass.ssgi-hierarchical-gather")ssgi_gather_record_microseconds_=record_duration;
    else if(pass_id=="render.pass.ssgi-spatial-resolve")ssgi_spatial_record_microseconds_=record_duration;
    else if(pass_id=="render.pass.ssgi-temporal-resolve")ssgi_temporal_record_microseconds_=record_duration;
    else if(pass_id=="render.pass.ssgi-composite")ssgi_composite_record_microseconds_=record_duration;
    else if(pass_id=="render.pass.ssr-hierarchical-trace")ssr_trace_record_microseconds_=record_duration;
    else if(pass_id=="render.pass.ssr-temporal-resolve")ssr_temporal_record_microseconds_=record_duration;
    else if(pass_id=="render.pass.ssr-composite")ssr_composite_record_microseconds_=record_duration;
    else if (pass_id=="render.pass.transparent-lit") transparent_record_microseconds_=record_duration;
    else if (pass_id=="render.pass.temporal-resolve") taa_record_microseconds_=record_duration;
    else if (pass_id=="render.pass.auto-exposure") auto_exposure_record_microseconds_=record_duration;
    else if(pass_id.starts_with("render.pass.bloom-"))bloom_record_microseconds_+=record_duration;
    else if (pass_id=="render.pass.tone-map") tone_map_record_microseconds_=record_duration;
    else if (pass_id=="render.pass.fxaa") fxaa_record_microseconds_=record_duration;
    }
    if(!temporal_history_committed && !commit_temporal_history(false,
        temporal_history_reset_mask(TemporalHistoryResetReason::output_invalid)))return;
    if(!ssgi_history_committed && !commit_ssgi_history(false,
        temporal_history_reset_mask(TemporalHistoryResetReason::output_invalid)))return;
    if(!ssr_history_committed && !commit_ssr_history(false,
        temporal_history_reset_mask(TemporalHistoryResetReason::output_invalid)))return;
    gpu_pass_timestamps_.end_frame(command);
    previous_view_projection_=view_projection.value;
    previous_unjittered_view_projection_=unjittered_view_projection.value;
    previous_temporal_frame_=frame_index_;
    previous_camera_id_=active_camera_id_;
    previous_models_.clear();
    previous_skinning_matrices_.clear();
    for (const auto& object:objects) {
        previous_models_[object.history_key]=object.data.model.value;
        if(!object.skinning_matrices)continue;
        auto& pose=previous_skinning_matrices_[object.history_key];
        pose.assign(object.skinning_matrices->begin(),object.skinning_matrices->begin()+
            static_cast<std::ptrdiff_t>(std::min(object.skinning_matrices->size(),static_cast<std::size_t>(SkeletalPose::maximum_joints))));
    }
    for(const auto& sprite:sprites) previous_models_[sprite.history_key]=sprite.model.value;
    vfx_alive_buffer_index_=1U-vfx_alive_buffer_index_;
}

bool SceneRenderer::enqueue_gpu_visibility_readback(SDL_GPUCommandBuffer* command) {
    gpu_visibility_readback_error_.clear();
    gpu_visibility_readback_count_match_=false;
    gpu_visibility_readback_exact_set_match_=false;
    gpu_visibility_readback_conservative_subset_match_=false;
    gpu_visibility_readback_unexpected_visible_=0;
    gpu_occlusion_readback_statistics_.fill(0U);
    gpu_visibility_readback_match_=false;
    if(!command||gpu_visibility_readback_transfer_) {
        gpu_visibility_readback_state_="failed";
        gpu_visibility_readback_error_="readback already pending or command buffer is null";
        last_error_=gpu_visibility_readback_error_;
        return false;
    }
    if(!gpu_driven_indirect_buffer_||!gpu_driven_visible_index_buffer_||gpu_driven_batches_==0||
       gpu_driven_batch_candidate_offsets_.size()!=gpu_driven_batches_||
       gpu_driven_batch_candidate_counts_.size()!=gpu_driven_batches_||
       gpu_driven_batch_visible_offsets_.size()!=gpu_driven_batches_||
       gpu_driven_batch_reference_visible_indices_.size()!=gpu_driven_batches_) {
        gpu_visibility_readback_state_="failed";
        gpu_visibility_readback_error_="no complete GPU-driven visibility batch snapshot is available";
        last_error_=gpu_visibility_readback_error_;
        return false;
    }
    const auto indirect_bytes=gpu_driven_batches_*sizeof(GpuIndexedIndirectCommand);
    const auto visible_index_bytes=gpu_driven_candidates_*sizeof(std::uint32_t);
    if(indirect_bytes>std::numeric_limits<Uint32>::max()||
       visible_index_bytes>std::numeric_limits<Uint32>::max()-indirect_bytes) {
        gpu_visibility_readback_state_="failed";
        gpu_visibility_readback_error_="visibility command/index readback exceeds the SDL transfer size limit";
        last_error_=gpu_visibility_readback_error_;
        return false;
    }
    const auto statistics_bytes=gpu_occlusion_used_this_frame_&&gpu_occlusion_statistics_buffer_
        ?static_cast<std::size_t>(gpu_occlusion_statistics_bytes):0U;
    if(statistics_bytes>std::numeric_limits<Uint32>::max()-indirect_bytes-visible_index_bytes) {
        gpu_visibility_readback_state_="failed";
        gpu_visibility_readback_error_="visibility statistics readback exceeds the SDL transfer size limit";
        last_error_=gpu_visibility_readback_error_;
        return false;
    }
    const auto bytes=indirect_bytes+visible_index_bytes+statistics_bytes;
    const SDL_GPUTransferBufferCreateInfo info{SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD,static_cast<Uint32>(bytes),0};
    gpu_visibility_readback_transfer_=SDL_CreateGPUTransferBuffer(device_,&info);
    if(!gpu_visibility_readback_transfer_) {
        gpu_visibility_readback_state_="failed";
        gpu_visibility_readback_error_=SDL_GetError();last_error_=gpu_visibility_readback_error_;
        return false;
    }
    auto* copy=SDL_BeginGPUCopyPass(command);
    if(!copy) {
        gpu_visibility_readback_state_="failed";gpu_visibility_readback_error_=SDL_GetError();
        last_error_=gpu_visibility_readback_error_;
        SDL_ReleaseGPUTransferBuffer(device_,gpu_visibility_readback_transfer_);
        gpu_visibility_readback_transfer_=nullptr;
        return false;
    }
    const SDL_GPUBufferRegion command_source{gpu_driven_indirect_buffer_,0,static_cast<Uint32>(indirect_bytes)};
    const SDL_GPUTransferBufferLocation command_destination{gpu_visibility_readback_transfer_,0};
    SDL_DownloadFromGPUBuffer(copy,&command_source,&command_destination);
    const SDL_GPUBufferRegion index_source{gpu_driven_visible_index_buffer_,0,static_cast<Uint32>(visible_index_bytes)};
    const SDL_GPUTransferBufferLocation index_destination{gpu_visibility_readback_transfer_,static_cast<Uint32>(indirect_bytes)};
    SDL_DownloadFromGPUBuffer(copy,&index_source,&index_destination);
    if(statistics_bytes>0U) {
        const SDL_GPUBufferRegion statistics_source{gpu_occlusion_statistics_buffer_,0,
            static_cast<Uint32>(statistics_bytes)};
        const SDL_GPUTransferBufferLocation statistics_destination{gpu_visibility_readback_transfer_,
            static_cast<Uint32>(indirect_bytes+visible_index_bytes)};
        SDL_DownloadFromGPUBuffer(copy,&statistics_source,&statistics_destination);
    }
    SDL_EndGPUCopyPass(copy);
    gpu_visibility_readback_state_="queued";
    gpu_visibility_readback_frame_=frame_index_;
    gpu_visibility_readback_candidates_=gpu_driven_candidates_;
    gpu_visibility_readback_batches_=gpu_driven_batches_;
    gpu_visibility_readback_cpu_visible_=gpu_driven_reference_visible_;
    gpu_visibility_readback_gpu_visible_=0;
    gpu_visibility_readback_invalid_batches_=0;
    gpu_visibility_readback_mismatched_batch_counts_=0;
    gpu_visibility_readback_out_of_range_indices_=0;
    gpu_visibility_readback_wrong_batch_indices_=0;
    gpu_visibility_readback_duplicate_indices_=0;
    gpu_visibility_readback_occlusion_active_=statistics_bytes>0U;
    gpu_visibility_readback_bytes_=bytes;
    gpu_visibility_readback_indirect_bytes_=indirect_bytes;
    gpu_visibility_readback_cpu_set_hash_=visibility_index_set_hash(gpu_driven_batch_reference_visible_indices_);
    gpu_visibility_readback_gpu_set_hash_.clear();
    gpu_visibility_readback_actual_indices_.clear();
    gpu_visibility_readback_actual_draw_ids_.clear();
    gpu_visibility_readback_candidate_draw_ids_=gpu_driven_candidate_draw_ids_;
    gpu_visibility_readback_batch_candidate_offsets_=gpu_driven_batch_candidate_offsets_;
    gpu_visibility_readback_batch_candidate_counts_=gpu_driven_batch_candidate_counts_;
    gpu_visibility_readback_batch_visible_offsets_=gpu_driven_batch_visible_offsets_;
    gpu_visibility_readback_expected_indices_=gpu_driven_batch_reference_visible_indices_;
    return true;
}

void SceneRenderer::attach_gpu_visibility_readback_fence(SDL_GPUFence* fence) {
    if(gpu_visibility_readback_state_!="queued"||!gpu_visibility_readback_transfer_||!fence) {
        if(fence)SDL_ReleaseGPUFence(device_,fence);
        gpu_visibility_readback_state_="failed";
        gpu_visibility_readback_error_="GPU visibility readback fence was attached in an invalid state";
        last_error_=gpu_visibility_readback_error_;
        return;
    }
    gpu_visibility_readback_fence_=fence;
    gpu_visibility_readback_state_="submitted";
}

bool SceneRenderer::resolve_gpu_visibility_readback() {
    if(gpu_visibility_readback_state_!="submitted"||!gpu_visibility_readback_transfer_||!gpu_visibility_readback_fence_) {
        if(gpu_visibility_readback_state_!="failed") {
            gpu_visibility_readback_state_="failed";
            gpu_visibility_readback_error_="no submitted GPU visibility readback with an owned fence";
        }
        last_error_=gpu_visibility_readback_error_;
        return false;
    }
    SDL_GPUFence* fences[]{gpu_visibility_readback_fence_};
    if(!SDL_WaitForGPUFences(device_,true,fences,1)) {
        gpu_visibility_readback_state_="failed";gpu_visibility_readback_error_=SDL_GetError();
        last_error_=gpu_visibility_readback_error_;
        SDL_ReleaseGPUFence(device_,gpu_visibility_readback_fence_);gpu_visibility_readback_fence_=nullptr;
        SDL_ReleaseGPUTransferBuffer(device_,gpu_visibility_readback_transfer_);gpu_visibility_readback_transfer_=nullptr;
        return false;
    }
    SDL_ReleaseGPUFence(device_,gpu_visibility_readback_fence_);gpu_visibility_readback_fence_=nullptr;
    const auto* mapped=static_cast<const std::byte*>(
        SDL_MapGPUTransferBuffer(device_,gpu_visibility_readback_transfer_,false));
    if(!mapped) {
        gpu_visibility_readback_state_="failed";gpu_visibility_readback_error_=SDL_GetError();
        last_error_=gpu_visibility_readback_error_;
        SDL_ReleaseGPUTransferBuffer(device_,gpu_visibility_readback_transfer_);
        gpu_visibility_readback_transfer_=nullptr;
        return false;
    }
    std::size_t visible{};
    std::size_t invalid{};
    std::size_t mismatched_counts{};
    std::size_t out_of_range{};
    std::size_t wrong_batch{};
    std::size_t duplicates{};
    std::size_t unexpected_visible{};
    bool exact_set_match=true;
    std::vector<std::vector<std::uint32_t>> gpu_visible_indices(gpu_visibility_readback_batches_);
    std::vector<std::string> gpu_visible_draw_ids;
    for(std::size_t index=0;index<gpu_visibility_readback_batches_;++index) {
        GpuIndexedIndirectCommand command{};
        std::memcpy(&command,mapped+index*sizeof(command),sizeof(command));
        const auto candidate_offset=gpu_visibility_readback_batch_candidate_offsets_[index];
        const auto candidate_count=gpu_visibility_readback_batch_candidate_counts_[index];
        const auto visible_offset=gpu_visibility_readback_batch_visible_offsets_[index];
        if(command.instance_count>candidate_count)++invalid;
        const auto expected_count=gpu_visibility_readback_expected_indices_[index].size();
        if(gpu_visibility_readback_occlusion_active_
            ?command.instance_count>expected_count
            :command.instance_count!=expected_count)++mismatched_counts;
        visible+=command.instance_count;
        const auto readable_count=std::min(command.instance_count,candidate_count);
        auto& actual=gpu_visible_indices[index];
        actual.reserve(readable_count);
        for(std::uint32_t local=0;local<readable_count;++local) {
            std::uint32_t candidate{};
            const auto byte_offset=gpu_visibility_readback_indirect_bytes_+
                (static_cast<std::size_t>(visible_offset)+local)*sizeof(candidate);
            std::memcpy(&candidate,mapped+byte_offset,sizeof(candidate));
            actual.push_back(candidate);
            if(candidate>=gpu_visibility_readback_candidates_)++out_of_range;
            else if(candidate<candidate_offset||candidate>=candidate_offset+candidate_count)++wrong_batch;
            else if(candidate<gpu_visibility_readback_candidate_draw_ids_.size())
                gpu_visible_draw_ids.push_back(gpu_visibility_readback_candidate_draw_ids_[candidate]);
        }
        std::sort(actual.begin(),actual.end());
        for(std::size_t position=1;position<actual.size();++position) {
            if(actual[position]==actual[position-1])++duplicates;
        }
        auto expected=gpu_visibility_readback_expected_indices_[index];
        std::sort(expected.begin(),expected.end());
        for(const auto candidate:actual) {
            if(!std::binary_search(expected.begin(),expected.end(),candidate))++unexpected_visible;
        }
        if(actual!=expected)exact_set_match=false;
    }
    if(gpu_visibility_readback_occlusion_active_) {
        const auto statistics_offset=gpu_visibility_readback_indirect_bytes_+
            gpu_visibility_readback_candidates_*sizeof(std::uint32_t);
        std::memcpy(gpu_occlusion_readback_statistics_.data(),mapped+statistics_offset,
            gpu_occlusion_statistics_bytes);
    }
    SDL_UnmapGPUTransferBuffer(device_,gpu_visibility_readback_transfer_);
    SDL_ReleaseGPUTransferBuffer(device_,gpu_visibility_readback_transfer_);
    gpu_visibility_readback_transfer_=nullptr;
    gpu_visibility_readback_gpu_visible_=visible;
    gpu_visibility_readback_invalid_batches_=invalid;
    gpu_visibility_readback_mismatched_batch_counts_=mismatched_counts;
    gpu_visibility_readback_out_of_range_indices_=out_of_range;
    gpu_visibility_readback_wrong_batch_indices_=wrong_batch;
    gpu_visibility_readback_duplicate_indices_=duplicates;
    gpu_visibility_readback_unexpected_visible_=unexpected_visible;
    gpu_visibility_readback_gpu_set_hash_=visibility_index_set_hash(gpu_visible_indices);
    gpu_visibility_readback_actual_indices_=gpu_visible_indices;
    std::sort(gpu_visible_draw_ids.begin(),gpu_visible_draw_ids.end());
    gpu_visible_draw_ids.erase(std::unique(gpu_visible_draw_ids.begin(),gpu_visible_draw_ids.end()),gpu_visible_draw_ids.end());
    gpu_visibility_readback_actual_draw_ids_=std::move(gpu_visible_draw_ids);
    gpu_visibility_readback_count_match_=invalid==0&&mismatched_counts==0&&
        (gpu_visibility_readback_occlusion_active_
            ?visible<=gpu_visibility_readback_cpu_visible_
            :visible==gpu_visibility_readback_cpu_visible_)&&
        visible<=gpu_visibility_readback_candidates_;
    gpu_visibility_readback_exact_set_match_=exact_set_match&&out_of_range==0&&wrong_batch==0&&duplicates==0;
    gpu_visibility_readback_conservative_subset_match_=gpu_visibility_readback_occlusion_active_&&
        invalid==0&&out_of_range==0&&wrong_batch==0&&duplicates==0&&unexpected_visible==0&&
        visible<=gpu_visibility_readback_cpu_visible_;
    gpu_visibility_readback_match_=gpu_visibility_readback_occlusion_active_
        ?gpu_visibility_readback_count_match_&&gpu_visibility_readback_conservative_subset_match_
        :gpu_visibility_readback_count_match_&&gpu_visibility_readback_exact_set_match_&&
            gpu_visibility_readback_cpu_set_hash_==gpu_visibility_readback_gpu_set_hash_;
    gpu_visibility_readback_state_="complete";
    if(!gpu_visibility_readback_match_) {
        gpu_visibility_readback_error_=gpu_visibility_readback_occlusion_active_
            ?"GPU occlusion-visible indices are not a valid subset of the CPU frustum oracle"
            :"GPU compact visible-index sets do not exactly match the CPU frustum oracle";
        last_error_=gpu_visibility_readback_error_;
    }
    return gpu_visibility_readback_match_;
}

bool SceneRenderer::enqueue_color_capture(SDL_GPUCommandBuffer* command) {
    return enqueue_texture_capture(command,color_texture_,width_,height_);
}

bool SceneRenderer::enqueue_texture_capture(SDL_GPUCommandBuffer* command,SDL_GPUTexture* texture,
                                            const std::uint32_t width,const std::uint32_t height,const SDL_GPUTextureFormat format,
                                            const bool swap_red_blue) {
    if (!texture || capture_transfer_||width==0||height==0) return false;
    SDL_GPUTransferBufferCreateInfo info{};
    info.usage=SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
    info.size=width*height*4U;
    capture_transfer_=SDL_CreateGPUTransferBuffer(device_,&info);
    if (!capture_transfer_) { last_error_=SDL_GetError(); return false; }
    SDL_GPUCopyPass* pass=SDL_BeginGPUCopyPass(command);
    if (!pass) { last_error_=SDL_GetError(); SDL_ReleaseGPUTransferBuffer(device_,capture_transfer_); capture_transfer_=nullptr; return false; }
    SDL_GPUTextureRegion source{texture,0,0,0,0,0,width,height,1};
    SDL_GPUTextureTransferInfo destination{capture_transfer_,0,width,height};
    SDL_DownloadFromGPUTexture(pass,&source,&destination);
    SDL_EndGPUCopyPass(pass);
    capture_width_=width;capture_height_=height;capture_format_=format;capture_swap_red_blue_=swap_red_blue;return true;
}

bool SceneRenderer::save_color_capture(const std::string& path) {
    if (!capture_transfer_) { last_error_="No GPU color capture is pending"; return false; }
    if (!SDL_WaitForGPUIdle(device_)) { last_error_=SDL_GetError(); return false; }
    void* pixels=SDL_MapGPUTransferBuffer(device_,capture_transfer_,false);
    if (!pixels) { last_error_=SDL_GetError(); return false; }
    const auto pixel_count=static_cast<std::size_t>(capture_width_)*capture_height_;
    const auto* source_pixels=static_cast<const std::uint8_t*>(pixels);
    std::vector<std::uint8_t> converted;
    const auto bgra=capture_swap_red_blue_||capture_format_==SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM||
        capture_format_==SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM_SRGB;
    if(bgra) {
        converted.assign(source_pixels,source_pixels+pixel_count*4U);
        for(std::size_t index=0;index<pixel_count;++index)std::swap(converted[index*4U],converted[index*4U+2U]);
    }
    const auto* rgba=bgra?converted.data():source_pixels;
    double luma_sum{};
    std::size_t dark_pixels{};
    std::size_t bright_pixels{};
    for (std::size_t index=0;index<pixel_count;++index) {
        const auto offset=index*4U;
        const double luma=(0.2126*rgba[offset]+0.7152*rgba[offset+1U]+0.0722*rgba[offset+2U])/255.0;
        luma_sum+=luma;
        if (luma<0.01) ++dark_pixels;
        if (luma>0.99) ++bright_pixels;
    }
    const double mean_luma=pixel_count?luma_sum/static_cast<double>(pixel_count):0.0;
    const double dark_fraction=pixel_count?static_cast<double>(dark_pixels)/static_cast<double>(pixel_count):1.0;
    const double bright_fraction=pixel_count?static_cast<double>(bright_pixels)/static_cast<double>(pixel_count):1.0;
    SDL_Surface* surface=SDL_CreateSurfaceFrom(
        static_cast<int>(capture_width_),static_cast<int>(capture_height_),SDL_PIXELFORMAT_RGBA32,
        const_cast<std::uint8_t*>(rgba),static_cast<int>(capture_width_*4U));
    bool saved=false;
    const auto output=std::filesystem::path(path);
    if (surface) {
        if (!output.parent_path().empty()) std::filesystem::create_directories(output.parent_path());
        saved=SDL_SaveBMP(surface,output.string().c_str());
        if (!saved) last_error_=SDL_GetError();
        SDL_DestroySurface(surface);
    } else {
        last_error_=SDL_GetError();
    }
    if (saved) {
        auto reference_contract=nlohmann::json::parse(capture_contract_json_,nullptr,false);
        if(!reference_contract.is_object())reference_contract=nlohmann::json::object();
        const auto reference_quality=reference_contract.value("quality",nlohmann::json::object());
        const auto reference_capture=reference_contract.value("capture",nlohmann::json::object());
        const double mean_luma_min=reference_quality.value("meanLumaMin",0.05);
        const double mean_luma_max=reference_quality.value("meanLumaMax",0.90);
        const double dark_fraction_max=reference_quality.value("darkPixelFractionMax",0.60);
        const double bright_fraction_max=reference_quality.value("brightPixelFractionMax",0.10);
        const auto expected_width=reference_capture.value("width",capture_width_);
        const auto expected_height=reference_capture.value("height",capture_height_);
        const bool dimensions_match=capture_width_==expected_width&&capture_height_==expected_height;
        const bool quality_pass=dimensions_match&&mean_luma>=mean_luma_min&&mean_luma<=mean_luma_max&&
            dark_fraction<=dark_fraction_max&&bright_fraction<=bright_fraction_max;
        nlohmann::json report{
            {"schemaVersion","noemancer.render-quality.v1"}, {"imagePath",output.generic_string()},
            {"width",capture_width_}, {"height",capture_height_}, {"extractionId",extraction_id_},
            {"metrics",{{"meanLuma",mean_luma},{"darkPixelFraction",dark_fraction},{"brightPixelFraction",bright_fraction}}},
            {"contract",{{"meanLumaMin",mean_luma_min},{"meanLumaMax",mean_luma_max},
                {"darkPixelFractionMax",dark_fraction_max},{"brightPixelFractionMax",bright_fraction_max},
                {"expectedWidth",expected_width},{"expectedHeight",expected_height}}},
            {"dimensionsMatch",dimensions_match},
            {"pass",quality_pass}, {"renderer",nlohmann::json::parse(status_json())}};
        if(!reference_contract.empty())report["referenceContract"]=reference_contract;
        std::ofstream sidecar(output.string()+".quality.json",std::ios::binary|std::ios::trunc);
        if (sidecar) sidecar << report.dump(2) << '\n';
    }
    SDL_UnmapGPUTransferBuffer(device_,capture_transfer_);
    SDL_ReleaseGPUTransferBuffer(device_,capture_transfer_);
    capture_transfer_=nullptr;capture_width_=0;capture_height_=0;capture_swap_red_blue_=false;
    return saved;
}

bool SceneRenderer::enqueue_pick(SDL_GPUCommandBuffer* command, const std::uint32_t x, const std::uint32_t y) {
    last_error_.clear();
    if (!object_id_texture_ || pick_transfer_ || x >= width_ || y >= height_) return false;
    std::uint32_t render_x{};
    std::uint32_t render_y{};
    if (hybrid_pixel_active()) {
        const auto mapping = map_physical_pixel(pixel_presentation_, x, y);
        if (!mapping.valid_virtual_pixel) return false;
        render_x = mapping.virtual_x;
        render_y = mapping.virtual_y;
    } else {
        render_x=std::min(render_width_-1U,static_cast<std::uint32_t>(static_cast<std::uint64_t>(x)*render_width_/width_));
        render_y=std::min(render_height_-1U,static_cast<std::uint32_t>(static_cast<std::uint64_t>(y)*render_height_/height_));
    }
    SDL_GPUTransferBufferCreateInfo info{};
    info.usage=SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD; info.size=16;
    pick_transfer_=SDL_CreateGPUTransferBuffer(device_,&info);
    if (!pick_transfer_) { last_error_=SDL_GetError(); return false; }
    SDL_GPUCopyPass* pass=SDL_BeginGPUCopyPass(command);
    if (!pass) { last_error_=SDL_GetError(); SDL_ReleaseGPUTransferBuffer(device_,pick_transfer_); pick_transfer_=nullptr; return false; }
    SDL_GPUTextureRegion source{object_id_texture_,0,0,render_x,render_y,0,1,1,1};
    SDL_GPUTextureTransferInfo destination{pick_transfer_,0,1,1};
    SDL_DownloadFromGPUTexture(pass,&source,&destination);
    source.texture=depth_texture_; destination.offset=4;
    SDL_DownloadFromGPUTexture(pass,&source,&destination);
    source.texture=normal_texture_; destination.offset=8;
    SDL_DownloadFromGPUTexture(pass,&source,&destination);
    SDL_EndGPUCopyPass(pass);
    last_pick_x_=x; last_pick_y_=y;
    return true;
}

void SceneRenderer::attach_pick_fence(SDL_GPUFence* fence) {
    if (pick_fence_) SDL_ReleaseGPUFence(device_,pick_fence_);
    pick_fence_=fence;
}

std::string SceneRenderer::resolve_pick() {
    if (!pick_transfer_) return {};
    if (!pick_fence_) { last_error_="Pick readback has no submission fence"; return {}; }
    SDL_GPUFence* fences[]{pick_fence_};
    if (!SDL_WaitForGPUFences(device_,true,fences,1)) { last_error_=SDL_GetError(); return {}; }
    const auto* bytes=static_cast<const std::byte*>(SDL_MapGPUTransferBuffer(device_,pick_transfer_,false));
    std::uint32_t object_id{};
    std::array<std::uint16_t,4> encoded_normal{};
    if (bytes) {
        std::memcpy(&object_id,bytes,sizeof(object_id));
        std::memcpy(&last_pick_depth_,bytes+4,sizeof(last_pick_depth_));
        std::memcpy(encoded_normal.data(),bytes+8,sizeof(encoded_normal));
        for (std::size_t index=0;index<last_pick_normal_.size();++index) {
            const auto decoded=half_to_float(encoded_normal[index]);
            last_pick_normal_[index]=std::isfinite(decoded)?decoded:0.0F;
        }
        SDL_UnmapGPUTransferBuffer(device_,pick_transfer_);
        has_pixel_evidence_=true;
    }
    SDL_ReleaseGPUTransferBuffer(device_,pick_transfer_); pick_transfer_=nullptr;
    SDL_ReleaseGPUFence(device_,pick_fence_); pick_fence_=nullptr;
    last_picked_entity_id_=object_id<object_id_entities_.size()?object_id_entities_[object_id]:std::string{};
    return last_picked_entity_id_;
}

std::string SceneRenderer::last_pixel_evidence_json() const {
    if (!has_pixel_evidence_) return "null";
    std::ostringstream out;
    out << "{\"schemaVersion\":\"noemancer.pixel-evidence.v1\",\"extractionId\":\"" << extraction_id_
        << "\",\"pixel\":{\"x\":" << last_pick_x_ << ",\"y\":" << last_pick_y_ << "},\"entityId\":\""
        << last_picked_entity_id_ << "\",\"deviceDepth\":" << last_pick_depth_ << ",\"worldNormal\":["
        << last_pick_normal_[0] << ',' << last_pick_normal_[1] << ',' << last_pick_normal_[2]
        << "],\"resources\":[\"render.resource.object-id\",\"render.resource.scene-depth\",\"render.resource.world-normal\"]}";
    return out.str();
}

std::string SceneRenderer::status_json() const {
    const auto& shader_contract=runtime_shader_artifacts();
    const auto gpu_timestamp_evidence=nlohmann::json::parse(gpu_pass_timestamps_.status_json());
    const auto temporal_history_evidence=nlohmann::json::parse(temporal_history_authority_.canonical_evidence());
    auto ssr_history_evidence=nlohmann::json::object();
    ssr_history_evidence["schema"]=temporal_history_schema;
    for(const auto& consumer:temporal_history_evidence.value("consumers",nlohmann::json::array())) {
        if(consumer.value("consumer",std::string{})=="ssr") {
            for(auto it=consumer.begin();it!=consumer.end();++it)ssr_history_evidence[it.key()]=it.value();
            break;
        }
    }
    const auto reported_ssr_plan=ssr_plan_.valid?ssr_plan_:build_screen_space_reflections_plan(
        ssr_config_,hybrid_pixel_active(),{depth_pyramid_texture_!=nullptr,ao_composited_hdr_texture_!=nullptr,
            reflection_properties_texture_!=nullptr&&specular_indirect_texture_!=nullptr});
    const auto ssr_contract_evidence=nlohmann::json::parse(
        screen_space_reflections_canonical_evidence(reported_ssr_plan));
    const nlohmann::json ssr_evidence{
        {"schema",std::string(screen_space_reflections_schema)},
        {"enabled",reported_ssr_plan.enabled},
        {"code",reported_ssr_plan.code},
        {"settings",{{"quality",screen_space_reflections_quality_name(reported_ssr_plan.quality)},
            {"maxSteps",reported_ssr_plan.config.ray_march.max_steps},
            {"startMip",reported_ssr_plan.config.ray_march.start_mip},
            {"maxMip",std::min(reported_ssr_plan.config.ray_march.max_mip,
                depth_pyramid_mip_count_?depth_pyramid_mip_count_-1U:0U)},
            {"binarySearchSteps",reported_ssr_plan.config.ray_march.binary_search_steps},
            {"initialStepPixels",reported_ssr_plan.config.ray_march.initial_step_pixels},
            {"maxRayDistance",reported_ssr_plan.config.ray_march.max_distance},
            {"thickness",reported_ssr_plan.config.ray_march.thickness},
            {"roughnessCutoff",reported_ssr_plan.config.material.roughness_cutoff},
            {"edgeFadeStart",reported_ssr_plan.config.edge_fade.start},
            {"edgeFadeEnd",reported_ssr_plan.config.edge_fade.end},
            {"historyWeight",reported_ssr_plan.config.composition.history_weight}}},
        {"hiZ",{{"reused",true},{"resourceId","render.resource.scene-depth-pyramid"},
            {"encoding","linear-view-depth-min-max"},{"mipCount",depth_pyramid_mip_count_}}},
        {"historyAuthority",ssr_history_evidence},
        {"materialValidity",{{"contract",std::string(screen_space_reflections_material_contract)},
            {"roughnessSource","render.resource.world-normal.a"},
            {"surfaceSource","render.resource.surface-reflection-properties/base-color-rgb+metallic-a"},
            {"normalSource","render.resource.world-normal.xyz"},
            {"invalidMaterialPolicy","retain-ibl-specular"}}},
        {"composition",{{"strategy",std::string(screen_space_reflections_composition_strategy)},
            {"specularFallbackSource","render.resource.scene-specular-indirect"},
            {"ambientVisibilitySource","render.resource.ambient-occlusion-filtered"}}},
        {"fallback",{{"policy",std::string(screen_space_reflections_fallback_strategy)},
            {"debugView","miss"},{"offscreen","retain-ibl-specular"}}},
        {"debugView",ssr_debug_mode_name_},
        {"debugViews",{"final","confidence","hit-distance","roughness","miss","normal"}},
        {"passes",{"render.pass.ssr-hierarchical-trace","render.pass.ssr-temporal-resolve","render.pass.ssr-composite"}},
        {"workingSetBytes",static_cast<std::uint64_t>(render_width_)*render_height_*56ULL},
        {"contract",ssr_contract_evidence},
        {"fingerprint",screen_space_reflections_fingerprint(reported_ssr_plan)}};
    auto ssgi_history_evidence=nlohmann::json::object();
    ssgi_history_evidence["schema"]=temporal_history_schema;
    for(const auto& consumer:temporal_history_evidence.value("consumers",nlohmann::json::array())) {
        if(consumer.value("consumer",std::string{})=="ssgi") {
            for(auto it=consumer.begin();it!=consumer.end();++it)ssgi_history_evidence[it.key()]=it.value();
            break;
        }
    }
    const auto reported_ssgi_plan=ssgi_plan_.valid?ssgi_plan_:build_screen_space_global_illumination_plan(
        ssgi_config_,hybrid_pixel_active(),{depth_pyramid_texture_!=nullptr,normal_texture_!=nullptr,
            reflection_properties_texture_!=nullptr&&indirect_lighting_texture_!=nullptr,
            ssgi_history_textures_[0]&&ssgi_history_textures_[1]&&
                ssgi_bent_normal_history_textures_[0]&&ssgi_bent_normal_history_textures_[1]});
    const auto ssgi_contract_evidence=nlohmann::json::parse(
        screen_space_global_illumination_canonical_evidence(reported_ssgi_plan));
    const nlohmann::json ssgi_evidence{
        {"schema",std::string(screen_space_global_illumination_schema)},
        {"enabled",reported_ssgi_plan.enabled},{"code",reported_ssgi_plan.code},
        {"settings",{{"quality",screen_space_global_illumination_quality_name(reported_ssgi_plan.quality)},
            {"sampleCount",reported_ssgi_plan.config.sampling.sample_count},
            {"directions",reported_ssgi_plan.config.sampling.directions},
            {"maxSteps",reported_ssgi_plan.config.sampling.max_steps},
            {"maxMip",std::min(reported_ssgi_plan.config.sampling.max_mip,
                depth_pyramid_mip_count_?depth_pyramid_mip_count_-1U:0U)},
            {"radius",reported_ssgi_plan.config.sampling.radius},
            {"maxDistance",reported_ssgi_plan.config.sampling.max_distance},
            {"thickness",reported_ssgi_plan.config.sampling.thickness},
            {"intensity",reported_ssgi_plan.config.sampling.intensity},
            {"historyWeight",reported_ssgi_plan.config.history.weight}}},
        {"hiZ",{{"reused",true},{"resourceId","render.resource.scene-depth-pyramid"},
            {"encoding","linear-view-depth-min-max"},{"mipCount",depth_pyramid_mip_count_}}},
        {"historyAuthority",ssgi_history_evidence},
        {"materialValidity",{{"contract",std::string(screen_space_global_illumination_material_contract)},
            {"surfaceSource","render.resource.surface-reflection-properties/base-color-rgb+metallic-a"},
            {"normalSource","render.resource.world-normal.xyz"},
            {"roughnessSource","render.resource.world-normal.a"},{"invalidMaterialPolicy","retain-ibl-diffuse"}}},
        {"bentNormal",{{"enabled",reported_ssgi_plan.bent_normal_output},
            {"semantics",std::string(screen_space_global_illumination_bent_normal_semantics)},
            {"resourceId","render.resource.scene-gi-bent-normal-visibility"}}},
        {"visibility",{{"enabled",reported_ssgi_plan.visibility_output},
            {"semantics",std::string(screen_space_global_illumination_visibility_semantics)}}},
        {"composition",{{"strategy",std::string(screen_space_global_illumination_composition_strategy)},
            {"diffuseFallbackSource","render.resource.scene-indirect-minus-scene-specular-indirect"}}},
        {"fallback",{{"policy",std::string(screen_space_global_illumination_fallback_strategy)},
            {"offscreen","retain-ibl-diffuse"},{"history",std::string(screen_space_global_illumination_history_fallback)}}},
        {"debugView",ssgi_debug_mode_name_},{"debugViews",{"final","confidence","visibility","bent-normal","miss"}},
        {"passes",{"render.pass.ssgi-hierarchical-gather","render.pass.ssgi-spatial-resolve",
            "render.pass.ssgi-temporal-resolve","render.pass.ssgi-composite"}},
        {"workingSetBytes",static_cast<std::uint64_t>(render_width_)*render_height_*80ULL},
        {"contract",ssgi_contract_evidence},{"fingerprint",screen_space_global_illumination_fingerprint(reported_ssgi_plan)}};
    const nlohmann::json device_evidence{
        {"backend",gpu_backend_}, {"adapter",gpu_device_name_}, {"driverName",gpu_driver_name_},
        {"driverVersion",gpu_driver_version_}, {"driverInfo",gpu_driver_info_},
        {"validationEnabled",gpu_debug_},
        {"availableBackends",available_gpu_backends_}, {"shaderArtifact",shader_artifact_format_},
        {"engineShaderArtifacts",{"DXIL","SPIR-V"}},
        {"artifactStatus",shader_contract.valid()?"manifest-and-artifact-verified":"contract-invalid"},
        {"artifactContract",{{"schema",std::string(shader_artifact_manifest_schema)},
            {"manifestHash",std::string(shader_contract.manifest_hash())},
            {"sourceContractHash",std::string(shader_contract.source_hash())},
            {"root",shader_contract.manifest_path().parent_path().generic_string()},
            {"code",std::string(shader_contract.error_code())}}},
        {"portabilityScope","D3D12/DXIL and Vulkan/SPIR-V; Metal artifact pipeline pending"}
    };
    auto native_raytracing_evidence=nlohmann::json::parse(native_raytracing_status_json_,nullptr,false);
    if(native_raytracing_evidence.is_discarded()||!native_raytracing_evidence.is_object()) {
        native_raytracing_evidence=nlohmann::json{
            {"schema",std::string(scene_raytracing_bridge_schema)},
            {"requested",false},{"enabled",false},{"backend",gpu_backend_},
            {"sceneAccepted",false},{"nativeAsReady",false},{"nativeTraceReady",false},
            {"visualPath","ssgi-raster-fallback"},{"fallbackCode","bridge.not-observed"},
            {"rtgiReady",false}};
    }
    native_raytracing_evidence["composite"]["recordedThisFrame"]=
        native_rt_composite_recorded_;
    nlohmann::json stream_states=nlohmann::json::array();
    std::size_t authored_mip_levels_total{};
    for(std::size_t index=0;index<texture_streams_.size();++index) {
        const auto& stream=texture_streams_[index];authored_mip_levels_total+=stream.level_count;
        stream_states.push_back({{"streamIndex",index},{"assetId",stream.asset_id},
            {"semantic",stream.linear_semantic?"linear":"srgb"},{"mode",stream.streaming_enabled?"stream":"resident"},
            {"format",stream.format},{"width",stream.width},{"height",stream.height},
            {"levelCount",stream.level_count},{"residentMipStart",stream.resident_mip_start},
            {"screenMipStart",stream.screen_mip_start},{"targetMipStart",stream.target_mip_start},
            {"maximumMipStart",stream.maximum_mip_start},{"residentLevels",stream.level_count-stream.resident_mip_start},
            {"gpuAllocationBytesEstimate",stream.resident_bytes},{"fullChainBytesEstimate",stream.full_chain_bytes},
            {"importance",texture_streaming_importance_name(static_cast<TextureStreamingImportance>(
                std::min<std::uint8_t>(stream.authored_importance,3U)))},{"authoredPriority",stream.authored_priority},
            {"demandAgeFrames",stream.demand_age_frames},{"visibilityAgeFrames",stream.visibility_age_frames},
            {"lastUsedFrame",stream.last_used_frame},{"transitionPending",stream.transition_pending},
            {"atTarget",stream.at_target()},{"fullyResident",stream.complete()}});
    }
    nlohmann::json hybrid_pixel_evidence{
        {"schemaVersion", "noemancer.hybrid-pixel-render/0.1"},
        {"active", hybrid_pixel_active()},
        {"profile", nullptr},
        {"presentation", nullptr},
        {"projection", {
            {"enabled", hybrid_pixel_projection_.enabled},
            {"valid", hybrid_pixel_projection_.valid},
            {"code", hybrid_pixel_projection_.code},
            {"detail", hybrid_pixel_projection_.detail},
            {"orthographicCamera", hybrid_pixel_projection_.orthographic_camera},
            {"cameraSnapped", hybrid_pixel_projection_.camera_snapped},
            {"spritesSnapped", hybrid_pixel_projection_.sprites_snapped},
            {"tileCellsSnapped", hybrid_pixel_projection_.tile_cells_snapped},
            {"worldUnitsPerPixel", hybrid_pixel_projection_.world_units_per_pixel},
            {"orthographicHeight", hybrid_pixel_projection_.orthographic_height}}},
        {"vfx", {
            {"sameGpuLifecycle", true},
            {"centerSnap", hybrid_pixel_active() && hybrid_pixel_projection_.valid},
            {"sizeQuantization", hybrid_pixel_active() && hybrid_pixel_projection_.valid},
            {"renderExtent", hybrid_pixel_active() ? "virtual" : "render"},
            {"sampling", hybrid_pixel_active() ? "profile-controlled" : "linear"}}},
        {"postProcess", {
            {"renderExtent", hybrid_pixel_active() ? "virtual" : "post"},
            {"temporalHistory", hybrid_pixel_active() ? "disabled" : "enabled"},
            {"autoExposure", (!auto_exposure_enabled_ || hybrid_pixel_active()) ? "locked-unity" : "adaptive"},
            {"bloom", hybrid_pixel_active() ? "fixed-grid-dual-filter" : "dual-filter"},
            {"ambientOcclusion", hybrid_pixel_active() ? "virtual-grid-spatial" : "render-grid-spatial"},
            {"presentation", hybrid_pixel_active() ? "nearest-integer" : "native"}}}};
    if (hybrid_pixel_profile_) {
        hybrid_pixel_evidence["profile"] = {
            {"schema", hybrid_pixel_profile_->schema},
            {"profileId", hybrid_pixel_profile_->profile_id},
            {"enabled", hybrid_pixel_profile_->enabled},
            {"virtualWidth", hybrid_pixel_profile_->virtual_width},
            {"virtualHeight", hybrid_pixel_profile_->virtual_height},
            {"pixelsPerUnit", hybrid_pixel_profile_->pixels_per_unit},
            {"integerScaling", hybrid_pixel_profile_->integer_scaling},
            {"snapCamera", hybrid_pixel_profile_->snap_camera},
            {"snapSprites", hybrid_pixel_profile_->snap_sprites},
            {"presentationFilter", hybrid_pixel_profile_->presentation_filter}};
    }
    if (pixel_presentation_.valid) {
        const auto status = pixel_presentation_.status == PixelPresentationStatus::exact ? "exact" :
            pixel_presentation_.status == PixelPresentationStatus::letterboxed ? "letterboxed" :
            pixel_presentation_.status == PixelPresentationStatus::undersized ? "undersized" : "invalid";
        hybrid_pixel_evidence["presentation"] = {
            {"valid", true}, {"status", status}, {"code", pixel_presentation_.code},
            {"integerScale", pixel_presentation_.integer_scale},
            {"physicalOutput", {{"width", pixel_presentation_.physical_output_extent.width},
                {"height", pixel_presentation_.physical_output_extent.height}}},
            {"virtualExtent", {{"width", pixel_presentation_.virtual_extent.width},
                {"height", pixel_presentation_.virtual_extent.height}}},
            {"contentRect", {{"x", pixel_presentation_.content_rect.x}, {"y", pixel_presentation_.content_rect.y},
                {"width", pixel_presentation_.content_rect.width}, {"height", pixel_presentation_.content_rect.height}}},
            {"virtualRect", {{"x", pixel_presentation_.virtual_rect.x}, {"y", pixel_presentation_.virtual_rect.y},
                {"width", pixel_presentation_.virtual_rect.width}, {"height", pixel_presentation_.virtual_rect.height}}},
            {"letterbox", {{"left", pixel_presentation_.letterbox.left}, {"top", pixel_presentation_.letterbox.top},
                {"right", pixel_presentation_.letterbox.right}, {"bottom", pixel_presentation_.letterbox.bottom}}}};
    }
    std::optional<double> latest_shadow_gpu_milliseconds;
    if(const auto& latest=gpu_pass_timestamps_.latest_frame();latest&&latest->state=="available") {
        for(const auto& pass:latest->passes)if(pass.pass_id=="render.pass.shadow-depth"&&pass.milliseconds) {
            latest_shadow_gpu_milliseconds=pass.milliseconds;break;
        }
    }
    ShadowScalabilityInput shadow_scalability_input;
    shadow_scalability_input.workload=ShadowScalabilityWorkload{
        .directional_enabled=true,.cascade_count=shadow_cascade_count,.cascade_resolution=shadow_size,
        .local_enabled=true,.local_layer_count=local_shadow_layer_count,.local_resolution=local_shadow_resolution_,
        .requested_local_lights=static_cast<std::uint32_t>(local_shadow_requested_lights_),
        .selected_local_lights=static_cast<std::uint32_t>(local_shadow_selected_lights_),
        .dropped_local_lights=static_cast<std::uint32_t>(local_shadow_dropped_lights_),
        .estimated_atlas_bytes=shadow_texture_bytes_+local_shadow_texture_bytes_};
    shadow_scalability_input.cache=ShadowScalabilityCacheObservation{
        .available=true,
        .directional_cascades_available=static_cast<std::uint32_t>(directional_shadow_cascades_rendered_+directional_shadow_cascades_cached_),
        .directional_cascades_cached=static_cast<std::uint32_t>(directional_shadow_cascades_cached_),
        .directional_cache_hits=directional_shadow_cache_hits_,.directional_cache_misses=directional_shadow_cache_misses_,
        .local_faces_available=static_cast<std::uint32_t>(local_shadow_faces_rendered_+local_shadow_faces_cached_),
        .local_faces_cached=static_cast<std::uint32_t>(local_shadow_faces_cached_),
        .local_cache_hits=local_shadow_cache_hits_,.local_cache_misses=local_shadow_cache_misses_};
    shadow_scalability_input.geometry=ShadowScalabilityGeometryObservation{
        .available=shadow_casters_>0U&&shadow_primitives_>0U,
        .caster_count=shadow_casters_,.primitive_count=shadow_primitives_,
        .draw_count=shadow_caster_draws_+local_shadow_draw_calls_+
            directional_shadow_avoided_draws_+local_shadow_avoided_draws_,
        .instances_submitted=shadow_instances_submitted_+local_shadow_instances_submitted_+
            directional_shadow_avoided_instances_+local_shadow_avoided_instances_,
        .draw_calls_saved=shadow_draw_calls_saved_+local_shadow_draw_calls_saved_};
    shadow_scalability_input.timing=ShadowScalabilityTimingObservation{
        .available=latest_shadow_gpu_milliseconds.has_value(),
        .shadow_pass_milliseconds=latest_shadow_gpu_milliseconds.value_or(0.0),
        .directional_pass_milliseconds=0.0,.local_pass_milliseconds=0.0,
        .frame_budget_milliseconds=1000.0/60.0};
    shadow_scalability_input.invalidation=ShadowScalabilityInvalidationObservation{
        .available=frame_index_>0U,
        .invalidations_last_window=directional_shadow_cache_misses_+local_shadow_cache_misses_,
        .observation_frames=frame_index_};
    shadow_scalability_input.atlas_budget_bytes=128ULL*1024ULL*1024ULL;
    const auto shadow_scalability_plan=evaluate_shadow_scalability(shadow_scalability_input);
    auto shadow_scalability_evidence=nlohmann::json::parse(
        shadow_scalability_policy_canonical_evidence(shadow_scalability_plan));
    shadow_scalability_evidence["fingerprint"]=shadow_scalability_policy_fingerprint(shadow_scalability_plan);

    std::ostringstream out;
    out << "{\"schemaVersion\":\"noemancer.renderer-status.v30\",\"renderer\":\"SDL_GPU\",\"device\":" << device_evidence.dump()
        << ",\"nativeRayTracing\":" << native_raytracing_evidence.dump()
        << ",\"pipeline\":\"forward-lit\",\"builtInPrimitives\":{\"sphere\":{\"topology\":\"uv-sphere\",\"segments\":"
        << builtin_sphere_segments << ",\"rings\":" << builtin_sphere_rings << ",\"triangles\":"
        << builtin_sphere_index_count/3U << "}},\"surface\":{\"width\":" << width_
        << ",\"height\":" << height_ << ",\"renderWidth\":" << render_width_ << ",\"renderHeight\":" << render_height_
        << ",\"postWidth\":" << post_width_ << ",\"postHeight\":" << post_height_
        << ",\"renderScale\":" << render_scale_ << ",\"effectiveRenderScale\":" << (hybrid_pixel_active()?1.0F:render_scale_)
        << "},\"hybridPixel\":" << hybrid_pixel_evidence.dump()
        << ",\"framePipeline\":{\"allowedFramesInFlight\":" << allowed_frames_in_flight_
        << ",\"resourceCycling\":\"SDL_GPU-native-cycle-on-write\",\"gpuTimestampQueries\":"
        << (gpu_pass_timestamps_.supported()?"true":"false") << ",\"gpuTimestamp\":" << gpu_timestamp_evidence.dump()
        << ",\"gpuTimestampReason\":\"" << (gpu_pass_timestamps_.supported()?"ok":gpu_timestamp_evidence.value("reason",std::string{"unavailable"})) << "\""
        << ",\"debugCapture\":{\"commandGroups\":true,\"renderGraphPassLabels\":" << render_graph_.execution_order.size()
        << ",\"namedRenderTextures\":49,\"namedVfxBuffers\":13}},\"renderWorld\":{\"extractionId\":\"" << extraction_id_
        << "\",\"worldRevision\":" << world_revision_ << ",\"frameIndex\":" << frame_index_ << "},\"graph\":" << render_graph_json(render_graph_) << ","
        << "\"vfxGpu\":{\"pipelineCreated\":" << (vfx_compute_pipeline_&&vfx_spawn_pipeline_&&vfx_group_pipeline_&&vfx_sort_alpha_pipeline_&&vfx_alpha_draw_pipeline_&&vfx_additive_draw_pipeline_?"true":"false") << ",\"resourcesAllocated\":" << (vfx_particle_buffer_&&vfx_alive_buffers_[0]&&vfx_alive_buffers_[1]&&vfx_dead_buffer_&&vfx_counter_buffers_[0]&&vfx_counter_buffers_[1]&&vfx_dead_counter_buffer_&&vfx_spawn_buffer_&&vfx_spawn_graph_buffer_&&vfx_additive_indices_buffer_&&vfx_alpha_indices_buffer_&&vfx_additive_counter_buffer_&&vfx_alpha_counter_buffer_?"true":"false") << ",\"kernels\":[\"vfx_sim.comp\",\"vfx_spawn.comp\",\"vfx_group.comp\",\"vfx_sort_alpha.comp\"],\"drawShader\":\"vfx_billboard\",\"threadGroupSize\":64,\"alphaSortThreadGroupSize\":256,\"capacity\":" << vfx_gpu_capacity << ",\"particleStrideBytes\":" << vfx_particle_stride << ",\"spawnIdentityStrideBytes\":" << vfx_spawn_identity_stride << ",\"spawnGraphStrideBytes\":" << vfx_spawn_graph_stride << ",\"workingSetBytes\":" << (vfx_gpu_capacity*(vfx_particle_stride+vfx_spawn_identity_stride+vfx_spawn_graph_stride+20U)+vfx_counter_bytes*5U) << ",\"dispatchGroups\":" << vfx_dispatch_groups_ << ",\"controlUploads\":" << vfx_state_uploads_ << ",\"spawnIdentitiesUploaded\":" << vfx_particles_uploaded_ << ",\"spawnGraphCommandsUploaded\":" << vfx_spawn_graph_commands_uploaded_ << ",\"aliveIndicesUploaded\":0,\"dynamicParticleStateUploaded\":false,\"cpuExpectedResidentParticles\":" << vfx_resident_particles_ << ",\"cpuExpectedBlendGroups\":{\"additive\":" << vfx_expected_additive_particles_ << ",\"alpha\":" << vfx_expected_alpha_particles_ << "},\"cpuIdsReclaimedThisFrame\":" << vfx_slots_reclaimed_ << ",\"cpuSpawnPayloadsDroppedThisFrame\":" << vfx_particles_dropped_ << ",\"uploadBytesTotal\":" << vfx_upload_bytes_ << ",\"inputMode\":\"gpu-alive-ping-pong/dead-list/particle-identity-plus-emitter-graph-parameters\",\"particleStatePersistence\":true,\"fullParticleStateUploadPerFrame\":false,\"curveEvaluation\":\"gpu-age/color-size-start-end\",\"gpuNativeSlotAllocation\":true,\"gpuNativeAliveDeadLifecycle\":true,\"gpuNativeBlendGrouping\":true,\"gpuNativeAlphaSort\":true,\"alphaSortPolicy\":\"back-to-front/stable-particle-id/multi-dispatch-bitonic/dynamic-power-of-two-span/max-8192\",\"alphaSortSynchronization\":\"compute-pass-boundary-per-compare-stage\",\"gpuNativeRandomGeneration\":true,\"randomAlgorithm\":\"stateless-u32-hash/seed-particle-index-channel\",\"spawnCatchUp\":\"fixed-60hz/max-512-steps\",\"simulationDispatchesRecorded\":" << vfx_compute_dispatches_ << ",\"spawnDispatchesRecorded\":" << vfx_spawn_dispatches_ << ",\"groupDispatchesRecorded\":" << vfx_group_dispatches_ << ",\"sortDispatchesRecorded\":" << vfx_sort_dispatches_ << ",\"indirectDrawsRecorded\":" << vfx_indirect_draws_ << ",\"simulationReadWriteStorageBuffers\":7,\"spawnReadWriteStorageBuffers\":7,\"groupReadWriteStorageBuffers\":7,\"sortReadWriteStorageBuffers\":3,\"vertexStorageBuffers\":2,\"dispatchActive\":" << (vfx_compute_dispatches_>0?"true":"false") << ",\"compactionActive\":true,\"outputConsumedByDraw\":" << (vfx_indirect_draws_>0?"true":"false") << ",\"drawMode\":\"gpu-blend-grouped/additive-then-alpha/dual-indirect-billboard\",\"abi\":\"structured-particle-gpu-lifecycle-indirect/0.7\"},"
        << "\"evidence\":{\"objectId\":\"render.resource.object-id\",\"depth\":\"render.resource.scene-depth\",\"normal\":\"render.resource.world-normal\",\"motion\":\"render.resource.motion-vectors\",\"reactiveMask\":\"render.resource.reactive-mask\",\"lastPixel\":" << last_pixel_evidence_json() << "},"
        << "\"colorPipeline\":{\"workingSpace\":\"linear-rec709\",\"hdrFormat\":\"RGBA16_FLOAT\",\"toneMapper\":\"ACES-RRT-ODT-fit/matrix\",\"toneMapInput\":\"scene-linear-after-grading\",\"displayGamut\":\"rec709-bounded\",\"outputEncoding\":\"explicit-sRGB-transfer/RGBA8-UNORM\",\"exposureCompensation\":" << exposure_ << ",\"whitePoint\":" << white_point_ << ",\"autoExposure\":{\"enabled\":" << (auto_exposure_enabled_&&!hybrid_pixel_active()?"true":"false") << ",\"mode\":\"" << (auto_exposure_enabled_&&!hybrid_pixel_active()?"8x8-log-average":"locked-unity") << "\",\"historyFormat\":\"R16_FLOAT\",\"historyValid\":" << (exposure_history_valid_?"true":"false") << ",\"minimum\":" << (!auto_exposure_enabled_||hybrid_pixel_active()?1.0F:auto_exposure_min_) << ",\"maximum\":" << (!auto_exposure_enabled_||hybrid_pixel_active()?1.0F:auto_exposure_max_) << ",\"keyValue\":" << (!auto_exposure_enabled_||hybrid_pixel_active()?1.0F:auto_exposure_key_) << ",\"speedUp\":" << (!auto_exposure_enabled_||hybrid_pixel_active()?0.0F:auto_exposure_speed_up_) << ",\"speedDown\":" << (!auto_exposure_enabled_||hybrid_pixel_active()?0.0F:auto_exposure_speed_down_) << "},\"ambientOcclusion\":{\"enabled\":" << (ambient_occlusion_enabled_?"true":"false") << ",\"technique\":\"eight-direction-horizon/separable-bilateral/indirect-only\",\"resolutionDomain\":\"" << (hybrid_pixel_active()?"virtual-grid":"render") << "\",\"resolutionScale\":0.5,\"format\":\"R8_UNORM\",\"denoisePasses\":2,\"radiusPixels\":" << gtao_radius_pixels_ << ",\"intensity\":" << gtao_intensity_ << ",\"bias\":" << gtao_bias_ << ",\"power\":" << gtao_power_ << ",\"depthSigma\":" << gtao_denoise_depth_sigma_ << ",\"normalPower\":" << gtao_denoise_normal_power_ << ",\"workingSetBytes\":" << (static_cast<std::uint64_t>(std::max(1U,render_width_/2U))*std::max(1U,render_height_/2U)*3ULL+static_cast<std::uint64_t>(render_width_)*render_height_*16ULL) << "},\"colorGrading\":{\"model\":\"lift-gamma-gain/saturation/contrast/temperature/tint\",\"order\":\"scene-linear-before-tone-map\",\"saturation\":" << color_saturation_ << ",\"contrast\":" << color_contrast_ << ",\"temperature\":" << color_temperature_ << ",\"tint\":" << color_tint_ << "},\"bloom\":{\"enabled\":true,\"technique\":\"four-level-dual-filter\",\"resolutionDomain\":\"" << (hybrid_pixel_active()?"virtual-grid":"post") << "\",\"levels\":4,\"smallestResolutionScale\":0.0625,\"format\":\"RGBA16_FLOAT\",\"threshold\":" << bloom_threshold_ << ",\"softKnee\":" << bloom_soft_knee_ << ",\"scatter\":" << bloom_scatter_ << ",\"strength\":" << bloom_strength_ << ",\"workingSetBytes\":" << bloom_working_set_bytes_ << "},\"antiAliasing\":\"" << (hybrid_pixel_active()?"spatial-pixel-stable":"TAA") << "\",\"compatibilityAntiAliasing\":\"FXAA\",\"materialMipmaps\":true,\"materialModel\":\"glTF-metallic-roughness/ggx-multiscatter\",\"alphaBlend\":\"sorted-back-to-front/straight-alpha\",\"materialChannels\":[\"baseColor-sRGB\",\"normal-linear\",\"metallicRoughness-linear\",\"occlusion-linear\",\"emissive-sRGB\"],\"ibl\":{\"model\":\"split-sum-ggx-multiscatter\",\"cookVersion\":\"split-sum-ggx/2\",\"sourceAssetId\":\"" << environment_source_id_ << "\",\"sourceFingerprint\":\"" << ibl_source_fingerprint_ << "\",\"sourceType\":\"" << (environment_source_width_?"radiance-hdr":"procedural-hdr") << "\",\"sourceWidth\":" << environment_source_width_ << ",\"sourceHeight\":" << environment_source_height_ << ",\"irradianceResolution\":16,\"prefilteredSpecularResolution\":64,\"prefilteredMipLevels\":7,\"brdfLutResolution\":128,\"format\":\"RGBA16F/RG16F\",\"cacheHit\":" << (ibl_cache_hit_?"true":"false") << ",\"cacheRebuilt\":" << (ibl_cache_rebuilt_?"true":"false") << ",\"cookMicroseconds\":" << ibl_cook_microseconds_ << ",\"artifactBytes\":" << ibl_artifact_bytes_ << ",\"artifactUri\":\"file://" << ibl_artifact_path_ << "\"}},"
        << "\"skyAtmosphere\":{\"pipelineCreated\":" << (sky_atmosphere_pipeline_&&sky_atmosphere_analytic_pipeline_&&aerial_perspective_pipeline_?"true":"false")
        << ",\"lutPipelinesCreated\":" << (sky_transmittance_pipeline_&&sky_multi_scattering_pipeline_&&sky_view_pipeline_&&sky_camera_volume_pipeline_?"true":"false")
        << ",\"path\":\"" << sky_last_path_
        << "\",\"analyticConstantCost\":true,\"viewSamples\":" << (sky_last_path_.starts_with("analytic")?0:8)
        << ",\"lightSamples\":" << (sky_last_path_.starts_with("analytic")?0:4)
        << ",\"lutPathReady\":" << (sky_last_path_=="transmittance/multi-scattering/sky-view/camera-volume"?"true":"false")
        << ",\"lutFallbackReason\":" << nlohmann::json(sky_lut_fallback_reason_).dump()
        << ",\"lutSize\":[" << sky_lut_width_ << ',' << sky_lut_height_ << "]"
        << ",\"cameraVolumeSize\":[" << sky_camera_volume_extent_[0] << ',' << sky_camera_volume_extent_[1] << ',' << sky_camera_volume_extent_[2] << "]"
        << ",\"aerialPerspectiveReady\":" << (!sky_camera_volume_identity_.empty()?"true":"false")
        << ",\"aerialProjectionPolicy\":\"perspective-rays/orthographic-parallel-rays\""
        << ",\"activeProjection\":\"" << (sky_last_camera_orthographic_?"orthographic":"perspective") << "\""
        << ",\"lutRegenerations\":" << sky_lut_regenerations_
        << ",\"mediumLutRegenerations\":" << sky_medium_lut_regenerations_
        << ",\"skyViewLutRegenerations\":" << sky_view_lut_regenerations_
        << ",\"cameraVolumeRegenerations\":" << sky_camera_volume_regenerations_ << ",\"contract\":"
        << sky_atmosphere_canonical_evidence(sky_atmosphere_) << "},"
        << "\"screenSpaceFoundation\":{\"schema\":\"noemancer.screen-space-foundation/0.1\",\"depthPyramid\":{\"ready\":"
        << (depth_pyramid_texture_&&depth_pyramid_seed_pipeline_&&depth_pyramid_reduce_pipeline_?"true":"false")
        << ",\"format\":\"RG32_FLOAT\",\"encoding\":\"linear-view-depth-min-max\",\"baseExtent\":[" << render_width_ << ',' << render_height_
        << "],\"mipCount\":" << depth_pyramid_mip_count_ << ",\"workingSetBytes\":" << depth_pyramid_working_set_bytes_
        << ",\"seedDispatches\":" << depth_pyramid_seed_dispatches_ << ",\"reduceDispatches\":" << depth_pyramid_reduce_dispatches_
        << ",\"oddExtentPolicy\":\"ceil-half-clamped-2x2\",\"consumers\":[\"GTAO-ordering\",\"temporal-denoise-ordering\",\"SSR-production\",\"SSGI-production\"]}"
        << ",\"historyAuthority\":" << temporal_history_evidence.dump()
        << ",\"fallback\":\"history-rejected-current-frame-spatial-resolve\"},"
        << "\"screenSpaceReflections\":" << ssr_evidence.dump() << ','
        << "\"screenSpaceGlobalIllumination\":" << ssgi_evidence.dump() << ','
        << "\"temporal\":{\"mode\":\"" << (hybrid_pixel_active()?"spatial-pixel-stable":"shared-temporal-denoise")
        << "\",\"debugView\":\"" << temporal_debug_mode_name_ << "\",\"debugViews\":[\"final\",\"motion\",\"reactive\",\"disocclusion\",\"history-weight\",\"history-clamp\",\"linear-depth\",\"normal\"],\"upscalingActive\":"
        << (!hybrid_pixel_active()&&render_scale_<0.999F?"true":"false")
        << ",\"motionVectorFormat\":\"RG16_FLOAT\",\"historyFormat\":\"RGBA16_FLOAT\",\"historyDepthFormat\":\"R32_FLOAT\",\"reactiveMaskFormat\":\"R8_UNORM\",\"workingSetBytes\":"
        << static_cast<std::uint64_t>(render_width_)*render_height_*5ULL+static_cast<std::uint64_t>(post_width_)*post_height_*32ULL
        << ",\"historyNormalFormat\":\"RGBA16_FLOAT\",\"historyWeight\":" << (hybrid_pixel_active()?0.0F:0.9F) << ",\"historyValid\":" << (temporal_history_valid_?"true":"false")
        << ",\"historyIndex\":" << taa_history_index_ << ",\"historyResets\":" << taa_history_resets_
        << ",\"previousCamera\":true,\"previousModel\":true,\"previousSkinnedPose\":true,\"projectionJitter\":" << (hybrid_pixel_active()?"false":"true")
        << ",\"jitterSequence\":\"" << (hybrid_pixel_active()?"disabled":"Halton(2,3)/8") << "\",\"jitterSample\":" << temporal_jitter_sample_
        << ",\"jitterPixels\":[" << temporal_jitter_pixels_[0] << ',' << temporal_jitter_pixels_[1] << "],\"jitterNdc\":[" << temporal_jitter_ndc_[0] << ',' << temporal_jitter_ndc_[1]
        << "],\"depthRejection\":{\"relativeThreshold\":0.02,\"minimumWorldThreshold\":0.1},\"historyRejection\":[\"viewport\",\"sky-depth\",\"previous-depth-disocclusion\",\"reactive-mask\",\"luminance-disagreement\",\"motion-magnitude\",\"neighborhood-clamp\"]},"
        << "\"cpuRecordMicroseconds\":{\"render.pass.shadow-depth\":" << shadow_record_microseconds_ << ",\"render.pass.gpu-visibility\":" << gpu_visibility_record_microseconds_ << ",\"render.pass.sky-atmosphere\":" << sky_atmosphere_record_microseconds_ << ",\"render.pass.opaque-lit\":" << opaque_record_microseconds_ << ",\"render.pass.aerial-perspective\":" << aerial_perspective_record_microseconds_ << ",\"render.pass.ambient-occlusion\":" << gtao_record_microseconds_ << ",\"render.pass.ambient-occlusion-denoise\":" << ao_denoise_record_microseconds_ << ",\"render.pass.ambient-occlusion-composite\":" << ao_composite_record_microseconds_ << ",\"render.pass.ssgi-hierarchical-gather\":" << ssgi_gather_record_microseconds_ << ",\"render.pass.ssgi-spatial-resolve\":" << ssgi_spatial_record_microseconds_ << ",\"render.pass.ssgi-temporal-resolve\":" << ssgi_temporal_record_microseconds_ << ",\"render.pass.ssgi-composite\":" << ssgi_composite_record_microseconds_ << ",\"render.pass.ssr-hierarchical-trace\":" << ssr_trace_record_microseconds_ << ",\"render.pass.ssr-temporal-resolve\":" << ssr_temporal_record_microseconds_ << ",\"render.pass.ssr-composite\":" << ssr_composite_record_microseconds_ << ",\"render.pass.transparent-lit\":" << transparent_record_microseconds_ << ",\"render.pass.temporal-resolve\":" << taa_record_microseconds_ << ",\"render.pass.auto-exposure\":" << auto_exposure_record_microseconds_ << ",\"render.pass.bloom-pyramid\":" << bloom_record_microseconds_ << ",\"render.pass.tone-map\":" << tone_map_record_microseconds_ << "},"
        << "\"shadow\":{\"technique\":\"directional-CSM-PCF-3x3\",\"cascadeCount\":4,\"resolutionPerCascade\":2048,\"format\":\"D32_FLOAT\",\"textureBytes\":" << shadow_texture_bytes_
        << ",\"splitLambda\":0.65,\"blendFraction\":0.10,\"maximumDistance\":" << shadow_splits_[3]
        << ",\"splitDistances\":[" << shadow_splits_[0] << ',' << shadow_splits_[1] << ',' << shadow_splits_[2] << ',' << shadow_splits_[3]
        << "],\"stableRadii\":[" << shadow_radii_[0] << ',' << shadow_radii_[1] << ',' << shadow_radii_[2] << ',' << shadow_radii_[3]
        << "],\"worldUnitsPerTexel\":[" << shadow_world_units_per_texel_[0] << ',' << shadow_world_units_per_texel_[1] << ',' << shadow_world_units_per_texel_[2] << ',' << shadow_world_units_per_texel_[3]
        << "],\"instancesPerCascade\":[" << shadow_instances_per_cascade_[0] << ',' << shadow_instances_per_cascade_[1] << ',' << shadow_instances_per_cascade_[2] << ',' << shadow_instances_per_cascade_[3]
        << "],\"drawCallsPerCascade\":[" << shadow_draws_per_cascade_[0] << ',' << shadow_draws_per_cascade_[1] << ',' << shadow_draws_per_cascade_[2] << ',' << shadow_draws_per_cascade_[3]
        << "],\"drawCallsSavedPerCascade\":[" << shadow_draw_calls_saved_per_cascade_[0] << ',' << shadow_draw_calls_saved_per_cascade_[1] << ',' << shadow_draw_calls_saved_per_cascade_[2] << ',' << shadow_draw_calls_saved_per_cascade_[3]
        << "],\"culledPerCascade\":[" << shadow_culled_per_cascade_[0] << ',' << shadow_culled_per_cascade_[1] << ',' << shadow_culled_per_cascade_[2] << ',' << shadow_culled_per_cascade_[3]
        << "],\"cascadesAvailable\":" << (directional_shadow_cascades_rendered_+directional_shadow_cascades_cached_)
        << ",\"cascadesRendered\":" << directional_shadow_cascades_rendered_ << ",\"cascadesCached\":" << directional_shadow_cascades_cached_
        << ",\"cacheHitsTotal\":" << directional_shadow_cache_hits_ << ",\"cacheMissesTotal\":" << directional_shadow_cache_misses_
        << ",\"avoidedInstancesEstimate\":" << directional_shadow_avoided_instances_ << ",\"avoidedDrawsEstimate\":" << directional_shadow_avoided_draws_
        << ",\"cachePolicy\":\"per-cascade-stable-matrix-visible-caster-transform-cutout-skin-fingerprint\""
        << ",\"invalidation\":[\"target-recreation\",\"camera-or-light-matrix-change\",\"visible-caster-change\"]"
        << ",\"instances\":" << shadow_instances_submitted_ << ",\"drawCalls\":" << shadow_caster_draws_ << ",\"drawCallsSaved\":" << shadow_draw_calls_saved_ << "},"
        << "\"localShadow\":{\"enabled\":" << (local_shadow_selected_lights_>0?"true":"false")
        << ",\"technique\":\"bounded-point-cubefaces-and-spot-array-PCF-3x3\",\"quality\":\"" << shadow_quality_
        << "\",\"resolutionPerLayer\":" << local_shadow_resolution_
        << ",\"maximumPointLights\":" << maximum_shadowed_point_lights_ << ",\"maximumSpotLights\":" << maximum_shadowed_spot_lights_
        << ",\"layerCapacity\":" << local_shadow_layer_count << ",\"requestedLights\":" << local_shadow_requested_lights_
        << ",\"selectedLights\":" << local_shadow_selected_lights_ << ",\"droppedLights\":" << local_shadow_dropped_lights_
        << ",\"pointLights\":" << local_shadow_point_lights_ << ",\"spotLights\":" << local_shadow_spot_lights_
        << ",\"facesAvailable\":" << (local_shadow_faces_rendered_+local_shadow_faces_cached_)
        << ",\"facesRendered\":" << local_shadow_faces_rendered_ << ",\"facesCached\":" << local_shadow_faces_cached_
        << ",\"cacheHitsTotal\":" << local_shadow_cache_hits_ << ",\"cacheMissesTotal\":" << local_shadow_cache_misses_
        << ",\"avoidedInstancesEstimate\":" << local_shadow_avoided_instances_ << ",\"avoidedDrawsEstimate\":" << local_shadow_avoided_draws_
        << ",\"instances\":" << local_shadow_instances_submitted_
        << ",\"drawCalls\":" << local_shadow_draw_calls_ << ",\"drawCallsSaved\":" << local_shadow_draw_calls_saved_
        << ",\"culledDraws\":" << local_shadow_culled_draws_ << ",\"textureBytes\":" << local_shadow_texture_bytes_
        << ",\"selectedLightIds\":" << nlohmann::json(local_shadow_selected_ids_).dump()
        << ",\"selection\":\"camera-weighted-luminous-influence/stable-id-tie-break/kind-budgets\""
        << ",\"cachePolicy\":\"per-layer-stable-light-face/matrix-visible-caster-transform-cutout-skin-fingerprint\""
        << ",\"invalidation\":[\"target-recreation\",\"quality-change\",\"light-face-change\",\"visible-caster-change\"]"
        << ",\"format\":\"D32_FLOAT\"},"
        << "\"shadowScalability\":" << shadow_scalability_evidence.dump() << ','
        << "\"textureResidency\":{\"schemaVersion\":\"noemancer.texture-residency/0.3\",\"ktxTextures\":" << ktx_textures_uploaded_
        << ",\"nativeCompressedTextures\":" << ktx_native_compressed_textures_
        << ",\"rgba8FallbackTextures\":" << ktx_rgba8_fallback_textures_
        << ",\"authoredMipLevelsTotal\":" << authored_mip_levels_total << ",\"authoredMipLevelsUploaded\":" << ktx_mip_levels_uploaded_
        << ",\"sourceArtifactBytes\":" << ktx_source_bytes_ << ",\"gpuAllocationBytesEstimate\":" << ktx_resident_bytes_
        << ",\"fullChainBytesEstimate\":" << std::accumulate(texture_streams_.begin(),texture_streams_.end(),std::uint64_t{},
            [](const std::uint64_t total,const RuntimeTextureStream& stream){return total+stream.full_chain_bytes;})
        << ",\"initialTailBytes\":" << ktx_tail_bytes_ << ",\"alignedStagingBytes\":" << ktx_staging_bytes_
        << ",\"formatSelection\":\"device-query/BC7-then-RGBA8\",\"uploadOrder\":\"smallest-mip-first/tail-then-detail\""
        << ",\"maximumInitialTailLevels\":4,\"visibility\":\"physical-mip-tier/rebased-source-level-at-gpu-level-zero\""
        << ",\"asyncFrameStreaming\":true,\"scheduler\":\"screen-footprint/importance/priority/visibility-aging/hysteresis/resident-budget\""
        << ",\"workload\":\"" << texture_streaming_workload_ << "\""
        << ",\"budgetBytesPerFrame\":" << texture_streaming_budget_bytes_
        << ",\"residentBudgetBytes\":" << texture_streaming_resident_budget_bytes_
        << ",\"demandBytes\":" << texture_streaming_demand_bytes_ << ",\"plannedBytes\":" << texture_streaming_planned_bytes_
        << ",\"overBudget\":" << (texture_streaming_over_budget_?"true":"false") << ",\"planCode\":\"" << texture_streaming_plan_code_ << "\""
        << ",\"uploadedSourceBytesThisFrame\":" << texture_streaming_bytes_this_frame_
        << ",\"uploadedCopyBytesThisFrame\":" << texture_streaming_copy_bytes_this_frame_
        << ",\"uploadedSourceBytesTotal\":" << texture_streaming_bytes_total_
        << ",\"uploadedCopyBytesTotal\":" << texture_streaming_copy_bytes_total_
        << ",\"transitionsThisFrame\":" << texture_streaming_levels_this_frame_
        << ",\"upgradesThisFrame\":" << texture_streaming_upgrades_this_frame_
        << ",\"downgradesThisFrame\":" << texture_streaming_downgrades_this_frame_
        << ",\"bytesReleasedThisFrameEstimate\":" << texture_streaming_bytes_released_this_frame_
        << ",\"bytesReleasedTotalEstimate\":" << texture_streaming_bytes_released_total_
        << ",\"evictionsTotal\":" << texture_streaming_evictions_total_ << ",\"reuploadsTotal\":" << texture_streaming_reuploads_total_
        << ",\"pendingLevels\":" << texture_streaming_pending_levels_
        << ",\"completedStreams\":" << texture_streaming_completed_streams_
        << ",\"physicalTelemetry\":{\"available\":false,\"reason\":\"SDL_GPU does not expose backend memory budget telemetry\"}"
        << ",\"streams\":" << stream_states.dump() << "},"
        << "\"textureResources\":" << texture_resources_.observe_json() << ","
        << "\"sprites\":{\"pipelineCreated\":" << (sprite_cutout_pipeline_&&sprite_alpha_pipeline_?"true":"false")
        << ",\"textureCount\":" << sprite_textures_uploaded_ << ",\"instancesSubmitted\":" << sprite_instances_submitted_
        << ",\"drawsSubmitted\":" << sprite_draws_submitted_ << ",\"drawsSaved\":" << sprite_draws_saved_
        << ",\"atlasRuntime\":{\"schemaVersion\":\"noemancer.sprite-atlas-runtime/0.1\",\"manifestsDiscovered\":"
        << sprite_atlas_manifests_discovered_ << ",\"manifestsValid\":" << sprite_atlas_manifests_valid_
        << ",\"manifestsInvalid\":" << sprite_atlas_manifests_invalid_
        << ",\"declaredPageAssets\":" << sprite_atlas_declared_page_assets_
        << ",\"uniquePageAssets\":" << sprite_atlas_unique_page_assets_
        << ",\"pageTexturesUploaded\":" << sprite_atlas_page_textures_uploaded_
        << ",\"pageTexturesMissing\":" << sprite_atlas_page_textures_missing_
        << ",\"pageTexturesAvailable\":" << sprite_atlas_page_textures_available_
        << ",\"countsTruncated\":" << (sprite_atlas_counts_truncated_?"true":"false")
        << ",\"sourceSpriteCompatibility\":{\"authoring\":\"single-texture-sprite-asset\",\"overlay\":\"runtime-only-page-binding\",\"fallback\":\"source-texture-asset\"}"
        << ",\"scope\":\"validated-manifest/page-asset/resource-table-counts; no-GPU-timing-claim\"}"
        << ",\"maximumInstancesPerDraw\":" << sprite_instance_capacity
        << ",\"instanceStorage\":{\"path\":\"graphics-storage-buffer\",\"capacity\":" << sprite_instance_capacity
        << ",\"allocated\":" << (sprite_instance_buffer_&&sprite_instance_upload_?"true":"false")
        << ",\"uploaded\":" << sprite_instances_uploaded_ << ",\"dropped\":" << sprite_instances_dropped_
        << ",\"reused\":" << sprite_instances_reused_ << ",\"dirtyRanges\":" << sprite_instance_dirty_ranges_
        << ",\"uploadBytes\":" << sprite_instance_upload_bytes_ << ",\"uploadBytesTotal\":" << sprite_instance_upload_bytes_total_
        << ",\"uploadPolicy\":\"stable-range-arena/contiguous-dirty-ranges\""
        << ",\"rangeAllocator\":{\"highWater\":" << sprite_range_allocator_.statistics().high_water
        << ",\"liveRanges\":" << sprite_range_allocator_.statistics().live_ranges
        << ",\"liveSlots\":" << sprite_range_allocator_.statistics().live_slots
        << ",\"freeSlots\":" << sprite_range_allocator_.statistics().free_slots
        << ",\"largestFreeRange\":" << sprite_range_allocator_.statistics().largest_free_range
        << ",\"moves\":" << sprite_range_allocator_.statistics().moves
        << ",\"evictions\":" << sprite_range_allocator_.statistics().evictions
        << ",\"evictedThisFrame\":" << sprite_stable_range_evictions_ << ",\"retentionFrames\":120}"
        << ",\"drawIndirection\":{\"path\":\"uint32-graphics-storage-buffer\",\"allocated\":"
        << (sprite_draw_index_buffer_&&sprite_draw_index_upload_?"true":"false")
        << ",\"uploaded\":" << sprite_draw_indices_uploaded_ << ",\"dirtyRanges\":" << sprite_draw_index_dirty_ranges_
        << ",\"uploadBytes\":" << sprite_draw_index_upload_bytes_ << ",\"uploadBytesTotal\":" << sprite_draw_index_upload_bytes_total_ << "}}"
        << ",\"missingTextureDraws\":" << sprite_draws_missing_texture_ << ",\"missingMaterialTextures\":" << sprite_material_textures_missing_
        << ",\"litInstances\":" << sprite_lit_instances_ << ",\"unlitInstances\":" << sprite_unlit_instances_
        << ",\"shadowReceivers\":" << sprite_shadow_receivers_ << ",\"authoredShadowCasters\":" << sprite_shadow_casters_
        << ",\"materialChannels\":[\"normal\",\"emissive-mask\",\"height-depth\",\"metallic\",\"roughness\"]"
        << ",\"lightingPath\":\"shared-directional/clustered-point-spot/shared-shadow-resources\""
        << ",\"shader\":\"sprite\",\"modes\":[\"cutout\",\"alpha\"],\"gpuPicking\":true,\"motionVectors\":true},"
        << "\"tilemaps\":{\"count\":" << tilemap_count_ << ",\"cellInstancesRequested\":" << tile_cell_instances_requested_
        << ",\"cellInstancesSubmitted\":" << tile_cell_instances_submitted_
        << ",\"visibleChunks\":" << tilemap_visible_chunks_ << ",\"culledChunks\":" << tilemap_culled_chunks_
        << ",\"earlyVisibility\":{\"applied\":" << (tilemap_early_visibility_applied_?"true":"false")
        << ",\"chunksResolved\":" << tilemap_chunks_resolved_ << ",\"chunksSkippedBeforeResolution\":" << tilemap_chunks_skipped_before_resolution_
        << ",\"cellsSkippedBeforeResolution\":" << tilemap_cells_skipped_before_resolution_ << "}"
        << ",\"instanceRanges\":{\"count\":" << tilemap_chunk_ranges_ << ",\"largestCellCount\":" << tilemap_largest_chunk_range_
        << ",\"layout\":\"stable-power-of-two-gpu-range/draw-index-indirection\"}"
        << ",\"bakeCache\":{\"hits\":" << tilemap_bake_cache_hits_ << ",\"rebuilds\":" << tilemap_bake_cache_rebuilds_
        << ",\"evictions\":" << tilemap_bake_cache_evictions_ << ",\"cachedChunks\":" << tilemap_bake_cached_chunks_
        << ",\"retainedOffscreenChunks\":" << tilemap_bake_retained_offscreen_chunks_ << ",\"offscreenRetentionFrames\":120"
        << ",\"invalidation\":\"chunk-content/sprite-frame/entity-transform/render-visibility\"}"
        << ",\"renderPath\":\"sprite-material/object-id/instancing\",\"physicsPath\":\"merged-static-jolt\"},"
        << "\"submission\":{\"opaqueInstances\":" << opaque_instances_submitted_ << ",\"vfxParticles\":" << vfx_particles_submitted_ << ",\"opaqueDrawCalls\":" << opaque_draw_calls_
        << ",\"opaqueDrawCallsSaved\":" << opaque_draw_calls_saved_ << ",\"maximumInstancesPerDraw\":" << maximum_instances_per_draw
        << ",\"gpuDriven\":{\"enabled\":" << (gpu_driven_enabled_&&gpu_visibility_pipeline_&&gpu_driven_lit_pipeline_&&gpu_driven_lit_double_sided_pipeline_&&gpu_driven_instance_buffer_&&gpu_driven_batch_buffer_&&gpu_driven_visible_index_buffer_&&gpu_driven_indirect_buffer_&&gpu_driven_upload_buffer_?"true":"false")
        << ",\"available\":" << (gpu_visibility_pipeline_&&gpu_driven_lit_pipeline_&&gpu_driven_lit_double_sided_pipeline_&&gpu_driven_instance_buffer_&&gpu_driven_batch_buffer_&&gpu_driven_visible_index_buffer_&&gpu_driven_indirect_buffer_&&gpu_driven_upload_buffer_?"true":"false")
        << ",\"path\":\"compute-frustum-compact/indexed-indirect\",\"candidateCapacity\":" << gpu_driven_instance_capacity
        << ",\"batchCapacity\":" << gpu_driven_batch_capacity << ",\"minimumBatchSize\":" << gpu_driven_minimum_batch_size
        << ",\"candidates\":" << gpu_driven_candidates_ << ",\"batches\":" << gpu_driven_batches_
        << ",\"cpuReferenceVisible\":" << gpu_driven_reference_visible_ << ",\"fallbackInstances\":" << gpu_driven_fallback_instances_
        << ",\"uploadBytesThisFrame\":" << gpu_driven_upload_bytes_ << ",\"dispatchesRecordedTotal\":" << gpu_driven_dispatches_
        << ",\"instanceUploadsTotal\":" << gpu_driven_instance_uploads_
        << ",\"indirectDrawsRecordedTotal\":" << gpu_driven_indirect_draws_
        << ",\"fallback\":\"direct-cpu-cull/16-instance-uniform-for-skinned-transparent-small-or-overflow\""
        << ",\"lastRuntimeFallback\":" << (gpu_driven_fallback_reason_.empty()?"null":nlohmann::json(gpu_driven_fallback_reason_).dump())
        << ",\"stableBatching\":{\"schemaVersion\":\"noemancer.gpu-stable-batching/0.1\",\"enabled\":"
        << (gpu_driven_enabled_&&gpu_visibility_pipeline_&&gpu_driven_lit_pipeline_&&gpu_driven_lit_double_sided_pipeline_&&gpu_driven_instance_buffer_&&gpu_driven_batch_buffer_&&gpu_driven_visible_index_buffer_&&gpu_driven_indirect_buffer_&&gpu_driven_upload_buffer_?"true":"false")
        << ",\"topologyReused\":" << (gpu_driven_topology_reused_?"true":"false")
        << ",\"topologyChanged\":" << (gpu_driven_candidates_>0&&!gpu_driven_topology_reused_?"true":"false")
        << ",\"topologyRebuildsTotal\":" << gpu_driven_topology_rebuilds_
        << ",\"topologyReusesTotal\":" << gpu_driven_topology_reuses_
        << ",\"stableSlotsReused\":" << gpu_driven_stable_slots_reused_ << ",\"movedSlots\":" << gpu_driven_moved_slots_
        << ",\"dirtyRanges\":" << gpu_driven_dirty_ranges_ << ",\"dirtyInstances\":" << gpu_driven_dirty_instances_
        << ",\"instanceUploadBytes\":" << gpu_driven_instance_upload_bytes_
        << ",\"batchUploadBytes\":" << gpu_driven_batch_upload_bytes_
        << ",\"commandUploadBytes\":" << gpu_driven_command_upload_bytes_
        << ",\"uploadBytes\":" << (gpu_driven_instance_upload_bytes_+gpu_driven_batch_upload_bytes_)
        << ",\"uploadBytesTotal\":" << gpu_driven_stable_upload_bytes_total_
        << ",\"uploadPolicy\":\"exact-linear-dirty-ranges/no-cycle-for-partial/full-cycle-for-rebuild\"}"
        << ",\"occlusion\":{\"schemaVersion\":\"noemancer.gpu-occlusion-runtime/0.1\",\"requested\":"
        << (gpu_occlusion_enabled_?"true":"false") << ",\"available\":"
        << (gpu_occlusion_pipeline_&&gpu_occlusion_statistics_buffer_&&depth_pyramid_texture_?"true":"false")
        << ",\"usedThisFrame\":" << (gpu_occlusion_used_this_frame_?"true":"false")
        << ",\"historyValid\":" << (gpu_occlusion_history_valid_?"true":"false")
        << ",\"source\":\"previous-frame-shared-rg32f-min-max-hiz\",\"strategy\":\"projected-sphere-conservative-max-depth\""
        << ",\"eligibility\":\"static-opaque-unskinned-perspective-and-stationary-camera\""
        << ",\"fallbackReason\":" << nlohmann::json(gpu_occlusion_fallback_reason_).dump()
        << ",\"statisticsSource\":\"explicit-one-shot-readback\",\"statisticsFrame\":" << gpu_visibility_readback_frame_
        << ",\"statistics\":{\"candidates\":" << gpu_occlusion_readback_statistics_[0]
        << ",\"frustumCulled\":" << gpu_occlusion_readback_statistics_[1]
        << ",\"hizTested\":" << gpu_occlusion_readback_statistics_[2]
        << ",\"hizCulled\":" << gpu_occlusion_readback_statistics_[3]
        << ",\"uncertainVisible\":" << gpu_occlusion_readback_statistics_[4]
        << ",\"offscreenVisible\":" << gpu_occlusion_readback_statistics_[5]
        << ",\"disabledVisible\":" << gpu_occlusion_readback_statistics_[6]
        << ",\"acceptedVisible\":" << gpu_occlusion_readback_statistics_[7] << "}}"
        << ",\"readback\":{\"state\":" << nlohmann::json(gpu_visibility_readback_state_).dump()
        << ",\"frame\":" << gpu_visibility_readback_frame_ << ",\"candidates\":" << gpu_visibility_readback_candidates_
        << ",\"batches\":" << gpu_visibility_readback_batches_ << ",\"cpuReferenceVisible\":" << gpu_visibility_readback_cpu_visible_
        << ",\"gpuVisible\":" << gpu_visibility_readback_gpu_visible_ << ",\"invalidBatchCounts\":" << gpu_visibility_readback_invalid_batches_
        << ",\"mismatchedBatchCounts\":" << gpu_visibility_readback_mismatched_batch_counts_
        << ",\"outOfRangeIndices\":" << gpu_visibility_readback_out_of_range_indices_
        << ",\"wrongBatchIndices\":" << gpu_visibility_readback_wrong_batch_indices_
        << ",\"duplicateIndices\":" << gpu_visibility_readback_duplicate_indices_
        << ",\"unexpectedVisibleIndices\":" << gpu_visibility_readback_unexpected_visible_
        << ",\"occlusionActive\":" << (gpu_visibility_readback_occlusion_active_?"true":"false")
        << ",\"cpuSetHash\":" << (gpu_visibility_readback_cpu_set_hash_.empty()?"null":nlohmann::json(gpu_visibility_readback_cpu_set_hash_).dump())
        << ",\"gpuSetHash\":" << (gpu_visibility_readback_gpu_set_hash_.empty()?"null":nlohmann::json(gpu_visibility_readback_gpu_set_hash_).dump())
        << ",\"indexSets\":{\"ordering\":\"batch-then-ascending-u32\",\"hashAlgorithm\":\"fnv1a64/le-u64-batch-and-count/le-u32-index\",\"cpu\":"
        << nlohmann::json(gpu_visibility_readback_expected_indices_).dump() << ",\"gpu\":"
        << nlohmann::json(gpu_visibility_readback_actual_indices_).dump() << "}"
        << ",\"actualDrawIds\":" << nlohmann::json(gpu_visibility_readback_actual_draw_ids_).dump()
        << ",\"countMatch\":" << (gpu_visibility_readback_count_match_?"true":"false")
        << ",\"exactSetMatch\":" << (gpu_visibility_readback_exact_set_match_?"true":"false")
        << ",\"conservativeSubsetMatch\":" << (gpu_visibility_readback_conservative_subset_match_?"true":"false")
        << ",\"transferBytes\":" << gpu_visibility_readback_bytes_ << ",\"match\":" << (gpu_visibility_readback_match_?"true":"false")
        << ",\"error\":" << (gpu_visibility_readback_error_.empty()?"null":nlohmann::json(gpu_visibility_readback_error_).dump())
        << ",\"synchronization\":\"same-command-buffer-command-index-and-optional-statistics-copy/fenced-one-shot-submit\",\"includedInPerformanceSample\":false,\"abi\":\"noemancer.gpu-visibility-readback/0.3\"}"
        << ",\"abi\":\"noemancer.gpu-driven-static-opaque/0.4\"}"
        << ",\"skinning\":{\"renderInstances\":" << skinned_render_instances_ << ",\"drawItems\":" << skinned_draw_items_
        << ",\"jointMatrices\":" << skinning_joint_matrices_ << ",\"paletteCapacity\":" << SkeletalPose::maximum_joints
        << ",\"poseSource\":\"ozz-render-extract-once-per-frame\"}"
        << ",\"eligibility\":\"opaque-unskinned-identical-geometry-material-state\"},"
        << "\"clusteredLighting\":{\"enabled\":true,\"assignment\":\"cpu-conservative-sphere\",\"consumption\":\"gpu-forward-pbr\""
        << ",\"grid\":[" << clustered_lighting_config_.tiles_x << ',' << clustered_lighting_config_.tiles_y << ',' << clustered_lighting_config_.depth_slices << ']'
        << ",\"depthSlices\":\"logarithmic\",\"maximumLights\":" << clustered_lighting_config_.maximum_lights
        << ",\"maximumLightsPerCluster\":" << clustered_lighting_config_.maximum_lights_per_cluster
        << ",\"submittedLights\":" << local_lights_submitted_ << ",\"droppedLights\":" << local_lights_dropped_
        << ",\"clusterAssignments\":" << light_cluster_assignments_ << ",\"overflowedAssignments\":" << light_cluster_overflows_
        << ",\"uploadBytesTotal\":" << clustered_lighting_upload_bytes_ << ",\"intensityUnit\":\"lumen-authoring/candela-gpu\""
        << ",\"localLightShadows\":" << (local_shadow_selected_lights_>0?"true":"false")
        << ",\"abi\":\"noemancer.clustered-forward/0.2\"},"
        << "\"sceneSource\":\"ecs\",\"activeCameraId\":\"" << active_camera_id_
        << "\",\"activeLightId\":\"" << active_light_id_ << "\",\"visibleRenderables\":" << visible_renderables_
        << ",\"visibleDraws\":" << visible_draws_ << ",\"cameraCulledDraws\":" << camera_culled_draws_ << ",\"shadowCasters\":" << shadow_casters_
        << ",\"shadowCasterDraws\":" << shadow_instances_submitted_ << ",\"importedGpuMeshes\":" << imported_meshes_
        << ",\"importedPrimitives\":" << imported_primitives_ << ",\"importedTextures\":" << imported_textures_
        << ",\"geometryLoading\":{\"cookedArtifactLoads\":" << cooked_geometry_loads_
        << ",\"sourceAssetDecodes\":" << source_geometry_decodes_
        << ",\"offlineCompiles\":0}"
        << ",\"materialFeatureCounts\":{\"normalMapped\":" << normal_mapped_primitives_
        << ",\"metallicRoughnessMapped\":" << metallic_roughness_mapped_primitives_
        << ",\"occlusionMapped\":" << occlusion_mapped_primitives_ << ",\"emissiveMapped\":" << emissive_mapped_primitives_
        << ",\"alphaMasked\":" << alpha_masked_primitives_ << ",\"alphaBlended\":" << alpha_blended_primitives_
        << ",\"doubleSided\":" << double_sided_primitives_ << "}}";
    return out.str();
}

void SceneRenderer::release_targets() {
    const auto temporal_state=temporal_history_authority_.state(TemporalHistoryConsumer::taa);
    if(temporal_state.current_valid) {
        const auto reset=temporal_history_authority_.reset({TemporalHistoryConsumer::taa,temporal_state.revision,
            temporal_history_reset_mask(TemporalHistoryResetReason::output_invalid)});
        if(reset.success)taa_history_resets_=temporal_history_authority_.state(TemporalHistoryConsumer::taa).reset_count;
    }
    const auto ssr_state=temporal_history_authority_.state(TemporalHistoryConsumer::ssr);
    if(ssr_state.current_valid) {
        (void)temporal_history_authority_.reset({TemporalHistoryConsumer::ssr,ssr_state.revision,
            temporal_history_reset_mask(TemporalHistoryResetReason::output_invalid)});
    }
    const auto ssgi_state=temporal_history_authority_.state(TemporalHistoryConsumer::ssgi);
    if(ssgi_state.current_valid) {
        (void)temporal_history_authority_.reset({TemporalHistoryConsumer::ssgi,ssgi_state.revision,
            temporal_history_reset_mask(TemporalHistoryResetReason::output_invalid)});
    }
    if (color_texture_) SDL_ReleaseGPUTexture(device_,color_texture_);
    if (hdr_texture_) SDL_ReleaseGPUTexture(device_,hdr_texture_);
    if(aerial_hdr_texture_)SDL_ReleaseGPUTexture(device_,aerial_hdr_texture_);
    if(ao_composited_hdr_texture_)SDL_ReleaseGPUTexture(device_,ao_composited_hdr_texture_);
    if(indirect_lighting_texture_)SDL_ReleaseGPUTexture(device_,indirect_lighting_texture_);
    if(specular_indirect_texture_)SDL_ReleaseGPUTexture(device_,specular_indirect_texture_);
    if(reflection_properties_texture_)SDL_ReleaseGPUTexture(device_,reflection_properties_texture_);
    if(ssr_raw_texture_)SDL_ReleaseGPUTexture(device_,ssr_raw_texture_);
    if(ssr_resolved_texture_)SDL_ReleaseGPUTexture(device_,ssr_resolved_texture_);
    for(auto* history:ssr_history_textures_)if(history)SDL_ReleaseGPUTexture(device_,history);
    if(ssgi_raw_texture_)SDL_ReleaseGPUTexture(device_,ssgi_raw_texture_);
    if(ssgi_raw_bent_normal_texture_)SDL_ReleaseGPUTexture(device_,ssgi_raw_bent_normal_texture_);
    if(ssgi_spatial_texture_)SDL_ReleaseGPUTexture(device_,ssgi_spatial_texture_);
    if(ssgi_spatial_bent_normal_texture_)SDL_ReleaseGPUTexture(device_,ssgi_spatial_bent_normal_texture_);
    if(ssgi_bent_normal_texture_)SDL_ReleaseGPUTexture(device_,ssgi_bent_normal_texture_);
    if(ssgi_resolved_texture_)SDL_ReleaseGPUTexture(device_,ssgi_resolved_texture_);
    for(auto* history:ssgi_history_textures_)if(history)SDL_ReleaseGPUTexture(device_,history);
    for(auto* history:ssgi_bent_normal_history_textures_)if(history)SDL_ReleaseGPUTexture(device_,history);
    if(ssgi_composited_hdr_texture_)SDL_ReleaseGPUTexture(device_,ssgi_composited_hdr_texture_);
    if(reflected_hdr_texture_)SDL_ReleaseGPUTexture(device_,reflected_hdr_texture_);
    if (tone_mapped_texture_) SDL_ReleaseGPUTexture(device_,tone_mapped_texture_);
    if (object_id_texture_) SDL_ReleaseGPUTexture(device_,object_id_texture_);
    if (normal_texture_) SDL_ReleaseGPUTexture(device_,normal_texture_);
    if (motion_texture_) SDL_ReleaseGPUTexture(device_,motion_texture_);
    if (reactive_mask_texture_) SDL_ReleaseGPUTexture(device_,reactive_mask_texture_);
    if (taa_resolved_texture_) SDL_ReleaseGPUTexture(device_,taa_resolved_texture_);
    for(auto* texture:bloom_downsample_textures_)if(texture)SDL_ReleaseGPUTexture(device_,texture);
    for(auto* texture:bloom_upsample_textures_)if(texture)SDL_ReleaseGPUTexture(device_,texture);
    if (ambient_occlusion_texture_) SDL_ReleaseGPUTexture(device_,ambient_occlusion_texture_);
    if(ambient_occlusion_temp_texture_)SDL_ReleaseGPUTexture(device_,ambient_occlusion_temp_texture_);
    if(ambient_occlusion_filtered_texture_)SDL_ReleaseGPUTexture(device_,ambient_occlusion_filtered_texture_);
    for (auto* history:exposure_history_textures_) if (history) SDL_ReleaseGPUTexture(device_,history);
    for (auto* history:taa_history_textures_) if (history) SDL_ReleaseGPUTexture(device_,history);
    for (auto* history:taa_history_depth_textures_) if (history) SDL_ReleaseGPUTexture(device_,history);
    for(auto* history:temporal_history_normal_textures_)if(history)SDL_ReleaseGPUTexture(device_,history);
    if (depth_texture_) SDL_ReleaseGPUTexture(device_,depth_texture_);
    if(depth_pyramid_texture_)SDL_ReleaseGPUTexture(device_,depth_pyramid_texture_);
    if (shadow_texture_) SDL_ReleaseGPUTexture(device_,shadow_texture_);
    if (local_shadow_texture_) SDL_ReleaseGPUTexture(device_,local_shadow_texture_);
    color_texture_=nullptr;hdr_texture_=nullptr;aerial_hdr_texture_=nullptr;ao_composited_hdr_texture_=nullptr;indirect_lighting_texture_=nullptr;
    specular_indirect_texture_=nullptr;reflection_properties_texture_=nullptr;ssr_raw_texture_=nullptr;
    ssr_resolved_texture_=nullptr;ssr_history_textures_.fill(nullptr);reflected_hdr_texture_=nullptr;
    ssgi_raw_texture_=nullptr;ssgi_raw_bent_normal_texture_=nullptr;ssgi_spatial_texture_=nullptr;
    ssgi_spatial_bent_normal_texture_=nullptr;ssgi_bent_normal_texture_=nullptr;
    ssgi_resolved_texture_=nullptr;ssgi_history_textures_.fill(nullptr);
    ssgi_bent_normal_history_textures_.fill(nullptr);ssgi_composited_hdr_texture_=nullptr;
    tone_mapped_texture_=nullptr;object_id_texture_=nullptr;normal_texture_=nullptr;
    motion_texture_=nullptr;reactive_mask_texture_=nullptr;taa_resolved_texture_=nullptr;ambient_occlusion_texture_=nullptr;
    ambient_occlusion_temp_texture_=nullptr;ambient_occlusion_filtered_texture_=nullptr;
    bloom_downsample_textures_.fill(nullptr);bloom_upsample_textures_.fill(nullptr);bloom_working_set_bytes_=0;
    exposure_history_textures_.fill(nullptr); taa_history_textures_.fill(nullptr);
    taa_history_depth_textures_.fill(nullptr);
    temporal_history_normal_textures_.fill(nullptr);
    depth_texture_=nullptr;depth_pyramid_texture_=nullptr;shadow_texture_=nullptr;local_shadow_texture_=nullptr;
    depth_pyramid_mip_count_=0U;depth_pyramid_working_set_bytes_=0U;
    width_=height_=render_width_=render_height_=post_width_=post_height_=0;
    pixel_presentation_={};
    temporal_history_valid_=false;ssr_history_valid_=false;ssgi_history_valid_=false;
    taa_history_index_=0;ssr_history_index_=0;ssgi_history_index_=0;
    exposure_history_valid_=false; exposure_history_index_=0;
    previous_models_.clear(); previous_skinning_matrices_.clear();
    directional_shadow_cascade_cache_valid_.fill(false);
    local_shadow_face_cache_valid_.fill(false);
}

void SceneRenderer::release() {
    release_targets();
    scene_raytracing_bridge_.reset();
    release_sdl_gpu_native_rt_texture(device_,native_rt_texture_export_);
    raytracing_geometry_cache_.clear();
    raytracing_geometries_.clear();
    native_raytracing_status_json_.clear();
    for (auto& [id, mesh] : gpu_meshes_) {
        static_cast<void>(id);
        for (const auto handle : mesh.textures_srgb) if(handle.valid())
            if(auto* texture=texture_resources_.remove(handle))SDL_ReleaseGPUTexture(device_,texture);
        for (const auto handle : mesh.textures_linear) if(handle.valid())
            if(auto* texture=texture_resources_.remove(handle))SDL_ReleaseGPUTexture(device_,texture);
        if (mesh.index_buffer) SDL_ReleaseGPUBuffer(device_,mesh.index_buffer);
        if (mesh.vertex_buffer) SDL_ReleaseGPUBuffer(device_,mesh.vertex_buffer);
    }
    gpu_meshes_.clear(); imported_meshes_=0; imported_primitives_=0; imported_textures_=0;
    const auto streamed=[&](const TextureResourceHandle handle) {
        return std::ranges::find(texture_stream_handles_,handle)!=texture_stream_handles_.end();
    };
    for(auto& [id,handle]:sprite_textures_){static_cast<void>(id);if(handle.valid()&&!streamed(handle))
        if(auto* texture=texture_resources_.remove(handle))SDL_ReleaseGPUTexture(device_,texture);}
    for(auto& [id,handle]:sprite_linear_textures_){static_cast<void>(id);if(handle.valid()&&!streamed(handle))
        if(auto* texture=texture_resources_.remove(handle))SDL_ReleaseGPUTexture(device_,texture);}
    for(std::size_t index=0;index<texture_streams_.size();++index) {
        if(index<texture_stream_handles_.size()) {
            if(texture_streams_[index].transition_pending)
                static_cast<void>(texture_resources_.rollback_replacement(texture_stream_handles_[index]));
            static_cast<void>(texture_resources_.remove(texture_stream_handles_[index]));
        }
        release_texture_stream(device_,texture_streams_[index]);
    }
    texture_streams_.clear();texture_stream_handles_.clear();texture_stream_lookup_.clear();
    sprite_textures_.clear();sprite_linear_textures_.clear();sprite_textures_uploaded_=0;sprite_draws_submitted_=0;sprite_instances_submitted_=0;
    ktx_textures_uploaded_=0;ktx_native_compressed_textures_=0;ktx_rgba8_fallback_textures_=0;ktx_mip_levels_uploaded_=0;
    ktx_source_bytes_=0;ktx_resident_bytes_=0;ktx_tail_bytes_=0;ktx_staging_bytes_=0;
    texture_streaming_bytes_this_frame_=0;texture_streaming_bytes_total_=0;texture_streaming_copy_bytes_total_=0;
    texture_streaming_levels_this_frame_=0;texture_streaming_pending_levels_=0;texture_streaming_completed_streams_=0;
    texture_streaming_cursor_=0;
    sprite_draws_saved_=0;sprite_draws_missing_texture_=0;sprite_material_textures_missing_=0;
    sprite_atlas_manifests_discovered_=0;sprite_atlas_manifests_valid_=0;sprite_atlas_manifests_invalid_=0;
    sprite_atlas_declared_page_assets_=0;sprite_atlas_unique_page_assets_=0;
    sprite_atlas_page_textures_uploaded_=0;sprite_atlas_page_textures_missing_=0;
    sprite_atlas_page_textures_available_=0;sprite_atlas_counts_truncated_=false;
    sprite_lit_instances_=0;sprite_unlit_instances_=0;sprite_shadow_receivers_=0;sprite_shadow_casters_=0;
    sprite_instances_uploaded_=0;sprite_instances_dropped_=0;sprite_instance_upload_bytes_=0;sprite_instance_upload_bytes_total_=0;
    sprite_instance_dirty_ranges_=0;sprite_instances_reused_=0;
    sprite_instance_mirror_.clear();sprite_draw_index_mirror_.clear();sprite_range_allocator_.clear();
    sprite_draw_indices_uploaded_=0;sprite_draw_index_upload_bytes_=0;sprite_draw_index_upload_bytes_total_=0;
    sprite_draw_index_dirty_ranges_=0;sprite_stable_range_evictions_=0;
    tilemap_count_=0;tile_cell_instances_requested_=0;tile_cell_instances_submitted_=0;tilemap_visible_chunks_=0;tilemap_culled_chunks_=0;
    tilemap_bake_cache_hits_=0;tilemap_bake_cache_rebuilds_=0;tilemap_bake_cache_evictions_=0;tilemap_bake_cached_chunks_=0;
    tilemap_bake_retained_offscreen_chunks_=0;
    tilemap_chunks_resolved_=0;tilemap_chunks_skipped_before_resolution_=0;tilemap_cells_skipped_before_resolution_=0;
    tilemap_early_visibility_applied_=false;
    tilemap_chunk_ranges_=0;tilemap_largest_chunk_range_=0;
    normal_mapped_primitives_=0; metallic_roughness_mapped_primitives_=0; occlusion_mapped_primitives_=0;
    emissive_mapped_primitives_=0; alpha_masked_primitives_=0; double_sided_primitives_=0;
    alpha_blended_primitives_=0;
    if (capture_transfer_) SDL_ReleaseGPUTransferBuffer(device_,capture_transfer_);
    if (pick_transfer_) SDL_ReleaseGPUTransferBuffer(device_,pick_transfer_);
    if (pick_fence_) SDL_ReleaseGPUFence(device_,pick_fence_);
    if (checker_texture_) SDL_ReleaseGPUTexture(device_,checker_texture_);
    if (environment_texture_) SDL_ReleaseGPUTexture(device_,environment_texture_);
    if (irradiance_texture_) SDL_ReleaseGPUTexture(device_,irradiance_texture_);
    if (brdf_lut_texture_) SDL_ReleaseGPUTexture(device_,brdf_lut_texture_);
    if (white_texture_) SDL_ReleaseGPUTexture(device_,white_texture_);
    if (linear_white_texture_) SDL_ReleaseGPUTexture(device_,linear_white_texture_);
    if (linear_black_texture_) SDL_ReleaseGPUTexture(device_,linear_black_texture_);
    if (flat_normal_texture_) SDL_ReleaseGPUTexture(device_,flat_normal_texture_);
    if (black_texture_) SDL_ReleaseGPUTexture(device_,black_texture_);
    if (material_sampler_) SDL_ReleaseGPUSampler(device_,material_sampler_);
    if (sprite_nearest_sampler_) SDL_ReleaseGPUSampler(device_,sprite_nearest_sampler_);
    for(auto* sampler:sprite_nearest_lod_samplers_)if(sampler)SDL_ReleaseGPUSampler(device_,sampler);
    for(auto* sampler:sprite_linear_lod_samplers_)if(sampler)SDL_ReleaseGPUSampler(device_,sampler);
    if (tone_map_sampler_) SDL_ReleaseGPUSampler(device_,tone_map_sampler_);
    if (environment_sampler_) SDL_ReleaseGPUSampler(device_,environment_sampler_);
    if (shadow_sampler_) SDL_ReleaseGPUSampler(device_,shadow_sampler_);
    if (shadow_pipeline_) SDL_ReleaseGPUGraphicsPipeline(device_,shadow_pipeline_);
    if (shadow_double_sided_pipeline_) SDL_ReleaseGPUGraphicsPipeline(device_,shadow_double_sided_pipeline_);
    if (lit_pipeline_) SDL_ReleaseGPUGraphicsPipeline(device_,lit_pipeline_);
    if (lit_double_sided_pipeline_) SDL_ReleaseGPUGraphicsPipeline(device_,lit_double_sided_pipeline_);
    if(gpu_driven_lit_pipeline_)SDL_ReleaseGPUGraphicsPipeline(device_,gpu_driven_lit_pipeline_);
    if(gpu_driven_lit_double_sided_pipeline_)SDL_ReleaseGPUGraphicsPipeline(device_,gpu_driven_lit_double_sided_pipeline_);
    if (transparent_pipeline_) SDL_ReleaseGPUGraphicsPipeline(device_,transparent_pipeline_);
    if (transparent_double_sided_pipeline_) SDL_ReleaseGPUGraphicsPipeline(device_,transparent_double_sided_pipeline_);
    if(sprite_cutout_pipeline_)SDL_ReleaseGPUGraphicsPipeline(device_,sprite_cutout_pipeline_);
    if(sprite_alpha_pipeline_)SDL_ReleaseGPUGraphicsPipeline(device_,sprite_alpha_pipeline_);
    if(sprite_shadow_pipeline_)SDL_ReleaseGPUGraphicsPipeline(device_,sprite_shadow_pipeline_);
    if(sprite_instance_buffer_)SDL_ReleaseGPUBuffer(device_,sprite_instance_buffer_);
    if(sprite_instance_upload_)SDL_ReleaseGPUTransferBuffer(device_,sprite_instance_upload_);
    if(sprite_draw_index_buffer_)SDL_ReleaseGPUBuffer(device_,sprite_draw_index_buffer_);
    if(sprite_draw_index_upload_)SDL_ReleaseGPUTransferBuffer(device_,sprite_draw_index_upload_);
    if(local_light_buffer_)SDL_ReleaseGPUBuffer(device_,local_light_buffer_);
    if(light_cluster_buffer_)SDL_ReleaseGPUBuffer(device_,light_cluster_buffer_);
    if(light_cluster_index_buffer_)SDL_ReleaseGPUBuffer(device_,light_cluster_index_buffer_);
    if(local_light_upload_)SDL_ReleaseGPUTransferBuffer(device_,local_light_upload_);
    if(light_cluster_upload_)SDL_ReleaseGPUTransferBuffer(device_,light_cluster_upload_);
    if(light_cluster_index_upload_)SDL_ReleaseGPUTransferBuffer(device_,light_cluster_index_upload_);
    if(bloom_downsample_pipeline_)SDL_ReleaseGPUGraphicsPipeline(device_,bloom_downsample_pipeline_);
    if(bloom_upsample_pipeline_)SDL_ReleaseGPUGraphicsPipeline(device_,bloom_upsample_pipeline_);
    if (gtao_pipeline_) SDL_ReleaseGPUGraphicsPipeline(device_,gtao_pipeline_);
    if(ao_denoise_pipeline_)SDL_ReleaseGPUGraphicsPipeline(device_,ao_denoise_pipeline_);
    if (ao_composite_pipeline_) SDL_ReleaseGPUGraphicsPipeline(device_,ao_composite_pipeline_);
    if (auto_exposure_pipeline_) SDL_ReleaseGPUGraphicsPipeline(device_,auto_exposure_pipeline_);
    if (tone_map_pipeline_) SDL_ReleaseGPUGraphicsPipeline(device_,tone_map_pipeline_);
    if(native_rt_composite_pipeline_)SDL_ReleaseGPUGraphicsPipeline(device_,native_rt_composite_pipeline_);
    if (sky_atmosphere_pipeline_) SDL_ReleaseGPUGraphicsPipeline(device_,sky_atmosphere_pipeline_);
    if(sky_atmosphere_analytic_pipeline_)SDL_ReleaseGPUGraphicsPipeline(device_,sky_atmosphere_analytic_pipeline_);
    if(aerial_perspective_pipeline_)SDL_ReleaseGPUGraphicsPipeline(device_,aerial_perspective_pipeline_);
    if(sky_transmittance_pipeline_)SDL_ReleaseGPUComputePipeline(device_,sky_transmittance_pipeline_);
    if(sky_multi_scattering_pipeline_)SDL_ReleaseGPUComputePipeline(device_,sky_multi_scattering_pipeline_);
    if(sky_view_pipeline_)SDL_ReleaseGPUComputePipeline(device_,sky_view_pipeline_);
    if(sky_camera_volume_pipeline_)SDL_ReleaseGPUComputePipeline(device_,sky_camera_volume_pipeline_);
    if(sky_transmittance_lut_)SDL_ReleaseGPUTexture(device_,sky_transmittance_lut_);
    if(sky_multi_scattering_lut_)SDL_ReleaseGPUTexture(device_,sky_multi_scattering_lut_);
    if(sky_view_lut_)SDL_ReleaseGPUTexture(device_,sky_view_lut_);
    if(sky_camera_volume_lut_)SDL_ReleaseGPUTexture(device_,sky_camera_volume_lut_);
    if(sky_lut_sampler_)SDL_ReleaseGPUSampler(device_,sky_lut_sampler_);
    if (fxaa_pipeline_) SDL_ReleaseGPUGraphicsPipeline(device_,fxaa_pipeline_);
    if (taa_pipeline_) SDL_ReleaseGPUGraphicsPipeline(device_,taa_pipeline_);
    if(ssr_trace_pipeline_)SDL_ReleaseGPUGraphicsPipeline(device_,ssr_trace_pipeline_);
    if(ssr_temporal_pipeline_)SDL_ReleaseGPUGraphicsPipeline(device_,ssr_temporal_pipeline_);
    if(ssr_composite_pipeline_)SDL_ReleaseGPUGraphicsPipeline(device_,ssr_composite_pipeline_);
    if(ssgi_gather_pipeline_)SDL_ReleaseGPUGraphicsPipeline(device_,ssgi_gather_pipeline_);
    if(ssgi_spatial_pipeline_)SDL_ReleaseGPUGraphicsPipeline(device_,ssgi_spatial_pipeline_);
    if(ssgi_temporal_pipeline_)SDL_ReleaseGPUGraphicsPipeline(device_,ssgi_temporal_pipeline_);
    if(ssgi_composite_pipeline_)SDL_ReleaseGPUGraphicsPipeline(device_,ssgi_composite_pipeline_);
    if(depth_pyramid_seed_pipeline_)SDL_ReleaseGPUComputePipeline(device_,depth_pyramid_seed_pipeline_);
    if(depth_pyramid_reduce_pipeline_)SDL_ReleaseGPUComputePipeline(device_,depth_pyramid_reduce_pipeline_);
    if (vfx_alpha_draw_pipeline_) SDL_ReleaseGPUGraphicsPipeline(device_,vfx_alpha_draw_pipeline_);
    if (vfx_additive_draw_pipeline_) SDL_ReleaseGPUGraphicsPipeline(device_,vfx_additive_draw_pipeline_);
    if (vfx_compute_pipeline_) SDL_ReleaseGPUComputePipeline(device_,vfx_compute_pipeline_);
    if(gpu_visibility_pipeline_)SDL_ReleaseGPUComputePipeline(device_,gpu_visibility_pipeline_);
    if(gpu_occlusion_pipeline_)SDL_ReleaseGPUComputePipeline(device_,gpu_occlusion_pipeline_);
    if(gpu_driven_instance_buffer_)SDL_ReleaseGPUBuffer(device_,gpu_driven_instance_buffer_);
    if(gpu_driven_batch_buffer_)SDL_ReleaseGPUBuffer(device_,gpu_driven_batch_buffer_);
    if(gpu_driven_visible_index_buffer_)SDL_ReleaseGPUBuffer(device_,gpu_driven_visible_index_buffer_);
    if(gpu_driven_indirect_buffer_)SDL_ReleaseGPUBuffer(device_,gpu_driven_indirect_buffer_);
    if(gpu_occlusion_statistics_buffer_)SDL_ReleaseGPUBuffer(device_,gpu_occlusion_statistics_buffer_);
    if(gpu_driven_upload_buffer_)SDL_ReleaseGPUTransferBuffer(device_,gpu_driven_upload_buffer_);
    if(gpu_visibility_readback_transfer_)SDL_ReleaseGPUTransferBuffer(device_,gpu_visibility_readback_transfer_);
    if(gpu_visibility_readback_fence_)SDL_ReleaseGPUFence(device_,gpu_visibility_readback_fence_);
    if (vfx_spawn_pipeline_) SDL_ReleaseGPUComputePipeline(device_,vfx_spawn_pipeline_);
    if (vfx_group_pipeline_) SDL_ReleaseGPUComputePipeline(device_,vfx_group_pipeline_);
    if (vfx_sort_alpha_pipeline_) SDL_ReleaseGPUComputePipeline(device_,vfx_sort_alpha_pipeline_);
    if (vfx_particle_buffer_) SDL_ReleaseGPUBuffer(device_,vfx_particle_buffer_);
    for (auto* buffer:vfx_alive_buffers_) if (buffer) SDL_ReleaseGPUBuffer(device_,buffer);
    if (vfx_dead_buffer_) SDL_ReleaseGPUBuffer(device_,vfx_dead_buffer_);
    for (auto* buffer:vfx_counter_buffers_) if (buffer) SDL_ReleaseGPUBuffer(device_,buffer);
    if (vfx_dead_counter_buffer_) SDL_ReleaseGPUBuffer(device_,vfx_dead_counter_buffer_);
    if (vfx_spawn_buffer_) SDL_ReleaseGPUBuffer(device_,vfx_spawn_buffer_);
    if (vfx_spawn_graph_buffer_) SDL_ReleaseGPUBuffer(device_,vfx_spawn_graph_buffer_);
    if (vfx_additive_indices_buffer_) SDL_ReleaseGPUBuffer(device_,vfx_additive_indices_buffer_);
    if (vfx_alpha_indices_buffer_) SDL_ReleaseGPUBuffer(device_,vfx_alpha_indices_buffer_);
    if (vfx_additive_counter_buffer_) SDL_ReleaseGPUBuffer(device_,vfx_additive_counter_buffer_);
    if (vfx_alpha_counter_buffer_) SDL_ReleaseGPUBuffer(device_,vfx_alpha_counter_buffer_);
    if (vfx_upload_buffer_) SDL_ReleaseGPUTransferBuffer(device_,vfx_upload_buffer_);
    if (index_buffer_) SDL_ReleaseGPUBuffer(device_,index_buffer_);
    if (vertex_buffer_) SDL_ReleaseGPUBuffer(device_,vertex_buffer_);
    capture_transfer_=nullptr; pick_transfer_=nullptr; pick_fence_=nullptr; checker_texture_=nullptr; white_texture_=nullptr;
    linear_white_texture_=nullptr; linear_black_texture_=nullptr; flat_normal_texture_=nullptr; black_texture_=nullptr; environment_texture_=nullptr;
    irradiance_texture_=nullptr; brdf_lut_texture_=nullptr;
    material_sampler_=nullptr; sprite_nearest_sampler_=nullptr;tone_map_sampler_=nullptr; environment_sampler_=nullptr; shadow_sampler_=nullptr;
    sprite_nearest_lod_samplers_.fill(nullptr);sprite_linear_lod_samplers_.fill(nullptr);
    shadow_pipeline_=nullptr; shadow_double_sided_pipeline_=nullptr; lit_pipeline_=nullptr; lit_double_sided_pipeline_=nullptr;
    gpu_driven_lit_pipeline_=nullptr;gpu_driven_lit_double_sided_pipeline_=nullptr;
    transparent_pipeline_=nullptr; transparent_double_sided_pipeline_=nullptr;sprite_cutout_pipeline_=nullptr;sprite_alpha_pipeline_=nullptr;
    sprite_shadow_pipeline_=nullptr;
    sprite_instance_buffer_=nullptr;sprite_instance_upload_=nullptr;sprite_draw_index_buffer_=nullptr;sprite_draw_index_upload_=nullptr;
    local_light_buffer_=nullptr;light_cluster_buffer_=nullptr;light_cluster_index_buffer_=nullptr;
    local_light_upload_=nullptr;light_cluster_upload_=nullptr;light_cluster_index_upload_=nullptr;
    bloom_downsample_pipeline_=nullptr;bloom_upsample_pipeline_=nullptr;gtao_pipeline_=nullptr;ao_denoise_pipeline_=nullptr;
    ao_composite_pipeline_=nullptr;auto_exposure_pipeline_=nullptr;
    tone_map_pipeline_=nullptr;native_rt_composite_pipeline_=nullptr;sky_atmosphere_pipeline_=nullptr;sky_atmosphere_analytic_pipeline_=nullptr;aerial_perspective_pipeline_=nullptr;fxaa_pipeline_=nullptr;taa_pipeline_=nullptr;
    ssr_trace_pipeline_=nullptr;ssr_temporal_pipeline_=nullptr;ssr_composite_pipeline_=nullptr;
    ssgi_gather_pipeline_=nullptr;ssgi_spatial_pipeline_=nullptr;ssgi_temporal_pipeline_=nullptr;ssgi_composite_pipeline_=nullptr;
    depth_pyramid_seed_pipeline_=nullptr;depth_pyramid_reduce_pipeline_=nullptr;vfx_alpha_draw_pipeline_=nullptr;
    sky_transmittance_pipeline_=nullptr;sky_multi_scattering_pipeline_=nullptr;sky_view_pipeline_=nullptr;sky_camera_volume_pipeline_=nullptr;
    sky_transmittance_lut_=nullptr;sky_multi_scattering_lut_=nullptr;sky_view_lut_=nullptr;sky_camera_volume_lut_=nullptr;sky_lut_sampler_=nullptr;
    sky_medium_lut_valid_=false;sky_lut_valid_=false;sky_medium_lut_identity_.clear();
    sky_lut_identity_.clear();sky_camera_volume_identity_.clear();sky_lut_width_=sky_lut_height_=0;
    sky_camera_volume_extent_.fill(0);
    vfx_additive_draw_pipeline_=nullptr;vfx_compute_pipeline_=nullptr;gpu_visibility_pipeline_=nullptr;gpu_occlusion_pipeline_=nullptr;
    gpu_driven_instance_buffer_=nullptr;gpu_driven_batch_buffer_=nullptr;gpu_driven_visible_index_buffer_=nullptr;
    gpu_driven_indirect_buffer_=nullptr;gpu_driven_upload_buffer_=nullptr;gpu_occlusion_statistics_buffer_=nullptr;vfx_spawn_pipeline_=nullptr;
    gpu_visibility_readback_transfer_=nullptr;gpu_visibility_readback_fence_=nullptr;
    vfx_group_pipeline_=nullptr; vfx_sort_alpha_pipeline_=nullptr; vfx_particle_buffer_=nullptr;
    vfx_alive_buffers_.fill(nullptr); vfx_dead_buffer_=nullptr;
    vfx_counter_buffers_.fill(nullptr); vfx_dead_counter_buffer_=nullptr; vfx_spawn_buffer_=nullptr; vfx_spawn_graph_buffer_=nullptr;
    vfx_additive_indices_buffer_=nullptr; vfx_alpha_indices_buffer_=nullptr;
    vfx_additive_counter_buffer_=nullptr; vfx_alpha_counter_buffer_=nullptr;
    vfx_upload_buffer_=nullptr;
    index_buffer_=nullptr; vertex_buffer_=nullptr;
    gpu_driven_instance_mirror_.clear();gpu_driven_batch_mirror_.clear();gpu_batch_cache_.clear();
    gpu_batch_resource_keys_.clear();gpu_batch_texture_table_revision_=0U;
    gpu_driven_cached_plan_={};gpu_driven_topology_fingerprint_=0U;gpu_driven_cached_plan_valid_=false;
    gpu_driven_batch_candidate_offsets_.clear();gpu_driven_batch_candidate_counts_.clear();gpu_driven_batch_visible_offsets_.clear();
    gpu_driven_batch_reference_visible_indices_.clear();
    gpu_visibility_readback_batch_candidate_offsets_.clear();gpu_visibility_readback_batch_candidate_counts_.clear();
    gpu_visibility_readback_batch_visible_offsets_.clear();gpu_visibility_readback_expected_indices_.clear();
    gpu_visibility_readback_actual_indices_.clear();
    gpu_driven_candidate_draw_ids_.clear();gpu_visibility_readback_candidate_draw_ids_.clear();
    gpu_visibility_readback_actual_draw_ids_.clear();
    depth_pyramid_history_ready_=false;
}

} // namespace noemancer
