#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace noemancer {

// A long-lived companion to the short-lived native_d3d12_raytracing_executor.
// Only plain data crosses this boundary.  Device, queue, fence, acceleration
// structures, shader table and output resources remain in the private .cpp
// implementation and are released by the context destructor/shutdown().
inline constexpr std::string_view native_d3d12_raytracing_context_schema =
    "noemancer.native-d3d12-raytracing-context/0.1";
inline constexpr std::size_t native_d3d12_raytracing_context_max_text_bytes = 512U;
inline constexpr std::size_t native_d3d12_raytracing_context_max_geometry_count = 1024U;
inline constexpr std::size_t native_d3d12_raytracing_context_max_vertex_count = 1U << 20U;
inline constexpr std::size_t native_d3d12_raytracing_context_max_index_count = 3U << 20U;
inline constexpr std::uint64_t native_d3d12_raytracing_context_max_resource_bytes = 1ULL << 40U;
inline constexpr std::string_view native_d3d12_raytracing_output_surface_schema =
    "noemancer.native-d3d12-raytracing-output-surface/0.1";
// The camera is a versioned, bounded input to the full-frame shader ABI.  It
// uses world-space basis vectors rather than a matrix, so there is no hidden
// row/column-major convention at this boundary: right/up/forward are the
// directions used directly by RayGen and must form a right-handed orthonormal
// frame (right x up = forward).  vertical_fov_tan_half is tan(vertical-FOV / 2)
// and avoids a per-ray trigonometric conversion; the default values preserve
// the historical marker probe camera.
inline constexpr std::string_view native_d3d12_raytracing_camera_schema =
    "noemancer.native-d3d12-raytracing-camera/0.1";
inline constexpr std::string_view native_d3d12_raytracing_shading_schema =
    "noemancer.native-d3d12-raytracing-shading/0.1";
inline constexpr std::string_view native_d3d12_raytracing_full_frame_shader_contract =
    "noemancer.native-rt-full-frame/0.3";

struct NativeD3D12RayTracingCamera final {
    std::array<float, 3U> position{0.0F, 0.0F, -1.0F};
    std::array<float, 3U> right{1.0F, 0.0F, 0.0F};
    std::array<float, 3U> up{0.0F, 1.0F, 0.0F};
    std::array<float, 3U> forward{0.0F, 0.0F, 1.0F};
    float vertical_fov_tan_half{1.0F};
    float aspect_ratio{1.0F};
    float near_plane{0.001F};
    float far_plane{1.0e6F};
};

// Scene-linear material values for one retained TLAS instance.  geometry_id
// is resolved against the canonical scene snapshot before the GPU table is
// built; this avoids accidental positional remapping after scene sorting.
struct NativeD3D12RayTracingMaterial final {
    std::string geometry_id;
    std::array<float, 4U> base_color{0.80F, 0.36F, 0.12F, 1.0F};
    float metallic{};
    float roughness{0.5F};
    std::array<float, 3U> emissive{};
    float emissive_intensity{1.0F};
};

// A bounded direct + ambient lighting input.  directional_direction points
// from the shaded point toward the light and must be finite, bounded and
// non-zero.  This contract intentionally does not claim shadows or RTGI.
struct NativeD3D12RayTracingLighting final {
    std::array<float, 3U> directional_direction{-0.35F, 0.80F, 0.45F};
    std::array<float, 3U> directional_color{1.0F, 0.95F, 0.85F};
    float directional_intensity{1.0F};
    std::array<float, 3U> ambient_color{0.045F, 0.055F, 0.075F};
    float ambient_intensity{1.0F};
};

// The default policy keeps the historical deterministic contract: trace() and
// output-copy return only after their queue submission has completed.  The
// submit_only policy is an explicit opt-in for a render loop that wants to
// overlap CPU work with the GPU.  Callers then use synchronize(false) to poll
// or synchronize(true) to wait before consuming the private output view.
enum class NativeD3D12RayTracingSynchronizationPolicy : std::uint8_t {
    wait_for_completion = 0U,
    submit_only = 1U,
};

[[nodiscard]] std::string_view native_d3d12_raytracing_synchronization_policy_name(
    NativeD3D12RayTracingSynchronizationPolicy policy) noexcept;

enum class NativeD3D12RayTracingContextState : std::uint8_t {
    uninitialized,
    ready,
    unsupported,
    failed,
    shutdown,
};

enum class NativeD3D12RayTracingContextFailureStage : std::uint8_t {
    none,
    platform,
    loader,
    factory,
    adapter,
    device,
    feature,
    command_queue,
    command_allocator,
    command_list,
    fence,
    camera,
    shading,
    scene,
    blas,
    tlas,
    shader_pipeline,
    shader_table,
    output,
    trace,
    readback,
    synchronization,
    cleanup,
};

