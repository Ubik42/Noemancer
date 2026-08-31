#include "runtime/raytracing_output_interop.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>

namespace noemancer {

namespace {

std::string bounded_text(const std::string_view value) {
    return std::string(value.substr(
        0U, std::min(value.size(), raytracing_output_interop_max_text_bytes)));
}

bool valid_backend(const RayTracingOutputInteropBackend backend) noexcept {
    return backend == RayTracingOutputInteropBackend::d3d12 ||
        backend == RayTracingOutputInteropBackend::vulkan;
}

bool valid_layout(const RayTracingOutputInteropLayout layout) noexcept {
    return layout == RayTracingOutputInteropLayout::ray_tracing_output ||
        layout == RayTracingOutputInteropLayout::copy_source ||
        layout == RayTracingOutputInteropLayout::shader_read;
}

bool valid_ownership(const RayTracingOutputInteropOwnership ownership) noexcept {
    return ownership == RayTracingOutputInteropOwnership::ray_tracing ||
        ownership == RayTracingOutputInteropOwnership::renderer;
}

} // namespace

std::string_view raytracing_output_interop_backend_name(
    const RayTracingOutputInteropBackend backend) noexcept {
    switch (backend) {
    case RayTracingOutputInteropBackend::unknown:
        return "unknown";
    case RayTracingOutputInteropBackend::d3d12:
        return "d3d12";
    case RayTracingOutputInteropBackend::vulkan:
        return "vulkan";
    }
    return "unknown";
}

std::string_view raytracing_output_interop_state_name(
    const RayTracingOutputInteropState state) noexcept {
    switch (state) {
    case RayTracingOutputInteropState::uninitialized:
        return "uninitialized";
    case RayTracingOutputInteropState::configured:
        return "configured";
    case RayTracingOutputInteropState::produced:
        return "produced";
    case RayTracingOutputInteropState::consumed:
        return "consumed";
    case RayTracingOutputInteropState::shutdown:
        return "shutdown";
    }
    return "uninitialized";
}

std::string_view raytracing_output_interop_mode_name(
    const RayTracingOutputInteropMode mode) noexcept {
    switch (mode) {
    case RayTracingOutputInteropMode::unavailable:
        return "unavailable";
    case RayTracingOutputInteropMode::direct_share:
        return "direct-share";
    case RayTracingOutputInteropMode::gpu_copy:
        return "gpu-copy";
    }
    return "unavailable";
}

std::string_view raytracing_output_interop_layout_name(
    const RayTracingOutputInteropLayout layout) noexcept {
    switch (layout) {
    case RayTracingOutputInteropLayout::undefined:
        return "undefined";
    case RayTracingOutputInteropLayout::ray_tracing_output:
        return "ray-tracing-output";
    case RayTracingOutputInteropLayout::copy_source:
        return "copy-source";
    case RayTracingOutputInteropLayout::shader_read:
        return "shader-read";
    }
    return "undefined";
}

std::string_view raytracing_output_interop_ownership_name(
    const RayTracingOutputInteropOwnership ownership) noexcept {
    switch (ownership) {
    case RayTracingOutputInteropOwnership::none:
        return "none";
    case RayTracingOutputInteropOwnership::ray_tracing:
        return "ray-tracing";
    case RayTracingOutputInteropOwnership::renderer:
        return "renderer";
    }
    return "none";
}

std::string_view raytracing_output_interop_device_compatibility_name(
    const RayTracingOutputInteropDeviceCompatibility compatibility) noexcept {
    switch (compatibility) {
    case RayTracingOutputInteropDeviceCompatibility::unknown:
        return "unknown";
    case RayTracingOutputInteropDeviceCompatibility::same_device:
        return "same-device";
    case RayTracingOutputInteropDeviceCompatibility::cross_device:
        return "cross-device";
    case RayTracingOutputInteropDeviceCompatibility::incompatible:
        return "incompatible";
    }
    return "unknown";
}

RayTracingOutputInteropReceipt RayTracingOutputInterop::make_receipt(
    const std::string_view operation) const {
    RayTracingOutputInteropReceipt result;
    result.operation = bounded_text(operation);
    result.state = state_;
    result.mode = last_mode_;
    result.device_compatibility = last_device_compatibility;
    result.producer_layout = producer_layout_;
    result.consumer_layout = consumer_layout_;
    result.producer_ownership = producer_ownership_;
    result.consumer_ownership = consumer_ownership_;
    result.producer_sync_generation = producer_sync_generation_;
    result.consumer_sync_generation = consumer_sync_generation_;
    result.native_handles_exposed = false;
    if (resource_) {
        result.resource_id = resource_->resource_id;
        result.backend = std::string(raytracing_output_interop_backend_name(resource_->backend));
        result.format = resource_->format;
        result.width = resource_->width;
        result.height = resource_->height;
        result.resource_generation = resource_->resource_generation;
    }
    result.producer_ready = state_ == RayTracingOutputInteropState::produced ||
        state_ == RayTracingOutputInteropState::consumed;
    result.consumer_ready = state_ == RayTracingOutputInteropState::consumed;
    result.visual_path_eligible = result.consumer_ready &&
        result.mode != RayTracingOutputInteropMode::unavailable;
    result.visual_path = result.visual_path_eligible
        ? std::string(result.mode == RayTracingOutputInteropMode::direct_share
                ? "rt-direct-share"
                : "rt-gpu-copy")
        : "raster-pbr-fallback";
    return result;
}

RayTracingOutputInteropReceipt RayTracingOutputInterop::reject(
    const std::string_view operation,
    const std::string_view code,
    const std::string_view detail,
    const bool stale) const {
    auto result = make_receipt(operation);
    result.accepted = false;
    result.completed = false;
    result.mode = RayTracingOutputInteropMode::unavailable;
    result.code = bounded_text(code);
    result.detail = bounded_text(detail);
    result.generation_stale = stale;
    result.visual_path_eligible = false;
    result.visual_path = "raster-pbr-fallback";
    return result;
}

bool RayTracingOutputInterop::valid_resource(
    const RayTracingOutputInteropResource& resource) const noexcept {
    return !resource.resource_id.empty() &&
        resource.resource_id.size() <= raytracing_output_interop_max_text_bytes &&
        valid_backend(resource.backend) && !resource.device_id.empty() &&
        resource.device_id.size() <= raytracing_output_interop_max_text_bytes &&
        resource.width > 0U && resource.width <= raytracing_output_interop_max_extent &&
        resource.height > 0U && resource.height <= raytracing_output_interop_max_extent &&
        !resource.format.empty() &&
        resource.format.size() <= raytracing_output_interop_max_text_bytes &&
        resource.resource_generation != 0U;
}

bool RayTracingOutputInterop::same_resource(
    const RayTracingOutputInteropResource& left,
    const RayTracingOutputInteropResource& right) const noexcept {
    return left.resource_id == right.resource_id && left.backend == right.backend &&
        left.device_id == right.device_id && left.width == right.width &&
        left.height == right.height && left.format == right.format &&
        left.resource_generation == right.resource_generation;
}

RayTracingOutputInteropDeviceCompatibility RayTracingOutputInterop::device_compatibility(
    const RayTracingOutputInteropResource& producer,
    const RayTracingOutputInteropResource& consumer) const noexcept {
    if (producer.backend != consumer.backend) {
        return RayTracingOutputInteropDeviceCompatibility::incompatible;
    }
    if (producer.device_id.empty() || consumer.device_id.empty()) {
        return RayTracingOutputInteropDeviceCompatibility::unknown;
    }
    return producer.device_id == consumer.device_id
        ? RayTracingOutputInteropDeviceCompatibility::same_device
        : RayTracingOutputInteropDeviceCompatibility::cross_device;
}

RayTracingOutputInteropReceipt RayTracingOutputInterop::configure(
    const RayTracingOutputInteropResource& resource) {
    if (state_ == RayTracingOutputInteropState::shutdown) {
        return reject("configure", "interop-shutdown", "The interop state machine is terminally shut down.");
    }
    if (!valid_resource(resource)) {
        return reject("configure", "resource-invalid", "Output identity, extent, format and generation must be bounded and valid.");
    }
    if (state_ != RayTracingOutputInteropState::uninitialized) {
        if (resource_ && same_resource(resource, *resource_)) {
            auto result = make_receipt("configure");
            result.accepted = true;
            result.completed = true;
            result.code = "configure-reused";
            result.detail = "The configured output resource identity is unchanged.";
            return result;
        }
        return reject("configure", "configure-requires-resize",
                      "A live output must use resize() to move to a new resource generation.");
    }

    resource_ = resource;
    state_ = RayTracingOutputInteropState::configured;
    last_mode_ = RayTracingOutputInteropMode::unavailable;
    last_device_compatibility = RayTracingOutputInteropDeviceCompatibility::unknown;
    producer_layout_ = RayTracingOutputInteropLayout::undefined;
    consumer_layout_ = RayTracingOutputInteropLayout::undefined;
    producer_ownership_ = RayTracingOutputInteropOwnership::none;
    consumer_ownership_ = RayTracingOutputInteropOwnership::none;
    producer_sync_generation_ = 0U;
    consumer_sync_generation_ = 0U;
    auto result = make_receipt("configure");
    result.accepted = true;
    result.completed = true;
    result.code = "configured";
    result.detail = "The private RT output identity is ready for a producer frame.";
    return result;
}

RayTracingOutputInteropReceipt RayTracingOutputInterop::publish(
    const RayTracingOutputInteropProducerFrame& frame) {
    if (state_ == RayTracingOutputInteropState::shutdown) {
        return reject("publish", "interop-shutdown", "The interop state machine is terminally shut down.");
    }
    if (!resource_) {
        return reject("publish", "not-configured", "Configure an output resource before publishing a frame.");
    }
    if (!valid_resource(frame.resource)) {
        return reject("publish", "resource-invalid", "Producer output identity is invalid.");
    }
    if (frame.resource.resource_generation < resource_->resource_generation) {
        return reject("publish", "producer-generation-stale",
                      "The producer submitted an output generation older than the configured view.", true);
    }
    if (frame.resource.resource_generation > resource_->resource_generation) {
        return reject("publish", "producer-generation-unconfigured",
                      "The producer submitted a generation that has not passed through resize().");
    }
    if (!same_resource(frame.resource, *resource_)) {
        if (frame.resource.backend != resource_->backend || frame.resource.device_id != resource_->device_id) {
            return reject("publish", "producer-device-incompatible",
                          "Producer output backend or device does not match the configured private view.");
        }
        if (frame.resource.width != resource_->width || frame.resource.height != resource_->height) {
            return reject("publish", "producer-extent-mismatch",
                          "Producer output extent does not match the configured view.");
        }
        return reject("publish", "producer-resource-incompatible",
                      "Producer output identity or format does not match the configured view.");
    }
    if (!valid_layout(frame.layout) ||
        frame.layout != RayTracingOutputInteropLayout::ray_tracing_output) {
        return reject("publish", "producer-layout-invalid",
                      "A producer frame must arrive in ray-tracing-output layout.");
    }
    if (!valid_ownership(frame.ownership) ||
        frame.ownership != RayTracingOutputInteropOwnership::ray_tracing) {
        return reject("publish", "producer-ownership-invalid",
                      "A producer frame must be owned by the ray-tracing queue.");
    }
    if (!frame.producer_complete) {
        return reject("publish", "producer-incomplete",
                      "The producer must signal completion before interop publication.");
    }
    if (frame.producer_sync_generation == 0U ||
        frame.producer_sync_generation <= producer_sync_generation_) {
        return reject("publish", "producer-sync-stale",
                      "Producer synchronization generation must advance monotonically.", true);
    }

    state_ = RayTracingOutputInteropState::produced;
    last_mode_ = RayTracingOutputInteropMode::unavailable;
    last_device_compatibility = RayTracingOutputInteropDeviceCompatibility::unknown;
    producer_layout_ = frame.layout;
    consumer_layout_ = RayTracingOutputInteropLayout::undefined;
    producer_ownership_ = frame.ownership;
    consumer_ownership_ = RayTracingOutputInteropOwnership::none;
    producer_sync_generation_ = frame.producer_sync_generation;
    consumer_sync_generation_ = 0U;
    auto result = make_receipt("publish");
    result.accepted = true;
    result.completed = true;
    result.code = "producer-frame-published";
    result.detail = "A completed RT output frame is available for direct sharing or GPU copy.";
    return result;
}

RayTracingOutputInteropReceipt RayTracingOutputInterop::acquire(
    const RayTracingOutputInteropConsumerRequest& request) {
    if (state_ == RayTracingOutputInteropState::shutdown) {
        return reject("acquire", "interop-shutdown", "The interop state machine is terminally shut down.");
    }
    if (!resource_) {
        return reject("acquire", "not-configured", "Configure an output resource before acquisition.");
    }
    if (state_ != RayTracingOutputInteropState::produced) {
        return reject("acquire", state_ == RayTracingOutputInteropState::consumed
                          ? "frame-already-consumed" : "producer-frame-missing",
                      state_ == RayTracingOutputInteropState::consumed
                          ? "The current producer frame has already been consumed."
                          : "A completed producer frame is required before acquisition.");
    }
    if (!valid_resource(request.resource)) {
        return reject("acquire", "consumer-resource-invalid", "Consumer output identity is invalid.");
    }
    if (request.resource.resource_generation < resource_->resource_generation) {
        return reject("acquire", "consumer-generation-stale",
                      "The consumer requested an older output resource generation.", true);
    }
    if (request.resource.resource_generation > resource_->resource_generation) {
        return reject("acquire", "consumer-generation-unconfigured",
                      "The consumer requested a resource generation not configured by resize().");
    }
    if (request.resource.resource_id != resource_->resource_id) {
        return reject("acquire", "consumer-resource-incompatible",
                      "Consumer resource id does not match the configured output view.");
    }
    if (request.resource.width != resource_->width || request.resource.height != resource_->height) {
        return reject("acquire", "consumer-extent-mismatch",
                      "Consumer extent does not match the producer output.");
    }
    if (request.resource.format != resource_->format) {
        return reject("acquire", "consumer-format-mismatch",
                      "Consumer format does not match the producer output.");
    }
    if (request.expected_producer_sync_generation == 0U) {
        return reject("acquire", "consumer-sync-missing",
                      "Consumer must identify the producer synchronization generation.");
    }
    if (request.expected_producer_sync_generation < producer_sync_generation_) {
        return reject("acquire", "consumer-sync-stale",
                      "Consumer synchronization generation is older than the published frame.", true);
    }
    if (request.expected_producer_sync_generation > producer_sync_generation_) {
        return reject("acquire", "consumer-sync-not-ready",
                      "The requested producer synchronization generation is not published yet.");
    }
    if (!valid_layout(request.desired_layout) ||
        request.desired_layout != RayTracingOutputInteropLayout::shader_read) {
        return reject("acquire", "consumer-layout-invalid",
                      "The renderer consumer must acquire the output in shader-read layout.");
    }
    if (!valid_ownership(request.desired_ownership) ||
        request.desired_ownership != RayTracingOutputInteropOwnership::renderer) {
        return reject("acquire", "consumer-ownership-invalid",
                      "The renderer consumer must acquire renderer ownership.");
    }

    const auto compatibility = device_compatibility(*resource_, request.resource);
    last_device_compatibility = compatibility;
    if (compatibility == RayTracingOutputInteropDeviceCompatibility::incompatible) {
        return reject("acquire", "backend-incompatible",
                      "D3D12 and Vulkan output views cannot be directly or copied across this boundary.");
    }
    if (compatibility == RayTracingOutputInteropDeviceCompatibility::unknown) {
        return reject("acquire", "device-unknown",
                      "Direct sharing and GPU copy require stable producer and consumer device identities.");
    }

    RayTracingOutputInteropMode selected = RayTracingOutputInteropMode::unavailable;
    if (request.allow_direct_share && request.direct_share_supported &&
        compatibility == RayTracingOutputInteropDeviceCompatibility::same_device) {
        selected = RayTracingOutputInteropMode::direct_share;
    } else if (request.allow_gpu_copy && request.gpu_copy_supported) {
        selected = RayTracingOutputInteropMode::gpu_copy;
    }
    if (selected == RayTracingOutputInteropMode::unavailable) {
        return reject("acquire", compatibility == RayTracingOutputInteropDeviceCompatibility::cross_device
                          ? "device-incompatible" : "transfer-path-unavailable",
                      compatibility == RayTracingOutputInteropDeviceCompatibility::cross_device
                          ? "The devices differ and no supported GPU-copy path was provided."
                          : "The consumer provided neither a usable direct-share nor GPU-copy path.");
    }

    state_ = RayTracingOutputInteropState::consumed;
    last_mode_ = selected;
    consumer_layout_ = request.desired_layout;
    consumer_ownership_ = request.desired_ownership;
    consumer_sync_generation_ = producer_sync_generation_;
    auto result = make_receipt("acquire");
    result.mode = selected;
    result.accepted = true;
    result.completed = true;
    result.ownership_transferred = true;
    result.code = selected == RayTracingOutputInteropMode::direct_share
        ? "direct-share-acquired" : "gpu-copy-acquired";
    result.detail = selected == RayTracingOutputInteropMode::direct_share
        ? "The renderer acquired the producer output on the same compatible device."
        : "The renderer acquired the producer output through the explicitly enabled GPU-copy path.";
    return result;
}

RayTracingOutputInteropReceipt RayTracingOutputInterop::resize(
    const RayTracingOutputInteropResource& resource) {
    if (state_ == RayTracingOutputInteropState::shutdown) {
        return reject("resize", "interop-shutdown", "The interop state machine is terminally shut down.");
    }
    if (!resource_) {
        return reject("resize", "not-configured", "Configure an output resource before resize().");
    }
    if (!valid_resource(resource)) {
        return reject("resize", "resource-invalid", "Resized output identity is invalid.");
    }
    if (resource.resource_id != resource_->resource_id || resource.backend != resource_->backend ||
        resource.device_id != resource_->device_id || resource.format != resource_->format) {
        return reject("resize", "resize-identity-mismatch",
                      "Resize preserves resource id, backend, device and format.");
    }
    if (resource.resource_generation <= resource_->resource_generation) {
        return reject("resize", "resize-generation-not-increasing",
                      "Resize must advance resource_generation monotonically.", true);
    }

    resource_ = resource;
    state_ = RayTracingOutputInteropState::configured;
    last_mode_ = RayTracingOutputInteropMode::unavailable;
    last_device_compatibility = RayTracingOutputInteropDeviceCompatibility::unknown;
    producer_layout_ = RayTracingOutputInteropLayout::undefined;
    consumer_layout_ = RayTracingOutputInteropLayout::undefined;
    producer_ownership_ = RayTracingOutputInteropOwnership::none;
    consumer_ownership_ = RayTracingOutputInteropOwnership::none;
    producer_sync_generation_ = 0U;
    consumer_sync_generation_ = 0U;
    auto result = make_receipt("resize");
    result.accepted = true;
    result.completed = true;
    result.resized = true;
    result.code = "resized";
    result.detail = "The old output generation was retired; a new producer frame is required.";
    return result;
}

RayTracingOutputInteropReceipt RayTracingOutputInterop::shutdown() noexcept {
    if (state_ == RayTracingOutputInteropState::shutdown) {
        auto result = make_receipt("shutdown");
        result.accepted = true;
        result.completed = true;
        result.code = "shutdown-idempotent";
        result.detail = "The interop state machine was already shut down.";
        return result;
    }
    state_ = RayTracingOutputInteropState::shutdown;
    last_mode_ = RayTracingOutputInteropMode::unavailable;
    producer_layout_ = RayTracingOutputInteropLayout::undefined;
    consumer_layout_ = RayTracingOutputInteropLayout::undefined;
    producer_ownership_ = RayTracingOutputInteropOwnership::none;
    consumer_ownership_ = RayTracingOutputInteropOwnership::none;
    producer_sync_generation_ = 0U;
    consumer_sync_generation_ = 0U;
    auto result = make_receipt("shutdown");
    result.accepted = true;
    result.completed = true;
    result.code = "shutdown-complete";
    result.detail = "All future producer and consumer transitions fail closed.";
    return result;
}

RayTracingOutputInteropReceipt RayTracingOutputInterop::status() const {
    auto result = make_receipt("status");
    switch (state_) {
    case RayTracingOutputInteropState::uninitialized:
        result.code = "uninitialized";
        result.detail = "No RT output resource has been configured.";
        break;
    case RayTracingOutputInteropState::configured:
        result.code = "configured";
        result.detail = "Waiting for a completed producer frame.";
        break;
    case RayTracingOutputInteropState::produced:
        result.code = "producer-ready";
        result.detail = "A completed producer frame awaits consumer acquisition.";
        break;
    case RayTracingOutputInteropState::consumed:
        result.code = "consumer-ready";
        result.detail = "The consumer owns the acquired visual output path.";
        break;
    case RayTracingOutputInteropState::shutdown:
        result.code = "shutdown";
        result.detail = "The interop state machine is terminally shut down.";
        break;
    }
    return result;
}

} // namespace noemancer
