#include "runtime/native_raytracing_capability_adapter.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <string>
#include <ranges>
#include <utility>
#include <vector>

#if defined(_WIN32)
#  define NOMINMAX
#  include <windows.h>
#  include <d3d12.h>
#  include <dxgi1_6.h>
#  include <wrl/client.h>
#else
#  include <dlfcn.h>
#endif

namespace noemancer {
namespace {

std::string bounded_text(const std::string_view value) {
    return std::string(value.substr(0U, native_raytracing_capability_max_text_bytes));
}

NativeRayTracingCapability base_capability(const NativeRayTracingBackend backend) {
    NativeRayTracingCapability result;
    result.backend = std::string(native_raytracing_backend_name(backend));
    return result;
}

void set_result(NativeRayTracingCapability& result,
                const NativeRayTracingProbeState state,
                const std::string_view code,
                const std::string_view detail) {
    result.state = state;
    result.code = bounded_text(code);
    result.detail = bounded_text(detail);
}

#if defined(_WIN32)

std::string utf8_from_wide(const wchar_t* value) {
    if (value == nullptr || value[0] == L'\0') return {};
    const int required = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value, -1,
                                             nullptr, 0, nullptr, nullptr);
    if (required <= 1) return {};
    std::string output(static_cast<std::size_t>(required - 1), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value, -1, output.data(),
                            required, nullptr, nullptr) <= 0) return {};
    return output;
}

