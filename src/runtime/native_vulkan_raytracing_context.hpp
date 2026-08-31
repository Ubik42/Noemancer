#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>

namespace noemancer {

inline constexpr std::string_view native_vulkan_raytracing_context_schema =
    "noemancer.native-vulkan-raytracing-context/0.1";
inline constexpr std::string_view native_vulkan_raytracing_output_image_contract =
    "noemancer.native-vulkan-raytracing-output-image/0.1";
inline constexpr std::string_view native_vulkan_raytracing_full_frame_shader_contract =
    "noemancer.native-rt-full-frame/0.1";
// The camera contract is deliberately separate from the pinned probe shader
// ABI. It describes the bounded plain-data input consumed by the embedded
// RayGen camera block when the native Vulkan path is active. Fallback receipts
// keep the same contract visible but never claim shader consumption.
inline constexpr std::string_view native_vulkan_raytracing_camera_contract =
    "noemancer.native-vulkan-raytracing-camera/0.1";
inline constexpr std::uint32_t native_vulkan_raytracing_camera_contract_version = 1U;
inline constexpr std::size_t native_vulkan_raytracing_context_max_text_bytes = 256U;
inline constexpr std::size_t native_vulkan_raytracing_context_hard_max_triangles = 65536U;

enum class NativeVulkanRayTracingContextState : std::uint8_t {
    uninitialized = 0U,
    ready = 1U,
    unsupported = 2U,
    fallback = 3U,
    error = 4U,
    shutdown = 5U,
};

enum class NativeVulkanRayTracingContextFailureStage : std::uint8_t {
    none = 0U,
    loader = 1U,
    instance = 2U,
    physical_device = 3U,
    device = 4U,
    acceleration_structure = 5U,
    pipeline = 6U,
    scene = 7U,
    trace = 8U,
    readback = 9U,
    shutdown = 10U,
};

[[nodiscard]] std::string_view native_vulkan_raytracing_context_state_name(
    NativeVulkanRayTracingContextState state) noexcept;
[[nodiscard]] std::string_view native_vulkan_raytracing_context_failure_stage_name(
    NativeVulkanRayTracingContextFailureStage stage) noexcept;

// Runtime-only adoption input for an SDL_GPU Vulkan device.  The fields are
// intentionally opaque pointers rather than Vulkan types so this contract
// cannot leak third-party handles into Engine/Agent plain-data schemas.  A
// non-empty value must provide all four handles; the context borrows them and
// never destroys the instance, physical device, device, or queue.
struct NativeVulkanRayTracingBorrowedDevice final {
    void* instance{};
    void* physical_device{};
    void* device{};
    void* queue{};
    std::uint32_t queue_family_index{};
};

struct NativeVulkanRayTracingContextOptions final {
    // The persistent Vulkan path is capability-gated.  When it is not yet
    // available, allow_fallback keeps authoring and tests usable without ever
    // pretending that CPU work is a Vulkan trace.
    bool allow_fallback{true};
    std::size_t maximum_triangles{4096U};
    std::uint32_t output_width{1U};
    std::uint32_t output_height{1U};
    std::uint32_t output_depth{1U};
    NativeVulkanRayTracingBorrowedDevice borrowed_device{};
};

struct NativeVulkanRayTracingTriangle final {
    std::array<std::array<float, 3U>, 3U> positions{};
};

struct NativeVulkanRayTracingScene final {
    std::span<const NativeVulkanRayTracingTriangle> triangles;
    // topology_revision changes when the triangle/instance layout changes;
    // content_revision changes when vertex values change in-place.  These are
    // authoring revisions, not Vulkan generations or native handles.
    std::uint64_t topology_revision{};
    std::uint64_t content_revision{};
};

// Versioned, bounded, runtime-only camera input. This is intentionally plain
// data: no Vulkan/SDL handles, pointers, callbacks, or opaque engine objects
// cross the trace boundary. The native probe maps it to a std140-compatible
// camera block (position, orthonormal basis, lens/clip values).
struct NativeVulkanRayTracingCameraInput final {
    std::uint32_t contract_version{native_vulkan_raytracing_camera_contract_version};
    std::array<float, 3U> position{0.0F, 0.0F, -1.0F};
    std::array<float, 3U> forward{0.0F, 0.0F, 1.0F};
    std::array<float, 3U> up{0.0F, 1.0F, 0.0F};
    float vertical_fov_degrees{60.0F};
    float near_distance{0.01F};
    float far_distance{1.0e6F};
};

struct NativeVulkanRayTracingTraceRequest final {
    std::array<float, 3U> origin{0.0F, 0.0F, -1.0F};
    std::array<float, 3U> direction{0.0F, 0.0F, 1.0F};
    float minimum_distance{0.0F};
    float maximum_distance{1.0e6F};
    // Camera input is opt-in so existing ray requests remain source- and
    // behavior-compatible. Native receipts set camera_shader_consumed only
    // after a successful trace has consumed the validated descriptor.
    bool camera_enabled{};
    NativeVulkanRayTracingCameraInput camera{};
};

// Plain, bounded evidence for one context operation.  Native Vulkan handles
// (VkInstance/VkDevice/VkQueue/VkFence/AS/SBT/output) are intentionally not
// represented here; they remain private to the PImpl backend.
struct NativeVulkanRayTracingContextReceipt final {
    std::string schema{std::string(native_vulkan_raytracing_context_schema)};
    NativeVulkanRayTracingContextState state{NativeVulkanRayTracingContextState::uninitialized};
    NativeVulkanRayTracingContextFailureStage failure_stage{
        NativeVulkanRayTracingContextFailureStage::none};
    std::string code;
    std::string detail;

