// CoreGraphics HLE: a small software renderer for RGBA8 bitmap contexts.
//
// Coordinate conventions follow Quartz: device space has its origin at the
// bottom-left with y up; bitmap memory row 0 is the top of the device. CGImage
// pixel data is stored with row 0 at the top, premultiplied RGBA.
#include "hle/cg.h"

#include <SDL.h>
#include <jni.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <mutex>
#include <vector>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_NO_STDIO
#include "stb_image.h"

#include "hle/nsstring.h"
#include "hle/config.h"
#include "hle/bitmap_hd.h"
#include "hle/texture_pixels.h"
#include "hle/uikit.h"
#include "objc/runtime.h"

namespace cg {

struct Affine {
    double a = 1, b = 0, c = 0, d = 1, tx = 0, ty = 0;
    void apply(double x, double y, double& ox, double& oy) const {
        ox = a * x + c * y + tx;
        oy = b * x + d * y + ty;
    }
    // this followed by m: p -> m(this(p))
    Affine then(const Affine& m) const {
        Affine r;
        r.a = a * m.a + b * m.c;
        r.b = a * m.b + b * m.d;
        r.c = c * m.a + d * m.c;
        r.d = c * m.b + d * m.d;
        r.tx = tx * m.a + ty * m.c + m.tx;
        r.ty = tx * m.b + ty * m.d + m.ty;
        return r;
    }
    bool invert(Affine& out) const {
        double det = a * d - b * c;
        if (std::fabs(det) < 1e-12) return false;
        out.a = d / det;
        out.b = -b / det;
        out.c = -c / det;
        out.d = a / det;
        out.tx = -(out.a * tx + out.c * ty);
        out.ty = -(out.b * tx + out.d * ty);
        return true;
    }
};

namespace {

Class g_CGImage, g_CGContext, g_CGColor, g_CGColorSpace, g_CGDataProvider;

// --- objects ---------------------------------------------------------------------
struct ImageData {
    int w = 0, h = 0;
    int logical_w = 0, logical_h = 0;
    std::vector<uint32_t> px;  // premultiplied RGBA (little-endian u32: R | G<<8 | B<<16 | A<<24)
};
struct ImageObj {
    Class isa;
    ImageData* d;
};
ImageData* IMG(id o) { return ((ImageObj*)o)->d; }

struct ColorObj {
    Class isa;
    double rgba[4];
};

struct Shadow {
    bool on = false;
    double dx = 0, dy = 0, blur = 0;
    double rgba[4] = {0, 0, 0, 1.0 / 3};
};

struct GState {
    Affine ctm;
    double fill[4] = {0, 0, 0, 1};
    double stroke[4] = {0, 0, 0, 1};
    double line_width = 1;
    int clip_x0 = 0, clip_y0 = 0, clip_x1 = 1 << 30, clip_y1 = 1 << 30;  // memory coords, exclusive
    int blend = 0;
    int text_mode = 0;
    Shadow shadow;
    bool interpolate = true;
};

struct PathPt {
    double x, y;
};

struct CtxData {
    uint8_t* data = nullptr;
    bool owns = false;
    int w = 0, h = 0;
    size_t bpr = 0;
    uint32_t info = 1;
    GState gs;
    std::vector<GState> stack;
    std::vector<std::vector<PathPt>> path;  // device space (y up)
    int scale = 1, logical_w = 0, logical_h = 0;
    uint8_t* logical_data = nullptr;
    size_t logical_bpr = 0;
    bool logical_owns = false;
};
struct CtxObj {
    Class isa;
    CtxData* d;
};
CtxData* CTX(id o) { return ((CtxObj*)o)->d; }

struct ProviderObj {
    Class isa;
    std::vector<uint8_t>* bytes;
    void* info;
    void (*release_info)(void*);
};

// --- pixel helpers ---------------------------------------------------------------
inline uint32_t pack(int r, int g, int b, int a) {
    return (uint32_t)r | ((uint32_t)g << 8) | ((uint32_t)b << 16) | ((uint32_t)a << 24);
}
inline uint32_t premul_color(const double c[4], double alpha_scale = 1.0) {
    double a = std::clamp(c[3] * alpha_scale, 0.0, 1.0);
    return pack((int)std::lround(std::clamp(c[0], 0.0, 1.0) * a * 255), (int)std::lround(std::clamp(c[1], 0.0, 1.0) * a * 255),
                (int)std::lround(std::clamp(c[2], 0.0, 1.0) * a * 255), (int)std::lround(a * 255));
}
inline uint32_t scale_px(uint32_t p, int cov /*0..255*/) {
    if (cov >= 255) return p;
    uint32_t rb = ((p & 0x00ff00ff) * cov + 0x00800080) >> 8 & 0x00ff00ff;
    uint32_t ga = (((p >> 8) & 0x00ff00ff) * cov + 0x00800080) >> 8 & 0x00ff00ff;
    return rb | (ga << 8);
}
inline uint32_t over(uint32_t src, uint32_t dst) {
    uint32_t sa = src >> 24;
    if (sa == 255) return src;
    if (sa == 0) return dst;
    return src + scale_px(dst, 255 - sa);
}
inline uint32_t blend_px(int mode, uint32_t src, uint32_t dst) {
    switch (mode) {
        case 0: return over(src, dst);
        case 16: return 0;     // clear
        case 17: return src;   // copy
        case 18: return scale_px(src, dst >> 24);                       // source in
        case 19: return scale_px(src, 255 - (dst >> 24));               // source out
        case 21: return over(dst, src);                                 // destination over
        case 22: return scale_px(dst, src >> 24);                       // destination in
        case 23: return scale_px(dst, 255 - (src >> 24));               // destination out
        case 27: {                                                      // plus lighter
            uint32_t out = 0;
            for (int k = 0; k < 32; k += 8) out |= std::min(255u, ((src >> k) & 255) + ((dst >> k) & 255)) << k;
            return out;
        }
        case 1: {  // multiply (premultiplied): s*d + s*(1-da) + d*(1-sa)
            uint32_t out = 0;
            uint32_t sa = src >> 24, da = dst >> 24;
            for (int k = 0; k < 32; k += 8) {
                uint32_t s = (src >> k) & 255, d = (dst >> k) & 255;
                uint32_t v = (s * d + s * (255 - da) + d * (255 - sa) + 127) / 255;
                if (k == 24) v = sa + da - (sa * da + 127) / 255;
                out |= std::min(255u, v) << k;
            }
            return out;
        }
        default: return over(src, dst);
    }
}

// Bitmap formats: we support 32-bit RGBA/ARGB variants (premultiplied or not)
// and 8-bit alpha-only. Pixels are converted to/from our internal RGBA.
inline bool alpha_first(uint32_t info) {
    uint32_t a = info & 0x1f;
    return a == 2 || a == 4 || a == 6;  // PremultipliedFirst, First, NoneSkipFirst
}
inline bool byte_order_little(uint32_t info) { return (info & 0x7000) == 0x2000; }
inline bool has_alpha(uint32_t info) {
    uint32_t a = info & 0x1f;
    return a == 1 || a == 2 || a == 3 || a == 4 || a == 7;
}
inline uint32_t load_px(const CtxData* c, int x, int y) {
    const uint8_t* p = c->data + y * c->bpr + x * 4;
    uint8_t r, g, b, a;
    if (byte_order_little(c->info)) {  // memory: B G R A (first) or A B G R (last)
        if (alpha_first(c->info)) { b = p[0]; g = p[1]; r = p[2]; a = p[3]; }
        else { a = p[0]; b = p[1]; g = p[2]; r = p[3]; }
    } else {
        if (alpha_first(c->info)) { a = p[0]; r = p[1]; g = p[2]; b = p[3]; }
        else { r = p[0]; g = p[1]; b = p[2]; a = p[3]; }
    }
    if (!has_alpha(c->info)) a = 255;
    return pack(r, g, b, a);
}
inline void store_px(CtxData* c, int x, int y, uint32_t v) {
    uint8_t* p = c->data + y * c->bpr + x * 4;
    uint8_t r = v & 255, g = (v >> 8) & 255, b = (v >> 16) & 255, a = v >> 24;
    if (!has_alpha(c->info)) a = 255;
    if (byte_order_little(c->info)) {
        if (alpha_first(c->info)) { p[0] = b; p[1] = g; p[2] = r; p[3] = a; }
        else { p[0] = a; p[1] = b; p[2] = g; p[3] = r; }
    } else {
        if (alpha_first(c->info)) { p[0] = a; p[1] = r; p[2] = g; p[3] = b; }
        else { p[0] = r; p[1] = g; p[2] = b; p[3] = a; }
    }
}
inline bool fast_rgba(const CtxData* c) {
    return (c->info & 0x1f) == 1 && !byte_order_little(c->info);  // premultiplied last, big endian = RGBA bytes
}

void scale_state(GState& state, int scale) {
    state.ctm.a *= scale; state.ctm.b *= scale; state.ctm.c *= scale; state.ctm.d *= scale;
    state.ctm.tx *= scale; state.ctm.ty *= scale;
    for (int* bound : {&state.clip_x0, &state.clip_y0, &state.clip_x1, &state.clip_y1})
        *bound = (int)std::clamp<int64_t>((int64_t)*bound * scale, INT32_MIN, INT32_MAX);
    state.shadow.dx *= scale; state.shadow.dy *= scale; state.shadow.blur *= scale;
}
void high_resolution_context(CtxData* c) {
    int scale = hle::config().text_scale;
    if (c->scale > 1 || scale <= 1 || c->w <= 0 || c->h <= 0 || c->w > 1024 || c->h > 1024 || c->bpr < size_t(c->w) * 4) return;
    auto* physical = (uint8_t*)malloc(size_t(c->w) * c->h * scale * scale * 4);
    if (!physical) return;
    c->logical_w = c->w; c->logical_h = c->h;
    c->logical_data = c->data; c->logical_bpr = c->bpr; c->logical_owns = c->owns;
    c->scale = scale;
    c->w *= scale; c->h *= scale; c->bpr = size_t(c->w) * 4;
    c->data = physical; c->owns = true;
    for (int y = 0; y < c->h; ++y) for (int x = 0; x < c->w; ++x)
        memcpy(c->data + y * c->bpr + x * 4, c->logical_data + (y / scale) * c->logical_bpr + (x / scale) * 4, 4);
    scale_state(c->gs, scale);
    for (auto& state : c->stack) scale_state(state, scale);
    for (auto& polygon : c->path) for (auto& point : polygon) { point.x *= scale; point.y *= scale; }
    static int logs = 0;
    if (logs++ < 8) LOGI("HD text bitmap: %dx%d -> %dx%d", c->logical_w, c->logical_h, c->w, c->h);
}
void sync_high_resolution(CtxData* c) {
    if (c->scale <= 1) return;
    CtxData logical;
    logical.data = c->logical_data; logical.bpr = c->logical_bpr; logical.info = c->info;
    std::vector<uint32_t> small(size_t(c->logical_w) * c->logical_h), large(size_t(c->w) * c->h);
    for (int y = 0; y < c->h; ++y) for (int x = 0; x < c->w; ++x) large[size_t(y) * c->w + x] = load_px(c, x, y);
    int count = c->scale * c->scale;
    for (int y = 0; y < c->logical_h; ++y) for (int x = 0; x < c->logical_w; ++x) {
        int rgba[4]{};
        for (int dy = 0; dy < c->scale; ++dy) for (int dx = 0; dx < c->scale; ++dx) {
            uint32_t value = large[size_t(y * c->scale + dy) * c->w + x * c->scale + dx];
            for (int channel = 0; channel < 4; ++channel) rgba[channel] += (value >> (channel * 8)) & 255;
        }
        uint32_t value = pack((rgba[0] + count / 2) / count, (rgba[1] + count / 2) / count,
                              (rgba[2] + count / 2) / count, (rgba[3] + count / 2) / count);
        store_px(&logical, x, y, value);
        small[size_t(y) * c->logical_w + x] = value;
    }
    hd::publish_bitmap(c->logical_w, c->logical_h, c->scale, (uint8_t*)small.data(), (uint8_t*)large.data());
}
inline void put(CtxData* c, int x, int y, uint32_t src, int mode) {
    if (fast_rgba(c)) {
        uint32_t* p = (uint32_t*)(c->data + y * c->bpr) + x;
        *p = blend_px(mode, src, *p);
    } else {
        store_px(c, x, y, blend_px(mode, src, load_px(c, x, y)));
    }
}

void clip_bounds(const CtxData* c, int& x0, int& y0, int& x1, int& y1) {
    x0 = std::max({x0, 0, c->gs.clip_x0});
    y0 = std::max({y0, 0, c->gs.clip_y0});
    x1 = std::min({x1, c->w, c->gs.clip_x1});
    y1 = std::min({y1, c->h, c->gs.clip_y1});
}

// Composite a coverage mask (memory coords, origin mx,my) with a solid colour.
void composite_mask(CtxData* c, const std::vector<uint8_t>& mask, int mw, int mh, int mx, int my, uint32_t color) {
    int x0 = mx, y0 = my, x1 = mx + mw, y1 = my + mh;
    clip_bounds(c, x0, y0, x1, y1);
    int mode = c->gs.blend;
    for (int y = y0; y < y1; y++) {
        const uint8_t* m = &mask[(y - my) * mw];
        for (int x = x0; x < x1; x++) {
            int cov = m[x - mx];
            if (!cov) continue;
            put(c, x, y, scale_px(color, cov), mode);
        }
    }
}

void box_blur(std::vector<uint8_t>& m, int w, int h, int r) {
    if (r <= 0) return;
    std::vector<uint8_t> tmp(m.size());
    for (int pass = 0; pass < 2; pass++) {
        for (int y = 0; y < h; y++) {
            int acc = 0;
            for (int x = -r; x <= r; x++) acc += m[y * w + std::clamp(x, 0, w - 1)];
            for (int x = 0; x < w; x++) {
                tmp[y * w + x] = (uint8_t)(acc / (2 * r + 1));
                acc += m[y * w + std::min(x + r + 1, w - 1)] - m[y * w + std::max(x - r, 0)];
            }
        }
        for (int x = 0; x < w; x++) {
            int acc = 0;
            for (int y = -r; y <= r; y++) acc += tmp[std::clamp(y, 0, h - 1) * w + x];
            for (int y = 0; y < h; y++) {
                m[y * w + x] = (uint8_t)(acc / (2 * r + 1));
                acc += tmp[std::min(y + r + 1, h - 1) * w + x] - tmp[std::max(y - r, 0) * w + x];
            }
        }
    }
}

// Draw a coverage mask with the current shadow (if any) then the colour.
void draw_mask_with_shadow(CtxData* c, std::vector<uint8_t> mask, int mw, int mh, int mx, int my, uint32_t color) {
    if (c->gs.shadow.on && c->gs.shadow.rgba[3] > 0) {
        int r = (int)std::ceil(c->gs.shadow.blur / 2);
        int pw = mw + 2 * r, ph = mh + 2 * r;
        std::vector<uint8_t> sm(pw * ph, 0);
        for (int y = 0; y < mh; y++) memcpy(&sm[(y + r) * pw + r], &mask[y * mw], mw);
        box_blur(sm, pw, ph, r / 2 + (r > 0 ? 1 : 0));
        int sx = mx - r + (int)std::lround(c->gs.shadow.dx);
        int sy = my - r - (int)std::lround(c->gs.shadow.dy);  // base space y is up
        composite_mask(c, sm, pw, ph, sx, sy, premul_color(c->gs.shadow.rgba));
    }
    composite_mask(c, mask, mw, mh, mx, my, color);
}

// --- polygon rasteriser (non-zero winding, 4 sub-scanlines) -----------------------
struct Edge {
    double x0, y0, x1, y1;
    int dir;
};
void rasterize(CtxData* c, const std::vector<std::vector<PathPt>>& polys, uint32_t color) {
    // polys in device space (y up) -> memory space (y down)
    std::vector<Edge> edges;
    double minx = 1e18, miny = 1e18, maxx = -1e18, maxy = -1e18;
    for (auto& poly : polys) {
        if (poly.size() < 2) continue;
        for (size_t i = 0; i < poly.size(); i++) {
            PathPt p = poly[i], q = poly[(i + 1) % poly.size()];
            double y0 = c->h - p.y, y1 = c->h - q.y;
            minx = std::min({minx, p.x, q.x});
            maxx = std::max({maxx, p.x, q.x});
            miny = std::min({miny, y0, y1});
            maxy = std::max({maxy, y0, y1});
            if (y0 == y1) continue;
            if (y0 < y1) edges.push_back({p.x, y0, q.x, y1, 1});
            else edges.push_back({q.x, y1, p.x, y0, -1});
        }
    }
    if (edges.empty()) return;
    int bx0 = (int)std::floor(minx), by0 = (int)std::floor(miny), bx1 = (int)std::ceil(maxx) + 1, by1 = (int)std::ceil(maxy) + 1;
    clip_bounds(c, bx0, by0, bx1, by1);
    if (bx0 >= bx1 || by0 >= by1) return;
    int mw = bx1 - bx0, mh = by1 - by0;
    std::vector<uint16_t> acc(mw * mh, 0);
    const int SUB = 4;
    std::vector<std::pair<double, int>> xs;
    for (int row = by0; row < by1; row++) {
        for (int s = 0; s < SUB; s++) {
            double sy = row + (s + 0.5) / SUB;
            xs.clear();
            for (auto& e : edges) {
                if (sy < e.y0 || sy >= e.y1) continue;
                double t = (sy - e.y0) / (e.y1 - e.y0);
                xs.push_back({e.x0 + t * (e.x1 - e.x0), e.dir});
            }
            if (xs.size() < 2) continue;
            std::sort(xs.begin(), xs.end());
            int wind = 0;
            for (size_t i = 0; i + 1 < xs.size(); i++) {
                wind += xs[i].second;
                if (wind == 0) continue;
                double xa = std::max(xs[i].first, (double)bx0), xb = std::min(xs[i + 1].first, (double)bx1);
                if (xb <= xa) continue;
                // accumulate fractional horizontal coverage (scaled to 64 per sub-scanline)
                int ia = (int)std::floor(xa), ib = (int)std::floor(xb);
                uint16_t* line = &acc[(row - by0) * mw];
                if (ia == ib) {
                    line[ia - bx0] += (uint16_t)std::lround((xb - xa) * 64);
                } else {
                    line[ia - bx0] += (uint16_t)std::lround((ia + 1 - xa) * 64);
                    for (int x = ia + 1; x < ib; x++) line[x - bx0] += 64;
                    if (ib < bx1) line[ib - bx0] += (uint16_t)std::lround((xb - ib) * 64);
                }
            }
        }
    }
    std::vector<uint8_t> mask(mw * mh);
    for (size_t i = 0; i < acc.size(); i++) mask[i] = (uint8_t)std::min(255, acc[i] * 255 / (64 * SUB));
    draw_mask_with_shadow(c, std::move(mask), mw, mh, bx0, by0, color);
}

void flatten_cubic(std::vector<PathPt>& out, PathPt p0, PathPt p1, PathPt p2, PathPt p3) {
    double len = std::hypot(p1.x - p0.x, p1.y - p0.y) + std::hypot(p2.x - p1.x, p2.y - p1.y) + std::hypot(p3.x - p2.x, p3.y - p2.y);
    int n = std::clamp((int)(len / 3), 4, 64);
    for (int i = 1; i <= n; i++) {
        double t = (double)i / n, u = 1 - t;
        double a = u * u * u, b = 3 * u * u * t, cc = 3 * u * t * t, d = t * t * t;
        out.push_back({a * p0.x + b * p1.x + cc * p2.x + d * p3.x, a * p0.y + b * p1.y + cc * p2.y + d * p3.y});
    }
}

void stroke_polyline(CtxData* c, const std::vector<PathPt>& pts, bool closed, double width, uint32_t color) {
    std::vector<std::vector<PathPt>> polys;
    double hw = std::max(width, 1.0) / 2;
    size_t n = pts.size();
    for (size_t i = 0; i + 1 < n + (closed ? 1 : 0); i++) {
        PathPt p = pts[i], q = pts[(i + 1) % n];
        double dx = q.x - p.x, dy = q.y - p.y, l = std::hypot(dx, dy);
        if (l < 1e-9) continue;
        double nx = -dy / l * hw, ny = dx / l * hw;
        // extend by hw for square joins/caps
        double ex = dx / l * hw, ey = dy / l * hw;
        polys.push_back({{p.x - ex + nx, p.y - ey + ny}, {q.x + ex + nx, q.y + ey + ny},
                         {q.x + ex - nx, q.y + ey - ny}, {p.x - ex - nx, p.y - ey - ny}});
    }
    rasterize(c, polys, color);
}

// --- images ----------------------------------------------------------------------
id new_image(int w, int h) {
    id o = objc::alloc_instance(g_CGImage);
    auto* d = new ImageData();
    d->w = w;
    d->h = h;
    d->px.resize((size_t)w * h);
    ((ImageObj*)o)->d = d;
    return o;
}

id decode_image(const uint8_t* bytes, size_t n) {
    bool cgbi = n > 16 && !memcmp(bytes + 12, "CgBI", 4);
    int w, h, comp;
    stbi_uc* px = stbi_load_from_memory(bytes, (int)n, &w, &h, &comp, 4);
    if (!px) {
        LOGW("image decode failed: %s", stbi_failure_reason());
        return nil;
    }
    id img = new_image(w, h);
    auto& out = IMG(img)->px;
    for (int i = 0; i < w * h; i++) {
        uint8_t* p = px + i * 4;
        if (cgbi) out[i] = pack(p[0], p[1], p[2], p[3]);  // already premultiplied
        else {
            int a = p[3];
            out[i] = pack((p[0] * a + 127) / 255, (p[1] * a + 127) / 255, (p[2] * a + 127) / 255, a);
        }
    }
    stbi_image_free(px);
    return img;
}

void draw_image_impl(CtxData* c, ImageData* img, CGRect r) {
    if (!img || img->w == 0 || img->h == 0 || r.size.width == 0 || r.size.height == 0) return;
    // image pixel (u,v) [v down] -> user -> device(y up) -> memory(y down)
    Affine i2u;
    i2u.a = r.size.width / img->w;
    i2u.d = -r.size.height / img->h;
    i2u.tx = r.origin.x;
    i2u.ty = r.origin.y + r.size.height;
    Affine dev2mem;
    dev2mem.d = -1;
    dev2mem.ty = c->h;
    Affine m = i2u.then(c->gs.ctm).then(dev2mem);
    Affine inv;
    if (!m.invert(inv)) return;
    // bounding box in memory space
    double xs[4], ys[4];
    m.apply(0, 0, xs[0], ys[0]);
    m.apply(img->w, 0, xs[1], ys[1]);
    m.apply(0, img->h, xs[2], ys[2]);
    m.apply(img->w, img->h, xs[3], ys[3]);
    int x0 = (int)std::floor(*std::min_element(xs, xs + 4) + 1e-6), x1 = (int)std::ceil(*std::max_element(xs, xs + 4) - 1e-6);
    int y0 = (int)std::floor(*std::min_element(ys, ys + 4) + 1e-6), y1 = (int)std::ceil(*std::max_element(ys, ys + 4) - 1e-6);
    clip_bounds(c, x0, y0, x1, y1);
    if (x0 >= x1 || y0 >= y1) return;
    int mode = c->gs.blend;
    bool axis = std::fabs(m.b) < 1e-9 && std::fabs(m.c) < 1e-9;
    bool unit = axis && std::fabs(std::fabs(m.a) - 1) < 1e-9 && std::fabs(std::fabs(m.d) - 1) < 1e-9;
    if (c->gs.shadow.on) {
        // Build an alpha mask for the shadow, then draw normally.
        int mw = x1 - x0, mh = y1 - y0;
        std::vector<uint8_t> mask(mw * mh, 0);
        for (int y = y0; y < y1; y++)
            for (int x = x0; x < x1; x++) {
                double u, v;
                inv.apply(x + 0.5, y + 0.5, u, v);
                int iu = (int)std::floor(u), iv = (int)std::floor(v);
                if (iu < 0 || iv < 0 || iu >= img->w || iv >= img->h) continue;
                mask[(y - y0) * mw + (x - x0)] = img->px[iv * img->w + iu] >> 24;
            }
        Shadow saved = c->gs.shadow;
        int rr = (int)std::ceil(saved.blur / 2);
        int pw = mw + 2 * rr, ph = mh + 2 * rr;
        std::vector<uint8_t> sm(pw * ph, 0);
        for (int y = 0; y < mh; y++) memcpy(&sm[(y + rr) * pw + rr], &mask[y * mw], mw);
        box_blur(sm, pw, ph, rr / 2 + (rr > 0 ? 1 : 0));
        composite_mask(c, sm, pw, ph, x0 - rr + (int)std::lround(saved.dx), y0 - rr - (int)std::lround(saved.dy),
                       premul_color(saved.rgba));
    }
    if (unit) {
        for (int y = y0; y < y1; y++) {
            double u0, v;
            inv.apply(x0 + 0.5, y + 0.5, u0, v);
            int iv = (int)std::floor(v);
            if (iv < 0 || iv >= img->h) continue;
            const uint32_t* row = &img->px[iv * img->w];
            int step = m.a > 0 ? 1 : -1;
            int iu = (int)std::floor(u0);
            for (int x = x0; x < x1; x++, iu += step) {
                if (iu < 0 || iu >= img->w) continue;
                uint32_t s = row[iu];
                if (mode == 0 && (s >> 24) == 0) continue;
                put(c, x, y, s, mode);
            }
        }
        return;
    }
    bool bilinear = c->gs.interpolate;
    for (int y = y0; y < y1; y++) {
        for (int x = x0; x < x1; x++) {
            double u, v;
            inv.apply(x + 0.5, y + 0.5, u, v);
            if (u < 0 || v < 0 || u >= img->w || v >= img->h) continue;
            uint32_t s;
            if (!bilinear) {
                s = img->px[(int)v * img->w + (int)u];
            } else {
                double fu = u - 0.5, fv = v - 0.5;
                int iu = (int)std::floor(fu), iv = (int)std::floor(fv);
                double tu = fu - iu, tv = fv - iv;
                auto at = [&](int xx, int yy) {
                    xx = std::clamp(xx, 0, img->w - 1);
                    yy = std::clamp(yy, 0, img->h - 1);
                    return img->px[yy * img->w + xx];
                };
                uint32_t p00 = at(iu, iv), p10 = at(iu + 1, iv), p01 = at(iu, iv + 1), p11 = at(iu + 1, iv + 1);
                s = 0;
                for (int k = 0; k < 32; k += 8) {
                    double val = ((p00 >> k) & 255) * (1 - tu) * (1 - tv) + ((p10 >> k) & 255) * tu * (1 - tv) +
                                 ((p01 >> k) & 255) * (1 - tu) * tv + ((p11 >> k) & 255) * tu * tv;
                    s |= (uint32_t)std::lround(val) << k;
                }
            }
            if (mode == 0 && (s >> 24) == 0) continue;
            put(c, x, y, s, mode);
        }
    }
}

// --- text (JNI) -------------------------------------------------------------------
std::mutex g_jni_mutex;
jclass g_text_cls = nullptr;
jmethodID g_measure = nullptr, g_render = nullptr;

bool jni_ready(JNIEnv*& env) {
    env = (JNIEnv*)SDL_AndroidGetJNIEnv();
    if (!env) return false;
    if (g_text_cls) return true;
    jobject activity = (jobject)SDL_AndroidGetActivity();
    jclass act_cls = env->GetObjectClass(activity);
    jmethodID get_loader = env->GetMethodID(act_cls, "getClassLoader", "()Ljava/lang/ClassLoader;");
    jobject loader = env->CallObjectMethod(activity, get_loader);
    jclass loader_cls = env->FindClass("java/lang/ClassLoader");
    jmethodID load_class = env->GetMethodID(loader_cls, "loadClass", "(Ljava/lang/String;)Ljava/lang/Class;");
    jstring name = env->NewStringUTF("com.ts3wa.port.TextRenderer");
    jclass cls = (jclass)env->CallObjectMethod(loader, load_class, name);
    if (env->ExceptionCheck() || !cls) {
        env->ExceptionClear();
        LOGE("TextRenderer class not found");
        return false;
    }
    g_text_cls = (jclass)env->NewGlobalRef(cls);
    g_measure = env->GetStaticMethodID(g_text_cls, "measure", "(Ljava/lang/String;FZ)[F");
    g_render = env->GetStaticMethodID(g_text_cls, "renderScaled", "(Ljava/lang/String;FZI)[B");
    env->DeleteLocalRef(name);
    env->DeleteLocalRef(cls);
    env->DeleteLocalRef(loader);
    env->DeleteLocalRef(act_cls);
    env->DeleteLocalRef(activity);
    return true;
}

jstring to_jstring(JNIEnv* env, const std::u16string& s) { return env->NewString((const jchar*)s.data(), (jsize)s.size()); }

struct TextMask {
    int w = 0, h = 0, ascent = 0;
    std::vector<uint8_t> px;
};
bool render_text(const std::u16string& s, FontDesc f, TextMask& out, int scale) {
    std::lock_guard<std::mutex> lk(g_jni_mutex);
    JNIEnv* env;
    if (!jni_ready(env)) return false;
    jstring js = to_jstring(env, s);
    auto arr = (jbyteArray)env->CallStaticObjectMethod(g_text_cls, g_render, js, (jfloat)f.size, (jboolean)f.bold, (jint)scale);
    env->DeleteLocalRef(js);
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        LOGW("TextRenderer could not rasterize a text bitmap");
        if (arr) env->DeleteLocalRef(arr);
        return false;
    }
    if (!arr) return false;
    jsize n = env->GetArrayLength(arr);
    if (n < 16) { env->DeleteLocalRef(arr); return false; }
    std::vector<uint8_t> buf(n);
    env->GetByteArrayRegion(arr, 0, n, (jbyte*)buf.data());
    env->DeleteLocalRef(arr);
    int32_t hdr[4];
    memcpy(hdr, buf.data(), 16);
    if (hdr[0] <= 0 || hdr[1] <= 0 || hdr[2] < hdr[0] ||
        uint64_t(hdr[2]) * hdr[1] > size_t(n - 16)) return false;
    out.w = hdr[0];
    out.h = hdr[1];
    out.ascent = hdr[3];
    int rb = hdr[2];
    out.px.resize(out.w * out.h);
    for (int y = 0; y < out.h; y++) memcpy(&out.px[y * out.w], &buf[16 + y * rb], out.w);
    return true;
}

// --- UIGraphics context stack ------------------------------------------------------
thread_local std::vector<id> t_ctx_stack;
thread_local std::vector<id> t_image_ctx;  // contexts created by UIGraphicsBeginImageContext

}  // namespace

