#include "compositor.hpp"

#include <bridger/core/log.hpp>

#include <Windows.h>
#include <d3d11_1.h>
#include <dcomp.h>
#include <dxgi1_6.h>
#include <wil/com.h>

#include <algorithm>
#include <cstring>

namespace bridger::app {

struct Compositor::Impl {
    HWND hwnd = nullptr;
    u32 width = 0, height = 0;
    wil::com_ptr<ID3D11Device> device;
    wil::com_ptr<ID3D11DeviceContext> context;
    wil::com_ptr<IDXGISwapChain1> swapchain;
    wil::com_ptr<ID3D11Texture2D> scene, previous_scene;
    HANDLE scene_handle = nullptr, previous_handle = nullptr;
    wil::com_ptr<ID3D11Query> copied;
    bool copy_pending = false;
    wil::com_ptr<IDCompositionDevice> dcomp;
    wil::com_ptr<IDCompositionTarget> target;
    wil::com_ptr<IDCompositionVisual> root, scene_visual, ui_visual;

    ~Impl() {
        if (scene_handle != nullptr) CloseHandle(scene_handle);
        if (previous_handle != nullptr) CloseHandle(previous_handle);
    }

    bool fail(const char* what, HRESULT hr) {
        log::error("compositor: {} failed (0x{:08x})", what, static_cast<unsigned>(hr));
        return false;
    }

    bool make_scene_texture() {
        if (previous_handle != nullptr) CloseHandle(previous_handle);
        previous_scene = std::move(scene);
        previous_handle = scene_handle;
        scene_handle = nullptr;
        D3D11_TEXTURE2D_DESC td{};
        td.Width = width;
        td.Height = height;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        td.MiscFlags = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
        HRESULT hr = device->CreateTexture2D(&td, nullptr, scene.put());
        if (FAILED(hr)) return fail("CreateTexture2D (shared)", hr);
        auto resource = scene.query<IDXGIResource1>();
        hr = resource->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr, &scene_handle);
        if (FAILED(hr)) return fail("CreateSharedHandle", hr);
        return true;
    }

    bool init(void* window, std::optional<u64> luid, u32 w, u32 h) {
        hwnd = static_cast<HWND>(window);
        width = std::max(1u, w);
        height = std::max(1u, h);
        wil::com_ptr<IDXGIFactory4> factory;
        HRESULT hr = CreateDXGIFactory2(0, IID_PPV_ARGS(factory.put()));
        if (FAILED(hr)) return fail("CreateDXGIFactory2", hr);
        wil::com_ptr<IDXGIAdapter1> adapter;
        if (luid) {
            LUID l{};
            std::memcpy(&l, &*luid, sizeof(l));
            if (FAILED(factory->EnumAdapterByLuid(l, IID_PPV_ARGS(adapter.put())))) adapter.reset();
        }
        constexpr D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
        hr = D3D11CreateDevice(adapter.get(), adapter ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE, nullptr,
                               D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, 2, D3D11_SDK_VERSION, device.put(), nullptr, context.put());
        if (FAILED(hr)) // no usable GPU driver: WARP (software) still composites the interface
            hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, 2, D3D11_SDK_VERSION,
                                   device.put(), nullptr, context.put());
        if (FAILED(hr)) return fail("D3D11CreateDevice", hr);

        DXGI_SWAP_CHAIN_DESC1 sd{};
        sd.Width = width;
        sd.Height = height;
        sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        sd.SampleDesc.Count = 1;
        sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        sd.BufferCount = 2;
        sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
        sd.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
        hr = factory->CreateSwapChainForComposition(device.get(), &sd, nullptr, swapchain.put());
        if (FAILED(hr)) return fail("CreateSwapChainForComposition", hr);
        if (!make_scene_texture()) return false;
        D3D11_QUERY_DESC qd{D3D11_QUERY_EVENT, 0};
        hr = device->CreateQuery(&qd, copied.put());
        if (FAILED(hr)) return fail("CreateQuery", hr);

        auto dxgi_device = device.query<IDXGIDevice>();
        hr = DCompositionCreateDevice(dxgi_device.get(), IID_PPV_ARGS(dcomp.put()));
        if (FAILED(hr)) return fail("DCompositionCreateDevice", hr);
        hr = dcomp->CreateTargetForHwnd(hwnd, TRUE, target.put());
        if (FAILED(hr)) return fail("CreateTargetForHwnd", hr);
        dcomp->CreateVisual(root.put());
        dcomp->CreateVisual(scene_visual.put());
        dcomp->CreateVisual(ui_visual.put());
        scene_visual->SetContent(swapchain.get());
        root->AddVisual(scene_visual.get(), FALSE, nullptr);
        root->AddVisual(ui_visual.get(), TRUE, scene_visual.get()); // above the scene
        target->SetRoot(root.get());
        dcomp->Commit();
        return true;
    }

    void wait_for_copy() {
        if (!copy_pending) return;
        while (context->GetData(copied.get(), nullptr, 0, 0) == S_FALSE) SwitchToThread();
        copy_pending = false;
    }

    void present() {
        wil::com_ptr<ID3D11Texture2D> back;
        if (FAILED(swapchain->GetBuffer(0, IID_PPV_ARGS(back.put())))) return;
        context->CopyResource(back.get(), scene.get());
        context->End(copied.get());
        copy_pending = true;
        back.reset();
        swapchain->Present(1, 0);
    }

    bool resize(u32 w, u32 h) {
        w = std::max(1u, w);
        h = std::max(1u, h);
        if (w == width && h == height) return true;
        wait_for_copy();
        width = w;
        height = h;
        context->ClearState();
        context->Flush();
        const HRESULT hr = swapchain->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, 0);
        if (FAILED(hr)) return fail("ResizeBuffers", hr);
        return make_scene_texture();
    }
};

Compositor::Compositor() : impl_(std::make_unique<Impl>()) {}
Compositor::~Compositor() = default;

std::unique_ptr<Compositor> Compositor::create(void* hwnd, std::optional<u64> adapter_luid, u32 width, u32 height) {
    std::unique_ptr<Compositor> c(new Compositor());
    if (!c->impl_->init(hwnd, adapter_luid, width, height)) return nullptr;
    return c;
}

void* Compositor::shared_handle() const noexcept { return impl_->scene_handle; }
bool Compositor::resize(u32 width, u32 height) { return impl_->resize(width, height); }
void Compositor::wait_for_copy() { impl_->wait_for_copy(); }
void Compositor::present() { impl_->present(); }
void* Compositor::ui_visual() const noexcept { return static_cast<IUnknown*>(impl_->ui_visual.get()); }
void Compositor::commit() { impl_->dcomp->Commit(); }

} // namespace bridger::app
