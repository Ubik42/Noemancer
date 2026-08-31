#pragma once

#include "runtime/raytracing_context_session.hpp"
#include "runtime/scene_raytracing_geometry_cache.hpp"
#include "runtime/sdl_gpu_native_device_bridge.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace noemancer {

// Runtime-only adapter from the renderer's derived geometry snapshot to one
// long-lived renderer-neutral ray-tracing session.  It owns no Scene/World
// authority and never publishes a native API handle.
inline constexpr std::string_view scene_raytracing_bridge_schema =
    "noemancer.scene-raytracing-bridge/0.1";
inline constexpr std::size_t scene_raytracing_bridge_max_text_bytes = 512U;

struct SceneRayTracingBridgeOptions final {
    bool allow_fallback{true};
    std::uint32_t output_width{1U};
    std::uint32_t output_height{1U};
    std::uint64_t graph_generation{1U};
    // Runtime-private borrowed SDL_GPU handles. They are consumed only by the
    // selected native context and never copied into a receipt.
    SdlGpuNativeDeviceHandles native_device;
};

struct SceneRayTracingBridgeRequest final {
    // Canonical values are "d3d12" and "vulkan".  The bridge accepts their
    // ASCII case variants and reports the canonical backend in its receipt.
    std::string backend{"vulkan"};
    bool enabled{true};
    bool request_trace{true};
    bool request_readback{true};
};

struct SceneRayTracingBridgeReceipt final {
    std::string schema{std::string(scene_raytracing_bridge_schema)};
    std::string backend;
    std::string cache_state{"empty"};
    std::string visual_path{"raster-pbr"};
    std::string fallback_code{"none"};
    std::string fallback_detail;
    std::string plan_fingerprint;
    std::string code;
    std::string detail;

    // These are the stable production fields consumed by the renderer and by
    // the Agent observation layer.  `requested` means enabled + trace was
    // requested; `native_trace_ready` never promotes a native output to the
    // visible path because SDL_GPU output sharing is not wired here yet.
    bool requested{};
    bool scene_accepted{};
    bool native_as_ready{};
    bool native_trace_ready{};
    bool shared_device{};
    bool shared_queue{};
    bool output_resource_live{};
    bool output_trace_written{};
    bool output_transfer_candidate{};
    std::uint64_t output_resource_generation{};
    std::string output_format;

    // Additional bounded lifecycle evidence keeps fallback and reuse
    // decisions inspectable without exposing backend objects.
    bool enabled{};
    bool trace_requested{};
    bool readback_requested{};
    bool plan_valid{};
    bool plan_supported{};
    bool session_executed{};
    bool fallback_active{};
    bool failed{};
    bool content_updated{};
    bool topology_rebuilt{};
    bool shutdown_completed{};
    std::uint64_t frame_generation{};
    std::uint64_t graph_generation{};
    std::uint64_t topology_revision{};
    std::uint64_t content_revision{};
    std::uint32_t triangle_count{};
};

// A persistent bridge is intentionally separate from scene cache ownership:
// callers submit the current cache snapshot each frame, while the selected
// backend session and graph previous-resource identities remain resident in
// this object until shutdown().
class SceneRayTracingBridge final {
public:
    explicit SceneRayTracingBridge(SceneRayTracingBridgeOptions options = {});
    ~SceneRayTracingBridge();
    SceneRayTracingBridge(const SceneRayTracingBridge&) = delete;
    SceneRayTracingBridge& operator=(const SceneRayTracingBridge&) = delete;

    // Submit one current cache snapshot.  The session and previous graph
    // identities stay resident across calls; this is the primary runtime
    // update entry point.
    [[nodiscard]] SceneRayTracingBridgeReceipt update(
        const SceneRayTracingBridgeRequest& request,
        const SceneRayTracingGeometryCacheSnapshot& snapshot);
    // Convenience form for renderer call sites that already keep the backend
    // selector and request flags as separate values.
    [[nodiscard]] SceneRayTracingBridgeReceipt update(
        std::string_view backend,
        const SceneRayTracingGeometryCacheSnapshot& snapshot,
        bool enabled = true,
        bool request_trace = true,
        bool request_readback = true);
    // `execute` is retained as a semantic alias for tools that treat this
    // bridge as a plan executor rather than a frame update service.
    [[nodiscard]] SceneRayTracingBridgeReceipt execute(
        const SceneRayTracingBridgeRequest& request,
        const SceneRayTracingGeometryCacheSnapshot& snapshot);
    [[nodiscard]] RayTracingContextSessionOutputTransferReceipt transfer_output_to(
        void* destination_resource);
    [[nodiscard]] SceneRayTracingBridgeReceipt shutdown();
    // Plain-data observation of the most recent update/shutdown result.
    [[nodiscard]] SceneRayTracingBridgeReceipt observation() const;
    [[nodiscard]] SceneRayTracingBridgeReceipt status() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace noemancer
