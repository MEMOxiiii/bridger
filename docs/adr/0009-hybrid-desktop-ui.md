# 0009. Desktop app: web UI in WebView2, composited over the native Vulkan viewer

- Status: accepted (Phase 8). Supersedes the "Dear ImGui" part of ADR 0005.
- Date: 2026-10-13

## Context

The build specification planned the desktop app with SDL3 + Dear ImGui. For Phase 8 the maintainer asked for a
UI/UX of much higher quality: motion, typography and layout built with the GSAP, Lenis and ui-ux skills, which
are web technologies. The maintainer rejected Electron and preferred a hybrid over a Rust (Tauri) shell. The 3D
viewer must stay native: it renders worlds with millions of quads and must not go through a browser.

## Decision

- **Shell:** `bridger-gui.exe` is a native C++ program. SDL3 creates the window and handles input, file
  dialogs and drag and drop. The C++ core (`convert`, `render`, ...) runs in-process; there is no separate
  backend process and no local server.
- **UI:** React + TypeScript (`src/app/ui`), animated with GSAP (`@gsap/react`), smooth scrolling with Lenis,
  built by Vite into static files. CMake runs `npm ci` / `npm run build` (`src/app/CMakeLists.txt`) and embeds
  the result as a zip. The app serves it to WebView2 from memory under `https://bridger.local/`
  (`WebResourceRequested`); nothing is written to disk and no network access is needed.
- **Composition (the part that makes the hybrid work):** WebView2 is hosted in *visual* mode
  (`ICoreWebView2CompositionController`), not as a child window. A child `HWND` cannot be transparent over a
  Vulkan swapchain: its transparent pixels only blend with the parent's GDI surface, so the scene showed as
  black (verified 2026-10-13). Instead:
  - Vulkan renders into a D3D11 texture shared by an NT handle (`VK_KHR_external_memory_win32`, handle type
    `D3D11_TEXTURE`; `Renderer::set_shared_target`, `src/render/win32/external_image_win32.cpp`);
  - D3D11 copies it into a DXGI composition swapchain (`CreateSwapChainForComposition`);
  - DirectComposition stacks two visuals: the swapchain at the bottom, WebView2's root visual on top
    (`src/app/win32/compositor.cpp`). The page background is transparent, so the UI floats over the scene with
    real alpha blending.
  - Synchronisation is simple and safe: `Renderer::render` waits for its fence before D3D11 copies, and the app
    waits for the D3D11 copy (an event query) before the next Vulkan frame. One shared texture is enough.
- **Input:** in visual mode the host forwards mouse messages to WebView2 (`SendMouseInput`) through a window
  subclass, applies the cursor WebView2 asks for, and synthesises double-clicks (the SDL window class has no
  `CS_DBLCLKS`). Camera input reaches the viewer as small JSON messages from the page (orbit/pan/zoom deltas,
  batched per animation frame). Fly mode is the exception: it switches to SDL relative mouse mode and reads
  WASD natively by scancode (any keyboard layout), and WebView2 stops receiving the mouse until Esc.
- **Bridge:** JSON messages both ways (`chrome.webview.postMessage` / `PostWebMessageAsJson`), documented in
  `docs/app/bridge.md`. Work that takes time (opening, meshing, converting) runs on worker threads; results are
  posted back to the UI thread through a task queue, because WebView2 and the renderer are single-threaded.
- **OS code:** the app's `win32/` folder and `src/render/win32/` hold Windows-only code (WebView2, D3D11,
  DirectComposition, the Win32 handle import). This extends the "only `platform` includes OS headers" rule to
  these two presentation folders, which cannot be written portably; `render` stays OS-free everywhere else,
  and a `posix/` stub keeps the module building elsewhere.

## Consequences

- The UI can use the web's typography, layout and animation tools; it is designed and reviewed in a normal
  browser too (`npm run dev` with a mock bridge, `src/app/ui/src/mock.ts`).
- Building the app needs Node.js/npm (CI installs Node; `package-lock.json` pins every package).
- The WebView2 Runtime must be present. It ships with Windows 11 and current Windows 10; if it is missing the
  app says so at start.
- CSS `backdrop-filter` cannot blur the native layer underneath (it is outside the page), so panels use mostly
  opaque surfaces instead of "glass".
- Each frame costs one GPU copy (D3D11 `CopyResource`) and a CPU wait on the Vulkan fence; both are measured in
  `docs/performance.md`. The app renders only when something changes (camera motion, streaming, animation), so
  an idle window uses no GPU time.
