// Linux host (docs/adr/0010): a GTK 4 window with a GtkOverlay; the scene is a GtkPicture showing the renderer's
// offscreen pixels, and a WebKitWebView with a transparent background lies on top of it. GTK composites both with
// real alpha. The interface is served from memory under bridger://app/.

#include "../host.hpp"
#include "../webkit_bridge.hpp"

#include <bridger/core/log.hpp>

#include <gtk/gtk.h>
#include <webkit/webkit.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <string>

namespace bridger::app {
namespace {

constexpr const char* kCss = "window.bridger-window { background-color: #0F1217; }"; // --bg in the interface's tokens.css

// A GTK dialog's callback, owned by GTK until it answers.
struct DialogCall {
    PathCallback done;
};

std::optional<std::string> path_of(GFile* file) {
    if (file == nullptr) return std::nullopt;
    char* p = g_file_get_path(file);
    g_object_unref(file);
    if (p == nullptr) return std::nullopt;
    std::string s(p);
    g_free(p);
    return s;
}

GListModel* make_filters(std::span<const FileFilter> filters) {
    GListStore* store = g_list_store_new(GTK_TYPE_FILE_FILTER);
    for (const FileFilter& f : filters) {
        GtkFileFilter* filter = gtk_file_filter_new();
        gtk_file_filter_set_name(filter, f.name.c_str());
        std::string_view rest = f.patterns;
        while (!rest.empty()) {
            const usize semi = rest.find(';');
            const std::string one(rest.substr(0, semi));
            if (one == "*") gtk_file_filter_add_pattern(filter, "*");
            else if (!one.empty()) gtk_file_filter_add_suffix(filter, one.c_str());
            rest = semi == std::string_view::npos ? std::string_view() : rest.substr(semi + 1);
        }
        g_list_store_append(store, filter);
        g_object_unref(filter);
    }
    return G_LIST_MODEL(store);
}

} // namespace

struct Host::Impl {
    HostEvents events;
    GtkWidget* window = nullptr;
    GtkWidget* overlay = nullptr;
    GtkWidget* picture = nullptr;
    WebKitWebView* web = nullptr;
    WebKitUserContentManager* content = nullptr;
    std::function<std::optional<WebFile>(std::string_view)> resource;
    u32 width = 1, height = 1;
    bool quit = false;
    bool web_reported = false;

    ~Impl() {
        if (window != nullptr) gtk_window_destroy(GTK_WINDOW(window));
        if (content != nullptr) g_object_unref(content);
        while (g_main_context_pending(nullptr)) g_main_context_iteration(nullptr, FALSE);
    }

    // The scene size in pixels: the overlay's size times the window's scale (fractional on GTK 4.12+).
    void check_size() {
        if (overlay == nullptr) return;
        double scale = gtk_widget_get_scale_factor(overlay);
        if (GtkNative* native = gtk_widget_get_native(overlay))
            if (GdkSurface* surface = gtk_native_get_surface(native)) scale = gdk_surface_get_scale(surface);
        const int lw = gtk_widget_get_width(overlay), lh = gtk_widget_get_height(overlay);
        if (lw <= 0 || lh <= 0) return;
        const auto w = static_cast<u32>(std::lround(lw * scale)), h = static_cast<u32>(std::lround(lh * scale));
        if (w == width && h == height) return;
        width = w;
        height = h;
        events.resized(w, h);
    }

    // ---- GTK and WebKit callbacks --------------------------------------------------------------

    static gboolean on_close(GtkWindow*, gpointer self) {
        static_cast<Impl*>(self)->events.close_requested();
        return TRUE; // the app decides; it quits the loop
    }

    static void on_script_message(WebKitUserContentManager*, JSCValue* value, gpointer self) {
        char* text = jsc_value_to_string(value);
        if (text == nullptr) return;
        const std::string copy(text);
        g_free(text);
        static_cast<Impl*>(self)->events.message(copy);
    }

    static void on_scheme_request(WebKitURISchemeRequest* request, gpointer self) {
        auto* d = static_cast<Impl*>(self);
        std::string_view path = webkit_uri_scheme_request_get_path(request);
        while (path.starts_with('/')) path.remove_prefix(1);
        const auto file = d->resource ? d->resource(path) : std::nullopt;
        if (!file) {
            GError* error = g_error_new(G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "not part of the interface: %s", std::string(path).c_str());
            webkit_uri_scheme_request_finish_error(request, error);
            g_error_free(error);
            return;
        }
        GInputStream* stream = g_memory_input_stream_new_from_data(file->bytes.data(), static_cast<gssize>(file->bytes.size()), nullptr);
        WebKitURISchemeResponse* response = webkit_uri_scheme_response_new(stream, static_cast<gint64>(file->bytes.size()));
        webkit_uri_scheme_response_set_content_type(response, file->mime.c_str());
        webkit_uri_scheme_request_finish_with_response(request, response);
        g_object_unref(response);
        g_object_unref(stream);
    }

