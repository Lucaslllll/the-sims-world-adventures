#include "hle/format.h"

#include <cstdarg>
#include <cstring>

#include "hle/nsstring.h"
#include "objc/runtime.h"

namespace hle {

namespace {

void append_utf16(std::string& out, const char16_t* s, long n) {
    std::u16string tmp = n < 0 ? std::u16string(s) : std::u16string(s, n);
    out += ns::u16_to_utf8(tmp);
}

std::string object_description(id obj) {
    if (!obj) return "(null)";
    if (ns::is_string(obj)) return ns::to_utf8(obj);
    id d = objc::send(obj, SEL_("description"));
    return d ? ns::to_utf8(d) : "(null)";
}

}  // namespace

std::string apple_format(const char* fmt, AppleArgs args, bool allow_objects) {
    std::string out;
    const uintptr_t* next = args.slots;
    auto slot = [&](int pos, bool wide64 = false) -> uint64_t {
        // pos > 0: explicit positional argument (1-based)
        const uintptr_t* at = pos > 0 ? args.slots + pos - 1 : next;
        uint64_t result = *at;
        if (wide64) result |= (uint64_t)at[1] << 32;
        if (pos <= 0) next += wide64 ? 2 : 1;
        return result;
    };
    const char* p = fmt;
    while (*p) {
        if (*p != '%') {
            out += *p++;
            continue;
        }
        const char* start = p++;
        if (*p == '%') {
            out += '%';
            p++;
            continue;
        }
        // positional "%n$"
        int pos = 0;
        {
            const char* q = p;
            int n = 0;
            while (*q >= '0' && *q <= '9') n = n * 10 + (*q++ - '0');
            if (*q == '$' && n > 0) {
                pos = n;
                p = q + 1;
            }
        }
        std::string spec = "%";
        while (*p && strchr("-+ #0'", *p)) spec += *p++;
        // width
        if (*p == '*') {
            p++;
            int wpos = 0;
            const char* q = p;
            int n = 0;
            while (*q >= '0' && *q <= '9') n = n * 10 + (*q++ - '0');
            if (*q == '$') { wpos = n; p = q + 1; }
            spec += std::to_string((int32_t)slot(wpos));
        } else {
            while (*p >= '0' && *p <= '9') spec += *p++;
        }
        // precision
        if (*p == '.') {
            spec += *p++;
            if (*p == '*') {
                p++;
                int ppos = 0;
                const char* q = p;
                int n = 0;
                while (*q >= '0' && *q <= '9') n = n * 10 + (*q++ - '0');
                if (*q == '$') { ppos = n; p = q + 1; }
                spec += std::to_string((int32_t)slot(ppos));
            } else {
                while (*p >= '0' && *p <= '9') spec += *p++;
            }
        }
        // length modifiers
        int lng = 0;  // 0=int, 1=long, 2=long long, -1=short, -2=char
        bool wide = false;
        for (;;) {
            if (*p == 'l') { lng = lng >= 1 ? 2 : 1; wide = true; p++; }
            else if (*p == 'h') { lng = lng == -1 ? -2 : -1; p++; }
            else if (*p == 'q' || *p == 'j') { lng = 2; p++; }
            else if (*p == 'z' || *p == 't') { lng = 1; p++; }
            else if (*p == 'L') { p++; }  // long double == double on Apple arm64
            else break;
        }
        char conv = *p ? *p++ : 0;
        char buf[512];
        switch (conv) {
            case 'd':
            case 'i': {
                uint64_t v = slot(pos, lng == 2);
                long long sv = lng == 2 ? (long long)v
                             : lng == 1 ? (long long)(int32_t)v
                             : lng == -1 ? (long long)(int16_t)v
                             : lng == -2 ? (long long)(int8_t)v
                                         : (long long)(int32_t)v;
                snprintf(buf, sizeof buf, (spec + "ll" + conv).c_str(), sv);
                out += buf;
                break;
            }
            case 'u':
            case 'o':
            case 'x':
            case 'X': {
                uint64_t v = slot(pos, lng == 2);
                unsigned long long uv = lng >= 1 ? v
                                      : lng == -1 ? (uint16_t)v
                                      : lng == -2 ? (uint8_t)v
                                                  : (uint32_t)v;
                snprintf(buf, sizeof buf, (spec + "ll" + conv).c_str(), uv);
                out += buf;
                break;
            }
            case 'D': case 'U': case 'O': {  // obsolete long forms
                uint64_t v = slot(pos);
                char c = conv == 'D' ? 'd' : conv == 'U' ? 'u' : 'o';
                snprintf(buf, sizeof buf, (spec + "l" + c).c_str(), (long)v);
                out += buf;
                break;
            }
            case 'f': case 'F': case 'e': case 'E':
            case 'g': case 'G': case 'a': case 'A': {
                uint64_t v = slot(pos, true);
                double d;
                memcpy(&d, &v, 8);
                snprintf(buf, sizeof buf, (spec + conv).c_str(), d);
                out += buf;
                break;
            }
            case 'c': {
                uint64_t v = slot(pos);
                if (wide) {
                    char16_t c = (char16_t)v;
                    append_utf16(out, &c, 1);
                } else {
                    snprintf(buf, sizeof buf, (spec + 'c').c_str(), (int)(char)v);
                    out += buf;
                }
                break;
            }
            case 'C': {
                char16_t c = (char16_t)slot(pos);
                append_utf16(out, &c, 1);
                break;
            }
            case 's': {
                const char* s = (const char*)slot(pos);
                if (wide) {
                    // %ls: wchar_t (32-bit) string
                    std::string tmp;
                    for (const uint32_t* w = (const uint32_t*)s; w && *w; w++) {
                        char16_t c = (char16_t)*w;
                        append_utf16(tmp, &c, 1);
                    }
                    out += tmp;
                } else {
                    std::string sp = spec + 's';
                    if (sp == "%s") out += s ? s : "(null)";
                    else {
                        std::string tmp(4096, '\0');
                        int n = snprintf(tmp.data(), tmp.size(), sp.c_str(), s ? s : "(null)");
                        tmp.resize(n < 0 ? 0 : std::min<size_t>(n, tmp.size() - 1));
                        out += tmp;
                    }
                }
                break;
            }
            case 'S': {
                const char16_t* s = (const char16_t*)slot(pos);
                if (s) append_utf16(out, s, -1);
                else out += "(null)";
                break;
            }
            case 'p': {
                snprintf(buf, sizeof buf, "%p", (void*)slot(pos));
                out += buf;
                break;
            }
            case '@': {
                id obj = (id)slot(pos);
                if (allow_objects) out += object_description(obj);
                else out += "(object)";
                break;
            }
            case 'n':
                slot(pos);
                break;
            default:
                // Unknown conversion: emit it verbatim.
                out.append(start, p - start);
                break;
        }
    }
    return out;
}

}  // namespace hle
