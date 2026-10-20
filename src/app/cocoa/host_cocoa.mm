// macOS host (docs/adr/0010): an NSWindow whose content view holds a layer-backed scene view (the renderer's
// offscreen pixels as a CGImage) and, on top of it, a WKWebView that does not draw a background. Core Animation
// blends the two. The interface is served from memory under bridger://app/. Objective-C++ with ARC.

#include "../host.hpp"
#include "../webkit_bridge.hpp"

#include <bridger/core/log.hpp>

#import <AppKit/AppKit.h>
#import <QuartzCore/QuartzCore.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>
#import <WebKit/WebKit.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <string>

namespace bridger::app {
struct HostCocoaState; // the C++ side the Objective-C delegates call into
}

namespace {

NSString* ns(std::string_view s) {
    NSString* str = [[NSString alloc] initWithBytes:s.data() length:s.size() encoding:NSUTF8StringEncoding];
    return str != nil ? str : @"";
}

std::string utf8(NSString* s) { return s != nil ? std::string([s UTF8String]) : std::string(); }

std::optional<std::string> path_of(NSURL* url) {
    if (url == nil || !url.isFileURL) return std::nullopt;
    return utf8(url.path);
}

// Content types for a filter list; nil when any filter allows every file.
NSArray<UTType*>* content_types(std::span<const bridger::app::FileFilter> filters) {
    NSMutableArray<UTType*>* types = [NSMutableArray array];
    for (const auto& f : filters) {
        std::string_view rest = f.patterns;
        while (!rest.empty()) {
            const size_t semi = rest.find(';');
            const std::string_view one = rest.substr(0, semi);
            if (one == "*") return nil;
            if (UTType* t = [UTType typeWithFilenameExtension:ns(one)]) [types addObject:t];
            rest = semi == std::string_view::npos ? std::string_view() : rest.substr(semi + 1);
        }
    }
    return types.count > 0 ? types : nil;
}

} // namespace

namespace bridger::app {

struct HostCocoaState {
    HostEvents events;
    std::function<std::optional<Host::WebFile>(std::string_view)> resource;
    bool web_reported = false;
};

} // namespace bridger::app

using bridger::app::HostCocoaState;

// ---- Objective-C helpers ------------------------------------------------------------------------------------

// Closing the window and Quit (Cmd+Q) both ask the app, which ends its loop and cleans up (workers, settings).
@interface BridgerWindowDelegate : NSObject <NSWindowDelegate, NSApplicationDelegate>
@property(nonatomic, assign) HostCocoaState* state;
@end

@implementation BridgerWindowDelegate
- (BOOL)windowShouldClose:(NSWindow*)sender {
    (void)sender;
    self.state->events.close_requested();
    return NO;
}
- (NSApplicationTerminateReply)applicationShouldTerminate:(NSApplication*)sender {
    (void)sender;
    self.state->events.close_requested();
    return NSTerminateCancel;
}
@end

// The web view also takes file drops for the app (before WebKit would open the file itself).
@interface BridgerWebView : WKWebView
@property(nonatomic, assign) HostCocoaState* state;
@end

@implementation BridgerWebView
- (NSDragOperation)draggingEntered:(id<NSDraggingInfo>)sender {
    if (![sender.draggingPasteboard canReadObjectForClasses:@[ NSURL.class ] options:@{NSPasteboardURLReadingFileURLsOnlyKey : @YES}])
        return NSDragOperationNone;
    self.state->events.drop_hover(true);
    return NSDragOperationCopy;
}
- (NSDragOperation)draggingUpdated:(id<NSDraggingInfo>)sender {
    (void)sender;
    return NSDragOperationCopy;
}
- (void)draggingExited:(id<NSDraggingInfo>)sender {
    (void)sender;
    self.state->events.drop_hover(false);
}
- (BOOL)prepareForDragOperation:(id<NSDraggingInfo>)sender {
    (void)sender;
    return YES;
}
- (BOOL)performDragOperation:(id<NSDraggingInfo>)sender {
    self.state->events.drop_hover(false);
    NSArray<NSURL*>* urls = [sender.draggingPasteboard readObjectsForClasses:@[ NSURL.class ]
                                                                     options:@{NSPasteboardURLReadingFileURLsOnlyKey : @YES}];
    for (NSURL* url in urls)
        if (auto p = path_of(url)) self.state->events.drop_file(*p);
    return YES;
}
@end

