#include "../external_image.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <vulkan/vulkan_win32.h>

namespace bridger::render::detail {

Result<VkDeviceMemory> import_shared_texture(VkDevice device, VkPhysicalDevice physical, VkImage image, void* handle) {
    VkMemoryRequirements req{};
    vkGetImageMemoryRequirements(device, image, &req);
    VkPhysicalDeviceMemoryProperties props{};
    vkGetPhysicalDeviceMemoryProperties(physical, &props);
    u32 type = UINT32_MAX;
    for (u32 i = 0; i < props.memoryTypeCount && type == UINT32_MAX; ++i)
        if ((req.memoryTypeBits & (1u << i)) != 0 && (props.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != 0) type = i;
    for (u32 i = 0; i < props.memoryTypeCount && type == UINT32_MAX; ++i)
        if ((req.memoryTypeBits & (1u << i)) != 0) type = i;
    if (type == UINT32_MAX) return make_error(ErrorCode::IoError, "Vulkan: no memory type can import the shared texture");

    VkMemoryDedicatedAllocateInfo dedicated{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
    dedicated.image = image;
    VkImportMemoryWin32HandleInfoKHR import{VK_STRUCTURE_TYPE_IMPORT_MEMORY_WIN32_HANDLE_INFO_KHR};
    import.pNext = &dedicated;
    import.handleType = kSharedTextureHandleType;
    import.handle = static_cast<HANDLE>(handle);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.pNext = &import;
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = type;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    if (vkAllocateMemory(device, &ai, nullptr, &memory) != VK_SUCCESS)
        return make_error(ErrorCode::IoError, "Vulkan: importing the shared texture failed");
    if (vkBindImageMemory(device, image, memory, 0) != VK_SUCCESS) {
        vkFreeMemory(device, memory, nullptr);
        return make_error(ErrorCode::IoError, "Vulkan: binding the shared texture failed");
    }
    return memory;
}

} // namespace bridger::render::detail
