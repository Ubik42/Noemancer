#include "runtime/native_d3d12_raytracing_executor.hpp"

#include <algorithm>
#include <array>
#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
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
    return std::string(value.substr(0U,
                                    native_d3d12_raytracing_executor_max_text_bytes));
}

void set_receipt(NativeD3D12RayTracingReceipt& receipt,
                 const NativeD3D12RayTracingExecutionState state,
                 const NativeD3D12RayTracingFailureStage stage,
                 const std::string_view code, const std::string_view detail) {
    receipt.state = state;
    receipt.failure_stage = stage;
    receipt.code = bounded_text(code);
    receipt.detail = bounded_text(detail);
}

#if defined(_WIN32)

using Microsoft::WRL::ComPtr;

std::string utf8_from_wide(const wchar_t* value) {
    if (value == nullptr || value[0] == L'\0') return {};
    const int required = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
                                             value, -1, nullptr, 0, nullptr, nullptr);
    if (required <= 1) return {};
    std::string result(static_cast<std::size_t>(required - 1), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value, -1,
                            result.data(), required, nullptr, nullptr) <= 0)
        return {};
    return result;
}

std::string hresult_hex(const HRESULT value) {
    char buffer[32]{};
    std::snprintf(buffer, sizeof(buffer), "0x%08" PRIx32,
                  static_cast<std::uint32_t>(value));
    return buffer;
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

using CreateFactoryFn = HRESULT(WINAPI*)(UINT, REFIID, void**);
using CreateDeviceFn = HRESULT(WINAPI*)(IUnknown*, D3D_FEATURE_LEVEL, REFIID, void**);

struct HardwareDeviceSelection final {
    ComPtr<ID3D12Device5> device;
    std::string name;
    D3D12_RAYTRACING_TIER tier{D3D12_RAYTRACING_TIER_NOT_SUPPORTED};
};

bool resource_bytes_bounded(const std::uint64_t bytes) noexcept {
    return bytes > 0U && bytes <= native_d3d12_raytracing_executor_max_resource_bytes;
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
    description.Alignment = 0U;
    description.Width = bytes;
    description.Height = 1U;
    description.DepthOrArraySize = 1U;
    description.MipLevels = 1U;
    description.Format = DXGI_FORMAT_UNKNOWN;
    description.SampleDesc.Count = 1U;
    description.SampleDesc.Quality = 0U;
    description.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    description.Flags = flags;

    return device->CreateCommittedResource(
        &heap, D3D12_HEAP_FLAG_NONE, &description, state, nullptr,
        IID_PPV_ARGS(&resource));
}

bool choose_hardware_device(IDXGIFactory4* factory, CreateDeviceFn create_device,
                            HardwareDeviceSelection& selection,
                            NativeD3D12RayTracingReceipt& receipt) {
    if (factory == nullptr || create_device == nullptr) return false;

    for (UINT index = 0U;; ++index) {
        ComPtr<IDXGIAdapter1> adapter;
        const HRESULT enum_hr = factory->EnumAdapters1(index, &adapter);
        if (enum_hr == DXGI_ERROR_NOT_FOUND) break;
        if (FAILED(enum_hr) || !adapter) continue;

        DXGI_ADAPTER_DESC1 description{};
        if (FAILED(adapter->GetDesc1(&description))) continue;
        if ((description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0U) continue;
        ++receipt.hardware_adapter_count;

        ComPtr<ID3D12Device> base_device;
        const HRESULT device_hr = create_device(
            adapter.Get(), D3D_FEATURE_LEVEL_12_0, __uuidof(ID3D12Device),
            reinterpret_cast<void**>(base_device.GetAddressOf()));
        if (FAILED(device_hr) || !base_device) continue;
        receipt.hardware_device_created = true;

        D3D12_FEATURE_DATA_D3D12_OPTIONS5 options5{};
        if (FAILED(base_device->CheckFeatureSupport(
                D3D12_FEATURE_D3D12_OPTIONS5, &options5, sizeof(options5))))
            continue;
        if (options5.RaytracingTier == D3D12_RAYTRACING_TIER_NOT_SUPPORTED)
            continue;

        ComPtr<ID3D12Device5> device5;
        if (FAILED(base_device.As(&device5)) || !device5) continue;
        ++receipt.hardware_raytracing_device_count;
        if (!selection.device || options5.RaytracingTier > selection.tier) {
            selection.device = std::move(device5);
            selection.tier = options5.RaytracingTier;
            selection.name = bounded_text(utf8_from_wide(description.Description));
        }
    }
    receipt.hardware_raytracing_device_found =
        receipt.hardware_raytracing_device_count > 0U;
    return receipt.hardware_raytracing_device_found;
}

bool query_warp_raytracing(CreateDeviceFn create_device,
                           NativeD3D12RayTracingReceipt& receipt) {
    receipt.warp_fallback_attempted = true;
    if (create_device == nullptr) return false;
    ComPtr<ID3D12Device> warp_device;
    const HRESULT device_hr = create_device(
        nullptr, D3D_FEATURE_LEVEL_12_0, __uuidof(ID3D12Device),
        reinterpret_cast<void**>(warp_device.GetAddressOf()));
    if (FAILED(device_hr) || !warp_device) return false;
    D3D12_FEATURE_DATA_D3D12_OPTIONS5 options5{};
    if (FAILED(warp_device->CheckFeatureSupport(
            D3D12_FEATURE_D3D12_OPTIONS5, &options5, sizeof(options5))))
        return false;
    receipt.warp_raytracing_supported =
        options5.RaytracingTier != D3D12_RAYTRACING_TIER_NOT_SUPPORTED;
    // WARP is deliberately never accepted as a hardware success.  The value
    // is diagnostic only and remains unsupported even if a future WARP build
    // advertises an experimental tier.
    return receipt.warp_raytracing_supported;
}

bool fill_upload_buffer(ID3D12Resource* resource, const void* data,
                        const std::size_t bytes) {
    if (resource == nullptr || data == nullptr || bytes == 0U) return false;
    void* mapped = nullptr;
    const D3D12_RANGE read_range{0U, 0U};
    if (FAILED(resource->Map(0U, &read_range, &mapped)) || mapped == nullptr)
        return false;
    std::memcpy(mapped, data, bytes);
    resource->Unmap(0U, nullptr);
    return true;
}

struct TriangleVertex final {
    float x;
    float y;
    float z;
};

NativeD3D12RayTracingReceipt execute_hardware(
    const HardwareDeviceSelection& selection,
    const NativeD3D12RayTracingReceipt& probe_receipt) {
    NativeD3D12RayTracingReceipt receipt = probe_receipt;
    receipt.hardware_probe_completed = true;
    receipt.hardware_device_created = selection.device != nullptr;
    receipt.hardware_raytracing_device_found = selection.device != nullptr;
    receipt.raytracing_tier = static_cast<std::uint32_t>(selection.tier);
    receipt.device_name = bounded_text(selection.name);
    receipt.native_handle_exposed = false;

    if (!selection.device) {
        set_receipt(receipt, NativeD3D12RayTracingExecutionState::unsupported,
                    NativeD3D12RayTracingFailureStage::device,
                    "native-d3d12-raytracing.no-hardware-device",
                    "No hardware D3D12 device with a ray-tracing tier was selected.");
        return receipt;
    }
    if (selection.tier < D3D12_RAYTRACING_TIER_1_0) {
        set_receipt(receipt, NativeD3D12RayTracingExecutionState::unsupported,
                    NativeD3D12RayTracingFailureStage::feature,
                    "native-d3d12-raytracing.tier-unsupported",
                    "The selected hardware device did not expose D3D12 ray-tracing tier 1.0.");
        return receipt;
    }

    ComPtr<ID3D12CommandQueue> queue;
    D3D12_COMMAND_QUEUE_DESC queue_description{};
    queue_description.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    HRESULT hr = selection.device->CreateCommandQueue(
        &queue_description, IID_PPV_ARGS(&queue));
    if (FAILED(hr) || !queue) {
        set_receipt(receipt, NativeD3D12RayTracingExecutionState::failed,
                    NativeD3D12RayTracingFailureStage::command_queue,
                    "native-d3d12-raytracing.command-queue-create-failed",
                    "CreateCommandQueue failed with " + hresult_hex(hr) + ".");
        return receipt;
    }

    ComPtr<ID3D12CommandAllocator> allocator;
    hr = selection.device->CreateCommandAllocator(
        D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator));
    if (FAILED(hr) || !allocator) {
        set_receipt(receipt, NativeD3D12RayTracingExecutionState::failed,
                    NativeD3D12RayTracingFailureStage::command_allocator,
                    "native-d3d12-raytracing.command-allocator-create-failed",
                    "CreateCommandAllocator failed with " + hresult_hex(hr) + ".");
        return receipt;
    }

    ComPtr<ID3D12GraphicsCommandList> base_command_list;
    hr = selection.device->CreateCommandList(
        0U, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
        IID_PPV_ARGS(&base_command_list));
    if (FAILED(hr) || !base_command_list) {
        set_receipt(receipt, NativeD3D12RayTracingExecutionState::failed,
                    NativeD3D12RayTracingFailureStage::command_list,
                    "native-d3d12-raytracing.command-list-create-failed",
                    "CreateCommandList failed with " + hresult_hex(hr) + ".");
        return receipt;
    }
    ComPtr<ID3D12GraphicsCommandList4> command_list;
    hr = base_command_list.As(&command_list);
    if (FAILED(hr) || !command_list) {
        set_receipt(receipt, NativeD3D12RayTracingExecutionState::failed,
                    NativeD3D12RayTracingFailureStage::interface_query,
                    "native-d3d12-raytracing.command-list4-unavailable",
                    "The hardware device did not expose ID3D12GraphicsCommandList4.");
        return receipt;
    }

    constexpr std::array<TriangleVertex, 3U> triangle{
        TriangleVertex{-0.75F, -0.75F, 0.0F},
        TriangleVertex{0.0F, 0.75F, 0.0F},
        TriangleVertex{0.75F, -0.75F, 0.0F},
    };
    receipt.vertex_buffer_bytes = sizeof(triangle);

    ComPtr<ID3D12Resource> vertex_buffer;
    ComPtr<ID3D12Resource> vertex_upload;
    hr = create_committed_buffer(
        selection.device.Get(), receipt.vertex_buffer_bytes, D3D12_HEAP_TYPE_DEFAULT,
        D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_FLAG_NONE, vertex_buffer);
    if (FAILED(hr) || !vertex_buffer) {
        set_receipt(receipt, NativeD3D12RayTracingExecutionState::failed,
                    NativeD3D12RayTracingFailureStage::vertex_buffer,
                    "native-d3d12-raytracing.vertex-buffer-create-failed",
                    "Default-heap vertex buffer creation failed with " + hresult_hex(hr) + ".");
        return receipt;
    }
    hr = create_committed_buffer(
        selection.device.Get(), receipt.vertex_buffer_bytes, D3D12_HEAP_TYPE_UPLOAD,
        D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_FLAG_NONE, vertex_upload);
    if (FAILED(hr) || !vertex_upload ||
        !fill_upload_buffer(vertex_upload.Get(), triangle.data(), sizeof(triangle))) {
        set_receipt(receipt, NativeD3D12RayTracingExecutionState::failed,
                    NativeD3D12RayTracingFailureStage::vertex_buffer,
                    "native-d3d12-raytracing.vertex-upload-failed",
                    "Upload-heap vertex initialization failed with " + hresult_hex(hr) + ".");
        return receipt;
    }
    command_list->CopyBufferRegion(vertex_buffer.Get(), 0U, vertex_upload.Get(),
                                   0U, receipt.vertex_buffer_bytes);
    D3D12_RESOURCE_BARRIER vertex_barrier{};
    vertex_barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    vertex_barrier.Transition.pResource = vertex_buffer.Get();
    vertex_barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    vertex_barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    vertex_barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    command_list->ResourceBarrier(1U, &vertex_barrier);

    D3D12_RAYTRACING_GEOMETRY_DESC geometry{};
    geometry.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
    geometry.Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
    geometry.Triangles.VertexCount = 3U;
    geometry.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
    geometry.Triangles.VertexBuffer.StartAddress = vertex_buffer->GetGPUVirtualAddress();
    geometry.Triangles.VertexBuffer.StrideInBytes = sizeof(TriangleVertex);

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS blas_inputs{};
    blas_inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
    blas_inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    blas_inputs.NumDescs = 1U;
    blas_inputs.pGeometryDescs = &geometry;
    blas_inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;

    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO blas_info{};
    selection.device->GetRaytracingAccelerationStructurePrebuildInfo(
        &blas_inputs, &blas_info);
    receipt.blas_prebuild_completed =
        resource_bytes_bounded(blas_info.ResultDataMaxSizeInBytes) &&
        resource_bytes_bounded(blas_info.ScratchDataSizeInBytes);
    receipt.blas_result_bytes = blas_info.ResultDataMaxSizeInBytes;
    receipt.blas_scratch_bytes = blas_info.ScratchDataSizeInBytes;
    if (!receipt.blas_prebuild_completed) {
        set_receipt(receipt, NativeD3D12RayTracingExecutionState::failed,
                    NativeD3D12RayTracingFailureStage::blas_prebuild,
                    "native-d3d12-raytracing.blas-prebuild-invalid",
                    "D3D12 returned an empty or unbounded BLAS prebuild size.");
        return receipt;
    }

    ComPtr<ID3D12Resource> blas_result;
    ComPtr<ID3D12Resource> blas_scratch;
    hr = create_committed_buffer(
        selection.device.Get(), blas_info.ResultDataMaxSizeInBytes,
        D3D12_HEAP_TYPE_DEFAULT,
        D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE,
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, blas_result);
    if (FAILED(hr) || !blas_result) {
        set_receipt(receipt, NativeD3D12RayTracingExecutionState::failed,
                    NativeD3D12RayTracingFailureStage::blas_resources,
                    "native-d3d12-raytracing.blas-result-create-failed",
                    "BLAS result resource creation failed with " + hresult_hex(hr) + ".");
        return receipt;
    }
    hr = create_committed_buffer(
        selection.device.Get(), blas_info.ScratchDataSizeInBytes,
        D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, blas_scratch);
    if (FAILED(hr) || !blas_scratch) {
        set_receipt(receipt, NativeD3D12RayTracingExecutionState::failed,
                    NativeD3D12RayTracingFailureStage::blas_resources,
                    "native-d3d12-raytracing.blas-scratch-create-failed",
                    "BLAS scratch resource creation failed with " + hresult_hex(hr) + ".");
        return receipt;
    }

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC blas_build{};
    blas_build.Inputs = blas_inputs;
    blas_build.ScratchAccelerationStructureData = blas_scratch->GetGPUVirtualAddress();
    blas_build.DestAccelerationStructureData = blas_result->GetGPUVirtualAddress();
    command_list->BuildRaytracingAccelerationStructure(&blas_build, 0U, nullptr);
    receipt.blas_build_submitted = true;
    D3D12_RESOURCE_BARRIER blas_barrier{};
    blas_barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    blas_barrier.UAV.pResource = blas_result.Get();
    command_list->ResourceBarrier(1U, &blas_barrier);

    D3D12_RAYTRACING_INSTANCE_DESC instance_desc{};
    instance_desc.Transform[0][0] = 1.0F;
    instance_desc.Transform[1][1] = 1.0F;
    instance_desc.Transform[2][2] = 1.0F;
    instance_desc.InstanceMask = 0xffU;
    instance_desc.AccelerationStructure = blas_result->GetGPUVirtualAddress();

    ComPtr<ID3D12Resource> instance_buffer;
    hr = create_committed_buffer(
        selection.device.Get(), sizeof(instance_desc), D3D12_HEAP_TYPE_UPLOAD,
        D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_FLAG_NONE, instance_buffer);
    if (FAILED(hr) || !instance_buffer ||
        !fill_upload_buffer(instance_buffer.Get(), &instance_desc,
                            sizeof(instance_desc))) {
        set_receipt(receipt, NativeD3D12RayTracingExecutionState::failed,
                    NativeD3D12RayTracingFailureStage::tlas_resources,
                    "native-d3d12-raytracing.instance-buffer-failed",
                    "TLAS instance descriptor upload failed with " + hresult_hex(hr) + ".");
        return receipt;
    }

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS tlas_inputs{};
    tlas_inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
    tlas_inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    tlas_inputs.NumDescs = 1U;
    tlas_inputs.InstanceDescs = instance_buffer->GetGPUVirtualAddress();
    tlas_inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;

    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO tlas_info{};
    selection.device->GetRaytracingAccelerationStructurePrebuildInfo(
        &tlas_inputs, &tlas_info);
    receipt.tlas_prebuild_completed =
        resource_bytes_bounded(tlas_info.ResultDataMaxSizeInBytes) &&
        resource_bytes_bounded(tlas_info.ScratchDataSizeInBytes);
    receipt.tlas_result_bytes = tlas_info.ResultDataMaxSizeInBytes;
    receipt.tlas_scratch_bytes = tlas_info.ScratchDataSizeInBytes;
    if (!receipt.tlas_prebuild_completed) {
        set_receipt(receipt, NativeD3D12RayTracingExecutionState::failed,
                    NativeD3D12RayTracingFailureStage::tlas_prebuild,
                    "native-d3d12-raytracing.tlas-prebuild-invalid",
                    "D3D12 returned an empty or unbounded TLAS prebuild size.");
        return receipt;
    }

    ComPtr<ID3D12Resource> tlas_result;
    ComPtr<ID3D12Resource> tlas_scratch;
    hr = create_committed_buffer(
        selection.device.Get(), tlas_info.ResultDataMaxSizeInBytes,
        D3D12_HEAP_TYPE_DEFAULT,
        D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE,
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, tlas_result);
    if (FAILED(hr) || !tlas_result) {
        set_receipt(receipt, NativeD3D12RayTracingExecutionState::failed,
                    NativeD3D12RayTracingFailureStage::tlas_resources,
                    "native-d3d12-raytracing.tlas-result-create-failed",
                    "TLAS result resource creation failed with " + hresult_hex(hr) + ".");
        return receipt;
    }
    hr = create_committed_buffer(
        selection.device.Get(), tlas_info.ScratchDataSizeInBytes,
        D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, tlas_scratch);
    if (FAILED(hr) || !tlas_scratch) {
        set_receipt(receipt, NativeD3D12RayTracingExecutionState::failed,
                    NativeD3D12RayTracingFailureStage::tlas_resources,
                    "native-d3d12-raytracing.tlas-scratch-create-failed",
                    "TLAS scratch resource creation failed with " + hresult_hex(hr) + ".");
        return receipt;
    }

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC tlas_build{};
    tlas_build.Inputs = tlas_inputs;
    tlas_build.ScratchAccelerationStructureData = tlas_scratch->GetGPUVirtualAddress();
    tlas_build.DestAccelerationStructureData = tlas_result->GetGPUVirtualAddress();
    command_list->BuildRaytracingAccelerationStructure(&tlas_build, 0U, nullptr);
    receipt.tlas_build_submitted = true;

    hr = base_command_list->Close();
    if (FAILED(hr)) {
        set_receipt(receipt, NativeD3D12RayTracingExecutionState::failed,
                    NativeD3D12RayTracingFailureStage::command_list,
                    "native-d3d12-raytracing.command-list-close-failed",
                    "Close failed after BLAS/TLAS recording with " + hresult_hex(hr) + ".");
        return receipt;
    }
    ID3D12CommandList* lists[] = {base_command_list.Get()};
    queue->ExecuteCommandLists(1U, lists);

    ComPtr<ID3D12Fence> fence;
    hr = selection.device->CreateFence(0U, D3D12_FENCE_FLAG_NONE,
                                       IID_PPV_ARGS(&fence));
    if (FAILED(hr) || !fence) {
        set_receipt(receipt, NativeD3D12RayTracingExecutionState::failed,
                    NativeD3D12RayTracingFailureStage::synchronization,
                    "native-d3d12-raytracing.fence-create-failed",
                    "Fence creation failed with " + hresult_hex(hr) + ".");
        return receipt;
    }
    constexpr UINT64 fence_value = 1U;
    hr = queue->Signal(fence.Get(), fence_value);
    if (FAILED(hr)) {
        set_receipt(receipt, NativeD3D12RayTracingExecutionState::failed,
                    NativeD3D12RayTracingFailureStage::synchronization,
                    "native-d3d12-raytracing.fence-signal-failed",
                    "Queue fence signal failed with " + hresult_hex(hr) + ".");
        return receipt;
    }
    if (fence->GetCompletedValue() < fence_value) {
        const HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (event == nullptr) {
            set_receipt(receipt, NativeD3D12RayTracingExecutionState::failed,
                        NativeD3D12RayTracingFailureStage::synchronization,
                        "native-d3d12-raytracing.fence-event-failed",
                        "CreateEvent failed while waiting for BLAS/TLAS completion.");
            return receipt;
        }
        hr = fence->SetEventOnCompletion(fence_value, event);
        if (SUCCEEDED(hr)) {
            const DWORD wait_result = WaitForSingleObject(event, INFINITE);
            if (wait_result != WAIT_OBJECT_0) hr = E_FAIL;
        }
        CloseHandle(event);
        if (FAILED(hr)) {
            set_receipt(receipt, NativeD3D12RayTracingExecutionState::failed,
                        NativeD3D12RayTracingFailureStage::synchronization,
                        "native-d3d12-raytracing.fence-wait-failed",
                        "Fence completion wait failed with " + hresult_hex(hr) + ".");
            return receipt;
        }
    }
    receipt.blas_build_completed = true;
    receipt.tlas_build_completed = true;
    receipt.synchronization_completed = true;
    set_receipt(receipt, NativeD3D12RayTracingExecutionState::succeeded,
                NativeD3D12RayTracingFailureStage::none,
                "native-d3d12-raytracing.blas-tlas-succeeded",
                "Hardware D3D12 completed one triangle BLAS and one instance TLAS; no trace dispatch was issued.");
    return receipt;
}

#endif

} // namespace

