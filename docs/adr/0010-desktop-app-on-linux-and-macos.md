# 0010. Desktop app on Linux and macOS: one app core, a native host per system

- Status: accepted (after 1.2.0). Extends ADR 0009 (which stays the Windows design).
- Date: 2026-10-20

## Context

`bridger-gui.exe` (ADR 0009) is an SDL3 window with WebView2 composited over a Vulkan scene through Direct3D 11 and
DirectComposition. None of those three layers exists on Linux or macOS. The maintainer asked for the desktop app on
both systems after the command line was ported (1.2.0). Requirements carried over from ADR 0009: the same React
interface (no second UI), the native Vulkan viewer underneath it with real transparency, no local server, no
Electron.

What each system offers for "a web page with a transparent background over native drawing":

- **Linux:** WebKitGTK (the GTK 4 build, API `webkitgtk-6.0`) is the system web engine on every major desktop
  distribution. In GTK 4 every widget is composited by GTK's own renderer, so a `WebKitWebView` with a transparent
  background (`webkit_web_view_set_background_color`, alpha 0) stacked in a `GtkOverlay` over another widget blends
  with real alpha. A Vulkan swapchain cannot be a GTK 4 widget, but a texture can (`GdkTexture` in a `GtkPicture`).
- **macOS:** `WKWebView` is an `NSView`; with `drawsBackground` off it is transparent, and Core Animation blends it
  over the layer-backed view below it.
- **Vulkan on macOS** exists only through MoltenVK (Vulkan over Metal, Apache-2.0).

## Decision

1. **One app core, three hosts.** `src/app/app.cpp` keeps every behaviour (bridge messages, documents, workers,
   viewer, converting, settings, updates) and loses every OS call. Those move behind `app::Host`
   (`src/app/host.hpp`): the window, the web view, presenting the scene, file dialogs, drops, the message loop and
   the few shell helpers. One implementation per system, chosen at build time (pimpl, no virtual calls):
   - `win32/host_win32.cpp`: the ADR 0009 stack unchanged (SDL3, WebView2, `Compositor`), moved out of `app.cpp`.
   - `gtk/host_gtk.cpp`: GTK 4 + WebKitGTK 6.0, from the system (pkg-config); not shipped with Bridger.
   - `cocoa/host_cocoa.mm`: AppKit + WebKit (system frameworks), Objective-C++.
2. **The scene on Linux and macOS goes through memory.** The renderer draws offscreen (its existing target for
   `bridger render`), and the host shows the pixels under the web view: a `GdkMemoryTexture` in a `GtkPicture`
   (Linux), a `CGImage` as the contents of a layer-backed view (macOS). This costs one GPU → CPU copy and one upload
   per frame, against a zero-copy path that would need driver-specific interop (DMA-BUF export on Linux, IOSurface
   on macOS) that this project cannot test on real hardware for macOS. The app draws only when something changes
   (ADR 0009), so an idle window copies nothing. The cost is measured in `docs/performance.md`; the zero-copy paths
   stay open behind the same `Host` call if a measurement asks for them.
3. **The same interface, the same protocol.** The page talks to `window.chrome.webview` (WebView2's API). On WebKit
   the host injects a short script at document start that provides that object over WebKit's message handlers
   (`webkit.messageHandlers.bridger`), so `bridge.ts` and every message in `docs/app/bridge.md` stay the same. Files
   are served from the embedded zip under a custom scheme (`bridger://app/`; WebKitGTK
   `webkit_web_context_register_uri_scheme`, `WKURLSchemeHandler` on macOS), still from memory, still offline.
4. **Fly mode input.** Windows keeps native relative mouse mode (SDL). On Linux and macOS the page drives fly mode
   with the browser's Pointer Lock API and sends the mouse deltas and the pressed keys (by `KeyboardEvent.code`, so
   any keyboard layout works) as `look` / `fly_keys` messages; `hello.fly_input` says which mode the host uses.
   Without pointer lock (refused, or a Wayland compositor without pointer constraints) the page falls back to
   looking around while a mouse button is held.
5. **Vulkan on macOS:** MoltenVK, shipped inside the app bundle (`Bridger.app/Contents/Frameworks/libMoltenVK.dylib`)
   and loaded directly (it exports the Vulkan entry points; no Vulkan loader). If the device does not offer what
   the renderer requires, the app works without the 3D preview and says why, as on Windows without a GPU.
6. **Updates on Linux and macOS** are offered with the release page (no self-install): a Linux system package of
   GTK/WebKit and an app bundle in `/Applications` are not folders Bridger should rewrite itself. The update check,
   the notes and "skip this version" work as on Windows. `https_get` on POSIX uses the system's libcurl
   (`libcurl.so.4`, `/usr/lib/libcurl.4.dylib`), loaded at run time so the command line keeps no link dependency;
   SHA-1/SHA-256 are computed by Bridger's own portable code (checked against the NIST test vectors).

## Consequences

- `app.cpp` becomes testable without a window; the Windows app keeps its exact behaviour (same code, moved).
- Linux users need GTK 4.14+ and WebKitGTK 6.0 from their distribution (Ubuntu 24.04, Debian 13, Fedora 40 and
  later have them); the archive says so. A Flatpak or AppImage can follow.
- The macOS app is not signed or notarized (maintainer's decision for now): Gatekeeper asks the user to allow it
  once. It is built and smoke-tested by CI on a macOS runner, but nobody on the project runs it on a real Mac yet;
  `docs/testing.md` lists what still needs a person.
- One copy per frame on Linux and macOS (decision 2). CSS `backdrop-filter` still cannot blur the scene (ADR 0009).
- New dependencies (`docs/THIRD_PARTY.md`): GTK 4 and WebKitGTK (system, LGPL-2.1+, dynamically linked, not
  shipped), MoltenVK (Apache-2.0, shipped in the macOS bundle), libcurl (system, curl license, loaded at run time).
