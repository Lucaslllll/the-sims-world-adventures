// NSString / CFString implementation (toll-free bridged).
#include "hle/nsstring.h"

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstring>
#include <cwctype>
#include <mutex>
#include <unordered_map>

#include "hle/foundation.h"
#include "hle/format.h"
#include "objc/runtime.h"

namespace ns {

namespace {

struct StrData {
    std::u16string s;
    std::string utf8;
    bool utf8_valid = false;
};

Class g_NSString, g_NSMutableString, g_CFString, g_CFConstantString;

constexpr uint64_t kInfoUTF16 = 0x7d0;

bool is_constant(id s) { return s->isa == g_CFConstantString; }
StrData* dyn(id s) { return (StrData*)((StringObj*)s)->data; }

std::mutex g_const_cache_mutex;
std::unordered_map<id, std::string>& const_utf8_cache() {
    static std::unordered_map<id, std::string> m;
    return m;
}

}  // namespace

// --- UTF conversions ------------------------------------------------------------
std::u16string utf8_to_u16(const std::string& s) {
    std::u16string out;
    out.reserve(s.size());
    size_t i = 0;
    while (i < s.size()) {
        uint32_t c = (uint8_t)s[i];
        int extra = 0;
        if (c >= 0xf0) { c &= 0x07; extra = 3; }
        else if (c >= 0xe0) { c &= 0x0f; extra = 2; }
        else if (c >= 0xc0) { c &= 0x1f; extra = 1; }
        else if (c >= 0x80) { c = 0xfffd; }
        i++;
        for (int k = 0; k < extra && i < s.size(); k++, i++) c = (c << 6) | ((uint8_t)s[i] & 0x3f);
        if (c >= 0x10000) {
            c -= 0x10000;
            out += (char16_t)(0xd800 + (c >> 10));
            out += (char16_t)(0xdc00 + (c & 0x3ff));
        } else {
            out += (char16_t)c;
        }
    }
    return out;
}

std::string u16_to_utf8(const std::u16string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); i++) {
        uint32_t c = s[i];
        if (c >= 0xd800 && c < 0xdc00 && i + 1 < s.size() && s[i + 1] >= 0xdc00 && s[i + 1] < 0xe000) {
            c = 0x10000 + ((c - 0xd800) << 10) + (s[i + 1] - 0xdc00);
            i++;
        }
        if (c < 0x80) out += (char)c;
        else if (c < 0x800) {
            out += (char)(0xc0 | (c >> 6));
            out += (char)(0x80 | (c & 0x3f));
        } else if (c < 0x10000) {
            out += (char)(0xe0 | (c >> 12));
            out += (char)(0x80 | ((c >> 6) & 0x3f));
            out += (char)(0x80 | (c & 0x3f));
        } else {
            out += (char)(0xf0 | (c >> 18));
            out += (char)(0x80 | ((c >> 12) & 0x3f));
            out += (char)(0x80 | ((c >> 6) & 0x3f));
            out += (char)(0x80 | (c & 0x3f));
        }
    }
    return out;
}

uint32_t ns_to_cf_encoding(NSUInteger e) {
    switch (e) {
        case 1: return 0x0600;       // ASCII
        case 2: return 0x0B01;       // NEXTSTEP
        case 3: return 0x0920;       // EUC-JP
        case 4: return 0x08000100;   // UTF-8
        case 5: return 0x0201;       // ISO Latin 1
        case 8: return 0x0A01;       // Shift-JIS
        case 10: return 0x0100;      // UTF-16
        case 12: return 0x0500;      // Windows-1252
        case 30: return 0;           // MacRoman
        default:
            if (e & 0x80000000) return (uint32_t)(e & 0x7fffffff);
            return (uint32_t)e;
    }
}

bool decode_bytes(const uint8_t* b, size_t n, uint32_t enc, std::u16string& out) {
    out.clear();
    switch (enc) {
        case 0x08000100:  // UTF-8
            out = utf8_to_u16(std::string((const char*)b, n));
            return true;
        case 0x0600:  // ASCII
        case 0x0201:  // Latin1
        case 0x0500:  // CP1252 (approximation)
        case 0:       // MacRoman (approximation; game strings are ASCII)
        case 0x0B01:
            for (size_t i = 0; i < n; i++) out += (char16_t)b[i];
            return true;
        case 0x0100:      // UTF-16 (host order, BOM aware)
        case 0x14000100:  // UTF-16LE
        case 0x10000100: {  // UTF-16BE
            bool be = enc == 0x10000100;
            size_t i = 0;
            if (enc == 0x0100 && n >= 2) {
                if (b[0] == 0xfe && b[1] == 0xff) { be = true; i = 2; }
                else if (b[0] == 0xff && b[1] == 0xfe) { i = 2; }
            }
            for (; i + 1 < n; i += 2) out += (char16_t)(be ? (b[i] << 8 | b[i + 1]) : (b[i] | b[i + 1] << 8));
            return true;
        }
        case 0x0c000100:  // UTF-32
        case 0x1c000100:  // UTF-32LE
            for (size_t i = 0; i + 3 < n; i += 4) {
                uint32_t c = b[i] | b[i + 1] << 8 | b[i + 2] << 16 | (uint32_t)b[i + 3] << 24;
                if (c >= 0x10000) {
                    c -= 0x10000;
                    out += (char16_t)(0xd800 + (c >> 10));
                    out += (char16_t)(0xdc00 + (c & 0x3ff));
                } else out += (char16_t)c;
            }
            return true;
        default:
            LOGW("decode_bytes: unsupported encoding %#x, assuming UTF-8", enc);
            out = utf8_to_u16(std::string((const char*)b, n));
            return true;
    }
}

