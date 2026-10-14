// Objective-C runtime: classes, selectors, method lookup, refcounting.
#include "objc/runtime.h"

#include <cstdlib>
#include <cstring>
#include <mutex>
#include <set>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "loader/macho.h"
#include "loader/abi32.h"

namespace {

// --- Apple ObjC2 metadata structures (64-bit) -------------------------------
struct method_t {
    const char* name;
    const char* types;
    void* imp;
};
struct method_list_t {
    uint32_t entsize_flags;
    uint32_t count;
    method_t* at(uint32_t i) {
        return (method_t*)((char*)(this + 1) + i * (entsize_flags & ~3u));
    }
};
struct ivar_t {
    int32_t* offset;
    const char* name;
    const char* type;
    uint32_t alignment_raw;
    uint32_t size;
};
struct ivar_list_t {
    uint32_t entsize;
    uint32_t count;
    ivar_t* at(uint32_t i) { return (ivar_t*)((char*)(this + 1) + i * entsize); }
};
struct protocol_t {
    void* isa;
    const char* name;
};
struct protocol_list_t {
    uintptr_t count;
    protocol_t* list[1];
};
struct class_ro_t {
    uint32_t flags;
    uint32_t instanceStart;
    uint32_t instanceSize;
    const uint8_t* ivarLayout;
    const char* name;
    method_list_t* baseMethods;
    protocol_list_t* baseProtocols;
    ivar_list_t* ivars;
    const uint8_t* weakIvarLayout;
    void* baseProperties;
};
struct category_t {
    const char* name;
    Class cls;
    method_list_t* instanceMethods;
    method_list_t* classMethods;
    protocol_list_t* protocols;
    void* instanceProperties;
};

constexpr uint32_t RO_META = 1 << 0;

// --- Our per-class runtime data ----------------------------------------------
struct ClassRT {
    Class cls;
    const char* name;
    bool meta;
    uint32_t instance_size;
    std::unordered_map<SEL, IMP> methods;
    std::unordered_map<SEL, IMP> cache;
    std::vector<const char*> protocols;
};

std::recursive_mutex g_lock;

std::unordered_map<std::string_view, const char*>& sel_table() {
    static std::unordered_map<std::string_view, const char*> t;
    return t;
}
std::unordered_map<std::string_view, Class>& class_table() {
    static std::unordered_map<std::string_view, Class> t;
    return t;
}

inline ClassRT* rt(Class c) { return (ClassRT*)c->cache; }

Class g_root_meta = nullptr;  // NSObject's metaclass

// --- object header -------------------------------------------------------------
constexpr uint64_t kObjMagic = 0x3341535443424f4fULL;  // "OOBCTSA3"
struct ObjHeader {
    std::atomic<int64_t> rc;
    uint64_t magic;
};
inline ObjHeader* header(id obj) { return (ObjHeader*)((char*)obj - sizeof(ObjHeader)); }

// --- autorelease pools ---------------------------------------------------------
thread_local std::vector<id>* t_pool = nullptr;

std::vector<id>& pool_stack() {
    if (!t_pool) t_pool = new std::vector<id>();
    return *t_pool;
}

void log_unrecognized(Class cls, SEL sel) {
    static std::mutex m;
    static std::set<std::pair<Class, SEL>> seen;
    std::lock_guard<std::mutex> lk(m);
    if (seen.insert({cls, sel}).second) {
        bool meta = rt(cls)->meta;
        LOGW("unrecognized selector %c[%s %s]", meta ? '+' : '-', rt(cls)->name, sel);
    }
}

ClassRT* new_rt(Class cls, const char* name, bool meta, uint32_t size) {
    auto* r = new ClassRT();
    r->cls = cls;
    r->name = name;
    r->meta = meta;
    r->instance_size = size;
    cls->cache = r;
    cls->vtable = nullptr;
    return r;
}

void add_method_list(ClassRT* r, method_list_t* ml, bool fix_names) {
    if (!ml) return;
    for (uint32_t i = 0; i < ml->count; i++) {
        method_t* m = ml->at(i);
        SEL s = objc::sel(m->name);
        if (fix_names) m->name = s;
        // Categories override; later lists win.
        r->methods[s] = (IMP)m->imp;
    }
}

}  // namespace

