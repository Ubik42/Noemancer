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
#include <vector>

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
using SerializeRootSignatureFn = HRESULT(WINAPI*)(
    const D3D12_ROOT_SIGNATURE_DESC*, D3D_ROOT_SIGNATURE_VERSION, ID3DBlob**, ID3DBlob**);

// This is a tiny lib_6_3 DXIL probe compiled by the repository's pinned DXC
// toolchain. Keeping the bounded artifact with the executor makes the
// short-lived probe independent of an untracked build path or a machine-local
// compiler installation. It only writes one 16-byte UAV result and makes no
// claim about a production shader pipeline.
constexpr std::string_view kDxrProbeDxilBase64 =
    "RFhCQ/QoGcU0CaFax3Iw2oDaZrABAAAAMBUAAAYAAAA4AAAASAAAAHQAAABEAgAA4AoAAPwKAABTRkkwCAAAAAAAAAAAAAAA"
    "VkVSUyQAAAABAAkAAAAAABoVAAAUAAAAMGQzZWU2YjUAMS45LjAuNTQwMgBSREFUyAEAABAAAAAEAAAAGAAAALgAAAAIAQAA"
    "tAEAAAEAAACYAAAAAGdTY2VuZQBnT3V0cHV0AAE/UmF5R2VuQEBZQVhYWgBSYXlHZW4AAT9NaXNzQEBZQVhVUGF5bG9hZEBA"
    "QFoATWlzcwABP0Nsb3Nlc3RIaXRAQFlBWFVQYXlsb2FkQEBVQnVpbHRJblRyaWFuZ2xlSW50ZXJzZWN0aW9uQXR0cmlidXRl"
    "c0BAQFoAQ2xvc2VzdEhpdAAAAAADAAAASAAAAAIAAAAgAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAABAAAA"
    "CwAAAAAAAAAAAAAAAAAAAAAAAAAIAAAAAAAAAAQAAACkAAAAAwAAADQAAAAQAAAAIAAAAAAAAAD/////BwAAAAAAAAAAAAAA"
    "AAAAAAAAAACAAAAAYwAHAAAAAAD/////JwAAAD8AAAD//////////wsAAAAEAAAAAAAAAAAAAAAAAAAAAAgAAGMACwAAAAAA"
    "/////0QAAACKAAAA//////////8KAAAABAAAAAgAAAAAAAAAAAAAAAAEAABjAAoAAAAAAP////8CAAAADAAAAAIAAAAAAAAA"
    "AQAAAFNUQVSUCAAAYwAGACUCAABEWElMAwEAABAAAAB8CAAAQkPA3iEMAAAcAgAAC4IgAAIAAAATAAAAB4EjkUHIBEkGEDI5"
    "kgGEDCUFCBkeBItigBhFAkKSC0LEEDIUOAgYSwoyYohIkBQgQ0aIpQAZMkLkSA6QESPEUEFRgYzhg+WKBDFGBlEYAAAMAAAA"
    "G4jg/////wdA2mAIAZAAywZjEIAFoDYYxP////8PgARQG4zi/////wdAAioAAAAASRgAAAQAAAATgmCCIAQTBmEIJgTEhKAA"
    "iSAAAD4AAAAyIogJIGSFBBMjpIQEEyPjhKGQFBJMjIwLhMRMEJjBHAEYnBlIU0QJk78C2BQBAtIYmiAQCxEBE+I07BRRwkRF"
    "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"
    "FCEBNRGScRwPgC0OMAAAAAAAAAAAAAAA";

