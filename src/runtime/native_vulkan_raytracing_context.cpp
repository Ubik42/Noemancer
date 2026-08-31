#include "runtime/native_vulkan_raytracing_context.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
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

constexpr float kEpsilon = 1.0e-6F;
constexpr float kMaximumCoordinate = 1.0e6F;
constexpr float kMaximumDistance = 1.0e9F;
constexpr std::uint32_t kHitMarker = 0x48495421U;
constexpr std::uint32_t kMissMarker = 0x4D495353U;
constexpr std::uint64_t kFnvOffsetBasis = 1469598103934665603ULL;
constexpr std::uint64_t kFnvPrime = 1099511628211ULL;

#if NOEMANCER_HAS_VULKAN_HEADERS
// Reused verbatim from the short-lived Vulkan RT executor.  The module has
// RayGen, Miss and ClosestHit entry points and writes a four-byte marker via
// set 0 binding 1.  Keeping this bounded fixture embedded makes the context
// independent from a build-directory shader path.
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
    0x00000020U,
    0x00000006U, 0x000014DEU, 0x0004003BU, 0x00000020U, 0x00000008U, 0x000014DEU, 0x0004002CU, 0x0000000BU,
    0x00000024U, 0x0000000EU, 0x0004002CU, 0x0000000BU, 0x00000025U, 0x0000001BU, 0x0004002CU, 0x0000000BU,
    0x00000026U, 0x0000001CU, 0x00040020U, 0x00000027U, 0x000014DAU, 0x0000000DU, 0x00050036U, 0x00000021U,
    0x00000001U, 0x00000000U, 0x00000022U, 0x000200F8U, 0x00000028U, 0x0003003EU, 0x00000004U, 0x00000024U,
    0x0004003DU, 0x00000009U, 0x00000029U, 0x00000002U, 0x000C115DU, 0x00000029U, 0x0000000EU, 0x00000019U,
    0x0000000EU, 0x0000001AU, 0x0000000EU, 0x00000015U, 0x00000012U, 0x00000017U, 0x00000018U, 0x00000004U,
    0x00050041U, 0x00000027U, 0x0000002AU, 0x00000004U, 0x0000000EU, 0x0004003DU, 0x0000000DU, 0x0000002BU,
    0x0000002AU, 0x00060041U, 0x00000023U, 0x0000002CU, 0x00000003U, 0x00000010U, 0x0000000EU, 0x0003003EU,
    0x0000002CU, 0x0000002BU, 0x000100FDU, 0x00010038U, 0x00050036U, 0x00000021U, 0x00000005U, 0x00000000U,
    0x00000022U, 0x000200F8U, 0x0000002DU, 0x0003003EU, 0x00000006U, 0x00000025U, 0x000100FDU, 0x00010038U,
    0x00050036U, 0x00000021U, 0x00000007U, 0x00000000U, 0x00000022U, 0x000200F8U, 0x0000002EU, 0x0003003EU,
    0x00000008U, 0x00000026U, 0x000100FDU, 0x00010038U,
};
#endif

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
    for (const auto& position : triangle.positions)
        if (!finite_vec(to_vec3(position))) return false;
    return true;
}

bool valid_trace_request(const NativeVulkanRayTracingTraceRequest& request) noexcept {
    const auto origin = to_vec3(request.origin);
    const auto direction = to_vec3(request.direction);
    const auto direction_length_squared = dot(direction, direction);
    return finite_vec(origin) && finite_vec(direction) && std::isfinite(direction_length_squared) &&
           direction_length_squared > kEpsilon * kEpsilon &&
           finite_bounded(request.minimum_distance, kMaximumDistance) &&
           finite_bounded(request.maximum_distance, kMaximumDistance) &&
           request.minimum_distance >= 0.0F && request.maximum_distance > request.minimum_distance;
}

std::uint64_t hash_u32(std::uint64_t hash, const std::uint32_t value) noexcept {
    for (std::uint32_t shift = 0U; shift < 32U; shift += 8U) {
        hash ^= static_cast<std::uint8_t>((value >> shift) & 0xffU);
        hash *= kFnvPrime;
    }
    return hash;
}

std::uint64_t scene_fingerprint(const NativeVulkanRayTracingScene& scene) noexcept {
    auto hash = kFnvOffsetBasis;
    hash = hash_u32(hash, static_cast<std::uint32_t>(scene.triangles.size()));
    hash = hash_u32(hash, static_cast<std::uint32_t>(scene.topology_revision));
    hash = hash_u32(hash, static_cast<std::uint32_t>(scene.topology_revision >> 32U));
    hash = hash_u32(hash, static_cast<std::uint32_t>(scene.content_revision));
    hash = hash_u32(hash, static_cast<std::uint32_t>(scene.content_revision >> 32U));
    for (const auto& triangle : scene.triangles)
        for (const auto& position : triangle.positions)
            for (const auto coordinate : position)
                hash = hash_u32(hash, std::bit_cast<std::uint32_t>(coordinate));
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

#if NOEMANCER_HAS_VULKAN_HEADERS

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
                     const PFN_vkGetInstanceProcAddr gipa,
                     const char* name) {
    auto function = module.symbol(name);
    if (function == nullptr && gipa != nullptr)
        function = reinterpret_cast<void*>(gipa(VK_NULL_HANDLE, name));
    return reinterpret_cast<Function>(function);
}

template <typename Function>
Function load_instance(const PFN_vkGetInstanceProcAddr gipa,
                       const VkInstance instance,
                       const char* name) {
    if (gipa == nullptr || instance == VK_NULL_HANDLE) return nullptr;
    return reinterpret_cast<Function>(gipa(instance, name));
}

template <typename Function>
Function load_device(const PFN_vkGetDeviceProcAddr gdpa,
                     const VkDevice device,
                     const char* name) {
    if (gdpa == nullptr || device == VK_NULL_HANDLE) return nullptr;
    return reinterpret_cast<Function>(gdpa(device, name));
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
    PFN_vkGetPhysicalDeviceFormatProperties get_physical_device_format_properties{};
    PFN_vkGetDeviceProcAddr get_device_proc_addr{};
    PFN_vkCreateDevice create_device{};
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
    PFN_vkCreateImage create_image{};
    PFN_vkDestroyImage destroy_image{};
    PFN_vkGetImageMemoryRequirements get_image_memory_requirements{};
    PFN_vkBindImageMemory bind_image_memory{};
    PFN_vkCreateImageView create_image_view{};
    PFN_vkDestroyImageView destroy_image_view{};
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
    PFN_vkResetFences reset_fences{};
    PFN_vkWaitForFences wait_for_fences{};
    PFN_vkQueueSubmit queue_submit{};
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
    PFN_vkCreateRayTracingPipelinesKHR create_ray_tracing_pipelines{};
    PFN_vkDestroyPipeline destroy_pipeline{};
    PFN_vkGetRayTracingShaderGroupHandlesKHR get_ray_tracing_shader_group_handles{};
    PFN_vkCmdBindPipeline cmd_bind_pipeline{};
    PFN_vkCmdBindDescriptorSets cmd_bind_descriptor_sets{};
    PFN_vkCmdTraceRaysKHR cmd_trace_rays{};
};

struct BufferResource final {
    VkBuffer buffer{};
    VkDeviceMemory memory{};
    VkDeviceSize allocation_size{};
    VkDeviceAddress device_address{};
};

struct ImageResource final {
    VkImage image{};
    VkImageView view{};
    VkDeviceMemory memory{};
    VkDeviceSize allocation_size{};
};

struct AccelerationStructureResource final {
    VkAccelerationStructureKHR acceleration_structure{};
    VkDeviceAddress device_address{};
};

struct SelectedPhysicalDevice final {
    VkPhysicalDevice handle{};
    VkPhysicalDeviceProperties properties{};
    VkPhysicalDeviceMemoryProperties memory_properties{};
    VkPhysicalDeviceAccelerationStructurePropertiesKHR acceleration_properties{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR};
    VkPhysicalDeviceRayTracingPipelinePropertiesKHR ray_tracing_properties{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_PROPERTIES_KHR};
    std::uint32_t queue_family_index{};
    bool buffer_device_address_extension{};
};

bool has_extension(const std::vector<VkExtensionProperties>& extensions, const char* name) {
    return std::ranges::any_of(extensions, [name](const VkExtensionProperties& extension) {
        return std::strcmp(extension.extensionName, name) == 0;
    });
}

std::optional<std::uint32_t> find_memory_type(const VkPhysicalDeviceMemoryProperties& properties,
                                               const std::uint32_t bits,
                                               const VkMemoryPropertyFlags required) {
    for (std::uint32_t index = 0U; index < properties.memoryTypeCount; ++index)
        if ((bits & (1U << index)) != 0U &&
            (properties.memoryTypes[index].propertyFlags & required) == required)
            return index;
    return std::nullopt;
}

VkDeviceAddress align_address(const VkDeviceAddress address, const VkDeviceSize alignment) {
    if (alignment <= 1U) return address;
    const auto remainder = address % alignment;
    return remainder == 0U ? address : address + alignment - remainder;
}

std::string_view image_layout_name(const VkImageLayout layout) noexcept {
    switch (layout) {
    case VK_IMAGE_LAYOUT_UNDEFINED: return "undefined";
    case VK_IMAGE_LAYOUT_GENERAL: return "general";
    case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL: return "shader-read-only-optimal";
    case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL: return "transfer-src-optimal";
    default: return "other";
    }
}

bool require_instance_functions(const InstanceFunctions& functions) {
    return functions.destroy_instance != nullptr && functions.enumerate_physical_devices != nullptr &&
           functions.enumerate_device_extension_properties != nullptr &&
           functions.get_physical_device_features2 != nullptr &&
           functions.get_physical_device_properties2 != nullptr &&
           functions.get_physical_device_properties != nullptr &&
           functions.get_physical_device_memory_properties != nullptr &&
           functions.get_physical_device_queue_family_properties != nullptr &&
           functions.get_physical_device_format_properties != nullptr &&
           functions.get_device_proc_addr != nullptr && functions.create_device != nullptr;
}

bool require_borrowed_instance_functions(const InstanceFunctions& functions) {
    return functions.enumerate_device_extension_properties != nullptr &&
           functions.get_physical_device_features2 != nullptr &&
           functions.get_physical_device_properties2 != nullptr &&
           functions.get_physical_device_properties != nullptr &&
           functions.get_physical_device_memory_properties != nullptr &&
           functions.get_physical_device_queue_family_properties != nullptr &&
           functions.get_physical_device_format_properties != nullptr &&
           functions.get_device_proc_addr != nullptr;
}

bool require_device_functions(const DeviceFunctions& functions, const bool borrowed_device) {
    return (borrowed_device || functions.get_device_queue != nullptr) &&
           (borrowed_device || functions.destroy_device != nullptr) &&
           (borrowed_device || functions.device_wait_idle != nullptr) &&
           functions.create_buffer != nullptr &&
           functions.destroy_buffer != nullptr && functions.get_buffer_memory_requirements != nullptr &&
           functions.allocate_memory != nullptr && functions.free_memory != nullptr &&
           functions.bind_buffer_memory != nullptr && functions.map_memory != nullptr &&
           functions.unmap_memory != nullptr && functions.get_buffer_device_address != nullptr &&
           functions.create_image != nullptr && functions.destroy_image != nullptr &&
           functions.get_image_memory_requirements != nullptr && functions.bind_image_memory != nullptr &&
           functions.create_image_view != nullptr && functions.destroy_image_view != nullptr &&
           functions.create_acceleration_structure != nullptr &&
           functions.destroy_acceleration_structure != nullptr &&
           functions.get_acceleration_structure_build_sizes != nullptr &&
           functions.get_acceleration_structure_device_address != nullptr &&
           functions.cmd_build_acceleration_structures != nullptr &&
           functions.create_command_pool != nullptr && functions.destroy_command_pool != nullptr &&
           functions.allocate_command_buffers != nullptr && functions.begin_command_buffer != nullptr &&
           functions.end_command_buffer != nullptr && functions.cmd_pipeline_barrier != nullptr &&
           functions.create_fence != nullptr && functions.destroy_fence != nullptr &&
           functions.reset_fences != nullptr && functions.wait_for_fences != nullptr &&
           functions.queue_submit != nullptr && functions.create_shader_module != nullptr &&
           functions.destroy_shader_module != nullptr && functions.create_descriptor_set_layout != nullptr &&
           functions.destroy_descriptor_set_layout != nullptr && functions.create_pipeline_layout != nullptr &&
           functions.destroy_pipeline_layout != nullptr && functions.create_descriptor_pool != nullptr &&
           functions.destroy_descriptor_pool != nullptr && functions.allocate_descriptor_sets != nullptr &&
           functions.update_descriptor_sets != nullptr && functions.create_ray_tracing_pipelines != nullptr &&
           functions.destroy_pipeline != nullptr && functions.get_ray_tracing_shader_group_handles != nullptr &&
           functions.cmd_bind_pipeline != nullptr && functions.cmd_bind_descriptor_sets != nullptr &&
           functions.cmd_trace_rays != nullptr;
}