bool encode_string(const std::u16string& s, uint32_t enc, std::string& out) {
    out.clear();
    switch (enc) {
        case 0x08000100:
            out = u16_to_utf8(s);
            return true;
        case 0x0600:
        case 0x0201:
        case 0x0500:
        case 0:
        case 0x0B01: {
            bool lossless = true;
            for (char16_t c : s) {
                if ((enc == 0x0600 && c > 0x7f) || c > 0xff) {
                    out += '?';
                    lossless = false;
                } else out += (char)c;
            }
            return lossless;
        }
        case 0x0100:
        case 0x14000100:
            out.assign((const char*)s.data(), s.size() * 2);
            return true;
        case 0x10000100:
            for (char16_t c : s) {
                out += (char)(c >> 8);
                out += (char)(c & 0xff);
            }
            return true;
        default:
            out = u16_to_utf8(s);
            return true;
    }
}

// --- accessors ------------------------------------------------------------------
bool is_string(id obj) { return obj && objc::is_kind_of(obj, g_NSString); }

std::u16string to_u16(id s) {
    if (!s) return {};
    if (is_constant(s)) {
        auto* o = (StringObj*)s;
        if (o->info & 0x10 /* UTF-16 */ || o->info == kInfoUTF16)
            return std::u16string((const char16_t*)o->data, o->length);
        std::u16string out;
        out.reserve(o->length);
        for (int64_t i = 0; i < o->length; i++) out += (char16_t)((const uint8_t*)o->data)[i];
        return out;
    }
    if (s->isa == g_CFString || objc::is_kind_of(s, g_CFString)) return dyn(s)->s;
    // Some other NSString subclass: ask it for UTF8String.
    const char* u = objc::send<const char*>(s, SEL_("UTF8String"));
    return utf8_to_u16(u ? u : "");
}

const char* utf8_ptr(id s) {
    if (!s) return nullptr;
    if (is_constant(s)) {
        auto* o = (StringObj*)s;
        if (!(o->info & 0x10) && o->info != kInfoUTF16) return (const char*)o->data;
        std::lock_guard<std::mutex> lk(g_const_cache_mutex);
        auto& c = const_utf8_cache();
        auto it = c.find(s);
        if (it == c.end()) it = c.emplace(s, u16_to_utf8(to_u16(s))).first;
        return it->second.c_str();
    }
    if (objc::is_kind_of(s, g_CFString)) {
        StrData* d = dyn(s);
        if (!d->utf8_valid) {
            d->utf8 = u16_to_utf8(d->s);
            d->utf8_valid = true;
        }
        return d->utf8.c_str();
    }
    return objc::send<const char*>(s, SEL_("UTF8String"));
}

std::string to_utf8(id s) {
    const char* p = utf8_ptr(s);
    return p ? p : "";
}

long length(id s) {
    if (!s) return 0;
    if (is_constant(s)) {
        auto* o = (StringObj*)s;
        return o->length;
    }
    if (objc::is_kind_of(s, g_CFString)) return (long)dyn(s)->s.size();
    return (long)to_u16(s).size();
}

static id new_dynamic(Class cls) {
    id o = objc::alloc_instance(cls ? cls : g_CFString);
    auto* so = (StringObj*)o;
    so->info = 0;
    so->data = new StrData();
    return o;
}

id make_string(const std::u16string& s) {
    id o = new_dynamic(g_CFString);
    dyn(o)->s = s;
    return o;
}

id make_string_utf8(const char* s, size_t len) {
    if (!s) s = "";
    return make_string(utf8_to_u16(len == std::string::npos ? std::string(s) : std::string(s, len)));
}

id make_string_utf8(const std::string& s) { return make_string(utf8_to_u16(s)); }

id make_constant(const char* s) {
    id o = make_string_utf8(s);
    for (int i = 0; i < 1000000; i++) objc::retain(o);  // effectively immortal
    return o;
}

id autoreleased(id obj) { return objc::autorelease(obj); }
id str_auto(const std::string& s) { return objc::autorelease(make_string_utf8(s)); }
id str_auto(const std::u16string& s) { return objc::autorelease(make_string(s)); }