    bool initialized{};
    bool persistent_backend{};
    bool fallback_active{};
    bool scene_ready{};
    bool scene_rebuilt{};
    bool scene_updated{};
    bool scene_reused{};
    bool build_submitted{};
    bool build_completed{};
    bool trace_submitted{};
    bool trace_completed{};
    bool readback_completed{};
    bool camera_requested{};
    bool camera_valid{};
    bool camera_shader_consumed{};
    // The embedded SPIR-V probe is the pinned full-frame shader ABI.  A
    // caller must never infer this from a generic Vulkan pipeline alone;
    // the explicit contract string is the version gate.
    bool full_frame_shader_ready{};
    // This records whether TraceRays covered the requested output extent.  It
    // is intentionally independent from shader readiness and camera
    // consumption; a one-texel probe is never reported as a full-frame pass.
    bool full_frame_dispatch{};
    // The output image is runtime-private.  These fields describe a bounded
    // access contract only; no VkImage/VkImageView/native synchronization
    // handle crosses this plain-data receipt.
    bool output_image_live{};
    bool output_image_view_live{};
    bool output_image_runtime_private{};
    bool output_image_interop_ready{};
    bool output_image_external_import_supported{};
    bool output_image_same_device_required{};
    bool output_image_layout_ready{};
    bool output_image_sync_complete{};
    bool output_image_trace_written{};
    bool output_image_cpu_readback_supported{};
    bool resources_live{};
    bool shutdown{};

    std::uint64_t generation{};
    std::uint64_t scene_topology_revision{};
    std::uint64_t scene_content_revision{};
    std::uint64_t scene_fingerprint{};
    std::uint32_t triangle_count{};
    std::uint32_t output_width{};
    std::uint32_t output_height{};
    std::uint32_t output_depth{};
    std::uint32_t output_image_queue_family{};
    std::uint32_t output_value{};
    std::uint32_t output_hit{};
    std::uint64_t output_hash{};
    std::uint64_t output_bytes{};
    std::uint64_t readback_bytes{};
    std::uint64_t output_image_generation{};
    std::uint64_t output_image_bytes{};
    std::uint64_t output_image_sync_value{};
    std::uint32_t camera_contract_version{};
    std::string camera_contract;
    std::string camera_boundary;
    std::string shader_contract;
    std::string output_image_contract{std::string(native_vulkan_raytracing_output_image_contract)};
    std::string output_image_format;
    std::string output_image_layout;
    std::string output_image_access;
    std::string output_image_sync_kind;
    std::string output_image_interop_boundary;
};

// A single-owner RAII context boundary for persistent Vulkan RT work.  When
// the device exposes the required acceleration-structure and ray-tracing
// pipeline features, the implementation owns the loader/instance/device/
// queue/command stream plus persistent geometry, BLAS, TLAS, scratch,
// descriptor, pipeline, SBT and output/readback resources.  It supports
// rebuild/update submissions and separate trace/readback calls while keeping
// every native handle private to the PImpl.  Devices without those capabilities
// use a deterministic CPU fallback and never project fallback work as `ready`.
class NativeVulkanRayTracingContext final {
public:
    explicit NativeVulkanRayTracingContext(
        const NativeVulkanRayTracingContextOptions& options = {});
    ~NativeVulkanRayTracingContext();
    NativeVulkanRayTracingContext(const NativeVulkanRayTracingContext&) = delete;
    NativeVulkanRayTracingContext& operator=(const NativeVulkanRayTracingContext&) = delete;

    [[nodiscard]] NativeVulkanRayTracingContextReceipt initialize();
    [[nodiscard]] NativeVulkanRayTracingContextReceipt ensure_scene(
        const NativeVulkanRayTracingScene& scene);
    [[nodiscard]] NativeVulkanRayTracingContextReceipt build_or_update();
    [[nodiscard]] NativeVulkanRayTracingContextReceipt trace(
        const NativeVulkanRayTracingTraceRequest& request = {});
    [[nodiscard]] NativeVulkanRayTracingContextReceipt readback();
    [[nodiscard]] NativeVulkanRayTracingContextReceipt shutdown() noexcept;

    [[nodiscard]] bool initialized() const noexcept;
    [[nodiscard]] bool scene_ready() const noexcept;
    [[nodiscard]] std::uint64_t generation() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace noemancer
