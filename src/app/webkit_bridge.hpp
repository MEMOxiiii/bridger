#pragma once

// The page's message channel on WebKit (Linux and macOS hosts, docs/adr/0010). The interface talks to
// `window.chrome.webview` (WebView2's API, src/app/ui/src/bridge.ts); this script, injected at document start,
// provides that object over WebKit's script message handler `bridger`. The host delivers messages to the page by
// evaluating `window.__bridgerReceive(<json>)`.

namespace bridger::app {

inline constexpr const char* kWebKitBridgeScript = R"JS((() => {
  const listeners = [];
  const handler = window.webkit.messageHandlers.bridger;
  window.__bridgerReceive = (data) => {
    for (const listener of listeners) listener({ data });
  };
  window.chrome = window.chrome || {};
  window.chrome.webview = {
    postMessage: (message) => handler.postMessage(typeof message === 'string' ? message : JSON.stringify(message)),
    addEventListener: (type, listener) => {
      if (type === 'message') listeners.push(listener);
    },
    removeEventListener: (type, listener) => {
      const i = listeners.indexOf(listener);
      if (type === 'message' && i >= 0) listeners.splice(i, 1);
    },
  };
})();)JS";

// The URL the hosts load: the embedded interface under a custom scheme, served from memory.
inline constexpr const char* kWebKitStartUrl = "bridger://app/index.html";

} // namespace bridger::app