extern "C" {
// Exposed for the assembly fast path.
IMP objc_lookup_for_object(id self, SEL sel);
IMP objc_lookup_super2(objc_super* sup, SEL sel);
void objc_nil_method();  // msgsend.S: returns 0 in x0/x1/d0-d3
}

namespace objc {

SEL sel(const char* name) {
    std::lock_guard<std::recursive_mutex> lk(g_lock);
    auto& t = sel_table();
    auto it = t.find(name);
    if (it != t.end()) return it->second;
    const char* s = strdup(name);
    t.emplace(std::string_view(s), s);
    return s;
}

Class get_class(const char* name) {
    std::lock_guard<std::recursive_mutex> lk(g_lock);
    auto& t = class_table();
    auto it = t.find(name);
    return it == t.end() ? nullptr : it->second;
}

Class get_metaclass(const char* name) {
    Class c = get_class(name);
    return c ? c->isa : nullptr;
}

const char* class_name(Class cls) { return cls ? rt(cls)->name : "nil"; }

Class object_class(id obj) { return obj ? obj->isa : nullptr; }

bool is_meta(Class cls) { return rt(cls)->meta; }

bool is_subclass(Class cls, Class parent) {
    for (Class c = cls; c; c = c->superclass)
        if (c == parent) return true;
    return false;
}

bool is_kind_of(id obj, Class parent) { return obj && is_subclass(obj->isa, parent); }

size_t instance_size(Class cls) { return rt(cls)->instance_size; }

IMP lookup_method(Class cls, SEL s) {
    std::lock_guard<std::recursive_mutex> lk(g_lock);
    ClassRT* r = rt(cls);
    auto it = r->cache.find(s);
    if (it != r->cache.end()) return it->second;
    for (Class c = cls; c; c = c->superclass) {
        auto m = rt(c)->methods.find(s);
        if (m != rt(c)->methods.end()) {
            r->cache[s] = m->second;
            return m->second;
        }
    }
    return nullptr;
}

bool responds_to(Class cls, SEL s) { return lookup_method(cls, s) != nullptr; }

Class define_class(const char* name, const char* super_name, size_t size) {
    std::lock_guard<std::recursive_mutex> lk(g_lock);
    if (Class existing = get_class(name)) return existing;
    Class super = super_name ? get_class(super_name) : nullptr;
    if (super_name && !super) {
        LOGE("define_class %s: superclass %s not defined yet", name, super_name);
        abort();
    }
    Class cls = (Class)calloc(1, sizeof(objc_class));
    Class meta = (Class)calloc(1, sizeof(objc_class));
    const char* n = strdup(name);
    new_rt(cls, n, false, (uint32_t)size);
    new_rt(meta, n, true, sizeof(objc_class));
    cls->isa = meta;
    cls->superclass = super;
    if (super) {
        meta->superclass = super->isa;
        meta->isa = g_root_meta;
    } else {
        // Root class: its metaclass inherits from the class itself.
        meta->superclass = cls;
        meta->isa = meta;
        if (!g_root_meta) g_root_meta = meta;
    }
    class_table().emplace(std::string_view(n), cls);
    return cls;
}

void add_method(Class cls, const char* sel_name, void* imp) {
    std::lock_guard<std::recursive_mutex> lk(g_lock);
    rt(cls)->methods[sel(sel_name)] = (IMP)arm32_wrap_function(imp);
    rt(cls)->cache.clear();
}

void add_class_method(Class cls, const char* sel_name, void* imp) {
    add_method(cls->isa, sel_name, imp);
}

void* resolve_class_symbol(const char* name) {
    static const char kCls[] = "_OBJC_CLASS_$_";
    static const char kMeta[] = "_OBJC_METACLASS_$_";
    bool meta = false;
    const char* cname = nullptr;
    if (!strncmp(name, kCls, sizeof(kCls) - 1)) {
        cname = name + sizeof(kCls) - 1;
    } else if (!strncmp(name, kMeta, sizeof(kMeta) - 1)) {
        cname = name + sizeof(kMeta) - 1;
        meta = true;
    } else {
        return nullptr;
    }
    Class c = get_class(cname);
    if (!c) {
        LOGW("class %s not implemented; creating empty placeholder", cname);
        c = define_class(cname, "NSObject", 64);
    }
    return meta ? (void*)c->isa : (void*)c;
}

// Realize one class from the image (and its metaclass). Superclasses are
// realized first so ivar offsets can be slid.
static void realize(Class cls) {
    // HLE classes and already-realized image classes have a ClassRT.
    if (cls->cache != nullptr) return;
    if (cls->superclass) realize(cls->superclass);

    auto* ro = (class_ro_t*)(cls->bits & ~(uintptr_t)3);
    auto* mro = (class_ro_t*)(cls->isa->bits & ~(uintptr_t)3);
    uint32_t super_size = cls->superclass ? rt(cls->superclass)->instance_size : 0;
    uint32_t size = ro->instanceSize;
    if (ro->instanceStart < super_size) {
        int32_t delta = super_size - ro->instanceStart;
        LOGW("sliding ivars of %s by %d", ro->name, delta);
        if (ro->ivars)
            for (uint32_t i = 0; i < ro->ivars->count; i++) {
                ivar_t* iv = ro->ivars->at(i);
                if (iv->offset) *iv->offset += delta;
            }
        size += delta;
    }
    ClassRT* r = new_rt(cls, ro->name, false, size);
    add_method_list(r, ro->baseMethods, true);
    if (ro->baseProtocols)
        for (uintptr_t i = 0; i < ro->baseProtocols->count; i++)
            r->protocols.push_back(ro->baseProtocols->list[i]->name);

    Class meta = cls->isa;
    ClassRT* mr = new_rt(meta, ro->name, true, sizeof(objc_class));
    add_method_list(mr, mro->baseMethods, true);
    class_table()[std::string_view(ro->name)] = cls;
    // The image binds meta->isa to NSObject's metaclass and
    // meta->superclass to the superclass's metaclass already.
}

void load_image(const MachOImage& img) {
    std::lock_guard<std::recursive_mutex> lk(g_lock);
    // 1. selector references
    if (auto* s = img.section("__DATA", "__objc_selrefs")) {
        auto** refs = (const char**)s->addr;
        for (size_t i = 0; i < s->size / sizeof(void*); i++) refs[i] = sel(refs[i]);
    }
    // 2. classes. Mark image classes by clearing the HLE marker (cache field
    // holds __objc_empty_cache after binding).
    std::vector<Class> classes;
    if (auto* s = img.section("__DATA", "__objc_classlist")) {
        auto** list = (Class*)s->addr;
        for (size_t i = 0; i < s->size / sizeof(void*); i++) {
            list[i]->cache = nullptr;
            list[i]->isa->cache = nullptr;
            classes.push_back(list[i]);
        }
    }
    for (Class c : classes) realize(c);
    for (Class c : classes) LOGI("realized class %s : %s (size %u)", rt(c)->name,
                                 c->superclass ? rt(c->superclass)->name : "-", rt(c)->instance_size);
    // 3. categories
    if (auto* s = img.section("__DATA", "__objc_catlist")) {
        auto** list = (category_t**)s->addr;
        for (size_t i = 0; i < s->size / sizeof(void*); i++) {
            category_t* cat = list[i];
            add_method_list(rt(cat->cls), cat->instanceMethods, true);
            add_method_list(rt(cat->cls->isa), cat->classMethods, true);
            rt(cat->cls)->cache.clear();
            rt(cat->cls->isa)->cache.clear();
            LOGI("attached category %s(%s)", rt(cat->cls)->name, cat->name);
        }
    }
}

// --- objects -----------------------------------------------------------------
id alloc_instance(Class cls, size_t extra) {
    size_t sz = rt(cls)->instance_size + extra;
    if (sz < sizeof(void*)) sz = sizeof(void*);
    auto* h = (ObjHeader*)calloc(1, sizeof(ObjHeader) + sz);
    h->rc.store(1);
    h->magic = kObjMagic;
    id obj = (id)(h + 1);
    obj->isa = cls;
    return obj;
}

bool is_static_object(id obj) {
    if (!obj) return true;
    if (g_game_image && g_game_image->contains((uint64_t)obj)) return true;
    if (rt(obj->isa)->meta) return true;  // class object
    return header(obj)->magic != kObjMagic;
}

void dispose_instance(id obj) {
    if (is_static_object(obj)) return;
    header(obj)->magic = 0;
    free(header(obj));
}

id retain(id obj) {
    if (!obj || is_static_object(obj)) return obj;
    header(obj)->rc.fetch_add(1, std::memory_order_relaxed);
    return obj;
}

void release(id obj) {
    if (!obj || is_static_object(obj)) return;
    if (header(obj)->rc.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        send<void>(obj, SEL_("dealloc"));
    }
}

NSUInteger retain_count(id obj) {
    if (!obj || is_static_object(obj)) return UINT32_MAX;
    return (NSUInteger)header(obj)->rc.load();
}

id autorelease(id obj) {
    if (!obj) return obj;
    pool_stack().push_back(obj);
    return obj;
}

void* pool_push() { return (void*)(uintptr_t)(pool_stack().size() + 1); }

void pool_pop(void* token) {
    size_t mark = (uintptr_t)token - 1;
    auto& st = pool_stack();
    while (st.size() > mark) {
        id o = st.back();
        st.pop_back();
        // send release so overridden -release methods run
        send<void>(o, SEL_("release"));
    }
}

}  // namespace objc

