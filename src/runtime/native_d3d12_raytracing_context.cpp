#include "runtime/native_d3d12_raytracing_context.hpp"

#include <algorithm>
#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <ranges>
#include <string>
#include <utility>

#if defined(_WIN32)
#  define NOMINMAX
#  include <windows.h>
#  include <d3d12.h>
#  include <dxgi1_6.h>
#  include <wrl/client.h>
#endif

namespace noemancer {
namespace {

std::string bounded_text(const std::string_view value) {
    return std::string(value.substr(0U, native_d3d12_raytracing_context_max_text_bytes));
}

bool valid_identifier(const std::string_view value) {
    if (value.empty() || value.size() > native_d3d12_raytracing_context_max_text_bytes)
        return false;
    for (const auto byte : value) {
        const auto character = static_cast<unsigned char>(byte);
        if (character < 0x20U || character == 0x7fU || character == '/' || character == '\\')
            return false;
    }
    return true;
}

std::uint64_t hash_bytes(std::uint64_t hash, const void* data, const std::size_t size) noexcept {
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    for (std::size_t index = 0U; index < size; ++index) {
        hash ^= bytes[index];
        hash *= 1099511628211ULL;
    }
    return hash;
}

std::uint64_t hash_string(std::uint64_t hash, const std::string_view value) noexcept {
    hash = hash_bytes(hash, value.data(), value.size());
    const std::uint8_t separator = 0xffU;
    return hash_bytes(hash, &separator, sizeof(separator));
}

std::uint64_t scene_signature(const NativeD3D12RayTracingScene& scene) noexcept {
    std::uint64_t hash = 14695981039346656037ULL;
    hash = hash_string(hash, scene.scene_id);
    hash = hash_bytes(hash, &scene.revision, sizeof(scene.revision));
    hash = hash_bytes(hash, &scene.allow_update, sizeof(scene.allow_update));
    for (const auto& geometry : scene.geometries) {
        hash = hash_string(hash, geometry.geometry_id);
        hash = hash_bytes(hash, &geometry.allow_update, sizeof(geometry.allow_update));
        if (!geometry.position_xyz.empty())
            hash = hash_bytes(hash, geometry.position_xyz.data(),
                              geometry.position_xyz.size() * sizeof(float));
        if (!geometry.indices.empty())
            hash = hash_bytes(hash, geometry.indices.data(),
                              geometry.indices.size() * sizeof(std::uint32_t));
    }
    return hash == 0U ? 1U : hash;
}

// Resource identity is deliberately separated from content identity.  A
// revision or vertex edit can reuse persistent BLAS/TLAS storage when the
// geometry topology and update flags remain compatible; adding/removing a
// geometry or changing its vertex/index cardinality requires a rebuild.
std::uint64_t scene_topology_signature(const NativeD3D12RayTracingScene& scene) noexcept {
    std::uint64_t hash = 14695981039346656037ULL;
    hash = hash_string(hash, scene.scene_id);
    hash = hash_bytes(hash, &scene.allow_update, sizeof(scene.allow_update));
    for (const auto& geometry : scene.geometries) {
        hash = hash_string(hash, geometry.geometry_id);
        hash = hash_bytes(hash, &geometry.allow_update, sizeof(geometry.allow_update));
        const auto vertex_count = static_cast<std::uint64_t>(geometry.position_xyz.size() / 3U);
        const auto index_count = static_cast<std::uint64_t>(geometry.indices.size());
        hash = hash_bytes(hash, &vertex_count, sizeof(vertex_count));
        hash = hash_bytes(hash, &index_count, sizeof(index_count));
        // D3D12 AS update preserves the primitive topology.  Treat an index
        // stream edit as a rebuild boundary while allowing vertex position
        // edits to use the cheaper in-place update path.
        if (!geometry.indices.empty())
            hash = hash_bytes(hash, geometry.indices.data(),
                              geometry.indices.size() * sizeof(std::uint32_t));
    }
    return hash == 0U ? 1U : hash;
}

struct SceneValidation final {
    bool valid{};
    std::string code;
    std::string detail;
};

SceneValidation validate_scene(const NativeD3D12RayTracingScene& scene,
                               const NativeD3D12RayTracingContextOptions& options) {
    if (!valid_identifier(scene.scene_id))
        return {false, "native-d3d12.context.scene-id-invalid",
                "Scene ID must be non-empty, bounded and path-safe."};
    if (scene.geometries.empty())
        return {false, "native-d3d12.context.scene-empty",
                "A ray-tracing scene requires at least one triangle geometry."};
    const auto maximum_geometry_count = std::min<std::size_t>(
        options.max_geometry_count == 0U ? native_d3d12_raytracing_context_max_geometry_count
                                         : options.max_geometry_count,
        native_d3d12_raytracing_context_max_geometry_count);
    if (scene.geometries.size() > maximum_geometry_count)
        return {false, "native-d3d12.context.geometry-count-exceeded",
                "The scene geometry count exceeds the bounded context contract."};

    std::uint64_t total_bytes = 0U;
    std::string previous_id;
    bool first = true;
    for (const auto& geometry : scene.geometries) {
        if (!valid_identifier(geometry.geometry_id))
            return {false, "native-d3d12.context.geometry-id-invalid",
                    "Geometry IDs must be non-empty, bounded and path-safe."};
        if (!first && previous_id == geometry.geometry_id)
            return {false, "native-d3d12.context.geometry-id-duplicate",
                    "Geometry IDs must be unique within one scene."};
        previous_id = geometry.geometry_id;
        first = false;
        if (geometry.position_xyz.size() < 9U || geometry.position_xyz.size() % 3U != 0U)
            return {false, "native-d3d12.context.vertex-layout-invalid",
                    "Position input must contain at least one triangle of XYZ floats."};
        if (geometry.indices.size() < 3U || geometry.indices.size() % 3U != 0U)
            return {false, "native-d3d12.context.index-layout-invalid",
                    "Index input must contain at least one triangle of 32-bit indices."};
        const auto vertex_count = geometry.position_xyz.size() / 3U;
        if (vertex_count > native_d3d12_raytracing_context_max_vertex_count ||
            geometry.indices.size() > native_d3d12_raytracing_context_max_index_count)
            return {false, "native-d3d12.context.geometry-size-exceeded",
                    "A geometry exceeds the bounded vertex or index contract."};
        for (const auto coordinate : geometry.position_xyz) {
            if (!std::isfinite(coordinate))
                return {false, "native-d3d12.context.vertex-non-finite",
                        "Position input must contain finite coordinates."};
        }
        for (const auto index : geometry.indices) {
            if (index >= vertex_count)
                return {false, "native-d3d12.context.index-out-of-range",
                        "A triangle index refers to a vertex outside the position buffer."};
        }
        const auto vertex_bytes = static_cast<std::uint64_t>(geometry.position_xyz.size()) * sizeof(float);
        const auto index_bytes = static_cast<std::uint64_t>(geometry.indices.size()) * sizeof(std::uint32_t);
        if (vertex_bytes > std::numeric_limits<std::uint64_t>::max() - total_bytes)
            return {false, "native-d3d12.context.resource-size-overflow",
                    "Scene resource byte accounting overflowed."};
        total_bytes += vertex_bytes;
        if (index_bytes > std::numeric_limits<std::uint64_t>::max() - total_bytes)
            return {false, "native-d3d12.context.resource-size-overflow",
                    "Scene resource byte accounting overflowed."};
        total_bytes += index_bytes;
    }
    const auto maximum_resource_bytes = options.max_resource_bytes == 0U
        ? native_d3d12_raytracing_context_max_resource_bytes
        : std::min(options.max_resource_bytes, native_d3d12_raytracing_context_max_resource_bytes);
    if (total_bytes == 0U || total_bytes > maximum_resource_bytes)
        return {false, "native-d3d12.context.resource-budget-exceeded",
                "Scene geometry exceeds the bounded upload resource budget."};
    return {true, {}, {}};
}

void sort_scene(NativeD3D12RayTracingScene& scene) {
    scene.revision = scene.revision == 0U ? 1U : scene.revision;
    std::sort(scene.geometries.begin(), scene.geometries.end(),
              [](const auto& left, const auto& right) {
                  if (left.geometry_id != right.geometry_id)
                      return left.geometry_id < right.geometry_id;
                  if (left.allow_update != right.allow_update)
                      return left.allow_update < right.allow_update;
                  if (left.position_xyz != right.position_xyz)
                      return left.position_xyz < right.position_xyz;
                  return left.indices < right.indices;
              });
}

#if defined(_WIN32)

using Microsoft::WRL::ComPtr;
using CreateFactoryFn = HRESULT(WINAPI*)(UINT, REFIID, void**);
using CreateDeviceFn = HRESULT(WINAPI*)(IUnknown*, D3D_FEATURE_LEVEL, REFIID, void**);

std::string hresult_hex(const HRESULT value) {
    char buffer[32]{};
    std::snprintf(buffer, sizeof(buffer), "0x%08" PRIx32,
                  static_cast<std::uint32_t>(value));
    return buffer;
}

std::string utf8_from_wide(const wchar_t* value) {
    if (value == nullptr || value[0] == L'\0') return {};
    const int required = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
                                             value, -1, nullptr, 0, nullptr, nullptr);
    if (required <= 1) return {};
    std::string result(static_cast<std::size_t>(required), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value, -1,
                            result.data(), required, nullptr, nullptr) <= 0)
        return {};
    if (!result.empty() && result.back() == '\0') result.pop_back();
    return result;
}

