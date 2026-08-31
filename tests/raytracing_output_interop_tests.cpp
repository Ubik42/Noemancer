#include "runtime/raytracing_output_interop.hpp"

#include <cstdint>
#include <iostream>
#include <string_view>

namespace {

using namespace noemancer;

bool check(const bool condition, const std::string_view message) {
    if (!condition) std::cerr << "raytracing_output_interop_tests: " << message << '\n';
    return condition;
}

RayTracingOutputInteropResource resource(const std::uint64_t generation = 1U,
                                         const std::string_view device = "gpu0") {
    RayTracingOutputInteropResource result;
    result.resource_id = "rt.output.main";
    result.backend = RayTracingOutputInteropBackend::d3d12;
    result.device_id = std::string(device);
    result.width = 1280U;
    result.height = 720U;
    result.format = "rgba16f";
    result.resource_generation = generation;
    return result;
}

RayTracingOutputInteropProducerFrame producer_frame(
    const RayTracingOutputInteropResource& output,
    const std::uint64_t sync_generation = 1U) {
    RayTracingOutputInteropProducerFrame result;
    result.resource = output;
    result.layout = RayTracingOutputInteropLayout::ray_tracing_output;
    result.ownership = RayTracingOutputInteropOwnership::ray_tracing;
    result.producer_sync_generation = sync_generation;
    result.producer_complete = true;
    return result;
}

RayTracingOutputInteropConsumerRequest consumer_request(
    const RayTracingOutputInteropResource& output,
    const std::uint64_t producer_sync_generation = 1U) {
    RayTracingOutputInteropConsumerRequest result;
    result.resource = output;
    result.expected_producer_sync_generation = producer_sync_generation;
    result.desired_layout = RayTracingOutputInteropLayout::shader_read;
    result.desired_ownership = RayTracingOutputInteropOwnership::renderer;
    result.allow_direct_share = true;
    result.direct_share_supported = true;
    result.allow_gpu_copy = true;
    result.gpu_copy_supported = false;
    return result;
}

bool test_direct_share_and_visual_promotion() {
    RayTracingOutputInterop interop;
    const auto output = resource();
    const auto configured = interop.configure(output);
    if (!check(configured.accepted && configured.state == RayTracingOutputInteropState::configured &&
                   configured.resource_generation == 1U && !configured.native_handles_exposed,
               "output identity was not configured as bounded plain data"))
        return false;
    const auto published = interop.publish(producer_frame(output, 7U));
    if (!check(published.accepted && published.state == RayTracingOutputInteropState::produced &&
                   published.producer_ready && published.producer_sync_generation == 7U,
               "completed producer output was not published"))
        return false;
    const auto acquired = interop.acquire(consumer_request(output, 7U));
    if (!check(acquired.accepted && acquired.completed &&
                   acquired.mode == RayTracingOutputInteropMode::direct_share &&
                   acquired.state == RayTracingOutputInteropState::consumed &&
                   acquired.device_compatibility == RayTracingOutputInteropDeviceCompatibility::same_device &&
                   acquired.consumer_sync_generation == 7U && acquired.ownership_transferred &&
                   acquired.visual_path_eligible && acquired.visual_path == "rt-direct-share" &&
                   !acquired.native_handles_exposed,
               "same-device output did not select direct sharing and visual promotion"))
        return false;
    const auto status = interop.status();
    return check(status.code == "consumer-ready" && status.visual_path_eligible &&
                     status.visual_path == "rt-direct-share" && status.consumer_ready,
                 "consumed direct-share state was not exposed by status");
}

bool test_gpu_copy_and_unavailable_path() {
    RayTracingOutputInterop interop;
    const auto output = resource();
    if (!check(interop.configure(output).accepted &&
                   interop.publish(producer_frame(output, 3U)).accepted,
               "GPU-copy fixture could not publish a producer frame"))
        return false;

    auto cross_device = consumer_request(resource(1U, "gpu1"), 3U);
    cross_device.direct_share_supported = true;
    cross_device.gpu_copy_supported = true;
    const auto copied = interop.acquire(cross_device);
    if (!check(copied.accepted && copied.mode == RayTracingOutputInteropMode::gpu_copy &&
                   copied.device_compatibility == RayTracingOutputInteropDeviceCompatibility::cross_device &&
                   copied.visual_path == "rt-gpu-copy" && copied.visual_path_eligible,
               "cross-device output did not select the explicit GPU-copy path"))
        return false;

    RayTracingOutputInterop unavailable;
    if (!check(unavailable.configure(output).accepted &&
                   unavailable.publish(producer_frame(output, 4U)).accepted,
               "unavailable fixture could not publish a producer frame"))
        return false;
    auto no_path = consumer_request(output, 4U);
    no_path.allow_direct_share = false;
    no_path.direct_share_supported = false;
    no_path.allow_gpu_copy = false;
    const auto rejected = unavailable.acquire(no_path);
    return check(!rejected.accepted && !rejected.visual_path_eligible &&
                     rejected.mode == RayTracingOutputInteropMode::unavailable &&
                     rejected.code == "transfer-path-unavailable" &&
                     unavailable.status().state == RayTracingOutputInteropState::produced,
                 "missing transfer paths did not remain an explicit raster fallback");
}

bool test_generation_layout_and_state_guards() {
    RayTracingOutputInterop interop;
    const auto output = resource();
    if (!check(interop.configure(output).accepted, "guard fixture could not be configured")) return false;
    const auto before_producer = interop.acquire(consumer_request(output));
    if (!check(!before_producer.accepted && before_producer.code == "producer-frame-missing" &&
                   before_producer.state == RayTracingOutputInteropState::configured,
               "acquisition before producer publication was not rejected"))
        return false;

    auto wrong_layout = producer_frame(output, 1U);
    wrong_layout.layout = RayTracingOutputInteropLayout::shader_read;
    const auto layout_rejected = interop.publish(wrong_layout);
    if (!check(!layout_rejected.accepted && layout_rejected.code == "producer-layout-invalid",
               "producer layout mismatch was accepted"))
        return false;
    if (!check(interop.publish(producer_frame(output, 5U)).accepted,
               "guard fixture could not publish a valid producer frame"))
        return false;

    auto stale = consumer_request(output, 4U);
    const auto stale_receipt = interop.acquire(stale);
    if (!check(!stale_receipt.accepted && stale_receipt.generation_stale &&
                   stale_receipt.code == "consumer-sync-stale" &&
                   interop.status().state == RayTracingOutputInteropState::produced,
               "stale producer synchronization generation was accepted"))
        return false;
    auto wrong_consumer_layout = consumer_request(output, 5U);
    wrong_consumer_layout.desired_layout = RayTracingOutputInteropLayout::copy_source;
    const auto consumer_layout = interop.acquire(wrong_consumer_layout);
    if (!check(!consumer_layout.accepted && consumer_layout.code == "consumer-layout-invalid",
               "consumer layout mismatch was accepted"))
        return false;
    auto valid = consumer_request(output, 5U);
    return check(interop.acquire(valid).accepted,
                 "a valid consumer could not retry after rejected guard operations");
}

bool test_resize_and_stale_resource_generation() {
    RayTracingOutputInterop interop;
    const auto output = resource(1U);
    if (!check(interop.configure(output).accepted, "resize fixture could not be configured")) return false;

    const auto same_generation = interop.resize(output);
    if (!check(!same_generation.accepted && same_generation.generation_stale &&
                   same_generation.code == "resize-generation-not-increasing",
               "resize accepted a non-increasing resource generation"))
        return false;
    auto wrong_identity = resource(2U);
    wrong_identity.format = "rgba8";
    const auto identity = interop.resize(wrong_identity);
    if (!check(!identity.accepted && identity.code == "resize-identity-mismatch",
               "resize accepted a changed resource identity"))
        return false;

    const auto resized = interop.resize(resource(2U));
    if (!check(resized.accepted && resized.resized &&
                   resized.state == RayTracingOutputInteropState::configured &&
                   resized.resource_generation == 2U && resized.producer_sync_generation == 0U,
               "resize did not retire the previous output generation"))
        return false;
    if (!check(interop.publish(producer_frame(resource(2U), 8U)).accepted,
               "resized fixture could not publish the new generation"))
        return false;
    auto stale_request = consumer_request(resource(1U), 8U);
    const auto stale = interop.acquire(stale_request);
    return check(!stale.accepted && stale.generation_stale &&
                     stale.code == "consumer-generation-stale" &&
                     interop.status().state == RayTracingOutputInteropState::produced,
                 "consumer acquired a retired output resource generation");
}

bool test_shutdown_is_terminal_and_fail_closed() {
    RayTracingOutputInterop interop;
    const auto output = resource();
    if (!check(interop.configure(output).accepted, "shutdown fixture could not be configured")) return false;
    const auto shutdown = interop.shutdown();
    if (!check(shutdown.accepted && shutdown.completed &&
                   shutdown.state == RayTracingOutputInteropState::shutdown,
               "shutdown did not enter the terminal state"))
        return false;
    const auto publish = interop.publish(producer_frame(output));
    const auto acquire = interop.acquire(consumer_request(output));
    const auto resize = interop.resize(resource(2U));
    if (!check(!publish.accepted && publish.code == "interop-shutdown" &&
                   !acquire.accepted && acquire.code == "interop-shutdown" &&
                   !resize.accepted && resize.code == "interop-shutdown",
               "post-shutdown transitions were not fail-closed"))
        return false;
    const auto again = interop.shutdown();
    return check(again.accepted && again.completed && again.code == "shutdown-idempotent" &&
                     interop.status().state == RayTracingOutputInteropState::shutdown,
                 "shutdown was not idempotent");
}

} // namespace

int main() {
    if (!test_direct_share_and_visual_promotion()) return 1;
    if (!test_gpu_copy_and_unavailable_path()) return 2;
    if (!test_generation_layout_and_state_guards()) return 3;
    if (!test_resize_and_stale_resource_generation()) return 4;
    if (!test_shutdown_is_terminal_and_fail_closed()) return 5;
    std::cout << "raytracing_output_interop_tests: ok\n";
    return 0;
}