std::string_view native_d3d12_raytracing_execution_state_name(
    const NativeD3D12RayTracingExecutionState state) noexcept {
    switch (state) {
    case NativeD3D12RayTracingExecutionState::unavailable: return "unavailable";
    case NativeD3D12RayTracingExecutionState::unsupported: return "unsupported";
    case NativeD3D12RayTracingExecutionState::failed: return "failed";
    case NativeD3D12RayTracingExecutionState::succeeded: return "succeeded";
    }
    return "unavailable";
}

std::string_view native_d3d12_raytracing_failure_stage_name(
    const NativeD3D12RayTracingFailureStage stage) noexcept {
    switch (stage) {
    case NativeD3D12RayTracingFailureStage::none: return "none";
    case NativeD3D12RayTracingFailureStage::platform: return "platform";
    case NativeD3D12RayTracingFailureStage::loader: return "loader";
    case NativeD3D12RayTracingFailureStage::factory: return "factory";
    case NativeD3D12RayTracingFailureStage::adapter: return "adapter";
    case NativeD3D12RayTracingFailureStage::device: return "device";
    case NativeD3D12RayTracingFailureStage::feature: return "feature";
    case NativeD3D12RayTracingFailureStage::interface_query: return "interface-query";
    case NativeD3D12RayTracingFailureStage::command_queue: return "command-queue";
    case NativeD3D12RayTracingFailureStage::command_allocator:
        return "command-allocator";
    case NativeD3D12RayTracingFailureStage::command_list: return "command-list";
    case NativeD3D12RayTracingFailureStage::vertex_buffer: return "vertex-buffer";
    case NativeD3D12RayTracingFailureStage::blas_prebuild: return "blas-prebuild";
    case NativeD3D12RayTracingFailureStage::blas_resources: return "blas-resources";
    case NativeD3D12RayTracingFailureStage::blas_build: return "blas-build";
    case NativeD3D12RayTracingFailureStage::tlas_prebuild: return "tlas-prebuild";
    case NativeD3D12RayTracingFailureStage::tlas_resources: return "tlas-resources";
    case NativeD3D12RayTracingFailureStage::tlas_build: return "tlas-build";
    case NativeD3D12RayTracingFailureStage::synchronization:
        return "synchronization";
    case NativeD3D12RayTracingFailureStage::cleanup: return "cleanup";
    }
    return "cleanup";
}

