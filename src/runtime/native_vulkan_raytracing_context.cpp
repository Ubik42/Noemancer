#include "runtime/native_vulkan_raytracing_context.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace noemancer {

namespace {

constexpr float kEpsilon = 1.0e-6F;
constexpr float kMaximumCoordinate = 1.0e6F;
constexpr float kMaximumDistance = 1.0e9F;
constexpr std::uint32_t kHitMarker = 0x48495421U;
constexpr std::uint32_t kMissMarker = 0x4D495353U;
constexpr std::uint64_t kFnvOffsetBasis = 1469598103934665603ULL;
constexpr std::uint64_t kFnvPrime = 1099511628211ULL;

std::string bounded_text(const std::string_view value) {
    return std::string(value.substr(0U, std::min(value.size(), native_vulkan_raytracing_context_max_text_bytes)));
}

bool finite_bounded(const float value, const float maximum = kMaximumCoordinate) noexcept {
    return std::isfinite(value) && std::abs(value) <= maximum;
}

struct Vec3 final {
    float x{};
    float y{};
    float z{};
};

Vec3 to_vec3(const std::array<float, 3U>& value) noexcept { return {value[0], value[1], value[2]}; }

Vec3 operator-(const Vec3 left, const Vec3 right) noexcept {
    return {left.x - right.x, left.y - right.y, left.z - right.z};
}

Vec3 cross(const Vec3 left, const Vec3 right) noexcept {
    return {left.y * right.z - left.z * right.y,
            left.z * right.x - left.x * right.z,
            left.x * right.y - left.y * right.x};
}

float dot(const Vec3 left, const Vec3 right) noexcept {
    return left.x * right.x + left.y * right.y + left.z * right.z;
}

bool finite_vec(const Vec3 value) noexcept {
    return finite_bounded(value.x) && finite_bounded(value.y) && finite_bounded(value.z);
}

bool valid_scene_triangle(const NativeVulkanRayTracingTriangle& triangle) noexcept {
    for (const auto& position : triangle.positions) {
        if (!finite_vec(to_vec3(position))) return false;
    }
    return true;
}

bool valid_trace_request(const NativeVulkanRayTracingTraceRequest& request) noexcept {
    const auto origin = to_vec3(request.origin);
    const auto direction = to_vec3(request.direction);
    const auto direction_length_squared = dot(direction, direction);
    return finite_vec(origin) && finite_vec(direction) && std::isfinite(direction_length_squared) &&
           direction_length_squared > kEpsilon * kEpsilon && finite_bounded(request.minimum_distance, kMaximumDistance) &&
           finite_bounded(request.maximum_distance, kMaximumDistance) && request.minimum_distance >= 0.0F &&
           request.maximum_distance > request.minimum_distance;
}

std::uint64_t hash_u32(std::uint64_t hash, const std::uint32_t value) noexcept {
    for (std::uint32_t shift = 0U; shift < 32U; shift += 8U) {
        hash ^= static_cast<std::uint8_t>((value >> shift) & 0xffU);
        hash *= kFnvPrime;
    }
    return hash;
}

std::uint64_t hash_float(std::uint64_t hash, const float value) noexcept {
    return hash_u32(hash, std::bit_cast<std::uint32_t>(value));
}

std::uint64_t scene_fingerprint(const NativeVulkanRayTracingScene& scene) noexcept {
    auto hash = kFnvOffsetBasis;
    hash = hash_u32(hash, static_cast<std::uint32_t>(scene.triangles.size()));
    hash = hash_u32(hash, static_cast<std::uint32_t>(scene.topology_revision));
    hash = hash_u32(hash, static_cast<std::uint32_t>(scene.topology_revision >> 32U));
    hash = hash_u32(hash, static_cast<std::uint32_t>(scene.content_revision));
    hash = hash_u32(hash, static_cast<std::uint32_t>(scene.content_revision >> 32U));
    for (const auto& triangle : scene.triangles) {
        for (const auto& position : triangle.positions) {
            for (const auto coordinate : position) hash = hash_float(hash, coordinate);
        }
    }
    return hash;
}

bool ray_intersects_triangle(const Vec3 origin,
                             const Vec3 direction,
                             const NativeVulkanRayTracingTriangle& triangle,
                             const float minimum_distance,
                             const float maximum_distance) noexcept {
    const auto v0 = to_vec3(triangle.positions[0U]);
    const auto v1 = to_vec3(triangle.positions[1U]);
    const auto v2 = to_vec3(triangle.positions[2U]);
    const auto edge_1 = v1 - v0;
    const auto edge_2 = v2 - v0;
    const auto p = cross(direction, edge_2);
    const auto determinant = dot(edge_1, p);
    if (!std::isfinite(determinant) || std::abs(determinant) <= kEpsilon) return false;
    const auto inverse_determinant = 1.0F / determinant;
    const auto from_v0 = origin - v0;
    const auto u = inverse_determinant * dot(from_v0, p);
    if (!std::isfinite(u) || u < 0.0F || u > 1.0F) return false;
    const auto q = cross(from_v0, edge_1);
    const auto v = inverse_determinant * dot(direction, q);
    if (!std::isfinite(v) || v < 0.0F || u + v > 1.0F) return false;
    const auto distance = inverse_determinant * dot(edge_2, q);
    return std::isfinite(distance) && distance >= minimum_distance && distance <= maximum_distance;
}

} // namespace