// --- C entry points used by msgsend.S ------------------------------------------
extern "C" IMP objc_lookup_for_object(id self, SEL s) {
    Class cls = self->isa;
    if (!cls) {
        LOGE("message %s to object %p with nil isa", s, self);
        return (IMP)objc_nil_method;
    }
    IMP imp = objc::lookup_method(cls, s);
    if (imp) return imp;
    log_unrecognized(cls, s);
    return (IMP)objc_nil_method;
}

extern "C" IMP objc_lookup_super2(objc_super* sup, SEL s) {
    Class start = sup->cls->superclass;
    IMP imp = start ? objc::lookup_method(start, s) : nullptr;
    if (imp) return imp;
    log_unrecognized(sup->cls, s);
    return (IMP)objc_nil_method;
}

// --- libobjc exports -----------------------------------------------------------
static void* g_empty_cache[4] = {};

extern "C" void hle_objc_setProperty_nonatomic(id self, SEL, id newValue, ptrdiff_t offset) {
    id* slot = (id*)((char*)self + offset);
    id old = *slot;
    *slot = objc::retain(newValue);
    objc::release(old);
}

extern "C" void hle_objc_enumerationMutation(id obj) {
    LOGE("collection %p mutated during enumeration", obj);
}

HLE_EXPORT(objc_msgSend, objc_msgSend);
HLE_EXPORT(objc_msgSendSuper2, objc_msgSendSuper2);
HLE_EXPORT(objc_msgSend_stret, objc_msgSend_stret);
HLE_EXPORT(objc_setProperty_nonatomic, hle_objc_setProperty_nonatomic);
HLE_EXPORT(objc_enumerationMutation, hle_objc_enumerationMutation);
HLE_EXPORT(_objc_empty_cache, g_empty_cache);
HLE_EXPORT(_objc_empty_vtable, g_empty_cache);

extern "C" id hle_objc_getProperty(id self, SEL, ptrdiff_t offset, BOOL) {
    return *(id*)((char*)self + offset);
}
extern "C" void hle_objc_setProperty(id self, SEL, ptrdiff_t offset, id value, BOOL, BOOL copy) {
    id replacement = copy ? objc::send(value, SEL_("copy")) : objc::retain(value);
    id* slot = (id*)((char*)self + offset);
    id old = *slot;
    *slot = replacement;
    objc::release(old);
}
HLE_EXPORT(objc_getProperty, hle_objc_getProperty);
HLE_EXPORT(objc_setProperty, hle_objc_setProperty);
