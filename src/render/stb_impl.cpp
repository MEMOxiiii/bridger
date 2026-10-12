// stb_image / stb_image_write implementation unit (PNG textures from the user's resource packs,
// screenshots). Third-party code: compiled with the project's flags but warnings are not ours.

#if defined(_MSC_VER)
#pragma warning(push, 0)
#endif
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Weverything"
#endif

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image.h>
#include <stb_image_write.h>

#if defined(__clang__)
#pragma clang diagnostic pop
#endif
#if defined(_MSC_VER)
#pragma warning(pop)
#endif
