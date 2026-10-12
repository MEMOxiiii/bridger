#include "webview_host.hpp"

#include <bridger/core/log.hpp>

#include <Windows.h>
#include <CommCtrl.h>
#include <Shlwapi.h>
#include <windowsx.h>
#include <wrl.h>

#include <WebView2.h>
#include <wil/com.h>

#include <cstdlib>
#include <string>

namespace bridger::app {

using Microsoft::WRL::Callback;

namespace {

std::wstring widen(std::string_view s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<usize>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

std::string narrow(const wchar_t* w) {
    if (w == nullptr) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return {};
    std::string s(static_cast<usize>(n - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
    return s;
}

constexpr std::wstring_view kHost = L"https://bridger.local/";
constexpr UINT_PTR kSubclassId = 0xB71D;

} // namespace

struct WebViewHost::Impl {
    HWND hwnd = nullptr;
    IUnknown* visual = nullptr;
    WebViewCallbacks cb;
    std::wstring start_url;
    bool dev_tools = false;
    wil::com_ptr<ICoreWebView2Environment3> env;
    wil::com_ptr<ICoreWebView2CompositionController> composition;
    wil::com_ptr<ICoreWebView2Controller> controller;
    wil::com_ptr<ICoreWebView2> view;
    HCURSOR cursor = LoadCursorW(nullptr, IDC_ARROW);
    bool tracking_leave = false;
    bool is_ready = false;
    bool subclassed = false;
    bool input_enabled = true;
    // Double-click synthesis (the SDL window class has no CS_DBLCLKS, so Windows never sends
    // WM_xBUTTONDBLCLK; the page needs them for dblclick).
    UINT last_down = 0;
    DWORD last_down_time = 0;
    POINT last_down_pt{};

    void fail(std::string_view what, HRESULT hr) {
        log::error("WebView2: {} (0x{:08x})", what, static_cast<unsigned>(hr));
        if (cb.on_ready) cb.on_ready(false, what);
    }

    // Visual hosting has no child window: the host forwards mouse input (the event kinds share
    // their values with the WM_ messages) and applies the cursor WebView2 asks for.
    bool forward_mouse(UINT msg, WPARAM wp, LPARAM lp) {
        if (!composition) return false;
        POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        UINT32 data = 0;
        switch (msg) {
        case WM_MOUSEWHEEL:
        case WM_MOUSEHWHEEL:
            ScreenToClient(hwnd, &pt); // wheel messages carry screen coordinates
            data = static_cast<UINT32>(GET_WHEEL_DELTA_WPARAM(wp));
            break;
        case WM_XBUTTONDOWN:
        case WM_XBUTTONUP:
        case WM_XBUTTONDBLCLK:
            data = GET_XBUTTON_WPARAM(wp);
            break;
        case WM_MOUSEMOVE:
            if (!tracking_leave) {
                TRACKMOUSEEVENT t{sizeof(t), TME_LEAVE, hwnd, 0};
                tracking_leave = TrackMouseEvent(&t) != FALSE;
            }
            break;
        case WM_MOUSELEAVE:
            tracking_leave = false;
            break;
        case WM_LBUTTONDOWN:
        case WM_RBUTTONDOWN:
        case WM_MBUTTONDOWN: {
            SetCapture(hwnd);
            if (controller) controller->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
            const DWORD now = GetMessageTime();
            const bool dbl = msg == last_down && now - last_down_time <= GetDoubleClickTime() &&
                             std::abs(pt.x - last_down_pt.x) <= GetSystemMetrics(SM_CXDOUBLECLK) / 2 &&
                             std::abs(pt.y - last_down_pt.y) <= GetSystemMetrics(SM_CYDOUBLECLK) / 2;
            last_down = dbl ? 0 : msg; // a third click starts a new pair
            last_down_time = now;
            last_down_pt = pt;
            if (dbl) msg += WM_LBUTTONDBLCLK - WM_LBUTTONDOWN; // DOWN -> DBLCLK (same offset for L, R, M)
            break;
        }
        case WM_LBUTTONUP:
        case WM_RBUTTONUP:
        case WM_MBUTTONUP:
            if ((GET_KEYSTATE_WPARAM(wp) & (MK_LBUTTON | MK_RBUTTON | MK_MBUTTON)) == 0) ReleaseCapture();
            break;
        default:
            break;
        }
        const auto keys = static_cast<COREWEBVIEW2_MOUSE_EVENT_VIRTUAL_KEYS>(msg == WM_MOUSELEAVE ? 0 : GET_KEYSTATE_WPARAM(wp));
        composition->SendMouseInput(static_cast<COREWEBVIEW2_MOUSE_EVENT_KIND>(msg), keys, data, pt);
        return true;
    }

    static LRESULT CALLBACK subclass_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR ref) {
        auto* self = reinterpret_cast<Impl*>(ref);
        if (((msg >= WM_MOUSEFIRST && msg <= WM_MOUSELAST) || msg == WM_MOUSELEAVE) && self->input_enabled) {
            self->forward_mouse(msg, wp, lp);
            return 0;
        }
        if (msg == WM_SETCURSOR && LOWORD(lp) == HTCLIENT && self->composition) {
            SetCursor(self->cursor);
            return TRUE;
        }
        return DefSubclassProc(h, msg, wp, lp);
    }

    void on_controller(ICoreWebView2CompositionController* c) {
        composition = c;
        controller = composition.query<ICoreWebView2Controller>();
        controller->get_CoreWebView2(view.put());
        composition->put_RootVisualTarget(visual);
        // Transparent page background: the 3D scene shows through (docs/adr/0009).
        if (auto c2 = controller.try_query<ICoreWebView2Controller2>()) c2->put_DefaultBackgroundColor({0, 0, 0, 0});
        composition->add_CursorChanged(Callback<ICoreWebView2CursorChangedEventHandler>([this](ICoreWebView2CompositionController* s, IUnknown*) {
                                           HCURSOR h = nullptr;
                                           if (SUCCEEDED(s->get_Cursor(&h)) && h != nullptr) cursor = h;
                                           SetCursor(cursor);
                                           return S_OK;
                                       }).Get(),
                                       nullptr);
        SetWindowSubclass(hwnd, &subclass_proc, kSubclassId, reinterpret_cast<DWORD_PTR>(this));
        subclassed = true;

        wil::com_ptr<ICoreWebView2Settings> settings;
        view->get_Settings(settings.put());
        settings->put_AreDevToolsEnabled(dev_tools ? TRUE : FALSE);
        settings->put_AreDefaultContextMenusEnabled(dev_tools ? TRUE : FALSE);
        settings->put_IsStatusBarEnabled(FALSE);
        settings->put_IsZoomControlEnabled(FALSE);
        if (auto s3 = settings.try_query<ICoreWebView2Settings3>()) s3->put_AreBrowserAcceleratorKeysEnabled(dev_tools ? TRUE : FALSE);

        RECT r{};
        GetClientRect(hwnd, &r);
        controller->put_Bounds(r);
        controller->put_IsVisible(TRUE);

        // UI files from memory.
        view->AddWebResourceRequestedFilter((std::wstring(kHost) + L"*").c_str(), COREWEBVIEW2_WEB_RESOURCE_CONTEXT_ALL);
        view->add_WebResourceRequested(
            Callback<ICoreWebView2WebResourceRequestedEventHandler>([this](ICoreWebView2*, ICoreWebView2WebResourceRequestedEventArgs* args) {
                wil::com_ptr<ICoreWebView2WebResourceRequest> req;
                args->get_Request(req.put());
                wil::unique_cotaskmem_string uri;
                req->get_Uri(&uri);
                std::wstring u = uri.get();
                if (u.rfind(kHost, 0) != 0) return S_OK;
                std::string path = narrow(u.substr(kHost.size()).c_str());
                if (const auto q = path.find_first_of("?#"); q != std::string::npos) path.resize(q);
                if (path.empty()) path = "index.html";
                std::optional<WebResource> res = cb.resource ? cb.resource(path) : std::nullopt;
                wil::com_ptr<ICoreWebView2WebResourceResponse> response;
                if (res) {
                    wil::com_ptr<IStream> stream;
                    stream.attach(SHCreateMemStream(res->bytes.data(), static_cast<UINT>(res->bytes.size())));
                    const std::wstring headers = L"Content-Type: " + widen(res->mime) + L"\r\nCache-Control: no-store";
                    env->CreateWebResourceResponse(stream.get(), 200, L"OK", headers.c_str(), response.put());
                } else {
                    env->CreateWebResourceResponse(nullptr, 404, L"Not Found", L"", response.put());
                }
                args->put_Response(response.get());
                return S_OK;
            }).Get(),
            nullptr);

        view->add_WebMessageReceived(
            Callback<ICoreWebView2WebMessageReceivedEventHandler>([this](ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs* args) {
                wil::unique_cotaskmem_string json;
                if (FAILED(args->get_WebMessageAsJson(&json))) return S_OK;
                if (cb.on_message) cb.on_message(narrow(json.get()));
                return S_OK;
            }).Get(),
            nullptr);

        view->add_NavigationCompleted(
            Callback<ICoreWebView2NavigationCompletedEventHandler>([this](ICoreWebView2*, ICoreWebView2NavigationCompletedEventArgs* a) {
                BOOL ok = FALSE;
                a->get_IsSuccess(&ok);
                if (!is_ready) {
                    is_ready = ok == TRUE;
                    if (cb.on_ready) cb.on_ready(is_ready, is_ready ? "" : "the user interface failed to load");
                }
                return S_OK;
            }).Get(),
            nullptr);
        view->Navigate(start_url.c_str());
    }
};

WebViewHost::WebViewHost() : impl_(std::make_unique<Impl>()) {}
WebViewHost::~WebViewHost() {
    if (impl_->subclassed) RemoveWindowSubclass(impl_->hwnd, &Impl::subclass_proc, kSubclassId);
    if (impl_->controller) impl_->controller->Close();
}

std::unique_ptr<WebViewHost> WebViewHost::create(void* hwnd, void* visual, std::string start_url, std::string user_data_dir,
                                                 WebViewCallbacks callbacks, bool dev_tools) {
    std::unique_ptr<WebViewHost> host(new WebViewHost());
    Impl* m = host->impl_.get();
    m->hwnd = static_cast<HWND>(hwnd);
    m->visual = static_cast<IUnknown*>(visual);
    m->cb = std::move(callbacks);
    m->start_url = widen(start_url);
    m->dev_tools = dev_tools;
    const std::wstring data = widen(user_data_dir);
    const HRESULT hr = CreateCoreWebView2EnvironmentWithOptions(
        nullptr, data.c_str(), nullptr,
        Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>([m](HRESULT r, ICoreWebView2Environment* env) {
            if (FAILED(r) || env == nullptr) {
                m->fail("the WebView2 runtime is not available", r);
                return S_OK;
            }
            m->env = wil::try_com_query<ICoreWebView2Environment3>(env);
            if (!m->env) {
                m->fail("the WebView2 runtime is too old (visual hosting needs version 1.0.774 or later)", E_NOINTERFACE);
                return S_OK;
            }
            m->env->CreateCoreWebView2CompositionController(
                m->hwnd,
                Callback<ICoreWebView2CreateCoreWebView2CompositionControllerCompletedHandler>(
                    [m](HRESULT r2, ICoreWebView2CompositionController* c) {
                        if (FAILED(r2) || c == nullptr) {
                            m->fail("creating the WebView2 controller failed", r2);
                            return S_OK;
                        }
                        m->on_controller(c);
                        return S_OK;
                    })
                    .Get());
            return S_OK;
        }).Get());
    if (FAILED(hr)) {
        m->fail("the WebView2 runtime is not installed", hr);
        return nullptr;
    }
    return host;
}

void WebViewHost::resize(int width, int height) {
    if (impl_->controller) impl_->controller->put_Bounds({0, 0, width, height});
}

void WebViewHost::post_json(std::string_view json) {
    if (impl_->view) impl_->view->PostWebMessageAsJson(widen(json).c_str());
}

void WebViewHost::focus() {
    if (impl_->controller) impl_->controller->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
}

void WebViewHost::set_input_enabled(bool enabled) {
    impl_->input_enabled = enabled;
    if (!enabled && impl_->composition) {
        // The page must not keep a hover or a pressed button from before.
        impl_->composition->SendMouseInput(COREWEBVIEW2_MOUSE_EVENT_KIND_LEAVE, COREWEBVIEW2_MOUSE_EVENT_VIRTUAL_KEYS_NONE, 0, {0, 0});
        ReleaseCapture();
    }
}

bool WebViewHost::ready() const noexcept { return impl_->is_ready; }

} // namespace bridger::app