void append(id s, const std::u16string& more) {
    if (!objc::is_kind_of(s, g_CFString)) {
        LOGE("append to immutable/constant string");
        return;
    }
    StrData* d = dyn(s);
    d->s += more;
    d->utf8_valid = false;
}

void replace(id s, long loc, long len, const std::u16string& with) {
    if (!objc::is_kind_of(s, g_CFString)) return;
    StrData* d = dyn(s);
    if (loc < 0 || loc > (long)d->s.size()) return;
    d->s.replace(loc, std::min<long>(len, d->s.size() - loc), with);
    d->utf8_valid = false;
}

static char16_t fold(char16_t c) { return (char16_t)towlower(c); }

int compare(const std::u16string& a, const std::u16string& b, unsigned long opt) {
    bool ci = opt & 1, numeric = opt & 64;
    size_t i = 0, j = 0;
    while (i < a.size() && j < b.size()) {
        char16_t ca = a[i], cb = b[j];
        if (numeric && ca >= '0' && ca <= '9' && cb >= '0' && cb <= '9') {
            unsigned long long na = 0, nb = 0;
            while (i < a.size() && a[i] >= '0' && a[i] <= '9') na = na * 10 + (a[i++] - '0');
            while (j < b.size() && b[j] >= '0' && b[j] <= '9') nb = nb * 10 + (b[j++] - '0');
            if (na != nb) return na < nb ? -1 : 1;
            continue;
        }
        if (ci) { ca = fold(ca); cb = fold(cb); }
        if (ca != cb) return ca < cb ? -1 : 1;
        i++;
        j++;
    }
    if (i < a.size()) return 1;
    if (j < b.size()) return -1;
    return 0;
}

}  // namespace ns

// =============================================================================
// Objective-C classes
// =============================================================================
using namespace ns;