NativeRayTracingCapability probe_d3d12_impl() {
    auto result = base_capability(NativeRayTracingBackend::d3d12);
    const auto dxgi_module = LoadLibraryW(L"dxgi.dll");
    const auto d3d12_module = LoadLibraryW(L"d3d12.dll");
    if (dxgi_module == nullptr || d3d12_module == nullptr) {
        if (dxgi_module != nullptr) FreeLibrary(dxgi_module);
        if (d3d12_module != nullptr) FreeLibrary(d3d12_module);
        set_result(result, NativeRayTracingProbeState::unavailable,
                   "native-raytracing.d3d12-loader-unavailable",
                   "dxgi.dll or d3d12.dll could not be loaded.");
        return result;
    }
    result.loader_available = true;

    using CreateFactoryFn = HRESULT(WINAPI*)(UINT, REFIID, void**);
    using CreateDeviceFn = HRESULT(WINAPI*)(IUnknown*, D3D_FEATURE_LEVEL, REFIID, void**);
    const auto create_factory = reinterpret_cast<CreateFactoryFn>(
        GetProcAddress(dxgi_module, "CreateDXGIFactory2"));
    const auto create_device = reinterpret_cast<CreateDeviceFn>(
        GetProcAddress(d3d12_module, "D3D12CreateDevice"));
    if (create_factory == nullptr || create_device == nullptr) {
        FreeLibrary(d3d12_module);
        FreeLibrary(dxgi_module);
        set_result(result, NativeRayTracingProbeState::unavailable,
                   "native-raytracing.d3d12-entrypoint-unavailable",
                   "The Windows graphics entry points required for a read-only probe are missing.");
        return result;
    }

    Microsoft::WRL::ComPtr<IDXGIFactory4> factory;
    const auto factory_hr = create_factory(0U, IID_PPV_ARGS(&factory));
    if (FAILED(factory_hr)) {
        FreeLibrary(d3d12_module);
        FreeLibrary(dxgi_module);
        set_result(result, NativeRayTracingProbeState::query_failed,
                   "native-raytracing.d3d12-factory-query-failed",
                   "CreateDXGIFactory2 failed while enumerating hardware adapters.");
        return result;
    }

    D3D12_RAYTRACING_TIER best_tier = D3D12_RAYTRACING_TIER_NOT_SUPPORTED;
    for (UINT index = 0U;; ++index) {
        Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
        if (factory->EnumAdapters1(index, &adapter) == DXGI_ERROR_NOT_FOUND) break;
        if (!adapter) continue;
        ++result.device_count;
        DXGI_ADAPTER_DESC1 description{};
        if (FAILED(adapter->GetDesc1(&description)) ||
            (description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0U) continue;

        Microsoft::WRL::ComPtr<ID3D12Device> device;
        const auto device_hr = create_device(adapter.Get(), D3D_FEATURE_LEVEL_12_0,
                                             IID_PPV_ARGS(&device));
        if (FAILED(device_hr) || !device) continue;
        result.native_device_created = true;

        D3D12_FEATURE_DATA_D3D12_OPTIONS5 options5{};
        const auto feature_hr = device->CheckFeatureSupport(
            D3D12_FEATURE_D3D12_OPTIONS5, &options5, sizeof(options5));
        if (FAILED(feature_hr)) continue;
        result.feature_query_completed = true;
        if (options5.RaytracingTier > best_tier) {
            best_tier = options5.RaytracingTier;
            result.ray_tracing_tier = static_cast<std::uint32_t>(options5.RaytracingTier);
            result.device_name = bounded_text(utf8_from_wide(description.Description));
        }
        if (options5.RaytracingTier != D3D12_RAYTRACING_TIER_NOT_SUPPORTED) {
            ++result.supported_device_count;
            result.acceleration_structure_feature = true;
            result.ray_tracing_pipeline_feature = true;
        }
    }
    result.device_query_completed = true;
    if (result.supported_device_count > 0U) {
        set_result(result, NativeRayTracingProbeState::supported,
                   "native-raytracing.d3d12-supported",
                   "D3D12 OPTIONS5 reported a hardware ray-tracing tier.");
    } else if (result.native_device_created && !result.feature_query_completed) {
        set_result(result, NativeRayTracingProbeState::query_failed,
                   "native-raytracing.d3d12-feature-query-failed",
                   "A D3D12 device was created, but OPTIONS5 could not be queried.");
    } else if (result.device_count > 0U) {
        set_result(result, NativeRayTracingProbeState::unsupported,
                   "native-raytracing.d3d12-tier-unavailable",
                   "Hardware adapters were enumerated, but none reported a ray-tracing tier.");
    } else {
        set_result(result, NativeRayTracingProbeState::unsupported,
                   "native-raytracing.d3d12-no-hardware-adapter",
                   "The DXGI factory exposed no hardware adapter usable by D3D12.");
    }
    // ComPtr releases the temporary factory/device before the loader modules
    // are unloaded.  No device or adapter handle is retained in the result.
    factory.Reset();
    FreeLibrary(d3d12_module);
    FreeLibrary(dxgi_module);
    return result;
}

#else

NativeRayTracingCapability probe_d3d12_impl() {
    auto result = base_capability(NativeRayTracingBackend::d3d12);
    set_result(result, NativeRayTracingProbeState::unavailable,
               "native-raytracing.d3d12-platform-unavailable",
               "D3D12 capability probing is only available on Windows.");
    return result;
}

#endif

// The runtime does not require a Vulkan SDK at link time.  These private ABI
// declarations cover only the loader/instance/feature query structs used by
// this read-only probe.  They mirror Vulkan 1.1/KHR ray-tracing definitions
// and never cross the public adapter boundary.
namespace vulkan_abi {

#if defined(_WIN32)
#  define NOEMANCER_VK_CALL __stdcall
#else
#  define NOEMANCER_VK_CALL
#endif

using Bool32 = std::uint32_t;
using DeviceSize = std::uint64_t;
using Result = std::int32_t;
using StructureType = std::int32_t;
using Instance = void*;
using PhysicalDevice = void*;
using VoidFunction = void*;

constexpr StructureType structure_type_application_info = 0;
constexpr StructureType structure_type_instance_create_info = 1;
constexpr StructureType structure_type_physical_device_features_2 = 1000059000;
constexpr StructureType structure_type_acceleration_structure_features = 1000150013;
constexpr StructureType structure_type_ray_tracing_pipeline_features = 1000347000;
constexpr StructureType structure_type_buffer_device_address_features = 1000257000;
constexpr Result result_success = 0;
constexpr Result result_incomplete = 5;
constexpr std::uint32_t api_version_1_0 = (1U << 22U);
constexpr std::uint32_t api_version_1_1 = (1U << 22U) | (1U << 12U);

struct ApplicationInfo final {
    StructureType s_type;
    const void* p_next;
    const char* application_name;
    std::uint32_t application_version;
    const char* engine_name;
    std::uint32_t engine_version;
    std::uint32_t api_version;
};

struct InstanceCreateInfo final {
    StructureType s_type;
    const void* p_next;
    std::uint32_t flags;
    const ApplicationInfo* application_info;
    std::uint32_t enabled_layer_count;
    const char* const* enabled_layer_names;
    std::uint32_t enabled_extension_count;
    const char* const* enabled_extension_names;
};

struct ExtensionProperties final {
    char extension_name[256];
    std::uint32_t spec_version;
};

struct PhysicalDeviceFeatures final {
    std::array<Bool32, 55U> values{};
};

struct PhysicalDeviceFeatures2 final {
    StructureType s_type;
    void* p_next;
    PhysicalDeviceFeatures features;
};

struct AccelerationStructureFeatures final {
    StructureType s_type;
    void* p_next;
    Bool32 acceleration_structure;
    Bool32 acceleration_structure_capture_replay;
    Bool32 acceleration_structure_indirect_build;
    Bool32 acceleration_structure_host_commands;
    Bool32 descriptor_binding_acceleration_structure_update_after_bind;
};

struct RayTracingPipelineFeatures final {
    StructureType s_type;
    void* p_next;
    Bool32 ray_tracing_pipeline;
    Bool32 ray_tracing_pipeline_shader_group_handle_capture_replay;
    Bool32 ray_tracing_pipeline_shader_group_handle_capture_replay_mixed;
    Bool32 ray_tracing_pipeline_trace_rays_indirect;
    Bool32 ray_traversal_primitive_culling;
};

struct BufferDeviceAddressFeatures final {
    StructureType s_type;
    void* p_next;
    Bool32 buffer_device_address;
    Bool32 buffer_device_address_capture_replay;
    Bool32 buffer_device_address_multi_device;
};

using GetInstanceProcAddr = VoidFunction (NOEMANCER_VK_CALL*)(Instance, const char*);
using EnumerateInstanceVersion = Result (NOEMANCER_VK_CALL*)(std::uint32_t*);
using CreateInstance = Result (NOEMANCER_VK_CALL*)(const InstanceCreateInfo*, const void*, Instance*);
using DestroyInstance = void (NOEMANCER_VK_CALL*)(Instance, const void*);
using EnumeratePhysicalDevices = Result (NOEMANCER_VK_CALL*)(Instance, std::uint32_t*, PhysicalDevice*);
using EnumerateDeviceExtensionProperties = Result (NOEMANCER_VK_CALL*)(
    PhysicalDevice, const char*, std::uint32_t*, ExtensionProperties*);
using GetPhysicalDeviceFeatures2 = void (NOEMANCER_VK_CALL*)(PhysicalDevice, PhysicalDeviceFeatures2*);

} // namespace vulkan_abi

struct VulkanModule final {
#if defined(_WIN32)
    HMODULE handle{};
#else
    void* handle{};
#endif

    VulkanModule() = default;
    VulkanModule(const VulkanModule&) = delete;
    VulkanModule& operator=(const VulkanModule&) = delete;
    ~VulkanModule() {
#if defined(_WIN32)
        if (handle != nullptr) FreeLibrary(handle);
#else
        if (handle != nullptr) dlclose(handle);
#endif
    }

    [[nodiscard]] bool load() {
#if defined(_WIN32)
        handle = LoadLibraryW(L"vulkan-1.dll");
#else
        handle = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
        if (handle == nullptr) handle = dlopen("libvulkan.so", RTLD_NOW | RTLD_LOCAL);
#endif
        return handle != nullptr;
    }

    [[nodiscard]] void* symbol(const char* name) const {
        if (handle == nullptr) return nullptr;
#if defined(_WIN32)
        return reinterpret_cast<void*>(GetProcAddress(handle, name));
#else
        return dlsym(handle, name);
#endif
    }
};

template <typename Function>
Function load_vulkan_function(const VulkanModule& module,
                              const vulkan_abi::GetInstanceProcAddr get_instance_proc_addr,
                              const vulkan_abi::Instance instance,
                              const char* name) {
    auto function = module.symbol(name);
    if (function == nullptr && get_instance_proc_addr != nullptr)
        function = get_instance_proc_addr(instance, name);
    return reinterpret_cast<Function>(function);
}

std::uint32_t vulkan_version_major(const std::uint32_t version) { return version >> 22U; }
std::uint32_t vulkan_version_minor(const std::uint32_t version) { return (version >> 12U) & 0x3ffU; }
std::uint32_t vulkan_version_patch(const std::uint32_t version) { return version & 0xfffU; }

bool has_extension(const std::vector<vulkan_abi::ExtensionProperties>& extensions,
                   const std::string_view name) {
    return std::ranges::any_of(extensions, [name](const vulkan_abi::ExtensionProperties& extension) {
        return std::string_view(extension.extension_name) == name;
    });
}

NativeRayTracingCapability probe_vulkan_impl() {
    auto result = base_capability(NativeRayTracingBackend::vulkan);
    VulkanModule module;
    if (!module.load()) {
        set_result(result, NativeRayTracingProbeState::unavailable,
                   "native-raytracing.vulkan-loader-unavailable",
                   "The Vulkan loader could not be loaded.");
        return result;
    }
    result.loader_available = true;

    const auto get_instance_proc_addr = reinterpret_cast<vulkan_abi::GetInstanceProcAddr>(
        module.symbol("vkGetInstanceProcAddr"));
    if (get_instance_proc_addr == nullptr) {
        set_result(result, NativeRayTracingProbeState::unavailable,
                   "native-raytracing.vulkan-entrypoint-unavailable",
                   "vkGetInstanceProcAddr is missing from the Vulkan loader.");
        return result;
    }
    const auto enumerate_instance_version = load_vulkan_function<
        vulkan_abi::EnumerateInstanceVersion>(module, get_instance_proc_addr, nullptr,
                                              "vkEnumerateInstanceVersion");
    std::uint32_t api_version = vulkan_abi::api_version_1_0;
    if (enumerate_instance_version != nullptr) {
        std::uint32_t loader_version = vulkan_abi::api_version_1_0;
        if (enumerate_instance_version(&loader_version) == vulkan_abi::result_success)
            api_version = loader_version;
    }
    result.api_version_major = vulkan_version_major(api_version);
    result.api_version_minor = vulkan_version_minor(api_version);
    result.api_version_patch = vulkan_version_patch(api_version);

    const auto create_instance = load_vulkan_function<vulkan_abi::CreateInstance>(
        module, get_instance_proc_addr, nullptr, "vkCreateInstance");
    if (create_instance == nullptr) {
        set_result(result, NativeRayTracingProbeState::unavailable,
                   "native-raytracing.vulkan-entrypoint-unavailable",
                   "vkCreateInstance is missing from the Vulkan loader.");
        return result;
    }
    const auto requested_api = api_version >= vulkan_abi::api_version_1_1
        ? vulkan_abi::api_version_1_1 : vulkan_abi::api_version_1_0;
    const vulkan_abi::ApplicationInfo application_info{
        vulkan_abi::structure_type_application_info, nullptr, "Noemancer", 1U,
        "Noemancer", 1U, requested_api};
    const vulkan_abi::InstanceCreateInfo create_info{
        vulkan_abi::structure_type_instance_create_info, nullptr, 0U, &application_info,
        0U, nullptr, 0U, nullptr};
    vulkan_abi::Instance instance = nullptr;
    const auto create_result = create_instance(&create_info, nullptr, &instance);
    if (create_result != vulkan_abi::result_success || instance == nullptr) {
        set_result(result, NativeRayTracingProbeState::query_failed,
                   "native-raytracing.vulkan-instance-create-failed",
                   "The Vulkan loader was present but could not create a temporary query instance.");
        return result;
    }
    const auto destroy_instance = load_vulkan_function<vulkan_abi::DestroyInstance>(
        module, get_instance_proc_addr, instance, "vkDestroyInstance");
    const auto enumerate_physical_devices = load_vulkan_function<
        vulkan_abi::EnumeratePhysicalDevices>(module, get_instance_proc_addr, instance,
                                              "vkEnumeratePhysicalDevices");
    const auto enumerate_device_extensions = load_vulkan_function<
        vulkan_abi::EnumerateDeviceExtensionProperties>(
            module, get_instance_proc_addr, instance, "vkEnumerateDeviceExtensionProperties");
    const auto get_physical_device_features2 = load_vulkan_function<
        vulkan_abi::GetPhysicalDeviceFeatures2>(module, get_instance_proc_addr, instance,
                                                "vkGetPhysicalDeviceFeatures2");
    if (destroy_instance == nullptr || enumerate_physical_devices == nullptr ||
        enumerate_device_extensions == nullptr || get_physical_device_features2 == nullptr) {
        if (destroy_instance != nullptr) destroy_instance(instance, nullptr);
        set_result(result, NativeRayTracingProbeState::unavailable,
                   "native-raytracing.vulkan-query-entrypoint-unavailable",
                   "The temporary Vulkan instance lacks a required physical-device query entry point.");
        return result;
    }

    std::uint32_t device_count = 0U;
    const auto count_result = enumerate_physical_devices(instance, &device_count, nullptr);
    if (count_result != vulkan_abi::result_success && count_result != vulkan_abi::result_incomplete) {
        destroy_instance(instance, nullptr);
        set_result(result, NativeRayTracingProbeState::query_failed,
                   "native-raytracing.vulkan-physical-device-enumeration-failed",
                   "vkEnumeratePhysicalDevices failed while obtaining the device count.");
        return result;
    }
    device_count = std::min(device_count, 64U);
    std::vector<vulkan_abi::PhysicalDevice> devices(device_count);
    if (device_count > 0U) {
        const auto enumerate_result = enumerate_physical_devices(instance, &device_count,
                                                                  devices.data());
        if (enumerate_result != vulkan_abi::result_success &&
            enumerate_result != vulkan_abi::result_incomplete) {
            destroy_instance(instance, nullptr);
            set_result(result, NativeRayTracingProbeState::query_failed,
                       "native-raytracing.vulkan-physical-device-enumeration-failed",
                       "vkEnumeratePhysicalDevices failed while obtaining device handles.");
            return result;
        }
    }
    result.device_count = device_count;
    result.device_query_completed = true;
    for (std::uint32_t index = 0U; index < device_count; ++index) {
        std::uint32_t extension_count = 0U;
        const auto extension_count_result = enumerate_device_extensions(
            devices[index], nullptr, &extension_count, nullptr);
        if (extension_count_result != vulkan_abi::result_success &&
            extension_count_result != vulkan_abi::result_incomplete) continue;
        extension_count = std::min(extension_count, 256U);
        std::vector<vulkan_abi::ExtensionProperties> extensions(extension_count);
        if (extension_count > 0U) {
            const auto extension_result = enumerate_device_extensions(
                devices[index], nullptr, &extension_count, extensions.data());
            if (extension_result != vulkan_abi::result_success &&
                extension_result != vulkan_abi::result_incomplete) continue;
        }
        const bool has_acceleration = has_extension(
            extensions, "VK_KHR_acceleration_structure");
        const bool has_pipeline = has_extension(
            extensions, "VK_KHR_ray_tracing_pipeline");
        const bool has_deferred = has_extension(
            extensions, "VK_KHR_deferred_host_operations");
        const bool has_buffer_address = has_extension(
            extensions, "VK_KHR_buffer_device_address");
        result.acceleration_structure_extension |= has_acceleration;
        result.ray_tracing_pipeline_extension |= has_pipeline;
        result.deferred_host_operations_extension |= has_deferred;
        result.buffer_device_address_extension |= has_buffer_address;

        vulkan_abi::BufferDeviceAddressFeatures buffer_address{
            vulkan_abi::structure_type_buffer_device_address_features, nullptr, 0U, 0U, 0U};
        vulkan_abi::RayTracingPipelineFeatures pipeline{
            vulkan_abi::structure_type_ray_tracing_pipeline_features, &buffer_address,
            0U, 0U, 0U, 0U, 0U};
        vulkan_abi::AccelerationStructureFeatures acceleration{
            vulkan_abi::structure_type_acceleration_structure_features, &pipeline,
            0U, 0U, 0U, 0U, 0U};
        vulkan_abi::PhysicalDeviceFeatures2 features{
            vulkan_abi::structure_type_physical_device_features_2, &acceleration, {}};
        get_physical_device_features2(devices[index], &features);
        result.feature_query_completed = true;
        result.acceleration_structure_feature |= acceleration.acceleration_structure != 0U;
        result.ray_tracing_pipeline_feature |= pipeline.ray_tracing_pipeline != 0U;
        result.buffer_device_address_feature |= buffer_address.buffer_device_address != 0U;
        if (has_acceleration && has_pipeline && has_deferred && has_buffer_address &&
            acceleration.acceleration_structure != 0U &&
            pipeline.ray_tracing_pipeline != 0U &&
            buffer_address.buffer_device_address != 0U) {
            ++result.supported_device_count;
        }
    }
    destroy_instance(instance, nullptr);
    if (result.supported_device_count > 0U) {
        set_result(result, NativeRayTracingProbeState::supported,
                   "native-raytracing.vulkan-supported",
                   "A physical device exposed the required ray-tracing extensions and features.");
    } else if (result.device_count > 0U) {
        set_result(result, NativeRayTracingProbeState::unsupported,
                   "native-raytracing.vulkan-feature-unavailable",
                   "Physical devices were enumerated, but no device exposed complete ray-tracing support.");
    } else {
        set_result(result, NativeRayTracingProbeState::unsupported,
                   "native-raytracing.vulkan-no-physical-device",
                   "The Vulkan instance exposed no physical device for ray-tracing capability evaluation.");
    }
    return result;
}

} // namespace

std::string_view native_raytracing_backend_name(
    const NativeRayTracingBackend backend) noexcept {
    switch (backend) {
    case NativeRayTracingBackend::d3d12: return "d3d12";
    case NativeRayTracingBackend::vulkan: return "vulkan";
    }
    return "unknown";
}

std::string_view native_raytracing_probe_state_name(
    const NativeRayTracingProbeState state) noexcept {
    switch (state) {
    case NativeRayTracingProbeState::unavailable: return "unavailable";
    case NativeRayTracingProbeState::unsupported: return "unsupported";
    case NativeRayTracingProbeState::supported: return "supported";
    case NativeRayTracingProbeState::query_failed: return "query-failed";
    }
    return "query-failed";
}

NativeRayTracingCapability probe_native_raytracing_capability(
    const NativeRayTracingBackend backend) {
    return backend == NativeRayTracingBackend::d3d12
        ? probe_d3d12_raytracing_capability()
        : probe_vulkan_raytracing_capability();
}

NativeRayTracingCapability probe_d3d12_raytracing_capability() {
    return probe_d3d12_impl();
}

NativeRayTracingCapability probe_vulkan_raytracing_capability() {
    return probe_vulkan_impl();
}

} // namespace noemancer