@interface BridgerWebDelegate : NSObject <WKScriptMessageHandler, WKURLSchemeHandler, WKNavigationDelegate>
@property(nonatomic, assign) HostCocoaState* state;
@end

@implementation BridgerWebDelegate
- (void)userContentController:(WKUserContentController*)controller didReceiveScriptMessage:(WKScriptMessage*)message {
    (void)controller;
    if ([message.body isKindOfClass:NSString.class]) self.state->events.message(utf8(static_cast<NSString*>(message.body)));
}

- (void)webView:(WKWebView*)webView startURLSchemeTask:(id<WKURLSchemeTask>)task {
    (void)webView;
    std::string path = utf8(task.request.URL.path);
    while (!path.empty() && path.front() == '/') path.erase(0, 1);
    const auto file = self.state->resource ? self.state->resource(path) : std::nullopt;
    if (!file) {
        [task didFailWithError:[NSError errorWithDomain:NSURLErrorDomain code:NSURLErrorFileDoesNotExist userInfo:nil]];
        return;
    }
    NSURLResponse* response = [[NSURLResponse alloc] initWithURL:task.request.URL
                                                        MIMEType:ns(file->mime)
                                           expectedContentLength:static_cast<NSInteger>(file->bytes.size())
                                                textEncodingName:nil];
    [task didReceiveResponse:response];
    // The bytes belong to the embedded bundle and live as long as the program.
    [task didReceiveData:[NSData dataWithBytesNoCopy:const_cast<bridger::u8*>(file->bytes.data())
                                              length:file->bytes.size()
                                        freeWhenDone:NO]];
    [task didFinish];
}

- (void)webView:(WKWebView*)webView stopURLSchemeTask:(id<WKURLSchemeTask>)task {
    (void)webView;
    (void)task;
}

// Links never navigate the interface away: https links open in the browser, anything else is refused.
- (void)webView:(WKWebView*)webView
    decidePolicyForNavigationAction:(WKNavigationAction*)action
                    decisionHandler:(void (^)(WKNavigationActionPolicy))decision {
    (void)webView;
    NSURL* url = action.request.URL;
    if ([url.scheme isEqualToString:@"bridger"] && action.targetFrame != nil) return decision(WKNavigationActionPolicyAllow);
    if ([url.scheme isEqualToString:@"https"]) [[NSWorkspace sharedWorkspace] openURL:url];
    decision(WKNavigationActionPolicyCancel);
}

- (void)webView:(WKWebView*)webView didFinishNavigation:(WKNavigation*)navigation {
    (void)webView;
    (void)navigation;
    if (!self.state->web_reported) {
        self.state->web_reported = true;
        self.state->events.web_ready(true, {});
    }
}

- (void)reportFailure:(NSError*)error {
    if (self.state->web_reported) return;
    self.state->web_reported = true;
    self.state->events.web_ready(false, utf8(error.localizedDescription));
}

- (void)webView:(WKWebView*)webView didFailNavigation:(WKNavigation*)navigation withError:(NSError*)error {
    (void)webView;
    (void)navigation;
    [self reportFailure:error];
}

- (void)webView:(WKWebView*)webView didFailProvisionalNavigation:(WKNavigation*)navigation withError:(NSError*)error {
    (void)webView;
    (void)navigation;
    [self reportFailure:error];
}
@end