struct D3D12Module final {
    HMODULE handle{};
    D3D12Module() = default;
    D3D12Module(const D3D12Module&) = delete;
    D3D12Module& operator=(const D3D12Module&) = delete;
    ~D3D12Module() {
        if (handle != nullptr) FreeLibrary(handle);
    }
    [[nodiscard]] bool load() {
        handle = LoadLibraryW(L"d3d12.dll");
        return handle != nullptr;
    }
    template <typename Function>
    [[nodiscard]] Function symbol(const char* name) const {
        if (handle == nullptr) return nullptr;
        return reinterpret_cast<Function>(GetProcAddress(handle, name));
    }
};

struct DxgiModule final {
    HMODULE handle{};
    DxgiModule() = default;
    DxgiModule(const DxgiModule&) = delete;
    DxgiModule& operator=(const DxgiModule&) = delete;
    ~DxgiModule() {
        if (handle != nullptr) FreeLibrary(handle);
    }
    [[nodiscard]] bool load() {
        handle = LoadLibraryW(L"dxgi.dll");
        return handle != nullptr;
    }
    template <typename Function>
    [[nodiscard]] Function symbol(const char* name) const {
        if (handle == nullptr) return nullptr;
        return reinterpret_cast<Function>(GetProcAddress(handle, name));
    }
};

bool resource_bytes_bounded(const std::uint64_t bytes) noexcept {
    return bytes > 0U && bytes <= native_d3d12_raytracing_context_max_resource_bytes;
}

HRESULT create_committed_buffer(ID3D12Device* device, const std::uint64_t bytes,
                                const D3D12_HEAP_TYPE heap_type,
                                const D3D12_RESOURCE_STATES state,
                                const D3D12_RESOURCE_FLAGS flags,
                                ComPtr<ID3D12Resource>& resource) {
    if (device == nullptr || !resource_bytes_bounded(bytes))
        return E_INVALIDARG;
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = heap_type;
    heap.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    heap.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;

    D3D12_RESOURCE_DESC description{};
    description.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    description.Width = bytes;
    description.Height = 1U;
    description.DepthOrArraySize = 1U;
    description.MipLevels = 1U;
    description.Format = DXGI_FORMAT_UNKNOWN;
    description.SampleDesc.Count = 1U;
    description.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    description.Flags = flags;

    return device->CreateCommittedResource(
        &heap, D3D12_HEAP_FLAG_NONE, &description, state, nullptr,
        IID_PPV_ARGS(&resource));
}

bool fill_upload_buffer(ID3D12Resource* resource, const void* data,
                        const std::size_t bytes) {
    if (resource == nullptr || data == nullptr || bytes == 0U)
        return false;
    void* mapped = nullptr;
    const D3D12_RANGE read_range{0U, 0U};
    if (FAILED(resource->Map(0U, &read_range, &mapped)) || mapped == nullptr)
        return false;
    std::memcpy(mapped, data, bytes);
    resource->Unmap(0U, nullptr);
    return true;
}

#endif

} // namespace

struct NativeD3D12RayTracingContext::Impl final {
    NativeD3D12RayTracingContextOptions options;
    NativeD3D12RayTracingContextState state{
        NativeD3D12RayTracingContextState::uninitialized};
    std::uint64_t generation{};
    std::uint64_t scene_generation{};
    std::uint64_t resource_generation{};
    std::optional<NativeD3D12RayTracingScene> scene;
    std::uint64_t scene_hash{};
    bool scene_dirty{};
    bool native_handles_exposed{};
    std::string device_name;
    std::uint32_t raytracing_tier{};
    std::string last_code;
    std::string last_detail;
    NativeD3D12RayTracingContextFailureStage last_stage{
        NativeD3D12RayTracingContextFailureStage::none};

#if defined(_WIN32)
    struct GeometryResources final {
        ComPtr<ID3D12Resource> vertex_buffer;
        ComPtr<ID3D12Resource> vertex_upload;
        ComPtr<ID3D12Resource> index_buffer;
        ComPtr<ID3D12Resource> index_upload;
        ComPtr<ID3D12Resource> blas_result;
        ComPtr<ID3D12Resource> blas_scratch;
        std::uint64_t vertex_bytes{};
        std::uint64_t index_bytes{};
        std::uint64_t blas_result_bytes{};
        std::uint64_t blas_scratch_bytes{};
        std::uint32_t vertex_count{};
        std::uint32_t index_count{};
        bool allow_update{};
    };

    D3D12Module d3d12_module;
    DxgiModule dxgi_module;
    ComPtr<IDXGIFactory4> factory;
    ComPtr<ID3D12Device5> device;
    ComPtr<ID3D12CommandQueue> command_queue;
    ComPtr<ID3D12CommandAllocator> command_allocator;
    ComPtr<ID3D12GraphicsCommandList4> command_list;
    ComPtr<ID3D12Fence> fence;
    std::uint64_t next_fence_value{1U};
    HANDLE fence_event{};
    std::vector<GeometryResources> geometry_resources;
    ComPtr<ID3D12Resource> instance_buffer;
    ComPtr<ID3D12Resource> tlas_result;
    ComPtr<ID3D12Resource> tlas_scratch;
    std::uint64_t instance_buffer_bytes{};
    std::uint64_t tlas_result_bytes{};
    std::uint64_t tlas_scratch_bytes{};
    std::uint64_t built_scene_hash{};
    std::uint64_t built_topology_hash{};
    bool blas_ready{};
    bool tlas_ready{};
    bool last_build_submitted{};
    bool last_build_completed{};
    bool last_update_submitted{};
    bool last_update_completed{};
    bool last_synchronization_completed{};
#endif

    explicit Impl(NativeD3D12RayTracingContextOptions input)
        : options(std::move(input)) {
        if (options.output_width == 0U) options.output_width = 1U;
        if (options.output_height == 0U) options.output_height = 1U;
        options.output_width = std::min(options.output_width, 4096U);
        options.output_height = std::min(options.output_height, 4096U);
        if (options.max_geometry_count == 0U)
            options.max_geometry_count = native_d3d12_raytracing_context_max_geometry_count;
        options.max_geometry_count = std::min(
            options.max_geometry_count, native_d3d12_raytracing_context_max_geometry_count);
        if (options.max_resource_bytes == 0U)
            options.max_resource_bytes = native_d3d12_raytracing_context_max_resource_bytes;
        options.max_resource_bytes = std::min(
            options.max_resource_bytes, native_d3d12_raytracing_context_max_resource_bytes);
    }
};

void NativeD3D12RayTracingContext::save_result(
    NativeD3D12RayTracingContext::Impl& impl,
    const NativeD3D12RayTracingContextFailureStage stage,
    const std::string_view code, const std::string_view detail) {
    impl.last_stage = stage;
    impl.last_code = bounded_text(code);
    impl.last_detail = bounded_text(detail);
}