namespace {

id NSString_alloc(Class, SEL) { return new_dynamic(g_CFString); }

void CFString_dealloc(id self, SEL) {
    if (!is_constant(self)) delete dyn(self);
    objc::dispose_instance(self);
}

const char* NSString_UTF8String(id self, SEL) { return utf8_ptr(self); }
NSUInteger NSString_length(id self, SEL) { return length(self); }
char16_t NSString_characterAtIndex(id self, SEL, NSUInteger i) {
    auto s = to_u16(self);
    return i < s.size() ? s[i] : 0;
}

id NSString_description(id self, SEL) { return self; }
id NSString_copy(id self, SEL) {
    if (is_constant(self)) return self;
    return make_string(to_u16(self));
}
id NSString_mutableCopy(id self, SEL) {
    id o = new_dynamic(g_NSMutableString ? g_CFString : g_CFString);
    dyn(o)->s = to_u16(self);
    return o;
}

BOOL NSString_isEqualToString(id self, SEL, id other) {
    if (self == other) return YES;
    if (!other || !is_string(other)) return NO;
    return to_u16(self) == to_u16(other);
}
NSUInteger NSString_hash(id self, SEL) {
    auto s = to_u16(self);
    NSUInteger h = 5381;
    for (char16_t c : s) h = h * 33 + c;
    return h;
}

BOOL NSString_hasPrefix(id self, SEL, id p) {
    auto s = to_u16(self), q = to_u16(p);
    return !q.empty() && s.compare(0, q.size(), q) == 0;
}
BOOL NSString_hasSuffix(id self, SEL, id p) {
    auto s = to_u16(self), q = to_u16(p);
    return !q.empty() && s.size() >= q.size() && s.compare(s.size() - q.size(), q.size(), q) == 0;
}

id NSString_substringFromIndex(id self, SEL, NSUInteger i) {
    auto s = to_u16(self);
    return str_auto(i < s.size() ? s.substr(i) : std::u16string());
}
id NSString_substringToIndex(id self, SEL, NSUInteger i) {
    auto s = to_u16(self);
    return str_auto(s.substr(0, std::min<size_t>(i, s.size())));
}
id NSString_substringWithRange(id self, SEL, NSRange r) {
    auto s = to_u16(self);
    if (r.location > s.size()) return str_auto(std::u16string());
    return str_auto(s.substr(r.location, r.length));
}

NSRange NSString_rangeOfString_options(id self, SEL, id needle, NSUInteger opts) {
    auto s = to_u16(self), n = to_u16(needle);
    if (n.empty()) return {(NSUInteger)-1 >> 1, 0};  // NSNotFound
    if (opts & 1) {
        for (auto& c : s) c = towlower(c);
        for (auto& c : n) c = towlower(c);
    }
    size_t pos = (opts & 4) ? s.rfind(n) : s.find(n);
    if (opts & 8) {  // anchored
        if (opts & 4) pos = (s.size() >= n.size() && s.compare(s.size() - n.size(), n.size(), n) == 0) ? s.size() - n.size() : std::u16string::npos;
        else pos = s.compare(0, n.size(), n) == 0 ? 0 : std::u16string::npos;
    }
    if (pos == std::u16string::npos) return {(NSUInteger)LONG_MAX, 0};
    return {pos, n.size()};
}
NSRange NSString_rangeOfString(id self, SEL s, id needle) {
    return NSString_rangeOfString_options(self, s, needle, 0);
}

NSInteger NSString_compare_options(id self, SEL, id other, NSUInteger opts) {
    return compare(to_u16(self), to_u16(other), opts);
}
NSInteger NSString_compare(id self, SEL s, id other) { return compare(to_u16(self), to_u16(other), 0); }
NSInteger NSString_caseInsensitiveCompare(id self, SEL, id other) {
    return compare(to_u16(self), to_u16(other), 1);
}
BOOL NSString_isEqual(id self, SEL s, id other) { return NSString_isEqualToString(self, s, other); }

float NSString_floatValue(id self, SEL) { return strtof(to_utf8(self).c_str(), nullptr); }
double NSString_doubleValue(id self, SEL) { return strtod(to_utf8(self).c_str(), nullptr); }
int NSString_intValue(id self, SEL) { return (int)strtol(to_utf8(self).c_str(), nullptr, 10); }
NSInteger NSString_integerValue(id self, SEL) { return strtol(to_utf8(self).c_str(), nullptr, 10); }
long long NSString_longLongValue(id self, SEL) { return strtoll(to_utf8(self).c_str(), nullptr, 10); }
BOOL NSString_boolValue(id self, SEL) {
    auto s = to_utf8(self);
    size_t i = s.find_first_not_of(" \t\n");
    if (i == std::string::npos) return NO;
    char c = s[i];
    return c == 'Y' || c == 'y' || c == 'T' || c == 't' || (c >= '1' && c <= '9');
}

id NSString_stringByAppendingString(id self, SEL, id other) {
    return str_auto(to_u16(self) + to_u16(other));
}

std::string path_append(const std::string& a, const std::string& b) {
    if (a.empty()) return b;
    if (b.empty()) return a;
    std::string r = a;
    while (r.size() > 1 && r.back() == '/') r.pop_back();
    size_t k = 0;
    while (k < b.size() && b[k] == '/') k++;
    return r + "/" + b.substr(k);
}

id NSString_stringByAppendingPathComponent(id self, SEL, id comp) {
    return str_auto(path_append(to_utf8(self), to_utf8(comp)));
}
id NSString_stringByAppendingPathExtension(id self, SEL, id ext) {
    return str_auto(to_utf8(self) + "." + to_utf8(ext));
}
id NSString_lastPathComponent(id self, SEL) {
    std::string s = to_utf8(self);
    while (s.size() > 1 && s.back() == '/') s.pop_back();
    size_t p = s.rfind('/');
    return str_auto(p == std::string::npos ? s : s.substr(p + 1));
}
id NSString_stringByDeletingLastPathComponent(id self, SEL) {
    std::string s = to_utf8(self);
    while (s.size() > 1 && s.back() == '/') s.pop_back();
    size_t p = s.rfind('/');
    if (p == std::string::npos) return str_auto(std::string());
    return str_auto(p == 0 ? std::string("/") : s.substr(0, p));
}
id NSString_pathExtension(id self, SEL) {
    std::string s = to_utf8(self);
    size_t slash = s.rfind('/'), dot = s.rfind('.');
    if (dot == std::string::npos || (slash != std::string::npos && dot < slash)) return str_auto(std::string());
    return str_auto(s.substr(dot + 1));
}
id NSString_stringByDeletingPathExtension(id self, SEL) {
    std::string s = to_utf8(self);
    size_t slash = s.rfind('/'), dot = s.rfind('.');
    if (dot == std::string::npos || (slash != std::string::npos && dot < slash)) return str_auto(s);
    return str_auto(s.substr(0, dot));
}
id NSString_lowercaseString(id self, SEL) {
    auto s = to_u16(self);
    for (auto& c : s) c = towlower(c);
    return str_auto(s);
}
id NSString_uppercaseString(id self, SEL) {
    auto s = to_u16(self);
    for (auto& c : s) c = towupper(c);
    return str_auto(s);
}

id NSString_componentsSeparatedByString(id self, SEL, id sep) {
    auto s = to_u16(self), d = to_u16(sep);
    id arr = foundation::make_mutable_array();
    if (d.empty()) {
        foundation::array_add(arr, str_auto(s));
        return objc::autorelease(arr);
    }
    size_t start = 0;
    for (;;) {
        size_t p = s.find(d, start);
        foundation::array_add(arr, str_auto(s.substr(start, p == std::u16string::npos ? std::u16string::npos : p - start)));
        if (p == std::u16string::npos) break;
        start = p + d.size();
    }
    return objc::autorelease(arr);
}

id NSString_stringByTrimmingCharactersInSet(id self, SEL, id set) {
    auto s = to_u16(self);
    auto in = [&](char16_t c) { return foundation::charset_contains(set, c); };
    size_t b = 0, e = s.size();
    while (b < e && in(s[b])) b++;
    while (e > b && in(s[e - 1])) e--;
    return str_auto(s.substr(b, e - b));
}

const char* NSString_cStringUsingEncoding(id self, SEL, NSUInteger enc) {
    uint32_t cf = ns_to_cf_encoding(enc);
    if (cf == 0x08000100) return utf8_ptr(self);
    std::string out;
    encode_string(to_u16(self), cf, out);
    out.push_back('\0');
    if (cf == 0x0100 || cf == 0x14000100 || cf == 0x10000100) out.push_back('\0');
    id data = foundation::make_data(out.data(), out.size());
    objc::autorelease(data);
    return (const char*)foundation::data_bytes(data);
}

BOOL NSString_getCString(id self, SEL, char* buf, NSUInteger max, NSUInteger enc) {
    std::string out;
    encode_string(to_u16(self), ns_to_cf_encoding(enc), out);
    if (out.size() + 1 > max) return NO;
    memcpy(buf, out.c_str(), out.size() + 1);
    return YES;
}

// init / factory methods ---------------------------------------------------------
id NSString_init(id self, SEL) { return self; }
id NSString_initWithUTF8String(id self, SEL, const char* s) {
    if (!s) { objc::release(self); return nil; }
    dyn(self)->s = utf8_to_u16(s);
    return self;
}
id NSString_initWithCString_encoding(id self, SEL, const char* s, NSUInteger enc) {
    if (!s) { objc::release(self); return nil; }
    decode_bytes((const uint8_t*)s, strlen(s), ns_to_cf_encoding(enc), dyn(self)->s);
    return self;
}
id NSString_initWithString(id self, SEL, id other) {
    dyn(self)->s = to_u16(other);
    return self;
}
id NSString_initWithBytes(id self, SEL, const void* b, NSUInteger n, NSUInteger enc) {
    decode_bytes((const uint8_t*)b, n, ns_to_cf_encoding(enc), dyn(self)->s);
    return self;
}
id NSString_initWithCharacters(id self, SEL, const char16_t* c, NSUInteger n) {
    dyn(self)->s.assign(c, n);
    return self;
}
id NSString_initWithData_encoding(id self, SEL, id data, NSUInteger enc) {
    decode_bytes((const uint8_t*)foundation::data_bytes(data), foundation::data_length(data),
                 ns_to_cf_encoding(enc), dyn(self)->s);
    return self;
}

id NSString_string(Class, SEL) { return str_auto(std::u16string()); }
id NSString_stringWithUTF8String(Class, SEL, const char* s) {
    return s ? str_auto(std::string(s)) : nil;
}
id NSString_stringWithCString_encoding(Class c, SEL, const char* s, NSUInteger enc) {
    if (!s) return nil;
    id o = new_dynamic(g_CFString);
    decode_bytes((const uint8_t*)s, strlen(s), ns_to_cf_encoding(enc), dyn(o)->s);
    return objc::autorelease(o);
}
id NSString_stringWithString(Class, SEL, id other) { return str_auto(to_u16(other)); }
id NSString_stringWithCharacters(Class, SEL, const char16_t* c, NSUInteger n) {
    return str_auto(std::u16string(c, n));
}

}  // namespace