namespace bridger::app {
namespace {

constexpr CGFloat kCharcoal[3] = {0x0F / 255.0, 0x12 / 255.0, 0x17 / 255.0}; // --bg in the interface's tokens.css

// The standard application and Edit menus: Quit, and the shortcuts text fields need (copy, paste, select all, undo).
void install_menus() {
    NSMenu* bar = [[NSMenu alloc] init];
    NSMenuItem* app_item = [[NSMenuItem alloc] init];
    [bar addItem:app_item];
    NSMenu* app_menu = [[NSMenu alloc] init];
    [app_menu addItemWithTitle:@"Hide Bridger" action:@selector(hide:) keyEquivalent:@"h"];
    [app_menu addItem:[NSMenuItem separatorItem]];
    [app_menu addItemWithTitle:@"Quit Bridger" action:@selector(terminate:) keyEquivalent:@"q"];
    app_item.submenu = app_menu;

    NSMenuItem* edit_item = [[NSMenuItem alloc] init];
    [bar addItem:edit_item];
    NSMenu* edit = [[NSMenu alloc] initWithTitle:@"Edit"];
    [edit addItemWithTitle:@"Undo" action:@selector(undo:) keyEquivalent:@"z"];
    [edit addItemWithTitle:@"Redo" action:@selector(redo:) keyEquivalent:@"Z"];
    [edit addItem:[NSMenuItem separatorItem]];
    [edit addItemWithTitle:@"Cut" action:@selector(cut:) keyEquivalent:@"x"];
    [edit addItemWithTitle:@"Copy" action:@selector(copy:) keyEquivalent:@"c"];
    [edit addItemWithTitle:@"Paste" action:@selector(paste:) keyEquivalent:@"v"];
    [edit addItemWithTitle:@"Select All" action:@selector(selectAll:) keyEquivalent:@"a"];
    edit_item.submenu = edit;
    NSApp.mainMenu = bar;
}

} // namespace

struct Host::Impl {
    HostCocoaState state;
    NSWindow* window = nil;
    NSView* scene = nil;
    BridgerWebView* web = nil;
    BridgerWindowDelegate* window_delegate = nil;
    BridgerWebDelegate* web_delegate = nil;
    u32 width = 1, height = 1;
    bool quit = false;

    ~Impl() {
        [window orderOut:nil];
        window.delegate = nil;
        NSApp.delegate = nil;
    }