// =====================================================================================
// public helpers
// =====================================================================================
id image_from_file(const std::string& path) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return nil;
    std::vector<uint8_t> data;
    uint8_t buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) data.insert(data.end(), buf, buf + n);
    fclose(f);
    return decode_image(data.data(), data.size());
}
id image_from_rgba(const uint8_t* rgba, int w, int h) {
    id img = new_image(w, h);
    memcpy(IMG(img)->px.data(), rgba, (size_t)w * h * 4);
    return img;
}
int image_width(id img) { return img ? (IMG(img)->logical_w ? IMG(img)->logical_w : IMG(img)->w) : 0; }
int image_height(id img) { return img ? (IMG(img)->logical_h ? IMG(img)->logical_h : IMG(img)->h) : 0; }

id current_context() { return t_ctx_stack.empty() ? nil : t_ctx_stack.back(); }
void push_context(id ctx) { t_ctx_stack.push_back(ctx); }
void pop_context() {
    if (!t_ctx_stack.empty()) t_ctx_stack.pop_back();
}

id new_context(void* data, int w, int h, size_t bpr, uint32_t info) {
    id o = objc::alloc_instance(g_CGContext);
    auto* d = new CtxData();
    d->w = w;
    d->h = h;
    d->bpr = bpr ? bpr : (size_t)w * 4;
    d->info = info;
    if (data) d->data = (uint8_t*)data;
    else {
        d->data = (uint8_t*)calloc(d->bpr, h);
        d->owns = true;
    }
    d->gs.clip_x1 = w;
    d->gs.clip_y1 = h;
    ((CtxObj*)o)->d = d;
    return o;
}