// Variadic: called through the trampoline in varargs.S with x3 = stack args.
extern "C" id hle_NSString_stringWithFormat_impl(Class, SEL, id fmt, const uintptr_t* va) {
    std::string s = hle::apple_format(to_utf8(fmt).c_str(), {va}, true);
    return str_auto(s);
}
extern "C" id hle_NSString_initWithFormat_impl(id self, SEL, id fmt, const uintptr_t* va) {
    dyn(self)->s = utf8_to_u16(hle::apple_format(to_utf8(fmt).c_str(), {va}, true));
    return self;
}
extern "C" id hle_NSString_stringWithFormat(Class, SEL, id, ...);
extern "C" id hle_NSString_initWithFormat(id, SEL, id, ...);
extern "C" id hle_NSMutableString_appendFormat(id, SEL, id, ...);
extern "C" void hle_NSMutableString_appendFormat_impl(id self, SEL, id fmt, const uintptr_t* va) {
    append(self, utf8_to_u16(hle::apple_format(to_utf8(fmt).c_str(), {va}, true)));
}

static void NSMutableString_appendString(id self, SEL, id other) { append(self, to_u16(other)); }
static void NSMutableString_setString(id self, SEL, id other) {
    if (objc::is_kind_of(self, g_CFString)) {
        dyn(self)->s = to_u16(other);
        dyn(self)->utf8_valid = false;
    }
}
static id NSMutableString_stringWithCapacity(Class, SEL, NSUInteger) {
    return objc::autorelease(new_dynamic(g_CFString));
}