std::vector<std::uint8_t> decode_base64(const std::string_view encoded) {
    std::vector<std::uint8_t> decoded;
    decoded.reserve((encoded.size() / 4U) * 3U);
    std::uint32_t accumulator = 0U;
    std::uint32_t bits = 0U;
    for (const unsigned char character : encoded) {
        if (character == '=') break;
        std::uint32_t value = 0U;
        if (character >= 'A' && character <= 'Z')
            value = static_cast<std::uint32_t>(character - 'A');
        else if (character >= 'a' && character <= 'z')
            value = static_cast<std::uint32_t>(character - 'a' + 26U);
        else if (character >= '0' && character <= '9')
            value = static_cast<std::uint32_t>(character - '0' + 52U);
        else if (character == '+')
            value = 62U;
        else if (character == '/')
            value = 63U;
        else
            continue;
        accumulator = (accumulator << 6U) | value;
        bits += 6U;
        if (bits >= 8U) {
            bits -= 8U;
            decoded.push_back(static_cast<std::uint8_t>(
                (accumulator >> bits) & 0xffU));
        }
    }
    return decoded;
}

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
    const NativeD3D12RayTracingReceipt& probe_receipt,
    const SerializeRootSignatureFn serialize_root_signature) {
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
    receipt.blas_flags_observed = true;
    receipt.blas_allow_update =
        (blas_inputs.Flags & D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_UPDATE) != 0U;
    receipt.blas_allow_compaction =
        (blas_inputs.Flags & D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_COMPACTION) != 0U;
    receipt.blas_prefer_fast_trace =
        (blas_inputs.Flags & D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE) != 0U;
    receipt.blas_prefer_fast_build =
        (blas_inputs.Flags & D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_BUILD) != 0U;

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
    receipt.blas_build_count = 1U;
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
    receipt.tlas_flags_observed = true;
    receipt.tlas_allow_update =
        (tlas_inputs.Flags & D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_UPDATE) != 0U;
    receipt.tlas_allow_compaction =
        (tlas_inputs.Flags & D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_COMPACTION) != 0U;
    receipt.tlas_prefer_fast_trace =
        (tlas_inputs.Flags & D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE) != 0U;
    receipt.tlas_prefer_fast_build =
        (tlas_inputs.Flags & D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_BUILD) != 0U;

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
    receipt.tlas_build_count = 1U;

    if (serialize_root_signature == nullptr) {
        set_receipt(receipt, NativeD3D12RayTracingExecutionState::unsupported,
                    NativeD3D12RayTracingFailureStage::root_signature,
                    "native-d3d12-raytracing.root-signature-serializer-unavailable",
                    "D3D12SerializeRootSignature is not exported by the loaded D3D12 runtime.");
        return receipt;
    }

    D3D12_ROOT_PARAMETER root_parameters[2U]{};
    root_parameters[0U].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    root_parameters[0U].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    root_parameters[0U].Descriptor.ShaderRegister = 0U;
    root_parameters[0U].Descriptor.RegisterSpace = 0U;
    root_parameters[1U].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    root_parameters[1U].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    root_parameters[1U].Descriptor.ShaderRegister = 0U;
    root_parameters[1U].Descriptor.RegisterSpace = 0U;
    D3D12_ROOT_SIGNATURE_DESC root_signature_description{};
    root_signature_description.NumParameters = 2U;
    root_signature_description.pParameters = root_parameters;
    ComPtr<ID3DBlob> root_signature_blob;
    ComPtr<ID3DBlob> root_signature_error;
    hr = serialize_root_signature(
        &root_signature_description, D3D_ROOT_SIGNATURE_VERSION_1,
        &root_signature_blob, &root_signature_error);
    if (FAILED(hr) || !root_signature_blob || root_signature_blob->GetBufferSize() == 0U) {
        set_receipt(receipt, NativeD3D12RayTracingExecutionState::failed,
                    NativeD3D12RayTracingFailureStage::root_signature,
                    "native-d3d12-raytracing.root-signature-serialize-failed",
                    "D3D12SerializeRootSignature failed with " + hresult_hex(hr) + ".");
        return receipt;
    }
    ComPtr<ID3D12RootSignature> root_signature;
    hr = selection.device->CreateRootSignature(
        0U, root_signature_blob->GetBufferPointer(), root_signature_blob->GetBufferSize(),
        IID_PPV_ARGS(&root_signature));
    if (FAILED(hr) || !root_signature) {
        set_receipt(receipt, NativeD3D12RayTracingExecutionState::failed,
                    NativeD3D12RayTracingFailureStage::root_signature,
                    "native-d3d12-raytracing.root-signature-create-failed",
                    "CreateRootSignature failed with " + hresult_hex(hr) + ".");
        return receipt;
    }
    receipt.root_signature_created = true;

    const std::vector<std::uint8_t> dxil = decode_base64(kDxrProbeDxilBase64);
    if (dxil.empty() || dxil.size() > native_d3d12_raytracing_executor_max_resource_bytes) {
        set_receipt(receipt, NativeD3D12RayTracingExecutionState::failed,
                    NativeD3D12RayTracingFailureStage::state_object,
                    "native-d3d12-raytracing.dxil-artifact-invalid",
                    "The embedded DXIL probe artifact was empty or exceeded the bounded resource budget.");
        return receipt;
    }
    D3D12_EXPORT_DESC exports[3U]{};
    exports[0U].Name = L"RayGen";
    exports[1U].Name = L"Miss";
    exports[2U].Name = L"ClosestHit";
    D3D12_DXIL_LIBRARY_DESC dxil_library{};
    dxil_library.DXILLibrary.BytecodeLength = dxil.size();
    dxil_library.DXILLibrary.pShaderBytecode = dxil.data();
    dxil_library.NumExports = 3U;
    dxil_library.pExports = exports;
    D3D12_HIT_GROUP_DESC hit_group{};
    hit_group.Type = D3D12_HIT_GROUP_TYPE_TRIANGLES;
    hit_group.HitGroupExport = L"TriangleHitGroup";
    hit_group.ClosestHitShaderImport = L"ClosestHit";
    D3D12_RAYTRACING_SHADER_CONFIG shader_config{};
    shader_config.MaxPayloadSizeInBytes = sizeof(std::uint32_t);
    shader_config.MaxAttributeSizeInBytes = sizeof(float) * 2U;
    D3D12_GLOBAL_ROOT_SIGNATURE global_root_signature{};
    global_root_signature.pGlobalRootSignature = root_signature.Get();
    D3D12_RAYTRACING_PIPELINE_CONFIG pipeline_config{};
    pipeline_config.MaxTraceRecursionDepth = 1U;
    std::array<D3D12_STATE_SUBOBJECT, 5U> subobjects{};
    subobjects[0U].Type = D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY;
    subobjects[0U].pDesc = &dxil_library;
    subobjects[1U].Type = D3D12_STATE_SUBOBJECT_TYPE_HIT_GROUP;
    subobjects[1U].pDesc = &hit_group;
    subobjects[2U].Type = D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_SHADER_CONFIG;
    subobjects[2U].pDesc = &shader_config;
    subobjects[3U].Type = D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE;
    subobjects[3U].pDesc = &global_root_signature;
    subobjects[4U].Type = D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG;
    subobjects[4U].pDesc = &pipeline_config;
    D3D12_STATE_OBJECT_DESC state_object_description{};
    state_object_description.Type = D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE;
    state_object_description.NumSubobjects = static_cast<UINT>(subobjects.size());
    state_object_description.pSubobjects = subobjects.data();
    ComPtr<ID3D12StateObject> state_object;
    hr = selection.device->CreateStateObject(
        &state_object_description, IID_PPV_ARGS(&state_object));
    if (FAILED(hr) || !state_object) {
        set_receipt(receipt, NativeD3D12RayTracingExecutionState::failed,
                    NativeD3D12RayTracingFailureStage::state_object,
                    "native-d3d12-raytracing.state-object-create-failed",
                    "CreateStateObject failed with " + hresult_hex(hr) + ".");
        return receipt;
    }
    receipt.state_object_created = true;
    ComPtr<ID3D12StateObjectProperties> state_properties;
    hr = state_object.As(&state_properties);
    if (FAILED(hr) || !state_properties) {
        set_receipt(receipt, NativeD3D12RayTracingExecutionState::failed,
                    NativeD3D12RayTracingFailureStage::state_object,
                    "native-d3d12-raytracing.state-object-properties-unavailable",
                    "ID3D12StateObjectProperties was unavailable after CreateStateObject.");
        return receipt;
    }

    constexpr std::uint32_t shader_record_bytes =
        D3D12_RAYTRACING_SHADER_RECORD_BYTE_ALIGNMENT;
    constexpr std::uint32_t shader_record_count = 3U;
    static constexpr std::size_t shader_table_byte_count =
        static_cast<std::size_t>(shader_record_bytes) * 6U;
    constexpr std::uint64_t shader_table_bytes = shader_table_byte_count;
    receipt.shader_table_record_count = shader_record_count;
    receipt.shader_table_record_bytes = shader_record_bytes;
    receipt.shader_table_bytes = shader_table_bytes;
    const void* raygen_identifier = state_properties->GetShaderIdentifier(L"RayGen");
    const void* miss_identifier = state_properties->GetShaderIdentifier(L"Miss");
    const void* hit_identifier = state_properties->GetShaderIdentifier(L"TriangleHitGroup");
    if (raygen_identifier == nullptr || miss_identifier == nullptr || hit_identifier == nullptr) {
        set_receipt(receipt, NativeD3D12RayTracingExecutionState::failed,
                    NativeD3D12RayTracingFailureStage::shader_table,
                    "native-d3d12-raytracing.shader-identifiers-missing",
                    "The DXR state object did not expose all three shader identifiers.");
        return receipt;
    }
    std::array<std::uint8_t, shader_table_byte_count> shader_table{};
    std::memcpy(shader_table.data(), raygen_identifier, D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES);
    std::memcpy(shader_table.data() + shader_record_bytes * 2U,
                miss_identifier, D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES);
    std::memcpy(shader_table.data() + shader_record_bytes * 4U,
                hit_identifier, D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES);
    ComPtr<ID3D12Resource> shader_table_resource;
    hr = create_committed_buffer(
        selection.device.Get(), shader_table_bytes, D3D12_HEAP_TYPE_UPLOAD,
        D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_FLAG_NONE,
        shader_table_resource);
    if (FAILED(hr) || !shader_table_resource ||
        !fill_upload_buffer(shader_table_resource.Get(), shader_table.data(), shader_table.size())) {
        set_receipt(receipt, NativeD3D12RayTracingExecutionState::failed,
                    NativeD3D12RayTracingFailureStage::shader_table,
                    "native-d3d12-raytracing.shader-table-upload-failed",
                    "Shader table upload failed with " + hresult_hex(hr) + ".");
        return receipt;
    }
    receipt.shader_table_prepared = true;
    receipt.shader_table_uploaded = true;

    constexpr std::uint32_t output_width = 1U;
    constexpr std::uint32_t output_height = 1U;
    constexpr std::uint32_t output_stride_bytes = sizeof(std::uint32_t) * 4U;
    constexpr std::uint64_t output_bytes =
        static_cast<std::uint64_t>(output_width) * output_height * output_stride_bytes;
    receipt.output_width = output_width;
    receipt.output_height = output_height;
    receipt.output_pixel_stride_bytes = output_stride_bytes;
    receipt.output_bytes = output_bytes;
    receipt.output_readback_bytes = output_bytes;
    ComPtr<ID3D12Resource> output_resource;
    hr = create_committed_buffer(
        selection.device.Get(), output_bytes, D3D12_HEAP_TYPE_DEFAULT,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
        output_resource);
    if (FAILED(hr) || !output_resource) {
        set_receipt(receipt, NativeD3D12RayTracingExecutionState::failed,
                    NativeD3D12RayTracingFailureStage::output_resource,
                    "native-d3d12-raytracing.output-create-failed",
                    "The 1x1 UAV output resource could not be created with " + hresult_hex(hr) + ".");
        return receipt;
    }
    ComPtr<ID3D12Resource> output_readback;
    hr = create_committed_buffer(
        selection.device.Get(), output_bytes, D3D12_HEAP_TYPE_READBACK,
        D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_FLAG_NONE, output_readback);
    if (FAILED(hr) || !output_readback) {
        set_receipt(receipt, NativeD3D12RayTracingExecutionState::failed,
                    NativeD3D12RayTracingFailureStage::output_resource,
                    "native-d3d12-raytracing.readback-create-failed",
                    "The 1x1 readback resource could not be created with " + hresult_hex(hr) + ".");
        return receipt;
    }
    receipt.output_resource_created = true;

    UINT64 timestamp_frequency = 0U;
    hr = queue->GetTimestampFrequency(&timestamp_frequency);
    if (FAILED(hr) || timestamp_frequency == 0U || timestamp_frequency > 1'000'000'000'000ULL) {
        set_receipt(receipt, NativeD3D12RayTracingExecutionState::failed,
                    NativeD3D12RayTracingFailureStage::timestamp_query,
                    "native-d3d12-raytracing.timestamp-frequency-unavailable",
                    "The D3D12 command queue did not return a bounded timestamp frequency with " + hresult_hex(hr) + ".");
        return receipt;
    }
    receipt.gpu_timestamp_frequency_hz = timestamp_frequency;
    D3D12_QUERY_HEAP_DESC timestamp_heap_description{};
    timestamp_heap_description.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
    timestamp_heap_description.Count = 2U;
    ComPtr<ID3D12QueryHeap> timestamp_heap;
    hr = selection.device->CreateQueryHeap(
        &timestamp_heap_description, IID_PPV_ARGS(&timestamp_heap));
    if (FAILED(hr) || !timestamp_heap) {
        set_receipt(receipt, NativeD3D12RayTracingExecutionState::failed,
                    NativeD3D12RayTracingFailureStage::timestamp_query,
                    "native-d3d12-raytracing.timestamp-heap-create-failed",
                    "The D3D12 timestamp query heap could not be created with " + hresult_hex(hr) + ".");
        return receipt;
    }
    constexpr std::uint64_t timestamp_readback_bytes = sizeof(UINT64) * 2U;
    receipt.gpu_timestamp_readback_bytes = timestamp_readback_bytes;
    ComPtr<ID3D12Resource> timestamp_readback;
    hr = create_committed_buffer(
        selection.device.Get(), timestamp_readback_bytes, D3D12_HEAP_TYPE_READBACK,
        D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_FLAG_NONE,
        timestamp_readback);
    if (FAILED(hr) || !timestamp_readback) {
        set_receipt(receipt, NativeD3D12RayTracingExecutionState::failed,
                    NativeD3D12RayTracingFailureStage::timestamp_query,
                    "native-d3d12-raytracing.timestamp-readback-create-failed",
                    "The D3D12 timestamp readback resource could not be created with " + hresult_hex(hr) + ".");
        return receipt;
    }
    receipt.timestamp_query_created = true;

    D3D12_RESOURCE_BARRIER tlas_uav_barrier{};
    tlas_uav_barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    tlas_uav_barrier.UAV.pResource = tlas_result.Get();
    command_list->ResourceBarrier(1U, &tlas_uav_barrier);
    command_list->SetComputeRootSignature(root_signature.Get());
    command_list->SetPipelineState1(state_object.Get());
    command_list->SetComputeRootShaderResourceView(0U, tlas_result->GetGPUVirtualAddress());
    command_list->SetComputeRootUnorderedAccessView(1U, output_resource->GetGPUVirtualAddress());
    D3D12_DISPATCH_RAYS_DESC dispatch{};
    const D3D12_GPU_VIRTUAL_ADDRESS shader_table_address =
        shader_table_resource->GetGPUVirtualAddress();
    dispatch.RayGenerationShaderRecord.StartAddress = shader_table_address;
    dispatch.RayGenerationShaderRecord.SizeInBytes = shader_record_bytes;
    dispatch.MissShaderTable.StartAddress = shader_table_address + shader_record_bytes * 2U;
    dispatch.MissShaderTable.SizeInBytes = shader_record_bytes;
    dispatch.MissShaderTable.StrideInBytes = shader_record_bytes;
    dispatch.HitGroupTable.StartAddress = shader_table_address + shader_record_bytes * 4U;
    dispatch.HitGroupTable.SizeInBytes = shader_record_bytes;
    dispatch.HitGroupTable.StrideInBytes = shader_record_bytes;
    dispatch.Width = output_width;
    dispatch.Height = output_height;
    dispatch.Depth = 1U;
    command_list->EndQuery(timestamp_heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0U);
    command_list->DispatchRays(&dispatch);
    command_list->EndQuery(timestamp_heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 1U);
    receipt.timestamp_queries_issued = true;
    receipt.trace_dispatch_issued = true;
    receipt.build_only = false;
    command_list->ResolveQueryData(
        timestamp_heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0U, 2U,
        timestamp_readback.Get(), 0U);
    receipt.timestamp_data_resolved = true;
    D3D12_RESOURCE_BARRIER output_barrier{};
    output_barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    output_barrier.Transition.pResource = output_resource.Get();
    output_barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    output_barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    output_barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    command_list->ResourceBarrier(1U, &output_barrier);
    command_list->CopyResource(output_readback.Get(), output_resource.Get());

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
    receipt.trace_dispatch_submitted = receipt.trace_dispatch_issued;

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
                        "CreateEvent failed while waiting for BLAS/TLAS and TraceRays completion.");
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
    receipt.trace_dispatch_completed = receipt.trace_dispatch_submitted;
    receipt.synchronization_completed = true;

    std::array<std::uint64_t, 2U> timestamp_values{};
    void* mapped_timestamps = nullptr;
    const D3D12_RANGE timestamp_read_range{0U, static_cast<SIZE_T>(timestamp_readback_bytes)};
    hr = timestamp_readback->Map(0U, &timestamp_read_range, &mapped_timestamps);
    if (FAILED(hr) || mapped_timestamps == nullptr) {
        set_receipt(receipt, NativeD3D12RayTracingExecutionState::failed,
                    NativeD3D12RayTracingFailureStage::timestamp_query,
                    "native-d3d12-raytracing.timestamp-readback-map-failed",
                    "Mapping the completed timestamp readback failed with " + hresult_hex(hr) + ".");
        return receipt;
    }
    std::memcpy(timestamp_values.data(), mapped_timestamps,
                timestamp_values.size() * sizeof(std::uint64_t));
    const D3D12_RANGE timestamp_written_range{0U, 0U};
    timestamp_readback->Unmap(0U, &timestamp_written_range);
    receipt.gpu_timestamp_ticks_begin = timestamp_values[0U];
    receipt.gpu_timestamp_ticks_end = timestamp_values[1U];
    if (receipt.gpu_timestamp_ticks_end <= receipt.gpu_timestamp_ticks_begin) {
        set_receipt(receipt, NativeD3D12RayTracingExecutionState::failed,
                    NativeD3D12RayTracingFailureStage::timestamp_query,
                    "native-d3d12-raytracing.timestamp-order-invalid",
                    "GPU timestamp readback did not produce a positive TraceRays interval.");
        return receipt;
    }
    receipt.gpu_timestamp_ticks_delta = receipt.gpu_timestamp_ticks_end -
        receipt.gpu_timestamp_ticks_begin;
    if (receipt.gpu_timestamp_ticks_delta > 60ULL * timestamp_frequency) {
        set_receipt(receipt, NativeD3D12RayTracingExecutionState::failed,
                    NativeD3D12RayTracingFailureStage::timestamp_query,
                    "native-d3d12-raytracing.timestamp-duration-unbounded",
                    "GPU TraceRays duration exceeded the bounded 60-second evidence window.");
        return receipt;
    }
    const auto whole_seconds = receipt.gpu_timestamp_ticks_delta / timestamp_frequency;
    const auto fractional_ticks = receipt.gpu_timestamp_ticks_delta % timestamp_frequency;
    receipt.gpu_timestamp_duration_ns = whole_seconds * 1'000'000'000ULL +
        static_cast<std::uint64_t>(
            (static_cast<long double>(fractional_ticks) * 1'000'000'000.0L) /
            static_cast<long double>(timestamp_frequency));
    receipt.gpu_timestamps_valid = receipt.timestamp_query_created &&
        receipt.timestamp_queries_issued && receipt.timestamp_data_resolved &&
        receipt.gpu_timestamp_frequency_hz > 0U &&
        receipt.gpu_timestamp_ticks_delta > 0U &&
        receipt.gpu_timestamp_duration_ns > 0U &&
        receipt.gpu_timestamp_readback_bytes == timestamp_readback_bytes;
    if (!receipt.gpu_timestamps_valid) {
        set_receipt(receipt, NativeD3D12RayTracingExecutionState::failed,
                    NativeD3D12RayTracingFailureStage::timestamp_query,
                    "native-d3d12-raytracing.timestamp-contract-invalid",
                    "Timestamp query metadata was not complete after GPU readback.");
        return receipt;
    }

    std::array<std::uint32_t, 4U> output_words{};
    void* mapped_output = nullptr;
    const D3D12_RANGE output_read_range{0U, static_cast<SIZE_T>(output_bytes)};
    hr = output_readback->Map(0U, &output_read_range, &mapped_output);
    if (FAILED(hr) || mapped_output == nullptr) {
        set_receipt(receipt, NativeD3D12RayTracingExecutionState::failed,
                    NativeD3D12RayTracingFailureStage::readback,
                    "native-d3d12-raytracing.output-readback-map-failed",
                    "Mapping the completed 1x1 output readback failed with " + hresult_hex(hr) + ".");
        return receipt;
    }
    std::memcpy(output_words.data(), mapped_output, output_words.size() * sizeof(std::uint32_t));
    const D3D12_RANGE output_written_range{0U, 0U};
    output_readback->Unmap(0U, &output_written_range);
    receipt.output_sentinel = output_words[0U];
    receipt.output_hit = output_words[1U];
    receipt.output_pixel_x = output_words[2U];
    receipt.output_pixel_y = output_words[3U];
    std::uint64_t output_hash = 1469598103934665603ULL;
    const auto* output_bytes_ptr = reinterpret_cast<const std::uint8_t*>(output_words.data());
    for (std::size_t index = 0U; index < output_bytes; ++index) {
        output_hash ^= output_bytes_ptr[index];
        output_hash *= 1099511628211ULL;
    }
    receipt.output_hash = output_hash;
    receipt.output_readback_completed = true;
    if (receipt.output_sentinel != 0x52415931U || receipt.output_hit != 1U ||
        receipt.output_pixel_x != 0U || receipt.output_pixel_y != 0U) {
        set_receipt(receipt, NativeD3D12RayTracingExecutionState::failed,
                    NativeD3D12RayTracingFailureStage::readback,
                    "native-d3d12-raytracing.output-sentinel-mismatch",
                    "TraceRays completed but the 1x1 readback did not contain the expected hit sentinel.");
        return receipt;
    }
    set_receipt(receipt, NativeD3D12RayTracingExecutionState::succeeded,
                NativeD3D12RayTracingFailureStage::none,
                "native-d3d12-raytracing.trace-dispatch-succeeded",
                "Hardware D3D12 completed one triangle BLAS/TLAS, one 1x1 shader-table dispatch and a 16-byte UAV readback; this is not RTGI.");
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
    case NativeD3D12RayTracingFailureStage::root_signature:
        return "root-signature";
    case NativeD3D12RayTracingFailureStage::state_object: return "state-object";
    case NativeD3D12RayTracingFailureStage::shader_table: return "shader-table";
    case NativeD3D12RayTracingFailureStage::output_resource:
        return "output-resource";
    case NativeD3D12RayTracingFailureStage::trace_dispatch:
        return "trace-dispatch";
    case NativeD3D12RayTracingFailureStage::readback: return "readback";
    case NativeD3D12RayTracingFailureStage::timestamp_query:
        return "timestamp-query";
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
    const auto serialize_root_signature =
        d3d12_module.symbol<SerializeRootSignatureFn>("D3D12SerializeRootSignature");
    return execute_hardware(selection, receipt, serialize_root_signature);
#endif
}

} // namespace noemancer