[[nodiscard]] std::string_view native_d3d12_raytracing_context_state_name(
    NativeD3D12RayTracingContextState state) noexcept;
[[nodiscard]] std::string_view native_d3d12_raytracing_context_failure_stage_name(
    NativeD3D12RayTracingContextFailureStage stage) noexcept;

// This is the resource contract consumed by a future Runtime interop adapter.
// It describes the retained output without exposing a COM pointer to Engine,
// Agent or any JSON/semantic receipt.  The current probe stores a linear UAV
// buffer (four 32-bit lanes per pixel), not an SDL_GPU texture.  A valid view
// is only published after TraceRays has completed and the output is in
// COPY_SOURCE state; consumers can therefore issue a GPU copy without mapping
// the resource on the CPU.
enum class NativeD3D12RayTracingOutputSurfaceState : std::uint8_t {
    unavailable = 0U,
    unordered_access = 1U,
    copy_source = 2U,
    released = 3U,
};

[[nodiscard]] std::string_view native_d3d12_raytracing_output_surface_state_name(
    NativeD3D12RayTracingOutputSurfaceState state) noexcept;

struct NativeD3D12RayTracingOutputSurfaceMetadata final {
    std::string schema{std::string(native_d3d12_raytracing_output_surface_schema)};
    std::string resource_kind{"buffer"};
    std::string format{"R32G32B32A32_UINT"};
    NativeD3D12RayTracingOutputSurfaceState resource_state{
        NativeD3D12RayTracingOutputSurfaceState::unavailable};
    bool valid{};
    bool resource_ready{};
    bool gpu_write_complete{};
    // This contract is GPU-only.  The diagnostic readback() method remains an
    // explicit opt-in proof path, but no CPU mapping is required to consume a
    // valid output view or copy it into another same-device resource.
    bool cpu_readback_required{};
    bool shared_device{};
    bool shared_command_queue{};
    // True when the retained linear layout can be copied directly into the
    // exported SDL texture footprint on the shared D3D12 device.  This does
    // not imply that the current probe shader wrote meaningful full-frame
    // pixels or that the texture has been composited by the Render Graph.
    bool direct_sdl_gpu_import_supported{};
    bool expired{};
    std::uint32_t width{};
    std::uint32_t height{};
    std::uint32_t depth{1U};
    std::uint32_t pixel_stride_bytes{};
    std::uint64_t bytes{};
    std::uint64_t resource_generation{};
    std::uint64_t context_generation{};
    std::uint64_t submitted_fence_value{};
    std::uint64_t completed_fence_value{};
    std::string code{"native-d3d12.context.output-surface-unavailable"};
    std::string detail;
};

// Runtime-private borrowed access.  The four pointers are non-owning COM
// interfaces and are intentionally absent from NativeD3D12RayTracingContextReceipt.
// A caller must keep the context alive, validate the access token immediately
// before use, and never release these pointers.  The context itself owns the
// QueryInterface/AddRef references it adopts during initialize().
struct NativeD3D12RayTracingContextPrivateOutputView final {
    NativeD3D12RayTracingOutputSurfaceMetadata metadata;
    std::uint64_t access_token{};
    void* resource{};
    void* device{};
    void* command_queue{};
    void* fence{};
};

struct NativeD3D12RayTracingContextShaderSet final {
    // Optional DXIL blobs for a caller-owned RayGen/Miss/ClosestHit shader
    // contract.  An all-empty set selects the pinned deterministic probe used
    // by context 0.1.  Non-empty sets are rejected until a versioned export,
    // root-signature and shader-table ABI is published; the context never
    // guesses how arbitrary DXIL should be wired into the retained pipeline.
    std::vector<std::byte> ray_generation_dxil;
    std::vector<std::byte> miss_dxil;
    std::vector<std::byte> closest_hit_dxil;
    // Versioned production library containing RayGen, Miss and ClosestHit.
    // This is loaded from the pinned build artifact by SceneRenderer; the
    // embedded marker probe remains available only when this vector is empty.
    std::string full_frame_contract;
    std::vector<std::byte> full_frame_library_dxil;

    [[nodiscard]] bool complete() const noexcept {
        return !ray_generation_dxil.empty() && !miss_dxil.empty() &&
            !closest_hit_dxil.empty();
    }
};