id begin_image_context(double w, double h, double scale) {
    int pw = std::max(1, (int)std::ceil(w * scale)), ph = std::max(1, (int)std::ceil(h * scale));
    id ctx = new_context(nullptr, pw, ph, 0, 1);
    // UIKit contexts are flipped (origin top-left, y down) and scaled.
    Affine& m = CTX(ctx)->gs.ctm;
    m.a = scale;
    m.d = -scale;
    m.ty = ph;
    t_image_ctx.push_back(ctx);
    push_context(ctx);
    return ctx;
}
id image_from_current_context() {
    id ctx = current_context();
    if (!ctx) return nil;
    CtxData* c = CTX(ctx);
    sync_high_resolution(c);
    id img = new_image(c->w, c->h);
    IMG(img)->logical_w = c->logical_w; IMG(img)->logical_h = c->logical_h;
    for (int y = 0; y < c->h; y++)
        for (int x = 0; x < c->w; x++) IMG(img)->px[y * c->w + x] = load_px(c, x, y);
    return img;
}
void end_image_context() {
    if (t_image_ctx.empty()) return;
    id ctx = t_image_ctx.back();
    t_image_ctx.pop_back();
    for (size_t i = t_ctx_stack.size(); i-- > 0;)
        if (t_ctx_stack[i] == ctx) {
            t_ctx_stack.erase(t_ctx_stack.begin() + i);
            break;
        }
    objc::release(ctx);
}