bool create_buffer(const DeviceFunctions& functions,
                   const VkDevice device,
                   const VkPhysicalDeviceMemoryProperties& properties,
                   const VkDeviceSize size,
                   const VkBufferUsageFlags usage,
                   const VkMemoryPropertyFlags memory_flags,
                   const void* initial_data,
                   const std::size_t initial_bytes,
                   BufferResource& result,
                   std::string& error_code,
                   std::string& error_detail) {
    result = {};
    if (size == 0U || initial_bytes > static_cast<std::size_t>(size)) {
        error_code = "native-vulkan-rt.buffer-size-invalid";
        error_detail = "The persistent Vulkan buffer size is invalid.";
        return false;
    }
    VkBufferCreateInfo buffer_info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    buffer_info.size = size;
    buffer_info.usage = usage;
    buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (functions.create_buffer(device, &buffer_info, nullptr, &result.buffer) != VK_SUCCESS) {
        error_code = "native-vulkan-rt.create-buffer-failed";
        error_detail = "vkCreateBuffer failed.";
        return false;
    }
    VkMemoryRequirements requirements{};
    functions.get_buffer_memory_requirements(device, result.buffer, &requirements);
    const auto memory_type = find_memory_type(properties, requirements.memoryTypeBits, memory_flags);
    if (!memory_type) {
        functions.destroy_buffer(device, result.buffer, nullptr);
        result = {};
        error_code = "native-vulkan-rt.memory-type-unavailable";
        error_detail = "No compatible Vulkan memory type satisfied the persistent buffer flags.";
        return false;
    }
    VkMemoryAllocateFlagsInfo address_flags{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO};
    address_flags.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
    VkMemoryAllocateInfo allocate_info{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocate_info.pNext = &address_flags;
    allocate_info.allocationSize = requirements.size;
    allocate_info.memoryTypeIndex = *memory_type;
    if (functions.allocate_memory(device, &allocate_info, nullptr, &result.memory) != VK_SUCCESS) {
        functions.destroy_buffer(device, result.buffer, nullptr);
        result = {};
        error_code = "native-vulkan-rt.allocate-buffer-memory-failed";
        error_detail = "vkAllocateMemory failed for a persistent Vulkan buffer.";
        return false;
    }
    result.allocation_size = requirements.size;
    if (functions.bind_buffer_memory(device, result.buffer, result.memory, 0U) != VK_SUCCESS) {
        functions.free_memory(device, result.memory, nullptr);
        functions.destroy_buffer(device, result.buffer, nullptr);
        result = {};
        error_code = "native-vulkan-rt.bind-buffer-memory-failed";
        error_detail = "vkBindBufferMemory failed.";
        return false;
    }
    if (initial_data != nullptr && initial_bytes > 0U) {
        void* mapped = nullptr;
        if (functions.map_memory(device, result.memory, 0U, initial_bytes, 0U, &mapped) != VK_SUCCESS ||
            mapped == nullptr) {
            functions.free_memory(device, result.memory, nullptr);
            functions.destroy_buffer(device, result.buffer, nullptr);
            result = {};
            error_code = "native-vulkan-rt.map-buffer-memory-failed";
            error_detail = "vkMapMemory failed for persistent host-visible data.";
            return false;
        }
        std::memcpy(mapped, initial_data, initial_bytes);
        functions.unmap_memory(device, result.memory);
    }
    VkBufferDeviceAddressInfo address_info{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
    address_info.buffer = result.buffer;
    result.device_address = functions.get_buffer_device_address(device, &address_info);
    if (result.device_address == 0U) {
        functions.free_memory(device, result.memory, nullptr);
        functions.destroy_buffer(device, result.buffer, nullptr);
        result = {};
        error_code = "native-vulkan-rt.buffer-device-address-unavailable";
        error_detail = "vkGetBufferDeviceAddress returned zero.";
        return false;
    }
    return true;
}

bool create_output_image(const InstanceFunctions& instance_functions,
                         const DeviceFunctions& device_functions,
                         const VkPhysicalDevice physical_device,
                         const VkDevice device,
                         const VkPhysicalDeviceMemoryProperties& memory_properties,
                         const std::uint32_t width,
                         const std::uint32_t height,
                         const std::uint32_t depth,
                         ImageResource& result,
                         std::string& error_code,
                         std::string& error_detail) {
    result = {};
    if (physical_device == VK_NULL_HANDLE || device == VK_NULL_HANDLE || width == 0U || height == 0U ||
        depth == 0U || instance_functions.get_physical_device_format_properties == nullptr) {
        error_code = "native-vulkan-rt.output-image-contract-invalid";
        error_detail = "The runtime-private Vulkan output image arguments are invalid.";
        return false;
    }
    VkFormatProperties format_properties{};
    instance_functions.get_physical_device_format_properties(
        physical_device, VK_FORMAT_R32_UINT, &format_properties);
    if ((format_properties.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) == 0U) {
        error_code = "native-vulkan-rt.output-image-format-unsupported";
        error_detail = "VK_FORMAT_R32_UINT is not available for an optimal-tiled storage image.";
        return false;
    }
    VkImageCreateInfo image_info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    image_info.imageType = depth > 1U ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D;
    image_info.format = VK_FORMAT_R32_UINT;
    image_info.extent = {width, height, depth};
    image_info.mipLevels = 1U;
    image_info.arrayLayers = 1U;
    image_info.samples = VK_SAMPLE_COUNT_1_BIT;
    image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
    image_info.usage = VK_IMAGE_USAGE_STORAGE_BIT;
    image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (device_functions.create_image(device, &image_info, nullptr, &result.image) != VK_SUCCESS ||
        result.image == VK_NULL_HANDLE) {
        error_code = "native-vulkan-rt.create-output-image-failed";
        error_detail = "vkCreateImage failed for the runtime-private Vulkan output image.";
        return false;
    }
    VkMemoryRequirements requirements{};
    device_functions.get_image_memory_requirements(device, result.image, &requirements);
    const auto memory_type = find_memory_type(
        memory_properties, requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (!memory_type) {
        device_functions.destroy_image(device, result.image, nullptr);
        result = {};
        error_code = "native-vulkan-rt.output-image-memory-type-unavailable";
        error_detail = "No device-local memory type can back the runtime-private output image.";
        return false;
    }
    VkMemoryAllocateInfo allocate_info{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocate_info.allocationSize = requirements.size;
    allocate_info.memoryTypeIndex = *memory_type;
    if (device_functions.allocate_memory(device, &allocate_info, nullptr, &result.memory) != VK_SUCCESS ||
        result.memory == VK_NULL_HANDLE) {
        device_functions.destroy_image(device, result.image, nullptr);
        result = {};
        error_code = "native-vulkan-rt.allocate-output-image-memory-failed";
        error_detail = "vkAllocateMemory failed for the runtime-private output image.";
        return false;
    }
    result.allocation_size = requirements.size;
    if (device_functions.bind_image_memory(device, result.image, result.memory, 0U) != VK_SUCCESS) {
        device_functions.free_memory(device, result.memory, nullptr);
        device_functions.destroy_image(device, result.image, nullptr);
        result = {};
        error_code = "native-vulkan-rt.bind-output-image-memory-failed";
        error_detail = "vkBindImageMemory failed for the runtime-private output image.";
        return false;
    }
    VkImageViewCreateInfo view_info{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    view_info.image = result.image;
    view_info.viewType = depth > 1U ? VK_IMAGE_VIEW_TYPE_3D : VK_IMAGE_VIEW_TYPE_2D;
    view_info.format = VK_FORMAT_R32_UINT;
    view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    view_info.subresourceRange.baseMipLevel = 0U;
    view_info.subresourceRange.levelCount = 1U;
    view_info.subresourceRange.baseArrayLayer = 0U;
    view_info.subresourceRange.layerCount = 1U;
    if (device_functions.create_image_view(device, &view_info, nullptr, &result.view) != VK_SUCCESS ||
        result.view == VK_NULL_HANDLE) {
        device_functions.free_memory(device, result.memory, nullptr);
        device_functions.destroy_image(device, result.image, nullptr);
        result = {};
        error_code = "native-vulkan-rt.create-output-image-view-failed";
        error_detail = "vkCreateImageView failed for the runtime-private output image.";
        return false;
    }
    return true;
}

bool write_buffer(const DeviceFunctions& functions,
                  const VkDevice device,
                  const BufferResource& buffer,
                  const void* data,
                  const std::size_t bytes,
                  std::string& error_code,
                  std::string& error_detail) {
    if (data == nullptr || bytes == 0U || bytes > static_cast<std::size_t>(buffer.allocation_size)) {
        error_code = "native-vulkan-rt.buffer-write-invalid";
        error_detail = "The persistent geometry update exceeds its allocation.";
        return false;
    }
    void* mapped = nullptr;
    if (functions.map_memory(device, buffer.memory, 0U, bytes, 0U, &mapped) != VK_SUCCESS || mapped == nullptr) {
        error_code = "native-vulkan-rt.map-buffer-update-failed";
        error_detail = "vkMapMemory failed for the persistent geometry update.";
        return false;
    }
    std::memcpy(mapped, data, bytes);
    functions.unmap_memory(device, buffer.memory);
    return true;
}

bool create_acceleration_structure(const DeviceFunctions& functions,
                                   const VkDevice device,
                                   const VkBuffer storage_buffer,
                                   const VkDeviceSize size,
                                   const VkAccelerationStructureTypeKHR type,
                                   AccelerationStructureResource& result,
                                   std::string& error_code,
                                   std::string& error_detail) {
    result = {};
    VkAccelerationStructureCreateInfoKHR info{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR};
    info.buffer = storage_buffer;
    info.size = size;
    info.type = type;
    if (functions.create_acceleration_structure(device, &info, nullptr, &result.acceleration_structure) != VK_SUCCESS) {
        error_code = "native-vulkan-rt.create-acceleration-structure-failed";
        error_detail = "vkCreateAccelerationStructureKHR failed.";
        return false;
    }
    VkAccelerationStructureDeviceAddressInfoKHR address_info{
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR};
    address_info.accelerationStructure = result.acceleration_structure;
    result.device_address = functions.get_acceleration_structure_device_address(device, &address_info);
    if (result.device_address == 0U) {
        functions.destroy_acceleration_structure(device, result.acceleration_structure, nullptr);
        result = {};
        error_code = "native-vulkan-rt.acceleration-structure-address-unavailable";
        error_detail = "vkGetAccelerationStructureDeviceAddressKHR returned zero.";
        return false;
    }
    return true;
}

#endif

} // namespace

#if NOEMANCER_HAS_VULKAN_HEADERS

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
    bool build_submitted{};
    bool build_completed{};
    bool trace_submitted{};
    bool borrowed_device{};
    bool output_image_sync_complete{};
    bool output_image_trace_written{};
    std::uint64_t output_image_generation{};
    std::uint64_t output_image_generation_serial{};
    std::uint64_t output_image_sync_value{};
    VkImageLayout output_image_layout{VK_IMAGE_LAYOUT_UNDEFINED};
    std::uint64_t generation{};
    std::uint64_t scene_topology_revision{};
    std::uint64_t scene_content_revision{};
    std::uint64_t scene_fingerprint{};
    std::uint32_t output_value{};
    std::uint64_t output_hash{};
    std::vector<NativeVulkanRayTracingTriangle> triangles;

    VulkanModule module;
    PFN_vkGetInstanceProcAddr get_instance_proc_addr{};
    InstanceFunctions instance_functions;
    DeviceFunctions device_functions;
    VkInstance instance{};
    VkPhysicalDevice physical_device{};
    VkDevice device{};
    VkQueue queue{};
    VkCommandPool command_pool{};
    VkCommandBuffer command_buffer{};
    VkFence fence{};
    VkPhysicalDeviceMemoryProperties memory_properties{};
    VkPhysicalDeviceAccelerationStructurePropertiesKHR acceleration_properties{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR};
    VkPhysicalDeviceRayTracingPipelinePropertiesKHR ray_tracing_properties{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_PROPERTIES_KHR};
    std::uint32_t queue_family_index{};
    VkDeviceSize scratch_alignment{256U};

    BufferResource vertex_buffer;
    BufferResource instance_buffer;
    BufferResource blas_result_buffer;
    BufferResource blas_scratch_buffer;
    BufferResource tlas_result_buffer;
    BufferResource tlas_scratch_buffer;
    BufferResource output_buffer;
    ImageResource output_image;
    BufferResource sbt_buffer;
    AccelerationStructureResource blas;
    AccelerationStructureResource tlas;
    std::uint32_t native_primitive_count{};
    std::size_t native_vertex_bytes{};
    VkDeviceSize blas_scratch_bytes{};
    VkDeviceSize tlas_scratch_bytes{};
    VkDeviceSize sbt_stride{};
    VkDeviceSize sbt_bytes{};
    VkShaderModule shader_module{};
    VkDescriptorSetLayout descriptor_set_layout{};
    VkPipelineLayout pipeline_layout{};
    VkDescriptorPool descriptor_pool{};
    VkDescriptorSet descriptor_set{};
    VkPipeline pipeline{};
    bool trace_pipeline_ready{};
    bool output_readback_available{};
    bool native_scene_allocated{};
    bool native_scene_dirty{};
    bool native_scene_requires_rebuild{};
    // Tracks whether the resident native BLAS/TLAS contain the latest scene.
    // build_completed is an operation receipt and is intentionally cleared by
    // a cached frame; this bit keeps trace legal across those frame calls.
    bool native_scene_built{};

    explicit Impl(const NativeVulkanRayTracingContextOptions& source) : options(source) {
        options.maximum_triangles = std::min(options.maximum_triangles,
                                             native_vulkan_raytracing_context_hard_max_triangles);
        options.output_width = std::clamp(options.output_width, 1U, 4096U);
        options.output_height = std::clamp(options.output_height, 1U, 4096U);
        options.output_depth = std::clamp(options.output_depth, 1U, 64U);
        triangles.reserve(std::min(options.maximum_triangles, std::size_t{256U}));
    }

    [[nodiscard]] bool borrowed_device_requested() const noexcept {
        const auto& borrowed = options.borrowed_device;
        return borrowed.instance != nullptr || borrowed.physical_device != nullptr ||
               borrowed.device != nullptr || borrowed.queue != nullptr || borrowed.queue_family_index != 0U;
    }

    [[nodiscard]] bool borrowed_device_complete() const noexcept {
        const auto& borrowed = options.borrowed_device;
        return borrowed.instance != nullptr && borrowed.physical_device != nullptr &&
               borrowed.device != nullptr && borrowed.queue != nullptr;
    }

    void destroy_buffer(BufferResource& resource) noexcept {
        if (device != VK_NULL_HANDLE && resource.buffer != VK_NULL_HANDLE && device_functions.destroy_buffer != nullptr)
            device_functions.destroy_buffer(device, resource.buffer, nullptr);
        if (device != VK_NULL_HANDLE && resource.memory != VK_NULL_HANDLE && device_functions.free_memory != nullptr)
            device_functions.free_memory(device, resource.memory, nullptr);
        resource = {};
    }

    void destroy_scene_native() noexcept {
        if (device != VK_NULL_HANDLE && device_functions.destroy_acceleration_structure != nullptr) {
            if (tlas.acceleration_structure != VK_NULL_HANDLE)
                device_functions.destroy_acceleration_structure(device, tlas.acceleration_structure, nullptr);
            if (blas.acceleration_structure != VK_NULL_HANDLE)
                device_functions.destroy_acceleration_structure(device, blas.acceleration_structure, nullptr);
        }
        tlas = {};
        blas = {};
        destroy_buffer(tlas_scratch_buffer);
        destroy_buffer(tlas_result_buffer);
        destroy_buffer(instance_buffer);
        destroy_buffer(blas_scratch_buffer);
        destroy_buffer(blas_result_buffer);
        destroy_buffer(vertex_buffer);
        native_primitive_count = 0U;
        native_vertex_bytes = 0U;
        blas_scratch_bytes = 0U;
        tlas_scratch_bytes = 0U;
        native_scene_allocated = false;
        native_scene_dirty = false;
        native_scene_requires_rebuild = false;
        native_scene_built = false;
    }

    void destroy_output_image() noexcept {
        if (device != VK_NULL_HANDLE && output_image.view != VK_NULL_HANDLE &&
            device_functions.destroy_image_view != nullptr)
            device_functions.destroy_image_view(device, output_image.view, nullptr);
        if (device != VK_NULL_HANDLE && output_image.image != VK_NULL_HANDLE &&
            device_functions.destroy_image != nullptr)
            device_functions.destroy_image(device, output_image.image, nullptr);
        if (device != VK_NULL_HANDLE && output_image.memory != VK_NULL_HANDLE &&
            device_functions.free_memory != nullptr)
            device_functions.free_memory(device, output_image.memory, nullptr);
        output_image = {};
        output_image_layout = VK_IMAGE_LAYOUT_UNDEFINED;
        output_image_generation = 0U;
        output_image_sync_value = 0U;
        output_image_sync_complete = false;
        output_image_trace_written = false;
    }

    void destroy_trace_objects() noexcept {
        if (device != VK_NULL_HANDLE && pipeline != VK_NULL_HANDLE &&
            device_functions.destroy_pipeline != nullptr)
            device_functions.destroy_pipeline(device, pipeline, nullptr);
        pipeline = VK_NULL_HANDLE;
        if (device != VK_NULL_HANDLE && descriptor_pool != VK_NULL_HANDLE &&
            device_functions.destroy_descriptor_pool != nullptr)
            device_functions.destroy_descriptor_pool(device, descriptor_pool, nullptr);
        descriptor_pool = VK_NULL_HANDLE;
        descriptor_set = VK_NULL_HANDLE;
        if (device != VK_NULL_HANDLE && pipeline_layout != VK_NULL_HANDLE &&
            device_functions.destroy_pipeline_layout != nullptr)
            device_functions.destroy_pipeline_layout(device, pipeline_layout, nullptr);
        pipeline_layout = VK_NULL_HANDLE;
        if (device != VK_NULL_HANDLE && descriptor_set_layout != VK_NULL_HANDLE &&
            device_functions.destroy_descriptor_set_layout != nullptr)
            device_functions.destroy_descriptor_set_layout(device, descriptor_set_layout, nullptr);
        descriptor_set_layout = VK_NULL_HANDLE;
        if (device != VK_NULL_HANDLE && shader_module != VK_NULL_HANDLE &&
            device_functions.destroy_shader_module != nullptr)
            device_functions.destroy_shader_module(device, shader_module, nullptr);
        shader_module = VK_NULL_HANDLE;
        destroy_buffer(sbt_buffer);
        sbt_stride = 0U;
        sbt_bytes = 0U;
        trace_pipeline_ready = false;
    }

    void destroy_native() noexcept {
        // A borrowed SDL_GPU device is not owned by this context.  Every
        // submission made by this context is synchronously fenced before the
        // corresponding resources are destroyed, so do not impose a global
        // vkDeviceWaitIdle on the host application's queue/device.
        if (!borrowed_device && device != VK_NULL_HANDLE && device_functions.device_wait_idle != nullptr)
            static_cast<void>(device_functions.device_wait_idle(device));
        destroy_trace_objects();
        destroy_scene_native();
        destroy_output_image();
        destroy_buffer(output_buffer);
        if (device != VK_NULL_HANDLE && fence != VK_NULL_HANDLE && device_functions.destroy_fence != nullptr)
            device_functions.destroy_fence(device, fence, nullptr);
        fence = VK_NULL_HANDLE;
        if (device != VK_NULL_HANDLE && command_pool != VK_NULL_HANDLE && device_functions.destroy_command_pool != nullptr)
            device_functions.destroy_command_pool(device, command_pool, nullptr);
        command_pool = VK_NULL_HANDLE;
        command_buffer = VK_NULL_HANDLE;
        if (!borrowed_device && device != VK_NULL_HANDLE && device_functions.destroy_device != nullptr)
            device_functions.destroy_device(device, nullptr);
        device = VK_NULL_HANDLE;
        queue = VK_NULL_HANDLE;
        physical_device = VK_NULL_HANDLE;
        if (!borrowed_device && instance != VK_NULL_HANDLE && instance_functions.destroy_instance != nullptr)
            instance_functions.destroy_instance(instance, nullptr);
        instance = VK_NULL_HANDLE;
        get_instance_proc_addr = nullptr;
        instance_functions = {};
        device_functions = {};
        memory_properties = {};
        queue_family_index = 0U;
        borrowed_device = false;
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
        result.persistent_backend = device != VK_NULL_HANDLE && state != NativeVulkanRayTracingContextState::fallback;
        result.fallback_active = result_state == NativeVulkanRayTracingContextState::fallback ||
                                 state == NativeVulkanRayTracingContextState::fallback;
        result.scene_ready = scene_ready;
        result.scene_rebuilt = scene_rebuilt;
        result.scene_updated = scene_updated;
        result.scene_reused = scene_reused;
        result.build_submitted = build_submitted;
        result.build_completed = build_completed;
        result.trace_submitted = trace_submitted;
        result.trace_completed = trace_completed;
        result.readback_completed = readback_completed;
        result.output_image_live = output_image.image != VK_NULL_HANDLE;
        result.output_image_view_live = output_image.view != VK_NULL_HANDLE;
        result.output_image_runtime_private = result.output_image_live;
        // SDL_GPU owns a separate device in the current runtime.  Until a
        // same-device adapter is explicitly supplied, no image is advertised
        // as externally importable and no native handle leaves this PImpl.
        result.output_image_interop_ready = false;
        result.output_image_external_import_supported = false;
        result.output_image_same_device_required = result.output_image_live;
        result.output_image_layout_ready = result.output_image_live &&
                                           output_image_layout == VK_IMAGE_LAYOUT_GENERAL;
        result.output_image_sync_complete = result.output_image_live && output_image_sync_complete;
        result.output_image_trace_written = output_image_trace_written;
        result.output_image_cpu_readback_supported = false;
        result.resources_live = device != VK_NULL_HANDLE && instance != VK_NULL_HANDLE;
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
        result.output_image_queue_family = result.output_image_live ? queue_family_index : 0U;
        result.output_image_generation = result.output_image_live ? output_image_generation : 0U;
        result.output_image_bytes = result.output_image_live ? output_image.allocation_size : 0U;
        result.output_image_sync_value = result.output_image_live ? output_image_sync_value : 0U;
        result.output_image_format = result.output_image_live ? "r32_uint" : "none";
        result.output_image_layout = result.output_image_live ? std::string(image_layout_name(output_image_layout)) : "none";
        result.output_image_access = result.output_image_live ? "storage-read-write" : "none";
        result.output_image_sync_kind = result.output_image_live ? "fence" : "none";
        result.output_image_interop_boundary = result.output_image_live
                                                   ? "runtime-private; same-device adapter only; no handle export"
                                                   : "unavailable";
        return result;
    }

    NativeVulkanRayTracingContextReceipt not_ready(
        const NativeVulkanRayTracingContextFailureStage stage,
        const std::string_view code,
        const std::string_view detail) const {
        return receipt(NativeVulkanRayTracingContextState::error, stage, code, detail);
    }

    bool initialize_native(std::string& error_code, std::string& error_detail, bool& unsupported) {
        unsupported = false;
        if (!module.load()) {
            unsupported = true;
            error_code = "native-vulkan-rt.loader-unavailable";
            error_detail = "The Vulkan loader could not be loaded.";
            return false;
        }
        get_instance_proc_addr = reinterpret_cast<PFN_vkGetInstanceProcAddr>(module.symbol("vkGetInstanceProcAddr"));
        if (get_instance_proc_addr == nullptr) {
            unsupported = true;
            error_code = "native-vulkan-rt.entrypoint-unavailable";
            error_detail = "vkGetInstanceProcAddr is missing from the Vulkan loader.";
            return false;
        }
        const auto enumerate_instance_version = load_global<PFN_vkEnumerateInstanceVersion>(
            module, get_instance_proc_addr, "vkEnumerateInstanceVersion");
        std::uint32_t loader_api = VK_API_VERSION_1_0;
        if (enumerate_instance_version != nullptr) {
            std::uint32_t queried_api = VK_API_VERSION_1_0;
            if (enumerate_instance_version(&queried_api) == VK_SUCCESS) loader_api = queried_api;
        }
        if (loader_api < VK_API_VERSION_1_1) {
            unsupported = true;
            error_code = "native-vulkan-rt.vulkan-version-unsupported";
            error_detail = "Persistent acceleration structures require a Vulkan 1.1-capable loader.";
            return false;
        }
        VkApplicationInfo application_info{VK_STRUCTURE_TYPE_APPLICATION_INFO};
        application_info.pApplicationName = "Noemancer Native RHI";
        application_info.applicationVersion = VK_MAKE_VERSION(0, 2, 0);
        application_info.pEngineName = "Noemancer";
        application_info.engineVersion = VK_MAKE_VERSION(0, 2, 0);
        application_info.apiVersion = VK_API_VERSION_1_1;
        VkInstanceCreateInfo instance_info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
        instance_info.pApplicationInfo = &application_info;
        const auto borrowed_requested = borrowed_device_requested();
        if (borrowed_requested) {
            if (!borrowed_device_complete()) {
                unsupported = true;
                error_code = "native-vulkan-rt.shared-device-incomplete";
                error_detail = "Borrowed SDL_GPU Vulkan adoption requires instance, physical device, device and queue handles.";
                return false;
            }
            instance = reinterpret_cast<VkInstance>(options.borrowed_device.instance);
            physical_device = reinterpret_cast<VkPhysicalDevice>(options.borrowed_device.physical_device);
            device = reinterpret_cast<VkDevice>(options.borrowed_device.device);
            queue = reinterpret_cast<VkQueue>(options.borrowed_device.queue);
            queue_family_index = options.borrowed_device.queue_family_index;
            borrowed_device = true;
        } else {
            const auto create_instance = load_global<PFN_vkCreateInstance>(
                module, get_instance_proc_addr, "vkCreateInstance");
            if (create_instance == nullptr) {
                unsupported = true;
                error_code = "native-vulkan-rt.create-instance-entrypoint-unavailable";
                error_detail = "vkCreateInstance is missing from the Vulkan loader.";
                return false;
            }
            if (create_instance(&instance_info, nullptr, &instance) != VK_SUCCESS || instance == VK_NULL_HANDLE) {
                error_code = "native-vulkan-rt.create-instance-failed";
                error_detail = "vkCreateInstance failed.";
                return false;
            }
        }
        instance_functions.destroy_instance = load_instance<PFN_vkDestroyInstance>(get_instance_proc_addr, instance, "vkDestroyInstance");
        instance_functions.enumerate_physical_devices = load_instance<PFN_vkEnumeratePhysicalDevices>(get_instance_proc_addr, instance, "vkEnumeratePhysicalDevices");
        instance_functions.enumerate_device_extension_properties = load_instance<PFN_vkEnumerateDeviceExtensionProperties>(get_instance_proc_addr, instance, "vkEnumerateDeviceExtensionProperties");
        instance_functions.get_physical_device_features2 = load_instance<PFN_vkGetPhysicalDeviceFeatures2>(get_instance_proc_addr, instance, "vkGetPhysicalDeviceFeatures2");
        if (instance_functions.get_physical_device_features2 == nullptr)
            instance_functions.get_physical_device_features2 =
                load_instance<PFN_vkGetPhysicalDeviceFeatures2KHR>(
                    get_instance_proc_addr, instance, "vkGetPhysicalDeviceFeatures2KHR");
        instance_functions.get_physical_device_properties2 = load_instance<PFN_vkGetPhysicalDeviceProperties2>(get_instance_proc_addr, instance, "vkGetPhysicalDeviceProperties2");
        if (instance_functions.get_physical_device_properties2 == nullptr)
            instance_functions.get_physical_device_properties2 =
                load_instance<PFN_vkGetPhysicalDeviceProperties2KHR>(
                    get_instance_proc_addr, instance, "vkGetPhysicalDeviceProperties2KHR");
        instance_functions.get_physical_device_properties = load_instance<PFN_vkGetPhysicalDeviceProperties>(get_instance_proc_addr, instance, "vkGetPhysicalDeviceProperties");
        instance_functions.get_physical_device_memory_properties = load_instance<PFN_vkGetPhysicalDeviceMemoryProperties>(get_instance_proc_addr, instance, "vkGetPhysicalDeviceMemoryProperties");
        instance_functions.get_physical_device_queue_family_properties = load_instance<PFN_vkGetPhysicalDeviceQueueFamilyProperties>(get_instance_proc_addr, instance, "vkGetPhysicalDeviceQueueFamilyProperties");
        instance_functions.get_physical_device_format_properties = load_instance<PFN_vkGetPhysicalDeviceFormatProperties>(get_instance_proc_addr, instance, "vkGetPhysicalDeviceFormatProperties");
        instance_functions.get_device_proc_addr = load_instance<PFN_vkGetDeviceProcAddr>(get_instance_proc_addr, instance, "vkGetDeviceProcAddr");
        instance_functions.create_device = load_instance<PFN_vkCreateDevice>(get_instance_proc_addr, instance, "vkCreateDevice");
        if ((borrowed_requested && !require_borrowed_instance_functions(instance_functions)) ||
            (!borrowed_requested && !require_instance_functions(instance_functions))) {
            unsupported = true;
            error_code = borrowed_requested ? "native-vulkan-rt.shared-device-unsupported"
                                            : "native-vulkan-rt.instance-query-entrypoint-unavailable";
            error_detail = borrowed_requested
                               ? "The borrowed SDL_GPU instance lacks a required Vulkan RT query entry point."
                               : "The Vulkan instance lacks a required physical-device query entry point.";
            return false;
        }
        if (borrowed_requested) {
            std::uint32_t extension_count = 0U;
            auto extension_result = instance_functions.enumerate_device_extension_properties(
                physical_device, nullptr, &extension_count, nullptr);
            if (extension_result != VK_SUCCESS && extension_result != VK_INCOMPLETE) {
                unsupported = true;
                error_code = "native-vulkan-rt.shared-device-unsupported";
                error_detail = "The borrowed physical device could not expose its device extensions.";
                return false;
            }
            extension_count = std::min(extension_count, 256U);
            std::vector<VkExtensionProperties> extensions(extension_count);
            if (extension_count > 0U) {
                extension_result = instance_functions.enumerate_device_extension_properties(
                    physical_device, nullptr, &extension_count, extensions.data());
                if (extension_result != VK_SUCCESS && extension_result != VK_INCOMPLETE) {
                    unsupported = true;
                    error_code = "native-vulkan-rt.shared-device-unsupported";
                    error_detail = "The borrowed physical device extension query was incomplete.";
                    return false;
                }
            }
            const bool bda_extension = has_extension(extensions, VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME);
            VkPhysicalDeviceProperties borrowed_properties{};
            instance_functions.get_physical_device_properties(physical_device, &borrowed_properties);
            if (!has_extension(extensions, VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME) ||
                !has_extension(extensions, VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME) ||
                !has_extension(extensions, VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME) ||
                !has_extension(extensions, VK_KHR_SPIRV_1_4_EXTENSION_NAME) ||
                !has_extension(extensions, VK_KHR_SHADER_FLOAT_CONTROLS_EXTENSION_NAME) ||
                (!bda_extension && borrowed_properties.apiVersion < VK_API_VERSION_1_2)) {
                unsupported = true;
                error_code = "native-vulkan-rt.shared-device-unsupported";
                error_detail = "The borrowed SDL_GPU device was not created with the required Vulkan RT extensions.";
                return false;
            }
            VkPhysicalDeviceBufferDeviceAddressFeatures bda{
                VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES};
            VkPhysicalDeviceAccelerationStructureFeaturesKHR as{
                VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR};
            VkPhysicalDeviceRayTracingPipelineFeaturesKHR ray_pipeline{
                VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR};
            ray_pipeline.pNext = &as;
            as.pNext = &bda;
            VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
            features.pNext = &ray_pipeline;
            instance_functions.get_physical_device_features2(physical_device, &features);
            if (ray_pipeline.rayTracingPipeline != VK_TRUE || as.accelerationStructure != VK_TRUE ||
                bda.bufferDeviceAddress != VK_TRUE) {
                unsupported = true;
                error_code = "native-vulkan-rt.shared-device-unsupported";
                error_detail = "The borrowed SDL_GPU device does not expose the required Vulkan RT features.";
                return false;
            }
            std::uint32_t family_count = 0U;
            instance_functions.get_physical_device_queue_family_properties(
                physical_device, &family_count, nullptr);
            std::vector<VkQueueFamilyProperties> families(family_count);
            if (family_count > 0U)
                instance_functions.get_physical_device_queue_family_properties(
                    physical_device, &family_count, families.data());
            if (queue_family_index >= family_count ||
                (families[queue_family_index].queueFlags & VK_QUEUE_COMPUTE_BIT) == 0U) {
                unsupported = true;
                error_code = "native-vulkan-rt.shared-device-unsupported";
                error_detail = "The borrowed SDL_GPU queue family is not a valid compute queue for RT work.";
                return false;
            }
            instance_functions.get_physical_device_memory_properties(physical_device, &memory_properties);
            VkPhysicalDeviceProperties2 properties2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
            ray_tracing_properties = {
                VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_PROPERTIES_KHR};
            ray_tracing_properties.pNext = &acceleration_properties;
            properties2.pNext = &ray_tracing_properties;
            instance_functions.get_physical_device_properties2(physical_device, &properties2);
        }
        if (!borrowed_requested) {
        std::uint32_t device_count = 0U;
        auto enumerate_result = instance_functions.enumerate_physical_devices(instance, &device_count, nullptr);
        if (enumerate_result != VK_SUCCESS && enumerate_result != VK_INCOMPLETE) {
            error_code = "native-vulkan-rt.enumerate-physical-devices-failed";
            error_detail = "vkEnumeratePhysicalDevices failed.";
            return false;
        }
        device_count = std::min(device_count, 64U);
        std::vector<VkPhysicalDevice> devices(device_count);
        if (device_count > 0U) {
            enumerate_result = instance_functions.enumerate_physical_devices(instance, &device_count, devices.data());
            if (enumerate_result != VK_SUCCESS && enumerate_result != VK_INCOMPLETE) {
                error_code = "native-vulkan-rt.enumerate-physical-devices-failed";
                error_detail = "vkEnumeratePhysicalDevices failed while reading handles.";
                return false;
            }
        }
        SelectedPhysicalDevice selected;
        bool found = false;
        for (const auto candidate : devices) {
            std::uint32_t extension_count = 0U;
            auto extension_result = instance_functions.enumerate_device_extension_properties(candidate, nullptr, &extension_count, nullptr);
            if (extension_result != VK_SUCCESS && extension_result != VK_INCOMPLETE) continue;
            extension_count = std::min(extension_count, 256U);
            std::vector<VkExtensionProperties> extensions(extension_count);
            if (extension_count > 0U) {
                extension_result = instance_functions.enumerate_device_extension_properties(candidate, nullptr, &extension_count, extensions.data());
                if (extension_result != VK_SUCCESS && extension_result != VK_INCOMPLETE) continue;
            }
            if (!has_extension(extensions, VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME) ||
                !has_extension(extensions, VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME) ||
                !has_extension(extensions, VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME) ||
                !has_extension(extensions, VK_KHR_SPIRV_1_4_EXTENSION_NAME) ||
                !has_extension(extensions, VK_KHR_SHADER_FLOAT_CONTROLS_EXTENSION_NAME)) continue;
            VkPhysicalDeviceProperties candidate_properties{};
            instance_functions.get_physical_device_properties(candidate, &candidate_properties);
            const bool bda_extension = has_extension(extensions, VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME);
            if (!bda_extension && candidate_properties.apiVersion < VK_API_VERSION_1_2) continue;
            VkPhysicalDeviceBufferDeviceAddressFeatures bda{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES};
            VkPhysicalDeviceAccelerationStructureFeaturesKHR as{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR};
            VkPhysicalDeviceRayTracingPipelineFeaturesKHR ray_pipeline{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR};
            ray_pipeline.pNext = &as;
            as.pNext = &bda;
            VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
            features.pNext = &ray_pipeline;
            instance_functions.get_physical_device_features2(candidate, &features);
            if (ray_pipeline.rayTracingPipeline != VK_TRUE || as.accelerationStructure != VK_TRUE ||
                bda.bufferDeviceAddress != VK_TRUE) continue;
            std::uint32_t family_count = 0U;
            instance_functions.get_physical_device_queue_family_properties(candidate, &family_count, nullptr);
            std::vector<VkQueueFamilyProperties> families(family_count);
            if (family_count > 0U)
                instance_functions.get_physical_device_queue_family_properties(candidate, &family_count, families.data());
            std::optional<std::uint32_t> family;
            for (std::uint32_t index = 0U; index < family_count; ++index) {
                if ((families[index].queueFlags & VK_QUEUE_COMPUTE_BIT) != 0U) {
                    family = index;
                    if ((families[index].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0U) break;
                }
            }
            if (!family) continue;
            selected.handle = candidate;
            selected.queue_family_index = *family;
            selected.properties = candidate_properties;
            instance_functions.get_physical_device_memory_properties(candidate, &selected.memory_properties);
            VkPhysicalDeviceProperties2 properties2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
            selected.ray_tracing_properties = {
                VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_PROPERTIES_KHR};
            selected.ray_tracing_properties.pNext = &selected.acceleration_properties;
            properties2.pNext = &selected.ray_tracing_properties;
            instance_functions.get_physical_device_properties2(candidate, &properties2);
            selected.buffer_device_address_extension = bda_extension;
            found = true;
            break;
        }
        if (!found) {
            unsupported = true;
            error_code = "native-vulkan-rt.ray-tracing-unsupported";
            error_detail = "No physical device exposed acceleration structures, buffer device address, and a compute queue.";
            return false;
        }
        physical_device = selected.handle;
        memory_properties = selected.memory_properties;
        acceleration_properties = selected.acceleration_properties;
        ray_tracing_properties = selected.ray_tracing_properties;
        queue_family_index = selected.queue_family_index;
        const float queue_priority = 1.0F;
        VkDeviceQueueCreateInfo queue_info{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        queue_info.queueFamilyIndex = queue_family_index;
        queue_info.queueCount = 1U;
        queue_info.pQueuePriorities = &queue_priority;
        std::vector<const char*> extensions{
            VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME,
            VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME,
            VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME,
            VK_KHR_SPIRV_1_4_EXTENSION_NAME,
            VK_KHR_SHADER_FLOAT_CONTROLS_EXTENSION_NAME};
        if (selected.buffer_device_address_extension)
            extensions.push_back(VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME);
        VkPhysicalDeviceBufferDeviceAddressFeatures enabled_bda{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES};
        enabled_bda.bufferDeviceAddress = VK_TRUE;
        VkPhysicalDeviceAccelerationStructureFeaturesKHR enabled_as{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR};
        enabled_as.accelerationStructure = VK_TRUE;
        VkPhysicalDeviceRayTracingPipelineFeaturesKHR enabled_ray_pipeline{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR};
        enabled_ray_pipeline.rayTracingPipeline = VK_TRUE;
        enabled_ray_pipeline.pNext = &enabled_as;
        enabled_as.pNext = &enabled_bda;
        VkPhysicalDeviceFeatures2 enabled_features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
        enabled_features.pNext = &enabled_ray_pipeline;
        VkDeviceCreateInfo device_info{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
        device_info.pNext = &enabled_features;
        device_info.queueCreateInfoCount = 1U;
        device_info.pQueueCreateInfos = &queue_info;
        device_info.enabledExtensionCount = static_cast<std::uint32_t>(extensions.size());
        device_info.ppEnabledExtensionNames = extensions.data();
        if (instance_functions.create_device(physical_device, &device_info, nullptr, &device) != VK_SUCCESS || device == VK_NULL_HANDLE) {
            error_code = "native-vulkan-rt.create-device-failed";
            error_detail = "vkCreateDevice failed for the acceleration-structure feature chain.";
            return false;
        }
        }
#define NOEMANCER_LOAD_DEVICE(member, type, name) \
        device_functions.member = load_device<type>(instance_functions.get_device_proc_addr, device, name)
        NOEMANCER_LOAD_DEVICE(get_device_queue, PFN_vkGetDeviceQueue, "vkGetDeviceQueue");
        NOEMANCER_LOAD_DEVICE(destroy_device, PFN_vkDestroyDevice, "vkDestroyDevice");
        NOEMANCER_LOAD_DEVICE(device_wait_idle, PFN_vkDeviceWaitIdle, "vkDeviceWaitIdle");
        NOEMANCER_LOAD_DEVICE(create_buffer, PFN_vkCreateBuffer, "vkCreateBuffer");
        NOEMANCER_LOAD_DEVICE(destroy_buffer, PFN_vkDestroyBuffer, "vkDestroyBuffer");
        NOEMANCER_LOAD_DEVICE(get_buffer_memory_requirements, PFN_vkGetBufferMemoryRequirements, "vkGetBufferMemoryRequirements");
        NOEMANCER_LOAD_DEVICE(allocate_memory, PFN_vkAllocateMemory, "vkAllocateMemory");
        NOEMANCER_LOAD_DEVICE(free_memory, PFN_vkFreeMemory, "vkFreeMemory");
        NOEMANCER_LOAD_DEVICE(bind_buffer_memory, PFN_vkBindBufferMemory, "vkBindBufferMemory");
        NOEMANCER_LOAD_DEVICE(map_memory, PFN_vkMapMemory, "vkMapMemory");
        NOEMANCER_LOAD_DEVICE(unmap_memory, PFN_vkUnmapMemory, "vkUnmapMemory");
        NOEMANCER_LOAD_DEVICE(get_buffer_device_address, PFN_vkGetBufferDeviceAddress, "vkGetBufferDeviceAddress");
        if (device_functions.get_buffer_device_address == nullptr)
            NOEMANCER_LOAD_DEVICE(get_buffer_device_address, PFN_vkGetBufferDeviceAddressKHR, "vkGetBufferDeviceAddressKHR");
        NOEMANCER_LOAD_DEVICE(create_image, PFN_vkCreateImage, "vkCreateImage");
        NOEMANCER_LOAD_DEVICE(destroy_image, PFN_vkDestroyImage, "vkDestroyImage");
        NOEMANCER_LOAD_DEVICE(get_image_memory_requirements, PFN_vkGetImageMemoryRequirements, "vkGetImageMemoryRequirements");
        NOEMANCER_LOAD_DEVICE(bind_image_memory, PFN_vkBindImageMemory, "vkBindImageMemory");
        NOEMANCER_LOAD_DEVICE(create_image_view, PFN_vkCreateImageView, "vkCreateImageView");
        NOEMANCER_LOAD_DEVICE(destroy_image_view, PFN_vkDestroyImageView, "vkDestroyImageView");
        NOEMANCER_LOAD_DEVICE(create_acceleration_structure, PFN_vkCreateAccelerationStructureKHR, "vkCreateAccelerationStructureKHR");
        NOEMANCER_LOAD_DEVICE(destroy_acceleration_structure, PFN_vkDestroyAccelerationStructureKHR, "vkDestroyAccelerationStructureKHR");
        NOEMANCER_LOAD_DEVICE(get_acceleration_structure_build_sizes, PFN_vkGetAccelerationStructureBuildSizesKHR, "vkGetAccelerationStructureBuildSizesKHR");
        NOEMANCER_LOAD_DEVICE(get_acceleration_structure_device_address, PFN_vkGetAccelerationStructureDeviceAddressKHR, "vkGetAccelerationStructureDeviceAddressKHR");
        NOEMANCER_LOAD_DEVICE(cmd_build_acceleration_structures, PFN_vkCmdBuildAccelerationStructuresKHR, "vkCmdBuildAccelerationStructuresKHR");
        NOEMANCER_LOAD_DEVICE(create_command_pool, PFN_vkCreateCommandPool, "vkCreateCommandPool");
        NOEMANCER_LOAD_DEVICE(destroy_command_pool, PFN_vkDestroyCommandPool, "vkDestroyCommandPool");
        NOEMANCER_LOAD_DEVICE(allocate_command_buffers, PFN_vkAllocateCommandBuffers, "vkAllocateCommandBuffers");
        NOEMANCER_LOAD_DEVICE(begin_command_buffer, PFN_vkBeginCommandBuffer, "vkBeginCommandBuffer");
        NOEMANCER_LOAD_DEVICE(end_command_buffer, PFN_vkEndCommandBuffer, "vkEndCommandBuffer");
        NOEMANCER_LOAD_DEVICE(cmd_pipeline_barrier, PFN_vkCmdPipelineBarrier, "vkCmdPipelineBarrier");
        NOEMANCER_LOAD_DEVICE(create_fence, PFN_vkCreateFence, "vkCreateFence");
        NOEMANCER_LOAD_DEVICE(destroy_fence, PFN_vkDestroyFence, "vkDestroyFence");
        NOEMANCER_LOAD_DEVICE(reset_fences, PFN_vkResetFences, "vkResetFences");
        NOEMANCER_LOAD_DEVICE(wait_for_fences, PFN_vkWaitForFences, "vkWaitForFences");
        NOEMANCER_LOAD_DEVICE(queue_submit, PFN_vkQueueSubmit, "vkQueueSubmit");
        NOEMANCER_LOAD_DEVICE(create_shader_module, PFN_vkCreateShaderModule, "vkCreateShaderModule");
        NOEMANCER_LOAD_DEVICE(destroy_shader_module, PFN_vkDestroyShaderModule, "vkDestroyShaderModule");
        NOEMANCER_LOAD_DEVICE(create_descriptor_set_layout, PFN_vkCreateDescriptorSetLayout, "vkCreateDescriptorSetLayout");
        NOEMANCER_LOAD_DEVICE(destroy_descriptor_set_layout, PFN_vkDestroyDescriptorSetLayout, "vkDestroyDescriptorSetLayout");
        NOEMANCER_LOAD_DEVICE(create_pipeline_layout, PFN_vkCreatePipelineLayout, "vkCreatePipelineLayout");
        NOEMANCER_LOAD_DEVICE(destroy_pipeline_layout, PFN_vkDestroyPipelineLayout, "vkDestroyPipelineLayout");
        NOEMANCER_LOAD_DEVICE(create_descriptor_pool, PFN_vkCreateDescriptorPool, "vkCreateDescriptorPool");
        NOEMANCER_LOAD_DEVICE(destroy_descriptor_pool, PFN_vkDestroyDescriptorPool, "vkDestroyDescriptorPool");
        NOEMANCER_LOAD_DEVICE(allocate_descriptor_sets, PFN_vkAllocateDescriptorSets, "vkAllocateDescriptorSets");
        NOEMANCER_LOAD_DEVICE(update_descriptor_sets, PFN_vkUpdateDescriptorSets, "vkUpdateDescriptorSets");
        NOEMANCER_LOAD_DEVICE(create_ray_tracing_pipelines, PFN_vkCreateRayTracingPipelinesKHR, "vkCreateRayTracingPipelinesKHR");
        NOEMANCER_LOAD_DEVICE(destroy_pipeline, PFN_vkDestroyPipeline, "vkDestroyPipeline");
        NOEMANCER_LOAD_DEVICE(get_ray_tracing_shader_group_handles, PFN_vkGetRayTracingShaderGroupHandlesKHR, "vkGetRayTracingShaderGroupHandlesKHR");
        NOEMANCER_LOAD_DEVICE(cmd_bind_pipeline, PFN_vkCmdBindPipeline, "vkCmdBindPipeline");
        NOEMANCER_LOAD_DEVICE(cmd_bind_descriptor_sets, PFN_vkCmdBindDescriptorSets, "vkCmdBindDescriptorSets");
        NOEMANCER_LOAD_DEVICE(cmd_trace_rays, PFN_vkCmdTraceRaysKHR, "vkCmdTraceRaysKHR");
        #undef NOEMANCER_LOAD_DEVICE
        if (!require_device_functions(device_functions, borrowed_requested)) {
            if (borrowed_requested) {
                unsupported = true;
                error_code = "native-vulkan-rt.shared-device-unsupported";
                error_detail = "The borrowed SDL_GPU device lacks a required Vulkan RT or output-image entry point.";
                return false;
            }
            error_code = "native-vulkan-rt.device-entrypoint-unavailable";
            error_detail = "The Vulkan device lacks a required persistent AS entry point.";
            return false;
        }
        if (!borrowed_requested)
            device_functions.get_device_queue(device, queue_family_index, 0U, &queue);
        if (queue == VK_NULL_HANDLE) {
            unsupported = borrowed_requested;
            error_code = borrowed_requested ? "native-vulkan-rt.shared-device-unsupported"
                                            : "native-vulkan-rt.queue-unavailable";
            error_detail = borrowed_requested ? "The borrowed SDL_GPU queue handle is null."
                                              : "vkGetDeviceQueue returned a null queue.";
            return false;
        }
        VkCommandPoolCreateInfo command_pool_info{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        command_pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        command_pool_info.queueFamilyIndex = queue_family_index;
        if (device_functions.create_command_pool(device, &command_pool_info, nullptr, &command_pool) != VK_SUCCESS) {
            error_code = "native-vulkan-rt.create-command-pool-failed";
            error_detail = "vkCreateCommandPool failed for persistent AS work.";
            return false;
        }
        VkCommandBufferAllocateInfo command_buffer_info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        command_buffer_info.commandPool = command_pool;
        command_buffer_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        command_buffer_info.commandBufferCount = 1U;
        if (device_functions.allocate_command_buffers(device, &command_buffer_info, &command_buffer) != VK_SUCCESS) {
            error_code = "native-vulkan-rt.allocate-command-buffer-failed";
            error_detail = "vkAllocateCommandBuffers failed for persistent AS work.";
            return false;
        }
        VkFenceCreateInfo fence_info{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        if (device_functions.create_fence(device, &fence_info, nullptr, &fence) != VK_SUCCESS) {
            error_code = "native-vulkan-rt.create-fence-failed";
            error_detail = "vkCreateFence failed for persistent AS submissions.";
            return false;
        }
        scratch_alignment = std::max<VkDeviceSize>(acceleration_properties.minAccelerationStructureScratchOffsetAlignment, 256U);
        const std::uint32_t output_marker = 0x4E4F5254U; // "NORT" until a trace writes hit/miss.
        if (!create_buffer(device_functions, device, memory_properties, sizeof(output_marker),
                           VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                               VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                           VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                           &output_marker, sizeof(output_marker), output_buffer, error_code, error_detail))
            return false;
        if (!create_output_image(instance_functions, device_functions, physical_device, device, memory_properties,
                                 options.output_width, options.output_height, options.output_depth,
                                 output_image, error_code, error_detail))
            return false;
        if (!transition_output_image_to_general(error_code, error_detail)) return false;
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
        VkDescriptorSetLayoutCreateInfo descriptor_layout_info{
            VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        descriptor_layout_info.bindingCount = static_cast<std::uint32_t>(descriptor_bindings.size());
        descriptor_layout_info.pBindings = descriptor_bindings.data();
        if (device_functions.create_descriptor_set_layout(
                device, &descriptor_layout_info, nullptr, &descriptor_set_layout) != VK_SUCCESS) {
            error_code = "native-vulkan-rt.create-descriptor-set-layout-failed";
            error_detail = "vkCreateDescriptorSetLayout failed for the persistent RT resources.";
            return false;
        }
        VkPipelineLayoutCreateInfo pipeline_layout_info{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        pipeline_layout_info.setLayoutCount = 1U;
        pipeline_layout_info.pSetLayouts = &descriptor_set_layout;
        if (device_functions.create_pipeline_layout(
                device, &pipeline_layout_info, nullptr, &pipeline_layout) != VK_SUCCESS) {
            error_code = "native-vulkan-rt.create-pipeline-layout-failed";
            error_detail = "vkCreatePipelineLayout failed for the persistent RT pipeline.";
            return false;
        }
        const std::array<VkDescriptorPoolSize, 2U> descriptor_pool_sizes{
            VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1U},
            VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1U}};
        VkDescriptorPoolCreateInfo descriptor_pool_info{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        descriptor_pool_info.maxSets = 1U;
        descriptor_pool_info.poolSizeCount = static_cast<std::uint32_t>(descriptor_pool_sizes.size());
        descriptor_pool_info.pPoolSizes = descriptor_pool_sizes.data();
        if (device_functions.create_descriptor_pool(
                device, &descriptor_pool_info, nullptr, &descriptor_pool) != VK_SUCCESS) {
            error_code = "native-vulkan-rt.create-descriptor-pool-failed";
            error_detail = "vkCreateDescriptorPool failed for the persistent RT resources.";
            return false;
        }
        VkDescriptorSetAllocateInfo descriptor_allocate_info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        descriptor_allocate_info.descriptorPool = descriptor_pool;
        descriptor_allocate_info.descriptorSetCount = 1U;
        descriptor_allocate_info.pSetLayouts = &descriptor_set_layout;
        if (device_functions.allocate_descriptor_sets(
                device, &descriptor_allocate_info, &descriptor_set) != VK_SUCCESS) {
            error_code = "native-vulkan-rt.allocate-descriptor-set-failed";
            error_detail = "vkAllocateDescriptorSets failed for the persistent RT resources.";
            return false;
        }
        VkShaderModuleCreateInfo shader_module_info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        shader_module_info.codeSize = native_raytracing_probe_spirv.size() * sizeof(std::uint32_t);
        shader_module_info.pCode = native_raytracing_probe_spirv.data();
        if (device_functions.create_shader_module(
                device, &shader_module_info, nullptr, &shader_module) != VK_SUCCESS) {
            error_code = "native-vulkan-rt.create-shader-module-failed";
            error_detail = "vkCreateShaderModule failed for the embedded RT probe.";
            return false;
        }
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
        std::array<VkRayTracingShaderGroupCreateInfoKHR, 3U> shader_groups{};
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
        if (device_functions.create_ray_tracing_pipelines(
                device, VK_NULL_HANDLE, VK_NULL_HANDLE, 1U, &pipeline_info, nullptr, &pipeline) != VK_SUCCESS) {
            error_code = "native-vulkan-rt.create-ray-tracing-pipeline-failed";
            error_detail = "vkCreateRayTracingPipelinesKHR failed for the persistent RT pipeline.";
            return false;
        }
        const auto handle_size = ray_tracing_properties.shaderGroupHandleSize;
        const auto handle_alignment = std::max(ray_tracing_properties.shaderGroupHandleAlignment, 1U);
        const auto base_alignment = std::max(ray_tracing_properties.shaderGroupBaseAlignment, 1U);
        const auto aligned_handle_size = align_address(handle_size, handle_alignment);
        sbt_stride = align_address(aligned_handle_size, base_alignment);
        if (handle_size == 0U || sbt_stride == 0U ||
            sbt_stride > ray_tracing_properties.maxShaderGroupStride ||
            sbt_stride > std::numeric_limits<VkDeviceSize>::max() / 3U) {
            error_code = "native-vulkan-rt.sbt-properties-invalid";
            error_detail = "The device reported invalid shader-group handle or SBT stride properties.";
            return false;
        }
        sbt_bytes = sbt_stride * 3U;
        std::vector<std::uint8_t> handles(static_cast<std::size_t>(handle_size) * 3U);
        if (device_functions.get_ray_tracing_shader_group_handles(
                device, pipeline, 0U, 3U, handles.size(), handles.data()) != VK_SUCCESS) {
            error_code = "native-vulkan-rt.get-sbt-handles-failed";
            error_detail = "vkGetRayTracingShaderGroupHandlesKHR failed for the persistent SBT.";
            return false;
        }
        std::vector<std::uint8_t> table(static_cast<std::size_t>(sbt_bytes), 0U);
        for (std::uint32_t group = 0U; group < 3U; ++group)
            std::memcpy(table.data() + static_cast<std::size_t>(group * sbt_stride),
                        handles.data() + static_cast<std::size_t>(group * handle_size), handle_size);
        if (!create_buffer(device_functions, device, memory_properties, sbt_bytes,
                           VK_BUFFER_USAGE_SHADER_BINDING_TABLE_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                           VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                           table.data(), table.size(), sbt_buffer, error_code, error_detail)) return false;
        trace_pipeline_ready = true;
        return true;
    }

    bool update_descriptor_bindings(std::string& error_code, std::string& error_detail) {
        if (descriptor_set == VK_NULL_HANDLE || tlas.acceleration_structure == VK_NULL_HANDLE ||
            output_buffer.buffer == VK_NULL_HANDLE) {
            error_code = "native-vulkan-rt.descriptor-resources-missing";
            error_detail = "The persistent descriptor set has no current TLAS or output buffer.";
            return false;
        }
        VkWriteDescriptorSetAccelerationStructureKHR acceleration_write{
            VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR};
        acceleration_write.accelerationStructureCount = 1U;
        acceleration_write.pAccelerationStructures = &tlas.acceleration_structure;
        VkDescriptorBufferInfo output_info{output_buffer.buffer, 0U, sizeof(std::uint32_t)};
        std::array<VkWriteDescriptorSet, 2U> writes{
            VkWriteDescriptorSet{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET},
            VkWriteDescriptorSet{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}};
        writes[0].pNext = &acceleration_write;
        writes[0].dstSet = descriptor_set;
        writes[0].dstBinding = 0U;
        writes[0].descriptorCount = 1U;
        writes[0].descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
        writes[1].dstSet = descriptor_set;
        writes[1].dstBinding = 1U;
        writes[1].descriptorCount = 1U;
        writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[1].pBufferInfo = &output_info;
        device_functions.update_descriptor_sets(device, static_cast<std::uint32_t>(writes.size()),
                                                writes.data(), 0U, nullptr);
        return true;
    }

    bool record_and_submit_native_trace(std::string& error_code, std::string& error_detail) {
        if (!trace_pipeline_ready || pipeline == VK_NULL_HANDLE || sbt_buffer.buffer == VK_NULL_HANDLE) {
            error_code = "native-vulkan-rt.trace-pipeline-unavailable";
            error_detail = "The persistent Vulkan trace pipeline or SBT is unavailable.";
            return false;
        }
        trace_submitted = false;
        trace_completed = false;
        output_readback_available = false;
        const std::uint32_t sentinel = 0x4E4F5254U;
        if (!write_buffer(device_functions, device, output_buffer, &sentinel, sizeof(sentinel),
                          error_code, error_detail)) return false;
        VkCommandBufferBeginInfo begin_info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        if (device_functions.begin_command_buffer(command_buffer, &begin_info) != VK_SUCCESS) {
            error_code = "native-vulkan-rt.begin-trace-command-buffer-failed";
            error_detail = "vkBeginCommandBuffer failed for the persistent RT trace.";
            return false;
        }
        VkMemoryBarrier as_to_trace{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        as_to_trace.srcAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
        as_to_trace.dstAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR;
        device_functions.cmd_pipeline_barrier(
            command_buffer, VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
            VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR, 0U, 1U, &as_to_trace,
            0U, nullptr, 0U, nullptr);
        VkMemoryBarrier host_to_trace{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        host_to_trace.srcAccessMask = VK_ACCESS_HOST_WRITE_BIT;
        host_to_trace.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        device_functions.cmd_pipeline_barrier(
            command_buffer, VK_PIPELINE_STAGE_HOST_BIT,
            VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR, 0U, 1U, &host_to_trace,
            0U, nullptr, 0U, nullptr);
        const VkStridedDeviceAddressRegionKHR raygen_region{
            sbt_buffer.device_address, sbt_stride, sbt_stride};
        const VkStridedDeviceAddressRegionKHR miss_region{
            sbt_buffer.device_address + sbt_stride, sbt_stride, sbt_stride};
        const VkStridedDeviceAddressRegionKHR hit_region{
            sbt_buffer.device_address + sbt_stride * 2U, sbt_stride, sbt_stride};
        const VkStridedDeviceAddressRegionKHR callable_region{};
        device_functions.cmd_bind_pipeline(command_buffer, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, pipeline);
        device_functions.cmd_bind_descriptor_sets(command_buffer, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR,
                                                   pipeline_layout, 0U, 1U, &descriptor_set, 0U, nullptr);
        device_functions.cmd_trace_rays(command_buffer, &raygen_region, &miss_region, &hit_region,
                                        &callable_region, 1U, 1U, 1U);
        VkMemoryBarrier trace_to_host{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        trace_to_host.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        trace_to_host.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        device_functions.cmd_pipeline_barrier(
            command_buffer, VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,
            VK_PIPELINE_STAGE_HOST_BIT, 0U, 1U, &trace_to_host,
            0U, nullptr, 0U, nullptr);
        if (device_functions.end_command_buffer(command_buffer) != VK_SUCCESS) {
            error_code = "native-vulkan-rt.end-trace-command-buffer-failed";
            error_detail = "vkEndCommandBuffer failed for the persistent RT trace.";
            return false;
        }
        if (device_functions.reset_fences(device, 1U, &fence) != VK_SUCCESS) {
            error_code = "native-vulkan-rt.reset-trace-fence-failed";
            error_detail = "vkResetFences failed before the persistent RT trace.";
            return false;
        }
        VkSubmitInfo submit_info{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submit_info.commandBufferCount = 1U;
        submit_info.pCommandBuffers = &command_buffer;
        if (device_functions.queue_submit(queue, 1U, &submit_info, fence) != VK_SUCCESS) {
            error_code = "native-vulkan-rt.trace-submit-failed";
            error_detail = "vkQueueSubmit failed for the persistent RT trace.";
            return false;
        }
        trace_submitted = true;
        if (device_functions.wait_for_fences(device, 1U, &fence, VK_TRUE,
                                             std::numeric_limits<std::uint64_t>::max()) != VK_SUCCESS) {
            error_code = "native-vulkan-rt.trace-fence-wait-failed";
            error_detail = "vkWaitForFences failed for the persistent RT trace.";
            return false;
        }
        trace_completed = true;
        output_readback_available = true;
        return true;
    }

    bool read_output(std::string& error_code, std::string& error_detail) {
        if (!output_readback_available || output_buffer.memory == VK_NULL_HANDLE) {
            error_code = "native-vulkan-rt.output-readback-unavailable";
            error_detail = "No completed native RT output is available for readback.";
            return false;
        }
        void* mapped = nullptr;
        if (device_functions.map_memory(device, output_buffer.memory, 0U, sizeof(std::uint32_t), 0U, &mapped) != VK_SUCCESS ||
            mapped == nullptr) {
            error_code = "native-vulkan-rt.output-readback-map-failed";
            error_detail = "vkMapMemory failed for the persistent RT output.";
            return false;
        }
        std::memcpy(&output_value, mapped, sizeof(output_value));
        device_functions.unmap_memory(device, output_buffer.memory);
        output_hash = kFnvOffsetBasis;
        for (std::size_t index = 0U; index < sizeof(output_value); ++index) {
            output_hash ^= static_cast<std::uint8_t>((output_value >> (index * 8U)) & 0xffU);
            output_hash *= kFnvPrime;
        }
        return true;
    }

    bool transition_output_image_to_general(std::string& error_code, std::string& error_detail) {
        if (output_image.image == VK_NULL_HANDLE || command_buffer == VK_NULL_HANDLE ||
            queue == VK_NULL_HANDLE || fence == VK_NULL_HANDLE) {
            error_code = "native-vulkan-rt.output-image-sync-unavailable";
            error_detail = "The runtime-private output image lacks a command stream or synchronization object.";
            return false;
        }
        VkCommandBufferBeginInfo begin_info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        if (device_functions.begin_command_buffer(command_buffer, &begin_info) != VK_SUCCESS) {
            error_code = "native-vulkan-rt.output-image-begin-command-buffer-failed";
            error_detail = "vkBeginCommandBuffer failed for the output image layout transition.";
            return false;
        }
        VkImageMemoryBarrier image_barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        image_barrier.srcAccessMask = 0U;
        image_barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        image_barrier.oldLayout = output_image_layout;
        image_barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        image_barrier.srcQueueFamilyIndex = queue_family_index;
        image_barrier.dstQueueFamilyIndex = queue_family_index;
        image_barrier.image = output_image.image;
        image_barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        image_barrier.subresourceRange.baseMipLevel = 0U;
        image_barrier.subresourceRange.levelCount = 1U;
        image_barrier.subresourceRange.baseArrayLayer = 0U;
        image_barrier.subresourceRange.layerCount = 1U;
        device_functions.cmd_pipeline_barrier(
            command_buffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
            VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR, 0U, 0U, nullptr, 0U, nullptr, 1U, &image_barrier);
        if (device_functions.end_command_buffer(command_buffer) != VK_SUCCESS) {
            error_code = "native-vulkan-rt.output-image-end-command-buffer-failed";
            error_detail = "vkEndCommandBuffer failed for the output image layout transition.";
            return false;
        }
        if (device_functions.reset_fences(device, 1U, &fence) != VK_SUCCESS) {
            error_code = "native-vulkan-rt.output-image-reset-fence-failed";
            error_detail = "vkResetFences failed before the output image layout transition.";
            return false;
        }
        VkSubmitInfo submit_info{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submit_info.commandBufferCount = 1U;
        submit_info.pCommandBuffers = &command_buffer;
        if (device_functions.queue_submit(queue, 1U, &submit_info, fence) != VK_SUCCESS) {
            error_code = "native-vulkan-rt.output-image-submit-failed";
            error_detail = "vkQueueSubmit failed for the output image layout transition.";
            return false;
        }
        if (device_functions.wait_for_fences(device, 1U, &fence, VK_TRUE,
                                             std::numeric_limits<std::uint64_t>::max()) != VK_SUCCESS) {
            error_code = "native-vulkan-rt.output-image-fence-wait-failed";
            error_detail = "vkWaitForFences failed for the output image layout transition.";
            return false;
        }
        output_image_layout = VK_IMAGE_LAYOUT_GENERAL;
        output_image_sync_complete = true;
        if (output_image_generation_serial == std::numeric_limits<std::uint64_t>::max() ||
            output_image_sync_value == std::numeric_limits<std::uint64_t>::max()) {
            error_code = "native-vulkan-rt.output-image-generation-overflow";
            error_detail = "The output image generation or synchronization value cannot be incremented safely.";
            return false;
        }
        ++output_image_generation_serial;
        ++output_image_sync_value;
        output_image_generation = output_image_generation_serial;
        return true;
    }

    bool allocate_native_scene(const std::vector<float>& vertices,
                               const std::uint32_t primitive_count,
                               std::string& error_code,
                               std::string& error_detail) {
        destroy_scene_native();
        const auto host_flags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        const auto geometry_usage = VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
                                    VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR;
        if (!create_buffer(device_functions, device, memory_properties,
                           static_cast<VkDeviceSize>(vertices.size() * sizeof(float)), geometry_usage,
                           host_flags, vertices.data(), vertices.size() * sizeof(float), vertex_buffer,
                           error_code, error_detail)) return false;
        native_vertex_bytes = vertices.size() * sizeof(float);
        VkAccelerationStructureGeometryTrianglesDataKHR triangle_data{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR};
        triangle_data.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
        triangle_data.vertexData.deviceAddress = vertex_buffer.device_address;
        triangle_data.vertexStride = sizeof(float) * 3U;
        triangle_data.maxVertex = primitive_count * 3U - 1U;
        triangle_data.indexType = VK_INDEX_TYPE_NONE_KHR;
        VkAccelerationStructureGeometryKHR geometry{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
        geometry.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
        geometry.flags = VK_GEOMETRY_OPAQUE_BIT_KHR;
        geometry.geometry.triangles = triangle_data;
        VkAccelerationStructureBuildGeometryInfoKHR blas_info{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
        blas_info.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
        blas_info.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR |
                          VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR;
        blas_info.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
        blas_info.geometryCount = 1U;
        blas_info.pGeometries = &geometry;
        const std::uint32_t primitive_count_value = primitive_count;
        VkAccelerationStructureBuildSizesInfoKHR blas_sizes{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
        device_functions.get_acceleration_structure_build_sizes(device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR,
                                                                &blas_info, &primitive_count_value, &blas_sizes);
        blas_scratch_bytes = blas_sizes.buildScratchSize + scratch_alignment;
        if (blas_sizes.updateScratchSize + scratch_alignment > blas_scratch_bytes)
            blas_scratch_bytes = blas_sizes.updateScratchSize + scratch_alignment;
        if (!create_buffer(device_functions, device, memory_properties, blas_scratch_bytes,
                           VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                           VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, nullptr, 0U, blas_scratch_buffer,
                           error_code, error_detail) ||
            !create_buffer(device_functions, device, memory_properties, blas_sizes.accelerationStructureSize,
                           VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                           VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, nullptr, 0U, blas_result_buffer,
                           error_code, error_detail) ||
            !create_acceleration_structure(device_functions, device, blas_result_buffer.buffer,
                                           blas_sizes.accelerationStructureSize,
                                           VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR, blas,
                                           error_code, error_detail)) return false;
        VkAccelerationStructureInstanceKHR instance_data{};
        instance_data.transform.matrix[0][0] = 1.0F;
        instance_data.transform.matrix[1][1] = 1.0F;
        instance_data.transform.matrix[2][2] = 1.0F;
        instance_data.mask = 0xffU;
        instance_data.flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;
        instance_data.accelerationStructureReference = blas.device_address;
        if (!create_buffer(device_functions, device, memory_properties, sizeof(instance_data), geometry_usage,
                           host_flags, &instance_data, sizeof(instance_data), instance_buffer,
                           error_code, error_detail)) return false;
        VkAccelerationStructureGeometryInstancesDataKHR instances{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR};
        instances.arrayOfPointers = VK_FALSE;
        instances.data.deviceAddress = instance_buffer.device_address;
        VkAccelerationStructureGeometryKHR tlas_geometry{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
        tlas_geometry.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
        tlas_geometry.geometry.instances = instances;
        VkAccelerationStructureBuildGeometryInfoKHR tlas_info{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
        tlas_info.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
        tlas_info.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR |
                          VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR;
        tlas_info.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
        tlas_info.geometryCount = 1U;
        tlas_info.pGeometries = &tlas_geometry;
        const std::uint32_t instance_count = 1U;
        VkAccelerationStructureBuildSizesInfoKHR tlas_sizes{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
        device_functions.get_acceleration_structure_build_sizes(device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR,
                                                                &tlas_info, &instance_count, &tlas_sizes);
        tlas_scratch_bytes = tlas_sizes.buildScratchSize + scratch_alignment;
        if (tlas_sizes.updateScratchSize + scratch_alignment > tlas_scratch_bytes)
            tlas_scratch_bytes = tlas_sizes.updateScratchSize + scratch_alignment;
        if (!create_buffer(device_functions, device, memory_properties, tlas_scratch_bytes,
                           VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                           VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, nullptr, 0U, tlas_scratch_buffer,
                           error_code, error_detail) ||
            !create_buffer(device_functions, device, memory_properties, tlas_sizes.accelerationStructureSize,
                           VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                           VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, nullptr, 0U, tlas_result_buffer,
                           error_code, error_detail) ||
            !create_acceleration_structure(device_functions, device, tlas_result_buffer.buffer,
                                           tlas_sizes.accelerationStructureSize,
                                           VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR, tlas,
                                           error_code, error_detail)) return false;
        native_primitive_count = primitive_count;
        native_scene_allocated = true;
        native_scene_dirty = true;
        native_scene_requires_rebuild = true;
        if (!update_descriptor_bindings(error_code, error_detail)) return false;
        return true;
    }

    bool update_native_vertices(const std::vector<float>& vertices,
                                std::string& error_code,
                                std::string& error_detail) {
        if (vertices.size() * sizeof(float) != native_vertex_bytes) return false;
        return write_buffer(device_functions, device, vertex_buffer, vertices.data(),
                            vertices.size() * sizeof(float), error_code, error_detail);
    }

    bool record_and_submit_native_build(std::string& error_code, std::string& error_detail) {
        if (!native_scene_allocated || !native_scene_dirty) return true;
        VkAccelerationStructureGeometryTrianglesDataKHR triangle_data{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR};
        triangle_data.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
        triangle_data.vertexData.deviceAddress = vertex_buffer.device_address;
        triangle_data.vertexStride = sizeof(float) * 3U;
        triangle_data.maxVertex = native_primitive_count * 3U - 1U;
        triangle_data.indexType = VK_INDEX_TYPE_NONE_KHR;
        VkAccelerationStructureGeometryKHR geometry{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
        geometry.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
        geometry.flags = VK_GEOMETRY_OPAQUE_BIT_KHR;
        geometry.geometry.triangles = triangle_data;
        VkAccelerationStructureBuildGeometryInfoKHR blas_info{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
        blas_info.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
        blas_info.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR |
                          VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR;
        blas_info.mode = native_scene_requires_rebuild ? VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR
                                                        : VK_BUILD_ACCELERATION_STRUCTURE_MODE_UPDATE_KHR;
        blas_info.geometryCount = 1U;
        blas_info.pGeometries = &geometry;
        blas_info.dstAccelerationStructure = blas.acceleration_structure;
        if (!native_scene_requires_rebuild) blas_info.srcAccelerationStructure = blas.acceleration_structure;
        const auto blas_scratch = align_address(blas_scratch_buffer.device_address, scratch_alignment);
        if (blas_scratch < blas_scratch_buffer.device_address ||
            blas_scratch - blas_scratch_buffer.device_address >
                blas_scratch_buffer.allocation_size - std::min(blas_scratch_buffer.allocation_size, blas_scratch_bytes)) {
            error_code = "native-vulkan-rt.blas-scratch-alignment-failed";
            error_detail = "BLAS scratch address could not satisfy the device alignment.";
            return false;
        }
        blas_info.scratchData.deviceAddress = blas_scratch;
        VkAccelerationStructureBuildRangeInfoKHR blas_range{native_primitive_count, 0U, 0U, 0U};
        const VkAccelerationStructureBuildRangeInfoKHR* blas_range_pointer = &blas_range;

        VkAccelerationStructureGeometryInstancesDataKHR instances{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR};
        instances.arrayOfPointers = VK_FALSE;
        instances.data.deviceAddress = instance_buffer.device_address;
        VkAccelerationStructureGeometryKHR tlas_geometry{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
        tlas_geometry.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
        tlas_geometry.geometry.instances = instances;
        VkAccelerationStructureBuildGeometryInfoKHR tlas_info{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
        tlas_info.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
        tlas_info.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR |
                          VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR;
        tlas_info.mode = native_scene_requires_rebuild ? VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR
                                                        : VK_BUILD_ACCELERATION_STRUCTURE_MODE_UPDATE_KHR;
        tlas_info.geometryCount = 1U;
        tlas_info.pGeometries = &tlas_geometry;
        tlas_info.dstAccelerationStructure = tlas.acceleration_structure;
        if (!native_scene_requires_rebuild) tlas_info.srcAccelerationStructure = tlas.acceleration_structure;
        const auto tlas_scratch = align_address(tlas_scratch_buffer.device_address, scratch_alignment);
        if (tlas_scratch < tlas_scratch_buffer.device_address ||
            tlas_scratch - tlas_scratch_buffer.device_address >
                tlas_scratch_buffer.allocation_size - std::min(tlas_scratch_buffer.allocation_size, tlas_scratch_bytes)) {
            error_code = "native-vulkan-rt.tlas-scratch-alignment-failed";
            error_detail = "TLAS scratch address could not satisfy the device alignment.";
            return false;
        }
        tlas_info.scratchData.deviceAddress = tlas_scratch;
        VkAccelerationStructureBuildRangeInfoKHR tlas_range{1U, 0U, 0U, 0U};
        const VkAccelerationStructureBuildRangeInfoKHR* tlas_range_pointer = &tlas_range;
        VkCommandBufferBeginInfo begin_info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        if (device_functions.begin_command_buffer(command_buffer, &begin_info) != VK_SUCCESS) {
            error_code = "native-vulkan-rt.begin-command-buffer-failed";
            error_detail = "vkBeginCommandBuffer failed for the persistent AS build.";
            return false;
        }
        VkMemoryBarrier host_to_as{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        host_to_as.srcAccessMask = VK_ACCESS_HOST_WRITE_BIT;
        host_to_as.dstAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR |
                                   VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
        device_functions.cmd_pipeline_barrier(command_buffer, VK_PIPELINE_STAGE_HOST_BIT,
                                               VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
                                               0U, 1U, &host_to_as, 0U, nullptr, 0U, nullptr);
        device_functions.cmd_build_acceleration_structures(command_buffer, 1U, &blas_info, &blas_range_pointer);
        VkMemoryBarrier blas_to_tlas{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        blas_to_tlas.srcAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
        blas_to_tlas.dstAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR |
                                     VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
        device_functions.cmd_pipeline_barrier(command_buffer,
                                               VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
                                               VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
                                               0U, 1U, &blas_to_tlas, 0U, nullptr, 0U, nullptr);
        device_functions.cmd_build_acceleration_structures(command_buffer, 1U, &tlas_info, &tlas_range_pointer);
        if (device_functions.end_command_buffer(command_buffer) != VK_SUCCESS) {
            error_code = "native-vulkan-rt.end-command-buffer-failed";
            error_detail = "vkEndCommandBuffer failed for the persistent AS build.";
            return false;
        }
        if (device_functions.reset_fences(device, 1U, &fence) != VK_SUCCESS) {
            error_code = "native-vulkan-rt.reset-fence-failed";
            error_detail = "vkResetFences failed before the persistent AS submit.";
            return false;
        }
        VkSubmitInfo submit_info{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submit_info.commandBufferCount = 1U;
        submit_info.pCommandBuffers = &command_buffer;
        if (device_functions.queue_submit(queue, 1U, &submit_info, fence) != VK_SUCCESS) {
            error_code = "native-vulkan-rt.queue-submit-failed";
            error_detail = "vkQueueSubmit failed for the persistent AS build.";
            return false;
        }
        build_submitted = true;
        if (device_functions.wait_for_fences(device, 1U, &fence, VK_TRUE,
                                             std::numeric_limits<std::uint64_t>::max()) != VK_SUCCESS) {
            error_code = "native-vulkan-rt.fence-wait-failed";
            error_detail = "vkWaitForFences failed for the persistent AS build.";
            return false;
        }
        build_completed = true;
        native_scene_built = true;
        native_scene_dirty = false;
        native_scene_requires_rebuild = false;
        return true;
    }
};

#else

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
    bool build_submitted{};
    bool build_completed{};
    bool trace_submitted{};
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
    NativeVulkanRayTracingContextReceipt receipt(const NativeVulkanRayTracingContextState result_state,
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
        result.build_submitted = build_submitted;
        result.build_completed = build_completed;
        result.trace_submitted = trace_submitted;
        result.trace_completed = trace_completed;
        result.readback_completed = readback_completed;
        result.output_image_live = false;
        result.output_image_view_live = false;
        result.output_image_runtime_private = false;
        result.output_image_interop_ready = false;
        result.output_image_external_import_supported = false;
        result.output_image_same_device_required = false;
        result.output_image_layout_ready = false;
        result.output_image_sync_complete = false;
        result.output_image_trace_written = false;
        result.output_image_cpu_readback_supported = false;
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
        result.output_image_format = "none";
        result.output_image_layout = "none";
        result.output_image_access = "none";
        result.output_image_sync_kind = "none";
        result.output_image_interop_boundary = "unavailable";
        return result;
    }
    NativeVulkanRayTracingContextReceipt not_ready(const NativeVulkanRayTracingContextFailureStage stage,
                                                   const std::string_view code,
                                                   const std::string_view detail) const {
        return receipt(NativeVulkanRayTracingContextState::error, stage, code, detail);
    }
};

#endif

std::string_view native_vulkan_raytracing_context_state_name(
    const NativeVulkanRayTracingContextState state) noexcept {
    switch (state) {
    case NativeVulkanRayTracingContextState::uninitialized: return "uninitialized";
    case NativeVulkanRayTracingContextState::ready: return "ready";
    case NativeVulkanRayTracingContextState::unsupported: return "unsupported";
    case NativeVulkanRayTracingContextState::fallback: return "fallback";
    case NativeVulkanRayTracingContextState::error: return "error";
    case NativeVulkanRayTracingContextState::shutdown: return "shutdown";
    }
    return "error";
}

std::string_view native_vulkan_raytracing_context_failure_stage_name(
    const NativeVulkanRayTracingContextFailureStage stage) noexcept {
    switch (stage) {
    case NativeVulkanRayTracingContextFailureStage::none: return "none";
    case NativeVulkanRayTracingContextFailureStage::loader: return "loader";
    case NativeVulkanRayTracingContextFailureStage::instance: return "instance";
    case NativeVulkanRayTracingContextFailureStage::physical_device: return "physical-device";
    case NativeVulkanRayTracingContextFailureStage::device: return "device";
    case NativeVulkanRayTracingContextFailureStage::acceleration_structure: return "acceleration-structure";
    case NativeVulkanRayTracingContextFailureStage::pipeline: return "pipeline";
    case NativeVulkanRayTracingContextFailureStage::scene: return "scene";
    case NativeVulkanRayTracingContextFailureStage::trace: return "trace";
    case NativeVulkanRayTracingContextFailureStage::readback: return "readback";
    case NativeVulkanRayTracingContextFailureStage::shutdown: return "shutdown";
    }
    return "shutdown";
}

NativeVulkanRayTracingContext::NativeVulkanRayTracingContext(
    const NativeVulkanRayTracingContextOptions& options)
    : impl_(std::make_unique<Impl>(options)) {}

NativeVulkanRayTracingContext::~NativeVulkanRayTracingContext() { static_cast<void>(shutdown()); }

NativeVulkanRayTracingContextReceipt NativeVulkanRayTracingContext::initialize() {
    if (impl_->state == NativeVulkanRayTracingContextState::shutdown)
        return impl_->not_ready(NativeVulkanRayTracingContextFailureStage::shutdown,
                                "native-vulkan-rt.context-shutdown",
                                "The context has already been shut down and cannot be initialized again.");
    if (impl_->initialized)
        return impl_->receipt(impl_->state, NativeVulkanRayTracingContextFailureStage::none,
                              impl_->state == NativeVulkanRayTracingContextState::fallback
                                  ? "native-vulkan-rt.context-fallback-already-initialized"
                                  : "native-vulkan-rt.context-already-initialized",
                              impl_->state == NativeVulkanRayTracingContextState::fallback
                                  ? "The persistent Vulkan backend is unavailable; the existing fallback context is reused."
                                  : "The persistent Vulkan context is already initialized.");
    impl_->initialized = true;
    impl_->generation = 1U;
#if NOEMANCER_HAS_VULKAN_HEADERS
    std::string error_code;
    std::string error_detail;
    bool unsupported = false;
    if (impl_->initialize_native(error_code, error_detail, unsupported)) {
        impl_->state = NativeVulkanRayTracingContextState::ready;
        return impl_->receipt(impl_->state, NativeVulkanRayTracingContextFailureStage::none,
                              "native-vulkan-rt.context-ready",
                              "Persistent Vulkan instance, device, queue, command pool, fence and output buffer are live.");
    }
    const auto stage = unsupported ? NativeVulkanRayTracingContextFailureStage::physical_device
                                   : NativeVulkanRayTracingContextFailureStage::device;
    impl_->destroy_native();
    if (!impl_->options.allow_fallback) {
        impl_->state = unsupported ? NativeVulkanRayTracingContextState::unsupported
                                   : NativeVulkanRayTracingContextState::error;
        return impl_->receipt(impl_->state, stage, error_code, error_detail);
    }
    impl_->state = NativeVulkanRayTracingContextState::fallback;
    return impl_->receipt(impl_->state, stage,
                          "native-vulkan-rt.context-fallback-after-native-failure",
                          error_detail.empty() ? "Persistent Vulkan resources were unavailable; CPU fallback is active."
                                               : error_detail);
#else
    impl_->state = impl_->options.allow_fallback ? NativeVulkanRayTracingContextState::fallback
                                                 : NativeVulkanRayTracingContextState::unsupported;
    return impl_->receipt(impl_->state, NativeVulkanRayTracingContextFailureStage::loader,
                          "native-vulkan-rt.vulkan-headers-unavailable",
                          impl_->options.allow_fallback
                              ? "The build has no Vulkan headers; the deterministic CPU fallback is active."
                              : "The build has no Vulkan headers for a native context.");
#endif
}

NativeVulkanRayTracingContextReceipt NativeVulkanRayTracingContext::ensure_scene(
    const NativeVulkanRayTracingScene& scene) {
    if (!impl_->initialized) static_cast<void>(initialize());
    if (impl_->state == NativeVulkanRayTracingContextState::shutdown)
        return impl_->not_ready(NativeVulkanRayTracingContextFailureStage::shutdown,
                                "native-vulkan-rt.context-shutdown",
                                "A scene cannot be submitted after context shutdown.");
    if (impl_->state == NativeVulkanRayTracingContextState::unsupported)
        return impl_->receipt(impl_->state, NativeVulkanRayTracingContextFailureStage::device,
                              "native-vulkan-rt.context-unsupported",
                              "The persistent Vulkan backend is unsupported and fallback is disabled.");
    if (scene.triangles.empty())
        return impl_->not_ready(NativeVulkanRayTracingContextFailureStage::scene,
                                "native-vulkan-rt.context-scene-empty",
                                "At least one triangle is required for the bounded scene contract.");
    if (scene.triangles.size() > impl_->options.maximum_triangles)
        return impl_->not_ready(NativeVulkanRayTracingContextFailureStage::scene,
                                "native-vulkan-rt.context-scene-limit",
                                "The scene exceeds the configured triangle budget.");
    for (const auto& triangle : scene.triangles)
        if (!valid_scene_triangle(triangle))
            return impl_->not_ready(NativeVulkanRayTracingContextFailureStage::scene,
                                    "native-vulkan-rt.context-scene-nonfinite",
                                    "Scene vertices must be finite and within the bounded coordinate range.");
    const auto fingerprint = scene_fingerprint(scene);
    const auto topology_changed = !impl_->scene_ready ||
                                   impl_->scene_topology_revision != scene.topology_revision ||
                                   impl_->triangles.size() != scene.triangles.size();
    const auto content_changed = !impl_->scene_ready || impl_->scene_fingerprint != fingerprint;
    impl_->scene_rebuilt = topology_changed;
    impl_->scene_updated = !topology_changed && content_changed;
    impl_->scene_reused = !topology_changed && !content_changed;
    impl_->build_submitted = false;
    impl_->build_completed = false;
    impl_->trace_submitted = false;
    if (content_changed) {
        if (impl_->generation == std::numeric_limits<std::uint64_t>::max())
            return impl_->not_ready(NativeVulkanRayTracingContextFailureStage::scene,
                                    "native-vulkan-rt.context-generation-overflow",
                                    "The context generation cannot be incremented safely.");
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
#if NOEMANCER_HAS_VULKAN_HEADERS
        impl_->output_readback_available = false;
#endif
#if NOEMANCER_HAS_VULKAN_HEADERS
        if (impl_->state == NativeVulkanRayTracingContextState::ready) {
            std::vector<float> vertices;
            vertices.reserve(scene.triangles.size() * 9U);
            for (const auto& triangle : scene.triangles)
                for (const auto& position : triangle.positions)
                    vertices.insert(vertices.end(), position.begin(), position.end());
            std::string error_code;
            std::string error_detail;
            bool native_ok = false;
            impl_->native_scene_built = false;
            if (topology_changed)
                native_ok = impl_->allocate_native_scene(vertices, static_cast<std::uint32_t>(scene.triangles.size()),
                                                         error_code, error_detail);
            else {
                native_ok = impl_->update_native_vertices(vertices, error_code, error_detail);
                impl_->native_scene_dirty = native_ok;
                impl_->native_scene_requires_rebuild = false;
            }
            if (!native_ok) {
                impl_->destroy_native();
                if (!impl_->options.allow_fallback) {
                    impl_->state = NativeVulkanRayTracingContextState::error;
                    return impl_->not_ready(NativeVulkanRayTracingContextFailureStage::acceleration_structure,
                                            error_code.empty() ? "native-vulkan-rt.context-scene-allocation-failed" : error_code,
                                            error_detail.empty() ? "Persistent Vulkan scene allocation failed." : error_detail);
                }
                impl_->state = NativeVulkanRayTracingContextState::fallback;
            }
        }
#endif
    }
    return impl_->receipt(impl_->state, NativeVulkanRayTracingContextFailureStage::scene,
                          topology_changed ? "native-vulkan-rt.context-scene-rebuilt"
                                            : (content_changed ? "native-vulkan-rt.context-scene-updated"
                                                                : "native-vulkan-rt.context-scene-reused"),
                          topology_changed ? "The persistent scene snapshot changed topology and was rebuilt."
                                            : (content_changed ? "The persistent scene snapshot changed content and was updated."
                                                               : "The scene fingerprint is unchanged; the existing snapshot is reused."));
}

NativeVulkanRayTracingContextReceipt NativeVulkanRayTracingContext::build_or_update() {
    if (!impl_->initialized) static_cast<void>(initialize());
    if (impl_->state == NativeVulkanRayTracingContextState::shutdown)
        return impl_->not_ready(NativeVulkanRayTracingContextFailureStage::shutdown,
                                "native-vulkan-rt.context-shutdown",
                                "A scene build cannot be submitted after context shutdown.");
    if (impl_->state == NativeVulkanRayTracingContextState::unsupported)
        return impl_->receipt(impl_->state, NativeVulkanRayTracingContextFailureStage::device,
                              "native-vulkan-rt.context-unsupported",
                              "The persistent Vulkan backend is unsupported and fallback is disabled.");
    if (!impl_->scene_ready)
        return impl_->not_ready(NativeVulkanRayTracingContextFailureStage::scene,
                                "native-vulkan-rt.context-scene-missing",
                                "ensure_scene must provide a scene before build_or_update.");
#if NOEMANCER_HAS_VULKAN_HEADERS
    if (impl_->state == NativeVulkanRayTracingContextState::ready) {
        if (!impl_->native_scene_allocated)
            return impl_->not_ready(NativeVulkanRayTracingContextFailureStage::acceleration_structure,
                                    "native-vulkan-rt.context-native-scene-missing",
                                    "The native context has no persistent geometry/AS allocation for the scene.");
        if (!impl_->native_scene_dirty) {
            impl_->build_submitted = false;
            impl_->build_completed = false;
            return impl_->receipt(impl_->state, NativeVulkanRayTracingContextFailureStage::acceleration_structure,
                                  "native-vulkan-rt.context-native-build-cached",
                                  "The persistent BLAS/TLAS are unchanged and no build was submitted.");
        }
        std::string error_code;
        std::string error_detail;
        if (!impl_->record_and_submit_native_build(error_code, error_detail)) {
            impl_->destroy_native();
            if (impl_->options.allow_fallback) {
                impl_->state = NativeVulkanRayTracingContextState::fallback;
                return impl_->receipt(impl_->state, NativeVulkanRayTracingContextFailureStage::acceleration_structure,
                                      "native-vulkan-rt.context-fallback-after-build-failure",
                                      error_detail.empty() ? "The native AS build failed; CPU fallback is active." : error_detail);
            }
            impl_->state = NativeVulkanRayTracingContextState::error;
            return impl_->not_ready(NativeVulkanRayTracingContextFailureStage::acceleration_structure,
                                    error_code.empty() ? "native-vulkan-rt.context-build-failed" : error_code,
                                    error_detail.empty() ? "The persistent AS build failed." : error_detail);
        }
        return impl_->receipt(impl_->state, NativeVulkanRayTracingContextFailureStage::acceleration_structure,
                              impl_->scene_rebuilt ? "native-vulkan-rt.context-native-build-completed"
                                                   : "native-vulkan-rt.context-native-update-completed",
                              impl_->scene_rebuilt ? "Persistent BLAS/TLAS build submitted and completed on Vulkan."
                                                   : "Persistent BLAS/TLAS update submitted and completed on Vulkan.");
    }
#endif
    return impl_->receipt(impl_->state, NativeVulkanRayTracingContextFailureStage::acceleration_structure,
                          "native-vulkan-rt.context-fallback-build-cached",
                          "The scene is resident in the fallback context; no Vulkan AS build is claimed.");
}

NativeVulkanRayTracingContextReceipt NativeVulkanRayTracingContext::trace(
    const NativeVulkanRayTracingTraceRequest& request) {
    if (!impl_->initialized) static_cast<void>(initialize());
    if (impl_->state == NativeVulkanRayTracingContextState::shutdown)
        return impl_->not_ready(NativeVulkanRayTracingContextFailureStage::shutdown,
                                "native-vulkan-rt.context-shutdown",
                                "A trace cannot be submitted after context shutdown.");
    if (impl_->state == NativeVulkanRayTracingContextState::unsupported)
        return impl_->receipt(impl_->state, NativeVulkanRayTracingContextFailureStage::device,
                              "native-vulkan-rt.context-unsupported",
                              "The persistent Vulkan backend is unsupported and fallback is disabled.");
    if (!impl_->scene_ready)
        return impl_->not_ready(NativeVulkanRayTracingContextFailureStage::scene,
                                "native-vulkan-rt.context-scene-missing",
                                "ensure_scene must provide a scene before trace.");
    if (!valid_trace_request(request))
        return impl_->not_ready(NativeVulkanRayTracingContextFailureStage::trace,
                                "native-vulkan-rt.context-trace-invalid-request",
                                "Trace origin, direction and distance range must be finite and bounded.");
#if NOEMANCER_HAS_VULKAN_HEADERS
    if (impl_->state == NativeVulkanRayTracingContextState::ready) {
        if (!impl_->native_scene_built)
            return impl_->not_ready(NativeVulkanRayTracingContextFailureStage::acceleration_structure,
                                    "native-vulkan-rt.context-build-missing",
                                    "build_or_update must complete before a native trace; the resident AS is not built.");
        std::string error_code;
        std::string error_detail;
        if (!impl_->record_and_submit_native_trace(error_code, error_detail)) {
            impl_->state = NativeVulkanRayTracingContextState::error;
            return impl_->not_ready(NativeVulkanRayTracingContextFailureStage::trace,
                                    error_code.empty() ? "native-vulkan-rt.context-trace-failed" : error_code,
                                    error_detail.empty() ? "The persistent Vulkan RT trace failed." : error_detail);
        }
        return impl_->receipt(impl_->state, NativeVulkanRayTracingContextFailureStage::trace,
                              "native-vulkan-rt.context-native-trace-completed",
                              "A persistent Vulkan RT pipeline traced the resident TLAS and completed its fence.");
    }
#endif
    const auto origin = to_vec3(request.origin);
    const auto direction = to_vec3(request.direction);
    bool hit = false;
    for (const auto& triangle : impl_->triangles)
        if (ray_intersects_triangle(origin, direction, triangle, request.minimum_distance, request.maximum_distance)) {
            hit = true;
            break;
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
    if (impl_->state == NativeVulkanRayTracingContextState::shutdown)
        return impl_->not_ready(NativeVulkanRayTracingContextFailureStage::shutdown,
                                "native-vulkan-rt.context-shutdown",
                                "A readback cannot be requested after context shutdown.");
    if (impl_->state == NativeVulkanRayTracingContextState::unsupported)
        return impl_->receipt(impl_->state, NativeVulkanRayTracingContextFailureStage::device,
                              "native-vulkan-rt.context-unsupported",
                              "The persistent Vulkan backend is unsupported and fallback is disabled.");
#if NOEMANCER_HAS_VULKAN_HEADERS
    if (impl_->state == NativeVulkanRayTracingContextState::ready) {
        if (!impl_->trace_completed)
            return impl_->not_ready(NativeVulkanRayTracingContextFailureStage::readback,
                                    "native-vulkan-rt.context-trace-missing",
                                    "trace must complete before native readback.");
        std::string error_code;
        std::string error_detail;
        if (!impl_->read_output(error_code, error_detail))
            return impl_->not_ready(NativeVulkanRayTracingContextFailureStage::readback,
                                    error_code.empty() ? "native-vulkan-rt.context-readback-failed" : error_code,
                                    error_detail.empty() ? "The persistent Vulkan output readback failed." : error_detail);
        impl_->readback_completed = true;
        return impl_->receipt(impl_->state, NativeVulkanRayTracingContextFailureStage::readback,
                              "native-vulkan-rt.context-native-readback-completed",
                              "The persistent Vulkan RT output marker was read back from host-visible memory.");
    }
#endif
    if (!impl_->trace_completed)
        return impl_->not_ready(NativeVulkanRayTracingContextFailureStage::readback,
                                "native-vulkan-rt.context-trace-missing",
                                "trace must complete before readback.");
    impl_->readback_completed = true;
    return impl_->receipt(impl_->state, NativeVulkanRayTracingContextFailureStage::readback,
                          "native-vulkan-rt.context-fallback-readback-completed",
                          "The fallback output marker was read back from the context-owned CPU snapshot; no Vulkan readback is claimed.");
}

NativeVulkanRayTracingContextReceipt NativeVulkanRayTracingContext::shutdown() noexcept {
    if (impl_ == nullptr) return {};
    if (impl_->state == NativeVulkanRayTracingContextState::shutdown)
        return impl_->receipt(NativeVulkanRayTracingContextState::shutdown,
                              NativeVulkanRayTracingContextFailureStage::none,
                              "native-vulkan-rt.context-shutdown-already-complete",
                              "The context shutdown operation is idempotent.");
#if NOEMANCER_HAS_VULKAN_HEADERS
    impl_->destroy_native();
#endif
    impl_->triangles.clear();
    impl_->scene_ready = false;
    impl_->trace_completed = false;
    impl_->readback_completed = false;
    impl_->scene_rebuilt = false;
    impl_->scene_updated = false;
    impl_->scene_reused = false;
    impl_->build_submitted = false;
    impl_->build_completed = false;
    impl_->trace_submitted = false;
    impl_->scene_topology_revision = 0U;
    impl_->scene_content_revision = 0U;
    impl_->scene_fingerprint = 0U;
    impl_->output_value = 0U;
    impl_->output_hash = 0U;
#if NOEMANCER_HAS_VULKAN_HEADERS
    impl_->output_readback_available = false;
#endif
    impl_->state = NativeVulkanRayTracingContextState::shutdown;
    return impl_->receipt(impl_->state, NativeVulkanRayTracingContextFailureStage::shutdown,
                          "native-vulkan-rt.context-shutdown-complete",
                          "The context is shut down; repeated shutdown calls are safe.");
}

bool NativeVulkanRayTracingContext::initialized() const noexcept {
    return impl_ != nullptr && impl_->initialized;
}

bool NativeVulkanRayTracingContext::scene_ready() const noexcept {
    return impl_ != nullptr && impl_->scene_ready;
}

std::uint64_t NativeVulkanRayTracingContext::generation() const noexcept {
    return impl_ == nullptr ? 0U : impl_->generation;
}

} // namespace noemancer
