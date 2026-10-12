#include "../external_image.hpp"

namespace bridger::render::detail {

Result<VkDeviceMemory> import_shared_texture(VkDevice, VkPhysicalDevice, VkImage, void*) {
    return make_error(ErrorCode::NotImplemented, "shared textures are only implemented on Windows");
}

} // namespace bridger::render::detail