namespace foundation {

void init_strings() {
    g_NSString = objc::define_class("NSString", "NSObject", sizeof(StringObj));
    g_NSMutableString = objc::define_class("NSMutableString", "NSString", sizeof(StringObj));
    g_CFString = objc::define_class("__NSCFString", "NSMutableString", sizeof(StringObj));
    g_CFConstantString = objc::define_class("__NSCFConstantString", "NSString", sizeof(StringObj));
    hle::register_symbol("___CFConstantStringClassReference", g_CFConstantString);

    Class S = g_NSString;
    objc::add_class_method(S, "alloc", (void*)NSString_alloc);
    objc::add_class_method(S, "allocWithZone:", (void*)NSString_alloc);
    objc::add_class_method(S, "string", (void*)NSString_string);
    objc::add_class_method(S, "stringWithUTF8String:", (void*)NSString_stringWithUTF8String);
    objc::add_class_method(S, "stringWithCString:encoding:", (void*)NSString_stringWithCString_encoding);
    objc::add_class_method(S, "stringWithString:", (void*)NSString_stringWithString);
    objc::add_class_method(S, "stringWithCharacters:length:", (void*)NSString_stringWithCharacters);
    objc::add_class_method(S, "stringWithFormat:", (void*)hle_NSString_stringWithFormat);

    objc::add_method(S, "init", (void*)NSString_init);
    objc::add_method(S, "initWithUTF8String:", (void*)NSString_initWithUTF8String);
    objc::add_method(S, "initWithCString:encoding:", (void*)NSString_initWithCString_encoding);
    objc::add_method(S, "initWithString:", (void*)NSString_initWithString);
    objc::add_method(S, "initWithBytes:length:encoding:", (void*)NSString_initWithBytes);
    objc::add_method(S, "initWithCharacters:length:", (void*)NSString_initWithCharacters);
    objc::add_method(S, "initWithData:encoding:", (void*)NSString_initWithData_encoding);
    objc::add_method(S, "initWithFormat:", (void*)hle_NSString_initWithFormat);
    objc::add_method(S, "dealloc", (void*)CFString_dealloc);

    objc::add_method(S, "UTF8String", (void*)NSString_UTF8String);
    objc::add_method(S, "cStringUsingEncoding:", (void*)NSString_cStringUsingEncoding);
    objc::add_method(S, "getCString:maxLength:encoding:", (void*)NSString_getCString);
    objc::add_method(S, "length", (void*)NSString_length);
    objc::add_method(S, "characterAtIndex:", (void*)NSString_characterAtIndex);
    objc::add_method(S, "description", (void*)NSString_description);
    objc::add_method(S, "copy", (void*)NSString_copy);
    objc::add_method(S, "copyWithZone:", (void*)NSString_copy);
    objc::add_method(S, "mutableCopy", (void*)NSString_mutableCopy);
    objc::add_method(S, "isEqualToString:", (void*)NSString_isEqualToString);
    objc::add_method(S, "isEqual:", (void*)NSString_isEqual);
    objc::add_method(S, "hash", (void*)NSString_hash);
    objc::add_method(S, "hasPrefix:", (void*)NSString_hasPrefix);
    objc::add_method(S, "hasSuffix:", (void*)NSString_hasSuffix);
    objc::add_method(S, "substringFromIndex:", (void*)NSString_substringFromIndex);
    objc::add_method(S, "substringToIndex:", (void*)NSString_substringToIndex);
    objc::add_method(S, "substringWithRange:", (void*)NSString_substringWithRange);
    objc::add_method(S, "rangeOfString:", (void*)NSString_rangeOfString);
    objc::add_method(S, "rangeOfString:options:", (void*)NSString_rangeOfString_options);
    objc::add_method(S, "compare:", (void*)NSString_compare);
    objc::add_method(S, "compare:options:", (void*)NSString_compare_options);
    objc::add_method(S, "caseInsensitiveCompare:", (void*)NSString_caseInsensitiveCompare);
    objc::add_method(S, "floatValue", (void*)NSString_floatValue);
    objc::add_method(S, "doubleValue", (void*)NSString_doubleValue);
    objc::add_method(S, "intValue", (void*)NSString_intValue);
    objc::add_method(S, "integerValue", (void*)NSString_integerValue);
    objc::add_method(S, "longLongValue", (void*)NSString_longLongValue);
    objc::add_method(S, "boolValue", (void*)NSString_boolValue);
    objc::add_method(S, "stringByAppendingString:", (void*)NSString_stringByAppendingString);
    objc::add_method(S, "stringByAppendingPathComponent:", (void*)NSString_stringByAppendingPathComponent);
    objc::add_method(S, "stringByAppendingPathExtension:", (void*)NSString_stringByAppendingPathExtension);
    objc::add_method(S, "lastPathComponent", (void*)NSString_lastPathComponent);
    objc::add_method(S, "stringByDeletingLastPathComponent", (void*)NSString_stringByDeletingLastPathComponent);
    objc::add_method(S, "pathExtension", (void*)NSString_pathExtension);
    objc::add_method(S, "stringByDeletingPathExtension", (void*)NSString_stringByDeletingPathExtension);
    objc::add_method(S, "lowercaseString", (void*)NSString_lowercaseString);
    objc::add_method(S, "uppercaseString", (void*)NSString_uppercaseString);
    objc::add_method(S, "componentsSeparatedByString:", (void*)NSString_componentsSeparatedByString);
    objc::add_method(S, "stringByTrimmingCharactersInSet:", (void*)NSString_stringByTrimmingCharactersInSet);

    Class M = g_NSMutableString;
    objc::add_class_method(M, "stringWithCapacity:", (void*)NSMutableString_stringWithCapacity);
    objc::add_method(M, "appendString:", (void*)NSMutableString_appendString);
    objc::add_method(M, "setString:", (void*)NSMutableString_setString);
    objc::add_method(M, "appendFormat:", (void*)hle_NSMutableString_appendFormat);
}

}  // namespace foundation

