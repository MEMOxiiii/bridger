bridger_add_module(render
    DEPS core platform io formats
    PUBLIC_LIBS  glm::glm
    PRIVATE_LIBS volk::volk vk-bootstrap::vk-bootstrap GPUOpen::VulkanMemoryAllocator nlohmann_json::nlohmann_json)

target_include_directories(bridger_render SYSTEM PRIVATE ${Stb_INCLUDE_DIR})
# GLM: Vulkan depth range (0..1).
target_compile_definitions(bridger_render PUBLIC GLM_FORCE_DEPTH_ZERO_TO_ONE)

bridger_compile_shaders(bridger_render
    shaders/chunk.vert
    shaders/chunk.frag)