void draw_image(id ctx, id cgimage, CGRect rect) {
    if (ctx && cgimage) { draw_image_impl(CTX(ctx), IMG(cgimage), rect); sync_high_resolution(CTX(ctx)); }
}

CGSize measure_text(const std::u16string& s, FontDesc f) {
    if (s.empty()) return {0, f.size * 1.2};
    std::lock_guard<std::mutex> lk(g_jni_mutex);
    JNIEnv* env;
    if (!jni_ready(env)) return {s.size() * f.size * 0.5, f.size * 1.2};
    jstring js = to_jstring(env, s);
    auto arr = (jfloatArray)env->CallStaticObjectMethod(g_text_cls, g_measure, js, (jfloat)f.size, (jboolean)f.bold);
    env->DeleteLocalRef(js);
    float v[3] = {0, 0, 0};
    if (arr) {
        env->GetFloatArrayRegion(arr, 0, 3, v);
        env->DeleteLocalRef(arr);
    }
    return {std::ceil(v[0]), std::ceil(v[1])};
}

void draw_text(id ctx, const std::u16string& s, CGPoint origin, FontDesc f) {
    if (!ctx || s.empty()) return;
    CtxData* c = CTX(ctx);
    high_resolution_context(c);
    TextMask tm;
    if (!render_text(s, f, tm, c->scale)) return;
    // text box (y down from origin) -> user space -> device -> memory
    Affine dev2mem;
    dev2mem.d = -1;
    dev2mem.ty = c->h;
    Affine t2u;
    t2u.a = t2u.d = 1.0 / c->scale;
    t2u.tx = origin.x;
    t2u.ty = origin.y;
    Affine m = t2u.then(c->gs.ctm).then(dev2mem);
    Affine inv;
    if (!m.invert(inv)) return;
    double xs[4], ys[4];
    m.apply(0, 0, xs[0], ys[0]);
    m.apply(tm.w, 0, xs[1], ys[1]);
    m.apply(0, tm.h, xs[2], ys[2]);
    m.apply(tm.w, tm.h, xs[3], ys[3]);
    int x0 = (int)std::floor(*std::min_element(xs, xs + 4)), x1 = (int)std::ceil(*std::max_element(xs, xs + 4));
    int y0 = (int)std::floor(*std::min_element(ys, ys + 4)), y1 = (int)std::ceil(*std::max_element(ys, ys + 4));
    int mw = x1 - x0, mh = y1 - y0;
    if (mw <= 0 || mh <= 0) return;
    std::vector<uint8_t> mask(mw * mh, 0);
    for (int y = 0; y < mh; y++)
        for (int x = 0; x < mw; x++) {
            double u, v;
            inv.apply(x0 + x + 0.5, y0 + y + 0.5, u, v);
            int iu = (int)std::floor(u), iv = (int)std::floor(v);
            if (iu < 0 || iv < 0 || iu >= tm.w || iv >= tm.h) continue;
            mask[y * mw + x] = tm.px[iv * tm.w + iu];
        }
    const double* col = c->gs.fill;
    if (c->gs.text_mode == 1 || c->gs.text_mode == 2) {
        // stroke: dilate the glyph coverage by half the line width
        int r = std::max(1, (int)std::lround(c->gs.line_width * c->scale / 2));
        int pw = mw + 2 * r, ph = mh + 2 * r;
        std::vector<uint8_t> dil(pw * ph, 0);
        for (int y = 0; y < mh; y++)
            for (int x = 0; x < mw; x++) {
                uint8_t v = mask[y * mw + x];
                if (!v) continue;
                for (int dy = -r; dy <= r; dy++)
                    for (int dx = -r; dx <= r; dx++) {
                        uint8_t& d = dil[(y + r + dy) * pw + (x + r + dx)];
                        d = std::max(d, v);
                    }
            }
        draw_mask_with_shadow(c, std::move(dil), pw, ph, x0 - r, y0 - r, premul_color(c->gs.stroke));
        if (c->gs.text_mode == 1) { sync_high_resolution(c); return; }
        Shadow saved = c->gs.shadow;
        c->gs.shadow.on = false;
        composite_mask(c, mask, mw, mh, x0, y0, premul_color(col));
        c->gs.shadow = saved;
        sync_high_resolution(c);
        return;
    }
    draw_mask_with_shadow(c, std::move(mask), mw, mh, x0, y0, premul_color(col));
    sync_high_resolution(c);
}

}  // namespace cg

