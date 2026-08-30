#include "runtime/native_vulkan_raytracing_executor.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <limits>
#include <optional>
#include <ranges>
#include <string>
#include <utility>
#include <vector>

#if __has_include(<vulkan/vulkan.h>)
#  include <vulkan/vulkan.h>
#  define NOEMANCER_HAS_VULKAN_HEADERS 1
#else
#  define NOEMANCER_HAS_VULKAN_HEADERS 0
#endif

#if defined(_WIN32)
#  define NOMINMAX
#  include <windows.h>
#else
#  include <dlfcn.h>
#endif

namespace noemancer {
namespace {

std::string bounded_text(const std::string_view value) {
    return std::string(value.substr(0U, native_vulkan_raytracing_executor_max_text_bytes));
}

void set_diagnostic(NativeVulkanRayTracingExecutionReceipt& receipt,
                    const NativeVulkanRayTracingExecutionState state,
                    const NativeVulkanRayTracingFailureStage stage,
                    const std::string_view code,
                    const std::string_view detail) {
    receipt.state = state;
    receipt.failure_stage = stage;
    receipt.code = bounded_text(code);
    receipt.detail = bounded_text(detail);
}

std::string result_detail(const std::string_view operation, const std::int32_t result) {
    return std::string(operation) + " failed with VkResult=" + std::to_string(result) + ".";
}

std::uint64_t saturating_add(const std::uint64_t left, const std::uint64_t right) {
    if (left > std::numeric_limits<std::uint64_t>::max() - right)
        return std::numeric_limits<std::uint64_t>::max();
    return left + right;
}

#if NOEMANCER_HAS_VULKAN_HEADERS

// Built from the bounded source used by this probe with the repository DXC
// 1.9.2607 toolchain:
// dxc -spirv -fspv-target-env=vulkan1.1spirv1.4 -T lib_6_3 -E RayGen
// The module contains RayGen, Miss and ClosestHit entry points.  It writes a
// four-byte hit/miss marker through set 0 binding 1 after tracing one ray.
// Keeping this tiny fixture embedded makes the probe independent of a build
// directory and prevents an untracked shader path from becoming runtime API.
constexpr std::array<std::uint32_t, 349U> native_raytracing_probe_spirv{
    0x07230203U, 0x00010400U, 0x000E0000U, 0x0000002FU, 0x00000000U, 0x00020011U, 0x00001178U, 0x00020011U,
    0x0000117FU, 0x0006000AU, 0x5F565053U, 0x5F52484BU, 0x5F796172U, 0x72657571U, 0x00000079U, 0x0006000AU,
    0x5F565053U, 0x5F52484BU, 0x5F796172U, 0x63617274U, 0x00676E69U, 0x0003000EU, 0x00000000U, 0x00000001U,
    0x0008000FU, 0x000014C1U, 0x00000001U, 0x47796152U, 0x00006E65U, 0x00000002U, 0x00000003U, 0x00000004U,
    0x0008000FU, 0x000014C5U, 0x00000005U, 0x7373694DU, 0x00000000U, 0x00000002U, 0x00000003U, 0x00000006U,
    0x0009000FU, 0x000014C4U, 0x00000007U, 0x736F6C43U, 0x48747365U, 0x00007469U, 0x00000002U, 0x00000003U,
    0x00000008U, 0x00030003U, 0x00000005U, 0x00000276U, 0x00080005U, 0x00000009U, 0x65636361U, 0x6172656CU,
    0x6E6F6974U, 0x75727453U, 0x72757463U, 0x00564E65U, 0x00040005U, 0x00000002U, 0x6E656353U, 0x00000065U,
    0x000A0005U, 0x0000000AU, 0x65707974U, 0x5357522EU, 0x63757274U, 0x65727574U, 0x66754264U, 0x2E726566U,
    0x746E6975U, 0x00000000U, 0x00040005U, 0x00000003U, 0x7074754FU, 0x00007475U, 0x00040005U, 0x0000000BU,
    0x6C796150U, 0x0064616FU, 0x00040006U, 0x0000000BU, 0x00000000U, 0x00746968U, 0x00030005U, 0x00000004U,
    0x00000070U, 0x00030005U, 0x00000006U, 0x00000070U, 0x00030005U, 0x00000008U, 0x00000070U, 0x00040005U,
    0x00000001U, 0x47796152U, 0x00006E65U, 0x00040005U, 0x00000005U, 0x7373694DU, 0x00000000U, 0x00050005U,
    0x00000007U, 0x736F6C43U, 0x48747365U, 0x00007469U, 0x00040047U, 0x00000004U, 0x0000001EU, 0x00000000U,
    0x00040047U, 0x00000002U, 0x00000022U, 0x00000000U, 0x00040047U, 0x00000002U, 0x00000021U, 0x00000000U,
    0x00040047U, 0x00000003U, 0x00000022U, 0x00000000U, 0x00040047U, 0x00000003U, 0x00000021U, 0x00000001U,
    0x00040047U, 0x0000000CU, 0x00000006U, 0x00000004U, 0x00050048U, 0x0000000AU, 0x00000000U, 0x00000023U,
    0x00000000U, 0x00030047U, 0x0000000AU, 0x00000002U, 0x00040015U, 0x0000000DU, 0x00000020U, 0x00000000U,
    0x0004002BU, 0x0000000DU, 0x0000000EU, 0x00000000U, 0x00040015U, 0x0000000FU, 0x00000020U, 0x00000001U,
    0x0004002BU, 0x0000000FU, 0x00000010U, 0x00000000U, 0x00030016U, 0x00000011U, 0x00000020U, 0x0004002BU,
    0x00000011U, 0x00000012U, 0x00000000U, 0x0004002BU, 0x00000011U, 0x00000013U, 0xC0000000U, 0x00040017U,
    0x00000014U, 0x00000011U, 0x00000003U, 0x0006002CU, 0x00000014U, 0x00000015U, 0x00000012U, 0x00000012U,
    0x00000013U, 0x0004002BU, 0x00000011U, 0x00000016U, 0x3F800000U, 0x0006002CU, 0x00000014U, 0x00000017U,
    0x00000012U, 0x00000012U, 0x00000016U, 0x0004002BU, 0x00000011U, 0x00000018U, 0x41200000U, 0x0004002BU,
    0x0000000DU, 0x00000019U, 0x000000FFU, 0x0004002BU, 0x0000000DU, 0x0000001AU, 0x00000001U, 0x0004002BU,
    0x0000000DU, 0x0000001BU, 0x4D495353U, 0x0004002BU, 0x0000000DU, 0x0000001CU, 0x48495421U, 0x000214DDU,
    0x00000009U, 0x00040020U, 0x0000001DU, 0x00000000U, 0x00000009U, 0x0003001DU, 0x0000000CU, 0x0000000DU,
    0x0003001EU, 0x0000000AU, 0x0000000CU, 0x00040020U, 0x0000001EU, 0x0000000CU, 0x0000000AU, 0x0003001EU,
    0x0000000BU, 0x0000000DU, 0x00040020U, 0x0000001FU, 0x000014DAU, 0x0000000BU, 0x00040020U, 0x00000020U,
    0x000014DEU, 0x0000000BU, 0x00020013U, 0x00000021U, 0x00030021U, 0x00000022U, 0x00000021U, 0x00040020U,
    0x00000023U, 0x0000000CU, 0x0000000DU, 0x0004003BU, 0x0000001DU, 0x00000002U, 0x00000000U, 0x0004003BU,
    0x0000001EU, 0x00000003U, 0x0000000CU, 0x0004003BU, 0x0000001FU, 0x00000004U, 0x000014DAU, 0x0004003BU,
    0x00000020U, 0x00000006U, 0x000014DEU, 0x0004003BU, 0x00000020U, 0x00000008U, 0x000014DEU, 0x0004002CU,
    0x0000000BU, 0x00000024U, 0x0000000EU, 0x0004002CU, 0x0000000BU, 0x00000025U, 0x0000001BU, 0x0004002CU,
    0x0000000BU, 0x00000026U, 0x0000001CU, 0x00040020U, 0x00000027U, 0x000014DAU, 0x0000000DU, 0x00050036U,
    0x00000021U, 0x00000001U, 0x00000000U, 0x00000022U, 0x000200F8U, 0x00000028U, 0x0003003EU, 0x00000004U,
    0x00000024U, 0x0004003DU, 0x00000009U, 0x00000029U, 0x00000002U, 0x000C115DU, 0x00000029U, 0x0000000EU,
    0x00000019U, 0x0000000EU, 0x0000001AU, 0x0000000EU, 0x00000015U, 0x00000012U, 0x00000017U, 0x00000018U,
    0x00000004U, 0x00050041U, 0x00000027U, 0x0000002AU, 0x00000004U, 0x0000000EU, 0x0004003DU, 0x0000000DU,
    0x0000002BU, 0x0000002AU, 0x00060041U, 0x00000023U,
    0x0000002CU, 0x00000003U, 0x00000010U, 0x0000000EU, 0x0003003EU, 0x0000002CU, 0x0000002BU, 0x000100FDU,
    0x00010038U, 0x00050036U, 0x00000021U, 0x00000005U, 0x00000000U, 0x00000022U, 0x000200F8U, 0x0000002DU,
    0x0003003EU, 0x00000006U, 0x00000025U, 0x000100FDU, 0x00010038U, 0x00050036U, 0x00000021U, 0x00000007U,
    0x00000000U, 0x00000022U, 0x000200F8U, 0x0000002EU, 0x0003003EU, 0x00000008U, 0x00000026U, 0x000100FDU,
    0x00010038U,
};

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
Function load_global(const VulkanModule& module,
                     const PFN_vkGetInstanceProcAddr get_instance_proc_addr,
                     const char* name) {
    auto function = module.symbol(name);
    if (function == nullptr && get_instance_proc_addr != nullptr)
        function = reinterpret_cast<void*>(get_instance_proc_addr(VK_NULL_HANDLE, name));
    return reinterpret_cast<Function>(function);
}

template <typename Function>
Function load_instance(const PFN_vkGetInstanceProcAddr get_instance_proc_addr,
                       const VkInstance instance, const char* name) {
    return reinterpret_cast<Function>(get_instance_proc_addr(instance, name));
}

template <typename Function>
Function load_device(const PFN_vkGetDeviceProcAddr get_device_proc_addr,
                     const VkDevice device, const char* name) {
    return reinterpret_cast<Function>(get_device_proc_addr(device, name));
}

struct InstanceFunctions final {
    PFN_vkDestroyInstance destroy_instance{};
    PFN_vkEnumeratePhysicalDevices enumerate_physical_devices{};
    PFN_vkEnumerateDeviceExtensionProperties enumerate_device_extension_properties{};
    PFN_vkGetPhysicalDeviceFeatures2 get_physical_device_features2{};
    PFN_vkGetPhysicalDeviceProperties2 get_physical_device_properties2{};
    PFN_vkGetPhysicalDeviceProperties get_physical_device_properties{};
    PFN_vkGetPhysicalDeviceMemoryProperties get_physical_device_memory_properties{};
    PFN_vkGetPhysicalDeviceQueueFamilyProperties get_physical_device_queue_family_properties{};
    PFN_vkGetDeviceProcAddr get_device_proc_addr{};
};

struct DeviceFunctions final {
    PFN_vkGetDeviceQueue get_device_queue{};
    PFN_vkDestroyDevice destroy_device{};
    PFN_vkDeviceWaitIdle device_wait_idle{};
    PFN_vkCreateBuffer create_buffer{};
    PFN_vkDestroyBuffer destroy_buffer{};
    PFN_vkGetBufferMemoryRequirements get_buffer_memory_requirements{};
    PFN_vkAllocateMemory allocate_memory{};
    PFN_vkFreeMemory free_memory{};
    PFN_vkBindBufferMemory bind_buffer_memory{};
    PFN_vkMapMemory map_memory{};
    PFN_vkUnmapMemory unmap_memory{};
    PFN_vkGetBufferDeviceAddress get_buffer_device_address{};
    PFN_vkCreateShaderModule create_shader_module{};
    PFN_vkDestroyShaderModule destroy_shader_module{};
    PFN_vkCreateDescriptorSetLayout create_descriptor_set_layout{};
    PFN_vkDestroyDescriptorSetLayout destroy_descriptor_set_layout{};
    PFN_vkCreatePipelineLayout create_pipeline_layout{};
    PFN_vkDestroyPipelineLayout destroy_pipeline_layout{};
    PFN_vkCreateDescriptorPool create_descriptor_pool{};
    PFN_vkDestroyDescriptorPool destroy_descriptor_pool{};
    PFN_vkAllocateDescriptorSets allocate_descriptor_sets{};
    PFN_vkUpdateDescriptorSets update_descriptor_sets{};
    PFN_vkCreateAccelerationStructureKHR create_acceleration_structure{};
    PFN_vkDestroyAccelerationStructureKHR destroy_acceleration_structure{};
    PFN_vkGetAccelerationStructureBuildSizesKHR get_acceleration_structure_build_sizes{};
    PFN_vkGetAccelerationStructureDeviceAddressKHR get_acceleration_structure_device_address{};
    PFN_vkCmdBuildAccelerationStructuresKHR cmd_build_acceleration_structures{};
    PFN_vkCreateCommandPool create_command_pool{};
    PFN_vkDestroyCommandPool destroy_command_pool{};
    PFN_vkAllocateCommandBuffers allocate_command_buffers{};
    PFN_vkBeginCommandBuffer begin_command_buffer{};
    PFN_vkEndCommandBuffer end_command_buffer{};
    PFN_vkCmdPipelineBarrier cmd_pipeline_barrier{};
    PFN_vkCreateFence create_fence{};
    PFN_vkDestroyFence destroy_fence{};
    PFN_vkQueueSubmit queue_submit{};
    PFN_vkWaitForFences wait_for_fences{};
    PFN_vkCreateRayTracingPipelinesKHR create_ray_tracing_pipelines{};
    PFN_vkDestroyPipeline destroy_pipeline{};
    PFN_vkGetRayTracingShaderGroupHandlesKHR get_ray_tracing_shader_group_handles{};
    PFN_vkCmdBindPipeline cmd_bind_pipeline{};
    PFN_vkCmdBindDescriptorSets cmd_bind_descriptor_sets{};
    PFN_vkCmdTraceRaysKHR cmd_trace_rays{};
    PFN_vkCreateQueryPool create_query_pool{};
    PFN_vkDestroyQueryPool destroy_query_pool{};
    PFN_vkCmdWriteTimestamp cmd_write_timestamp{};
    PFN_vkGetQueryPoolResults get_query_pool_results{};
};

struct BufferResource final {
    VkBuffer buffer{};
    VkDeviceMemory memory{};
    VkDeviceSize allocation_size{};
    VkDeviceAddress device_address{};
};

struct AccelerationStructureResource final {
    VkAccelerationStructureKHR acceleration_structure{};
    VkDeviceAddress device_address{};
    VkDeviceSize size{};
};

struct SelectedPhysicalDevice final {
    VkPhysicalDevice physical_device{};
    VkPhysicalDeviceProperties properties{};
    VkPhysicalDeviceMemoryProperties memory_properties{};
    VkPhysicalDeviceAccelerationStructurePropertiesKHR acceleration_properties{};
    VkPhysicalDeviceRayTracingPipelinePropertiesKHR ray_tracing_properties{};
    std::uint32_t queue_family_index{};
};

bool has_extension(const std::vector<VkExtensionProperties>& extensions,
                   const char* name) {
    return std::ranges::any_of(extensions, [name](const VkExtensionProperties& extension) {
        return std::strcmp(extension.extensionName, name) == 0;
    });
}

std::optional<std::uint32_t> find_memory_type(
    const VkPhysicalDeviceMemoryProperties& properties,
    const std::uint32_t type_bits, const VkMemoryPropertyFlags required) {
    for (std::uint32_t index = 0U; index < properties.memoryTypeCount; ++index) {
        if ((type_bits & (1U << index)) != 0U &&
            (properties.memoryTypes[index].propertyFlags & required) == required)
            return index;
    }
    return std::nullopt;
}

VkDeviceAddress align_device_address(const VkDeviceAddress address,
                                     const VkDeviceSize alignment) {
    if (alignment <= 1U) return address;
    const auto remainder = address % alignment;
    return remainder == 0U ? address : address + alignment - remainder;
}

bool require_instance_functions(const InstanceFunctions& functions) {
    return functions.destroy_instance != nullptr &&
        functions.enumerate_physical_devices != nullptr &&
        functions.enumerate_device_extension_properties != nullptr &&
        functions.get_physical_device_features2 != nullptr &&
        functions.get_physical_device_properties2 != nullptr &&
        functions.get_physical_device_memory_properties != nullptr &&
        functions.get_physical_device_queue_family_properties != nullptr &&
        functions.get_device_proc_addr != nullptr;
}

bool require_device_functions(const DeviceFunctions& functions) {
    return functions.get_device_queue != nullptr && functions.destroy_device != nullptr &&
        functions.device_wait_idle != nullptr && functions.create_buffer != nullptr &&
        functions.destroy_buffer != nullptr && functions.get_buffer_memory_requirements != nullptr &&
        functions.allocate_memory != nullptr && functions.free_memory != nullptr &&
        functions.bind_buffer_memory != nullptr && functions.map_memory != nullptr &&
        functions.unmap_memory != nullptr && functions.get_buffer_device_address != nullptr &&
        functions.create_shader_module != nullptr && functions.destroy_shader_module != nullptr &&
        functions.create_descriptor_set_layout != nullptr &&
        functions.destroy_descriptor_set_layout != nullptr &&
        functions.create_pipeline_layout != nullptr && functions.destroy_pipeline_layout != nullptr &&
        functions.create_descriptor_pool != nullptr && functions.destroy_descriptor_pool != nullptr &&
        functions.allocate_descriptor_sets != nullptr && functions.update_descriptor_sets != nullptr &&
        functions.create_acceleration_structure != nullptr &&
        functions.destroy_acceleration_structure != nullptr &&
        functions.get_acceleration_structure_build_sizes != nullptr &&
        functions.get_acceleration_structure_device_address != nullptr &&
        functions.cmd_build_acceleration_structures != nullptr &&
        functions.create_command_pool != nullptr && functions.destroy_command_pool != nullptr &&
        functions.allocate_command_buffers != nullptr && functions.begin_command_buffer != nullptr &&
        functions.end_command_buffer != nullptr && functions.cmd_pipeline_barrier != nullptr &&
        functions.create_fence != nullptr && functions.destroy_fence != nullptr &&
        functions.queue_submit != nullptr && functions.wait_for_fences != nullptr &&
        functions.create_ray_tracing_pipelines != nullptr && functions.destroy_pipeline != nullptr &&
        functions.get_ray_tracing_shader_group_handles != nullptr &&
        functions.cmd_bind_pipeline != nullptr && functions.cmd_bind_descriptor_sets != nullptr &&
        functions.cmd_trace_rays != nullptr && functions.create_query_pool != nullptr &&
        functions.destroy_query_pool != nullptr && functions.cmd_write_timestamp != nullptr &&
        functions.get_query_pool_results != nullptr;
}

bool create_buffer(const DeviceFunctions& functions, const VkDevice device,
                   const VkPhysicalDeviceMemoryProperties& memory_properties,
                   const VkDeviceSize size, const VkBufferUsageFlags usage,
                   const VkMemoryPropertyFlags memory_flags, const void* initial_data,
                   const std::size_t initial_data_bytes, BufferResource& result,
                   std::string& error_code, std::string& error_detail) {
    VkBufferCreateInfo create_info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    create_info.size = size;
    create_info.usage = usage;
    create_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (functions.create_buffer(device, &create_info, nullptr, &result.buffer) != VK_SUCCESS) {
        error_code = "native-vulkan-rt.create-buffer-failed";
        error_detail = "vkCreateBuffer failed.";
        return false;
    }
    VkMemoryRequirements requirements{};
    functions.get_buffer_memory_requirements(device, result.buffer, &requirements);
    const auto memory_type = find_memory_type(memory_properties, requirements.memoryTypeBits,
                                               memory_flags);
    if (!memory_type) {
        error_code = "native-vulkan-rt.memory-type-unavailable";
        error_detail = "No compatible memory type satisfied the requested buffer flags.";
        return false;
    }
    VkMemoryAllocateFlagsInfo address_flags{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO};
    address_flags.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
    VkMemoryAllocateInfo allocate_info{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocate_info.pNext = &address_flags;
    allocate_info.allocationSize = requirements.size;
    allocate_info.memoryTypeIndex = *memory_type;
    if (functions.allocate_memory(device, &allocate_info, nullptr, &result.memory) != VK_SUCCESS) {
        error_code = "native-vulkan-rt.allocate-buffer-memory-failed";
        error_detail = "vkAllocateMemory failed for a buffer.";
        return false;
    }
    result.allocation_size = requirements.size;
    if (functions.bind_buffer_memory(device, result.buffer, result.memory, 0U) != VK_SUCCESS) {
        error_code = "native-vulkan-rt.bind-buffer-memory-failed";
        error_detail = "vkBindBufferMemory failed.";
        return false;
    }
    if (initial_data != nullptr && initial_data_bytes > 0U) {
        void* mapped = nullptr;
        if (functions.map_memory(device, result.memory, 0U, initial_data_bytes, 0U, &mapped) != VK_SUCCESS ||
            mapped == nullptr) {
            error_code = "native-vulkan-rt.map-buffer-memory-failed";
            error_detail = "vkMapMemory failed for host-visible geometry data.";
            return false;
        }
        std::memcpy(mapped, initial_data, initial_data_bytes);
        functions.unmap_memory(device, result.memory);
    }
    VkBufferDeviceAddressInfo address_info{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
    address_info.buffer = result.buffer;
    result.device_address = functions.get_buffer_device_address(device, &address_info);
    if (result.device_address == 0U) {
        error_code = "native-vulkan-rt.buffer-device-address-unavailable";
        error_detail = "vkGetBufferDeviceAddress returned zero.";
        return false;
    }
    return true;
}

bool create_acceleration_structure(
    const DeviceFunctions& functions, const VkDevice device, const VkBuffer result_buffer,
    const VkDeviceSize size, const VkAccelerationStructureTypeKHR type,
    AccelerationStructureResource& result, std::string& error_code,
    std::string& error_detail) {
    VkAccelerationStructureCreateInfoKHR create_info{
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR};
    create_info.buffer = result_buffer;
    create_info.size = size;
    create_info.type = type;
    if (functions.create_acceleration_structure(device, &create_info, nullptr,
                                                &result.acceleration_structure) != VK_SUCCESS) {
        error_code = "native-vulkan-rt.create-acceleration-structure-failed";
        error_detail = "vkCreateAccelerationStructureKHR failed.";
        return false;
    }
    result.size = size;
    VkAccelerationStructureDeviceAddressInfoKHR address_info{
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR};
    address_info.accelerationStructure = result.acceleration_structure;
    result.device_address = functions.get_acceleration_structure_device_address(device,
                                                                                 &address_info);
    if (result.device_address == 0U) {
        error_code = "native-vulkan-rt.acceleration-structure-address-unavailable";
        error_detail = "vkGetAccelerationStructureDeviceAddressKHR returned zero.";
        return false;
    }
    return true;
}

NativeVulkanRayTracingExecutionReceipt execute_impl() {
    NativeVulkanRayTracingExecutionReceipt receipt;
    receipt.geometry_count = 1U;
    receipt.instance_count = 1U;

    VulkanModule module;
    if (!module.load()) {
        receipt.resources_released = true;
        set_diagnostic(receipt, NativeVulkanRayTracingExecutionState::unavailable,
                       NativeVulkanRayTracingFailureStage::loader,
                       "native-vulkan-rt.loader-unavailable",
                       "The Vulkan loader could not be loaded.");
        return receipt;
    }
    receipt.loader_available = true;

    const auto get_instance_proc_addr = reinterpret_cast<PFN_vkGetInstanceProcAddr>(
        module.symbol("vkGetInstanceProcAddr"));
    if (get_instance_proc_addr == nullptr) {
        receipt.resources_released = true;
        set_diagnostic(receipt, NativeVulkanRayTracingExecutionState::unavailable,
                       NativeVulkanRayTracingFailureStage::loader,
                       "native-vulkan-rt.entrypoint-unavailable",
                       "vkGetInstanceProcAddr is missing from the Vulkan loader.");
        return receipt;
    }
    const auto enumerate_instance_version = load_global<PFN_vkEnumerateInstanceVersion>(
        module, get_instance_proc_addr, "vkEnumerateInstanceVersion");
    std::uint32_t loader_api = VK_API_VERSION_1_0;
    if (enumerate_instance_version != nullptr) {
        std::uint32_t queried_api = VK_API_VERSION_1_0;
        if (enumerate_instance_version(&queried_api) == VK_SUCCESS) loader_api = queried_api;
    }
    const auto requested_api = loader_api >= VK_API_VERSION_1_1
        ? VK_API_VERSION_1_1 : VK_API_VERSION_1_0;
    VkApplicationInfo application_info{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    application_info.pApplicationName = "Noemancer RT Probe";
    application_info.applicationVersion = VK_MAKE_VERSION(0, 1, 0);
    application_info.pEngineName = "Noemancer";
    application_info.engineVersion = VK_MAKE_VERSION(0, 1, 0);
    application_info.apiVersion = requested_api;
    VkInstanceCreateInfo instance_create_info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    instance_create_info.pApplicationInfo = &application_info;
    const auto create_instance = load_global<PFN_vkCreateInstance>(
        module, get_instance_proc_addr, "vkCreateInstance");
    if (create_instance == nullptr) {
        receipt.resources_released = true;
        set_diagnostic(receipt, NativeVulkanRayTracingExecutionState::unavailable,
                       NativeVulkanRayTracingFailureStage::loader,
                       "native-vulkan-rt.create-instance-entrypoint-unavailable",
                       "vkCreateInstance is missing from the Vulkan loader.");
        return receipt;
    }
    VkInstance instance{};
    const auto create_instance_result = create_instance(&instance_create_info, nullptr, &instance);
    if (create_instance_result != VK_SUCCESS || instance == VK_NULL_HANDLE) {
        receipt.resources_released = true;
        set_diagnostic(receipt, NativeVulkanRayTracingExecutionState::failed,
                       NativeVulkanRayTracingFailureStage::instance,
                       "native-vulkan-rt.create-instance-failed",
                       result_detail("vkCreateInstance", create_instance_result));
        return receipt;
    }
    receipt.instance_created = true;
    receipt.resources_released = false;

    InstanceFunctions instance_functions;
    instance_functions.destroy_instance = load_instance<PFN_vkDestroyInstance>(
        get_instance_proc_addr, instance, "vkDestroyInstance");
    instance_functions.enumerate_physical_devices = load_instance<PFN_vkEnumeratePhysicalDevices>(
        get_instance_proc_addr, instance, "vkEnumeratePhysicalDevices");
    instance_functions.enumerate_device_extension_properties =
        load_instance<PFN_vkEnumerateDeviceExtensionProperties>(
            get_instance_proc_addr, instance, "vkEnumerateDeviceExtensionProperties");
    instance_functions.get_physical_device_features2 = load_instance<PFN_vkGetPhysicalDeviceFeatures2>(
        get_instance_proc_addr, instance, "vkGetPhysicalDeviceFeatures2");
    if (instance_functions.get_physical_device_features2 == nullptr)
        instance_functions.get_physical_device_features2 =
            load_instance<PFN_vkGetPhysicalDeviceFeatures2KHR>(
                get_instance_proc_addr, instance, "vkGetPhysicalDeviceFeatures2KHR");
    instance_functions.get_physical_device_properties2 =
        load_instance<PFN_vkGetPhysicalDeviceProperties2>(
            get_instance_proc_addr, instance, "vkGetPhysicalDeviceProperties2");
    if (instance_functions.get_physical_device_properties2 == nullptr)
        instance_functions.get_physical_device_properties2 =
            load_instance<PFN_vkGetPhysicalDeviceProperties2KHR>(
                get_instance_proc_addr, instance, "vkGetPhysicalDeviceProperties2KHR");
    instance_functions.get_physical_device_properties = load_instance<PFN_vkGetPhysicalDeviceProperties>(
        get_instance_proc_addr, instance, "vkGetPhysicalDeviceProperties");
    instance_functions.get_physical_device_memory_properties =
        load_instance<PFN_vkGetPhysicalDeviceMemoryProperties>(
            get_instance_proc_addr, instance, "vkGetPhysicalDeviceMemoryProperties");
    instance_functions.get_physical_device_queue_family_properties =
        load_instance<PFN_vkGetPhysicalDeviceQueueFamilyProperties>(
            get_instance_proc_addr, instance, "vkGetPhysicalDeviceQueueFamilyProperties");
    instance_functions.get_device_proc_addr = load_instance<PFN_vkGetDeviceProcAddr>(
        get_instance_proc_addr, instance, "vkGetDeviceProcAddr");
    if (!require_instance_functions(instance_functions)) {
        instance_functions.destroy_instance(instance, nullptr);
        receipt.instance_created = false;
        receipt.resources_released = true;
        set_diagnostic(receipt, NativeVulkanRayTracingExecutionState::unavailable,
                       NativeVulkanRayTracingFailureStage::instance,
                       "native-vulkan-rt.instance-query-entrypoint-unavailable",
                       "The Vulkan instance lacks a required physical-device query entry point.");
        return receipt;
    }

    std::uint32_t physical_device_count = 0U;
    auto enumerate_result = instance_functions.enumerate_physical_devices(
        instance, &physical_device_count, nullptr);
    if (enumerate_result != VK_SUCCESS && enumerate_result != VK_INCOMPLETE) {
        instance_functions.destroy_instance(instance, nullptr);
        receipt.instance_created = false;
        receipt.resources_released = true;
        set_diagnostic(receipt, NativeVulkanRayTracingExecutionState::failed,
                       NativeVulkanRayTracingFailureStage::physical_device,
                       "native-vulkan-rt.enumerate-physical-devices-failed",
                       result_detail("vkEnumeratePhysicalDevices", enumerate_result));
        return receipt;
    }
    physical_device_count = std::min(physical_device_count, 64U);
    std::vector<VkPhysicalDevice> physical_devices(physical_device_count);
    if (physical_device_count > 0U) {
        enumerate_result = instance_functions.enumerate_physical_devices(
            instance, &physical_device_count, physical_devices.data());
        if (enumerate_result != VK_SUCCESS && enumerate_result != VK_INCOMPLETE) {
            instance_functions.destroy_instance(instance, nullptr);
            receipt.instance_created = false;
            receipt.resources_released = true;
            set_diagnostic(receipt, NativeVulkanRayTracingExecutionState::failed,
                           NativeVulkanRayTracingFailureStage::physical_device,
                           "native-vulkan-rt.enumerate-physical-devices-failed",
                           result_detail("vkEnumeratePhysicalDevices", enumerate_result));
            return receipt;
        }
    }
    receipt.physical_device_count = physical_device_count;
    SelectedPhysicalDevice selected;
    bool found_supported_device = false;
    for (std::uint32_t index = 0U; index < physical_device_count && !found_supported_device; ++index) {
        const auto physical_device = physical_devices[index];
        std::uint32_t extension_count = 0U;
        auto extension_result = instance_functions.enumerate_device_extension_properties(
            physical_device, nullptr, &extension_count, nullptr);
        if (extension_result != VK_SUCCESS && extension_result != VK_INCOMPLETE) continue;
        extension_count = std::min(extension_count, 256U);
        std::vector<VkExtensionProperties> extensions(extension_count);
        if (extension_count > 0U) {
            extension_result = instance_functions.enumerate_device_extension_properties(
                physical_device, nullptr, &extension_count, extensions.data());
            if (extension_result != VK_SUCCESS && extension_result != VK_INCOMPLETE) continue;
        }
        if (!has_extension(extensions, VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME) ||
            !has_extension(extensions, VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME) ||
            !has_extension(extensions, VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME) ||
            !has_extension(extensions, VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME) ||
            !has_extension(extensions, VK_KHR_SPIRV_1_4_EXTENSION_NAME) ||
            !has_extension(extensions, VK_KHR_SHADER_FLOAT_CONTROLS_EXTENSION_NAME)) continue;

        VkPhysicalDeviceBufferDeviceAddressFeatures buffer_address{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES};
        VkPhysicalDeviceRayTracingPipelineFeaturesKHR ray_tracing_pipeline{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR};
        VkPhysicalDeviceAccelerationStructureFeaturesKHR acceleration_structure{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR};
        ray_tracing_pipeline.pNext = &acceleration_structure;
        acceleration_structure.pNext = &buffer_address;
        VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
        features.pNext = &ray_tracing_pipeline;
        instance_functions.get_physical_device_features2(physical_device, &features);
        if (buffer_address.bufferDeviceAddress != VK_TRUE ||
            acceleration_structure.accelerationStructure != VK_TRUE ||
            ray_tracing_pipeline.rayTracingPipeline != VK_TRUE) continue;

        std::uint32_t queue_family_count = 0U;
        instance_functions.get_physical_device_queue_family_properties(
            physical_device, &queue_family_count, nullptr);
        std::vector<VkQueueFamilyProperties> queue_families(queue_family_count);
        if (queue_family_count > 0U)
            instance_functions.get_physical_device_queue_family_properties(
                physical_device, &queue_family_count, queue_families.data());
        std::optional<std::uint32_t> queue_family;
        for (std::uint32_t family = 0U; family < queue_family_count; ++family) {
            if ((queue_families[family].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0U &&
                (queue_families[family].queueFlags & VK_QUEUE_COMPUTE_BIT) != 0U) {
                queue_family = family;
                break;
            }
        }
        if (!queue_family) {
            for (std::uint32_t family = 0U; family < queue_family_count; ++family) {
                if ((queue_families[family].queueFlags & VK_QUEUE_COMPUTE_BIT) != 0U) {
                    queue_family = family;
                    break;
                }
            }
        }
        if (!queue_family) continue;

        selected.physical_device = physical_device;
        selected.queue_family_index = *queue_family;
        selected.properties = {};
        instance_functions.get_physical_device_properties(physical_device, &selected.properties);
        selected.memory_properties = {};
        instance_functions.get_physical_device_memory_properties(
            physical_device, &selected.memory_properties);
        selected.acceleration_properties = {
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR};
        selected.ray_tracing_properties = {
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_PROPERTIES_KHR};
        selected.ray_tracing_properties.pNext = &selected.acceleration_properties;
        VkPhysicalDeviceProperties2 properties2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
        properties2.pNext = &selected.ray_tracing_properties;
        instance_functions.get_physical_device_properties2(physical_device, &properties2);
        found_supported_device = true;
    }
    if (!found_supported_device) {
        instance_functions.destroy_instance(instance, nullptr);
        receipt.instance_created = false;
        receipt.resources_released = true;
        set_diagnostic(receipt, NativeVulkanRayTracingExecutionState::unsupported,
                       NativeVulkanRayTracingFailureStage::physical_device,
                       "native-vulkan-rt.ray-tracing-unsupported",
                       "No physical device exposed the required RT extensions, features, and compute queue.");
        return receipt;
    }
    receipt.device_name = bounded_text(selected.properties.deviceName);
    receipt.vendor_id = selected.properties.vendorID;
    receipt.device_id = selected.properties.deviceID;
    receipt.api_version_major = VK_VERSION_MAJOR(selected.properties.apiVersion);
    receipt.api_version_minor = VK_VERSION_MINOR(selected.properties.apiVersion);
    receipt.api_version_patch = VK_VERSION_PATCH(selected.properties.apiVersion);
    receipt.queue_family_index = selected.queue_family_index;

    const float queue_priority = 1.0F;
    VkDeviceQueueCreateInfo queue_create_info{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    queue_create_info.queueFamilyIndex = selected.queue_family_index;
    queue_create_info.queueCount = 1U;
    queue_create_info.pQueuePriorities = &queue_priority;
    const std::array<const char*, 6U> device_extensions{
        VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME,
        VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME,
        VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME,
        VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME,
        VK_KHR_SPIRV_1_4_EXTENSION_NAME,
        VK_KHR_SHADER_FLOAT_CONTROLS_EXTENSION_NAME};
    VkPhysicalDeviceBufferDeviceAddressFeatures enabled_buffer_address{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES};
    enabled_buffer_address.bufferDeviceAddress = VK_TRUE;
    VkPhysicalDeviceRayTracingPipelineFeaturesKHR enabled_ray_tracing_pipeline{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR};
    VkPhysicalDeviceAccelerationStructureFeaturesKHR enabled_acceleration_structure{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR};
    enabled_ray_tracing_pipeline.pNext = &enabled_acceleration_structure;
    enabled_acceleration_structure.pNext = &enabled_buffer_address;
    enabled_ray_tracing_pipeline.rayTracingPipeline = VK_TRUE;
    enabled_acceleration_structure.accelerationStructure = VK_TRUE;
    VkPhysicalDeviceFeatures2 enabled_features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    enabled_features.pNext = &enabled_ray_tracing_pipeline;
    VkDeviceCreateInfo device_create_info{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    device_create_info.pNext = &enabled_features;
    device_create_info.queueCreateInfoCount = 1U;
    device_create_info.pQueueCreateInfos = &queue_create_info;
    device_create_info.enabledExtensionCount = static_cast<std::uint32_t>(device_extensions.size());
    device_create_info.ppEnabledExtensionNames = device_extensions.data();
    VkDevice device{};
    auto create_device = load_instance<PFN_vkCreateDevice>(
        get_instance_proc_addr, instance, "vkCreateDevice");
    if (create_device == nullptr) {
        instance_functions.destroy_instance(instance, nullptr);
        receipt.instance_created = false;
        receipt.resources_released = true;
        set_diagnostic(receipt, NativeVulkanRayTracingExecutionState::unavailable,
                       NativeVulkanRayTracingFailureStage::device,
                       "native-vulkan-rt.create-device-entrypoint-unavailable",
                       "vkCreateDevice is missing from the Vulkan instance.");
        return receipt;
    }
    const auto create_device_result = create_device(
        selected.physical_device, &device_create_info, nullptr, &device);
    if (create_device_result != VK_SUCCESS || device == VK_NULL_HANDLE) {
        instance_functions.destroy_instance(instance, nullptr);
        receipt.instance_created = false;
        receipt.resources_released = true;
        set_diagnostic(receipt, NativeVulkanRayTracingExecutionState::failed,
                       NativeVulkanRayTracingFailureStage::device,
                       "native-vulkan-rt.create-device-failed",
                       result_detail("vkCreateDevice", create_device_result));
        return receipt;
    }
    receipt.device_created = true;
    receipt.feature_chain_enabled = true;

    DeviceFunctions functions;
    functions.get_device_queue = load_device<PFN_vkGetDeviceQueue>(
        instance_functions.get_device_proc_addr, device, "vkGetDeviceQueue");
    functions.destroy_device = load_device<PFN_vkDestroyDevice>(
        instance_functions.get_device_proc_addr, device, "vkDestroyDevice");
    functions.device_wait_idle = load_device<PFN_vkDeviceWaitIdle>(
        instance_functions.get_device_proc_addr, device, "vkDeviceWaitIdle");
    functions.create_buffer = load_device<PFN_vkCreateBuffer>(
        instance_functions.get_device_proc_addr, device, "vkCreateBuffer");
    functions.destroy_buffer = load_device<PFN_vkDestroyBuffer>(
        instance_functions.get_device_proc_addr, device, "vkDestroyBuffer");
    functions.get_buffer_memory_requirements = load_device<PFN_vkGetBufferMemoryRequirements>(
        instance_functions.get_device_proc_addr, device, "vkGetBufferMemoryRequirements");
    functions.allocate_memory = load_device<PFN_vkAllocateMemory>(
        instance_functions.get_device_proc_addr, device, "vkAllocateMemory");
    functions.free_memory = load_device<PFN_vkFreeMemory>(
        instance_functions.get_device_proc_addr, device, "vkFreeMemory");
    functions.bind_buffer_memory = load_device<PFN_vkBindBufferMemory>(
        instance_functions.get_device_proc_addr, device, "vkBindBufferMemory");
    functions.map_memory = load_device<PFN_vkMapMemory>(
        instance_functions.get_device_proc_addr, device, "vkMapMemory");
    functions.unmap_memory = load_device<PFN_vkUnmapMemory>(
        instance_functions.get_device_proc_addr, device, "vkUnmapMemory");
    functions.get_buffer_device_address = load_device<PFN_vkGetBufferDeviceAddress>(
        instance_functions.get_device_proc_addr, device, "vkGetBufferDeviceAddress");
    if (functions.get_buffer_device_address == nullptr)
        functions.get_buffer_device_address = load_device<PFN_vkGetBufferDeviceAddressKHR>(
            instance_functions.get_device_proc_addr, device, "vkGetBufferDeviceAddressKHR");
    functions.create_shader_module = load_device<PFN_vkCreateShaderModule>(
        instance_functions.get_device_proc_addr, device, "vkCreateShaderModule");
    functions.destroy_shader_module = load_device<PFN_vkDestroyShaderModule>(
        instance_functions.get_device_proc_addr, device, "vkDestroyShaderModule");
    functions.create_descriptor_set_layout = load_device<PFN_vkCreateDescriptorSetLayout>(
        instance_functions.get_device_proc_addr, device, "vkCreateDescriptorSetLayout");
    functions.destroy_descriptor_set_layout = load_device<PFN_vkDestroyDescriptorSetLayout>(
        instance_functions.get_device_proc_addr, device, "vkDestroyDescriptorSetLayout");
    functions.create_pipeline_layout = load_device<PFN_vkCreatePipelineLayout>(
        instance_functions.get_device_proc_addr, device, "vkCreatePipelineLayout");
    functions.destroy_pipeline_layout = load_device<PFN_vkDestroyPipelineLayout>(
        instance_functions.get_device_proc_addr, device, "vkDestroyPipelineLayout");
    functions.create_descriptor_pool = load_device<PFN_vkCreateDescriptorPool>(
        instance_functions.get_device_proc_addr, device, "vkCreateDescriptorPool");
    functions.destroy_descriptor_pool = load_device<PFN_vkDestroyDescriptorPool>(
        instance_functions.get_device_proc_addr, device, "vkDestroyDescriptorPool");
    functions.allocate_descriptor_sets = load_device<PFN_vkAllocateDescriptorSets>(
        instance_functions.get_device_proc_addr, device, "vkAllocateDescriptorSets");
    functions.update_descriptor_sets = load_device<PFN_vkUpdateDescriptorSets>(
        instance_functions.get_device_proc_addr, device, "vkUpdateDescriptorSets");
    functions.create_acceleration_structure = load_device<PFN_vkCreateAccelerationStructureKHR>(
        instance_functions.get_device_proc_addr, device, "vkCreateAccelerationStructureKHR");
    functions.destroy_acceleration_structure = load_device<PFN_vkDestroyAccelerationStructureKHR>(
        instance_functions.get_device_proc_addr, device, "vkDestroyAccelerationStructureKHR");
    functions.get_acceleration_structure_build_sizes =
        load_device<PFN_vkGetAccelerationStructureBuildSizesKHR>(
            instance_functions.get_device_proc_addr, device,
            "vkGetAccelerationStructureBuildSizesKHR");
    functions.get_acceleration_structure_device_address =
        load_device<PFN_vkGetAccelerationStructureDeviceAddressKHR>(
            instance_functions.get_device_proc_addr, device,
            "vkGetAccelerationStructureDeviceAddressKHR");
    functions.cmd_build_acceleration_structures =
        load_device<PFN_vkCmdBuildAccelerationStructuresKHR>(
            instance_functions.get_device_proc_addr, device, "vkCmdBuildAccelerationStructuresKHR");
    functions.create_command_pool = load_device<PFN_vkCreateCommandPool>(
        instance_functions.get_device_proc_addr, device, "vkCreateCommandPool");
    functions.destroy_command_pool = load_device<PFN_vkDestroyCommandPool>(
        instance_functions.get_device_proc_addr, device, "vkDestroyCommandPool");
    functions.allocate_command_buffers = load_device<PFN_vkAllocateCommandBuffers>(
        instance_functions.get_device_proc_addr, device, "vkAllocateCommandBuffers");
    functions.begin_command_buffer = load_device<PFN_vkBeginCommandBuffer>(
        instance_functions.get_device_proc_addr, device, "vkBeginCommandBuffer");
    functions.end_command_buffer = load_device<PFN_vkEndCommandBuffer>(
        instance_functions.get_device_proc_addr, device, "vkEndCommandBuffer");
    functions.cmd_pipeline_barrier = load_device<PFN_vkCmdPipelineBarrier>(
        instance_functions.get_device_proc_addr, device, "vkCmdPipelineBarrier");
    functions.create_fence = load_device<PFN_vkCreateFence>(
        instance_functions.get_device_proc_addr, device, "vkCreateFence");
    functions.destroy_fence = load_device<PFN_vkDestroyFence>(
        instance_functions.get_device_proc_addr, device, "vkDestroyFence");
    functions.queue_submit = load_device<PFN_vkQueueSubmit>(
        instance_functions.get_device_proc_addr, device, "vkQueueSubmit");
    functions.wait_for_fences = load_device<PFN_vkWaitForFences>(
        instance_functions.get_device_proc_addr, device, "vkWaitForFences");
    functions.create_ray_tracing_pipelines =
        load_device<PFN_vkCreateRayTracingPipelinesKHR>(
            instance_functions.get_device_proc_addr, device, "vkCreateRayTracingPipelinesKHR");
    functions.destroy_pipeline = load_device<PFN_vkDestroyPipeline>(
        instance_functions.get_device_proc_addr, device, "vkDestroyPipeline");
    functions.get_ray_tracing_shader_group_handles =
        load_device<PFN_vkGetRayTracingShaderGroupHandlesKHR>(
            instance_functions.get_device_proc_addr, device,
            "vkGetRayTracingShaderGroupHandlesKHR");
    functions.cmd_bind_pipeline = load_device<PFN_vkCmdBindPipeline>(
        instance_functions.get_device_proc_addr, device, "vkCmdBindPipeline");
    functions.cmd_bind_descriptor_sets = load_device<PFN_vkCmdBindDescriptorSets>(
        instance_functions.get_device_proc_addr, device, "vkCmdBindDescriptorSets");
    functions.cmd_trace_rays = load_device<PFN_vkCmdTraceRaysKHR>(
        instance_functions.get_device_proc_addr, device, "vkCmdTraceRaysKHR");
    functions.create_query_pool = load_device<PFN_vkCreateQueryPool>(
        instance_functions.get_device_proc_addr, device, "vkCreateQueryPool");
    functions.destroy_query_pool = load_device<PFN_vkDestroyQueryPool>(
        instance_functions.get_device_proc_addr, device, "vkDestroyQueryPool");
    functions.cmd_write_timestamp = load_device<PFN_vkCmdWriteTimestamp>(
        instance_functions.get_device_proc_addr, device, "vkCmdWriteTimestamp");
    functions.get_query_pool_results = load_device<PFN_vkGetQueryPoolResults>(
        instance_functions.get_device_proc_addr, device, "vkGetQueryPoolResults");
    if (!require_device_functions(functions)) {
        if (functions.device_wait_idle != nullptr) functions.device_wait_idle(device);
        if (functions.destroy_device != nullptr) functions.destroy_device(device, nullptr);
        instance_functions.destroy_instance(instance, nullptr);
        receipt.device_created = false;
        receipt.instance_created = false;
        receipt.resources_released = true;
        set_diagnostic(receipt, NativeVulkanRayTracingExecutionState::unavailable,
                       NativeVulkanRayTracingFailureStage::device,
                       "native-vulkan-rt.device-entrypoint-unavailable",
                       "The Vulkan device lacks a required BLAS/TLAS execution entry point.");
        return receipt;
    }

    VkQueue queue{};
    functions.get_device_queue(device, selected.queue_family_index, 0U, &queue);
    if (queue == VK_NULL_HANDLE) {
        functions.device_wait_idle(device);
        functions.destroy_device(device, nullptr);
        instance_functions.destroy_instance(instance, nullptr);
        receipt.device_created = false;
        receipt.instance_created = false;
        receipt.resources_released = true;
        set_diagnostic(receipt, NativeVulkanRayTracingExecutionState::failed,
                       NativeVulkanRayTracingFailureStage::device,
                       "native-vulkan-rt.queue-unavailable",
                       "vkGetDeviceQueue returned a null queue.");
        return receipt;
    }
    receipt.queue_found = true;

    VkCommandPool command_pool{};
    VkCommandBuffer command_buffer{};
    VkFence fence{};
    VkShaderModule shader_module{};
    VkDescriptorSetLayout descriptor_set_layout{};
    VkPipelineLayout pipeline_layout{};
    VkDescriptorPool descriptor_pool{};
    VkDescriptorSet descriptor_set{};
    VkPipeline pipeline{};
    VkQueryPool query_pool{};
    BufferResource vertex_buffer;
    BufferResource instance_buffer;
    BufferResource blas_result_buffer;
    BufferResource blas_scratch_buffer;
    BufferResource tlas_result_buffer;
    BufferResource tlas_scratch_buffer;
    BufferResource sbt_buffer;
    BufferResource output_buffer;
    AccelerationStructureResource blas;
    AccelerationStructureResource tlas;
    bool cleanup_failed = false;
    const auto cleanup = [&]() {
        if (device != VK_NULL_HANDLE && functions.device_wait_idle != nullptr &&
            functions.device_wait_idle(device) != VK_SUCCESS) cleanup_failed = true;
        if (pipeline != VK_NULL_HANDLE)
            functions.destroy_pipeline(device, pipeline, nullptr);
        if (descriptor_pool != VK_NULL_HANDLE)
            functions.destroy_descriptor_pool(device, descriptor_pool, nullptr);
        if (pipeline_layout != VK_NULL_HANDLE)
            functions.destroy_pipeline_layout(device, pipeline_layout, nullptr);
        if (descriptor_set_layout != VK_NULL_HANDLE)
            functions.destroy_descriptor_set_layout(device, descriptor_set_layout, nullptr);
        if (shader_module != VK_NULL_HANDLE)
            functions.destroy_shader_module(device, shader_module, nullptr);
        if (query_pool != VK_NULL_HANDLE)
            functions.destroy_query_pool(device, query_pool, nullptr);
        if (tlas.acceleration_structure != VK_NULL_HANDLE)
            functions.destroy_acceleration_structure(device, tlas.acceleration_structure, nullptr);
        if (blas.acceleration_structure != VK_NULL_HANDLE)
            functions.destroy_acceleration_structure(device, blas.acceleration_structure, nullptr);
        const auto destroy_buffer = [&](BufferResource& resource) {
            if (resource.buffer != VK_NULL_HANDLE)
                functions.destroy_buffer(device, resource.buffer, nullptr);
            if (resource.memory != VK_NULL_HANDLE)
                functions.free_memory(device, resource.memory, nullptr);
            resource = {};
        };
        destroy_buffer(tlas_scratch_buffer);
        destroy_buffer(tlas_result_buffer);
        destroy_buffer(instance_buffer);
        destroy_buffer(blas_scratch_buffer);
        destroy_buffer(blas_result_buffer);
        destroy_buffer(vertex_buffer);
        destroy_buffer(sbt_buffer);
        destroy_buffer(output_buffer);
        if (fence != VK_NULL_HANDLE) functions.destroy_fence(device, fence, nullptr);
        if (command_pool != VK_NULL_HANDLE)
            functions.destroy_command_pool(device, command_pool, nullptr);
        if (device != VK_NULL_HANDLE) functions.destroy_device(device, nullptr);
        if (instance != VK_NULL_HANDLE) instance_functions.destroy_instance(instance, nullptr);
        receipt.resources_released = true;
    };
    const auto fail = [&](const NativeVulkanRayTracingFailureStage stage,
                          const std::string_view code, const std::string_view detail) {
        set_diagnostic(receipt, NativeVulkanRayTracingExecutionState::failed, stage, code, detail);
        cleanup();
        if (cleanup_failed) {
            receipt.failure_stage = NativeVulkanRayTracingFailureStage::cleanup;
            receipt.code = "native-vulkan-rt.cleanup-failed";
            receipt.detail = "The probe failed and vkDeviceWaitIdle also reported an error.";
        }
        return receipt;
    };

    VkCommandPoolCreateInfo command_pool_info{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    command_pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    command_pool_info.queueFamilyIndex = selected.queue_family_index;
    if (functions.create_command_pool(device, &command_pool_info, nullptr, &command_pool) != VK_SUCCESS)
        return fail(NativeVulkanRayTracingFailureStage::command,
                    "native-vulkan-rt.create-command-pool-failed",
                    "vkCreateCommandPool failed.");
    VkCommandBufferAllocateInfo command_buffer_info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    command_buffer_info.commandPool = command_pool;
    command_buffer_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    command_buffer_info.commandBufferCount = 1U;
    if (functions.allocate_command_buffers(device, &command_buffer_info, &command_buffer) != VK_SUCCESS)
        return fail(NativeVulkanRayTracingFailureStage::command,
                    "native-vulkan-rt.allocate-command-buffer-failed",
                    "vkAllocateCommandBuffers failed.");
    VkFenceCreateInfo fence_info{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    if (functions.create_fence(device, &fence_info, nullptr, &fence) != VK_SUCCESS)
        return fail(NativeVulkanRayTracingFailureStage::command,
                    "native-vulkan-rt.create-fence-failed", "vkCreateFence failed.");

    constexpr std::array<float, 9U> triangle_vertices{
        -0.75F, -0.75F, 0.0F, 0.75F, -0.75F, 0.0F, 0.0F, 0.75F, 0.0F};
    std::string error_code;
    std::string error_detail;
    const VkBufferUsageFlags geometry_usage =
        VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
        VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR;
    if (!create_buffer(functions, device, selected.memory_properties,
                       sizeof(triangle_vertices), geometry_usage,
                       VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                       triangle_vertices.data(), sizeof(triangle_vertices), vertex_buffer,
                       error_code, error_detail))
        return fail(NativeVulkanRayTracingFailureStage::buffer, error_code, error_detail);
    receipt.vertex_buffer_bytes = vertex_buffer.allocation_size;
    receipt.total_allocated_bytes = saturating_add(receipt.total_allocated_bytes,
                                                    vertex_buffer.allocation_size);

    VkAccelerationStructureGeometryTrianglesDataKHR triangles{
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR};
    triangles.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
    triangles.vertexData.deviceAddress = vertex_buffer.device_address;
    triangles.vertexStride = sizeof(float) * 3U;
    triangles.maxVertex = 2U;
    triangles.indexType = VK_INDEX_TYPE_NONE_KHR;
    VkAccelerationStructureGeometryKHR geometry{
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
    geometry.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
    geometry.flags = VK_GEOMETRY_OPAQUE_BIT_KHR;
    geometry.geometry.triangles = triangles;
    const std::uint32_t primitive_count = 1U;
    VkAccelerationStructureBuildGeometryInfoKHR blas_build_info{
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
    blas_build_info.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
    blas_build_info.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
    blas_build_info.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    blas_build_info.geometryCount = 1U;
    blas_build_info.pGeometries = &geometry;
    receipt.blas_flags_observed = true;
    receipt.blas_prefer_fast_trace =
        (blas_build_info.flags & VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR) != 0U;
    receipt.blas_prefer_fast_build =
        (blas_build_info.flags & VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_BUILD_BIT_KHR) != 0U;
    receipt.blas_allow_update =
        (blas_build_info.flags & VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR) != 0U;
    receipt.blas_allow_compaction =
        (blas_build_info.flags & VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_COMPACTION_BIT_KHR) != 0U;
    VkAccelerationStructureBuildSizesInfoKHR blas_sizes{
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
    functions.get_acceleration_structure_build_sizes(
        device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &blas_build_info,
        &primitive_count, &blas_sizes);
    const VkDeviceSize scratch_alignment = std::max<VkDeviceSize>(
        selected.acceleration_properties.minAccelerationStructureScratchOffsetAlignment, 256U);
    const auto blas_scratch_allocation = blas_sizes.buildScratchSize + scratch_alignment;
    if (!create_buffer(functions, device, selected.memory_properties,
                       blas_scratch_allocation,
                       VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                       VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, nullptr, 0U, blas_scratch_buffer,
                       error_code, error_detail))
        return fail(NativeVulkanRayTracingFailureStage::memory, error_code, error_detail);
    if (!create_buffer(functions, device, selected.memory_properties,
                       blas_sizes.accelerationStructureSize,
                       VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR |
                           VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                       VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, nullptr, 0U, blas_result_buffer,
                       error_code, error_detail))
        return fail(NativeVulkanRayTracingFailureStage::memory, error_code, error_detail);
    if (!create_acceleration_structure(
            functions, device, blas_result_buffer.buffer, blas_sizes.accelerationStructureSize,
            VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR, blas, error_code, error_detail))
        return fail(NativeVulkanRayTracingFailureStage::acceleration_structure,
                    error_code, error_detail);
    receipt.blas_scratch_bytes = blas_scratch_buffer.allocation_size;
    receipt.blas_result_bytes = blas_result_buffer.allocation_size;
    receipt.total_allocated_bytes = saturating_add(receipt.total_allocated_bytes,
                                                    blas_scratch_buffer.allocation_size);
    receipt.total_allocated_bytes = saturating_add(receipt.total_allocated_bytes,
                                                    blas_result_buffer.allocation_size);

    VkAccelerationStructureInstanceKHR instance_data{};
    instance_data.transform.matrix[0][0] = 1.0F;
    instance_data.transform.matrix[1][1] = 1.0F;
    instance_data.transform.matrix[2][2] = 1.0F;
    instance_data.instanceCustomIndex = 0U;
    instance_data.mask = 0xffU;
    instance_data.instanceShaderBindingTableRecordOffset = 0U;
    instance_data.flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;
    instance_data.accelerationStructureReference = blas.device_address;
    if (!create_buffer(functions, device, selected.memory_properties,
                       sizeof(instance_data), geometry_usage,
                       VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                       &instance_data, sizeof(instance_data), instance_buffer,
                       error_code, error_detail))
        return fail(NativeVulkanRayTracingFailureStage::buffer, error_code, error_detail);
    receipt.instance_buffer_bytes = instance_buffer.allocation_size;
    receipt.total_allocated_bytes = saturating_add(receipt.total_allocated_bytes,
                                                    instance_buffer.allocation_size);

    VkAccelerationStructureGeometryInstancesDataKHR instances{
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR};
    instances.arrayOfPointers = VK_FALSE;
    instances.data.deviceAddress = instance_buffer.device_address;
    VkAccelerationStructureGeometryKHR tlas_geometry{
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
    tlas_geometry.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
    tlas_geometry.flags = VK_GEOMETRY_OPAQUE_BIT_KHR;
    tlas_geometry.geometry.instances = instances;
    VkAccelerationStructureBuildGeometryInfoKHR tlas_build_info{
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
    tlas_build_info.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
    tlas_build_info.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
    tlas_build_info.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    tlas_build_info.geometryCount = 1U;
    tlas_build_info.pGeometries = &tlas_geometry;
    receipt.tlas_flags_observed = true;
    receipt.tlas_prefer_fast_trace =
        (tlas_build_info.flags & VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR) != 0U;
    receipt.tlas_prefer_fast_build =
        (tlas_build_info.flags & VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_BUILD_BIT_KHR) != 0U;
    receipt.tlas_allow_update =
        (tlas_build_info.flags & VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR) != 0U;
    receipt.tlas_allow_compaction =
        (tlas_build_info.flags & VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_COMPACTION_BIT_KHR) != 0U;
    VkAccelerationStructureBuildSizesInfoKHR tlas_sizes{
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
    functions.get_acceleration_structure_build_sizes(
        device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &tlas_build_info,
        &primitive_count, &tlas_sizes);
    const auto tlas_scratch_allocation = tlas_sizes.buildScratchSize + scratch_alignment;
    if (!create_buffer(functions, device, selected.memory_properties,
                       tlas_scratch_allocation,
                       VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                       VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, nullptr, 0U, tlas_scratch_buffer,
                       error_code, error_detail))
        return fail(NativeVulkanRayTracingFailureStage::memory, error_code, error_detail);
    if (!create_buffer(functions, device, selected.memory_properties,
                       tlas_sizes.accelerationStructureSize,
                       VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR |
                           VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                       VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, nullptr, 0U, tlas_result_buffer,
                       error_code, error_detail))
        return fail(NativeVulkanRayTracingFailureStage::memory, error_code, error_detail);
    if (!create_acceleration_structure(
            functions, device, tlas_result_buffer.buffer, tlas_sizes.accelerationStructureSize,
            VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR, tlas, error_code, error_detail))
        return fail(NativeVulkanRayTracingFailureStage::acceleration_structure,
                    error_code, error_detail);
    receipt.tlas_scratch_bytes = tlas_scratch_buffer.allocation_size;
    receipt.tlas_result_bytes = tlas_result_buffer.allocation_size;
    receipt.total_allocated_bytes = saturating_add(receipt.total_allocated_bytes,
                                                    tlas_scratch_buffer.allocation_size);
    receipt.total_allocated_bytes = saturating_add(receipt.total_allocated_bytes,
                                                    tlas_result_buffer.allocation_size);

    constexpr std::uint32_t trace_width = 1U;
    constexpr std::uint32_t trace_height = 1U;
    constexpr std::uint32_t trace_depth = 1U;
    constexpr std::uint32_t shader_group_count = 3U;
    constexpr std::uint32_t output_sentinel = 0x4E4F5254U; // "NORT"
    receipt.trace_width = trace_width;
    receipt.trace_height = trace_height;
    receipt.trace_depth = trace_depth;
    receipt.output_width = trace_width;
    receipt.output_height = trace_height;
    receipt.output_pixel_stride_bytes = sizeof(std::uint32_t);
    receipt.output_pixel_x = 0U;
    receipt.output_pixel_y = 0U;
    receipt.shader_group_count = shader_group_count;
    receipt.shader_table_record_count = shader_group_count;

    if (!create_buffer(
            functions, device, selected.memory_properties, sizeof(std::uint32_t),
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
            &output_sentinel, sizeof(output_sentinel), output_buffer, error_code, error_detail))
        return fail(NativeVulkanRayTracingFailureStage::buffer, error_code, error_detail);
    receipt.output_buffer_bytes = output_buffer.allocation_size;
    receipt.output_bytes = sizeof(std::uint32_t);
    receipt.output_resource_created = true;
    receipt.total_allocated_bytes = saturating_add(receipt.total_allocated_bytes,
                                                    output_buffer.allocation_size);

    const std::array<VkDescriptorSetLayoutBinding, 2U> descriptor_bindings{
        VkDescriptorSetLayoutBinding{
            0U, VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1U,
            VK_SHADER_STAGE_RAYGEN_BIT_KHR | VK_SHADER_STAGE_MISS_BIT_KHR |
                VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR,
            nullptr},
        VkDescriptorSetLayoutBinding{
            1U, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1U,
            VK_SHADER_STAGE_RAYGEN_BIT_KHR | VK_SHADER_STAGE_MISS_BIT_KHR |
                VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR,
            nullptr}};
    VkDescriptorSetLayoutCreateInfo descriptor_set_layout_info{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    descriptor_set_layout_info.bindingCount =
        static_cast<std::uint32_t>(descriptor_bindings.size());
    descriptor_set_layout_info.pBindings = descriptor_bindings.data();
    if (functions.create_descriptor_set_layout(
            device, &descriptor_set_layout_info, nullptr, &descriptor_set_layout) != VK_SUCCESS)
        return fail(NativeVulkanRayTracingFailureStage::descriptor,
                    "native-vulkan-rt.create-descriptor-set-layout-failed",
                    "vkCreateDescriptorSetLayout failed for the ray tracing resources.");
    receipt.descriptor_set_layout_created = true;

    VkPipelineLayoutCreateInfo pipeline_layout_info{
        VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pipeline_layout_info.setLayoutCount = 1U;
    pipeline_layout_info.pSetLayouts = &descriptor_set_layout;
    if (functions.create_pipeline_layout(
            device, &pipeline_layout_info, nullptr, &pipeline_layout) != VK_SUCCESS)
        return fail(NativeVulkanRayTracingFailureStage::descriptor,
                    "native-vulkan-rt.create-pipeline-layout-failed",
                    "vkCreatePipelineLayout failed for the ray tracing pipeline.");
    receipt.pipeline_layout_created = true;

    const std::array<VkDescriptorPoolSize, 2U> descriptor_pool_sizes{
        VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1U},
        VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1U}};
    VkDescriptorPoolCreateInfo descriptor_pool_info{
        VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    descriptor_pool_info.maxSets = 1U;
    descriptor_pool_info.poolSizeCount =
        static_cast<std::uint32_t>(descriptor_pool_sizes.size());
    descriptor_pool_info.pPoolSizes = descriptor_pool_sizes.data();
    if (functions.create_descriptor_pool(
            device, &descriptor_pool_info, nullptr, &descriptor_pool) != VK_SUCCESS)
        return fail(NativeVulkanRayTracingFailureStage::descriptor,
                    "native-vulkan-rt.create-descriptor-pool-failed",
                    "vkCreateDescriptorPool failed for the ray tracing resources.");

    VkDescriptorSetAllocateInfo descriptor_set_allocate_info{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    descriptor_set_allocate_info.descriptorPool = descriptor_pool;
    descriptor_set_allocate_info.descriptorSetCount = 1U;
    descriptor_set_allocate_info.pSetLayouts = &descriptor_set_layout;
    if (functions.allocate_descriptor_sets(
            device, &descriptor_set_allocate_info, &descriptor_set) != VK_SUCCESS)
        return fail(NativeVulkanRayTracingFailureStage::descriptor,
                    "native-vulkan-rt.allocate-descriptor-set-failed",
                    "vkAllocateDescriptorSets failed for the ray tracing resources.");
    receipt.descriptor_set_allocated = true;
    VkWriteDescriptorSetAccelerationStructureKHR acceleration_write{
        VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR};
    acceleration_write.accelerationStructureCount = 1U;
    acceleration_write.pAccelerationStructures = &tlas.acceleration_structure;
    VkDescriptorBufferInfo output_descriptor_info{
        output_buffer.buffer, 0U, sizeof(std::uint32_t)};
    std::array<VkWriteDescriptorSet, 2U> descriptor_writes{
        VkWriteDescriptorSet{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET},
        VkWriteDescriptorSet{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}};
    descriptor_writes[0].pNext = &acceleration_write;
    descriptor_writes[0].dstSet = descriptor_set;
    descriptor_writes[0].dstBinding = 0U;
    descriptor_writes[0].descriptorCount = 1U;
    descriptor_writes[0].descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
    descriptor_writes[1].dstSet = descriptor_set;
    descriptor_writes[1].dstBinding = 1U;
    descriptor_writes[1].descriptorCount = 1U;
    descriptor_writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    descriptor_writes[1].pBufferInfo = &output_descriptor_info;
    functions.update_descriptor_sets(device,
                                     static_cast<std::uint32_t>(descriptor_writes.size()),
                                     descriptor_writes.data(), 0U, nullptr);

    VkShaderModuleCreateInfo shader_module_info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    shader_module_info.codeSize =
        native_raytracing_probe_spirv.size() * sizeof(native_raytracing_probe_spirv[0]);
    shader_module_info.pCode = native_raytracing_probe_spirv.data();
    if (functions.create_shader_module(
            device, &shader_module_info, nullptr, &shader_module) != VK_SUCCESS)
        return fail(NativeVulkanRayTracingFailureStage::shader_module,
                    "native-vulkan-rt.create-shader-module-failed",
                    "vkCreateShaderModule failed for the embedded SPIR-V probe.");
    receipt.shader_module_created = true;

    const std::array<VkPipelineShaderStageCreateInfo, 3U> shader_stages{
        VkPipelineShaderStageCreateInfo{
            VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0U,
            VK_SHADER_STAGE_RAYGEN_BIT_KHR, shader_module, "RayGen", nullptr},
        VkPipelineShaderStageCreateInfo{
            VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0U,
            VK_SHADER_STAGE_MISS_BIT_KHR, shader_module, "Miss", nullptr},
        VkPipelineShaderStageCreateInfo{
            VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0U,
            VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR, shader_module, "ClosestHit", nullptr}};
    std::array<VkRayTracingShaderGroupCreateInfoKHR, shader_group_count> shader_groups{};
    for (auto& group : shader_groups) {
        group.sType = VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR;
        group.generalShader = VK_SHADER_UNUSED_KHR;
        group.closestHitShader = VK_SHADER_UNUSED_KHR;
        group.anyHitShader = VK_SHADER_UNUSED_KHR;
        group.intersectionShader = VK_SHADER_UNUSED_KHR;
    }
    shader_groups[0].type = VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR;
    shader_groups[0].generalShader = 0U;
    shader_groups[1].type = VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR;
    shader_groups[1].generalShader = 1U;
    shader_groups[2].type = VK_RAY_TRACING_SHADER_GROUP_TYPE_TRIANGLES_HIT_GROUP_KHR;
    shader_groups[2].closestHitShader = 2U;
    VkRayTracingPipelineInterfaceCreateInfoKHR pipeline_interface{
        VK_STRUCTURE_TYPE_RAY_TRACING_PIPELINE_INTERFACE_CREATE_INFO_KHR};
    pipeline_interface.maxPipelineRayPayloadSize = sizeof(std::uint32_t);
    pipeline_interface.maxPipelineRayHitAttributeSize = sizeof(float) * 2U;
    VkRayTracingPipelineCreateInfoKHR pipeline_info{
        VK_STRUCTURE_TYPE_RAY_TRACING_PIPELINE_CREATE_INFO_KHR};
    pipeline_info.pNext = &pipeline_interface;
    pipeline_info.stageCount = static_cast<std::uint32_t>(shader_stages.size());
    pipeline_info.pStages = shader_stages.data();
    pipeline_info.groupCount = static_cast<std::uint32_t>(shader_groups.size());
    pipeline_info.pGroups = shader_groups.data();
    pipeline_info.maxPipelineRayRecursionDepth = 1U;
    pipeline_info.layout = pipeline_layout;
    const auto pipeline_result = functions.create_ray_tracing_pipelines(
        device, VK_NULL_HANDLE, VK_NULL_HANDLE, 1U, &pipeline_info, nullptr, &pipeline);
    if (pipeline_result != VK_SUCCESS)
        return fail(NativeVulkanRayTracingFailureStage::pipeline,
                    "native-vulkan-rt.create-ray-tracing-pipeline-failed",
                    result_detail("vkCreateRayTracingPipelinesKHR", pipeline_result));
    receipt.pipeline_created = true;

    const auto& ray_tracing_properties = selected.ray_tracing_properties;
    const auto handle_size = ray_tracing_properties.shaderGroupHandleSize;
    const auto handle_alignment = std::max(ray_tracing_properties.shaderGroupHandleAlignment, 1U);
    const auto base_alignment = std::max(ray_tracing_properties.shaderGroupBaseAlignment, 1U);
    const auto aligned_handle_size = align_device_address(handle_size, handle_alignment);
    const auto sbt_stride = align_device_address(aligned_handle_size, base_alignment);
    if (handle_size == 0U || sbt_stride == 0U ||
        sbt_stride > ray_tracing_properties.maxShaderGroupStride ||
        sbt_stride > (std::numeric_limits<VkDeviceSize>::max() / shader_group_count))
        return fail(NativeVulkanRayTracingFailureStage::sbt,
                    "native-vulkan-rt.sbt-properties-invalid",
                    "The device reported invalid shader-group handle or stride properties.");
    const auto sbt_bytes = sbt_stride * shader_group_count;
    std::vector<std::uint8_t> shader_group_handles(
        static_cast<std::size_t>(handle_size) * shader_group_count);
    if (functions.get_ray_tracing_shader_group_handles(
            device, pipeline, 0U, shader_group_count, shader_group_handles.size(),
            shader_group_handles.data()) != VK_SUCCESS)
        return fail(NativeVulkanRayTracingFailureStage::sbt,
                    "native-vulkan-rt.get-sbt-handles-failed",
                    "vkGetRayTracingShaderGroupHandlesKHR failed for the pipeline groups.");
    std::vector<std::uint8_t> shader_table(static_cast<std::size_t>(sbt_bytes), 0U);
    for (std::uint32_t group = 0U; group < shader_group_count; ++group)
        std::memcpy(shader_table.data() + static_cast<std::size_t>(group * sbt_stride),
                    shader_group_handles.data() + static_cast<std::size_t>(group * handle_size),
                    handle_size);
    if (!create_buffer(
            functions, device, selected.memory_properties, sbt_bytes,
            VK_BUFFER_USAGE_SHADER_BINDING_TABLE_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
            shader_table.data(), shader_table.size(), sbt_buffer, error_code, error_detail))
        return fail(NativeVulkanRayTracingFailureStage::sbt, error_code, error_detail);
    receipt.sbt_buffer_bytes = sbt_buffer.allocation_size;
    receipt.shader_table_record_bytes = static_cast<std::uint32_t>(sbt_stride);
    receipt.shader_table_bytes = sbt_buffer.allocation_size;
    receipt.shader_table_prepared = true;
    receipt.shader_table_uploaded = true;
    receipt.sbt_built = true;
    receipt.total_allocated_bytes = saturating_add(receipt.total_allocated_bytes,
                                                    sbt_buffer.allocation_size);

    VkQueryPoolCreateInfo query_pool_info{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
    query_pool_info.queryType = VK_QUERY_TYPE_TIMESTAMP;
    query_pool_info.queryCount = 2U;
    if (functions.create_query_pool(device, &query_pool_info, nullptr, &query_pool) != VK_SUCCESS)
        return fail(NativeVulkanRayTracingFailureStage::command,
                    "native-vulkan-rt.create-timestamp-query-pool-failed",
                    "vkCreateQueryPool failed for the TraceRays timestamp interval.");
    receipt.timestamp_query_created = true;

    VkCommandBufferBeginInfo begin_info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    if (functions.begin_command_buffer(command_buffer, &begin_info) != VK_SUCCESS)
        return fail(NativeVulkanRayTracingFailureStage::command,
                    "native-vulkan-rt.begin-command-buffer-failed",
                    "vkBeginCommandBuffer failed.");
    VkMemoryBarrier host_to_blas{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    host_to_blas.srcAccessMask = VK_ACCESS_HOST_WRITE_BIT;
    host_to_blas.dstAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR |
        VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
    functions.cmd_pipeline_barrier(
        command_buffer, VK_PIPELINE_STAGE_HOST_BIT,
        VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR, 0U, 1U, &host_to_blas,
        0U, nullptr, 0U, nullptr);
    const auto blas_scratch_address = align_device_address(
        blas_scratch_buffer.device_address, scratch_alignment);
    if (blas_scratch_address + blas_sizes.buildScratchSize >
        blas_scratch_buffer.device_address + blas_scratch_buffer.allocation_size)
        return fail(NativeVulkanRayTracingFailureStage::memory,
                    "native-vulkan-rt.blas-scratch-alignment-failed",
                    "BLAS scratch device address could not satisfy the reported alignment.");
    blas_build_info.dstAccelerationStructure = blas.acceleration_structure;
    blas_build_info.scratchData.deviceAddress = blas_scratch_address;
    VkAccelerationStructureBuildRangeInfoKHR blas_range{1U, 0U, 0U, 0U};
    const VkAccelerationStructureBuildRangeInfoKHR* blas_range_pointer = &blas_range;
    functions.cmd_build_acceleration_structures(command_buffer, 1U, &blas_build_info,
                                                 &blas_range_pointer);
    receipt.blas_build_submitted = true;
    receipt.blas_build_count = 1U;
    VkMemoryBarrier blas_to_tlas{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    blas_to_tlas.srcAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
    blas_to_tlas.dstAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR |
        VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
    functions.cmd_pipeline_barrier(
        command_buffer, VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
        VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR, 0U, 1U, &blas_to_tlas,
        0U, nullptr, 0U, nullptr);
    VkMemoryBarrier instance_host_to_tlas{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    instance_host_to_tlas.srcAccessMask = VK_ACCESS_HOST_WRITE_BIT;
    instance_host_to_tlas.dstAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR |
        VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
    functions.cmd_pipeline_barrier(
        command_buffer, VK_PIPELINE_STAGE_HOST_BIT,
        VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR, 0U, 1U,
        &instance_host_to_tlas, 0U, nullptr, 0U, nullptr);
    const auto tlas_scratch_address = align_device_address(
        tlas_scratch_buffer.device_address, scratch_alignment);
    if (tlas_scratch_address + tlas_sizes.buildScratchSize >
        tlas_scratch_buffer.device_address + tlas_scratch_buffer.allocation_size)
        return fail(NativeVulkanRayTracingFailureStage::memory,
                    "native-vulkan-rt.tlas-scratch-alignment-failed",
                    "TLAS scratch device address could not satisfy the reported alignment.");
    tlas_build_info.dstAccelerationStructure = tlas.acceleration_structure;
    tlas_build_info.scratchData.deviceAddress = tlas_scratch_address;
    VkAccelerationStructureBuildRangeInfoKHR tlas_range{1U, 0U, 0U, 0U};
    const VkAccelerationStructureBuildRangeInfoKHR* tlas_range_pointer = &tlas_range;
    functions.cmd_build_acceleration_structures(command_buffer, 1U, &tlas_build_info,
                                                 &tlas_range_pointer);
    receipt.tlas_build_submitted = true;
    receipt.tlas_build_count = 1U;

    VkMemoryBarrier build_to_trace{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    build_to_trace.srcAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
    build_to_trace.dstAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR;
    functions.cmd_pipeline_barrier(
        command_buffer, VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
        VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR, 0U, 1U, &build_to_trace,
        0U, nullptr, 0U, nullptr);
    VkMemoryBarrier host_to_trace{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    host_to_trace.srcAccessMask = VK_ACCESS_HOST_WRITE_BIT;
    host_to_trace.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    functions.cmd_pipeline_barrier(
        command_buffer, VK_PIPELINE_STAGE_HOST_BIT,
        VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR, 0U, 1U, &host_to_trace,
        0U, nullptr, 0U, nullptr);
    const VkDeviceAddress sbt_device_address = sbt_buffer.device_address;
    const VkDeviceSize sbt_record_stride = receipt.shader_table_record_bytes;
    const VkStridedDeviceAddressRegionKHR raygen_region{
        sbt_device_address, sbt_record_stride, sbt_record_stride};
    const VkStridedDeviceAddressRegionKHR miss_region{
        sbt_device_address + sbt_record_stride, sbt_record_stride, sbt_record_stride};
    const VkStridedDeviceAddressRegionKHR hit_region{
        sbt_device_address + sbt_record_stride * 2U, sbt_record_stride, sbt_record_stride};
    const VkStridedDeviceAddressRegionKHR callable_region{};
    functions.cmd_write_timestamp(command_buffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                  query_pool, 0U);
    functions.cmd_bind_pipeline(command_buffer, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR,
                                pipeline);
    functions.cmd_bind_descriptor_sets(command_buffer, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR,
                                       pipeline_layout, 0U, 1U, &descriptor_set, 0U, nullptr);
    functions.cmd_trace_rays(command_buffer, &raygen_region, &miss_region, &hit_region,
                             &callable_region, trace_width, trace_height, trace_depth);
    functions.cmd_write_timestamp(command_buffer, VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,
                                  query_pool, 1U);
    receipt.timestamp_queries_issued = true;
    receipt.trace_dispatch_issued = true;
    receipt.build_only = false;
    VkMemoryBarrier trace_to_host{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    trace_to_host.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    trace_to_host.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    functions.cmd_pipeline_barrier(
        command_buffer, VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,
        VK_PIPELINE_STAGE_HOST_BIT, 0U, 1U, &trace_to_host,
        0U, nullptr, 0U, nullptr);
    if (functions.end_command_buffer(command_buffer) != VK_SUCCESS)
        return fail(NativeVulkanRayTracingFailureStage::command,
                    "native-vulkan-rt.end-command-buffer-failed",
                    "vkEndCommandBuffer failed.");
    functions.get_device_queue(device, selected.queue_family_index, 0U, &queue);
    VkSubmitInfo submit_info{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit_info.commandBufferCount = 1U;
    submit_info.pCommandBuffers = &command_buffer;
    const auto submit_result = functions.queue_submit(queue, 1U, &submit_info, fence);
    if (submit_result != VK_SUCCESS)
        return fail(NativeVulkanRayTracingFailureStage::submit,
                    "native-vulkan-rt.queue-submit-failed",
                    result_detail("vkQueueSubmit", submit_result));
    receipt.submitted = true;
    receipt.trace_dispatch_submitted = receipt.trace_dispatch_issued;
    const auto wait_result = functions.wait_for_fences(device, 1U, &fence, VK_TRUE,
                                                       std::numeric_limits<std::uint64_t>::max());
    if (wait_result != VK_SUCCESS)
        return fail(NativeVulkanRayTracingFailureStage::submit,
                    "native-vulkan-rt.fence-wait-failed",
                    result_detail("vkWaitForFences", wait_result));
    receipt.fence_signaled = true;
    receipt.trace_dispatch_submitted = receipt.trace_dispatch_issued;
    receipt.trace_dispatch_completed = receipt.trace_dispatch_issued;
    receipt.synchronization_completed = true;
    receipt.blas_built = true;
    receipt.tlas_built = true;
    receipt.blas_build_completed = true;
    receipt.tlas_build_completed = true;

    std::array<std::uint64_t, 2U> timestamp_values{};
    const auto timestamp_result = functions.get_query_pool_results(
        device, query_pool, 0U, 2U, sizeof(timestamp_values), timestamp_values.data(),
        sizeof(timestamp_values[0]), VK_QUERY_RESULT_64_BIT);
    if (timestamp_result != VK_SUCCESS)
        return fail(NativeVulkanRayTracingFailureStage::readback,
                    "native-vulkan-rt.timestamp-readback-failed",
                    result_detail("vkGetQueryPoolResults", timestamp_result));
    receipt.timestamp_data_resolved = true;
    receipt.gpu_timestamp_ticks_begin = timestamp_values[0U];
    receipt.gpu_timestamp_ticks_end = timestamp_values[1U];
    receipt.gpu_timestamp_readback_bytes = sizeof(timestamp_values);
    const auto timestamp_period = static_cast<long double>(selected.properties.limits.timestampPeriod);
    if (timestamp_period > 0.0L && timestamp_values[1U] > timestamp_values[0U]) {
        receipt.gpu_timestamp_ticks_delta =
            timestamp_values[1U] - timestamp_values[0U];
        receipt.gpu_timestamp_duration_ns = static_cast<std::uint64_t>(
            static_cast<long double>(receipt.gpu_timestamp_ticks_delta) * timestamp_period);
        receipt.gpu_timestamp_frequency_hz = static_cast<std::uint64_t>(
            1'000'000'000.0L / timestamp_period);
    }
    receipt.gpu_timestamps_valid = receipt.timestamp_query_created &&
        receipt.timestamp_queries_issued && receipt.timestamp_data_resolved &&
        receipt.gpu_timestamp_ticks_delta > 0U && receipt.gpu_timestamp_duration_ns > 0U &&
        receipt.gpu_timestamp_frequency_hz > 0U;
    if (!receipt.gpu_timestamps_valid)
        return fail(NativeVulkanRayTracingFailureStage::readback,
                    "native-vulkan-rt.timestamp-contract-invalid",
                    "Vulkan timestamp readback did not produce a positive TraceRays interval.");

    void* mapped_output = nullptr;
    const auto map_result = functions.map_memory(
        device, output_buffer.memory, 0U, sizeof(std::uint32_t), 0U, &mapped_output);
    if (map_result != VK_SUCCESS || mapped_output == nullptr)
        return fail(NativeVulkanRayTracingFailureStage::readback,
                    "native-vulkan-rt.output-readback-map-failed",
                    result_detail("vkMapMemory", map_result));
    std::uint32_t output_value = 0U;
    std::memcpy(&output_value, mapped_output, sizeof(output_value));
    functions.unmap_memory(device, output_buffer.memory);
    receipt.output_value = output_value;
    receipt.output_hit = output_value;
    receipt.output_sentinel = output_sentinel;
    receipt.output_readback_bytes = sizeof(output_value);
    constexpr std::uint64_t fnv_offset_basis = 1469598103934665603ULL;
    constexpr std::uint64_t fnv_prime = 1099511628211ULL;
    auto output_hash = fnv_offset_basis;
    for (std::size_t byte_index = 0U; byte_index < sizeof(output_value); ++byte_index) {
        output_hash ^= static_cast<std::uint8_t>(
            (output_value >> (byte_index * 8U)) & 0xffU);
        output_hash *= fnv_prime;
    }
    receipt.output_hash = output_hash;
    receipt.output_readback_completed = true;
    receipt.state = NativeVulkanRayTracingExecutionState::completed;
    receipt.failure_stage = NativeVulkanRayTracingFailureStage::none;
    receipt.code = "native-vulkan-rt.trace-readback-completed";
    receipt.detail = "A real one-triangle TLAS traced one 1x1 ray and returned a host-visible output marker.";
    cleanup();
    if (cleanup_failed) {
        receipt.state = NativeVulkanRayTracingExecutionState::failed;
        receipt.failure_stage = NativeVulkanRayTracingFailureStage::cleanup;
        receipt.code = "native-vulkan-rt.cleanup-failed";
        receipt.detail = "BLAS/TLAS submission completed, but vkDeviceWaitIdle failed during cleanup.";
    }
    return receipt;
}

#endif // NOEMANCER_HAS_VULKAN_HEADERS

} // namespace

std::string_view native_vulkan_raytracing_execution_state_name(
    const NativeVulkanRayTracingExecutionState state) noexcept {
    switch (state) {
    case NativeVulkanRayTracingExecutionState::unavailable: return "unavailable";
    case NativeVulkanRayTracingExecutionState::unsupported: return "unsupported";
    case NativeVulkanRayTracingExecutionState::completed: return "completed";
    case NativeVulkanRayTracingExecutionState::failed: return "failed";
    }
    return "failed";
}

std::string_view native_vulkan_raytracing_failure_stage_name(
    const NativeVulkanRayTracingFailureStage stage) noexcept {
    switch (stage) {
    case NativeVulkanRayTracingFailureStage::none: return "none";
    case NativeVulkanRayTracingFailureStage::loader: return "loader";
    case NativeVulkanRayTracingFailureStage::instance: return "instance";
    case NativeVulkanRayTracingFailureStage::physical_device: return "physical-device";
    case NativeVulkanRayTracingFailureStage::device: return "device";
    case NativeVulkanRayTracingFailureStage::memory: return "memory";
    case NativeVulkanRayTracingFailureStage::buffer: return "buffer";
    case NativeVulkanRayTracingFailureStage::acceleration_structure: return "acceleration-structure";
    case NativeVulkanRayTracingFailureStage::command: return "command";
    case NativeVulkanRayTracingFailureStage::submit: return "submit";
    case NativeVulkanRayTracingFailureStage::cleanup: return "cleanup";
    }
    return "cleanup";
}

NativeVulkanRayTracingExecutionReceipt execute_native_vulkan_raytracing_blas_tlas() {
#if NOEMANCER_HAS_VULKAN_HEADERS
    return execute_impl();
#else
    NativeVulkanRayTracingExecutionReceipt receipt;
    receipt.resources_released = true;
    set_diagnostic(receipt, NativeVulkanRayTracingExecutionState::unavailable,
                   NativeVulkanRayTracingFailureStage::loader,
                   "native-vulkan-rt.headers-unavailable",
                   "The build does not provide Vulkan headers for the native executor.");
    return receipt;
#endif
}

} // namespace noemancer
