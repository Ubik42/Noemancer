#pragma once

#include <SDL3/SDL_gpu.h>

#include "engine/asset_registry.hpp"
#include "engine/clustered_lighting.hpp"
#include "engine/gpu_batch_cache.hpp"
#include "engine/hybrid_pixel_profile.hpp"
#include "engine/hybrid_pixel_render.hpp"
#include "engine/pixel_presentation.hpp"
#include "engine/render_graph.hpp"
#include "engine/render_world.hpp"
#include "engine/sky_atmosphere.hpp"
#include "engine/stable_range_allocator.hpp"
#include "engine/temporal_history.hpp"
#include "engine/screen_space_reflections.hpp"
#include "engine/screen_space_global_illumination.hpp"
#include "engine/texture_streaming_demand.hpp"
#include "engine/vfx_gpu_residency.hpp"
#include "runtime/runtime_texture_upload.hpp"
#include "runtime/asset_vfs_catalog.hpp"
#include "runtime/gpu_pass_timestamp_adapter.hpp"
#include "runtime/scene_raytracing_geometry_cache.hpp"
#include "runtime/scene_raytracing_geometry_source_adapter.hpp"
#include "runtime/sdl_gpu_native_device_bridge.hpp"
#include "runtime/sdl_gpu_native_texture_bridge.hpp"
#include "runtime/texture_resource_table.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace noemancer {

class SceneRayTracingBridge;

class SceneRenderer final {
public:
    SceneRenderer(SDL_GPUDevice* device, const AssetRegistry& asset_registry,
                  std::shared_ptr<VirtualFileSystem> virtual_file_system,
                  const AssetVfsCatalog& asset_vfs_catalog,
                  TextureResourceTable& texture_resources, bool gpu_debug = false);
    ~SceneRenderer();

    SceneRenderer(const SceneRenderer&) = delete;
    SceneRenderer& operator=(const SceneRenderer&) = delete;

    [[nodiscard]] bool initialize();
    [[nodiscard]] bool resize(std::uint32_t width, std::uint32_t height);
    void set_exposure(float exposure);
    void set_render_scale(float render_scale);
    [[nodiscard]] bool set_hybrid_pixel_profile(std::optional<HybridPixelProfile> profile);
    void set_hybrid_pixel_projection(HybridPixelRenderProjection projection) noexcept;
    [[nodiscard]] bool set_shadow_quality(const std::string& quality);
    void set_texture_streaming_budget_kib(std::uint32_t budget_kib);
    void set_texture_streaming_resident_budget_kib(std::uint32_t budget_kib);
    void set_texture_streaming_workload(std::string workload);
    void commit_texture_streaming_frame();
    void rollback_texture_streaming_frame();
    void set_temporal_debug_mode(const std::string& mode);
    [[nodiscard]] bool set_ssr_options(bool enabled, const std::string& quality,
        const std::string& debug_mode);
    [[nodiscard]] bool set_ssgi_options(bool enabled, const std::string& quality,
        const std::string& debug_mode);
    void set_gpu_driven_enabled(bool enabled);
    void set_gpu_occlusion_enabled(bool enabled) noexcept { gpu_occlusion_enabled_ = enabled; }
    void set_native_raytracing_session_enabled(bool enabled) noexcept {
        native_raytracing_session_enabled_ = enabled;
    }
    void set_gpu_pass_timing_enabled(bool enabled) noexcept { gpu_pass_timestamps_.set_enabled(enabled); }
    [[nodiscard]] bool gpu_pass_timing_submission_pending() const noexcept {
        return gpu_pass_timestamps_.submission_requires_fence();
    }
    void attach_gpu_pass_timing_fence(SDL_GPUFence* fence) { gpu_pass_timestamps_.attach_submission_fence(fence); }
    void abandon_gpu_pass_timing_submission() noexcept { gpu_pass_timestamps_.abandon_submission(); }
    void poll_gpu_pass_timings() { gpu_pass_timestamps_.poll(); }
    [[nodiscard]] std::string gpu_pass_timing_evidence_json() const { return gpu_pass_timestamps_.evidence_json(); }
    void set_ambient_occlusion_enabled(bool enabled) noexcept { ambient_occlusion_enabled_ = enabled; }
    void set_auto_exposure_enabled(bool enabled) noexcept { auto_exposure_enabled_ = enabled; }
    void set_sky_atmosphere(SkyAtmosphereSettings settings);
    [[nodiscard]] bool enqueue_gpu_visibility_readback(SDL_GPUCommandBuffer* command_buffer);
    void attach_gpu_visibility_readback_fence(SDL_GPUFence* fence);
    [[nodiscard]] bool resolve_gpu_visibility_readback();
    [[nodiscard]] bool gpu_visibility_readback_passed() const noexcept {
        return gpu_visibility_readback_state_=="complete"&&gpu_visibility_readback_match_;
    }
    void set_capture_contract_json(std::string contract_json);
    void render(SDL_GPUCommandBuffer* command_buffer, const RenderWorldSnapshot& render_world);
    [[nodiscard]] bool enqueue_color_capture(SDL_GPUCommandBuffer* command_buffer);
    [[nodiscard]] bool enqueue_texture_capture(SDL_GPUCommandBuffer* command_buffer,SDL_GPUTexture* texture,
                                                std::uint32_t width,std::uint32_t height,
                                                SDL_GPUTextureFormat format=SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM,
                                                bool swap_red_blue=false);
    [[nodiscard]] bool save_color_capture(const std::string& path);
    [[nodiscard]] bool enqueue_pick(SDL_GPUCommandBuffer* command_buffer, std::uint32_t x, std::uint32_t y);
    void attach_pick_fence(SDL_GPUFence* fence);
    [[nodiscard]] std::string resolve_pick();
    [[nodiscard]] std::string last_pixel_evidence_json() const;
    [[nodiscard]] SDL_GPUTexture* color_texture() const { return color_texture_; }
    [[nodiscard]] std::uint32_t width() const { return width_; }
    [[nodiscard]] std::uint32_t height() const { return height_; }
    [[nodiscard]] std::string status_json() const;
    [[nodiscard]] const std::string& last_error() const { return last_error_; }

private:
    void release();
    void release_targets();
    [[nodiscard]] bool create_geometry();
    [[nodiscard]] bool create_imported_geometry();
    [[nodiscard]] bool create_material_resources();
    [[nodiscard]] bool create_sprite_resources();
    void record_texture_streaming(SDL_GPUCommandBuffer* command_buffer,const RenderWorldSnapshot& render_world);
    void update_native_raytracing_scene(const RenderWorldSnapshot& render_world);
    void refresh_texture_stream_bindings();
    void refresh_texture_streaming_statistics();
    [[nodiscard]] SDL_GPUSampler* sprite_sampler(SDL_GPUTexture* texture,bool nearest);
    [[nodiscard]] bool create_environment_resources();
    [[nodiscard]] bool create_clustered_lighting_resources();
    [[nodiscard]] bool create_gpu_driven_resources();
    [[nodiscard]] bool upload_clustered_lighting(SDL_GPUCommandBuffer* command_buffer,
        const RenderWorldSnapshot& render_world,const ClusteredLightingCamera& camera,
        std::span<const std::int32_t> shadow_base_layers);
    [[nodiscard]] bool create_vfx_compute_resources();
    [[nodiscard]] bool create_sky_atmosphere_resources();
    [[nodiscard]] bool ensure_sky_atmosphere_resources();
    [[nodiscard]] bool dispatch_sky_atmosphere_luts(SDL_GPUCommandBuffer* command_buffer,
        const std::array<float,3>& camera_position,const std::array<float,3>& camera_right,
        const std::array<float,3>& camera_up,const std::array<float,3>& camera_forward,
        float tan_half_fov_y,float aspect_ratio,float near_clip,float far_clip,
        bool orthographic_projection,float orthographic_height);
    [[nodiscard]] bool upload_vfx_compute_state(SDL_GPUCommandBuffer* command_buffer, const RenderWorldSnapshot& render_world);
    void dispatch_vfx_compute(SDL_GPUCommandBuffer* command_buffer);
    void dispatch_vfx_group_sort(SDL_GPUCommandBuffer* command_buffer, const std::array<float,3>& camera_position);
    [[nodiscard]] bool create_rgba8_texture(std::uint32_t width, std::uint32_t height,
                                             const std::uint8_t* pixels, std::size_t byte_count,
                                             bool srgb, SDL_GPUTexture*& texture);
    [[nodiscard]] bool create_pipelines();
    [[nodiscard]] bool create_targets(std::uint32_t width, std::uint32_t height);
    [[nodiscard]] bool hybrid_pixel_active() const noexcept;