NativeD3D12RayTracingContextReceipt NativeD3D12RayTracingContext::receipt_from(
    const NativeD3D12RayTracingContext::Impl& impl, const std::string_view operation) {
    NativeD3D12RayTracingContextReceipt result;
    result.operation = std::string(operation);
    result.state = impl.state;
    result.failure_stage = impl.last_stage;
    result.code = impl.last_code;
    result.detail = impl.last_detail;
    result.device_name = bounded_text(impl.device_name);
    result.native_handle_exposed = false;
    result.initialized = impl.generation != 0U;
#if defined(_WIN32)
    result.device_ready = impl.device != nullptr;
    result.command_queue_ready = impl.command_queue != nullptr;
    result.fence_ready = impl.fence != nullptr;
#else
    result.device_ready = false;
    result.command_queue_ready = false;
    result.fence_ready = false;
#endif
    result.generation = impl.generation;
    result.scene_generation = impl.scene_generation;
    result.resource_generation = impl.resource_generation;
    result.scene_received = impl.scene.has_value();
    if (impl.scene) {
        result.scene_revision = impl.scene->revision;
        result.geometry_count = static_cast<std::uint32_t>(impl.scene->geometries.size());
        result.instance_count = result.geometry_count;
    }
    result.raytracing_tier = impl.raytracing_tier;
    result.output_width = impl.options.output_width;
    result.output_height = impl.options.output_height;
    result.output_pixel_stride_bytes = sizeof(std::uint32_t) * 4U;
    result.output_bytes = static_cast<std::uint64_t>(result.output_width) * result.output_height *
        result.output_pixel_stride_bytes;
    result.output_readback_bytes = result.output_bytes;
#if defined(_WIN32)
    result.blas_ready = impl.blas_ready;
    result.tlas_ready = impl.tlas_ready;
    result.build_submitted = impl.last_build_submitted;
    result.build_completed = impl.last_build_completed;
    result.update_submitted = impl.last_update_submitted;
    result.update_completed = impl.last_update_completed;
    result.synchronization_completed = impl.last_synchronization_completed;
    for (const auto& geometry : impl.geometry_resources) {
        result.vertex_buffer_bytes += geometry.vertex_bytes;
        result.index_buffer_bytes += geometry.index_bytes;
        result.blas_result_bytes += geometry.blas_result_bytes;
        result.blas_scratch_bytes += geometry.blas_scratch_bytes;
    }
    result.blas_ready = impl.blas_ready;
    result.tlas_ready = impl.tlas_ready;
    result.tlas_result_bytes = impl.tlas_result_bytes;
    result.tlas_scratch_bytes = impl.tlas_scratch_bytes;
    result.output_resource_ready = false;
    result.shader_pipeline_ready = false;
    result.shader_table_ready = false;
#endif
    result.fallback_active = impl.state == NativeD3D12RayTracingContextState::unsupported;
    if (impl.state == NativeD3D12RayTracingContextState::shutdown) result.shutdown_completed = true;
    return result;
}

void NativeD3D12RayTracingContext::mark_unsupported(
    NativeD3D12RayTracingContext::Impl& impl,
    const NativeD3D12RayTracingContextFailureStage stage,
    const std::string_view operation,
    const std::string_view code, const std::string_view detail,
    NativeD3D12RayTracingContextReceipt& receipt) {
    save_result(impl, stage, code, detail);
    receipt = receipt_from(impl, operation);
    receipt.state = NativeD3D12RayTracingContextState::unsupported;
    receipt.fallback_active = true;
}

namespace {

#if defined(_WIN32)

bool select_hardware_device(IDXGIFactory4* factory, CreateDeviceFn create_device,
                            ID3D12Device5** output_device, std::string& output_name,
                            std::uint32_t& output_tier) {
    if (factory == nullptr || create_device == nullptr || output_device == nullptr) return false;
    *output_device = nullptr;
    ComPtr<IDXGIAdapter1> adapter;
    for (UINT index = 0U; factory->EnumAdapters1(index, &adapter) != DXGI_ERROR_NOT_FOUND; ++index) {
        DXGI_ADAPTER_DESC1 description{};
        if (FAILED(adapter->GetDesc1(&description))) {
            adapter.Reset();
            continue;
        }
        if ((description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0U) {
            adapter.Reset();
            continue;
        }
        ComPtr<ID3D12Device5> candidate;
        const auto hr = create_device(adapter.Get(), D3D_FEATURE_LEVEL_12_1,
                                      __uuidof(ID3D12Device5),
                                      reinterpret_cast<void**>(candidate.GetAddressOf()));
        if (FAILED(hr) || !candidate) {
            adapter.Reset();
            continue;
        }
        D3D12_FEATURE_DATA_D3D12_OPTIONS5 options5{};
        if (FAILED(candidate->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5,
                                                  &options5, sizeof(options5))) ||
            options5.RaytracingTier == D3D12_RAYTRACING_TIER_NOT_SUPPORTED) {
            adapter.Reset();
            continue;
        }
        *output_device = candidate.Detach();
        output_name = utf8_from_wide(description.Description);
        output_tier = static_cast<std::uint32_t>(options5.RaytracingTier);
        return true;
    }
    return false;
}

HRESULT submit_and_wait(ID3D12CommandQueue* queue,
                        ID3D12Fence* fence,
                        ID3D12GraphicsCommandList4* command_list,
                        HANDLE& fence_event,
                        std::uint64_t& next_fence_value) {
    if (queue == nullptr || fence == nullptr || command_list == nullptr ||
        next_fence_value == 0U || next_fence_value == std::numeric_limits<std::uint64_t>::max())
        return E_INVALIDARG;
    ID3D12CommandList* command_lists[] = {command_list};
    queue->ExecuteCommandLists(1U, command_lists);
    const auto fence_value = next_fence_value++;
    HRESULT hr = queue->Signal(fence, fence_value);
    if (FAILED(hr)) return hr;
    if (fence->GetCompletedValue() >= fence_value) return S_OK;
    if (fence_event == nullptr) {
        fence_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (fence_event == nullptr) return HRESULT_FROM_WIN32(GetLastError());
    }
    hr = fence->SetEventOnCompletion(fence_value, fence_event);
    if (FAILED(hr)) return hr;
    return WaitForSingleObject(fence_event, INFINITE) == WAIT_OBJECT_0
        ? S_OK : HRESULT_FROM_WIN32(GetLastError());
}

void transition_resource(ID3D12GraphicsCommandList4* command_list,
                         ID3D12Resource* resource,
                         const D3D12_RESOURCE_STATES before,
                         const D3D12_RESOURCE_STATES after) {
    if (command_list == nullptr || resource == nullptr || before == after) return;
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = resource;
    barrier.Transition.StateBefore = before;
    barrier.Transition.StateAfter = after;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    command_list->ResourceBarrier(1U, &barrier);
}

void uav_barrier(ID3D12GraphicsCommandList4* command_list, ID3D12Resource* resource) {
    if (command_list == nullptr || resource == nullptr) return;
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    barrier.UAV.pResource = resource;
    command_list->ResourceBarrier(1U, &barrier);
}

#endif

} // namespace

std::string_view native_d3d12_raytracing_context_state_name(
    const NativeD3D12RayTracingContextState state) noexcept {
    switch (state) {
    case NativeD3D12RayTracingContextState::uninitialized: return "uninitialized";
    case NativeD3D12RayTracingContextState::ready: return "ready";
    case NativeD3D12RayTracingContextState::unsupported: return "unsupported";
    case NativeD3D12RayTracingContextState::failed: return "failed";
    case NativeD3D12RayTracingContextState::shutdown: return "shutdown";
    }
    return "failed";
}

std::string_view native_d3d12_raytracing_context_failure_stage_name(
    const NativeD3D12RayTracingContextFailureStage stage) noexcept {
    switch (stage) {
    case NativeD3D12RayTracingContextFailureStage::none: return "none";
    case NativeD3D12RayTracingContextFailureStage::platform: return "platform";
    case NativeD3D12RayTracingContextFailureStage::loader: return "loader";
    case NativeD3D12RayTracingContextFailureStage::factory: return "factory";
    case NativeD3D12RayTracingContextFailureStage::adapter: return "adapter";
    case NativeD3D12RayTracingContextFailureStage::device: return "device";
    case NativeD3D12RayTracingContextFailureStage::feature: return "feature";
    case NativeD3D12RayTracingContextFailureStage::command_queue: return "command-queue";
    case NativeD3D12RayTracingContextFailureStage::command_allocator: return "command-allocator";
    case NativeD3D12RayTracingContextFailureStage::command_list: return "command-list";
    case NativeD3D12RayTracingContextFailureStage::fence: return "fence";
    case NativeD3D12RayTracingContextFailureStage::scene: return "scene";
    case NativeD3D12RayTracingContextFailureStage::blas: return "blas";
    case NativeD3D12RayTracingContextFailureStage::tlas: return "tlas";
    case NativeD3D12RayTracingContextFailureStage::shader_pipeline: return "shader-pipeline";
    case NativeD3D12RayTracingContextFailureStage::shader_table: return "shader-table";
    case NativeD3D12RayTracingContextFailureStage::output: return "output";
    case NativeD3D12RayTracingContextFailureStage::trace: return "trace";
    case NativeD3D12RayTracingContextFailureStage::readback: return "readback";
    case NativeD3D12RayTracingContextFailureStage::synchronization: return "synchronization";
    case NativeD3D12RayTracingContextFailureStage::cleanup: return "cleanup";
    }
    return "cleanup";
}

