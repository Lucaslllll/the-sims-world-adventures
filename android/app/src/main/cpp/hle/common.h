// Shared definitions for the HLE layer.
#pragma once

#include <android/log.h>
#include <cstddef>
#include <cstdint>
#include <cstdio>

#define LOG_TAG "TS3WA"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)

// Verbose per-call tracing of HLE entry points, enabled with -DTS3WA_TRACE.
#ifdef TS3WA_TRACE
#define TRACE(...) LOGD(__VA_ARGS__)
#else
#define TRACE(...) do {} while (0)
#endif

// Objective-C basic types with the Apple ARM32 ABI.
struct objc_object;
struct objc_class;
typedef objc_object* id;
typedef objc_class* Class;
typedef const char* SEL;  // canonical (interned) selector name
typedef id (*IMP)(id, SEL, ...);
typedef signed char BOOL;  // iOS ARM32
typedef long NSInteger;
typedef unsigned long NSUInteger;
typedef float CGFloat;

struct CGPoint { CGFloat x, y; };
struct CGSize { CGFloat width, height; };
struct CGRect { CGPoint origin; CGSize size; };
struct NSRange { NSUInteger location, length; };

#define nil ((id)0)
#define YES true
#define NO false

namespace hle {

// Register a symbol the game binary can import. `name` is the Mach-O name
// (with the leading underscore), e.g. "_malloc" or "_OBJC_CLASS_$_NSString".
void register_symbol(const char* name, void* addr);
void* find_symbol(const char* name);

struct AutoRegister {
    AutoRegister(const char* name, void* addr) { register_symbol(name, addr); }
};

// Path helpers (set by the platform layer before the game starts).
const char* bundle_path();     // .../sims3wa_iphone.app
const char* documents_path();  // writable Documents dir
const char* library_path();    // writable Library dir
const char* caches_path();     // writable Library/Caches dir

}  // namespace hle

#define HLE_CAT2(a, b) a##b
#define HLE_CAT(a, b) HLE_CAT2(a, b)
// Export a C function to the game under its Mach-O symbol name "_sym".
#define HLE_EXPORT_AS(symname, fn) \
    static hle::AutoRegister HLE_CAT(hle_reg_, __COUNTER__)(symname, (void*)(fn))
#define HLE_EXPORT(sym, fn) HLE_EXPORT_AS("_" #sym, fn)