NativeD3D12RayTracingReceipt run_native_d3d12_raytracing_executor(
    const NativeD3D12RayTracingExecutorOptions& options) {
    NativeD3D12RayTracingReceipt receipt;
    receipt.native_handle_exposed = false;
#if !defined(_WIN32)
    (void)options;
    set_receipt(receipt, NativeD3D12RayTracingExecutionState::unavailable,
                NativeD3D12RayTracingFailureStage::platform,
                "native-d3d12-raytracing.platform-unavailable",
                "The D3D12 executor is available only on Windows.");
    return receipt;
#else
    D3D12Module d3d12_module;
    DxgiModule dxgi_module;
    if (!d3d12_module.load() || !dxgi_module.load()) {
        set_receipt(receipt, NativeD3D12RayTracingExecutionState::unavailable,
                    NativeD3D12RayTracingFailureStage::loader,
                    "native-d3d12-raytracing.loader-unavailable",
                    "d3d12.dll or dxgi.dll could not be loaded.");
        return receipt;
    }
    const auto create_device = d3d12_module.symbol<CreateDeviceFn>("D3D12CreateDevice");
    const auto create_factory = dxgi_module.symbol<CreateFactoryFn>("CreateDXGIFactory2");
    if (create_device == nullptr || create_factory == nullptr) {
        set_receipt(receipt, NativeD3D12RayTracingExecutionState::unavailable,
                    NativeD3D12RayTracingFailureStage::loader,
                    "native-d3d12-raytracing.entrypoint-unavailable",
                    "The D3D12CreateDevice or CreateDXGIFactory2 entry point is unavailable.");
        return receipt;
    }

    ComPtr<IDXGIFactory4> factory;
    HRESULT hr = create_factory(0U, __uuidof(IDXGIFactory4),
                                reinterpret_cast<void**>(factory.GetAddressOf()));
    if (FAILED(hr) || !factory) {
        set_receipt(receipt, NativeD3D12RayTracingExecutionState::unavailable,
                    NativeD3D12RayTracingFailureStage::factory,
                    "native-d3d12-raytracing.factory-create-failed",
                    "CreateDXGIFactory2 failed with " + hresult_hex(hr) + ".");
        return receipt;
    }

    HardwareDeviceSelection selection;
    choose_hardware_device(factory.Get(), create_device, selection, receipt);
    receipt.hardware_probe_completed = true;
    if (!selection.device) {
        if (options.probe_warp_fallback)
            query_warp_raytracing(create_device, receipt);
        set_receipt(receipt, NativeD3D12RayTracingExecutionState::unsupported,
                    NativeD3D12RayTracingFailureStage::feature,
                    "native-d3d12-raytracing.hardware-unsupported",
                    receipt.warp_fallback_attempted
                        ? "No hardware RT device was available; WARP was probed as an explicit non-hardware fallback and is not accepted."
                        : "No hardware D3D12 device with a ray-tracing tier was available.");
        return receipt;
    }
    return execute_hardware(selection, receipt);
#endif
}

} // namespace noemancer