    struct GpuPrimitive final {
        std::uint32_t first_index{};
        std::uint32_t index_count{};
        std::array<float, 4> base_color{};
        float metallic{};
        float roughness{1.0F};
        bool unlit{};
        int base_color_image{-1};
        int normal_image{-1};
        int metallic_roughness_image{-1};
        int occlusion_image{-1};
        int emissive_image{-1};
        std::array<float, 3> emissive_factor{};
        float normal_scale{1.0F};
        float occlusion_strength{1.0F};
        float alpha_cutoff{0.5F};
        std::string alpha_mode{"OPAQUE"};
        bool double_sided{};
        int skin{-1};
        std::array<float,3> bounds_center{};
        float bounds_radius{};
    };
    struct GpuMesh final {
        SDL_GPUBuffer* vertex_buffer{};
        SDL_GPUBuffer* index_buffer{};
        std::vector<GpuPrimitive> primitives;
        std::vector<TextureResourceHandle> textures_srgb;
        std::vector<TextureResourceHandle> textures_linear;
        std::size_t vertex_count{};
        std::size_t index_count{};
    };

    SDL_GPUDevice* device_{nullptr};
    const AssetRegistry& asset_registry_;
    std::shared_ptr<VirtualFileSystem> virtual_file_system_;
    const AssetVfsCatalog& asset_vfs_catalog_;
    TextureResourceTable& texture_resources_;
    std::string gpu_backend_;
    bool gpu_debug_{};
    GpuPassTimestampAdapter gpu_pass_timestamps_;
    bool gpu_driven_enabled_{true};
    bool gpu_occlusion_enabled_{};
    bool native_raytracing_session_enabled_{};
    std::string gpu_device_name_;
    std::string gpu_driver_name_;
    std::string gpu_driver_version_;
    std::string gpu_driver_info_;
    SdlGpuNativeDeviceBridgeResult sdl_native_device_bridge_;
    SdlGpuNativeTextureExport native_rt_texture_export_;
    std::uint64_t native_rt_texture_generation_{};
    std::string shader_artifact_format_;
    std::vector<std::string> available_gpu_backends_;
    SDL_GPUBuffer* vertex_buffer_{nullptr};
    SDL_GPUBuffer* index_buffer_{nullptr};
    SDL_GPUGraphicsPipeline* shadow_pipeline_{nullptr};
    SDL_GPUGraphicsPipeline* shadow_double_sided_pipeline_{nullptr};
    SDL_GPUGraphicsPipeline* lit_pipeline_{nullptr};
    SDL_GPUGraphicsPipeline* lit_double_sided_pipeline_{nullptr};
    SDL_GPUGraphicsPipeline* gpu_driven_lit_pipeline_{nullptr};
    SDL_GPUGraphicsPipeline* gpu_driven_lit_double_sided_pipeline_{nullptr};
    SDL_GPUGraphicsPipeline* transparent_pipeline_{nullptr};
    SDL_GPUGraphicsPipeline* transparent_double_sided_pipeline_{nullptr};
    SDL_GPUGraphicsPipeline* sprite_cutout_pipeline_{nullptr};
    SDL_GPUGraphicsPipeline* sprite_alpha_pipeline_{nullptr};
    SDL_GPUGraphicsPipeline* sprite_shadow_pipeline_{nullptr};
    SDL_GPUBuffer* sprite_instance_buffer_{nullptr};
    SDL_GPUTransferBuffer* sprite_instance_upload_{nullptr};
    std::vector<std::byte> sprite_instance_mirror_;
    SDL_GPUBuffer* sprite_draw_index_buffer_{nullptr};
    SDL_GPUTransferBuffer* sprite_draw_index_upload_{nullptr};
    std::vector<std::uint32_t> sprite_draw_index_mirror_;
    StableRangeAllocator sprite_range_allocator_{131072};
    SDL_GPUBuffer* local_light_buffer_{nullptr};
    SDL_GPUBuffer* light_cluster_buffer_{nullptr};
    SDL_GPUBuffer* light_cluster_index_buffer_{nullptr};
    SDL_GPUTransferBuffer* local_light_upload_{nullptr};
    SDL_GPUTransferBuffer* light_cluster_upload_{nullptr};
    SDL_GPUTransferBuffer* light_cluster_index_upload_{nullptr};
    ClusteredLightingConfig clustered_lighting_config_{};
    std::size_t local_lights_submitted_{};
    std::size_t local_lights_dropped_{};
    std::size_t light_cluster_assignments_{};
    std::size_t light_cluster_overflows_{};
    std::uint64_t clustered_lighting_upload_bytes_{};
    SDL_GPUGraphicsPipeline* bloom_downsample_pipeline_{nullptr};
    SDL_GPUGraphicsPipeline* bloom_upsample_pipeline_{nullptr};
    SDL_GPUGraphicsPipeline* gtao_pipeline_{nullptr};
    SDL_GPUGraphicsPipeline* ao_denoise_pipeline_{nullptr};
    SDL_GPUGraphicsPipeline* ao_composite_pipeline_{nullptr};
    SDL_GPUGraphicsPipeline* ssr_trace_pipeline_{nullptr};
    SDL_GPUGraphicsPipeline* ssr_temporal_pipeline_{nullptr};
    SDL_GPUGraphicsPipeline* ssr_composite_pipeline_{nullptr};
    SDL_GPUGraphicsPipeline* ssgi_gather_pipeline_{nullptr};
    SDL_GPUGraphicsPipeline* ssgi_spatial_pipeline_{nullptr};
    SDL_GPUGraphicsPipeline* ssgi_temporal_pipeline_{nullptr};
    SDL_GPUGraphicsPipeline* ssgi_composite_pipeline_{nullptr};
    SDL_GPUGraphicsPipeline* auto_exposure_pipeline_{nullptr};
    SDL_GPUGraphicsPipeline* tone_map_pipeline_{nullptr};
    SDL_GPUGraphicsPipeline* sky_atmosphere_pipeline_{nullptr};
    SDL_GPUGraphicsPipeline* sky_atmosphere_analytic_pipeline_{nullptr};
    SDL_GPUGraphicsPipeline* aerial_perspective_pipeline_{nullptr};
    SDL_GPUComputePipeline* sky_transmittance_pipeline_{nullptr};
    SDL_GPUComputePipeline* sky_multi_scattering_pipeline_{nullptr};
    SDL_GPUComputePipeline* sky_view_pipeline_{nullptr};
    SDL_GPUComputePipeline* sky_camera_volume_pipeline_{nullptr};
    SDL_GPUTexture* sky_transmittance_lut_{nullptr};
    SDL_GPUTexture* sky_multi_scattering_lut_{nullptr};
    SDL_GPUTexture* sky_view_lut_{nullptr};
    SDL_GPUTexture* sky_camera_volume_lut_{nullptr};
    SDL_GPUSampler* sky_lut_sampler_{nullptr};
    std::string sky_medium_lut_identity_;
    std::string sky_lut_identity_;
    std::uint32_t sky_lut_width_{};
    std::uint32_t sky_lut_height_{};
    std::array<std::uint32_t,3> sky_camera_volume_extent_{};
    std::uint64_t sky_lut_regenerations_{};
    std::uint64_t sky_medium_lut_regenerations_{};
    std::uint64_t sky_view_lut_regenerations_{};
    std::uint64_t sky_camera_volume_regenerations_{};
    std::string sky_camera_volume_identity_;
    bool sky_lut_valid_{};
    bool sky_medium_lut_valid_{};
    bool sky_last_camera_orthographic_{};
    std::string sky_last_path_{"uninitialized"};
    std::string sky_lut_fallback_reason_;
    SDL_GPUGraphicsPipeline* fxaa_pipeline_{nullptr};
    SDL_GPUGraphicsPipeline* taa_pipeline_{nullptr};
    SDL_GPUComputePipeline* depth_pyramid_seed_pipeline_{nullptr};
    SDL_GPUComputePipeline* depth_pyramid_reduce_pipeline_{nullptr};
    SDL_GPUGraphicsPipeline* vfx_alpha_draw_pipeline_{nullptr};
    SDL_GPUGraphicsPipeline* vfx_additive_draw_pipeline_{nullptr};
    SDL_GPUComputePipeline* vfx_compute_pipeline_{nullptr};
    SDL_GPUComputePipeline* gpu_visibility_pipeline_{nullptr};
    SDL_GPUComputePipeline* gpu_occlusion_pipeline_{nullptr};
    SDL_GPUBuffer* gpu_driven_instance_buffer_{nullptr};
    SDL_GPUBuffer* gpu_driven_batch_buffer_{nullptr};
    SDL_GPUBuffer* gpu_driven_visible_index_buffer_{nullptr};
    SDL_GPUBuffer* gpu_driven_indirect_buffer_{nullptr};
    SDL_GPUBuffer* gpu_occlusion_statistics_buffer_{nullptr};
    SDL_GPUTransferBuffer* gpu_driven_upload_buffer_{nullptr};
    SDL_GPUTransferBuffer* gpu_visibility_readback_transfer_{nullptr};
    SDL_GPUFence* gpu_visibility_readback_fence_{nullptr};
    std::vector<std::byte> gpu_driven_instance_mirror_;
    std::vector<std::byte> gpu_driven_batch_mirror_;
    GpuBatchCache gpu_batch_cache_{{16384U,32U,1024U,8U,224U}};
    std::map<std::string, GpuBatchKey> gpu_batch_resource_keys_;
    std::uint64_t gpu_batch_texture_table_revision_{};
    GpuBatchPlan gpu_driven_cached_plan_;
    std::uint64_t gpu_driven_topology_fingerprint_{};
    bool gpu_driven_cached_plan_valid_{};
    std::vector<std::uint32_t> gpu_driven_batch_candidate_offsets_;
    std::vector<std::uint32_t> gpu_driven_batch_candidate_counts_;
    std::vector<std::uint32_t> gpu_driven_batch_visible_offsets_;
    std::vector<std::vector<std::uint32_t>> gpu_driven_batch_reference_visible_indices_;
    std::vector<std::uint32_t> gpu_visibility_readback_batch_candidate_offsets_;
    std::vector<std::uint32_t> gpu_visibility_readback_batch_candidate_counts_;
    std::vector<std::uint32_t> gpu_visibility_readback_batch_visible_offsets_;
    std::vector<std::vector<std::uint32_t>> gpu_visibility_readback_expected_indices_;
    std::vector<std::vector<std::uint32_t>> gpu_visibility_readback_actual_indices_;
    std::vector<std::string> gpu_driven_candidate_draw_ids_;
    std::vector<std::string> gpu_visibility_readback_candidate_draw_ids_;
    std::vector<std::string> gpu_visibility_readback_actual_draw_ids_;
    SDL_GPUComputePipeline* vfx_spawn_pipeline_{nullptr};
    SDL_GPUComputePipeline* vfx_group_pipeline_{nullptr};
    SDL_GPUComputePipeline* vfx_sort_alpha_pipeline_{nullptr};
    SDL_GPUBuffer* vfx_particle_buffer_{nullptr};
    std::array<SDL_GPUBuffer*,2> vfx_alive_buffers_{};
    SDL_GPUBuffer* vfx_dead_buffer_{nullptr};
    std::array<SDL_GPUBuffer*,2> vfx_counter_buffers_{};
    SDL_GPUBuffer* vfx_dead_counter_buffer_{nullptr};
    SDL_GPUBuffer* vfx_spawn_buffer_{nullptr};
    SDL_GPUBuffer* vfx_spawn_graph_buffer_{nullptr};
    SDL_GPUBuffer* vfx_additive_indices_buffer_{nullptr};
    SDL_GPUBuffer* vfx_alpha_indices_buffer_{nullptr};
    SDL_GPUBuffer* vfx_additive_counter_buffer_{nullptr};
    SDL_GPUBuffer* vfx_alpha_counter_buffer_{nullptr};
    SDL_GPUTransferBuffer* vfx_upload_buffer_{nullptr};
    VfxGpuResidency vfx_gpu_residency_{8192};
    std::uint32_t vfx_alive_buffer_index_{};
    std::uint32_t allowed_frames_in_flight_{3U};
    SDL_GPUTexture* color_texture_{nullptr};
    SDL_GPUTexture* hdr_texture_{nullptr};
    SDL_GPUTexture* aerial_hdr_texture_{nullptr};
    SDL_GPUTexture* ao_composited_hdr_texture_{nullptr};
    SDL_GPUTexture* indirect_lighting_texture_{nullptr};
    SDL_GPUTexture* specular_indirect_texture_{nullptr};
    SDL_GPUTexture* reflection_properties_texture_{nullptr};
    SDL_GPUTexture* ssr_raw_texture_{nullptr};
    SDL_GPUTexture* ssr_resolved_texture_{nullptr};
    std::array<SDL_GPUTexture*,2> ssr_history_textures_{};
    SDL_GPUTexture* ssgi_raw_texture_{nullptr};
    SDL_GPUTexture* ssgi_raw_bent_normal_texture_{nullptr};
    SDL_GPUTexture* ssgi_spatial_texture_{nullptr};
    SDL_GPUTexture* ssgi_spatial_bent_normal_texture_{nullptr};
    SDL_GPUTexture* ssgi_bent_normal_texture_{nullptr};
    SDL_GPUTexture* ssgi_resolved_texture_{nullptr};
    std::array<SDL_GPUTexture*,2> ssgi_history_textures_{};
    std::array<SDL_GPUTexture*,2> ssgi_bent_normal_history_textures_{};
    SDL_GPUTexture* ssgi_composited_hdr_texture_{nullptr};
    SDL_GPUTexture* reflected_hdr_texture_{nullptr};
    SDL_GPUTexture* tone_mapped_texture_{nullptr};
    SDL_GPUTexture* object_id_texture_{nullptr};
    SDL_GPUTexture* normal_texture_{nullptr};
    SDL_GPUTexture* motion_texture_{nullptr};
    SDL_GPUTexture* reactive_mask_texture_{nullptr};
    SDL_GPUTexture* taa_resolved_texture_{nullptr};
    std::array<SDL_GPUTexture*,4> bloom_downsample_textures_{};
    std::array<SDL_GPUTexture*,3> bloom_upsample_textures_{};
    SDL_GPUTexture* ambient_occlusion_texture_{nullptr};
    SDL_GPUTexture* ambient_occlusion_temp_texture_{nullptr};
    SDL_GPUTexture* ambient_occlusion_filtered_texture_{nullptr};
    std::array<SDL_GPUTexture*,2> exposure_history_textures_{};
    std::array<SDL_GPUTexture*,2> taa_history_textures_{};
    std::array<SDL_GPUTexture*,2> taa_history_depth_textures_{};
    std::array<SDL_GPUTexture*,2> temporal_history_normal_textures_{};
    SDL_GPUTexture* depth_pyramid_texture_{nullptr};
    std::uint32_t depth_pyramid_mip_count_{};
    bool depth_pyramid_history_ready_{};
    std::uint64_t depth_pyramid_working_set_bytes_{};
    std::uint64_t depth_pyramid_seed_dispatches_{};
    std::uint64_t depth_pyramid_reduce_dispatches_{};
    SDL_GPUTexture* depth_texture_{nullptr};
    SDL_GPUTexture* shadow_texture_{nullptr};
    SDL_GPUTexture* local_shadow_texture_{nullptr};
    SDL_GPUSampler* shadow_sampler_{nullptr};
    SDL_GPUSampler* material_sampler_{nullptr};
    SDL_GPUSampler* sprite_nearest_sampler_{nullptr};
    SDL_GPUSampler* tone_map_sampler_{nullptr};
    std::array<SDL_GPUSampler*,32> sprite_nearest_lod_samplers_{};
    std::array<SDL_GPUSampler*,32> sprite_linear_lod_samplers_{};
    SDL_GPUTexture* white_texture_{nullptr};
    SDL_GPUTexture* linear_white_texture_{nullptr};
    SDL_GPUTexture* linear_black_texture_{nullptr};
    SDL_GPUTexture* flat_normal_texture_{nullptr};
    SDL_GPUTexture* black_texture_{nullptr};
    SDL_GPUTexture* checker_texture_{nullptr};
    SDL_GPUTexture* environment_texture_{nullptr};
    SDL_GPUTexture* irradiance_texture_{nullptr};
    SDL_GPUTexture* brdf_lut_texture_{nullptr};
    SDL_GPUSampler* environment_sampler_{nullptr};
    SDL_GPUTransferBuffer* capture_transfer_{nullptr};
    std::uint32_t capture_width_{},capture_height_{};
    SDL_GPUTextureFormat capture_format_{SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM};
    bool capture_swap_red_blue_{};
    SDL_GPUTransferBuffer* pick_transfer_{nullptr};
    SDL_GPUFence* pick_fence_{nullptr};
    std::uint32_t width_{0};
    std::uint32_t height_{0};
    std::uint32_t render_width_{0};
    std::uint32_t render_height_{0};
    std::uint32_t post_width_{0};
    std::uint32_t post_height_{0};
    float render_scale_{1.0F};
    float allocated_render_scale_{1.0F};
    std::optional<HybridPixelProfile> hybrid_pixel_profile_;
    PixelPresentation pixel_presentation_;
    HybridPixelRenderProjection hybrid_pixel_projection_;
    std::uint32_t temporal_debug_mode_{};
    std::string temporal_debug_mode_name_{"final"};
    bool ssr_enabled_{true};
    std::string ssr_quality_{"high"};
    std::uint32_t ssr_debug_mode_{};
    std::string ssr_debug_mode_name_{"final"};
    bool ssgi_enabled_{true};
    std::string ssgi_quality_{"high"};
    std::uint32_t ssgi_debug_mode_{};
    std::string ssgi_debug_mode_name_{"final"};
    ScreenSpaceGlobalIlluminationConfig ssgi_config_{screen_space_global_illumination_quality_defaults(
        ScreenSpaceGlobalIlluminationQuality::high)};
    ScreenSpaceGlobalIlluminationPlan ssgi_plan_{};
    ScreenSpaceReflectionsConfig ssr_config_{screen_space_reflections_quality_defaults(
        ScreenSpaceReflectionsQuality::high)};
    ScreenSpaceReflectionsPlan ssr_plan_{};
    std::size_t visible_renderables_{0};
    std::size_t visible_draws_{0};
    std::size_t camera_culled_draws_{0};
    std::size_t opaque_instances_submitted_{0};
    std::size_t opaque_draw_calls_{0};
    std::size_t opaque_draw_calls_saved_{0};
    std::size_t gpu_driven_candidates_{};
    std::size_t gpu_driven_batches_{};
    std::size_t gpu_driven_reference_visible_{};
    std::size_t gpu_driven_fallback_instances_{};
    std::uint64_t gpu_driven_upload_bytes_{};
    std::uint64_t gpu_driven_instance_upload_bytes_{};
    std::uint64_t gpu_driven_batch_upload_bytes_{};
    std::uint64_t gpu_driven_command_upload_bytes_{};
    std::uint64_t gpu_driven_stable_upload_bytes_total_{};
    std::size_t gpu_driven_dirty_ranges_{};
    std::size_t gpu_driven_dirty_instances_{};
    std::size_t gpu_driven_stable_slots_reused_{};
    std::size_t gpu_driven_moved_slots_{};
    std::uint64_t gpu_driven_topology_rebuilds_{};
    std::uint64_t gpu_driven_topology_reuses_{};
    bool gpu_driven_topology_reused_{};
    std::size_t gpu_driven_instance_uploads_{};
    std::uint64_t gpu_driven_dispatches_{};
    std::uint64_t gpu_driven_indirect_draws_{};
    std::string gpu_driven_fallback_reason_;
    bool gpu_occlusion_history_valid_{};
    bool gpu_occlusion_used_this_frame_{};
    std::string gpu_occlusion_fallback_reason_{"disabled-by-policy"};
    std::array<std::uint32_t,8> gpu_occlusion_readback_statistics_{};
    bool gpu_visibility_readback_occlusion_active_{};
    std::size_t gpu_visibility_readback_unexpected_visible_{};
    bool gpu_visibility_readback_conservative_subset_match_{};
    std::string gpu_visibility_readback_state_{"not-requested"};
    std::string gpu_visibility_readback_error_;
    std::uint64_t gpu_visibility_readback_frame_{};
    std::size_t gpu_visibility_readback_candidates_{};
    std::size_t gpu_visibility_readback_batches_{};
    std::size_t gpu_visibility_readback_cpu_visible_{};
    std::size_t gpu_visibility_readback_gpu_visible_{};
    std::size_t gpu_visibility_readback_invalid_batches_{};
    std::size_t gpu_visibility_readback_mismatched_batch_counts_{};
    std::size_t gpu_visibility_readback_out_of_range_indices_{};
    std::size_t gpu_visibility_readback_wrong_batch_indices_{};
    std::size_t gpu_visibility_readback_duplicate_indices_{};
    std::size_t gpu_visibility_readback_bytes_{};
    std::size_t gpu_visibility_readback_indirect_bytes_{};
    std::string gpu_visibility_readback_cpu_set_hash_;
    std::string gpu_visibility_readback_gpu_set_hash_;
    bool gpu_visibility_readback_count_match_{};
    bool gpu_visibility_readback_exact_set_match_{};
    bool gpu_visibility_readback_match_{};
    std::size_t vfx_particles_submitted_{0};
    std::uint64_t vfx_compute_dispatches_{};
    std::uint64_t vfx_spawn_dispatches_{};
    std::size_t vfx_spawn_graph_commands_uploaded_{};
    std::uint64_t vfx_group_dispatches_{};
    std::uint64_t vfx_sort_dispatches_{};
    std::uint64_t vfx_state_uploads_{};
    std::size_t vfx_particles_uploaded_{};
    std::size_t vfx_dynamic_attributes_uploaded_{};
    std::size_t vfx_alive_input_count_{};
    std::size_t vfx_resident_particles_{};
    std::size_t vfx_slots_reclaimed_{};
    std::size_t vfx_particles_dropped_{};
    std::uint64_t vfx_upload_bytes_{};
    std::uint32_t vfx_dispatch_groups_{};
    std::uint64_t vfx_indirect_draws_{};
    std::size_t vfx_expected_additive_particles_{};
    std::size_t vfx_expected_alpha_particles_{};
    std::size_t shadow_casters_{0};
    std::size_t shadow_primitives_{};
    std::size_t skinned_render_instances_{};
    std::size_t skinned_draw_items_{};
    std::size_t skinning_joint_matrices_{};
    std::size_t shadow_caster_draws_{0};
    std::size_t shadow_instances_submitted_{0};
    std::size_t shadow_draw_calls_saved_{0};
    std::array<std::size_t,4> shadow_draws_per_cascade_{};
    std::array<std::size_t,4> shadow_instances_per_cascade_{};
    std::array<std::size_t,4> shadow_draw_calls_saved_per_cascade_{};
    std::array<std::size_t,4> shadow_culled_per_cascade_{};
    std::size_t directional_shadow_cascades_rendered_{};
    std::size_t directional_shadow_cascades_cached_{};
    std::size_t directional_shadow_avoided_instances_{};
    std::size_t directional_shadow_avoided_draws_{};
    std::uint64_t directional_shadow_cache_hits_{};
    std::uint64_t directional_shadow_cache_misses_{};
    std::array<std::uint64_t,4> directional_shadow_cascade_fingerprints_{};
    std::array<bool,4> directional_shadow_cascade_cache_valid_{};
    std::size_t local_shadow_requested_lights_{};
    std::size_t local_shadow_selected_lights_{};
    std::size_t local_shadow_dropped_lights_{};
    std::size_t local_shadow_point_lights_{};
    std::size_t local_shadow_spot_lights_{};
    std::size_t local_shadow_faces_rendered_{};
    std::size_t local_shadow_faces_cached_{};
    std::size_t local_shadow_avoided_instances_{};
    std::size_t local_shadow_avoided_draws_{};
    std::uint64_t local_shadow_cache_hits_{};
    std::uint64_t local_shadow_cache_misses_{};
    std::array<std::uint64_t,8> local_shadow_face_fingerprints_{};
    std::array<bool,8> local_shadow_face_cache_valid_{};
    std::size_t local_shadow_instances_submitted_{};
    std::size_t local_shadow_draw_calls_{};
    std::size_t local_shadow_draw_calls_saved_{};
    std::size_t local_shadow_culled_draws_{};
    std::uint64_t local_shadow_texture_bytes_{};
    std::string shadow_quality_{"high"};
    std::uint32_t local_shadow_resolution_{1024};
    std::uint32_t maximum_shadowed_point_lights_{1};
    std::uint32_t maximum_shadowed_spot_lights_{2};
    std::vector<std::string> local_shadow_selected_ids_;
    std::size_t imported_meshes_{0};
    std::size_t cooked_geometry_loads_{};
    std::size_t source_geometry_decodes_{};
    std::size_t imported_primitives_{0};
    std::size_t imported_textures_{0};
    std::size_t normal_mapped_primitives_{0};
    std::size_t metallic_roughness_mapped_primitives_{0};
    std::size_t occlusion_mapped_primitives_{0};
    std::size_t emissive_mapped_primitives_{0};
    std::size_t alpha_masked_primitives_{0};
    std::size_t double_sided_primitives_{0};
    std::size_t alpha_blended_primitives_{0};
    std::size_t sprite_textures_uploaded_{0};
    std::size_t ktx_textures_uploaded_{};
    std::size_t ktx_native_compressed_textures_{};
    std::size_t ktx_rgba8_fallback_textures_{};
    std::size_t ktx_mip_levels_uploaded_{};
    std::uint64_t ktx_source_bytes_{};
    std::uint64_t ktx_resident_bytes_{};
    std::uint64_t ktx_tail_bytes_{};
    std::uint64_t ktx_staging_bytes_{};
    std::uint64_t texture_streaming_budget_bytes_{512U*1024U};
    std::uint64_t texture_streaming_resident_budget_bytes_{256U*1024U*1024U};
    std::uint64_t texture_streaming_bytes_this_frame_{};
    std::uint64_t texture_streaming_copy_bytes_this_frame_{};
    std::uint64_t texture_streaming_bytes_total_{};
    std::uint64_t texture_streaming_copy_bytes_total_{};
    std::size_t texture_streaming_levels_this_frame_{};
    std::size_t texture_streaming_pending_levels_{};
    std::size_t texture_streaming_completed_streams_{};
    std::size_t texture_streaming_cursor_{};
    std::uint64_t texture_streaming_demand_bytes_{};
    std::uint64_t texture_streaming_planned_bytes_{};
    std::uint64_t texture_streaming_bytes_released_this_frame_{};
    std::uint64_t texture_streaming_bytes_released_total_{};
    std::size_t texture_streaming_upgrades_this_frame_{};
    std::size_t texture_streaming_downgrades_this_frame_{};
    std::size_t texture_streaming_evictions_total_{};
    std::size_t texture_streaming_reuploads_total_{};
    bool texture_streaming_over_budget_{};
    std::string texture_streaming_plan_code_{"ok"};
    std::string texture_streaming_workload_;
    std::size_t sprite_draws_submitted_{0};
    std::size_t sprite_instances_submitted_{0};
    std::size_t sprite_draws_saved_{0};
    std::size_t sprite_draws_missing_texture_{0};
    std::size_t sprite_material_textures_missing_{0};
    // Runtime-only accounting for validated SpriteAtlas manifests.  IDs stay
    // private; status_json publishes bounded counts and explicit truncation.
    std::size_t sprite_atlas_manifests_discovered_{};
    std::size_t sprite_atlas_manifests_valid_{};
    std::size_t sprite_atlas_manifests_invalid_{};
    std::size_t sprite_atlas_declared_page_assets_{};
    std::size_t sprite_atlas_unique_page_assets_{};
    std::size_t sprite_atlas_page_textures_uploaded_{};
    std::size_t sprite_atlas_page_textures_missing_{};
    std::size_t sprite_atlas_page_textures_available_{};
    bool sprite_atlas_counts_truncated_{};
    std::size_t sprite_lit_instances_{0};
    std::size_t sprite_unlit_instances_{0};
    std::size_t sprite_shadow_receivers_{0};
    std::size_t sprite_shadow_casters_{0};
    std::size_t sprite_instances_uploaded_{};
    std::size_t sprite_instances_dropped_{};
    std::uint64_t sprite_instance_upload_bytes_{};
    std::uint64_t sprite_instance_upload_bytes_total_{};
    std::size_t sprite_instance_dirty_ranges_{};
    std::size_t sprite_instances_reused_{};
    std::size_t sprite_draw_indices_uploaded_{};
    std::uint64_t sprite_draw_index_upload_bytes_{};
    std::uint64_t sprite_draw_index_upload_bytes_total_{};
    std::size_t sprite_draw_index_dirty_ranges_{};
    std::size_t sprite_stable_range_evictions_{};
    std::size_t tilemap_count_{0};
    std::size_t tile_cell_instances_requested_{0};
    std::size_t tile_cell_instances_submitted_{0};
    std::size_t tilemap_visible_chunks_{};
    std::size_t tilemap_culled_chunks_{};
    std::size_t tilemap_bake_cache_hits_{};
    std::size_t tilemap_bake_cache_rebuilds_{};
    std::size_t tilemap_bake_cache_evictions_{};
    std::size_t tilemap_bake_cached_chunks_{};
    std::size_t tilemap_bake_retained_offscreen_chunks_{};
    std::size_t tilemap_chunks_resolved_{};
    std::size_t tilemap_chunks_skipped_before_resolution_{};
    std::size_t tilemap_cells_skipped_before_resolution_{};
    bool tilemap_early_visibility_applied_{};
    std::size_t tilemap_chunk_ranges_{};
    std::size_t tilemap_largest_chunk_range_{};
    CompiledRenderGraph render_graph_;
    SkyAtmosphereSettings sky_atmosphere_{make_sky_atmosphere_settings(SkyAtmosphereQuality::low)};
    std::string extraction_id_;
    std::uint64_t world_revision_{};
    std::uint64_t frame_index_{};
    std::unordered_map<std::string, GpuMesh> gpu_meshes_;
    // Derived CPU geometry retained only for the opt-in native RT production
    // bridge. Scene/Render World remain authoritative; this map has no native
    // handle and is never persisted.
    std::unordered_map<std::string,SceneRayTracingGeometryInput> raytracing_geometries_;
    SceneRayTracingGeometryCache raytracing_geometry_cache_;
    std::unique_ptr<SceneRayTracingBridge> scene_raytracing_bridge_;
    std::string native_raytracing_status_json_;
    std::unordered_map<std::string,TextureResourceHandle> sprite_textures_;
    std::unordered_map<std::string,TextureResourceHandle> sprite_linear_textures_;
    std::vector<RuntimeTextureStream> texture_streams_;
    std::vector<TextureResourceHandle> texture_stream_handles_;
    std::unordered_map<SDL_GPUTexture*,std::size_t> texture_stream_lookup_;
    std::vector<std::string> object_id_entities_;
    std::string last_picked_entity_id_;
    std::uint32_t last_pick_x_{};
    std::uint32_t last_pick_y_{};
    float last_pick_depth_{1.0F};
    std::array<float, 3> last_pick_normal_{};
    bool has_pixel_evidence_{};
    float exposure_{1.0F};
    float white_point_{1.0F};
    std::uint32_t exposure_history_index_{};
    bool exposure_history_valid_{};
    bool auto_exposure_enabled_{true};
    float auto_exposure_min_{0.25F};
    float auto_exposure_max_{4.0F};
    float auto_exposure_key_{0.18F};
    float auto_exposure_speed_up_{3.0F};
    float auto_exposure_speed_down_{1.0F};
    float gtao_radius_pixels_{14.0F};
    float gtao_intensity_{1.35F};
    float gtao_bias_{0.02F};
    float gtao_power_{1.25F};
    float gtao_denoise_depth_sigma_{1.5F};
    float gtao_denoise_normal_power_{16.0F};
    bool ambient_occlusion_enabled_{true};
    std::array<float,4> color_lift_{0.0F,0.0F,0.0F,0.0F};
    std::array<float,4> color_gamma_{1.0F,1.0F,1.0F,0.0F};
    std::array<float,4> color_gain_{1.0F,1.0F,1.0F,0.0F};
    float color_saturation_{1.0F};
    float color_contrast_{1.0F};
    float color_temperature_{};
    float color_tint_{};
    double shadow_record_microseconds_{};
    double gpu_visibility_record_microseconds_{};
    double opaque_record_microseconds_{};
    double sky_atmosphere_record_microseconds_{};
    double aerial_perspective_record_microseconds_{};
    double transparent_record_microseconds_{};
    double bloom_record_microseconds_{};
    double gtao_record_microseconds_{};
    double ao_denoise_record_microseconds_{};
    double ao_composite_record_microseconds_{};
    double ssr_trace_record_microseconds_{};
    double ssr_temporal_record_microseconds_{};
    double ssr_composite_record_microseconds_{};
    double ssgi_gather_record_microseconds_{};
    double ssgi_spatial_record_microseconds_{};
    double ssgi_temporal_record_microseconds_{};
    double ssgi_composite_record_microseconds_{};
    double auto_exposure_record_microseconds_{};
    double tone_map_record_microseconds_{};
    double fxaa_record_microseconds_{};
    double taa_record_microseconds_{};
    std::array<float,4> shadow_splits_{};
    std::array<float,4> shadow_radii_{};
    std::array<float,4> shadow_world_units_per_texel_{};
    std::uint64_t shadow_texture_bytes_{};
    float fxaa_edge_threshold_{0.125F};
    float fxaa_edge_threshold_min_{0.0312F};
    float bloom_threshold_{1.0F};
    float bloom_soft_knee_{0.5F};
    float bloom_strength_{0.35F};
    float bloom_scatter_{0.7F};
    std::uint64_t bloom_working_set_bytes_{};
    std::string environment_source_id_{"asset.environment.procedural-sky"};
    std::string ibl_source_fingerprint_;
    std::string ibl_artifact_path_;
    std::uint32_t environment_source_width_{};
    std::uint32_t environment_source_height_{};
    std::uintmax_t ibl_artifact_bytes_{};
    double ibl_cook_microseconds_{};
    bool ibl_cache_hit_{};
    bool ibl_cache_rebuilt_{};
    std::unordered_map<std::string,std::array<float,16>> previous_models_;
    std::unordered_map<std::string,std::vector<std::array<float,16>>> previous_skinning_matrices_;
    std::array<float,16> previous_view_projection_{};
    std::array<float,16> previous_unjittered_view_projection_{};
    std::uint64_t previous_temporal_frame_{};
    std::string previous_camera_id_;
    std::uint32_t taa_history_index_{};
    std::uint64_t taa_history_resets_{};
    bool temporal_history_valid_{};
    TemporalHistoryAuthority temporal_history_authority_;
    std::uint32_t ssr_history_index_{};
    bool ssr_history_valid_{};
    std::uint32_t ssgi_history_index_{};
    bool ssgi_history_valid_{};
    std::uint64_t temporal_camera_cut_epoch_{};
    std::uint32_t temporal_jitter_sample_{};
    std::array<float,2> temporal_jitter_pixels_{};
    std::array<float,2> temporal_jitter_ndc_{};
    std::string active_camera_id_;
    std::string active_light_id_;
    std::string capture_contract_json_;
    std::string last_error_;
};

} // namespace noemancer