struct NativeVulkanRayTracingContext::Impl final {
    NativeVulkanRayTracingContextOptions options;
    NativeVulkanRayTracingContextState state{NativeVulkanRayTracingContextState::uninitialized};
    bool initialized{};
    bool scene_ready{};
    bool trace_completed{};
    bool readback_completed{};
    bool scene_rebuilt{};
    bool scene_updated{};
    bool scene_reused{};
    std::uint64_t generation{};
    std::uint64_t scene_topology_revision{};
    std::uint64_t scene_content_revision{};
    std::uint64_t scene_fingerprint{};
    std::uint32_t output_value{};
    std::uint64_t output_hash{};
    std::vector<NativeVulkanRayTracingTriangle> triangles;

    explicit Impl(const NativeVulkanRayTracingContextOptions& source) : options(source) {
        options.maximum_triangles = std::min(options.maximum_triangles,
                                             native_vulkan_raytracing_context_hard_max_triangles);
        options.output_width = std::clamp(options.output_width, 1U, 4096U);
        options.output_height = std::clamp(options.output_height, 1U, 4096U);
        options.output_depth = std::clamp(options.output_depth, 1U, 64U);
        triangles.reserve(std::min(options.maximum_triangles, std::size_t{256U}));
    }

    NativeVulkanRayTracingContextReceipt receipt(
        const NativeVulkanRayTracingContextState result_state,
        const NativeVulkanRayTracingContextFailureStage stage,
        const std::string_view code,
        const std::string_view detail) const {
        NativeVulkanRayTracingContextReceipt result;
        result.state = result_state;
        result.failure_stage = stage;
        result.code = bounded_text(code);
        result.detail = bounded_text(detail);
        result.initialized = initialized;
        result.persistent_backend = false;
        result.fallback_active = result_state == NativeVulkanRayTracingContextState::fallback ||
                                 state == NativeVulkanRayTracingContextState::fallback;
        result.scene_ready = scene_ready;
        result.scene_rebuilt = scene_rebuilt;
        result.scene_updated = scene_updated;
        result.scene_reused = scene_reused;
        result.trace_completed = trace_completed;
        result.readback_completed = readback_completed;
        result.resources_live = false;
        result.shutdown = result_state == NativeVulkanRayTracingContextState::shutdown ||
                          state == NativeVulkanRayTracingContextState::shutdown;
        result.generation = generation;
        result.scene_topology_revision = scene_topology_revision;
        result.scene_content_revision = scene_content_revision;
        result.scene_fingerprint = scene_fingerprint;
        result.triangle_count = static_cast<std::uint32_t>(triangles.size());
        result.output_width = options.output_width;
        result.output_height = options.output_height;
        result.output_depth = options.output_depth;
        result.output_value = output_value;
        result.output_hit = output_value == kHitMarker ? 1U : 0U;
        result.output_hash = output_hash;
        result.output_bytes = trace_completed ? sizeof(std::uint32_t) : 0U;
        result.readback_bytes = readback_completed ? sizeof(std::uint32_t) : 0U;
        return result;
    }

