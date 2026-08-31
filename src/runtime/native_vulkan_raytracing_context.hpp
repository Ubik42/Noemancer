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

struct NativeVulkanRayTracingContextOptions final {
    // The persistent Vulkan path is capability-gated.  When it is not yet
    // available, allow_fallback keeps authoring and tests usable without ever
    // pretending that CPU work is a Vulkan trace.
    bool allow_fallback{true};
    std::size_t maximum_triangles{4096U};
    std::uint32_t output_width{1U};
    std::uint32_t output_height{1U};
    std::uint32_t output_depth{1U};
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

struct NativeVulkanRayTracingTraceRequest final {
    std::array<float, 3U> origin{0.0F, 0.0F, -1.0F};
    std::array<float, 3U> direction{0.0F, 0.0F, 1.0F};
    float minimum_distance{0.0F};
    float maximum_distance{1.0e6F};
};

// Plain, bounded evidence for one context operation.  Native Vulkan handles
// (VkInstance/VkDevice/VkQueue/VkFence/AS/SBT/output) are intentionally not
// represented here; they remain private to the eventual backend Impl.
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
    std::uint32_t output_value{};
    std::uint32_t output_hit{};
    std::uint64_t output_hash{};
    std::uint64_t output_bytes{};
    std::uint64_t readback_bytes{};
};

// A single-owner RAII context boundary for persistent Vulkan RT work.  The
// first implementation intentionally exposes the complete lifecycle and a
// deterministic CPU fallback while the native extraction from the short-lived
// executor is pending.  It never calls the short-lived executor and never
// projects fallback work as NativeVulkanRayTracingContextState::ready.
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