// =====================================================================================
// exported C API
// =====================================================================================
using namespace cg;

namespace {

void Ctx_dealloc(id self, SEL) {
    if (auto* d = CTX(self)) {
        sync_high_resolution(d);
        if (d->logical_owns) free(d->logical_data);
        if (d->owns) free(d->data);
        delete d;
    }
    objc::dispose_instance(self);
}
void Img_dealloc(id self, SEL) {
    delete IMG(self);
    objc::dispose_instance(self);
}
void Prov_dealloc(id self, SEL) {
    auto* p = (ProviderObj*)self;
    if (p->release_info) p->release_info(p->info);
    delete p->bytes;
    objc::dispose_instance(self);
}

id c_CGColorSpaceCreateDeviceRGB() { return objc::alloc_instance(g_CGColorSpace); }
void c_CGColorSpaceRelease(id s) { objc::release(s); }
id c_CGColorCreate(id, const CGFloat* comps) {
    id o = objc::alloc_instance(g_CGColor);
    for (int i = 0; i < 4; ++i) ((ColorObj*)o)->rgba[i] = comps[i];
    return o;
}
void c_CGColorRelease(id c) { objc::release(c); }

id c_CGBitmapContextCreate(void* data, size_t w, size_t h, size_t bpc, size_t bpr, id, uint32_t info) {
    if (bpc != 8 || (bpr && bpr < w * 4)) LOGW("CGBitmapContextCreate: unsupported bpc=%zu bpr=%zu w=%zu", bpc, bpr, w);
    return new_context(data, (int)w, (int)h, bpr, info);
}
id c_CGBitmapContextCreateImage(id ctx) {
    CtxData* c = CTX(ctx);
    sync_high_resolution(c);
    id img = new_image(c->w, c->h);
    IMG(img)->logical_w = c->logical_w; IMG(img)->logical_h = c->logical_h;
    auto& px = IMG(img)->px;
    if (fast_rgba(c)) {
        for (int y = 0; y < c->h; y++) memcpy(&px[y * c->w], c->data + y * c->bpr, c->w * 4);
    } else {
        for (int y = 0; y < c->h; y++)
            for (int x = 0; x < c->w; x++) px[y * c->w + x] = load_px(c, x, y);
    }
    return img;
}
id c_CGContextRetain(id c) { return objc::retain(c); }
void c_CGContextRelease(id c) { objc::release(c); }
void c_CGContextSaveGState(id c) { CTX(c)->stack.push_back(CTX(c)->gs); }
void c_CGContextRestoreGState(id c) {
    auto* d = CTX(c);
    if (!d->stack.empty()) {
        d->gs = d->stack.back();
        d->stack.pop_back();
    }
}
void c_CGContextTranslateCTM(id c, CGFloat tx, CGFloat ty) {
    Affine t;
    t.tx = tx;
    t.ty = ty;
    CTX(c)->gs.ctm = t.then(CTX(c)->gs.ctm);
}
void c_CGContextScaleCTM(id c, CGFloat sx, CGFloat sy) {
    Affine t;
    t.a = sx;
    t.d = sy;
    CTX(c)->gs.ctm = t.then(CTX(c)->gs.ctm);
}
void c_CGContextRotateCTM(id c, CGFloat ang) {
    Affine t;
    t.a = std::cos(ang);
    t.b = std::sin(ang);
    t.c = -std::sin(ang);
    t.d = std::cos(ang);
    CTX(c)->gs.ctm = t.then(CTX(c)->gs.ctm);
}
void c_CGContextSetRGBFillColor(id c, CGFloat r, CGFloat g, CGFloat b, CGFloat a) {
    double* f = CTX(c)->gs.fill;
    f[0] = r;
    f[1] = g;
    f[2] = b;
    f[3] = a;
}
void c_CGContextSetRGBStrokeColor(id c, CGFloat r, CGFloat g, CGFloat b, CGFloat a) {
    double* f = CTX(c)->gs.stroke;
    f[0] = r;
    f[1] = g;
    f[2] = b;
    f[3] = a;
}
void c_CGContextSetLineWidth(id c, CGFloat w) { CTX(c)->gs.line_width = w; }
void c_CGContextSetBlendMode(id c, int mode) { CTX(c)->gs.blend = mode; }
void c_CGContextSetAllowsAntialiasing(id, bool) {}
void c_CGContextSetInterpolationQuality(id c, int q) { CTX(c)->gs.interpolate = q != 1; }
void c_CGContextSetTextDrawingMode(id c, int m) { CTX(c)->gs.text_mode = m; }
void c_CGContextSetShadowWithColor(id c, CGSize off, CGFloat blur, id color) {
    Shadow& s = CTX(c)->gs.shadow;
    s.on = color != nil || blur > 0 || off.width != 0 || off.height != 0;
    s.dx = off.width * CTX(c)->scale;
    s.dy = off.height * CTX(c)->scale;
    s.blur = blur * CTX(c)->scale;
    if (color && objc::is_kind_of(color, g_CGColor)) memcpy(s.rgba, ((ColorObj*)color)->rgba, sizeof s.rgba);
    else if (color) uikit::color_components(color, s.rgba);
    else {
        s.rgba[0] = s.rgba[1] = s.rgba[2] = 0;
        s.rgba[3] = 1.0 / 3;
    }
    if (!color) s.on = false;
}
void c_CGContextClipToRect(id c, CGRect r) {
    auto* d = CTX(c);
    double xs[4], ys[4];
    const Affine& m = d->gs.ctm;
    m.apply(r.origin.x, r.origin.y, xs[0], ys[0]);
    m.apply(r.origin.x + r.size.width, r.origin.y, xs[1], ys[1]);
    m.apply(r.origin.x, r.origin.y + r.size.height, xs[2], ys[2]);
    m.apply(r.origin.x + r.size.width, r.origin.y + r.size.height, xs[3], ys[3]);
    int x0 = (int)std::lround(*std::min_element(xs, xs + 4)), x1 = (int)std::lround(*std::max_element(xs, xs + 4));
    int y0 = d->h - (int)std::lround(*std::max_element(ys, ys + 4)), y1 = d->h - (int)std::lround(*std::min_element(ys, ys + 4));
    d->gs.clip_x0 = std::max(d->gs.clip_x0, x0);
    d->gs.clip_y0 = std::max(d->gs.clip_y0, y0);
    d->gs.clip_x1 = std::min(d->gs.clip_x1, x1);
    d->gs.clip_y1 = std::min(d->gs.clip_y1, y1);
}
void c_CGContextBeginPath(id c) { CTX(c)->path.clear(); }
void c_CGContextMoveToPoint(id c, CGFloat x, CGFloat y) {
    PathPt p;
    CTX(c)->gs.ctm.apply(x, y, p.x, p.y);
    CTX(c)->path.push_back({p});
}
void c_CGContextAddLineToPoint(id c, CGFloat x, CGFloat y) {
    auto& path = CTX(c)->path;
    PathPt p;
    CTX(c)->gs.ctm.apply(x, y, p.x, p.y);
    if (path.empty()) path.push_back({});
    path.back().push_back(p);
}
void c_CGContextAddCurveToPoint(id c, CGFloat c1x, CGFloat c1y, CGFloat c2x, CGFloat c2y, CGFloat x, CGFloat y) {
    auto& path = CTX(c)->path;
    const Affine& m = CTX(c)->gs.ctm;
    PathPt p1, p2, p3;
    m.apply(c1x, c1y, p1.x, p1.y);
    m.apply(c2x, c2y, p2.x, p2.y);
    m.apply(x, y, p3.x, p3.y);
    if (path.empty() || path.back().empty()) {
        path.push_back({p1});
    }
    PathPt p0 = path.back().back();
    flatten_cubic(path.back(), p0, p1, p2, p3);
}
void c_CGContextClosePath(id) {}
void c_CGContextFillPath(id c) {
    auto* d = CTX(c);
    rasterize(d, d->path, premul_color(d->gs.fill));
    d->path.clear();
    sync_high_resolution(d);
}
void c_CGContextStrokePath(id c) {
    auto* d = CTX(c);
    double lw = d->gs.line_width * std::sqrt(std::fabs(d->gs.ctm.a * d->gs.ctm.d - d->gs.ctm.b * d->gs.ctm.c));
    for (auto& sub : d->path) stroke_polyline(d, sub, false, lw, premul_color(d->gs.stroke));
    d->path.clear();
    sync_high_resolution(d);
}
void c_CGContextFillRect(id c, CGRect r) {
    auto* d = CTX(c);
    std::vector<PathPt> poly(4);
    const Affine& m = d->gs.ctm;
    m.apply(r.origin.x, r.origin.y, poly[0].x, poly[0].y);
    m.apply(r.origin.x + r.size.width, r.origin.y, poly[1].x, poly[1].y);
    m.apply(r.origin.x + r.size.width, r.origin.y + r.size.height, poly[2].x, poly[2].y);
    m.apply(r.origin.x, r.origin.y + r.size.height, poly[3].x, poly[3].y);
    rasterize(d, {poly}, premul_color(d->gs.fill));
    sync_high_resolution(d);
}
void c_CGContextStrokeRect(id c, CGRect r) {
    auto* d = CTX(c);
    std::vector<PathPt> poly(4);
    const Affine& m = d->gs.ctm;
    m.apply(r.origin.x, r.origin.y, poly[0].x, poly[0].y);
    m.apply(r.origin.x + r.size.width, r.origin.y, poly[1].x, poly[1].y);
    m.apply(r.origin.x + r.size.width, r.origin.y + r.size.height, poly[2].x, poly[2].y);
    m.apply(r.origin.x, r.origin.y + r.size.height, poly[3].x, poly[3].y);
    double lw = d->gs.line_width * std::sqrt(std::fabs(m.a * m.d - m.b * m.c));
    stroke_polyline(d, poly, true, lw, premul_color(d->gs.stroke));
    sync_high_resolution(d);
}
void c_CGContextClearRect(id c, CGRect r) {
    int saved = CTX(c)->gs.blend;
    CTX(c)->gs.blend = 16;
    c_CGContextFillRect(c, r);
    CTX(c)->gs.blend = saved;
}
void c_CGContextDrawImage(id c, CGRect r, id img) {
    if (!c || !img) return;
    draw_image_impl(CTX(c), IMG(img), r);
    sync_high_resolution(CTX(c));
}

struct SeqCallbacks {
    uint32_t version;
    size_t (*getBytes)(void* info, void* buffer, size_t count);
    int64_t (*skipForward)(void* info, int64_t count);
    void (*rewind)(void* info);
    void (*release)(void* info);
};
id c_CGDataProviderCreateSequential(void* info, const SeqCallbacks* cb) {
    id o = objc::alloc_instance(g_CGDataProvider);
    auto* bytes = new std::vector<uint8_t>();
    uint8_t buf[32768];
    for (;;) {
        size_t n = cb->getBytes(info, buf, sizeof buf);
        if (n == 0 || n > sizeof buf) break;
        bytes->insert(bytes->end(), buf, buf + n);
    }
    ((ProviderObj*)o)->bytes = bytes;
    ((ProviderObj*)o)->info = info;
    ((ProviderObj*)o)->release_info = cb->release;
    return o;
}
void c_CGDataProviderRelease(id p) { objc::release(p); }
id c_CGImageCreateWithPNGDataProvider(id provider, const CGFloat*, bool, int) {
    auto* b = ((ProviderObj*)provider)->bytes;
    return decode_image(b->data(), b->size());
}
id c_CGImageCreateWithImageInRect(id img, CGRect r) {
    ImageData* s = IMG(img);
    int scale = s->logical_w ? s->w / s->logical_w : 1;
    r.origin.x *= scale; r.origin.y *= scale; r.size.width *= scale; r.size.height *= scale;
    int x0 = std::max(0, (int)std::floor(r.origin.x)), y0 = std::max(0, (int)std::floor(r.origin.y));
    int x1 = std::min(s->w, (int)std::ceil(r.origin.x + r.size.width)), y1 = std::min(s->h, (int)std::ceil(r.origin.y + r.size.height));
    if (x1 <= x0 || y1 <= y0) return nil;
    id out = new_image(x1 - x0, y1 - y0);
    if (scale > 1) { IMG(out)->logical_w = (x1 - x0) / scale; IMG(out)->logical_h = (y1 - y0) / scale; }
    for (int y = y0; y < y1; y++) memcpy(&IMG(out)->px[(y - y0) * (x1 - x0)], &s->px[y * s->w + x0], (x1 - x0) * 4);
    return out;
}
size_t c_CGImageGetWidth(id img) { return image_width(img); }
size_t c_CGImageGetHeight(id img) { return image_height(img); }
id c_CGImageRetain(id img) { return objc::retain(img); }
void c_CGImageRelease(id img) { objc::release(img); }

struct CGAffineTransform {
    CGFloat a, b, c, d, tx, ty;
};
CGAffineTransform c_CGAffineTransformMakeTranslation(CGFloat tx, CGFloat ty) { return {1, 0, 0, 1, tx, ty}; }
CGAffineTransform c_CGAffineTransformRotate(CGAffineTransform t, CGFloat ang) {
    double cs = std::cos(ang), sn = std::sin(ang);
    // rotation applied first, then t
    return {cs * t.a + sn * t.c, cs * t.b + sn * t.d, -sn * t.a + cs * t.c, -sn * t.b + cs * t.d, t.tx, t.ty};
}

void c_UIGraphicsBeginImageContext(CGSize sz) { begin_image_context(sz.width, sz.height, 1.0); }
void c_UIGraphicsBeginImageContextWithOptions(CGSize sz, bool, CGFloat scale) {
    begin_image_context(sz.width, sz.height, scale > 0 ? scale : 1.0);
}

CGAffineTransform c_CGAffineTransformMakeRotation(CGFloat a) {
    return {std::cos(a), std::sin(a), -std::sin(a), std::cos(a), 0, 0};
}
CGAffineTransform c_CGAffineTransformScale(CGAffineTransform t, CGFloat x, CGFloat y) {
    t.a *= x; t.b *= x; t.c *= y; t.d *= y;
    return t;
}
id c_CGColorCreateGenericRGB(CGFloat r, CGFloat g, CGFloat b, CGFloat a) {
    CGFloat components[] = {r, g, b, a};
    return c_CGColorCreate(nil, components);
}
BOOL c_CGRectContainsPoint(CGRect r, CGPoint p) {
    return p.x >= r.origin.x && p.y >= r.origin.y &&
           p.x <= r.origin.x + r.size.width && p.y <= r.origin.y + r.size.height;
}
HLE_EXPORT(CGAffineTransformMakeRotation, c_CGAffineTransformMakeRotation);
HLE_EXPORT(CGAffineTransformScale, c_CGAffineTransformScale);
HLE_EXPORT(CGColorCreateGenericRGB, c_CGColorCreateGenericRGB);
HLE_EXPORT(CGRectContainsPoint, c_CGRectContainsPoint);
id c_UIGraphicsGetCurrentContext() { return current_context(); }
id c_UIGraphicsGetImageFromCurrentImageContext() {
    id cgimg = image_from_current_context();
    if (!cgimg) return nil;
    id ui = objc::send(objc::send((id)objc::get_class("UIImage"), SEL_("alloc")), SEL_("initWithCGImage:"), cgimg);
    objc::release(cgimg);
    return objc::autorelease(ui);
}
void c_UIGraphicsEndImageContext() { end_image_context(); }
void c_UIGraphicsPushContext(id ctx) { push_context(ctx); }
void c_UIGraphicsPopContext() { pop_context(); }

}  // namespace