    NativeVulkanRayTracingContextReceipt not_ready(
        const NativeVulkanRayTracingContextFailureStage stage,
        const std::string_view code,
        const std::string_view detail) const {
        return receipt(NativeVulkanRayTracingContextState::error, stage, code, detail);
    }

};

std::string_view native_vulkan_raytracing_context_state_name(
    const NativeVulkanRayTracingContextState state) noexcept {
    switch (state) {
    case NativeVulkanRayTracingContextState::uninitialized:
        return "uninitialized";
    case NativeVulkanRayTracingContextState::ready:
        return "ready";
    case NativeVulkanRayTracingContextState::unsupported:
        return "unsupported";
    case NativeVulkanRayTracingContextState::fallback:
        return "fallback";
    case NativeVulkanRayTracingContextState::error:
        return "error";
    case NativeVulkanRayTracingContextState::shutdown:
        return "shutdown";
    }
    return "error";
}

std::string_view native_vulkan_raytracing_context_failure_stage_name(
    const NativeVulkanRayTracingContextFailureStage stage) noexcept {
    switch (stage) {
    case NativeVulkanRayTracingContextFailureStage::none:
        return "none";
    case NativeVulkanRayTracingContextFailureStage::loader:
        return "loader";
    case NativeVulkanRayTracingContextFailureStage::instance:
        return "instance";
    case NativeVulkanRayTracingContextFailureStage::physical_device:
        return "physical-device";
    case NativeVulkanRayTracingContextFailureStage::device:
        return "device";
    case NativeVulkanRayTracingContextFailureStage::acceleration_structure:
        return "acceleration-structure";
    case NativeVulkanRayTracingContextFailureStage::pipeline:
        return "pipeline";
    case NativeVulkanRayTracingContextFailureStage::scene:
        return "scene";
    case NativeVulkanRayTracingContextFailureStage::trace:
        return "trace";
    case NativeVulkanRayTracingContextFailureStage::readback:
        return "readback";
    case NativeVulkanRayTracingContextFailureStage::shutdown:
        return "shutdown";
    }
    return "shutdown";
}

NativeVulkanRayTracingContext::NativeVulkanRayTracingContext(
    const NativeVulkanRayTracingContextOptions& options)
    : impl_(std::make_unique<Impl>(options)) {}

NativeVulkanRayTracingContext::~NativeVulkanRayTracingContext() { static_cast<void>(shutdown()); }