NativeD3D12RayTracingContext::NativeD3D12RayTracingContext(
    NativeD3D12RayTracingContextOptions options)
    : impl_(std::make_unique<Impl>(std::move(options))) {}

NativeD3D12RayTracingContext::~NativeD3D12RayTracingContext() {
    static_cast<void>(shutdown());
}

NativeD3D12RayTracingContextReceipt NativeD3D12RayTracingContext::initialize() {
    if (impl_->state == NativeD3D12RayTracingContextState::shutdown) {
        save_result(*impl_, NativeD3D12RayTracingContextFailureStage::cleanup,
                    "native-d3d12.context.already-shutdown",
                    "The D3D12 context was already shut down and cannot be reinitialized.");
        auto result = receipt_from(*impl_, "initialize");
        result.state = NativeD3D12RayTracingContextState::shutdown;
        return result;
    }
    if (impl_->state != NativeD3D12RayTracingContextState::uninitialized) {
        save_result(*impl_, NativeD3D12RayTracingContextFailureStage::none,
                    "native-d3d12.context.already-initialized",
                    "The D3D12 device, queue and fence context is already initialized.");
        return receipt_from(*impl_, "initialize");
    }
    if (impl_->options.output_width == 0U || impl_->options.output_height == 0U) {
        impl_->state = NativeD3D12RayTracingContextState::failed;
        save_result(*impl_, NativeD3D12RayTracingContextFailureStage::output,
                    "native-d3d12.context.output-size-invalid",
                    "Output dimensions must be positive and bounded.");
        return receipt_from(*impl_, "initialize");
    }
    const auto shader_bytes = static_cast<std::uint64_t>(
        impl_->options.shaders.ray_generation_dxil.size()) +
        static_cast<std::uint64_t>(impl_->options.shaders.miss_dxil.size()) +
        static_cast<std::uint64_t>(impl_->options.shaders.closest_hit_dxil.size());
    if (shader_bytes > impl_->options.max_resource_bytes) {
        impl_->state = NativeD3D12RayTracingContextState::failed;
        save_result(*impl_, NativeD3D12RayTracingContextFailureStage::shader_pipeline,
                    "native-d3d12.context.shader-budget-exceeded",
                    "The supplied DXIL shader set exceeds the bounded context resource budget.");
        return receipt_from(*impl_, "initialize");
    }

#if !defined(_WIN32)
    impl_->state = NativeD3D12RayTracingContextState::unsupported;
    save_result(*impl_, NativeD3D12RayTracingContextFailureStage::platform,
                "native-d3d12.context.platform-unavailable",
                "The persistent D3D12 context is available only on Windows; raster fallback remains explicit.");
    auto result = receipt_from(*impl_, "initialize");
    result.state = NativeD3D12RayTracingContextState::unsupported;
    result.fallback_active = true;
    return result;
#else
    if (!impl_->d3d12_module.load() || !impl_->dxgi_module.load()) {
        impl_->state = NativeD3D12RayTracingContextState::unsupported;
        save_result(*impl_, NativeD3D12RayTracingContextFailureStage::loader,
                    "native-d3d12.context.loader-unavailable",
                    "d3d12.dll or dxgi.dll could not be loaded; no native handles were retained.");
        auto result = receipt_from(*impl_, "initialize");
        result.state = NativeD3D12RayTracingContextState::unsupported;
        result.fallback_active = true;
        return result;
    }
    const auto create_device = impl_->d3d12_module.symbol<CreateDeviceFn>("D3D12CreateDevice");
    const auto create_factory = impl_->dxgi_module.symbol<CreateFactoryFn>("CreateDXGIFactory2");
    if (create_device == nullptr || create_factory == nullptr) {
        impl_->state = NativeD3D12RayTracingContextState::unsupported;
        save_result(*impl_, NativeD3D12RayTracingContextFailureStage::loader,
                    "native-d3d12.context.entrypoint-unavailable",
                    "The D3D12CreateDevice or CreateDXGIFactory2 entry point is unavailable.");
        auto result = receipt_from(*impl_, "initialize");
        result.state = NativeD3D12RayTracingContextState::unsupported;
        result.fallback_active = true;
        return result;
    }
    HRESULT hr = create_factory(0U, __uuidof(IDXGIFactory4),
                                reinterpret_cast<void**>(impl_->factory.GetAddressOf()));
    if (FAILED(hr) || !impl_->factory) {
        impl_->state = NativeD3D12RayTracingContextState::failed;
        save_result(*impl_, NativeD3D12RayTracingContextFailureStage::factory,
                    "native-d3d12.context.factory-create-failed",
                    "CreateDXGIFactory2 failed with " + hresult_hex(hr) + ".");
        return receipt_from(*impl_, "initialize");
    }
    hr = select_hardware_device(impl_->factory.Get(), create_device,
                                impl_->device.GetAddressOf(), impl_->device_name,
                                impl_->raytracing_tier)
        ? S_OK : DXGI_ERROR_UNSUPPORTED;
    if (FAILED(hr) || !impl_->device) {
        impl_->state = NativeD3D12RayTracingContextState::unsupported;
        save_result(*impl_, NativeD3D12RayTracingContextFailureStage::feature,
                    "native-d3d12.context.hardware-unsupported",
                    impl_->options.probe_warp_fallback
                        ? "No hardware D3D12 ray-tracing device was available; WARP is recorded only as a raster fallback."
                        : "No hardware D3D12 device with a ray-tracing tier was available.");
        auto result = receipt_from(*impl_, "initialize");
        result.state = NativeD3D12RayTracingContextState::unsupported;
        result.fallback_active = true;
        return result;
    }
    D3D12_COMMAND_QUEUE_DESC queue_description{};
    queue_description.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    hr = impl_->device->CreateCommandQueue(&queue_description,
                                           IID_PPV_ARGS(&impl_->command_queue));
    if (FAILED(hr) || !impl_->command_queue) {
        impl_->state = NativeD3D12RayTracingContextState::failed;
        save_result(*impl_, NativeD3D12RayTracingContextFailureStage::command_queue,
                    "native-d3d12.context.command-queue-create-failed",
                    "CreateCommandQueue failed with " + hresult_hex(hr) + ".");
        return receipt_from(*impl_, "initialize");
    }
    hr = impl_->device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                IID_PPV_ARGS(&impl_->command_allocator));
    if (FAILED(hr) || !impl_->command_allocator) {
        impl_->state = NativeD3D12RayTracingContextState::failed;
        save_result(*impl_, NativeD3D12RayTracingContextFailureStage::command_allocator,
                    "native-d3d12.context.command-allocator-create-failed",
                    "CreateCommandAllocator failed with " + hresult_hex(hr) + ".");
        return receipt_from(*impl_, "initialize");
    }
    hr = impl_->device->CreateCommandList(0U, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                          impl_->command_allocator.Get(), nullptr,
                                          IID_PPV_ARGS(&impl_->command_list));
    if (FAILED(hr) || !impl_->command_list || FAILED(impl_->command_list->Close())) {
        impl_->state = NativeD3D12RayTracingContextState::failed;
        save_result(*impl_, NativeD3D12RayTracingContextFailureStage::command_list,
                    "native-d3d12.context.command-list-create-failed",
                    "CreateCommandList or its initial Close failed with " + hresult_hex(hr) + ".");
        return receipt_from(*impl_, "initialize");
    }
    hr = impl_->device->CreateFence(0U, D3D12_FENCE_FLAG_NONE,
                                    IID_PPV_ARGS(&impl_->fence));
    if (FAILED(hr) || !impl_->fence) {
        impl_->state = NativeD3D12RayTracingContextState::failed;
        save_result(*impl_, NativeD3D12RayTracingContextFailureStage::fence,
                    "native-d3d12.context.fence-create-failed",
                    "CreateFence failed with " + hresult_hex(hr) + ".");
        return receipt_from(*impl_, "initialize");
    }
    impl_->generation = 1U;
    impl_->state = NativeD3D12RayTracingContextState::ready;
    save_result(*impl_, NativeD3D12RayTracingContextFailureStage::none,
                "native-d3d12.context.initialized",
                "Persistent D3D12 device, direct queue, command allocator/list and fence are retained; BLAS/TLAS materialization is available while shader/SBT/trace remain separate capability gates.");
    return receipt_from(*impl_, "initialize");
