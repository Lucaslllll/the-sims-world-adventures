// UIKit / QuartzCore HLE: application, windows/views/view controllers, screen,
// device, touches, colours, fonts, images, layers, text fields.
#include "hle/uikit.h"

#include <cmath>
#include <cstring>
#include <unordered_map>

#include "hle/cg.h"
#include "hle/config.h"
#include "hle/foundation.h"
#include "hle/nsstring.h"
#include "objc/runtime.h"
#include "platform/platform.h"

namespace uikit {

using hle::config;

static id g_UIImagePickerControllerOriginalImage = nil;

struct CGAffineTransform {
    CGFloat a, b, c, d, tx, ty;
};

namespace {

Class g_UIResponder, g_UIView, g_UIWindow, g_UIViewController, g_UIApplication, g_UIScreen, g_UIDevice,
    g_UITouch, g_UIEvent, g_UIColor, g_UIFont, g_UIImage, g_UIImageView, g_CALayer, g_CAEAGLLayer,
    g_UITextField, g_UIWebView, g_UIAccelerometer, g_UIAcceleration;

// ---------------------------------------------------------------------------
// Layers
// ---------------------------------------------------------------------------
struct LayerData {
    CGRect bounds{};
    bool opaque = false;
    double contents_scale = 1.0;
    id drawable_properties = nil;
    id delegate = nil;  // owning view
};
struct LayerObj {
    Class isa;
    LayerData* d;
};
LayerData* L(id o) { return ((LayerObj*)o)->d; }

id Layer_init(id self, SEL) {
    ((LayerObj*)self)->d = new LayerData();
    return self;
}
void Layer_dealloc(id self, SEL) {
    objc::release(L(self)->drawable_properties);
    delete L(self);
    objc::dispose_instance(self);
}
CGRect Layer_bounds(id self, SEL) { return L(self)->bounds; }
void Layer_setBounds(id self, SEL, CGRect r) { L(self)->bounds = r; }
CGRect Layer_frame(id self, SEL) { return L(self)->bounds; }
void Layer_setFrame(id self, SEL, CGRect r) { L(self)->bounds = {{0, 0}, r.size}; }
void Layer_setOpaque(id self, SEL, BOOL o) { L(self)->opaque = o; }
BOOL Layer_isOpaque(id self, SEL) { return L(self)->opaque; }
CGFloat Layer_contentsScale(id self, SEL) { return L(self)->contents_scale; }
void Layer_setContentsScale(id self, SEL, CGFloat s) { L(self)->contents_scale = s; }
void Layer_setDrawableProperties(id self, SEL, id p) {
    objc::retain(p);
    objc::release(L(self)->drawable_properties);
    L(self)->drawable_properties = p;
}
id Layer_drawableProperties(id self, SEL) { return L(self)->drawable_properties; }
void Layer_noop1(id, SEL, id) {}

// ---------------------------------------------------------------------------
// Views
// ---------------------------------------------------------------------------
struct ViewData {
    CGRect frame{};
    CGRect bounds{};
    id layer = nil;
    id superview = nil;  // weak
    std::vector<id> subviews;
    bool hidden = false, multi_touch = false, exclusive_touch = false, user_interaction = true;
    bool needs_layout = true;
    double alpha = 1.0;
    long content_mode = 0, tag = 0;
    double scale = 1.0;
    CGAffineTransform transform{1, 0, 0, 1, 0, 0};
    id background_color = nil;
    id view_controller = nil;  // weak
    // UIImageView
    id image = nil;
    // UITextField
    std::u16string text;
    id delegate = nil;  // weak
    struct TargetAction {
        id target;
        SEL action;
        unsigned long events;
    };
    std::vector<TargetAction> targets;
    std::u16string placeholder;
    bool secure = false;
};
struct ViewObj {
    Class isa;
    ViewData* d;
};
ViewData* V(id o) { return ((ViewObj*)o)->d; }

std::vector<id> g_all_views;  // for layout
id g_key_window = nil;
id g_first_responder = nil;

id View_initWithFrame(id self, SEL, CGRect frame) {
    auto* d = new ViewData();
    ((ViewObj*)self)->d = d;
    d->frame = frame;
    d->bounds = {{0, 0}, frame.size};
    Class layer_cls = (Class)objc::send((id)self->isa, SEL_("layerClass"));
    if (!layer_cls) layer_cls = g_CALayer;
    d->layer = objc::send(objc::send((id)layer_cls, SEL_("alloc")), SEL_("init"));
    L(d->layer)->bounds = d->bounds;
    L(d->layer)->delegate = self;
    g_all_views.push_back(self);
    return self;
}
id View_init(id self, SEL) {
    return objc::send<id>(self, SEL_("initWithFrame:"), CGRect{{0, 0}, {0, 0}});
}
id View_initWithCoder(id self, SEL s, id) { return View_init(self, s); }
id View_cls_layerClass(Class, SEL) { return (id)g_CALayer; }
void View_dealloc(id self, SEL) {
    auto* d = V(self);
    if (d) {
        for (id v : d->subviews) {
            V(v)->superview = nil;
            objc::release(v);
        }
        objc::release(d->layer);
        objc::release(d->background_color);
        objc::release(d->image);
        for (size_t i = 0; i < g_all_views.size(); i++)
            if (g_all_views[i] == self) {
                g_all_views.erase(g_all_views.begin() + i);
                break;
            }
        if (g_first_responder == self) g_first_responder = nil;
        delete d;
    }
    objc::dispose_instance(self);
}
CGRect View_frame(id self, SEL) { return V(self)->frame; }
void View_setFrame(id self, SEL, CGRect f) {
    auto* d = V(self);
    d->frame = f;
    d->bounds.size = f.size;
    L(d->layer)->bounds = d->bounds;
    d->needs_layout = true;
}
CGRect View_bounds(id self, SEL) { return V(self)->bounds; }
void View_setBounds(id self, SEL, CGRect b) {
    auto* d = V(self);
    d->bounds = b;
    d->frame.size = b.size;
    L(d->layer)->bounds = b;
    d->needs_layout = true;
}
CGPoint View_center(id self, SEL) {
    auto& f = V(self)->frame;
    return {f.origin.x + f.size.width / 2, f.origin.y + f.size.height / 2};
}
void View_setCenter(id self, SEL, CGPoint c) {
    auto& f = V(self)->frame;
    f.origin = {c.x - f.size.width / 2, c.y - f.size.height / 2};
}
id View_layer(id self, SEL) { return V(self)->layer; }
id View_superview(id self, SEL) { return V(self)->superview; }
id View_subviews(id self, SEL) { return objc::autorelease(foundation::make_array(V(self)->subviews)); }
id View_window(id self, SEL) {
    id v = self;
    while (v && !objc::is_kind_of(v, g_UIWindow)) v = V(v)->superview;
    return v;
}
void remove_from_superview(id v) {
    id sup = V(v)->superview;
    if (!sup) return;
    auto& subs = V(sup)->subviews;
    for (size_t i = 0; i < subs.size(); i++)
        if (subs[i] == v) {
            subs.erase(subs.begin() + i);
            break;
        }
    V(v)->superview = nil;
    objc::release(v);
}
Class g_UIImagePickerController = nullptr;
id g_active_picker = nil;
void maybe_start_picker(id view);

void View_addSubview(id self, SEL, id v) {
    if (!v || v == self) return;
    maybe_start_picker(v);
    objc::retain(v);
    remove_from_superview(v);
    V(self)->subviews.push_back(v);
    V(v)->superview = self;
    V(v)->needs_layout = true;
    V(self)->needs_layout = true;
}
void View_removeFromSuperview(id self, SEL) { remove_from_superview(self); }
void View_bringSubviewToFront(id self, SEL, id v) {
    auto& s = V(self)->subviews;
    for (size_t i = 0; i < s.size(); i++)
        if (s[i] == v) {
            s.erase(s.begin() + i);
            s.push_back(v);
            return;
        }
}
void View_sendSubviewToBack(id self, SEL, id v) {
    auto& s = V(self)->subviews;
    for (size_t i = 0; i < s.size(); i++)
        if (s[i] == v) {
            s.erase(s.begin() + i);
            s.insert(s.begin(), v);
            return;
        }
}
void View_setHidden(id self, SEL, BOOL h) { V(self)->hidden = h; }
BOOL View_isHidden(id self, SEL) { return V(self)->hidden; }
void View_setMultipleTouchEnabled(id self, SEL, BOOL b) { V(self)->multi_touch = b; }
BOOL View_isMultipleTouchEnabled(id self, SEL) { return V(self)->multi_touch; }
void View_setExclusiveTouch(id self, SEL, BOOL b) { V(self)->exclusive_touch = b; }
void View_setUserInteractionEnabled(id self, SEL, BOOL b) { V(self)->user_interaction = b; }
BOOL View_isUserInteractionEnabled(id self, SEL) { return V(self)->user_interaction; }
void View_setAlpha(id self, SEL, CGFloat a) { V(self)->alpha = a; }
CGFloat View_alpha(id self, SEL) { return V(self)->alpha; }
void View_setContentMode(id self, SEL, long m) { V(self)->content_mode = m; }
void View_setTag(id self, SEL, long t) { V(self)->tag = t; }
long View_tag(id self, SEL) { return V(self)->tag; }
void View_setTransform(id self, SEL, CGAffineTransform t) { V(self)->transform = t; }
CGAffineTransform View_transform(id self, SEL) { return V(self)->transform; }
void View_setBackgroundColor(id self, SEL, id c) {
    objc::retain(c);
    objc::release(V(self)->background_color);
    V(self)->background_color = c;
}
id View_backgroundColor(id self, SEL) { return V(self)->background_color; }
void View_setOpaque(id self, SEL, BOOL o) { L(V(self)->layer)->opaque = o; }
CGFloat View_contentScaleFactor(id self, SEL) { return L(V(self)->layer)->contents_scale; }
void View_setContentScaleFactor(id self, SEL, CGFloat s) { L(V(self)->layer)->contents_scale = s; }
void View_setNeedsLayout(id self, SEL) { V(self)->needs_layout = true; }
void View_layoutSubviews(id, SEL) {}
void View_noop(id, SEL) {}
void View_noop1(id, SEL, id) {}
void View_noopB(id, SEL, BOOL) {}
void View_noopL(id, SEL, long) {}
BOOL View_endEditing(id self, SEL, BOOL) {
    if (g_first_responder) objc::send<BOOL>(g_first_responder, SEL_("resignFirstResponder"));
    return YES;
}
CGPoint view_origin_in_window(id v) {
    CGPoint p{0, 0};
    for (id x = v; x && !objc::is_kind_of(x, g_UIWindow); x = V(x)->superview) {
        p.x += V(x)->frame.origin.x - V(x)->bounds.origin.x;
        p.y += V(x)->frame.origin.y - V(x)->bounds.origin.y;
    }
    return p;
}
CGPoint View_convertPoint_toView(id self, SEL, CGPoint p, id other) {
    CGPoint a = view_origin_in_window(self), b = other ? view_origin_in_window(other) : CGPoint{0, 0};
    return {p.x + a.x - b.x, p.y + a.y - b.y};
}
CGPoint View_convertPoint_fromView(id self, SEL, CGPoint p, id other) {
    CGPoint a = view_origin_in_window(self), b = other ? view_origin_in_window(other) : CGPoint{0, 0};
    return {p.x + b.x - a.x, p.y + b.y - a.y};
}
BOOL View_pointInside(id self, SEL, CGPoint p, id) {
    auto& b = V(self)->bounds;
    return p.x >= b.origin.x && p.y >= b.origin.y && p.x < b.origin.x + b.size.width &&
           p.y < b.origin.y + b.size.height;
}

// UIResponder
BOOL Resp_becomeFirstResponder(id self, SEL) {
    g_first_responder = self;
    return YES;
}
BOOL Resp_resignFirstResponder(id self, SEL) {
    if (g_first_responder == self) g_first_responder = nil;
    return YES;
}
BOOL Resp_isFirstResponder(id self, SEL) { return g_first_responder == self; }
BOOL Resp_canBecomeFirstResponder(id, SEL) { return YES; }
void Resp_touches(id, SEL, id, id) {}
id Resp_nextResponder(id self, SEL) {
    if (objc::is_kind_of(self, g_UIView)) return V(self)->view_controller ? V(self)->view_controller : V(self)->superview;
    return nil;
}

// ---------------------------------------------------------------------------
// UIWindow
// ---------------------------------------------------------------------------
id g_windows_root_vc = nil;
std::unordered_map<id, id> g_root_vc;  // window -> root view controller

id VC_view(id self, SEL);

void install_root_view(id window) {
    auto it = g_root_vc.find(window);
    if (it == g_root_vc.end() || !it->second) return;
    id view = VC_view(it->second, nullptr);
    if (view && V(view)->superview != window) View_addSubview(window, nullptr, view);
}
void Window_makeKeyAndVisible(id self, SEL) {
    g_key_window = self;
    V(self)->hidden = false;
    install_root_view(self);
    LOGI("window %p is key and visible (%zu subviews)", self, V(self)->subviews.size());
}
void Window_makeKeyWindow(id self, SEL) { g_key_window = self; }
id Window_rootViewController(id self, SEL) {
    auto it = g_root_vc.find(self);
    return it == g_root_vc.end() ? nil : it->second;
}
void Window_setRootViewController(id self, SEL, id vc) {
    objc::retain(vc);
    objc::release(g_root_vc[self]);
    g_root_vc[self] = vc;
    install_root_view(self);
}

// ---------------------------------------------------------------------------
// UIViewController (instance size 320 on iOS 8)
// ---------------------------------------------------------------------------
struct VCData {
    id delegate = nil;  // weak (UINavigationController / UIImagePickerController)
    id view = nil;
    id presented = nil;
    id presenting = nil;  // weak
    long modal_style = 0;
};
struct VCObj {
    Class isa;
    VCData* d;
};
VCData* VC(id o) { return ((VCObj*)o)->d; }

id VC_init(id self, SEL) {
    ((VCObj*)self)->d = new VCData();
    return self;
}
id VC_initWithNib(id self, SEL s, id, id) { return VC_init(self, s); }
void VC_dealloc(id self, SEL) {
    if (auto* d = VC(self)) {
        if (d->view) V(d->view)->view_controller = nil;
        objc::release(d->view);
        objc::release(d->presented);
        delete d;
    }
    objc::dispose_instance(self);
}
void VC_setView(id self, SEL, id v) {
    if (!VC(self)) VC_init(self, nullptr);
    objc::retain(v);
    if (VC(self)->view) V(VC(self)->view)->view_controller = nil;
    objc::release(VC(self)->view);
    VC(self)->view = v;
    if (v) V(v)->view_controller = self;
}
id VC_view(id self, SEL) {
    if (!VC(self)) VC_init(self, nullptr);
    if (!VC(self)->view) {
        if (objc::responds_to(self->isa, SEL_("loadView")) &&
            objc::lookup_method(self->isa, SEL_("loadView")) != objc::lookup_method(g_UIViewController, SEL_("loadView"))) {
            objc::send<void>(self, SEL_("loadView"));
        }
        if (!VC(self)->view) {
            CGRect r{{0, 0}, {(double)config().screen_w, (double)config().screen_h}};
            id v = objc::send<id>(objc::send((id)g_UIView, SEL_("alloc")), SEL_("initWithFrame:"), r);
            VC_setView(self, nullptr, v);
            objc::release(v);
        }
        objc::send<void>(self, SEL_("viewDidLoad"));
    }
    return VC(self)->view;
}
BOOL VC_isViewLoaded(id self, SEL) { return VC(self) && VC(self)->view; }
void VC_present(id self, SEL, id vc, BOOL animated, void* completion) {
    LOGI("presentViewController %s", objc::class_name(vc->isa));
    if (!VC(vc)) VC_init(vc, nullptr);
    objc::retain(vc);
    objc::release(VC(self)->presented);
    VC(self)->presented = vc;
    VC(vc)->presenting = self;
    id view = VC_view(vc, nullptr);
    objc::send<void>(vc, SEL_("viewWillAppear:"), (BOOL)animated);
    if (g_key_window && view) View_addSubview(g_key_window, nullptr, view);
    objc::send<void>(vc, SEL_("viewDidAppear:"), (BOOL)animated);
    objc::call_block(completion);
}
void VC_presentModal(id self, SEL s, id vc, BOOL animated) { VC_present(self, s, vc, animated, nullptr); }
void VC_dismiss(id self, SEL, BOOL animated, void* completion) {
    id target = VC(self)->presented ? self : VC(self)->presenting;
    id vc = target ? VC(target)->presented : nil;
    if (vc) {
        objc::send<void>(vc, SEL_("viewWillDisappear:"), (BOOL)animated);
        if (VC(vc)->view) remove_from_superview(VC(vc)->view);
        objc::send<void>(vc, SEL_("viewDidDisappear:"), (BOOL)animated);
        VC(target)->presented = nil;
        VC(vc)->presenting = nil;
        objc::release(vc);
    }
    objc::call_block(completion);
}
void VC_dismissModal(id self, SEL s, BOOL animated) { VC_dismiss(self, s, animated, nullptr); }
id VC_presentedViewController(id self, SEL) { return VC(self)->presented; }
id VC_presentingViewController(id self, SEL) { return VC(self)->presenting; }
long VC_interfaceOrientation(id, SEL) { return 1; }  // portrait (game rotates itself)
BOOL VC_shouldAutorotate(id, SEL) { return NO; }
void VC_noopB(id, SEL, BOOL) {}
void VC_noop(id, SEL) {}

// ---------------------------------------------------------------------------
// UIApplication
// ---------------------------------------------------------------------------
struct AppData {
    id delegate = nil;
    long status_bar_orientation = 1;
    bool idle_timer_disabled = false;
    long badge = 0;
};
id g_app = nil;
AppData* APP() { return (AppData*)((ViewObj*)g_app)->d; }

id App_sharedApplication(Class, SEL) {
    if (!g_app) {
        g_app = objc::alloc_instance(g_UIApplication);
        ((ViewObj*)g_app)->d = (ViewData*)new AppData();
    }
    return g_app;
}
id App_delegate(id, SEL) { return APP()->delegate; }
void App_setDelegate(id, SEL, id d) { APP()->delegate = d; }
id App_keyWindow(id, SEL) { return g_key_window; }
id App_windows(id, SEL) {
    std::vector<id> v;
    if (g_key_window) v.push_back(g_key_window);
    return objc::autorelease(foundation::make_array(v));
}
long App_statusBarOrientation(id, SEL) { return APP()->status_bar_orientation; }
void App_setStatusBarOrientation(id, SEL, long o) {
    LOGI("setStatusBarOrientation:%ld", o);
    APP()->status_bar_orientation = o;
}
void App_setStatusBarOrientation_animated(id s, SEL c, long o, BOOL) { App_setStatusBarOrientation(s, c, o); }
void App_setIdleTimerDisabled(id, SEL, BOOL b) { APP()->idle_timer_disabled = b; }
void App_setBadge(id, SEL, long n) { APP()->badge = n; }
long App_badge(id, SEL) { return APP()->badge; }
BOOL App_openURL(id, SEL, id url) {
    std::string s = foundation::url_string(url);
    LOGI("openURL: %s", s.c_str());
    platform::open_url(s);
    return YES;
}
BOOL App_canOpenURL(id, SEL, id url) {
    std::string s = foundation::url_string(url);
    return s.rfind("http", 0) == 0;
}
long App_applicationState(id, SEL) { return 0; }
void App_noop(id, SEL) {}
void App_noop1(id, SEL, id) {}
void App_noopB(id, SEL, BOOL) {}
void App_noopL(id, SEL, long) {}
void App_noopBB(id, SEL, BOOL, long) {}

// ---------------------------------------------------------------------------
// UIScreen / UIDevice
// ---------------------------------------------------------------------------
id g_screen = nil, g_device = nil;
id Screen_mainScreen(Class, SEL) {
    if (!g_screen) g_screen = objc::alloc_instance(g_UIScreen);
    return g_screen;
}
CGRect Screen_bounds(id, SEL) { return {{0, 0}, {(double)config().screen_w, (double)config().screen_h}}; }
CGRect Screen_nativeBounds(id, SEL) {
    double s = config().screen_scale;
    return {{0, 0}, {config().screen_w * s, config().screen_h * s}};
}
CGFloat Screen_scale(id, SEL) { return config().screen_scale; }
id Screen_screens(Class c, SEL s) {
    return objc::autorelease(foundation::make_array({Screen_mainScreen(c, s)}));
}

id Device_currentDevice(Class, SEL) {
    if (!g_device) g_device = objc::alloc_instance(g_UIDevice);
    return g_device;
}
id Device_systemVersion(id, SEL) { return ns::str_auto(config().system_version); }
id Device_systemName(id, SEL) { return ns::str_auto(std::string("iPhone OS")); }
id Device_model(id, SEL) { return ns::str_auto(std::string("iPhone")); }
id Device_name(id, SEL) { return ns::str_auto(std::string("Android")); }
long Device_userInterfaceIdiom(id, SEL) { return 0; }
long Device_orientation(id, SEL) { return 3; }  // landscape, home button right
BOOL Device_isMultitaskingSupported(id, SEL) { return YES; }
id Device_identifierForVendor(id, SEL) {
    return objc::autorelease(objc::alloc_instance(objc::get_class("NSUUID")));
}
float Device_batteryLevel(id, SEL) { return 1.0f; }
void Device_noop(id, SEL) {}

// ---------------------------------------------------------------------------
// Touches
// ---------------------------------------------------------------------------
struct TouchData {
    CGPoint loc{}, prev{};  // window coordinates
    long phase = 0;
    long tap_count = 1;
    id view = nil;
    id window = nil;
    double timestamp = 0;
};
struct TouchObj {
    Class isa;
    TouchData* d;
};
TouchData* T(id o) { return ((TouchObj*)o)->d; }

CGPoint Touch_locationInView(id self, SEL, id view) {
    CGPoint p = T(self)->loc;
    if (!view) return p;
    CGPoint o = view_origin_in_window(view);
    return {p.x - o.x, p.y - o.y};
}
CGPoint Touch_previousLocationInView(id self, SEL, id view) {
    CGPoint p = T(self)->prev;
    if (!view) return p;
    CGPoint o = view_origin_in_window(view);
    return {p.x - o.x, p.y - o.y};
}
long Touch_tapCount(id self, SEL) { return T(self)->tap_count; }
long Touch_phase(id self, SEL) { return T(self)->phase; }
id Touch_view(id self, SEL) { return T(self)->view; }
id Touch_window(id self, SEL) { return T(self)->window; }
double Touch_timestamp(id self, SEL) { return T(self)->timestamp; }
void Touch_dealloc(id self, SEL) {
    delete T(self);
    objc::dispose_instance(self);
}

struct EventObj {
    Class isa;
    id all_touches;  // NSSet
};
id Event_allTouches(id self, SEL) { return ((EventObj*)self)->all_touches; }
id Event_touchesForView(id self, SEL, id) { return ((EventObj*)self)->all_touches; }
id Event_touchesForWindow(id self, SEL, id) { return ((EventObj*)self)->all_touches; }
double Event_timestamp(id, SEL) { return foundation::now_seconds(); }
long Event_type(id, SEL) { return 0; }

std::unordered_map<long long, id> g_active_touches;
struct LastTap {
    double time = 0;
    CGPoint pos{};
    long count = 0;
} g_last_tap;

id hit_test(id view, CGPoint p_in_parent) {
    auto* d = V(view);
    if (d->hidden || !d->user_interaction || d->alpha < 0.01) return nil;
    CGPoint p{p_in_parent.x - d->frame.origin.x + d->bounds.origin.x, p_in_parent.y - d->frame.origin.y + d->bounds.origin.y};
    bool inside = p.x >= d->bounds.origin.x && p.y >= d->bounds.origin.y &&
                  p.x < d->bounds.origin.x + d->bounds.size.width && p.y < d->bounds.origin.y + d->bounds.size.height;
    if (!inside && !objc::is_kind_of(view, g_UIWindow)) return nil;
    for (auto it = d->subviews.rbegin(); it != d->subviews.rend(); ++it) {
        if (objc::is_kind_of(*it, g_UIImageView)) continue;  // image views ignore touches by default
        if (id hit = hit_test(*it, p)) return hit;
    }
    return view;
}

// ---------------------------------------------------------------------------
// UIColor / UIFont / UIImage
// ---------------------------------------------------------------------------
struct ColorObj {
    Class isa;
    double rgba[4];
    id cgcolor;
};
id make_color(double r, double g, double b, double a) {
    id o = objc::alloc_instance(g_UIColor);
    auto* c = (ColorObj*)o;
    c->rgba[0] = r;
    c->rgba[1] = g;
    c->rgba[2] = b;
    c->rgba[3] = a;
    return o;
}
id Color_rgba(Class, SEL, CGFloat r, CGFloat g, CGFloat b, CGFloat a) { return objc::autorelease(make_color(r, g, b, a)); }
id Color_white_alpha(Class, SEL, CGFloat w, CGFloat a) { return objc::autorelease(make_color(w, w, w, a)); }
id Color_init_rgba(id self, SEL, CGFloat r, CGFloat g, CGFloat b, CGFloat a) {
    auto* c = (ColorObj*)self;
    c->rgba[0] = r;
    c->rgba[1] = g;
    c->rgba[2] = b;
    c->rgba[3] = a;
    return self;
}
#define NAMED_COLOR(fn, r, g, b, a) \
    id fn(Class, SEL) { return objc::autorelease(make_color(r, g, b, a)); }
NAMED_COLOR(Color_black, 0, 0, 0, 1)
NAMED_COLOR(Color_white, 1, 1, 1, 1)
NAMED_COLOR(Color_clear, 0, 0, 0, 0)
NAMED_COLOR(Color_lightGray, 2.0 / 3, 2.0 / 3, 2.0 / 3, 1)
NAMED_COLOR(Color_gray, 0.5, 0.5, 0.5, 1)
NAMED_COLOR(Color_darkGray, 1.0 / 3, 1.0 / 3, 1.0 / 3, 1)
NAMED_COLOR(Color_red, 1, 0, 0, 1)
NAMED_COLOR(Color_green, 0, 1, 0, 1)
NAMED_COLOR(Color_blue, 0, 0, 1, 1)
NAMED_COLOR(Color_yellow, 1, 1, 0, 1)
id Color_CGColor(id self, SEL) { return self; }  // CG accepts UIColor objects as colours
void Color_set(id, SEL) {}

struct FontObj {
    Class isa;
    double size;
    bool bold;
    id name;
};
id make_font(double size, bool bold) {
    id o = objc::alloc_instance(g_UIFont);
    auto* f = (FontObj*)o;
    f->size = size;
    f->bold = bold;
    return objc::autorelease(o);
}
id Font_systemFontOfSize(Class, SEL, CGFloat s) { return make_font(s, false); }
id Font_boldSystemFontOfSize(Class, SEL, CGFloat s) { return make_font(s, true); }
id Font_fontWithName_size(Class, SEL, id name, CGFloat s) {
    std::string n = name ? ns::to_utf8(name) : "";
    bool bold = n.find("Bold") != std::string::npos;
    return make_font(s, bold);
}
CGFloat Font_systemFontSize(Class, SEL) { return 14.0; }
id Font_fontWithSize(id self, SEL, CGFloat s) { return make_font(s, ((FontObj*)self)->bold); }
CGFloat Font_pointSize(id self, SEL) { return ((FontObj*)self)->size; }
CGFloat Font_lineHeight(id self, SEL) { return ((FontObj*)self)->size * 1.2; }
CGFloat Font_ascender(id self, SEL) { return ((FontObj*)self)->size * 0.95; }
CGFloat Font_descender(id self, SEL) { return -((FontObj*)self)->size * 0.25; }
CGFloat Font_capHeight(id self, SEL) { return ((FontObj*)self)->size * 0.7; }
CGFloat Font_leading(id self, SEL) { return ((FontObj*)self)->size * 1.2; }

cg::FontDesc font_desc(id font) {
    if (!font) return {12.0, false};
    auto* f = (FontObj*)font;
    return {f->size, f->bold};
}

struct ImageObj {
    Class isa;
    id cgimage;
    double scale;
};
id make_image(id cgimage, double scale) {
    id o = objc::alloc_instance(g_UIImage);
    ((ImageObj*)o)->cgimage = cgimage;
    ((ImageObj*)o)->scale = scale;
    return o;
}
id Image_imageNamed(Class, SEL, id name) {
    if (!name) return nil;
    std::string n = ns::to_utf8(name);
    std::string base = config().bundle_path + "/";
    std::vector<std::pair<std::string, double>> candidates;
    bool has_ext = n.find('.') != std::string::npos;
    std::string stem = has_ext ? n.substr(0, n.rfind('.')) : n;
    std::string ext = has_ext ? n.substr(n.rfind('.')) : ".png";
    candidates.push_back({stem + "@2x" + ext, 2.0});
    candidates.push_back({stem + ext, 1.0});
    for (auto& c : candidates) {
        if (id img = cg::image_from_file(base + c.first)) return objc::autorelease(make_image(img, c.second));
    }
    LOGW("UIImage imageNamed:%s not found", n.c_str());
    return nil;
}
id Image_imageWithContentsOfFile(Class, SEL, id path) {
    if (!path) return nil;
    id img = cg::image_from_file(ns::to_utf8(path));
    return img ? objc::autorelease(make_image(img, 1.0)) : nil;
}
id Image_imageWithCGImage(Class, SEL, id cg) {
    return cg ? objc::autorelease(make_image(objc::retain(cg), 1.0)) : nil;
}
id Image_initWithCGImage(id self, SEL, id cg) {
    ((ImageObj*)self)->cgimage = objc::retain(cg);
    ((ImageObj*)self)->scale = 1.0;
    return self;
}
void Image_dealloc(id self, SEL) {
    objc::release(((ImageObj*)self)->cgimage);
    objc::dispose_instance(self);
}
id Image_CGImage(id self, SEL) { return ((ImageObj*)self)->cgimage; }
CGSize Image_size(id self, SEL) {
    auto* i = (ImageObj*)self;
    if (!i->cgimage) return {0, 0};
    return {cg::image_width(i->cgimage) / i->scale, cg::image_height(i->cgimage) / i->scale};
}
CGFloat Image_scale(id self, SEL) { return ((ImageObj*)self)->scale; }
void Image_drawInRect(id self, SEL, CGRect r) {
    if (id ctx = cg::current_context()) cg::draw_image(ctx, ((ImageObj*)self)->cgimage, r);
}
void Image_drawAtPoint(id self, SEL s, CGPoint p) {
    CGSize sz = Image_size(self, s);
    Image_drawInRect(self, s, {p, sz});
}

// UIImageView
id ImageView_initWithImage(id self, SEL, id image) {
    CGSize sz = image ? Image_size(image, nullptr) : CGSize{0, 0};
    self = objc::send<id>(self, SEL_("initWithFrame:"), CGRect{{0, 0}, sz});
    V(self)->image = objc::retain(image);
    V(self)->user_interaction = false;
    return self;
}
id ImageView_image(id self, SEL) { return V(self)->image; }
void ImageView_setImage(id self, SEL, id image) {
    objc::retain(image);
    objc::release(V(self)->image);
    V(self)->image = image;
}

// NSString (UIStringDrawing)
CGSize Str_sizeWithFont(id self, SEL, id font) { return cg::measure_text(ns::to_u16(self), font_desc(font)); }
CGSize Str_drawAtPoint_withFont(id self, SEL, CGPoint p, id font) {
    auto s = ns::to_u16(self);
    auto f = font_desc(font);
    if (id ctx = cg::current_context()) cg::draw_text(ctx, s, p, f);
    return cg::measure_text(s, f);
}

// ---------------------------------------------------------------------------
// UITextField
// ---------------------------------------------------------------------------
id g_UIKeyboardWillShow = nil, g_UIKeyboardDidShow = nil, g_UIKeyboardWillHide = nil, g_UIKeyboardDidHide = nil,
   g_UITextFieldTextDidChange = nil;
id g_editing_field = nil;

id TF_text(id self, SEL) { return ns::str_auto(V(self)->text); }
void TF_setText(id self, SEL, id t) { V(self)->text = t ? ns::to_u16(t) : std::u16string(); }
void TF_setDelegate(id self, SEL, id d) { V(self)->delegate = d; }
id TF_delegate(id self, SEL) { return V(self)->delegate; }
void TF_setSecure(id self, SEL, BOOL b) { V(self)->secure = b; }
// UIControlEvents used by text fields
constexpr unsigned long kEditingDidBegin = 1ul << 16, kEditingChanged = 1ul << 17, kEditingDidEnd = 1ul << 18,
                        kEditingDidEndOnExit = 1ul << 19;
void TF_addTarget(id self, SEL, id target, SEL action, unsigned long events) {
    V(self)->targets.push_back({target, action, events});
}
void TF_removeTarget(id self, SEL, id target, SEL action, unsigned long events) {
    auto& t = V(self)->targets;
    for (size_t i = 0; i < t.size();)
        if ((!target || t[i].target == target) && (!action || t[i].action == action) && (t[i].events & events)) t.erase(t.begin() + i);
        else i++;
}
void send_actions(id tf, unsigned long event) {
    auto targets = V(tf)->targets;  // actions may modify the list
    for (auto& t : targets)
        if (t.events & event) objc::send<void>(t.target, t.action, tf, (id)nil);
}
void TF_setPlaceholder(id self, SEL, id p) { V(self)->placeholder = p ? ns::to_u16(p) : std::u16string(); }
BOOL TF_becomeFirstResponder(id self, SEL) {
    g_first_responder = self;
    g_editing_field = self;
    LOGI("UITextField becomeFirstResponder (text '%s')", ns::u16_to_utf8(V(self)->text).c_str());
    platform::start_text_input(V(self)->text, V(self)->placeholder, V(self)->secure);
    send_actions(self, kEditingDidBegin);
    foundation::post_notification(g_UIKeyboardWillShow, nil);
    foundation::post_notification(g_UIKeyboardDidShow, nil);
    return YES;
}
BOOL TF_resignFirstResponder(id self, SEL) {
    if (g_first_responder == self) g_first_responder = nil;
    if (g_editing_field == self) {
        g_editing_field = nil;
        platform::stop_text_input();
        send_actions(self, kEditingDidEnd);
        foundation::post_notification(g_UIKeyboardWillHide, nil);
        foundation::post_notification(g_UIKeyboardDidHide, nil);
    }
    return YES;
}
void TF_noop1(id, SEL, id) {}
void TF_noopL(id, SEL, long) {}
void TF_noopB(id, SEL, BOOL) {}

void text_changed(id tf) {
    send_actions(tf, kEditingChanged);
    foundation::post_notification(g_UITextFieldTextDidChange, tf);
}

// ---------------------------------------------------------------------------
// UIWebView (opens the external browser)
// ---------------------------------------------------------------------------
void Web_loadRequest(id self, SEL, id request) {
    id url = objc::send(request, SEL_("URL"));
    std::string s = foundation::url_string(url);
    LOGI("UIWebView loadRequest: %s", s.c_str());
    id del = V(self)->delegate;
    if (s.rfind("http", 0) == 0) platform::open_url(s);
    if (del && objc::responds_to(del->isa, SEL_("webViewDidFinishLoad:")))
        objc::send<void>(del, SEL_("webViewDidFinishLoad:"), self);
}
void Web_setDelegate(id self, SEL, id d) { V(self)->delegate = d; }

// ---------------------------------------------------------------------------
// UIAccelerometer
// ---------------------------------------------------------------------------
id g_accel = nil, g_accel_delegate = nil;
double g_accel_interval = 0.1;
id Accel_shared(Class, SEL) {
    if (!g_accel) g_accel = objc::alloc_instance(g_UIAccelerometer);
    return g_accel;
}
void Accel_setDelegate(id, SEL, id d) { g_accel_delegate = d; }
id Accel_delegate(id, SEL) { return g_accel_delegate; }
void Accel_setUpdateInterval(id, SEL, double i) { g_accel_interval = i; }
struct AccelObj {
    Class isa;
    double x, y, z, t;
};
double Acc_x(id s, SEL) { return ((AccelObj*)s)->x; }
double Acc_y(id s, SEL) { return ((AccelObj*)s)->y; }
double Acc_z(id s, SEL) { return ((AccelObj*)s)->z; }
double Acc_t(id s, SEL) { return ((AccelObj*)s)->t; }

// ---------------------------------------------------------------------------
// CATransaction / misc
// ---------------------------------------------------------------------------
void* g_tx_completion = nullptr;
void CATx_begin(Class, SEL) {}
void CATx_commit(Class, SEL) {
    void* blk = g_tx_completion;
    g_tx_completion = nullptr;
    if (blk) objc::call_block(blk);
}
void CATx_setCompletionBlock(Class, SEL, void* blk) { g_tx_completion = blk; }
void CATx_noopB(Class, SEL, BOOL) {}
void CATx_noopD(Class, SEL, double) {}

// UIView animations: applied immediately; the did-stop selector is invoked
// from the run loop so callers observe the usual ordering.
id g_anim_delegate = nil;
SEL g_anim_stop_sel = nullptr;
id g_anim_id = nil;
void void_ptr_ctx(void*) {}
void Anim_begin(Class, SEL, id anim_id, void*) {
    g_anim_delegate = nil;
    g_anim_stop_sel = nullptr;
    g_anim_id = anim_id;
}
void Anim_setDelegate(Class, SEL, id d) { g_anim_delegate = d; }
void Anim_setDidStop(Class, SEL, SEL s) { g_anim_stop_sel = s; }
void Anim_noopD(Class, SEL, double) {}
void Anim_noopL(Class, SEL, long) {}
void Anim_noopB(Class, SEL, BOOL) {}
void Anim_commit(Class, SEL) {
    if (g_anim_delegate && g_anim_stop_sel) {
        id d = objc::retain(g_anim_delegate);
        SEL s = g_anim_stop_sel;
        id aid = objc::retain(g_anim_id);
        foundation::run_on_main(
            [d, s, aid]() {
                objc::send<void>(d, s, aid, foundation::make_number_bool(true), (void*)nullptr);
                objc::release(aid);
                objc::release(d);
            },
            false);
    }
    g_anim_delegate = nil;
    g_anim_stop_sel = nullptr;
}

id NotifSettings_settings(Class cls, SEL, unsigned long, id) { return objc::autorelease(objc::alloc_instance(cls)); }

void Nav_setDelegate(id self, SEL, id d) {
    if (!VC(self)) VC_init(self, nullptr);
    VC(self)->delegate = d;
}
id Nav_delegate(id self, SEL) { return VC(self) ? VC(self)->delegate : nil; }

// A UIImagePickerController's view being shown starts the Android gallery picker.
void maybe_start_picker(id view) {
    id vc = V(view)->view_controller;
    if (!vc || !objc::is_kind_of(vc, g_UIImagePickerController) || g_active_picker) return;
    LOGI("UIImagePickerController shown: opening gallery");
    g_active_picker = objc::retain(vc);
    platform::pick_image();
}

}  // namespace

// --- public API ------------------------------------------------------------------
CGRect layer_bounds(id layer) { return L(layer)->bounds; }
double layer_contents_scale(id layer) { return L(layer)->contents_scale; }

id ui_color(double r, double g, double b, double a) { return objc::autorelease(make_color(r, g, b, a)); }
void color_components(id color, double out[4]) {
    if (color && objc::is_kind_of(color, g_UIColor)) {
        memcpy(out, ((ColorObj*)color)->rgba, sizeof(double) * 4);
    } else {
        out[0] = out[1] = out[2] = 0;
        out[3] = 1;
    }
}

void layout_pass() {
    // Copy: layoutSubviews may create/destroy views.
    std::vector<id> views = g_all_views;
    for (id v : views) {
        if (V(v)->needs_layout) {
            V(v)->needs_layout = false;
            objc::send<void>(v, SEL_("layoutSubviews"));
        }
    }
}

void dispatch_touches(const std::vector<TouchEvent>& events) {
    if (!g_key_window) return;
    double now = foundation::now_seconds();
    std::unordered_map<id, std::vector<id>> began, moved, ended, cancelled;
    for (auto& e : events) {
        id touch = nil;
        if (e.phase == kBegan) {
            touch = objc::alloc_instance(g_UITouch);
            auto* d = new TouchData();
            ((TouchObj*)touch)->d = d;
            d->loc = d->prev = e.location;
            d->window = g_key_window;
            d->view = hit_test(g_key_window, e.location);
            if (!d->view) d->view = g_key_window;
            double dx = e.location.x - g_last_tap.pos.x, dy = e.location.y - g_last_tap.pos.y;
            if (now - g_last_tap.time < 0.35 && dx * dx + dy * dy < 30 * 30) g_last_tap.count++;
            else g_last_tap.count = 1;
            g_last_tap.time = now;
            g_last_tap.pos = e.location;
            d->tap_count = g_last_tap.count;
            g_active_touches[e.pointer_id] = touch;
        } else {
            auto it = g_active_touches.find(e.pointer_id);
            if (it == g_active_touches.end()) continue;
            touch = it->second;
            T(touch)->prev = T(touch)->loc;
            T(touch)->loc = e.location;
        }
        auto* d = T(touch);
        d->phase = e.phase;
        d->timestamp = now;
        switch (e.phase) {
            case kBegan: began[d->view].push_back(touch); break;
            case kMoved:
            case kStationary: moved[d->view].push_back(touch); break;
            case kEnded: ended[d->view].push_back(touch); break;
            case kCancelled: cancelled[d->view].push_back(touch); break;
        }
    }
    std::vector<id> all;
    for (auto& kv : g_active_touches) all.push_back(kv.second);
    id all_set = foundation::make_set(all);
    id event = objc::alloc_instance(g_UIEvent);
    ((EventObj*)event)->all_touches = all_set;
    auto deliver = [&](std::unordered_map<id, std::vector<id>>& m, SEL sel) {
        for (auto& kv : m) {
            id set = foundation::make_set(kv.second);
            objc::send<void>(kv.first, sel, set, event);
            objc::release(set);
        }
    };
    void* pool = objc::pool_push();
    deliver(began, SEL_("touchesBegan:withEvent:"));
    deliver(moved, SEL_("touchesMoved:withEvent:"));
    deliver(ended, SEL_("touchesEnded:withEvent:"));
    deliver(cancelled, SEL_("touchesCancelled:withEvent:"));
    objc::pool_pop(pool);
    for (auto& e : events) {
        if (e.phase == kEnded || e.phase == kCancelled) {
            auto it = g_active_touches.find(e.pointer_id);
            if (it != g_active_touches.end()) {
                objc::release(it->second);
                g_active_touches.erase(it);
            }
        }
    }
    objc::release(all_set);
    objc::dispose_instance(event);
}

static void delegate_call(const char* sel_name) {
    if (!g_app || !APP()->delegate) return;
    SEL s = objc::sel(sel_name);
    id del = APP()->delegate;
    if (objc::responds_to(del->isa, s)) {
        LOGI("app delegate: %s", sel_name);
        void* pool = objc::pool_push();
        objc::send<void>(del, s, g_app);
        objc::pool_pop(pool);
    }
}
void app_will_resign_active() { delegate_call("applicationWillResignActive:"); }
void app_did_enter_background() { delegate_call("applicationDidEnterBackground:"); }
void app_will_enter_foreground() { delegate_call("applicationWillEnterForeground:"); }
void app_did_become_active() { delegate_call("applicationDidBecomeActive:"); }
void app_will_terminate() { delegate_call("applicationWillTerminate:"); }
void app_memory_warning() { delegate_call("applicationDidReceiveMemoryWarning:"); }

void accelerometer_update(double x, double y, double z) {
    if (!g_accel_delegate) return;
    static double last = 0;
    double now = foundation::now_seconds();
    if (now - last < g_accel_interval) return;
    last = now;
    id a = objc::alloc_instance(g_UIAcceleration);
    auto* ao = (AccelObj*)a;
    ao->x = x;
    ao->y = y;
    ao->z = z;
    ao->t = now;
    objc::send<void>(g_accel_delegate, SEL_("accelerometer:didAccelerate:"), Accel_shared(nullptr, nullptr), a);
    objc::release(a);
}

bool text_input_active() { return g_editing_field != nil; }
void text_input_insert(const std::u16string& s) {
    if (!g_editing_field) return;
    V(g_editing_field)->text += s;
    text_changed(g_editing_field);
}
void text_input_backspace() {
    if (!g_editing_field || V(g_editing_field)->text.empty()) return;
    V(g_editing_field)->text.pop_back();
    text_changed(g_editing_field);
}
void text_input_return() {
    if (!g_editing_field) return;
    id tf = g_editing_field;
    id del = V(tf)->delegate;
    BOOL ok = YES;
    if (del && objc::responds_to(del->isa, SEL_("textFieldShouldReturn:")))
        ok = objc::send<BOOL>(del, SEL_("textFieldShouldReturn:"), tf);
    if (ok) {
        send_actions(tf, kEditingDidEndOnExit);
        if (g_editing_field == tf) TF_resignFirstResponder(tf, nullptr);
    }
}

void text_input_finish(const std::u16string& text, bool accepted) {
    id tf = g_editing_field;
    if (!tf) return;
    LOGI("text input finished (%s): '%s'", accepted ? "ok" : "cancel", ns::u16_to_utf8(text).c_str());
    if (accepted && text != V(tf)->text) {
        V(tf)->text = text;
        text_changed(tf);
    }
    text_input_return();
    // The game only leaves its text mode through the return key path.
    if (g_editing_field == tf) {
        send_actions(tf, kEditingDidEndOnExit);
        TF_resignFirstResponder(tf, nullptr);
    }
}

void image_picked(id cgimage) {
    id picker = g_active_picker;
    g_active_picker = nil;
    if (!picker) return;
    id del = VC(picker) ? VC(picker)->delegate : nil;
    void* pool = objc::pool_push();
    if (cgimage && del) {
        id image = objc::autorelease(make_image(objc::retain(cgimage), 1.0));
        id info = foundation::make_mutable_dictionary();
        foundation::dict_set(info, g_UIImagePickerControllerOriginalImage, image);
        objc::autorelease(info);
        LOGI("image picked: %dx%d", cg::image_width(cgimage), cg::image_height(cgimage));
        if (objc::responds_to(del->isa, SEL_("imagePickerController:didFinishPickingMediaWithInfo:")))
            objc::send<void>(del, SEL_("imagePickerController:didFinishPickingMediaWithInfo:"), picker, info);
    } else if (del && objc::responds_to(del->isa, SEL_("imagePickerControllerDidCancel:"))) {
        objc::send<void>(del, SEL_("imagePickerControllerDidCancel:"), picker);
    }
    objc::pool_pop(pool);
    objc::release(picker);
}

// --- UIApplicationMain -------------------------------------------------------------
static int hle_UIApplicationMain(int argc, char** argv, id principal, id delegate_name) {
    LOGI("UIApplicationMain(delegate=%s)", delegate_name ? ns::to_utf8(delegate_name).c_str() : "nil (nib)");
    id app = App_sharedApplication(nullptr, nullptr);
    Class dcls = delegate_name ? objc::get_class(ns::to_utf8(delegate_name).c_str())
                               : objc::get_class("RuntimeAppDelegate");  // from MainWindow.nib
    id delegate = objc::send(objc::send((id)dcls, SEL_("alloc")), SEL_("init"));
    App_setDelegate(app, nullptr, delegate);
    if (!delegate_name) {
        // MainWindow.nib: a full-screen UIWindow connected to delegate.window.
        CGRect r = Screen_bounds(nullptr, nullptr);
        id window = objc::send<id>(objc::send((id)g_UIWindow, SEL_("alloc")), SEL_("initWithFrame:"), r);
        objc::send<void>(delegate, SEL_("setWindow:"), window);
        objc::release(window);
    }
    {
        void* pool = objc::pool_push();
        id opts = foundation::make_mutable_dictionary();
        if (objc::responds_to(dcls, SEL_("application:didFinishLaunchingWithOptions:"))) {
            LOGI("-> application:didFinishLaunchingWithOptions:");
            objc::send<BOOL>(delegate, SEL_("application:didFinishLaunchingWithOptions:"), app, opts);
        } else {
            objc::send<void>(delegate, SEL_("applicationDidFinishLaunching:"), app);
        }
        objc::release(opts);
        objc::pool_pop(pool);
    }
    app_did_become_active();
    platform::run_main_loop();
    app_will_terminate();
    return 0;
}
HLE_EXPORT(UIApplicationMain, hle_UIApplicationMain);

static id g_UIApplicationLaunchOptionsRemoteNotificationKey = nil;
HLE_EXPORT(UIApplicationLaunchOptionsRemoteNotificationKey, &g_UIApplicationLaunchOptionsRemoteNotificationKey);
HLE_EXPORT(UIImagePickerControllerOriginalImage, &g_UIImagePickerControllerOriginalImage);

void init() {
    using objc::add_class_method;
    using objc::add_method;
    g_UIApplicationLaunchOptionsRemoteNotificationKey = ns::make_constant("UIApplicationLaunchOptionsRemoteNotificationKey");
    g_UIImagePickerControllerOriginalImage = ns::make_constant("UIImagePickerControllerOriginalImage");
    g_UIKeyboardWillShow = ns::make_constant("UIKeyboardWillShowNotification");
    g_UIKeyboardDidShow = ns::make_constant("UIKeyboardDidShowNotification");
    g_UIKeyboardWillHide = ns::make_constant("UIKeyboardWillHideNotification");
    g_UIKeyboardDidHide = ns::make_constant("UIKeyboardDidHideNotification");
    g_UITextFieldTextDidChange = ns::make_constant("UITextFieldTextDidChangeNotification");
    hle::register_symbol("_UIKeyboardWillShowNotification", &g_UIKeyboardWillShow);
    hle::register_symbol("_UIKeyboardDidShowNotification", &g_UIKeyboardDidShow);
    hle::register_symbol("_UIKeyboardWillHideNotification", &g_UIKeyboardWillHide);
    hle::register_symbol("_UIKeyboardDidHideNotification", &g_UIKeyboardDidHide);

    // Layers
    g_CALayer = objc::define_class("CALayer", "NSObject", sizeof(LayerObj));
    Class CL = g_CALayer;
    add_method(CL, "init", (void*)Layer_init);
    add_method(CL, "dealloc", (void*)Layer_dealloc);
    add_method(CL, "bounds", (void*)Layer_bounds);
    add_method(CL, "setBounds:", (void*)Layer_setBounds);
    add_method(CL, "frame", (void*)Layer_frame);
    add_method(CL, "setFrame:", (void*)Layer_setFrame);
    add_method(CL, "setOpaque:", (void*)Layer_setOpaque);
    add_method(CL, "isOpaque", (void*)Layer_isOpaque);
    add_method(CL, "contentsScale", (void*)Layer_contentsScale);
    add_method(CL, "setContentsScale:", (void*)Layer_setContentsScale);
    add_method(CL, "setDrawableProperties:", (void*)Layer_setDrawableProperties);
    add_method(CL, "drawableProperties", (void*)Layer_drawableProperties);
    add_method(CL, "setContents:", (void*)Layer_noop1);
    g_CAEAGLLayer = objc::define_class("CAEAGLLayer", "CALayer", sizeof(LayerObj));

    Class TX = objc::define_class("CATransaction", "NSObject", 8);
    add_class_method(TX, "begin", (void*)CATx_begin);
    add_class_method(TX, "commit", (void*)CATx_commit);
    add_class_method(TX, "flush", (void*)CATx_begin);
    add_class_method(TX, "setCompletionBlock:", (void*)CATx_setCompletionBlock);
    add_class_method(TX, "setDisableActions:", (void*)CATx_noopB);
    add_class_method(TX, "setAnimationDuration:", (void*)CATx_noopD);

    // Responder / views (sizes match iOS 8 so subclass ivars line up)
    g_UIResponder = objc::define_class("UIResponder", "NSObject", 8);
    Class R = g_UIResponder;
    add_method(R, "becomeFirstResponder", (void*)Resp_becomeFirstResponder);
    add_method(R, "resignFirstResponder", (void*)Resp_resignFirstResponder);
    add_method(R, "isFirstResponder", (void*)Resp_isFirstResponder);
    add_method(R, "canBecomeFirstResponder", (void*)Resp_canBecomeFirstResponder);
    add_method(R, "nextResponder", (void*)Resp_nextResponder);
    for (const char* s : {"touchesBegan:withEvent:", "touchesMoved:withEvent:", "touchesEnded:withEvent:",
                          "touchesCancelled:withEvent:"})
        add_method(R, s, (void*)Resp_touches);

    g_UIView = objc::define_class("UIView", "UIResponder", 36);
    Class Vw = g_UIView;
    add_class_method(Vw, "layerClass", (void*)View_cls_layerClass);
    add_class_method(Vw, "beginAnimations:context:", (void*)Anim_begin);
    add_class_method(Vw, "commitAnimations", (void*)Anim_commit);
    add_class_method(Vw, "setAnimationDelegate:", (void*)Anim_setDelegate);
    add_class_method(Vw, "setAnimationDidStopSelector:", (void*)Anim_setDidStop);
    add_class_method(Vw, "setAnimationDuration:", (void*)Anim_noopD);
    add_class_method(Vw, "setAnimationDelay:", (void*)Anim_noopD);
    add_class_method(Vw, "setAnimationCurve:", (void*)Anim_noopL);
    add_class_method(Vw, "setAnimationsEnabled:", (void*)Anim_noopB);
    add_method(Vw, "init", (void*)View_init);
    add_method(Vw, "initWithFrame:", (void*)View_initWithFrame);
    add_method(Vw, "initWithCoder:", (void*)View_initWithCoder);
    add_method(Vw, "dealloc", (void*)View_dealloc);
    add_method(Vw, "frame", (void*)View_frame);
    add_method(Vw, "setFrame:", (void*)View_setFrame);
    add_method(Vw, "bounds", (void*)View_bounds);
    add_method(Vw, "setBounds:", (void*)View_setBounds);
    add_method(Vw, "center", (void*)View_center);
    add_method(Vw, "setCenter:", (void*)View_setCenter);
    add_method(Vw, "layer", (void*)View_layer);
    add_method(Vw, "superview", (void*)View_superview);
    add_method(Vw, "subviews", (void*)View_subviews);
    add_method(Vw, "window", (void*)View_window);
    add_method(Vw, "addSubview:", (void*)View_addSubview);
    add_method(Vw, "removeFromSuperview", (void*)View_removeFromSuperview);
    add_method(Vw, "bringSubviewToFront:", (void*)View_bringSubviewToFront);
    add_method(Vw, "sendSubviewToBack:", (void*)View_sendSubviewToBack);
    add_method(Vw, "setHidden:", (void*)View_setHidden);
    add_method(Vw, "isHidden", (void*)View_isHidden);
    add_method(Vw, "setMultipleTouchEnabled:", (void*)View_setMultipleTouchEnabled);
    add_method(Vw, "isMultipleTouchEnabled", (void*)View_isMultipleTouchEnabled);
    add_method(Vw, "setExclusiveTouch:", (void*)View_setExclusiveTouch);
    add_method(Vw, "setUserInteractionEnabled:", (void*)View_setUserInteractionEnabled);
    add_method(Vw, "isUserInteractionEnabled", (void*)View_isUserInteractionEnabled);
    add_method(Vw, "setAlpha:", (void*)View_setAlpha);
    add_method(Vw, "alpha", (void*)View_alpha);
    add_method(Vw, "setContentMode:", (void*)View_setContentMode);
    add_method(Vw, "setTag:", (void*)View_setTag);
    add_method(Vw, "tag", (void*)View_tag);
    add_method(Vw, "setTransform:", (void*)View_setTransform);
    add_method(Vw, "transform", (void*)View_transform);
    add_method(Vw, "setBackgroundColor:", (void*)View_setBackgroundColor);
    add_method(Vw, "backgroundColor", (void*)View_backgroundColor);
    add_method(Vw, "setOpaque:", (void*)View_setOpaque);
    add_method(Vw, "contentScaleFactor", (void*)View_contentScaleFactor);
    add_method(Vw, "setContentScaleFactor:", (void*)View_setContentScaleFactor);
    add_method(Vw, "setNeedsLayout", (void*)View_setNeedsLayout);
    add_method(Vw, "layoutIfNeeded", (void*)View_noop);
    add_method(Vw, "layoutSubviews", (void*)View_layoutSubviews);
    add_method(Vw, "setNeedsDisplay", (void*)View_noop);
    add_method(Vw, "setAutoresizingMask:", (void*)View_noopL);
    add_method(Vw, "setAutoresizesSubviews:", (void*)View_noopB);
    add_method(Vw, "setClipsToBounds:", (void*)View_noopB);
    add_method(Vw, "endEditing:", (void*)View_endEditing);
    add_method(Vw, "convertPoint:toView:", (void*)View_convertPoint_toView);
    add_method(Vw, "convertPoint:fromView:", (void*)View_convertPoint_fromView);
    add_method(Vw, "pointInside:withEvent:", (void*)View_pointInside);

    g_UIWindow = objc::define_class("UIWindow", "UIView", 36);
    add_method(g_UIWindow, "makeKeyAndVisible", (void*)Window_makeKeyAndVisible);
    add_method(g_UIWindow, "makeKeyWindow", (void*)Window_makeKeyWindow);
    add_method(g_UIWindow, "becomeKeyWindow", (void*)Window_makeKeyWindow);
    add_method(g_UIWindow, "rootViewController", (void*)Window_rootViewController);
    add_method(g_UIWindow, "setRootViewController:", (void*)Window_setRootViewController);

    g_UIImageView = objc::define_class("UIImageView", "UIView", 104);
    add_method(g_UIImageView, "initWithImage:", (void*)ImageView_initWithImage);
    add_method(g_UIImageView, "image", (void*)ImageView_image);
    add_method(g_UIImageView, "setImage:", (void*)ImageView_setImage);

    g_UITextField = objc::define_class("UITextField", "UIView", 104);
    Class TF = g_UITextField;
    add_method(TF, "text", (void*)TF_text);
    add_method(TF, "setText:", (void*)TF_setText);
    add_method(TF, "setDelegate:", (void*)TF_setDelegate);
    add_method(TF, "delegate", (void*)TF_delegate);
    add_method(TF, "setSecureTextEntry:", (void*)TF_setSecure);
    add_method(TF, "addTarget:action:forControlEvents:", (void*)TF_addTarget);
    add_method(TF, "removeTarget:action:forControlEvents:", (void*)TF_removeTarget);
    add_method(TF, "setPlaceholder:", (void*)TF_setPlaceholder);
    add_method(TF, "becomeFirstResponder", (void*)TF_becomeFirstResponder);
    add_method(TF, "resignFirstResponder", (void*)TF_resignFirstResponder);
    for (const char* s : {"setTextColor:", "setFont:"}) add_method(TF, s, (void*)TF_noop1);
    for (const char* s : {"setBorderStyle:", "setKeyboardType:", "setReturnKeyType:", "setAutocorrectionType:",
                          "setAutocapitalizationType:", "setClearButtonMode:", "setTextAlignment:",
                          "setKeyboardAppearance:"})
        add_method(TF, s, (void*)TF_noopL);
    for (const char* s : {"setAdjustsFontSizeToFitWidth:", "setEnablesReturnKeyAutomatically:"})
        add_method(TF, s, (void*)TF_noopB);

    g_UIWebView = objc::define_class("UIWebView", "UIView", 104);
    add_method(g_UIWebView, "loadRequest:", (void*)Web_loadRequest);
    add_method(g_UIWebView, "setDelegate:", (void*)Web_setDelegate);
    add_method(g_UIWebView, "setScalesPageToFit:", (void*)View_noopB);
    add_method(g_UIWebView, "stopLoading", (void*)View_noop);

    g_UIViewController = objc::define_class("UIViewController", "UIResponder", 320);
    Class VCc = g_UIViewController;
    add_method(VCc, "init", (void*)VC_init);
    add_method(VCc, "initWithNibName:bundle:", (void*)VC_initWithNib);
    add_method(VCc, "dealloc", (void*)VC_dealloc);
    add_method(VCc, "view", (void*)VC_view);
    add_method(VCc, "setView:", (void*)VC_setView);
    add_method(VCc, "isViewLoaded", (void*)VC_isViewLoaded);
    add_method(VCc, "loadView", (void*)VC_noop);
    add_method(VCc, "viewDidLoad", (void*)VC_noop);
    add_method(VCc, "viewWillAppear:", (void*)VC_noopB);
    add_method(VCc, "viewDidAppear:", (void*)VC_noopB);
    add_method(VCc, "viewWillDisappear:", (void*)VC_noopB);
    add_method(VCc, "viewDidDisappear:", (void*)VC_noopB);
    add_method(VCc, "presentViewController:animated:completion:", (void*)VC_present);
    add_method(VCc, "presentModalViewController:animated:", (void*)VC_presentModal);
    add_method(VCc, "dismissViewControllerAnimated:completion:", (void*)VC_dismiss);
    add_method(VCc, "dismissModalViewControllerAnimated:", (void*)VC_dismissModal);
    add_method(VCc, "presentedViewController", (void*)VC_presentedViewController);
    add_method(VCc, "presentingViewController", (void*)VC_presentingViewController);
    add_method(VCc, "removeFromParentViewController", (void*)VC_noop);
    add_method(VCc, "interfaceOrientation", (void*)VC_interfaceOrientation);
    add_method(VCc, "shouldAutorotate", (void*)VC_shouldAutorotate);
    add_method(VCc, "setModalPresentationStyle:", (void*)View_noopL);
    add_method(VCc, "setWantsFullScreenLayout:", (void*)View_noopB);

    // UINavigationController / UIImagePickerController (iOS 8: 400 bytes)
    Class Nav = objc::define_class("UINavigationController", "UIViewController", 400);
    add_method(Nav, "setDelegate:", (void*)Nav_setDelegate);
    add_method(Nav, "delegate", (void*)Nav_delegate);
    Class Picker = objc::define_class("UIImagePickerController", "UINavigationController", 400);
    g_UIImagePickerController = Picker;
    add_class_method(Picker, "isSourceTypeAvailable:", (void*)+[](Class, SEL, long t) -> BOOL { return t != 1; });
    add_method(Picker, "setSourceType:", (void*)View_noopL);
    add_method(Picker, "setAllowsEditing:", (void*)View_noopB);

    g_UIApplication = objc::define_class("UIApplication", "UIResponder", 16);
    Class A = g_UIApplication;
    add_class_method(A, "sharedApplication", (void*)App_sharedApplication);
    add_method(A, "delegate", (void*)App_delegate);
    add_method(A, "setDelegate:", (void*)App_setDelegate);
    add_method(A, "keyWindow", (void*)App_keyWindow);
    add_method(A, "windows", (void*)App_windows);
    add_method(A, "statusBarOrientation", (void*)App_statusBarOrientation);
    add_method(A, "setStatusBarOrientation:", (void*)App_setStatusBarOrientation);
    add_method(A, "setStatusBarOrientation:animated:", (void*)App_setStatusBarOrientation_animated);
    add_method(A, "setIdleTimerDisabled:", (void*)App_setIdleTimerDisabled);
    add_method(A, "setApplicationIconBadgeNumber:", (void*)App_setBadge);
    add_method(A, "applicationIconBadgeNumber", (void*)App_badge);
    add_method(A, "openURL:", (void*)App_openURL);
    add_method(A, "canOpenURL:", (void*)App_canOpenURL);
    add_method(A, "applicationState", (void*)App_applicationState);
    add_method(A, "setStatusBarHidden:", (void*)App_noopB);
    add_method(A, "setStatusBarHidden:withAnimation:", (void*)App_noopBB);
    add_method(A, "setStatusBarStyle:", (void*)App_noopL);
    add_method(A, "setApplicationSupportsShakeToEdit:", (void*)App_noopB);
    add_method(A, "registerForRemoteNotificationTypes:", (void*)App_noopL);
    add_method(A, "registerUserNotificationSettings:", (void*)App_noop1);
    add_method(A, "registerForRemoteNotifications", (void*)App_noop);
    add_method(A, "beginIgnoringInteractionEvents", (void*)App_noop);
    add_method(A, "endIgnoringInteractionEvents", (void*)App_noop);
    add_method(A, "cancelAllLocalNotifications", (void*)App_noop);

    g_UIScreen = objc::define_class("UIScreen", "NSObject", 8);
    add_class_method(g_UIScreen, "mainScreen", (void*)Screen_mainScreen);
    add_class_method(g_UIScreen, "screens", (void*)Screen_screens);
    add_method(g_UIScreen, "bounds", (void*)Screen_bounds);
    add_method(g_UIScreen, "applicationFrame", (void*)Screen_bounds);
    add_method(g_UIScreen, "nativeBounds", (void*)Screen_nativeBounds);
    add_method(g_UIScreen, "scale", (void*)Screen_scale);
    add_method(g_UIScreen, "nativeScale", (void*)Screen_scale);

    g_UIDevice = objc::define_class("UIDevice", "NSObject", 8);
    add_class_method(g_UIDevice, "currentDevice", (void*)Device_currentDevice);
    add_method(g_UIDevice, "systemVersion", (void*)Device_systemVersion);
    add_method(g_UIDevice, "systemName", (void*)Device_systemName);
    add_method(g_UIDevice, "model", (void*)Device_model);
    add_method(g_UIDevice, "localizedModel", (void*)Device_model);
    add_method(g_UIDevice, "name", (void*)Device_name);
    add_method(g_UIDevice, "userInterfaceIdiom", (void*)Device_userInterfaceIdiom);
    add_method(g_UIDevice, "orientation", (void*)Device_orientation);
    add_method(g_UIDevice, "isMultitaskingSupported", (void*)Device_isMultitaskingSupported);
    add_method(g_UIDevice, "identifierForVendor", (void*)Device_identifierForVendor);
    add_method(g_UIDevice, "batteryLevel", (void*)Device_batteryLevel);
    add_method(g_UIDevice, "beginGeneratingDeviceOrientationNotifications", (void*)Device_noop);
    add_method(g_UIDevice, "endGeneratingDeviceOrientationNotifications", (void*)Device_noop);

    g_UITouch = objc::define_class("UITouch", "NSObject", sizeof(TouchObj));
    add_method(g_UITouch, "locationInView:", (void*)Touch_locationInView);
    add_method(g_UITouch, "previousLocationInView:", (void*)Touch_previousLocationInView);
    add_method(g_UITouch, "tapCount", (void*)Touch_tapCount);
    add_method(g_UITouch, "phase", (void*)Touch_phase);
    add_method(g_UITouch, "view", (void*)Touch_view);
    add_method(g_UITouch, "window", (void*)Touch_window);
    add_method(g_UITouch, "timestamp", (void*)Touch_timestamp);
    add_method(g_UITouch, "dealloc", (void*)Touch_dealloc);
    g_UIEvent = objc::define_class("UIEvent", "NSObject", sizeof(EventObj));
    add_method(g_UIEvent, "allTouches", (void*)Event_allTouches);
    add_method(g_UIEvent, "touchesForView:", (void*)Event_touchesForView);
    add_method(g_UIEvent, "touchesForWindow:", (void*)Event_touchesForWindow);
    add_method(g_UIEvent, "timestamp", (void*)Event_timestamp);
    add_method(g_UIEvent, "type", (void*)Event_type);

    g_UIColor = objc::define_class("UIColor", "NSObject", sizeof(ColorObj));
    Class C = g_UIColor;
    add_class_method(C, "colorWithRed:green:blue:alpha:", (void*)Color_rgba);
    add_class_method(C, "colorWithWhite:alpha:", (void*)Color_white_alpha);
    add_class_method(C, "blackColor", (void*)Color_black);
    add_class_method(C, "whiteColor", (void*)Color_white);
    add_class_method(C, "clearColor", (void*)Color_clear);
    add_class_method(C, "lightGrayColor", (void*)Color_lightGray);
    add_class_method(C, "grayColor", (void*)Color_gray);
    add_class_method(C, "darkGrayColor", (void*)Color_darkGray);
    add_class_method(C, "redColor", (void*)Color_red);
    add_class_method(C, "greenColor", (void*)Color_green);
    add_class_method(C, "blueColor", (void*)Color_blue);
    add_class_method(C, "yellowColor", (void*)Color_yellow);
    add_method(C, "initWithRed:green:blue:alpha:", (void*)Color_init_rgba);
    add_method(C, "CGColor", (void*)Color_CGColor);
    add_method(C, "set", (void*)Color_set);
    add_method(C, "setFill", (void*)Color_set);
    add_method(C, "setStroke", (void*)Color_set);

    g_UIFont = objc::define_class("UIFont", "NSObject", sizeof(FontObj));
    add_class_method(g_UIFont, "systemFontOfSize:", (void*)Font_systemFontOfSize);
    add_class_method(g_UIFont, "boldSystemFontOfSize:", (void*)Font_boldSystemFontOfSize);
    add_class_method(g_UIFont, "fontWithName:size:", (void*)Font_fontWithName_size);
    add_class_method(g_UIFont, "systemFontSize", (void*)Font_systemFontSize);
    add_method(g_UIFont, "fontWithSize:", (void*)Font_fontWithSize);
    add_method(g_UIFont, "pointSize", (void*)Font_pointSize);
    add_method(g_UIFont, "lineHeight", (void*)Font_lineHeight);
    add_method(g_UIFont, "ascender", (void*)Font_ascender);
    add_method(g_UIFont, "descender", (void*)Font_descender);
    add_method(g_UIFont, "capHeight", (void*)Font_capHeight);
    add_method(g_UIFont, "leading", (void*)Font_leading);

    g_UIImage = objc::define_class("UIImage", "NSObject", sizeof(ImageObj));
    add_class_method(g_UIImage, "imageNamed:", (void*)Image_imageNamed);
    add_class_method(g_UIImage, "imageWithContentsOfFile:", (void*)Image_imageWithContentsOfFile);
    add_class_method(g_UIImage, "imageWithCGImage:", (void*)Image_imageWithCGImage);
    add_method(g_UIImage, "initWithCGImage:", (void*)Image_initWithCGImage);
    add_method(g_UIImage, "dealloc", (void*)Image_dealloc);
    add_method(g_UIImage, "CGImage", (void*)Image_CGImage);
    add_method(g_UIImage, "size", (void*)Image_size);
    add_method(g_UIImage, "scale", (void*)Image_scale);
    add_method(g_UIImage, "drawInRect:", (void*)Image_drawInRect);
    add_method(g_UIImage, "drawAtPoint:", (void*)Image_drawAtPoint);

    Class S = objc::get_class("NSString");
    add_method(S, "sizeWithFont:", (void*)Str_sizeWithFont);
    add_method(S, "drawAtPoint:withFont:", (void*)Str_drawAtPoint_withFont);

    g_UIAccelerometer = objc::define_class("UIAccelerometer", "NSObject", 8);
    add_class_method(g_UIAccelerometer, "sharedAccelerometer", (void*)Accel_shared);
    add_method(g_UIAccelerometer, "setDelegate:", (void*)Accel_setDelegate);
    add_method(g_UIAccelerometer, "delegate", (void*)Accel_delegate);
    add_method(g_UIAccelerometer, "setUpdateInterval:", (void*)Accel_setUpdateInterval);
    g_UIAcceleration = objc::define_class("UIAcceleration", "NSObject", sizeof(AccelObj));
    add_method(g_UIAcceleration, "x", (void*)Acc_x);
    add_method(g_UIAcceleration, "y", (void*)Acc_y);
    add_method(g_UIAcceleration, "z", (void*)Acc_z);
    add_method(g_UIAcceleration, "timestamp", (void*)Acc_t);

    Class UNS = objc::define_class("UIUserNotificationSettings", "NSObject", 8);
    add_class_method(UNS, "settingsForTypes:categories:", (void*)NotifSettings_settings);
}

}  // namespace uikit