    static void on_load_changed(WebKitWebView*, WebKitLoadEvent event, gpointer self) {
        auto* d = static_cast<Impl*>(self);
        if (event == WEBKIT_LOAD_FINISHED && !d->web_reported) {
            d->web_reported = true;
            d->events.web_ready(true, {});
        }
    }

    static gboolean on_load_failed(WebKitWebView*, WebKitLoadEvent, char* uri, GError* error, gpointer self) {
        auto* d = static_cast<Impl*>(self);
        if (!d->web_reported) {
            d->web_reported = true;
            d->events.web_ready(false, std::string(error != nullptr ? error->message : "unknown error") + " (" + (uri ? uri : "") +
                                           ")\n\nBridger needs WebKitGTK 6.0 (package libwebkitgtk-6.0-4 or webkitgtk6.0).");
        }
        return TRUE;
    }

    // Links never navigate the interface away: https links open in the browser, anything else is refused.
    static gboolean on_decide_policy(WebKitWebView*, WebKitPolicyDecision* decision, WebKitPolicyDecisionType type, gpointer self) {
        if (type != WEBKIT_POLICY_DECISION_TYPE_NAVIGATION_ACTION && type != WEBKIT_POLICY_DECISION_TYPE_NEW_WINDOW_ACTION) return FALSE;
        WebKitNavigationAction* action = webkit_navigation_policy_decision_get_navigation_action(WEBKIT_NAVIGATION_POLICY_DECISION(decision));
        const char* uri = webkit_uri_request_get_uri(webkit_navigation_action_get_request(action));
        const std::string_view u = uri != nullptr ? uri : "";
        if (u.starts_with("bridger://") && type == WEBKIT_POLICY_DECISION_TYPE_NAVIGATION_ACTION) return FALSE;
        if (u.starts_with("https://")) {
            GtkUriLauncher* launcher = gtk_uri_launcher_new(uri);
            gtk_uri_launcher_launch(launcher, GTK_WINDOW(static_cast<Impl*>(self)->window), nullptr, nullptr, nullptr);
            g_object_unref(launcher);
        }
        webkit_policy_decision_ignore(decision);
        return TRUE;
    }

    static gboolean on_context_menu(WebKitWebView*, WebKitContextMenu*, WebKitHitTestResult*, gpointer) { return TRUE; }

    static GdkDragAction on_drop_enter(GtkDropTarget*, double, double, gpointer self) {
        static_cast<Impl*>(self)->events.drop_hover(true);
        return GDK_ACTION_COPY;
    }
    static void on_drop_leave(GtkDropTarget*, gpointer self) { static_cast<Impl*>(self)->events.drop_hover(false); }
    static gboolean on_drop(GtkDropTarget*, const GValue* value, double, double, gpointer self) {
        auto* d = static_cast<Impl*>(self);
        d->events.drop_hover(false);
        if (!G_VALUE_HOLDS(value, GDK_TYPE_FILE_LIST)) return FALSE;
        auto* files = static_cast<GSList*>(g_value_get_boxed(value));
        for (GSList* l = files; l != nullptr; l = l->next) {
            char* p = g_file_get_path(static_cast<GFile*>(l->data));
            if (p == nullptr) continue;
            const std::string path(p);
            g_free(p);
            d->events.drop_file(path);
        }
        return TRUE;
    }