struct NativeD3D12RayTracingContextOptions final {
    // A WARP probe is recorded as an explicit fallback only.  WARP is never
    // reported as hardware ray tracing support.
    bool probe_warp_fallback{true};
    bool enable_debug_layer{};
    std::uint32_t output_width{1U};
    std::uint32_t output_height{1U};
    std::size_t max_geometry_count{native_d3d12_raytracing_context_max_geometry_count};
    std::uint64_t max_resource_bytes{native_d3d12_raytracing_context_max_resource_bytes};
    // Validated once at initialize() and uploadable per frame through
    // set_camera().  The full-frame DXR contract consumes these constants;
    // the marker probe deliberately reports camera_shader_consumed=false.
    NativeD3D12RayTracingCamera camera{};
    NativeD3D12RayTracingLighting lighting{};
    // Empty uses the bounded default material for every geometry.  Otherwise
    // exactly one entry per geometry is required, keyed by geometry_id.
    std::vector<NativeD3D12RayTracingMaterial> materials;
    NativeD3D12RayTracingSynchronizationPolicy synchronization_policy{
        NativeD3D12RayTracingSynchronizationPolicy::wait_for_completion};
    NativeD3D12RayTracingContextShaderSet shaders;
    // Optional borrowed interfaces published by the Runtime-private SDL_GPU
    // D3D12 device bridge.  They are non-owning inputs: initialize() performs
    // QueryInterface/AddRef and shutdown() releases only those retained
    // references, never SDL_GPU's ownership objects.  A queue without its
    // matching device is rejected rather than silently creating a second RHI.
    void* borrowed_device{};
    void* borrowed_command_queue{};
};

struct NativeD3D12RayTracingGeometry final {
    std::string geometry_id;
    // Position-only triangle input.  The context owns its GPU upload after
    // ensure_scene(); the caller may release these vectors immediately.
    std::vector<float> position_xyz;
    std::vector<std::uint32_t> indices;
    bool allow_update{true};
};

struct NativeD3D12RayTracingScene final {
    std::string scene_id;
    std::uint64_t revision{1U};
    std::vector<NativeD3D12RayTracingGeometry> geometries;
    bool allow_update{true};
};

struct NativeD3D12RayTracingContextReceipt final {
    std::string schema{std::string(native_d3d12_raytracing_context_schema)};
    std::string operation;
    NativeD3D12RayTracingContextState state{
        NativeD3D12RayTracingContextState::uninitialized};
    NativeD3D12RayTracingContextFailureStage failure_stage{
        NativeD3D12RayTracingContextFailureStage::none};
    std::string code;
    std::string detail;
    std::string backend{"d3d12"};
    std::string device_name;
    bool native_handle_exposed{};
    bool fallback_active{};
    bool initialized{};
    bool device_ready{};
    bool command_queue_ready{};
    bool fence_ready{};
    bool scene_received{};
    bool scene_changed{};
    bool blas_ready{};
    bool tlas_ready{};
    bool shader_pipeline_ready{};
    bool shader_table_ready{};
    bool output_resource_ready{};
    bool build_submitted{};
    bool build_completed{};
    bool update_submitted{};
    bool update_completed{};
    bool trace_submitted{};
    bool trace_completed{};
    bool readback_completed{};
    bool synchronization_completed{};
    bool shutdown_completed{};
    bool borrowed_device_requested{};
    bool borrowed_command_queue_requested{};
    bool device_adopted{};
    bool command_queue_adopted{};
    bool shared_device{};
    bool shared_command_queue{};
    bool output_copy_submitted{};
    bool output_copy_completed{};
    bool camera_ready{};
    bool camera_shader_consumed{};
    std::string camera_schema;
    std::uint64_t camera_fingerprint{};
    bool shading_resources_ready{};
    bool linear_radiance_shader_consumed{};
    bool claims_rtgi{};
    std::string shading_schema;
    std::uint64_t shading_fingerprint{};
    std::uint32_t shading_material_count{};
    std::string synchronization_policy{"wait-for-completion"};
    bool completion_pending{};
    std::uint64_t submitted_fence_value{};
    std::uint64_t completed_fence_value{};
    bool full_frame_shader_ready{};
    std::string shader_contract;
    std::uint64_t generation{};
    std::uint64_t scene_generation{};
    std::uint64_t resource_generation{};
    std::uint64_t scene_revision{};
    std::uint32_t raytracing_tier{};
    std::uint32_t geometry_count{};
    std::uint32_t instance_count{};
    std::uint32_t output_width{};
    std::uint32_t output_height{};
    std::uint32_t output_pixel_stride_bytes{};
    std::uint64_t vertex_buffer_bytes{};
    std::uint64_t index_buffer_bytes{};
    std::uint64_t blas_result_bytes{};
    std::uint64_t blas_scratch_bytes{};
    std::uint64_t tlas_result_bytes{};
    std::uint64_t tlas_scratch_bytes{};
    std::uint64_t shader_table_bytes{};
    std::uint64_t output_bytes{};
    std::uint64_t output_readback_bytes{};
    std::uint32_t output_sentinel{};
    std::uint32_t output_hit{};
    bool output_radiance_valid{};
    std::array<float, 4U> output_radiance_probe{};
    std::uint64_t output_hash{};
    NativeD3D12RayTracingOutputSurfaceMetadata output_surface;
};