HLE_EXPORT(CGColorSpaceCreateDeviceRGB, c_CGColorSpaceCreateDeviceRGB);
HLE_EXPORT(CGColorSpaceRelease, c_CGColorSpaceRelease);
HLE_EXPORT(CGColorCreate, c_CGColorCreate);
HLE_EXPORT(CGColorRelease, c_CGColorRelease);
HLE_EXPORT(CGBitmapContextCreate, c_CGBitmapContextCreate);
HLE_EXPORT(CGBitmapContextCreateImage, c_CGBitmapContextCreateImage);
HLE_EXPORT(CGContextRetain, c_CGContextRetain);
HLE_EXPORT(CGContextRelease, c_CGContextRelease);
HLE_EXPORT(CGContextSaveGState, c_CGContextSaveGState);
HLE_EXPORT(CGContextRestoreGState, c_CGContextRestoreGState);
HLE_EXPORT(CGContextTranslateCTM, c_CGContextTranslateCTM);
HLE_EXPORT(CGContextScaleCTM, c_CGContextScaleCTM);
HLE_EXPORT(CGContextRotateCTM, c_CGContextRotateCTM);
HLE_EXPORT(CGContextSetRGBFillColor, c_CGContextSetRGBFillColor);
HLE_EXPORT(CGContextSetRGBStrokeColor, c_CGContextSetRGBStrokeColor);
HLE_EXPORT(CGContextSetLineWidth, c_CGContextSetLineWidth);
HLE_EXPORT(CGContextSetBlendMode, c_CGContextSetBlendMode);
HLE_EXPORT(CGContextSetAllowsAntialiasing, c_CGContextSetAllowsAntialiasing);
HLE_EXPORT(CGContextSetInterpolationQuality, c_CGContextSetInterpolationQuality);
HLE_EXPORT(CGContextSetTextDrawingMode, c_CGContextSetTextDrawingMode);
HLE_EXPORT(CGContextSetShadowWithColor, c_CGContextSetShadowWithColor);
HLE_EXPORT(CGContextClipToRect, c_CGContextClipToRect);
HLE_EXPORT(CGContextBeginPath, c_CGContextBeginPath);
HLE_EXPORT(CGContextMoveToPoint, c_CGContextMoveToPoint);
HLE_EXPORT(CGContextAddLineToPoint, c_CGContextAddLineToPoint);
HLE_EXPORT(CGContextAddCurveToPoint, c_CGContextAddCurveToPoint);
HLE_EXPORT(CGContextClosePath, c_CGContextClosePath);
HLE_EXPORT(CGContextFillPath, c_CGContextFillPath);
HLE_EXPORT(CGContextStrokePath, c_CGContextStrokePath);
HLE_EXPORT(CGContextFillRect, c_CGContextFillRect);
HLE_EXPORT(CGContextStrokeRect, c_CGContextStrokeRect);
HLE_EXPORT(CGContextClearRect, c_CGContextClearRect);
HLE_EXPORT(CGContextDrawImage, c_CGContextDrawImage);
HLE_EXPORT(CGDataProviderCreateSequential, c_CGDataProviderCreateSequential);
HLE_EXPORT(CGDataProviderRelease, c_CGDataProviderRelease);
HLE_EXPORT(CGImageCreateWithPNGDataProvider, c_CGImageCreateWithPNGDataProvider);
HLE_EXPORT(CGImageCreateWithImageInRect, c_CGImageCreateWithImageInRect);
HLE_EXPORT(CGImageGetWidth, c_CGImageGetWidth);
HLE_EXPORT(CGImageGetHeight, c_CGImageGetHeight);
HLE_EXPORT(CGImageRetain, c_CGImageRetain);
HLE_EXPORT(CGImageRelease, c_CGImageRelease);
HLE_EXPORT(CGAffineTransformMakeTranslation, c_CGAffineTransformMakeTranslation);
HLE_EXPORT(CGAffineTransformRotate, c_CGAffineTransformRotate);
HLE_EXPORT(UIGraphicsBeginImageContext, c_UIGraphicsBeginImageContext);
HLE_EXPORT(UIGraphicsBeginImageContextWithOptions, c_UIGraphicsBeginImageContextWithOptions);
HLE_EXPORT(UIGraphicsGetCurrentContext, c_UIGraphicsGetCurrentContext);
HLE_EXPORT(UIGraphicsGetImageFromCurrentImageContext, c_UIGraphicsGetImageFromCurrentImageContext);
HLE_EXPORT(UIGraphicsEndImageContext, c_UIGraphicsEndImageContext);
HLE_EXPORT(UIGraphicsPushContext, c_UIGraphicsPushContext);
HLE_EXPORT(UIGraphicsPopContext, c_UIGraphicsPopContext);

namespace cg {

void init() {
    stbi_convert_iphone_png_to_rgb(1);
    g_CGImage = objc::define_class("__CGImage", "NSObject", sizeof(ImageObj));
    objc::add_method(g_CGImage, "dealloc", (void*)Img_dealloc);
    g_CGContext = objc::define_class("__CGContext", "NSObject", sizeof(CtxObj));
    objc::add_method(g_CGContext, "dealloc", (void*)Ctx_dealloc);
    g_CGColor = objc::define_class("__CGColor", "NSObject", sizeof(ColorObj));
    g_CGColorSpace = objc::define_class("__CGColorSpace", "NSObject", 8);
    g_CGDataProvider = objc::define_class("__CGDataProvider", "NSObject", sizeof(ProviderObj));
    objc::add_method(g_CGDataProvider, "dealloc", (void*)Prov_dealloc);
}

}  // namespace cg