#endif
}

NativeD3D12RayTracingContextReceipt NativeD3D12RayTracingContext::ensure_scene(
    const NativeD3D12RayTracingScene& input_scene) {
    auto canonical_scene = input_scene;
    sort_scene(canonical_scene);
    const auto validation = validate_scene(canonical_scene, impl_->options);
    if (!validation.valid) {
        save_result(*impl_, NativeD3D12RayTracingContextFailureStage::scene,
                    validation.code, validation.detail);
        auto result = receipt_from(*impl_, "ensure-scene");
        result.state = NativeD3D12RayTracingContextState::failed;
        result.fallback_active = false;
        return result;
    }
    if (impl_->state == NativeD3D12RayTracingContextState::uninitialized) {
        static_cast<void>(initialize());
    }
    if (impl_->state == NativeD3D12RayTracingContextState::shutdown) {
        save_result(*impl_, NativeD3D12RayTracingContextFailureStage::cleanup,
                    "native-d3d12.context.already-shutdown",
                    "The D3D12 context was shut down; a new scene cannot be attached.");
        auto result = receipt_from(*impl_, "ensure-scene");
        result.state = NativeD3D12RayTracingContextState::shutdown;
        return result;
    }

    const auto signature = scene_signature(canonical_scene);
    const bool changed = !impl_->scene || impl_->scene_hash != signature;
    if (changed) {
        impl_->scene = std::move(canonical_scene);
        impl_->scene_hash = signature;
        impl_->scene_dirty = true;
        if (impl_->scene_generation != std::numeric_limits<std::uint64_t>::max())
            ++impl_->scene_generation;
    }
    save_result(*impl_, NativeD3D12RayTracingContextFailureStage::none,
                changed ? "native-d3d12.context.scene-changed" : "native-d3d12.context.scene-unchanged",
        changed ? "The bounded scene snapshot is retained for a later AS build/update operation."
                        : "The scene fingerprint is unchanged; persistent native resources can be reused without a new AS submission.");
    auto result = receipt_from(*impl_, "ensure-scene");
    result.scene_changed = changed;
    result.scene_received = true;
    if (impl_->state == NativeD3D12RayTracingContextState::unsupported) {
        result.state = NativeD3D12RayTracingContextState::unsupported;
        result.fallback_active = true;
    }
    return result;
}