NativeVulkanRayTracingContextReceipt NativeVulkanRayTracingContext::initialize() {
    if (impl_->state == NativeVulkanRayTracingContextState::shutdown) {
        return impl_->not_ready(NativeVulkanRayTracingContextFailureStage::shutdown,
                                "native-vulkan-rt.context-shutdown",
                                "The context has already been shut down and cannot be initialized again.");
    }
    if (impl_->initialized) {
        return impl_->receipt(impl_->state, NativeVulkanRayTracingContextFailureStage::none,
                              impl_->state == NativeVulkanRayTracingContextState::fallback
                                  ? "native-vulkan-rt.context-fallback-already-initialized"
                                  : "native-vulkan-rt.context-already-initialized",
                              impl_->state == NativeVulkanRayTracingContextState::fallback
                                  ? "The persistent Vulkan backend is unavailable; the existing fallback context is reused."
                                  : "The Vulkan context is already initialized.");
    }

    impl_->initialized = true;
    impl_->generation = 1U;
    if (!impl_->options.allow_fallback) {
        impl_->state = NativeVulkanRayTracingContextState::unsupported;
        return impl_->receipt(NativeVulkanRayTracingContextState::unsupported,
                              NativeVulkanRayTracingContextFailureStage::device,
                              "native-vulkan-rt.context-persistent-backend-unsupported",
                              "Persistent Vulkan instance/device/AS/SBT ownership is not available in this context slice; fallback is disabled.");
    }

    // Do not invoke execute_native_vulkan_raytracing_blas_tlas() here.  That
    // function owns a short-lived probe and releases every native object
    // before returning, which cannot satisfy a cross-frame context contract.
    impl_->state = NativeVulkanRayTracingContextState::fallback;
    return impl_->receipt(NativeVulkanRayTracingContextState::fallback,
                          NativeVulkanRayTracingContextFailureStage::device,
                          "native-vulkan-rt.context-persistent-backend-unavailable",
                          "Persistent Vulkan resource ownership is not extracted from the short-lived executor yet; the deterministic CPU fallback is active.");
}

NativeVulkanRayTracingContextReceipt NativeVulkanRayTracingContext::ensure_scene(
    const NativeVulkanRayTracingScene& scene) {
    if (!impl_->initialized) static_cast<void>(initialize());
    if (impl_->state == NativeVulkanRayTracingContextState::shutdown) {
        return impl_->not_ready(NativeVulkanRayTracingContextFailureStage::shutdown,
                                "native-vulkan-rt.context-shutdown",
                                "A scene cannot be submitted after context shutdown.");
    }
    if (impl_->state == NativeVulkanRayTracingContextState::unsupported) {
        return impl_->receipt(NativeVulkanRayTracingContextState::unsupported,
                              NativeVulkanRayTracingContextFailureStage::device,
                              "native-vulkan-rt.context-unsupported",
                              "The persistent Vulkan backend is unsupported and fallback is disabled.");
    }
    if (scene.triangles.empty()) {
        return impl_->not_ready(NativeVulkanRayTracingContextFailureStage::scene,
                                "native-vulkan-rt.context-scene-empty",
                                "At least one triangle is required for the bounded scene contract.");
    }
    if (scene.triangles.size() > impl_->options.maximum_triangles) {
        return impl_->not_ready(NativeVulkanRayTracingContextFailureStage::scene,
                                "native-vulkan-rt.context-scene-limit",
                                "The scene exceeds the configured triangle budget.");
    }
    for (const auto& triangle : scene.triangles) {
        if (!valid_scene_triangle(triangle)) {
            return impl_->not_ready(NativeVulkanRayTracingContextFailureStage::scene,
                                    "native-vulkan-rt.context-scene-nonfinite",
                                    "Scene vertices must be finite and within the bounded coordinate range.");
        }
    }
    const auto fingerprint = scene_fingerprint(scene);
    const auto topology_changed = !impl_->scene_ready ||
                                   impl_->scene_topology_revision != scene.topology_revision ||
                                   impl_->triangles.size() != scene.triangles.size();
    const auto content_changed = !impl_->scene_ready || impl_->scene_fingerprint != fingerprint;
    impl_->scene_rebuilt = topology_changed;
    impl_->scene_updated = !topology_changed && content_changed;
    impl_->scene_reused = !topology_changed && !content_changed;
    if (content_changed) {
        if (impl_->generation == std::numeric_limits<std::uint64_t>::max()) {
            return impl_->not_ready(NativeVulkanRayTracingContextFailureStage::scene,
                                    "native-vulkan-rt.context-generation-overflow",
                                    "The context generation cannot be incremented safely.");
        }
        ++impl_->generation;
        impl_->triangles.assign(scene.triangles.begin(), scene.triangles.end());
        impl_->scene_topology_revision = scene.topology_revision;
        impl_->scene_content_revision = scene.content_revision;
        impl_->scene_fingerprint = fingerprint;
        impl_->scene_ready = true;
        impl_->trace_completed = false;
        impl_->readback_completed = false;
        impl_->output_value = 0U;
        impl_->output_hash = 0U;
    }
    return impl_->receipt(impl_->state, NativeVulkanRayTracingContextFailureStage::scene,
                          topology_changed ? "native-vulkan-rt.context-scene-rebuilt"
                                            : (content_changed ? "native-vulkan-rt.context-scene-updated"
                                                                : "native-vulkan-rt.context-scene-reused"),
                          topology_changed ? "The fallback scene snapshot changed topology and was rebuilt."
                                            : (content_changed ? "The fallback scene snapshot changed content and was updated."
                                                               : "The scene fingerprint is unchanged; the existing snapshot is reused."));
}

