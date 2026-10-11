# 0005. Rendering and UI: Vulkan 1.3, SDL3, Dear ImGui

- Status: accepted (implementation in Phases 7–8)
- Date: 2026-10-11

## Context

The viewer must render very large worlds smoothly. The app needs windowing, input, file dialogs and a docking
UI. The stack was decided in the build specification (§3).

## Decision

- **Vulkan 1.3** with dynamic rendering (no render-pass objects). **volk** loads function pointers,
  **vk-bootstrap** sets up instance and device, **VulkanMemoryAllocator** manages memory.
- **GLSL → SPIR-V at build time** with glslang (`cmake/Shaders.cmake`). The SPIR-V is embedded into the binary
  (`cmake/EmbedResource.cmake`). Phase 0 compiles placeholder `chunk.vert/.frag` to prove the toolchain.
- **SDL3** only for windowing, input and dialogs. **Dear ImGui** (docking branch) with SDL3 + Vulkan backends.
- **GLM** for math. The camera keeps its position in double precision and renders camera-relative, so worlds
  stay precise far from the origin.

## Consequences and known risks (to resolve in Phase 7/8)

- **Loader conflict.** vcpkg's `imgui[vulkan-binding]` links `Vulkan::Vulkan` (the system loader import
  library), and its Vulkan backend calls `vk*` prototypes directly. volk defines global function pointers with
  the same names. Mixing the two in one executable can cause duplicate symbols or calls that bypass volk.
  Possible solutions: compile `imgui_impl_vulkan.cpp` ourselves with `VK_NO_PROTOTYPES` +
  `IMGUI_IMPL_VULKAN_USE_VOLK`, or drop the `vulkan-binding` feature. Either is decided in Phase 8, with an
  update to this ADR.
- **vulkan-1.dll.** vcpkg forces `vulkan-loader` to dynamic linkage. The release must not ship this DLL; the GPU
  driver provides the real loader. `docs/release.md` checks this.
