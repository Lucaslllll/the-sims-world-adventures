// Objective-C runtime compatible with Apple's ObjC2 ARM32 ABI.
#pragma once

#include <atomic>
#include <string>
#include <type_traits>

#include "hle/common.h"

struct MachOImage;

// Apple-compatible class object layout.
struct objc_class {
    objc_class* isa;
    objc_class* superclass;
    void* cache;   // Apple: bucket_t*; we store ClassRT*
    void* vtable;  // Apple: mask/occupied; unused
    uintptr_t bits;  // class_ro_t* (| flags) for image classes
};

struct objc_object {
    Class isa;
};

struct objc_super {
    id receiver;
    Class cls;  // objc_msgSendSuper2: the *current* class
};

extern "C" {
id objc_msgSend(id self, SEL op, ...);
id objc_msgSendSuper2(objc_super* super, SEL op, ...);
void objc_msgSend_stret(void* result, id self, SEL op, ...);
}

namespace objc {

// --- selectors / classes -----------------------------------------------------
SEL sel(const char* name);  // intern
Class get_class(const char* name);
Class get_metaclass(const char* name);
const char* class_name(Class cls);
Class object_class(id obj);
bool is_meta(Class cls);
bool is_subclass(Class cls, Class parent);
bool is_kind_of(id obj, Class parent);
size_t instance_size(Class cls);
bool responds_to(Class cls, SEL sel);
IMP lookup_method(Class cls, SEL sel);  // returns nullptr if not found

// Define an HLE class. `instance_size` includes the isa pointer.
Class define_class(const char* name, const char* super_name, size_t instance_size);
void add_method(Class cls, const char* sel_name, void* imp);
void add_class_method(Class cls, const char* sel_name, void* imp);

// Resolve "_OBJC_CLASS_$_X" / "_OBJC_METACLASS_$_X" import names.
void* resolve_class_symbol(const char* name);

// Realize classes/categories and fix selector references of a loaded image.
void load_image(const MachOImage& img);

// --- objects -------------------------------------------------------------------
id alloc_instance(Class cls, size_t extra = 0);
void dispose_instance(id obj);
id retain(id obj);
void release(id obj);
id autorelease(id obj);
NSUInteger retain_count(id obj);
bool is_static_object(id obj);

// Autorelease pools (per thread stack).
void* pool_push();
void pool_pop(void* token);

// Convenience typed senders.
template <typename R = id, typename... A>
inline R send(id self, SEL op, A... args) {
    if constexpr (std::is_class_v<R> || std::is_union_v<R>)
        return ((R(*)(id, SEL, A...))objc_msgSend_stret)(self, op, args...);
    else
        return ((R(*)(id, SEL, A...))objc_msgSend)(self, op, args...);
}
template <typename R = id, typename... A>
inline R send(Class self, SEL op, A... args) {
    return send<R>((id)self, op, args...);
}

// Blocks (only global/heap literal layout is needed).
struct Block {
    void* isa;
    int32_t flags;
    int32_t reserved;
    void* invoke;
    void* descriptor;
};
template <typename... A>
inline void call_block(void* blk, A... args) {
    if (!blk) return;
    auto* b = (Block*)blk;
    ((void (*)(void*, A...))b->invoke)(blk, args...);
}

}  // namespace objc

// Interned selector for a string literal, cached per call site.
#define SEL_(lit) ([]() -> SEL { static SEL s_ = objc::sel(lit); return s_; }())
// Class by name, cached per call site.
#define CLS_(lit) ([]() -> Class { static Class c_ = objc::get_class(lit); return c_; }())