NativeVulkanRayTracingContextReceipt NativeVulkanRayTracingContext::build_or_update() {
    if (!impl_->initialized) static_cast<void>(initialize());
    if (impl_->state == NativeVulkanRayTracingContextState::shutdown) {
        return impl_->not_ready(NativeVulkanRayTracingContextFailureStage::shutdown,
                                "native-vulkan-rt.context-shutdown",
                                "A scene build cannot be submitted after context shutdown.");
    }
    if (impl_->state == NativeVulkanRayTracingContextState::unsupported) {
        return impl_->receipt(NativeVulkanRayTracingContextState::unsupported,
                              NativeVulkanRayTracingContextFailureStage::device,
                              "native-vulkan-rt.context-unsupported",
                              "The persistent Vulkan backend is unsupported and fallback is disabled.");
    }
    if (!impl_->scene_ready) {
        return impl_->not_ready(NativeVulkanRayTracingContextFailureStage::scene,
                                "native-vulkan-rt.context-scene-missing",
                                "ensure_scene must provide a scene before build_or_update.");
    }
    return impl_->receipt(impl_->state, NativeVulkanRayTracingContextFailureStage::acceleration_structure,
                          "native-vulkan-rt.context-fallback-build-cached",
                          "The scene is resident in the fallback context; no Vulkan AS build is claimed.");
}

NativeVulkanRayTracingContextReceipt NativeVulkanRayTracingContext::trace(
    const NativeVulkanRayTracingTraceRequest& request) {
    if (!impl_->initialized) static_cast<void>(initialize());
    if (impl_->state == NativeVulkanRayTracingContextState::shutdown) {
        return impl_->not_ready(NativeVulkanRayTracingContextFailureStage::shutdown,
                                "native-vulkan-rt.context-shutdown",
                                "A trace cannot be submitted after context shutdown.");
    }
    if (impl_->state == NativeVulkanRayTracingContextState::unsupported) {
        return impl_->receipt(NativeVulkanRayTracingContextState::unsupported,
                              NativeVulkanRayTracingContextFailureStage::device,
                              "native-vulkan-rt.context-unsupported",
                              "The persistent Vulkan backend is unsupported and fallback is disabled.");
    }
    if (!impl_->scene_ready) {
        return impl_->not_ready(NativeVulkanRayTracingContextFailureStage::scene,
                                "native-vulkan-rt.context-scene-missing",
                                "ensure_scene must provide a scene before trace.");
    }
    if (!valid_trace_request(request)) {
        return impl_->not_ready(NativeVulkanRayTracingContextFailureStage::trace,
                                "native-vulkan-rt.context-trace-invalid-request",
                                "Trace origin, direction and distance range must be finite and bounded.");
    }

    const auto origin = to_vec3(request.origin);
    const auto direction = to_vec3(request.direction);
    bool hit = false;
    for (const auto& triangle : impl_->triangles) {
        if (ray_intersects_triangle(origin, direction, triangle, request.minimum_distance,
                                    request.maximum_distance)) {
            hit = true;
            break;
        }
    }
    impl_->output_value = hit ? kHitMarker : kMissMarker;
    impl_->output_hash = hash_u32(kFnvOffsetBasis, impl_->output_value);
    impl_->trace_completed = true;
    impl_->readback_completed = false;
    return impl_->receipt(impl_->state, NativeVulkanRayTracingContextFailureStage::trace,
                          hit ? "native-vulkan-rt.context-fallback-trace-hit"
                              : "native-vulkan-rt.context-fallback-trace-miss",
                          hit ? "The deterministic fallback ray intersected the resident scene."
                              : "The deterministic fallback ray completed without an intersection.");
}

