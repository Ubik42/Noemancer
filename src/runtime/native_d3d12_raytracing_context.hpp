#pragma once

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

struct NativeD3D12RayTracingContextShaderSet final {
    // Optional DXIL blobs for a caller-owned RayGen/Miss/ClosestHit shader
    // contract.  Empty blobs intentionally leave trace() unsupported while
    // AS build remains useful and testable.
    std::vector<std::byte> ray_generation_dxil;
    std::vector<std::byte> miss_dxil;
    std::vector<std::byte> closest_hit_dxil;

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
    NativeD3D12RayTracingContextShaderSet shaders;
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
    std::uint64_t output_hash{};
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
    [[nodiscard]] NativeD3D12RayTracingContextReceipt readback();
    [[nodiscard]] NativeD3D12RayTracingContextReceipt shutdown();

    [[nodiscard]] NativeD3D12RayTracingContextReceipt status() const;
    [[nodiscard]] NativeD3D12RayTracingContextState state() const noexcept;
    [[nodiscard]] std::uint64_t generation() const noexcept;
    [[nodiscard]] std::uint64_t scene_generation() const noexcept;
    [[nodiscard]] bool is_shutdown() const noexcept;

private:
    struct Impl;
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
