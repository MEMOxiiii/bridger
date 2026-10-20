#pragma once

// VulkanMemoryAllocator's header: vcpkg installs it as <vma/vk_mem_alloc.h> on some systems and as <vk_mem_alloc.h>
// on others (seen on Windows and Linux with the same vcpkg baseline).
#if __has_include(<vma/vk_mem_alloc.h>)
#include <vma/vk_mem_alloc.h>
#else
#include <vk_mem_alloc.h>
#endif