// =============================================================================
// CoreFoundation string API
// =============================================================================
namespace {

struct CFRange {
    long location, length;
};

long CFStringGetLength(id s) { return length(s); }
char16_t CFStringGetCharacterAtIndex(id s, long i) {
    auto u = to_u16(s);
    return i >= 0 && i < (long)u.size() ? u[i] : 0;
}
void CFStringGetCharacters(id s, CFRange r, char16_t* buf) {
    auto u = to_u16(s);
    for (long i = 0; i < r.length; i++) {
        long k = r.location + i;
        buf[i] = k < (long)u.size() ? u[k] : 0;
    }
}
const char16_t* CFStringGetCharactersPtr(id s) {
    if (!is_constant(s) && objc::is_kind_of(s, g_CFString)) return dyn(s)->s.c_str();
    auto* o = (StringObj*)s;
    if (is_constant(s) && (o->info & 0x10)) return (const char16_t*)o->data;
    return nullptr;
}
const char* CFStringGetCStringPtr(id s, uint32_t enc) {
    if (!s) return nullptr;
    if (is_constant(s)) {
        auto* o = (StringObj*)s;
        if (!(o->info & 0x10) && (enc == 0x08000100 || enc == 0x0600 || enc == 0 || enc == 0x0201))
            return (const char*)o->data;
        return nullptr;
    }
    if (enc == 0x08000100) return utf8_ptr(s);
    return nullptr;
}
bool CFStringGetCString(id s, char* buf, long size, uint32_t enc) {
    if (!s || !buf || size <= 0) return false;
    std::string out;
    encode_string(to_u16(s), enc, out);
    if ((long)out.size() + 1 > size) {
        // CF fails if it doesn't fit; still null-terminate defensively.
        buf[0] = 0;
        return false;
    }
    memcpy(buf, out.c_str(), out.size() + 1);
    return true;
}
long CFStringGetBytes(id s, CFRange r, uint32_t enc, uint8_t lossByte, bool external, uint8_t* buf,
                      long maxLen, long* usedLen) {
    auto u = to_u16(s);
    if (r.location < 0 || r.location > (long)u.size()) r = {0, 0};
    std::u16string part = u.substr(r.location, r.length);
    long converted = 0, used = 0;
    for (size_t i = 0; i < part.size(); i++) {
        std::string enc_c;
        encode_string(part.substr(i, 1), enc, enc_c);
        if (enc_c == "?" && part[i] != '?' && !lossByte) break;
        if (buf) {
            if (used + (long)enc_c.size() > maxLen) break;
            memcpy(buf + used, enc_c.data(), enc_c.size());
        }
        used += enc_c.size();
        converted++;
    }
    if (usedLen) *usedLen = used;
    return converted;
}

id CFStringCreateWithCString(void*, const char* s, uint32_t enc) {
    if (!s) return nil;
    id o = new_dynamic(g_CFString);
    decode_bytes((const uint8_t*)s, strlen(s), enc, dyn(o)->s);
    return o;
}
id CFStringCreateWithCStringNoCopy(void* a, const char* s, uint32_t enc, void*) {
    return CFStringCreateWithCString(a, s, enc);
}
id CFStringCreateWithBytes(void*, const uint8_t* b, long n, uint32_t enc, bool) {
    id o = new_dynamic(g_CFString);
    decode_bytes(b, n, enc, dyn(o)->s);
    return o;
}
id CFStringCreateCopy(void*, id s) { return make_string(to_u16(s)); }
id CFStringCreateMutable(void*, long) { return new_dynamic(g_CFString); }
id CFStringCreateWithSubstring(void*, id s, CFRange r) {
    auto u = to_u16(s);
    if (r.location < 0 || r.location > (long)u.size()) return make_string({});
    return make_string(u.substr(r.location, r.length));
}
void CFStringAppend(id s, id more) { append(s, to_u16(more)); }
void CFStringAppendCharacters(id s, const char16_t* chars, long count) {
    if (count > 0 && chars) append(s, std::u16string(chars, count));
}
int32_t CFStringGetIntValue(id s) { return (int32_t)strtol(to_utf8(s).c_str(), nullptr, 10); }
void CFStringAppendCString(id s, const char* c, uint32_t enc) {
    std::u16string u;
    decode_bytes((const uint8_t*)c, strlen(c), enc, u);
    append(s, u);
}
void CFStringDelete(id s, CFRange r) { replace(s, r.location, r.length, {}); }
long CFStringCompare(id a, id b, unsigned long opts) { return compare(to_u16(a), to_u16(b), opts); }
long CFStringCompareWithOptions(id a, id b, CFRange r, unsigned long opts) {
    auto u = to_u16(a);
    if (r.location < 0 || r.location > (long)u.size()) r = {0, 0};
    return compare(u.substr(r.location, r.length), to_u16(b), opts);
}
bool CFStringFindWithOptions(id s, id find, CFRange r, unsigned long opts, CFRange* result) {
    auto u = to_u16(s);
    if (r.location < 0 || r.location > (long)u.size()) return false;
    std::u16string hay = u.substr(r.location, r.length), n = to_u16(find);
    if (opts & 1) {
        for (auto& c : hay) c = towlower(c);
        for (auto& c : n) c = towlower(c);
    }
    size_t pos;
    if (opts & 8) {  // anchored
        if (opts & 4) pos = hay.size() >= n.size() && hay.compare(hay.size() - n.size(), n.size(), n) == 0 ? hay.size() - n.size() : std::u16string::npos;
        else pos = hay.compare(0, n.size(), n) == 0 ? 0 : std::u16string::npos;
    } else {
        pos = (opts & 4) ? hay.rfind(n) : hay.find(n);
    }
    if (pos == std::u16string::npos) return false;
    if (result) *result = {(long)pos + r.location, (long)n.size()};
    return true;
}

}  // namespace

