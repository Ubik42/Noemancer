#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace noemancer {

// Renderer/runtime-private contract for handing one completed RT output
// frame to the presentation consumer.  It describes ownership and
// synchronization, never a D3D12/Vulkan handle or descriptor.
inline constexpr std::string_view raytracing_output_interop_schema =
    "noemancer.raytracing-output-interop/0.1";
inline constexpr std::size_t raytracing_output_interop_max_text_bytes = 256U;
inline constexpr std::uint32_t raytracing_output_interop_max_extent = 16384U;

enum class RayTracingOutputInteropBackend : std::uint8_t {
    unknown = 0U,
    d3d12 = 1U,
    vulkan = 2U,

    Unknown = unknown,
    D3D12 = d3d12,
    Vulkan = vulkan,
};

enum class RayTracingOutputInteropState : std::uint8_t {
    uninitialized = 0U,
    configured = 1U,
    produced = 2U,
    consumed = 3U,
    shutdown = 4U,

    Uninitialized = uninitialized,
    Configured = configured,
    Produced = produced,
    Consumed = consumed,
    Shutdown = shutdown,
};

enum class RayTracingOutputInteropMode : std::uint8_t {
    unavailable = 0U,
    direct_share = 1U,
    gpu_copy = 2U,

    Unavailable = unavailable,
    DirectShare = direct_share,
    GpuCopy = gpu_copy,
};

enum class RayTracingOutputInteropLayout : std::uint8_t {
    undefined = 0U,
    ray_tracing_output = 1U,
    copy_source = 2U,
    shader_read = 3U,

    Undefined = undefined,
    RayTracingOutput = ray_tracing_output,
    CopySource = copy_source,
    ShaderRead = shader_read,
};

enum class RayTracingOutputInteropOwnership : std::uint8_t {
    none = 0U,
    ray_tracing = 1U,
    renderer = 2U,

    None = none,
    RayTracing = ray_tracing,
    Renderer = renderer,
};

enum class RayTracingOutputInteropDeviceCompatibility : std::uint8_t {
    unknown = 0U,
    same_device = 1U,
    cross_device = 2U,
    incompatible = 3U,

    Unknown = unknown,
    SameDevice = same_device,
    CrossDevice = cross_device,
    Incompatible = incompatible,
};

[[nodiscard]] std::string_view raytracing_output_interop_backend_name(
    RayTracingOutputInteropBackend backend) noexcept;
[[nodiscard]] std::string_view raytracing_output_interop_state_name(
    RayTracingOutputInteropState state) noexcept;
[[nodiscard]] std::string_view raytracing_output_interop_mode_name(
    RayTracingOutputInteropMode mode) noexcept;
[[nodiscard]] std::string_view raytracing_output_interop_layout_name(
    RayTracingOutputInteropLayout layout) noexcept;
[[nodiscard]] std::string_view raytracing_output_interop_ownership_name(
    RayTracingOutputInteropOwnership ownership) noexcept;
[[nodiscard]] std::string_view raytracing_output_interop_device_compatibility_name(
    RayTracingOutputInteropDeviceCompatibility compatibility) noexcept;

struct RayTracingOutputInteropResource final {
    // All identity fields are logical, stable values.  They are not native
    // pointers, API handles or serialized descriptors.
    std::string resource_id;
    RayTracingOutputInteropBackend backend{RayTracingOutputInteropBackend::unknown};
    std::string device_id;
    std::uint32_t width{};
    std::uint32_t height{};
    std::string format;
    std::uint64_t resource_generation{};
};

struct RayTracingOutputInteropProducerFrame final {
    RayTracingOutputInteropResource resource;
    RayTracingOutputInteropLayout layout{RayTracingOutputInteropLayout::undefined};
    RayTracingOutputInteropOwnership ownership{RayTracingOutputInteropOwnership::none};
    std::uint64_t producer_sync_generation{};
    bool producer_complete{};
};

struct RayTracingOutputInteropConsumerRequest final {
    RayTracingOutputInteropResource resource;
    std::uint64_t expected_producer_sync_generation{};
    RayTracingOutputInteropLayout desired_layout{RayTracingOutputInteropLayout::shader_read};
    RayTracingOutputInteropOwnership desired_ownership{RayTracingOutputInteropOwnership::renderer};
    bool allow_direct_share{true};
    bool direct_share_supported{true};
    bool allow_gpu_copy{true};
    bool gpu_copy_supported{};
};