NativeD3D12RayTracingContextReceipt NativeD3D12RayTracingContext::build_or_update() {
    if (impl_->state == NativeD3D12RayTracingContextState::uninitialized)
        static_cast<void>(initialize());
#if defined(_WIN32)
    impl_->last_build_submitted = false;
    impl_->last_build_completed = false;
    impl_->last_update_submitted = false;
    impl_->last_update_completed = false;
    impl_->last_synchronization_completed = false;
#endif
    if (!impl_->scene) {
        save_result(*impl_, NativeD3D12RayTracingContextFailureStage::scene,
                    "native-d3d12.context.scene-not-provided",
                    "Call ensure_scene with a validated triangle scene before build_or_update.");
        auto result = receipt_from(*impl_, "build-or-update");
        result.state = NativeD3D12RayTracingContextState::failed;
        result.fallback_active = false;
        return result;
    }
    if (impl_->state != NativeD3D12RayTracingContextState::ready) {
        auto result = receipt_from(*impl_, "build-or-update");
        result.scene_received = true;
        if (impl_->state == NativeD3D12RayTracingContextState::unsupported) {
            save_result(*impl_, NativeD3D12RayTracingContextFailureStage::blas,
                        "native-d3d12.context.as-materialization-unavailable",
                        "Hardware D3D12 ray-tracing resources are unavailable; the caller must use the explicit raster fallback.");
            result = receipt_from(*impl_, "build-or-update");
            result.state = NativeD3D12RayTracingContextState::unsupported;
            result.fallback_active = true;
        }
        return result;
    }

#if !defined(_WIN32)
    auto result = receipt_from(*impl_, "build-or-update");
    result.scene_received = true;
    mark_unsupported(*impl_, NativeD3D12RayTracingContextFailureStage::platform,
                     "build-or-update",
                     "native-d3d12.context.platform-unavailable",
                     "The persistent D3D12 context is unavailable on this platform; no GPU resource is reported ready.",
                     result);
    return result;
#else
    const auto topology_hash = scene_topology_signature(*impl_->scene);
    const bool has_compatible_resources =
        impl_->blas_ready && impl_->tlas_ready &&
        impl_->built_topology_hash == topology_hash &&
        impl_->geometry_resources.size() == impl_->scene->geometries.size() &&
        impl_->instance_buffer != nullptr && impl_->tlas_result != nullptr &&
        impl_->tlas_scratch != nullptr;
    const bool can_update = has_compatible_resources && impl_->scene->allow_update &&
        std::ranges::all_of(impl_->scene->geometries,
                            [](const auto& geometry) { return geometry.allow_update; }) &&
        std::ranges::all_of(impl_->geometry_resources,
                            [](const auto& geometry) { return geometry.allow_update; });

    if (!impl_->scene_dirty && impl_->built_scene_hash == impl_->scene_hash &&
        has_compatible_resources) {
        save_result(*impl_, NativeD3D12RayTracingContextFailureStage::none,
                    "native-d3d12.context.resources-reused",
                    "The persistent vertex/index buffers, BLAS, TLAS and instance descriptors were reused without a new GPU submission.");
        auto result = receipt_from(*impl_, "build-or-update");
        result.scene_received = true;
        result.state = NativeD3D12RayTracingContextState::ready;
        result.synchronization_completed = true;
        return result;
    }

    const auto fail = [&](const NativeD3D12RayTracingContextFailureStage stage,
                          const std::string_view code,
                          const std::string_view detail) {
        impl_->state = NativeD3D12RayTracingContextState::failed;
        save_result(*impl_, stage, code, detail);
        auto result = receipt_from(*impl_, "build-or-update");
        result.state = NativeD3D12RayTracingContextState::failed;
        result.fallback_active = false;
        result.scene_received = true;
        return result;
    };

    const bool rebuild = !can_update;
    std::vector<Impl::GeometryResources> next_geometry_resources;
    ComPtr<ID3D12Resource> next_instance_buffer;
    ComPtr<ID3D12Resource> next_tlas_result;
    ComPtr<ID3D12Resource> next_tlas_scratch;
    std::uint64_t next_tlas_result_bytes = 0U;
    std::uint64_t next_tlas_scratch_bytes = 0U;
    std::uint64_t total_resource_bytes = 0U;
    const auto resource_budget = impl_->options.max_resource_bytes;
    const auto add_resource_budget = [&](const std::uint64_t bytes) {
        if (bytes == 0U || bytes > resource_budget -
                std::min(total_resource_bytes, resource_budget))
            return false;
        total_resource_bytes += bytes;
        return total_resource_bytes <= resource_budget;
    };

    if (rebuild) {
        next_geometry_resources.reserve(impl_->scene->geometries.size());
        for (const auto& geometry : impl_->scene->geometries) {
            Impl::GeometryResources resources;
            const auto vertex_bytes = static_cast<std::uint64_t>(
                geometry.position_xyz.size()) * sizeof(float);
            const auto index_bytes = static_cast<std::uint64_t>(
                geometry.indices.size()) * sizeof(std::uint32_t);
            resources.vertex_bytes = vertex_bytes;
            resources.index_bytes = index_bytes;
            resources.vertex_count = static_cast<std::uint32_t>(
                geometry.position_xyz.size() / 3U);
            resources.index_count = static_cast<std::uint32_t>(geometry.indices.size());
            resources.allow_update = impl_->scene->allow_update && geometry.allow_update;
            if (!add_resource_budget(vertex_bytes) || !add_resource_budget(vertex_bytes) ||
                !add_resource_budget(index_bytes) || !add_resource_budget(index_bytes)) {
                return fail(NativeD3D12RayTracingContextFailureStage::scene,
                            "native-d3d12.context.resource-budget-exceeded",
                            "Persistent vertex/index resources exceed the configured D3D12 context budget.");
            }
            HRESULT hr = create_committed_buffer(
                impl_->device.Get(), vertex_bytes, D3D12_HEAP_TYPE_DEFAULT,
                D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_FLAG_NONE,
                resources.vertex_buffer);
            if (FAILED(hr) || !resources.vertex_buffer) {
                return fail(NativeD3D12RayTracingContextFailureStage::scene,
                            "native-d3d12.context.vertex-buffer-create-failed",
                            "Persistent default-heap vertex buffer creation failed with " + hresult_hex(hr) + ".");
            }
            hr = create_committed_buffer(
                impl_->device.Get(), vertex_bytes, D3D12_HEAP_TYPE_UPLOAD,
                D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_FLAG_NONE,
                resources.vertex_upload);
            if (FAILED(hr) || !resources.vertex_upload ||
                !fill_upload_buffer(resources.vertex_upload.Get(),
                                    geometry.position_xyz.data(),
                                    static_cast<std::size_t>(vertex_bytes))) {
                return fail(NativeD3D12RayTracingContextFailureStage::scene,
                            "native-d3d12.context.vertex-upload-failed",
                            "Persistent upload-heap vertex initialization failed with " + hresult_hex(hr) + ".");
            }
            hr = create_committed_buffer(
                impl_->device.Get(), index_bytes, D3D12_HEAP_TYPE_DEFAULT,
                D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_FLAG_NONE,
                resources.index_buffer);
            if (FAILED(hr) || !resources.index_buffer) {
                return fail(NativeD3D12RayTracingContextFailureStage::scene,
                            "native-d3d12.context.index-buffer-create-failed",
                            "Persistent default-heap index buffer creation failed with " + hresult_hex(hr) + ".");
            }
            hr = create_committed_buffer(
                impl_->device.Get(), index_bytes, D3D12_HEAP_TYPE_UPLOAD,
                D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_FLAG_NONE,
                resources.index_upload);
            if (FAILED(hr) || !resources.index_upload ||
                !fill_upload_buffer(resources.index_upload.Get(),
                                    geometry.indices.data(),
                                    static_cast<std::size_t>(index_bytes))) {
                return fail(NativeD3D12RayTracingContextFailureStage::scene,
                            "native-d3d12.context.index-upload-failed",
                            "Persistent upload-heap index initialization failed with " + hresult_hex(hr) + ".");
            }

            D3D12_RAYTRACING_GEOMETRY_DESC geometry_desc{};
            geometry_desc.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
            geometry_desc.Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
            geometry_desc.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
            geometry_desc.Triangles.VertexCount = resources.vertex_count;
            geometry_desc.Triangles.VertexBuffer.StartAddress =
                resources.vertex_buffer->GetGPUVirtualAddress();
            geometry_desc.Triangles.VertexBuffer.StrideInBytes = sizeof(float) * 3U;
            geometry_desc.Triangles.IndexFormat = DXGI_FORMAT_R32_UINT;
            geometry_desc.Triangles.IndexCount = resources.index_count;
            geometry_desc.Triangles.IndexBuffer = resources.index_buffer->GetGPUVirtualAddress();

            D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS blas_inputs{};
            blas_inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
            blas_inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
            blas_inputs.NumDescs = 1U;
            blas_inputs.pGeometryDescs = &geometry_desc;
            blas_inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
            if (resources.allow_update)
                blas_inputs.Flags |= D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_UPDATE;
            D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO blas_info{};
            impl_->device->GetRaytracingAccelerationStructurePrebuildInfo(
                &blas_inputs, &blas_info);
            if (!resource_bytes_bounded(blas_info.ResultDataMaxSizeInBytes) ||
                !resource_bytes_bounded(blas_info.ScratchDataSizeInBytes) ||
                !add_resource_budget(blas_info.ResultDataMaxSizeInBytes) ||
                !add_resource_budget(blas_info.ScratchDataSizeInBytes)) {
                return fail(NativeD3D12RayTracingContextFailureStage::blas,
                            "native-d3d12.context.blas-prebuild-invalid",
                            "D3D12 returned an empty, unbounded or over-budget BLAS prebuild size.");
            }
            resources.blas_result_bytes = blas_info.ResultDataMaxSizeInBytes;
            resources.blas_scratch_bytes = blas_info.ScratchDataSizeInBytes;
            hr = create_committed_buffer(
                impl_->device.Get(), resources.blas_result_bytes,
                D3D12_HEAP_TYPE_DEFAULT,
                D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE,
                D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                resources.blas_result);
            if (FAILED(hr) || !resources.blas_result) {
                return fail(NativeD3D12RayTracingContextFailureStage::blas,
                            "native-d3d12.context.blas-result-create-failed",
                            "Persistent BLAS result resource creation failed with " + hresult_hex(hr) + ".");
            }
            hr = create_committed_buffer(
                impl_->device.Get(), resources.blas_scratch_bytes,
                D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                resources.blas_scratch);
            if (FAILED(hr) || !resources.blas_scratch) {
                return fail(NativeD3D12RayTracingContextFailureStage::blas,
                            "native-d3d12.context.blas-scratch-create-failed",
                            "Persistent BLAS scratch resource creation failed with " + hresult_hex(hr) + ".");
            }
            next_geometry_resources.push_back(std::move(resources));
        }
    }

    auto& geometry_resources = rebuild ? next_geometry_resources : impl_->geometry_resources;
    std::vector<D3D12_RAYTRACING_GEOMETRY_DESC> geometry_descriptors;
    geometry_descriptors.reserve(geometry_resources.size());
    for (const auto& resources : geometry_resources) {
        D3D12_RAYTRACING_GEOMETRY_DESC geometry_desc{};
        geometry_desc.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
        geometry_desc.Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
        geometry_desc.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
        geometry_desc.Triangles.VertexCount = resources.vertex_count;
        geometry_desc.Triangles.VertexBuffer.StartAddress =
            resources.vertex_buffer->GetGPUVirtualAddress();
        geometry_desc.Triangles.VertexBuffer.StrideInBytes = sizeof(float) * 3U;
        geometry_desc.Triangles.IndexFormat = DXGI_FORMAT_R32_UINT;
        geometry_desc.Triangles.IndexCount = resources.index_count;
        geometry_desc.Triangles.IndexBuffer = resources.index_buffer->GetGPUVirtualAddress();
        geometry_descriptors.push_back(geometry_desc);
    }

    std::vector<D3D12_RAYTRACING_INSTANCE_DESC> instance_descriptors;
    instance_descriptors.resize(geometry_resources.size());
    for (std::size_t index = 0U; index < geometry_resources.size(); ++index) {
        auto& instance = instance_descriptors[index];
        instance.Transform[0][0] = 1.0F;
        instance.Transform[1][1] = 1.0F;
        instance.Transform[2][2] = 1.0F;
        instance.InstanceID = static_cast<UINT>(index);
        instance.InstanceMask = 0xffU;
        instance.AccelerationStructure = geometry_resources[index].blas_result->GetGPUVirtualAddress();
    }
    const auto instance_bytes = static_cast<std::uint64_t>(instance_descriptors.size()) *
        sizeof(D3D12_RAYTRACING_INSTANCE_DESC);
    if (!resource_bytes_bounded(instance_bytes)) {
        return fail(NativeD3D12RayTracingContextFailureStage::tlas,
                    "native-d3d12.context.instance-buffer-size-invalid",
                    "The persistent TLAS instance descriptor buffer exceeded the bounded resource contract.");
    }
    if (rebuild) {
        if (!add_resource_budget(instance_bytes)) {
            return fail(NativeD3D12RayTracingContextFailureStage::tlas,
                        "native-d3d12.context.resource-budget-exceeded",
                        "Persistent TLAS instance descriptors exceed the configured D3D12 context budget.");
        }
        HRESULT hr = create_committed_buffer(
            impl_->device.Get(), instance_bytes, D3D12_HEAP_TYPE_UPLOAD,
            D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_FLAG_NONE,
            next_instance_buffer);
        if (FAILED(hr) || !next_instance_buffer ||
            !fill_upload_buffer(next_instance_buffer.Get(), instance_descriptors.data(),
                                static_cast<std::size_t>(instance_bytes))) {
            return fail(NativeD3D12RayTracingContextFailureStage::tlas,
                        "native-d3d12.context.instance-buffer-failed",
                        "Persistent TLAS instance descriptor upload failed with " + hresult_hex(hr) + ".");
        }
    } else if (!fill_upload_buffer(impl_->instance_buffer.Get(), instance_descriptors.data(),
                                   static_cast<std::size_t>(instance_bytes))) {
        return fail(NativeD3D12RayTracingContextFailureStage::tlas,
                    "native-d3d12.context.instance-buffer-update-failed",
                    "Updating the persistent TLAS instance descriptors failed.");
    }
    ID3D12Resource* instance_buffer = rebuild ? next_instance_buffer.Get() : impl_->instance_buffer.Get();

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS tlas_inputs{};
    tlas_inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
    tlas_inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    tlas_inputs.NumDescs = static_cast<UINT>(instance_descriptors.size());
    tlas_inputs.InstanceDescs = instance_buffer->GetGPUVirtualAddress();
    tlas_inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
    if (impl_->scene->allow_update)
        tlas_inputs.Flags |= D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_UPDATE;

    if (rebuild) {
        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO tlas_info{};
        impl_->device->GetRaytracingAccelerationStructurePrebuildInfo(
            &tlas_inputs, &tlas_info);
        if (!resource_bytes_bounded(tlas_info.ResultDataMaxSizeInBytes) ||
            !resource_bytes_bounded(tlas_info.ScratchDataSizeInBytes) ||
            !add_resource_budget(tlas_info.ResultDataMaxSizeInBytes) ||
            !add_resource_budget(tlas_info.ScratchDataSizeInBytes)) {
            return fail(NativeD3D12RayTracingContextFailureStage::tlas,
                        "native-d3d12.context.tlas-prebuild-invalid",
                        "D3D12 returned an empty, unbounded or over-budget TLAS prebuild size.");
        }
        next_tlas_result_bytes = tlas_info.ResultDataMaxSizeInBytes;
        next_tlas_scratch_bytes = tlas_info.ScratchDataSizeInBytes;
        HRESULT hr = create_committed_buffer(
            impl_->device.Get(), next_tlas_result_bytes, D3D12_HEAP_TYPE_DEFAULT,
            D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE,
            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, next_tlas_result);
        if (FAILED(hr) || !next_tlas_result) {
            return fail(NativeD3D12RayTracingContextFailureStage::tlas,
                        "native-d3d12.context.tlas-result-create-failed",
                        "Persistent TLAS result resource creation failed with " + hresult_hex(hr) + ".");
        }
        hr = create_committed_buffer(
            impl_->device.Get(), next_tlas_scratch_bytes, D3D12_HEAP_TYPE_DEFAULT,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, next_tlas_scratch);
        if (FAILED(hr) || !next_tlas_scratch) {
            return fail(NativeD3D12RayTracingContextFailureStage::tlas,
                        "native-d3d12.context.tlas-scratch-create-failed",
                        "Persistent TLAS scratch resource creation failed with " + hresult_hex(hr) + ".");
        }
    }
    ID3D12Resource* tlas_result = rebuild ? next_tlas_result.Get() : impl_->tlas_result.Get();
    ID3D12Resource* tlas_scratch = rebuild ? next_tlas_scratch.Get() : impl_->tlas_scratch.Get();
    if (tlas_result == nullptr || tlas_scratch == nullptr || instance_buffer == nullptr) {
        return fail(NativeD3D12RayTracingContextFailureStage::tlas,
                    "native-d3d12.context.tlas-resources-unavailable",
                    "Persistent TLAS resources were not available for command recording.");
    }

    HRESULT hr = impl_->command_allocator->Reset();
    if (FAILED(hr)) {
        return fail(NativeD3D12RayTracingContextFailureStage::command_allocator,
                    "native-d3d12.context.command-allocator-reset-failed",
                    "Resetting the persistent command allocator failed with " + hresult_hex(hr) + ".");
    }
    hr = impl_->command_list->Reset(impl_->command_allocator.Get(), nullptr);
    if (FAILED(hr)) {
        return fail(NativeD3D12RayTracingContextFailureStage::command_list,
                    "native-d3d12.context.command-list-reset-failed",
                    "Resetting the persistent command list failed with " + hresult_hex(hr) + ".");
    }

    for (std::size_t index = 0U; index < geometry_resources.size(); ++index) {
        auto& resources = geometry_resources[index];
        const auto& geometry = impl_->scene->geometries[index];
        if (rebuild) {
            impl_->command_list->CopyBufferRegion(resources.vertex_buffer.Get(), 0U,
                                                  resources.vertex_upload.Get(), 0U,
                                                  resources.vertex_bytes);
            transition_resource(impl_->command_list.Get(), resources.vertex_buffer.Get(),
                                D3D12_RESOURCE_STATE_COPY_DEST,
                                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            impl_->command_list->CopyBufferRegion(resources.index_buffer.Get(), 0U,
                                                  resources.index_upload.Get(), 0U,
                                                  resources.index_bytes);
            transition_resource(impl_->command_list.Get(), resources.index_buffer.Get(),
                                D3D12_RESOURCE_STATE_COPY_DEST,
                                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        } else {
            if (!fill_upload_buffer(resources.vertex_upload.Get(), geometry.position_xyz.data(),
                                    static_cast<std::size_t>(resources.vertex_bytes)) ||
                !fill_upload_buffer(resources.index_upload.Get(), geometry.indices.data(),
                                    static_cast<std::size_t>(resources.index_bytes))) {
                static_cast<void>(impl_->command_list->Close());
                return fail(NativeD3D12RayTracingContextFailureStage::scene,
                            "native-d3d12.context.geometry-update-upload-failed",
                            "Updating persistent vertex or index upload data failed.");
            }
            transition_resource(impl_->command_list.Get(), resources.vertex_buffer.Get(),
                                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                D3D12_RESOURCE_STATE_COPY_DEST);
            impl_->command_list->CopyBufferRegion(resources.vertex_buffer.Get(), 0U,
                                                  resources.vertex_upload.Get(), 0U,
                                                  resources.vertex_bytes);
            transition_resource(impl_->command_list.Get(), resources.vertex_buffer.Get(),
                                D3D12_RESOURCE_STATE_COPY_DEST,
                                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            transition_resource(impl_->command_list.Get(), resources.index_buffer.Get(),
                                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                D3D12_RESOURCE_STATE_COPY_DEST);
            impl_->command_list->CopyBufferRegion(resources.index_buffer.Get(), 0U,
                                                  resources.index_upload.Get(), 0U,
                                                  resources.index_bytes);
            transition_resource(impl_->command_list.Get(), resources.index_buffer.Get(),
                                D3D12_RESOURCE_STATE_COPY_DEST,
                                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        }
        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS blas_inputs{};
        blas_inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
        blas_inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
        blas_inputs.NumDescs = 1U;
        blas_inputs.pGeometryDescs = &geometry_descriptors[index];
        blas_inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
        if (resources.allow_update)
            blas_inputs.Flags |= D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_UPDATE;
        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC blas_build{};
        blas_build.Inputs = blas_inputs;
        blas_build.ScratchAccelerationStructureData = resources.blas_scratch->GetGPUVirtualAddress();
        blas_build.DestAccelerationStructureData = resources.blas_result->GetGPUVirtualAddress();
        if (!rebuild) {
            blas_build.Inputs.Flags |= D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PERFORM_UPDATE;
            blas_build.SourceAccelerationStructureData = resources.blas_result->GetGPUVirtualAddress();
        }
        impl_->command_list->BuildRaytracingAccelerationStructure(&blas_build, 0U, nullptr);
        uav_barrier(impl_->command_list.Get(), resources.blas_result.Get());
    }

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC tlas_build{};
    tlas_build.Inputs = tlas_inputs;
    tlas_build.ScratchAccelerationStructureData = tlas_scratch->GetGPUVirtualAddress();
    tlas_build.DestAccelerationStructureData = tlas_result->GetGPUVirtualAddress();
    if (!rebuild) {
        tlas_build.Inputs.Flags |= D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PERFORM_UPDATE;
        tlas_build.SourceAccelerationStructureData = tlas_result->GetGPUVirtualAddress();
    }
    impl_->command_list->BuildRaytracingAccelerationStructure(&tlas_build, 0U, nullptr);
    hr = impl_->command_list->Close();
    if (FAILED(hr)) {
        return fail(NativeD3D12RayTracingContextFailureStage::command_list,
                    "native-d3d12.context.command-list-close-failed",
                    "Closing the persistent AS build command list failed with " + hresult_hex(hr) + ".");
    }
    impl_->last_build_submitted = true;
    impl_->last_update_submitted = !rebuild;
    hr = submit_and_wait(impl_->command_queue.Get(), impl_->fence.Get(),
                         impl_->command_list.Get(), impl_->fence_event,
                         impl_->next_fence_value);
    if (FAILED(hr)) {
        return fail(NativeD3D12RayTracingContextFailureStage::synchronization,
                    "native-d3d12.context.as-build-wait-failed",
                    "Waiting for the persistent BLAS/TLAS build failed with " + hresult_hex(hr) + ".");
    }

    if (rebuild) {
        impl_->geometry_resources = std::move(next_geometry_resources);
        impl_->instance_buffer = std::move(next_instance_buffer);
        impl_->tlas_result = std::move(next_tlas_result);
        impl_->tlas_scratch = std::move(next_tlas_scratch);
        impl_->instance_buffer_bytes = instance_bytes;
        impl_->tlas_result_bytes = next_tlas_result_bytes;
        impl_->tlas_scratch_bytes = next_tlas_scratch_bytes;
        if (impl_->resource_generation != std::numeric_limits<std::uint64_t>::max())
            ++impl_->resource_generation;
    }
    impl_->blas_ready = true;
    impl_->tlas_ready = true;
    impl_->last_build_completed = true;
    impl_->last_update_completed = !rebuild;
    impl_->last_synchronization_completed = true;
    impl_->built_scene_hash = impl_->scene_hash;
    impl_->built_topology_hash = topology_hash;
    impl_->scene_dirty = false;
    save_result(*impl_, NativeD3D12RayTracingContextFailureStage::none,
                rebuild ? "native-d3d12.context.as-build-complete"
                        : "native-d3d12.context.as-update-complete",
                rebuild
                    ? "Persistent vertex/index uploads, BLAS result/scratch, TLAS result/scratch and instance descriptors were built and synchronized on the retained D3D12 queue."
                    : "Persistent vertex/index uploads were refreshed and BLAS/TLAS update builds were synchronized on the retained D3D12 queue.");
    auto result = receipt_from(*impl_, "build-or-update");
    result.scene_received = true;
    result.state = NativeD3D12RayTracingContextState::ready;
    return result;
#endif
}

NativeD3D12RayTracingContextReceipt NativeD3D12RayTracingContext::trace() {
    if (impl_->state == NativeD3D12RayTracingContextState::uninitialized)
        static_cast<void>(initialize());
    auto result = receipt_from(*impl_, "trace");
    if (!impl_->scene) {
        save_result(*impl_, NativeD3D12RayTracingContextFailureStage::scene,
                    "native-d3d12.context.trace-scene-not-ready",
                    "Trace requires a retained scene and a completed AS build/update.");
        result = receipt_from(*impl_, "trace");
        result.state = NativeD3D12RayTracingContextState::failed;
        return result;
    }
    mark_unsupported(*impl_, NativeD3D12RayTracingContextFailureStage::shader_pipeline,
                     "trace",
                     "native-d3d12.context.trace-pipeline-unavailable",
                     impl_->options.shaders.complete()
                         ? "Shader bytecode was supplied, but pipeline/SBT creation is not yet enabled in this persistent context."
                         : "No complete RayGen/Miss/ClosestHit shader contract was supplied; TraceRays is intentionally unsupported.",
                     result);
    return result;
}

NativeD3D12RayTracingContextReceipt NativeD3D12RayTracingContext::readback() {
    if (impl_->state == NativeD3D12RayTracingContextState::uninitialized)
        static_cast<void>(initialize());
    auto result = receipt_from(*impl_, "readback");
    save_result(*impl_, NativeD3D12RayTracingContextFailureStage::readback,
                "native-d3d12.context.readback-not-ready",
                "Readback is gated on a completed trace dispatch; no output is reported ready before that proof.");
    result = receipt_from(*impl_, "readback");
    result.state = impl_->state == NativeD3D12RayTracingContextState::ready
        ? NativeD3D12RayTracingContextState::failed : impl_->state;
    result.fallback_active = impl_->state == NativeD3D12RayTracingContextState::unsupported;
    return result;
}

NativeD3D12RayTracingContextReceipt NativeD3D12RayTracingContext::shutdown() {
    if (impl_->state == NativeD3D12RayTracingContextState::shutdown) {
        save_result(*impl_, NativeD3D12RayTracingContextFailureStage::cleanup,
                    "native-d3d12.context.shutdown-idempotent",
                    "The persistent D3D12 context was already shut down; cleanup is complete.");
        auto result = receipt_from(*impl_, "shutdown");
        result.state = NativeD3D12RayTracingContextState::shutdown;
        result.shutdown_completed = true;
        return result;
    }
#if defined(_WIN32)
    // Every build/update is synchronously fenced before returning.  Keep the
    // same invariant here so resource destruction remains safe if a future
    // operation adds another submission path.
    if (impl_->fence != nullptr && impl_->next_fence_value > 1U &&
        impl_->fence->GetCompletedValue() < impl_->next_fence_value - 1U &&
        impl_->fence_event != nullptr) {
        if (SUCCEEDED(impl_->fence->SetEventOnCompletion(
                impl_->next_fence_value - 1U, impl_->fence_event)))
            static_cast<void>(WaitForSingleObject(impl_->fence_event, INFINITE));
    }
    impl_->geometry_resources.clear();
    impl_->instance_buffer.Reset();
    impl_->tlas_result.Reset();
    impl_->tlas_scratch.Reset();
    impl_->instance_buffer_bytes = 0U;
    impl_->tlas_result_bytes = 0U;
    impl_->tlas_scratch_bytes = 0U;
    impl_->built_scene_hash = 0U;
    impl_->built_topology_hash = 0U;
    impl_->blas_ready = false;
    impl_->tlas_ready = false;
    impl_->last_build_submitted = false;
    impl_->last_build_completed = false;
    impl_->last_update_submitted = false;
    impl_->last_update_completed = false;
    impl_->last_synchronization_completed = false;
    impl_->command_list.Reset();
    impl_->command_allocator.Reset();
    impl_->command_queue.Reset();
    impl_->fence.Reset();
    impl_->device.Reset();
    impl_->factory.Reset();
    if (impl_->fence_event != nullptr) {
        CloseHandle(impl_->fence_event);
        impl_->fence_event = nullptr;
    }
#endif
    impl_->state = NativeD3D12RayTracingContextState::shutdown;
    impl_->scene.reset();
    impl_->scene_dirty = false;
    save_result(*impl_, NativeD3D12RayTracingContextFailureStage::cleanup,
                "native-d3d12.context.shutdown-complete",
                "Persistent D3D12 handles and cached scene state were released exactly once.");
    auto result = receipt_from(*impl_, "shutdown");
    result.state = NativeD3D12RayTracingContextState::shutdown;
    result.scene_received = false;
    result.shutdown_completed = true;
    result.fallback_active = false;
    return result;
}

NativeD3D12RayTracingContextReceipt NativeD3D12RayTracingContext::status() const {
    return receipt_from(*impl_, "status");
}

NativeD3D12RayTracingContextState NativeD3D12RayTracingContext::state() const noexcept {
    return impl_->state;
}

std::uint64_t NativeD3D12RayTracingContext::generation() const noexcept {
    return impl_->generation;
}

std::uint64_t NativeD3D12RayTracingContext::scene_generation() const noexcept {
    return impl_->scene_generation;
}

bool NativeD3D12RayTracingContext::is_shutdown() const noexcept {
    return impl_->state == NativeD3D12RayTracingContextState::shutdown;
}

} // namespace noemancer