extern "C" id hle_CFStringCreateWithFormat_impl(void*, void*, id fmt, const uintptr_t* va) {
    return make_string_utf8(hle::apple_format(to_utf8(fmt).c_str(), {va}, true));
}
extern "C" id hle_CFStringCreateWithFormatAndArguments(void*, void*, id fmt, const uintptr_t* va) {
    return make_string_utf8(hle::apple_format(to_utf8(fmt).c_str(), {va}, true));
}
extern "C" void hle_CFStringAppendFormat_impl(id s, void*, id fmt, const uintptr_t* va) {
    append(s, utf8_to_u16(hle::apple_format(to_utf8(fmt).c_str(), {va}, true)));
}
extern "C" id hle_CFStringCreateWithFormat(void*, void*, id, ...);
extern "C" void hle_CFStringAppendFormat(id, void*, id, ...);

HLE_EXPORT(CFStringGetLength, CFStringGetLength);
HLE_EXPORT(CFStringGetCharacterAtIndex, CFStringGetCharacterAtIndex);
HLE_EXPORT(CFStringGetCharacters, CFStringGetCharacters);
HLE_EXPORT(CFStringGetCharactersPtr, CFStringGetCharactersPtr);
HLE_EXPORT(CFStringGetCStringPtr, CFStringGetCStringPtr);
HLE_EXPORT(CFStringGetCString, CFStringGetCString);
HLE_EXPORT(CFStringGetBytes, CFStringGetBytes);
HLE_EXPORT(CFStringCreateWithCString, CFStringCreateWithCString);
HLE_EXPORT(CFStringCreateWithCStringNoCopy, CFStringCreateWithCStringNoCopy);
HLE_EXPORT(CFStringCreateWithBytes, CFStringCreateWithBytes);
HLE_EXPORT(CFStringCreateCopy, CFStringCreateCopy);
HLE_EXPORT(CFStringCreateMutable, CFStringCreateMutable);
HLE_EXPORT(CFStringCreateWithSubstring, CFStringCreateWithSubstring);
HLE_EXPORT(CFStringAppend, CFStringAppend);
HLE_EXPORT(CFStringAppendCharacters, CFStringAppendCharacters);
HLE_EXPORT(CFStringGetIntValue, CFStringGetIntValue);
HLE_EXPORT(CFStringAppendCString, CFStringAppendCString);
HLE_EXPORT(CFStringDelete, CFStringDelete);
HLE_EXPORT(CFStringCompare, CFStringCompare);
HLE_EXPORT(CFStringCompareWithOptions, CFStringCompareWithOptions);
HLE_EXPORT(CFStringFindWithOptions, CFStringFindWithOptions);
HLE_EXPORT(CFStringCreateWithFormat, hle_CFStringCreateWithFormat);
HLE_EXPORT(CFStringCreateWithFormatAndArguments, hle_CFStringCreateWithFormatAndArguments);
HLE_EXPORT(CFStringAppendFormat, hle_CFStringAppendFormat);