// Bounded, Agent-safe evidence for one interop transition.  `visual_path`
// tells SceneRenderer whether a completed RT output can be promoted into the
// visible frame; an unavailable path must remain an explicit raster fallback.
struct RayTracingOutputInteropReceipt final {
    std::string schema{std::string(raytracing_output_interop_schema)};
    std::string operation;
    RayTracingOutputInteropState state{RayTracingOutputInteropState::uninitialized};
    RayTracingOutputInteropMode mode{RayTracingOutputInteropMode::unavailable};
    RayTracingOutputInteropDeviceCompatibility device_compatibility{
        RayTracingOutputInteropDeviceCompatibility::unknown};
    std::string code;
    std::string detail;
    std::string resource_id;
    std::string backend;
    std::string format;
    std::uint32_t width{};
    std::uint32_t height{};
    std::uint64_t resource_generation{};
    std::uint64_t producer_sync_generation{};
    std::uint64_t consumer_sync_generation{};
    RayTracingOutputInteropLayout producer_layout{RayTracingOutputInteropLayout::undefined};
    RayTracingOutputInteropLayout consumer_layout{RayTracingOutputInteropLayout::undefined};
    RayTracingOutputInteropOwnership producer_ownership{RayTracingOutputInteropOwnership::none};
    RayTracingOutputInteropOwnership consumer_ownership{RayTracingOutputInteropOwnership::none};
    bool accepted{};
    bool completed{};
    bool producer_ready{};
    bool consumer_ready{};
    bool ownership_transferred{};
    bool generation_stale{};
    bool resized{};
    bool visual_path_eligible{};
    bool native_handles_exposed{};
    std::string visual_path{"raster-pbr-fallback"};
};

class RayTracingOutputInterop final {
public:
    RayTracingOutputInterop() = default;
    ~RayTracingOutputInterop() = default;
    RayTracingOutputInterop(const RayTracingOutputInterop&) = delete;
    RayTracingOutputInterop& operator=(const RayTracingOutputInterop&) = delete;

    // Establish one private output view identity.  A live resource must be
    // resized through resize(), so generation changes cannot be accidental.
    [[nodiscard]] RayTracingOutputInteropReceipt configure(
        const RayTracingOutputInteropResource& resource);

    // Publish a producer-complete frame.  The frame must use the configured
    // resource generation and ray-tracing output layout/ownership.
    [[nodiscard]] RayTracingOutputInteropReceipt publish(
        const RayTracingOutputInteropProducerFrame& frame);

    // Choose direct-share first, then an explicitly supported GPU copy.  A
    // failed selection leaves the produced frame available for a later retry.
    [[nodiscard]] RayTracingOutputInteropReceipt acquire(
        const RayTracingOutputInteropConsumerRequest& request);

    // Resize/recreate the private output view.  The stable resource identity,
    // backend, device and format remain fixed while resource_generation rises.
    [[nodiscard]] RayTracingOutputInteropReceipt resize(
        const RayTracingOutputInteropResource& resource);

    // Terminal and idempotent.  All post-shutdown operations fail closed.
    [[nodiscard]] RayTracingOutputInteropReceipt shutdown() noexcept;

    [[nodiscard]] RayTracingOutputInteropReceipt status() const;

private:
    [[nodiscard]] RayTracingOutputInteropReceipt make_receipt(
        std::string_view operation) const;
    [[nodiscard]] RayTracingOutputInteropReceipt reject(
        std::string_view operation,
        std::string_view code,
        std::string_view detail,
        bool stale = false) const;
    [[nodiscard]] bool valid_resource(
        const RayTracingOutputInteropResource& resource) const noexcept;
    [[nodiscard]] bool same_resource(
        const RayTracingOutputInteropResource& left,
        const RayTracingOutputInteropResource& right) const noexcept;
    [[nodiscard]] RayTracingOutputInteropDeviceCompatibility device_compatibility(
        const RayTracingOutputInteropResource& producer,
        const RayTracingOutputInteropResource& consumer) const noexcept;

    std::optional<RayTracingOutputInteropResource> resource_;
    RayTracingOutputInteropState state_{RayTracingOutputInteropState::uninitialized};
    RayTracingOutputInteropMode last_mode_{RayTracingOutputInteropMode::unavailable};
    RayTracingOutputInteropDeviceCompatibility last_device_compatibility{
        RayTracingOutputInteropDeviceCompatibility::unknown};
    RayTracingOutputInteropLayout producer_layout_{RayTracingOutputInteropLayout::undefined};
    RayTracingOutputInteropLayout consumer_layout_{RayTracingOutputInteropLayout::undefined};
    RayTracingOutputInteropOwnership producer_ownership_{RayTracingOutputInteropOwnership::none};
    RayTracingOutputInteropOwnership consumer_ownership_{RayTracingOutputInteropOwnership::none};
    std::uint64_t producer_sync_generation_{};
    std::uint64_t consumer_sync_generation_{};
};

} // namespace noemancer
