#pragma once

// DirectComposition tree of the app window (docs/adr/0009): the 3D scene at the bottom (a DXGI
// composition swapchain filled from a D3D11 texture that the Vulkan renderer draws into) and the
// WebView2 user interface on top, blended with real transparency.
//
// Per frame: wait_for_copy() -> the renderer draws into shared_handle() -> present().

#include <bridger/core/types.hpp>

#include <memory>
#include <optional>

namespace bridger::app {

class Compositor {
public:
    // `adapter_luid`: the Vulkan GPU, so both APIs use the same adapter (nullopt: the default one).
    static std::unique_ptr<Compositor> create(void* hwnd, std::optional<u64> adapter_luid, u32 width, u32 height);
    ~Compositor();
    Compositor(const Compositor&) = delete;
    Compositor& operator=(const Compositor&) = delete;

    // NT handle of the D3D11 texture the renderer draws into. Changes after resize().
    [[nodiscard]] void* shared_handle() const noexcept;
    // New size: a new shared texture (the previous one stays alive until the next resize, so the
    // renderer can finish with it) and resized swapchain buffers.
    bool resize(u32 width, u32 height);

    // Blocks until Direct3D has finished copying the previous frame out of the shared texture.
    void wait_for_copy();
    // Copies the shared texture into the swapchain and presents (vsync).
    void present();

    // The IDCompositionVisual (as IUnknown*) that hosts the WebView2 composition controller.
    [[nodiscard]] void* ui_visual() const noexcept;
    void commit();

private:
    Compositor();
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace bridger::app