    void check_size() {
        const NSRect bounds = window.contentView.bounds;
        const CGFloat scale = window.backingScaleFactor;
        const auto w = static_cast<u32>(std::max(1.0, std::round(bounds.size.width * scale)));
        const auto h = static_cast<u32>(std::max(1.0, std::round(bounds.size.height * scale)));
        if (w == width && h == height) return;
        width = w;
        height = h;
        scene.layer.contentsScale = scale;
        state.events.resized(w, h);
    }
};

Host::Host() : impl_(std::make_unique<Impl>()) {}
Host::~Host() = default;

std::unique_ptr<Host> Host::create(HostEvents events) {
    std::unique_ptr<Host> host(new Host());
    Impl& d = *host->impl_;
    d.state.events = std::move(events);
    @autoreleasepool {
        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
        install_menus();
        [NSApp finishLaunching];

        const NSRect frame = NSMakeRect(0, 0, 1440, 900);
        d.window = [[NSWindow alloc] initWithContentRect:frame
                                               styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                                                         NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable
                                                 backing:NSBackingStoreBuffered
                                                   defer:NO];
        d.window.title = @"Bridger";
        d.window.minSize = NSMakeSize(960, 600);
        d.window.appearance = [NSAppearance appearanceNamed:NSAppearanceNameDarkAqua];
        d.window.backgroundColor = [NSColor colorWithSRGBRed:kCharcoal[0] green:kCharcoal[1] blue:kCharcoal[2] alpha:1.0];
        d.window.releasedWhenClosed = NO;
        [d.window center];
        d.window_delegate = [[BridgerWindowDelegate alloc] init];
        d.window_delegate.state = &d.state;
        d.window.delegate = d.window_delegate;
        NSApp.delegate = d.window_delegate;

        NSView* content = [[NSView alloc] initWithFrame:frame];
        content.wantsLayer = YES;
        d.scene = [[NSView alloc] initWithFrame:frame];
        d.scene.wantsLayer = YES;
        d.scene.layer.contentsGravity = kCAGravityResize;
        d.scene.layer.backgroundColor = d.window.backgroundColor.CGColor;
        d.scene.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
        [content addSubview:d.scene];
        d.window.contentView = content;
        const CGFloat scale = d.window.backingScaleFactor;
        d.width = static_cast<u32>(frame.size.width * scale);
        d.height = static_cast<u32>(frame.size.height * scale);
    }
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
    @autoreleasepool {
        CFDataRef data = CFDataCreate(nullptr, rgba.data(), static_cast<CFIndex>(static_cast<usize>(width) * height * 4));
        CGDataProviderRef provider = CGDataProviderCreateWithCFData(data);
        CGColorSpaceRef space = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
        // The alpha byte is not meant as transparency: the scene is opaque.
        CGImageRef image = CGImageCreate(width, height, 8, 32, static_cast<size_t>(width) * 4, space,
                                         static_cast<CGBitmapInfo>(kCGBitmapByteOrder32Big) | static_cast<CGBitmapInfo>(kCGImageAlphaNoneSkipLast),
                                         provider, nullptr, false, kCGRenderingIntentDefault);
        [CATransaction begin];
        [CATransaction setDisableActions:YES]; // no cross-fade between frames
        impl_->scene.layer.contents = (__bridge id)image;
        [CATransaction commit];
        CGImageRelease(image);
        CGColorSpaceRelease(space);
        CGDataProviderRelease(provider);
        CFRelease(data);
    }
}

void Host::start_web(std::function<std::optional<WebFile>(std::string_view path)> resource, bool dev_tools) {
    Impl& d = *impl_;
    d.state.resource = std::move(resource);
    @autoreleasepool {
        d.web_delegate = [[BridgerWebDelegate alloc] init];
        d.web_delegate.state = &d.state;
        WKWebViewConfiguration* config = [[WKWebViewConfiguration alloc] init];
        [config setURLSchemeHandler:d.web_delegate forURLScheme:@"bridger"];
        WKUserScript* script = [[WKUserScript alloc] initWithSource:ns(kWebKitBridgeScript)
                                                      injectionTime:WKUserScriptInjectionTimeAtDocumentStart
                                                   forMainFrameOnly:YES];
        [config.userContentController addUserScript:script];
        [config.userContentController addScriptMessageHandler:d.web_delegate name:@"bridger"];

        d.web = [[BridgerWebView alloc] initWithFrame:d.window.contentView.bounds configuration:config];
        d.web.state = &d.state;
        d.web.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
        d.web.navigationDelegate = d.web_delegate;
        [d.web setValue:@NO forKey:@"drawsBackground"]; // transparent: the scene shows through
        if (@available(macOS 13.3, *)) d.web.inspectable = dev_tools ? YES : NO;
        [d.web registerForDraggedTypes:@[ NSPasteboardTypeFileURL ]];
        [d.window.contentView addSubview:d.web positioned:NSWindowAbove relativeTo:d.scene];
        [d.web loadRequest:[NSURLRequest requestWithURL:[NSURL URLWithString:ns(kWebKitStartUrl)]]];
    }
}

void Host::post_json(std::string_view json) {
    if (impl_->web == nil) return;
    @autoreleasepool {
        [impl_->web evaluateJavaScript:ns("window.__bridgerReceive(" + std::string(json) + ")") completionHandler:nil];
    }
}

void Host::focus_web() {
    if (impl_->web != nil) [impl_->window makeFirstResponder:impl_->web];
}

void Host::show() {
    [impl_->window makeKeyAndOrderFront:nil];
    [NSApp activateIgnoringOtherApps:YES];
    focus_web();
}

void Host::set_title(std::string_view title) { impl_->window.title = ns(title); }

void Host::error_box(std::string_view message) {
    @autoreleasepool {
        NSAlert* alert = [[NSAlert alloc] init];
        alert.alertStyle = NSAlertStyleCritical;
        alert.messageText = @"Bridger";
        alert.informativeText = ns(message);
        [alert runModal];
    }
}

bool Host::save_screenshot(std::string_view) { return false; }

void Host::run(const std::function<bool()>& tick) {
    using clock = std::chrono::steady_clock;
    bool busy = true;
    auto last = clock::now();
    while (!impl_->quit) {
        @autoreleasepool {
            // Busy: about one pass per display frame (the scene is drawn offscreen). Idle: wait for input, at most ~15 ms.
            const double since = std::chrono::duration<double>(clock::now() - last).count();
            const double wait = busy ? std::max(0.0, 1.0 / 60.0 - since) : 0.015;
            NSDate* until = [NSDate dateWithTimeIntervalSinceNow:wait];
            for (NSEvent* e = [NSApp nextEventMatchingMask:NSEventMaskAny untilDate:until inMode:NSDefaultRunLoopMode dequeue:YES]; e != nil;
                 e = [NSApp nextEventMatchingMask:NSEventMaskAny untilDate:[NSDate distantPast] inMode:NSDefaultRunLoopMode dequeue:YES])
                [NSApp sendEvent:e];
            [NSApp updateWindows];
            if (impl_->quit) break;
            impl_->check_size();
            last = clock::now();
            busy = tick();
        }
    }
}

void Host::quit() { impl_->quit = true; }

void Host::open_file(std::span<const FileFilter> filters, PathCallback done) {
    NSOpenPanel* panel = [NSOpenPanel openPanel];
    panel.canChooseFiles = YES;
    panel.canChooseDirectories = NO;
    panel.allowsMultipleSelection = NO;
    if (NSArray<UTType*>* types = content_types(filters)) panel.allowedContentTypes = types;
    [panel beginSheetModalForWindow:impl_->window
                  completionHandler:^(NSModalResponse r) {
                    done(r == NSModalResponseOK ? path_of(panel.URL) : std::nullopt);
                  }];
}

void Host::open_folder(std::string_view default_dir, PathCallback done) {
    NSOpenPanel* panel = [NSOpenPanel openPanel];
    panel.canChooseFiles = NO;
    panel.canChooseDirectories = YES;
    panel.canCreateDirectories = YES;
    panel.allowsMultipleSelection = NO;
    if (!default_dir.empty()) panel.directoryURL = [NSURL fileURLWithPath:ns(default_dir) isDirectory:YES];
    [panel beginSheetModalForWindow:impl_->window
                  completionHandler:^(NSModalResponse r) {
                    done(r == NSModalResponseOK ? path_of(panel.URL) : std::nullopt);
                  }];
}

void Host::save_file(const FileFilter& filter, std::string_view default_path, PathCallback done) {
    NSSavePanel* panel = [NSSavePanel savePanel];
    panel.canCreateDirectories = YES;
    if (NSArray<UTType*>* types = content_types(std::span<const FileFilter>(&filter, 1))) panel.allowedContentTypes = types;
    if (!default_path.empty()) {
        NSURL* url = [NSURL fileURLWithPath:ns(default_path)];
        panel.directoryURL = url.URLByDeletingLastPathComponent;
        panel.nameFieldStringValue = url.lastPathComponent;
    }
    [panel beginSheetModalForWindow:impl_->window
                  completionHandler:^(NSModalResponse r) {
                    done(r == NSModalResponseOK ? path_of(panel.URL) : std::nullopt);
                  }];
}

void Host::reveal(std::string_view utf8_path) {
    [[NSWorkspace sharedWorkspace] activateFileViewerSelectingURLs:@[ [NSURL fileURLWithPath:ns(utf8_path)] ]];
}

void Host::open_url(std::string_view https_url) {
    if (!https_url.starts_with("https://")) return;
    if (NSURL* url = [NSURL URLWithString:ns(https_url)]) [[NSWorkspace sharedWorkspace] openURL:url];
}

FlyInput Host::fly_input() const noexcept { return FlyInput::Page; }
void Host::set_fly_capture(bool) {}
HostFlyKeys Host::fly_keys() const { return {}; }

} // namespace bridger::app
