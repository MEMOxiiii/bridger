#pragma once

// Imports a D3D11 texture shared by an NT handle as the memory of a VkImage, so the desktop app
// can composite the 3D view under the WebView2 user interface (docs/adr/0009). Windows only; the
// implementation lives in win32/ because it needs the Win32 handle types.

#include <bridger/core/result.hpp>

#include <volk.h>

namespace bridger::render::detail {

inline constexpr const char* kExternalMemoryExtension = "VK_KHR_external_memory_win32";
inline constexpr VkExternalMemoryHandleTypeFlagBits kSharedTextureHandleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT;

// Allocates dedicated memory imported from `handle` and binds it to `image` (created with
// VkExternalMemoryImageCreateInfo for kSharedTextureHandleType). The caller keeps owning `handle`.
[[nodiscard]] Result<VkDeviceMemory> import_shared_texture(VkDevice device, VkPhysicalDevice physical, VkImage image, void* handle);

} // namespace bridger::render::detail