NativeVulkanRayTracingContextReceipt NativeVulkanRayTracingContext::readback() {
    if (!impl_->initialized) static_cast<void>(initialize());
    if (impl_->state == NativeVulkanRayTracingContextState::shutdown) {
        return impl_->not_ready(NativeVulkanRayTracingContextFailureStage::shutdown,
                                "native-vulkan-rt.context-shutdown",
                                "A readback cannot be requested after context shutdown.");
    }
    if (impl_->state == NativeVulkanRayTracingContextState::unsupported) {
        return impl_->receipt(NativeVulkanRayTracingContextState::unsupported,
                              NativeVulkanRayTracingContextFailureStage::device,
                              "native-vulkan-rt.context-unsupported",
                              "The persistent Vulkan backend is unsupported and fallback is disabled.");
    }
    if (!impl_->trace_completed) {
        return impl_->not_ready(NativeVulkanRayTracingContextFailureStage::readback,
                                "native-vulkan-rt.context-trace-missing",
                                "trace must complete before readback.");
    }
    impl_->readback_completed = true;
    return impl_->receipt(impl_->state, NativeVulkanRayTracingContextFailureStage::readback,
                          "native-vulkan-rt.context-fallback-readback-completed",
                          "The fallback output marker was read back from the context-owned CPU snapshot; no Vulkan readback is claimed.");
}

NativeVulkanRayTracingContextReceipt NativeVulkanRayTracingContext::shutdown() noexcept {
    if (impl_ == nullptr) return {};
    if (impl_->state == NativeVulkanRayTracingContextState::shutdown) {
        return impl_->receipt(NativeVulkanRayTracingContextState::shutdown,
                              NativeVulkanRayTracingContextFailureStage::none,
                              "native-vulkan-rt.context-shutdown-already-complete",
                              "The context shutdown operation is idempotent.");
    }
    impl_->triangles.clear();
    impl_->scene_ready = false;
    impl_->trace_completed = false;
    impl_->readback_completed = false;
    impl_->scene_rebuilt = false;
    impl_->scene_updated = false;
    impl_->scene_reused = false;
    impl_->scene_topology_revision = 0U;
    impl_->scene_content_revision = 0U;
    impl_->scene_fingerprint = 0U;
    impl_->output_value = 0U;
    impl_->output_hash = 0U;
    impl_->state = NativeVulkanRayTracingContextState::shutdown;
    return impl_->receipt(NativeVulkanRayTracingContextState::shutdown,
                          NativeVulkanRayTracingContextFailureStage::shutdown,
                          "native-vulkan-rt.context-shutdown-complete",
                          "The context is shut down; repeated shutdown calls are safe.");
}

bool NativeVulkanRayTracingContext::initialized() const noexcept { return impl_ != nullptr && impl_->initialized; }

bool NativeVulkanRayTracingContext::scene_ready() const noexcept {
    return impl_ != nullptr && impl_->scene_ready;
}

std::uint64_t NativeVulkanRayTracingContext::generation() const noexcept {
    return impl_ == nullptr ? 0U : impl_->generation;
}

} // namespace noemancer
