// VulkanMemoryAllocator implementation unit. Vulkan functions come from volk
// (vmaImportVulkanFunctionsFromVolk in renderer.cpp), so VMA fetches none itself.

#include <volk.h>

#define VMA_IMPLEMENTATION
#define VMA_STATIC_VULKAN_FUNCTIONS 0
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 0
#include <vma/vk_mem_alloc.h>