// The context performs no work in its constructor.  initialize() is explicit
// and idempotent; all later calls reuse the same D3D12 device/queue/fence and
// private resources until shutdown().
class NativeD3D12RayTracingContext final {
public:
    explicit NativeD3D12RayTracingContext(
        NativeD3D12RayTracingContextOptions options = {});
    ~NativeD3D12RayTracingContext();
    NativeD3D12RayTracingContext(const NativeD3D12RayTracingContext&) = delete;
    NativeD3D12RayTracingContext& operator=(const NativeD3D12RayTracingContext&) = delete;

    [[nodiscard]] NativeD3D12RayTracingContextReceipt initialize();
    [[nodiscard]] NativeD3D12RayTracingContextReceipt ensure_scene(
        const NativeD3D12RayTracingScene& scene);
    [[nodiscard]] NativeD3D12RayTracingContextReceipt build_or_update();
    [[nodiscard]] NativeD3D12RayTracingContextReceipt trace();
    // Replace the bounded world-space camera constants without rebuilding the
    // retained scene/BLAS/TLAS.  The next full-frame trace uploads the values;
    // a marker-probe trace keeps the values in the receipt but does not claim
    // shader consumption because that legacy shader has no CBV parameter.
    [[nodiscard]] NativeD3D12RayTracingContextReceipt set_camera(
        const NativeD3D12RayTracingCamera& camera);
    // Replace scene-linear material/light inputs without rebuilding BLAS or
    // TLAS. The geometry ids are validated against the retained canonical
    // scene, and the next full-frame trace refreshes only shading resources.
    [[nodiscard]] NativeD3D12RayTracingContextReceipt set_shading(
        const NativeD3D12RayTracingLighting& lighting,
        const std::vector<NativeD3D12RayTracingMaterial>& materials);
    // Poll or explicitly wait for the last asynchronous trace/copy submission.
    // The default is non-blocking; pass true at a hand-off boundary where the
    // output resource must be complete before the caller consumes it.
    [[nodiscard]] NativeD3D12RayTracingContextReceipt synchronize(
        bool wait_for_completion = false);
    [[nodiscard]] NativeD3D12RayTracingContextReceipt readback();
    [[nodiscard]] NativeD3D12RayTracingContextReceipt shutdown();

    // Runtime-only output interop boundary.  The metadata method is safe for
    // observation.  The private view contains borrowed native pointers and is
    // never included in a receipt; validate it immediately before a GPU copy.
    [[nodiscard]] NativeD3D12RayTracingOutputSurfaceMetadata
    output_surface_metadata() const;
    [[nodiscard]] NativeD3D12RayTracingContextPrivateOutputView
    private_output_surface_view() const;
    [[nodiscard]] bool is_private_output_surface_view_current(
        const NativeD3D12RayTracingContextPrivateOutputView& view) const noexcept;
    [[nodiscard]] NativeD3D12RayTracingContextReceipt copy_output_to(
        const NativeD3D12RayTracingContextPrivateOutputView& view,
        void* destination_resource);

    [[nodiscard]] NativeD3D12RayTracingContextReceipt status() const;
    [[nodiscard]] NativeD3D12RayTracingContextState state() const noexcept;
    [[nodiscard]] std::uint64_t generation() const noexcept;
    [[nodiscard]] std::uint64_t scene_generation() const noexcept;
    [[nodiscard]] bool is_shutdown() const noexcept;

private:
    struct Impl;
    [[nodiscard]] static bool ensure_trace_pipeline(Impl& impl,
                                                    std::string& code,
                                                    std::string& detail);
    [[nodiscard]] static bool ensure_full_frame_shading_resources(
        Impl& impl, std::string& code, std::string& detail);
    static void save_result(Impl& impl,
                            NativeD3D12RayTracingContextFailureStage stage,
                            std::string_view code, std::string_view detail);
    [[nodiscard]] static NativeD3D12RayTracingContextReceipt receipt_from(
        const Impl& impl, std::string_view operation);
    static void mark_unsupported(Impl& impl,
                                 NativeD3D12RayTracingContextFailureStage stage,
                                 std::string_view operation,
                                 std::string_view code, std::string_view detail,
                                 NativeD3D12RayTracingContextReceipt& receipt);
    std::unique_ptr<Impl> impl_;
};

} // namespace noemancer