    // Runs the loop once, waiting at most `ms` for an event.
    static void iterate(int ms) {
        if (ms > 0) {
            GSource* timer = g_timeout_source_new(static_cast<guint>(ms));
            g_source_set_callback(timer, [](gpointer) -> gboolean { return G_SOURCE_REMOVE; }, nullptr, nullptr);
            g_source_attach(timer, nullptr);
            g_main_context_iteration(nullptr, TRUE);
            g_source_destroy(timer);
            g_source_unref(timer);
        }
        while (g_main_context_pending(nullptr)) g_main_context_iteration(nullptr, FALSE);
    }
};

Host::Host() : impl_(std::make_unique<Impl>()) {}
Host::~Host() = default;

std::unique_ptr<Host> Host::create(HostEvents events) {
    if (!gtk_init_check()) {
        log::error("GTK could not start (no display?)");
        return nullptr;
    }
    std::unique_ptr<Host> host(new Host());
    Impl& d = *host->impl_;
    d.events = std::move(events);
    g_object_set(gtk_settings_get_default(), "gtk-application-prefer-dark-theme", TRUE, nullptr);
    GtkCssProvider* css = gtk_css_provider_new();
    gtk_css_provider_load_from_string(css, kCss);
    gtk_style_context_add_provider_for_display(gdk_display_get_default(), GTK_STYLE_PROVIDER(css), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(css);

    d.window = gtk_window_new();
    gtk_widget_add_css_class(d.window, "bridger-window");
    gtk_window_set_title(GTK_WINDOW(d.window), "Bridger");
    gtk_window_set_default_size(GTK_WINDOW(d.window), 1440, 900);
    g_signal_connect(d.window, "close-request", G_CALLBACK(&Impl::on_close), &d);

    d.overlay = gtk_overlay_new();
    gtk_widget_set_size_request(d.overlay, 960, 600);
    d.picture = gtk_picture_new();
    gtk_picture_set_content_fit(GTK_PICTURE(d.picture), GTK_CONTENT_FIT_FILL);
    gtk_picture_set_can_shrink(GTK_PICTURE(d.picture), TRUE);
    gtk_widget_set_hexpand(d.picture, TRUE);
    gtk_widget_set_vexpand(d.picture, TRUE);
    gtk_overlay_set_child(GTK_OVERLAY(d.overlay), d.picture);
    gtk_window_set_child(GTK_WINDOW(d.window), d.overlay);

    // Files dropped anywhere on the window reach the app, before the web view would handle them.
    GtkDropTarget* drop = gtk_drop_target_new(GDK_TYPE_FILE_LIST, GDK_ACTION_COPY);
    gtk_event_controller_set_propagation_phase(GTK_EVENT_CONTROLLER(drop), GTK_PHASE_CAPTURE);
    g_signal_connect(drop, "enter", G_CALLBACK(&Impl::on_drop_enter), &d);
    g_signal_connect(drop, "leave", G_CALLBACK(&Impl::on_drop_leave), &d);
    g_signal_connect(drop, "drop", G_CALLBACK(&Impl::on_drop), &d);
    gtk_widget_add_controller(d.overlay, GTK_EVENT_CONTROLLER(drop));

    d.width = 1440;
    d.height = 900;
    return host;
}

SceneMode Host::scene_mode() const noexcept { return SceneMode::Pixels; }
u32 Host::width() const noexcept { return impl_->width; }
u32 Host::height() const noexcept { return impl_->height; }
bool Host::attach_scene(std::optional<u64>) { return true; }
void* Host::shared_handle() const noexcept { return nullptr; }
void Host::begin_frame() {}
void Host::present() {}

void Host::present_pixels(std::span<const u8> rgba, u32 width, u32 height) {
    if (rgba.size() < static_cast<usize>(width) * height * 4) return;
    GBytes* bytes = g_bytes_new(rgba.data(), static_cast<gsize>(width) * height * 4);
    // The alpha byte is not meant as transparency: the scene is opaque.
    GdkTexture* texture = gdk_memory_texture_new(static_cast<int>(width), static_cast<int>(height), GDK_MEMORY_R8G8B8X8, bytes,
                                                 static_cast<gsize>(width) * 4);
    gtk_picture_set_paintable(GTK_PICTURE(impl_->picture), GDK_PAINTABLE(texture));
    g_object_unref(texture);
    g_bytes_unref(bytes);
}

void Host::start_web(std::function<std::optional<WebFile>(std::string_view path)> resource, bool dev_tools) {
    Impl& d = *impl_;
    d.resource = std::move(resource);
    WebKitWebContext* context = webkit_web_context_get_default();
    webkit_web_context_register_uri_scheme(context, "bridger", &Impl::on_scheme_request, &d, nullptr);
    WebKitSecurityManager* security = webkit_web_context_get_security_manager(context);
    webkit_security_manager_register_uri_scheme_as_secure(security, "bridger");
    webkit_security_manager_register_uri_scheme_as_cors_enabled(security, "bridger");

    d.content = webkit_user_content_manager_new();
    WebKitUserScript* script = webkit_user_script_new(kWebKitBridgeScript, WEBKIT_USER_CONTENT_INJECT_TOP_FRAME,
                                                      WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_START, nullptr, nullptr);
    webkit_user_content_manager_add_script(d.content, script);
    webkit_user_script_unref(script);
    webkit_user_content_manager_register_script_message_handler(d.content, "bridger", nullptr);
    g_signal_connect(d.content, "script-message-received::bridger", G_CALLBACK(&Impl::on_script_message), &d);

    d.web = WEBKIT_WEB_VIEW(g_object_new(WEBKIT_TYPE_WEB_VIEW, "web-context", context, "user-content-manager", d.content, nullptr));
    const GdkRGBA transparent{0.0f, 0.0f, 0.0f, 0.0f};
    webkit_web_view_set_background_color(d.web, &transparent);
    WebKitSettings* settings = webkit_web_view_get_settings(d.web);
    webkit_settings_set_enable_developer_extras(settings, dev_tools);
    webkit_settings_set_enable_back_forward_navigation_gestures(settings, FALSE);
    g_signal_connect(d.web, "load-changed", G_CALLBACK(&Impl::on_load_changed), &d);
    g_signal_connect(d.web, "load-failed", G_CALLBACK(&Impl::on_load_failed), &d);
    g_signal_connect(d.web, "decide-policy", G_CALLBACK(&Impl::on_decide_policy), &d);
    if (!dev_tools) g_signal_connect(d.web, "context-menu", G_CALLBACK(&Impl::on_context_menu), &d);
    gtk_overlay_add_overlay(GTK_OVERLAY(d.overlay), GTK_WIDGET(d.web));
    webkit_web_view_load_uri(d.web, kWebKitStartUrl);
}

void Host::post_json(std::string_view json) {
    if (impl_->web == nullptr) return;
    const std::string script = "window.__bridgerReceive(" + std::string(json) + ")";
    webkit_web_view_evaluate_javascript(impl_->web, script.c_str(), static_cast<gssize>(script.size()), nullptr, nullptr, nullptr, nullptr,
                                        nullptr);
}

void Host::focus_web() {
    if (impl_->web != nullptr) gtk_widget_grab_focus(GTK_WIDGET(impl_->web));
}

void Host::show() {
    gtk_window_present(GTK_WINDOW(impl_->window));
    if (impl_->web != nullptr) gtk_widget_grab_focus(GTK_WIDGET(impl_->web));
}

void Host::set_title(std::string_view utf8) { gtk_window_set_title(GTK_WINDOW(impl_->window), std::string(utf8).c_str()); }

void Host::error_box(std::string_view message) {
    // GtkAlertDialog is asynchronous; wait for the answer here (this is only used before quitting).
    GtkAlertDialog* alert = gtk_alert_dialog_new("%s", std::string(message).c_str());
    bool done = false;
    gtk_alert_dialog_choose(
        alert, impl_->window != nullptr ? GTK_WINDOW(impl_->window) : nullptr, nullptr,
        [](GObject* source, GAsyncResult* result, gpointer flag) {
            gtk_alert_dialog_choose_finish(GTK_ALERT_DIALOG(source), result, nullptr);
            *static_cast<bool*>(flag) = true;
        },
        &done);
    while (!done) g_main_context_iteration(nullptr, TRUE);
    g_object_unref(alert);
}

bool Host::save_screenshot(std::string_view png_path) {
    GtkWidget* w = impl_->window;
    const int width = gtk_widget_get_width(w), height = gtk_widget_get_height(w);
    GskRenderer* renderer = gtk_native_get_renderer(GTK_NATIVE(w));
    if (width <= 0 || height <= 0 || renderer == nullptr) return false;
    GdkPaintable* paintable = gtk_widget_paintable_new(w);
    GtkSnapshot* snapshot = gtk_snapshot_new();
    gdk_paintable_snapshot(paintable, GDK_SNAPSHOT(snapshot), width, height);
    GskRenderNode* node = gtk_snapshot_free_to_node(snapshot);
    bool ok = false;
    if (node != nullptr) {
        const graphene_rect_t area = GRAPHENE_RECT_INIT(0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height));
        GdkTexture* texture = gsk_renderer_render_texture(renderer, node, &area);
        ok = gdk_texture_save_to_png(texture, std::string(png_path).c_str());
        g_object_unref(texture);
        gsk_render_node_unref(node);
    }
    g_object_unref(paintable);
    return ok;
}

void Host::run(const std::function<bool()>& tick) {
    using clock = std::chrono::steady_clock;
    bool busy = true;
    auto last = clock::now();
    while (!impl_->quit) {
        // Busy: about one pass per display frame (the scene is drawn offscreen, so nothing else paces it).
        // Idle: wait for input, at most ~15 ms.
        const auto since = std::chrono::duration_cast<std::chrono::milliseconds>(clock::now() - last).count();
        Impl::iterate(busy ? std::max(0, 16 - static_cast<int>(since)) : 15);
        if (impl_->quit) break;
        impl_->check_size();
        last = clock::now();
        busy = tick();
    }
}

void Host::quit() { impl_->quit = true; }

void Host::open_file(std::span<const FileFilter> filters, PathCallback done) {
    GtkFileDialog* dialog = gtk_file_dialog_new();
    GListModel* list = make_filters(filters);
    gtk_file_dialog_set_filters(dialog, list);
    g_object_unref(list);
    gtk_file_dialog_open(
        dialog, GTK_WINDOW(impl_->window), nullptr,
        [](GObject* source, GAsyncResult* result, gpointer data) {
            std::unique_ptr<DialogCall> call(static_cast<DialogCall*>(data));
            call->done(path_of(gtk_file_dialog_open_finish(GTK_FILE_DIALOG(source), result, nullptr)));
        },
        std::make_unique<DialogCall>(DialogCall{std::move(done)}).release());
    g_object_unref(dialog);
}

void Host::open_folder(std::string_view default_dir, PathCallback done) {
    GtkFileDialog* dialog = gtk_file_dialog_new();
    if (!default_dir.empty()) {
        GFile* folder = g_file_new_for_path(std::string(default_dir).c_str());
        gtk_file_dialog_set_initial_folder(dialog, folder);
        g_object_unref(folder);
    }
    gtk_file_dialog_select_folder(
        dialog, GTK_WINDOW(impl_->window), nullptr,
        [](GObject* source, GAsyncResult* result, gpointer data) {
            std::unique_ptr<DialogCall> call(static_cast<DialogCall*>(data));
            call->done(path_of(gtk_file_dialog_select_folder_finish(GTK_FILE_DIALOG(source), result, nullptr)));
        },
        std::make_unique<DialogCall>(DialogCall{std::move(done)}).release());
    g_object_unref(dialog);
}

void Host::save_file(const FileFilter& filter, std::string_view default_path, PathCallback done) {
    GtkFileDialog* dialog = gtk_file_dialog_new();
    GListModel* list = make_filters(std::span<const FileFilter>(&filter, 1));
    gtk_file_dialog_set_filters(dialog, list);
    g_object_unref(list);
    if (!default_path.empty()) {
        GFile* file = g_file_new_for_path(std::string(default_path).c_str());
        if (GFile* parent = g_file_get_parent(file)) {
            gtk_file_dialog_set_initial_folder(dialog, parent);
            g_object_unref(parent);
        }
        char* name = g_file_get_basename(file);
        if (name != nullptr) gtk_file_dialog_set_initial_name(dialog, name);
        g_free(name);
        g_object_unref(file);
    }
    gtk_file_dialog_save(
        dialog, GTK_WINDOW(impl_->window), nullptr,
        [](GObject* source, GAsyncResult* result, gpointer data) {
            std::unique_ptr<DialogCall> call(static_cast<DialogCall*>(data));
            call->done(path_of(gtk_file_dialog_save_finish(GTK_FILE_DIALOG(source), result, nullptr)));
        },
        std::make_unique<DialogCall>(DialogCall{std::move(done)}).release());
    g_object_unref(dialog);
}

void Host::reveal(std::string_view utf8_path) {
    GFile* file = g_file_new_for_path(std::string(utf8_path).c_str());
    GtkFileLauncher* launcher = gtk_file_launcher_new(file);
    gtk_file_launcher_open_containing_folder(launcher, GTK_WINDOW(impl_->window), nullptr, nullptr, nullptr);
    g_object_unref(launcher);
    g_object_unref(file);
}

void Host::open_url(std::string_view https_url) {
    if (!https_url.starts_with("https://")) return;
    GtkUriLauncher* launcher = gtk_uri_launcher_new(std::string(https_url).c_str());
    gtk_uri_launcher_launch(launcher, GTK_WINDOW(impl_->window), nullptr, nullptr, nullptr);
    g_object_unref(launcher);
}

FlyInput Host::fly_input() const noexcept { return FlyInput::Page; }
void Host::set_fly_capture(bool) {}
HostFlyKeys Host::fly_keys() const { return {}; }

} // namespace bridger::app
